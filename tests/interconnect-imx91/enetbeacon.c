/*
 * enetbeacon -- a persistent L2 beacon node for holobench's ethernet-segment lab.
 *
 * The i.MX 91 (Linux) node. Ethertype 0x88B8, assigned by holobench.
 *
 * THE FOUR RULES OF THE LAB, AND WHY EACH ONE EXISTS
 * ==================================================
 *
 * 1. BROADCAST YOUR OWN ETHERTYPE FOREVER.  A node that stops talking when it is
 *    satisfied is not a peer, it is a drive-by.
 *
 * 2. NEVER EXIT.  No SYS_EXIT, no poweroff.  If a node exits when happy, then "left"
 *    and "crashed" become the same observation and the lab cannot tell them apart.
 *
 * 3. IGNORE YOUR OWN ETHERTYPE ON RX *AND* YOUR OWN SOURCE MAC.  A multicast socket
 *    hands your own broadcast straight back to you; count it and you "see a peer" that
 *    is yourself.  holobench closed the ethertype door against themselves and then
 *    found the SAME frame arriving with their own MAC in the source field -- one door
 *    shut, the other left open.  Both are shut here.
 *
 * 4. ⭐ THE BODY IS THE EVIDENCE, NOT THE ETHERTYPE.
 *
 *    holobench, tonight: "Five transports prove the DATA crossed.  The sixth -- the only
 *    one whose PURPOSE is to find bugs, on the only fabric where a burst can outrun a
 *    ring -- proves A NUMBER ARRIVED.  I held ethernet to a weaker standard than I hold
 *    I2C, and I never noticed because ethernet was the one I was proud of."
 *
 *    So every frame carries a checkable body, and RX verifies it:
 *
 *      bytes 14..17   magic 0xB5B6B7C0
 *      bytes 18..19   the SENDER'S OWN ETHERTYPE, echoed inside the payload
 *      bytes 20..23   monotonic per-sender sequence number
 *      bytes 24..63   0x5A, repeated
 *
 *    The embedded ethertype is the sharp one: A FRAME THAT DISAGREES WITH ITSELF IS A
 *    STALE OR CLOBBERED BUFFER -- which is exactly what rt1180's NETC writeback bug
 *    produced (a 16-byte writeback that clobbered the buffer address, so the driver
 *    copied a stale buffer and believed it).  A header/payload mismatch cannot happen
 *    on a wire; it can only happen in a ring.
 *
 *    Bad magic, header-vs-payload ethertype mismatch, or a broken pattern => print
 *    "ENET-LAB3 CORRUPT:" and DO NOT COUNT IT AS SEEING A PEER.
 *
 *    Sequence GAPS are logged, never failed on.  A multicast socket may legitimately
 *    drop.  ⭐ CORRUPTION IS THE ASSERTION; LOSS IS A STATISTIC.
 *
 * 5. ⭐ THE HEARTBEAT RE-ARMS.  IT DOES NOT LATCH.
 *
 *    rt1180 proved this tonight, by accident, in the very test they wrote to check my
 *    prediction: their first version counted heartbeat TOTALS -- 20,587, green,
 *    meaningless.  A TOTAL CANNOT SEE A GAP.  A latched PASS prints once at t+5 and then
 *    shows NOTHING when a peer departs at t+17.
 *
 *    So PASS is re-earned every interval from a sliding window: a peer counts only if it
 *    was seen in the last PEER_TIMEOUT_MS.  When a node leaves, the heartbeat STOPS; when
 *    it returns, the heartbeat RESUMES.  The gap between those two is the departure
 *    window, and it is now a NUMBER a scorer can assert on, not a silence it must infer.
 *
 * Build:  aarch64-linux-gnu-gcc -O2 -static -o enetbeacon enetbeacon.c
 * Usage:  enetbeacon <ifname> <my-ethertype> <peer-ethertype>...
 *   e.g.  enetbeacon eth0 0x88B8 0x88B9 0x88BA 0x88BB
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAGIC            0xB5B6B7C0u
#define PATTERN_BYTE     0x5A
#define FRAME_LEN        64
#define PATTERN_OFF      24
#define PATTERN_LEN      (FRAME_LEN - PATTERN_OFF)   /* 40 */

#define BEACON_MS        100      /* transmit interval                        */
#define HEARTBEAT_MS     100      /* how often PASS is re-evaluated           */
#define PEER_TIMEOUT_MS  2000     /* a peer must be seen this recently        */
#define MAX_PEERS        8

struct peer {
    uint16_t ethertype;
    int64_t  last_seen_ms;        /* 0 = never */
    uint32_t last_seq;
    int      have_seq;
    uint64_t frames;
    uint64_t gaps;
    uint64_t corrupt;
};

static int64_t now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

static uint16_t get16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

int main(int argc, char **argv)
{
    uint8_t  tx[FRAME_LEN], rx[2048], my_mac[6];
    struct   peer peers[MAX_PEERS];
    struct   sockaddr_ll sa;
    struct   ifreq ifr;
    int      fd, ifindex, npeers = 0, i;
    uint16_t my_et;
    uint32_t seq = 0;
    int64_t  next_tx, next_hb;
    uint64_t rx_self = 0, rx_other = 0, beats = 0;
    int      was_passing = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <ifname> <my-ethertype> <peer-ethertype>...\n",
                argv[0]);
        return 2;
    }

    my_et = (uint16_t)strtoul(argv[2], NULL, 0);
    for (i = 3; i < argc && npeers < MAX_PEERS; i++) {
        memset(&peers[npeers], 0, sizeof(peers[0]));
        peers[npeers].ethertype = (uint16_t)strtoul(argv[i], NULL, 0);
        npeers++;
    }

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("ENET-LAB3: socket");
        return 1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ENET-LAB3: SIOCGIFINDEX");
        return 1;
    }
    ifindex = ifr.ifr_ifindex;

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ENET-LAB3: SIOCGIFHWADDR");
        return 1;
    }
    memcpy(my_mac, ifr.ifr_hwaddr.sa_data, 6);

    memset(&sa, 0, sizeof(sa));
    sa.sll_family   = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex  = ifindex;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("ENET-LAB3: bind");
        return 1;
    }

    printf("ENET-LAB3 UP: if=%s mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "ethertype=0x%04X peers=%d\n",
           argv[1], my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4],
           my_mac[5], my_et, npeers);
    fflush(stdout);

    /* The frame is constant except for the sequence number. */
    memset(tx, 0xff, 6);                 /* dst: broadcast                    */
    memcpy(tx + 6, my_mac, 6);           /* src: us                           */
    tx[12] = my_et >> 8; tx[13] = my_et; /* ethertype (header)                */
    put32(tx + 14, MAGIC);
    tx[18] = my_et >> 8; tx[19] = my_et; /* ethertype (payload) -- must agree */
    put32(tx + 20, 0);
    memset(tx + PATTERN_OFF, PATTERN_BYTE, PATTERN_LEN);

    /*
     * ⭐ THE NEGATIVE TEST LIVES IN THE SENDER, BECAUSE AN ASSERTION THAT HAS NEVER
     *    FIRED IS NOT AN ASSERTION.
     *
     * BEACON_CORRUPT deliberately malforms our OWN transmissions so the other nodes'
     * RX verifier can be proven to catch each failure mode.  Without this, "0 corrupt
     * frames" is indistinguishable from "the check does not work".
     *
     *   ethertype -- payload ethertype disagrees with the header.  This is the one that
     *                matters: it is the signature of a stale/clobbered RX buffer, and it
     *                is what rt1180's NETC writeback bug actually produced.
     *   magic     -- payload magic wrong.
     *   pattern   -- the 0x5A body is corrupted mid-frame.
     */
    {
        const char *c = getenv("BEACON_CORRUPT");

        if (c && !strcmp(c, "ethertype")) {
            tx[18] = 0xde; tx[19] = 0xad;         /* frame disagrees with itself */
            printf("ENET-LAB3 EVIL: sending frames whose PAYLOAD ethertype (0xDEAD) "
                   "disagrees with their HEADER ethertype (0x%04X)\n", my_et);
        } else if (c && !strcmp(c, "magic")) {
            put32(tx + 14, 0xdeadbeef);
            printf("ENET-LAB3 EVIL: sending frames with a bad magic\n");
        } else if (c && !strcmp(c, "pattern")) {
            tx[PATTERN_OFF + 7] = 0x00;
            printf("ENET-LAB3 EVIL: sending frames with a broken 0x5A pattern\n");
        }
        fflush(stdout);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sll_family  = AF_PACKET;
    sa.sll_ifindex = ifindex;
    sa.sll_halen   = 6;
    memset(sa.sll_addr, 0xff, 6);

    next_tx = next_hb = now_ms();

    for (;;) {                           /* RULE 2: NEVER EXIT. */
        struct timeval tmo = { 0, 20000 };
        fd_set rfds;
        int64_t t;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        select(fd + 1, &rfds, NULL, NULL, &tmo);

        if (FD_ISSET(fd, &rfds)) {
            ssize_t n = recv(fd, rx, sizeof(rx), 0);

            if (n >= 14) {
                uint16_t et = get16(rx + 12);

                /*
                 * RULE 3: reject ourselves by BOTH doors -- our ethertype and our
                 * source MAC.  A multicast socket hands our own broadcast back.
                 */
                if (et == my_et || memcmp(rx + 6, my_mac, 6) == 0) {
                    rx_self++;
                } else {
                    for (i = 0; i < npeers; i++) {
                        if (peers[i].ethertype != et) {
                            continue;
                        }
                        rx_other++;

                        /* RULE 4: THE BODY IS THE EVIDENCE. */
                        if (n < FRAME_LEN) {
                            peers[i].corrupt++;
                            printf("ENET-LAB3 CORRUPT: et=0x%04X short frame "
                                   "(%zd < %d)\n", et, n, FRAME_LEN);
                            fflush(stdout);
                            break;
                        }
                        if (get32(rx + 14) != MAGIC) {
                            peers[i].corrupt++;
                            printf("ENET-LAB3 CORRUPT: et=0x%04X bad magic "
                                   "0x%08X (want 0x%08X)\n",
                                   et, get32(rx + 14), MAGIC);
                            fflush(stdout);
                            break;
                        }
                        if (get16(rx + 18) != et) {
                            /*
                             * A FRAME THAT DISAGREES WITH ITSELF.  This cannot happen
                             * on a wire -- only in a ring, from a stale or clobbered
                             * buffer.  This is rt1180's NETC writeback bug's signature.
                             */
                            peers[i].corrupt++;
                            printf("ENET-LAB3 CORRUPT: header et=0x%04X but payload "
                                   "says 0x%04X -- FRAME DISAGREES WITH ITSELF "
                                   "(stale/clobbered buffer)\n", et, get16(rx + 18));
                            fflush(stdout);
                            break;
                        }
                        {
                            int bad = 0, k;

                            for (k = PATTERN_OFF; k < FRAME_LEN; k++) {
                                if (rx[k] != PATTERN_BYTE) {
                                    bad = k;
                                    break;
                                }
                            }
                            if (bad) {
                                peers[i].corrupt++;
                                printf("ENET-LAB3 CORRUPT: et=0x%04X pattern broken at "
                                       "byte %d: 0x%02X (want 0x%02X)\n",
                                       et, bad, rx[bad], PATTERN_BYTE);
                                fflush(stdout);
                                break;
                            }
                        }

                        /* Verified.  Only now does it count as seeing a peer. */
                        {
                            uint32_t s = get32(rx + 20);

                            if (peers[i].have_seq && s > peers[i].last_seq + 1) {
                                /* Loss is a statistic, not a failure. */
                                peers[i].gaps += s - peers[i].last_seq - 1;
                                printf("ENET-LAB3 GAP: et=0x%04X seq %u -> %u "
                                       "(%u lost, cumulative %llu)\n",
                                       et, peers[i].last_seq, s,
                                       s - peers[i].last_seq - 1,
                                       (unsigned long long)peers[i].gaps);
                                fflush(stdout);
                            }
                            peers[i].last_seq = s;
                            peers[i].have_seq = 1;
                        }
                        peers[i].frames++;
                        peers[i].last_seen_ms = now_ms();
                        break;
                    }
                }
            }
        }

        t = now_ms();

        if (t >= next_tx) {                     /* RULE 1: BEACON FOREVER. */
            put32(tx + 20, seq++);
            if (sendto(fd, tx, FRAME_LEN, 0, (struct sockaddr *)&sa,
                       sizeof(sa)) < 0 && errno != ENOBUFS) {
                perror("ENET-LAB3: sendto");
            }
            next_tx = t + BEACON_MS;
        }

        if (t >= next_hb) {                     /* RULE 5: THE PASS RE-ARMS. */
            int live = 0;

            for (i = 0; i < npeers; i++) {
                if (peers[i].last_seen_ms &&
                    t - peers[i].last_seen_ms <= PEER_TIMEOUT_MS) {
                    live++;
                }
            }

            if (live == npeers) {
                /*
                 * Re-EARNED, not latched.  When a peer departs this line STOPS; when it
                 * returns, it RESUMES.  The gap between is the departure window -- a
                 * number a scorer can assert on.  A total cannot see a gap.
                 */
                beats++;
                printf("ENET-LAB3 PASS: t=%lld.%03llds peers=%d/%d beat=%llu "
                       "rx_peer=%llu rx_self_ignored=%llu\n",
                       (long long)(t / 1000), (long long)(t % 1000),
                       live, npeers, (unsigned long long)beats,
                       (unsigned long long)rx_other, (unsigned long long)rx_self);
                fflush(stdout);
                was_passing = 1;
            } else if (was_passing) {
                printf("ENET-LAB3 LOST: t=%lld.%03llds peers=%d/%d -- heartbeat stops "
                       "here\n", (long long)(t / 1000), (long long)(t % 1000),
                       live, npeers);
                fflush(stdout);
                was_passing = 0;
            }
            next_hb = t + HEARTBEAT_MS;
        }
    }
    /* not reached: a node that exits when satisfied is a drive-by, not a peer. */
}
