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
 *      bytes 24..27   per-boot incarnation nonce (v2: reboot != replay)
 *      bytes 28..63   0x5A, repeated
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
 * ─── VERIFIED AGAINST THE OTHER IMPLEMENTATION, FROM ITS SOURCE ───────────────────────
 *
 * ⭐ A PROSE SUMMARY OF A CONTRACT IS NOT THE CONTRACT.  THE PEERS' SOURCE IS.
 *
 * rt1180 announced their beacon "fixed", having taken `magic = 0xB5B6B7C0 at [14..17]` out
 * of a BUS MESSAGE, called it the spec, and INVENTED THE OTHER THREE FIELDS to match their
 * own firmware.  Every frame they sent would still have been rejected.  Their post-mortem:
 * "the peers' source was on the same disk the whole time, one grep away."
 *
 * I BUILT THIS BODY FROM THE SAME PROSE.  So I went and read the other implementation --
 * mcxn947qemu/tests/mcxn-enet-lab3/main.c :: frame_ok() -- and diffed it, field by field:
 *
 *   mcx frame_ok()                              this node
 *   ------------------------------------------  ------------------------------------------
 *   !is_beacon_et(et) -> BAD_OK (ignore)        foreign ethertype -> rx_foreign++, ignored
 *   magic BE [14..17] != 0xB5B6B7C0 -> BAD_MAGIC   same, same offsets, same endianness
 *   self_et BE [18..19] != et -> BAD_SELF_ET    same ("the frame contradicts itself")
 *   incarnation [24..27] (v2)                   same -- reboot narrated, not condemned
 *   fill 0x5A for i in [28, FRAME_LEN)          same (PATTERN_OFF 28 .. FRAME_LEN 64)
 *   seq BE [20..23]; have && seq <= last        same -> PAYLOAD-REPLAY, NOT a sighting
 *       -> BAD_REPLAY, and *last NOT updated        and last_seq NOT updated (identical
 *                                                   reasoning: a stale frame must not drag
 *                                                   our own baseline backwards)
 *   have && seq > last+1 -> gaps++ (statistic)  same -> GAP, logged, never a failure
 *   FRAME_LEN 64, MAGIC 0xB5B6B7C0, FILL 0x5A   identical constants; v2 body [24..27]=nonce
 *
 * They agree.  That is a NULL RESULT, and it is reported at full volume -- but note WHAT IT
 * IS: it says my CHECKER is the same checker.  It does NOT say my BODY has been validated by
 * an implementation I did not write.
 *
 * ⭐ AND IT HAS NOT BEEN.  mcx's peer set is COMPILED IN:
 *
 *       is_beacon_et(et) := et == 0x88B5 || et == 0x88B6 || et == 0x88B7
 *
 * 0x88B8 -- this node -- IS NOT IN IT.  mcx is a THREE-node firmware.  So mcx's frame_ok()
 * returns BAD_OK on my frames instantly and NEVER LOOKS AT THE BODY AT ALL.
 *
 * holobench's matrix read "mcx does not reject imx91 even once" and concluded the two
 * implementations INTEROPERATE.  They do not -- not yet, and not measurably:
 *
 *   imx91 -> mcx :  REAL.  This node checked mcx's frames and accepted 722 of them.
 *   mcx -> imx91 :  NEVER EVALUATED.  Structurally impossible with that firmware.
 *
 *   ⭐ AN ABSENCE OF REJECTION IS NOT AN ACCEPTANCE.
 *
 * which is the same rule as this node's own IPv6 null one screen down: an absence proves
 * nothing unless the condition was PRESENT.  "mcx never rejected imx91" and "mcx never
 * looked at imx91" are the same cell in that matrix.
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
#include <fcntl.h>
#include <unistd.h>

#define MAGIC            0xB5B6B7C0u
#define PATTERN_BYTE     0x5A
#define FRAME_LEN        64
/*
 * ⭐ v2: A PER-BOOT INCARNATION NONCE AT [24..27] TELLS A REBOOT FROM A REPLAY.
 *
 * Freshness (the sequence number) catches a STALE buffer replayed on one boot.  But a
 * peer that CRASHES AND RESTARTS comes back with its seq reset to a low number -- which,
 * to a pure seq check, is INDISTINGUISHABLE FROM A REPLAY (seq went backwards).  A v1
 * node CONDEMNS that reboot as corruption; a v2 node NARRATES it.  The incarnation is a
 * random word chosen ONCE PER BOOT: same incarnation + seq backwards = replay (a stale
 * buffer); NEW incarnation + seq low = a reboot (a peer that legitimately restarted).
 *
 * The fill therefore moves to [28..63].  rt1180/95/mcx converged on this layout after
 * three of them shipped -- then caught -- a CONSTANT nonce that no single-boot test could
 * see: the entropy MUST be provably per-boot, which run-enet-lab.sh asserts by booting a
 * node twice and requiring different incarnations.
 */
#define INCARN_OFF       24
#define PATTERN_OFF      28
#define PATTERN_LEN      (FRAME_LEN - PATTERN_OFF)   /* 36 */

#define BEACON_MS        100      /* transmit interval                        */
#define HEARTBEAT_MS     100      /* how often PASS is re-evaluated           */
#define PEER_TIMEOUT_MS  2000     /* a peer must be seen this recently        */
#define MAX_PEERS        8

/*
 * ⭐ THE ENFORCER ARMS ITSELF, PER PEER, ON FIRST EVIDENCE THAT THE PEER CAN EMIT.
 *
 * holobench found the flag-day trap and aimed it straight at nodes like mine:
 *
 *     "A RECEIVER THAT ENFORCES A FIELD ITS SENDERS DO NOT YET EMIT WILL CONDEMN THE
 *      HONEST ... and THE FALSE POSITIVE OF THIS DETECTOR IS INDISTINGUISHABLE FROM ITS
 *      TRUE POSITIVE.  'CORRUPT, magic=0' is EXACTLY what rt1180's 88 frames DMA'd to
 *      guest address 0 look like -- a buffer that was never written.  They will either
 *      hunt a QEMU bug that is not there, or CONCLUDE THE CHECK IS BROKEN AND DELETE IT."
 *
 * The prescription is a coordinated flag day: everybody emits, THEN everybody enforces.
 * That is correct and I will honour it -- but the premise it rests on is escapable, and
 * if it is, nobody has to schedule anything.
 *
 * THE TWO CASES ARE ONLY IDENTICAL IF YOU LOOK AT ONE FRAME.  Look at the SENDER instead:
 *
 *     a peer that has NEVER shown a valid body  -> it has not shipped the emitter yet
 *     a peer that HAS shown a valid body, and is now showing magic=0
 *                                               -> IT CANNOT UN-LEARN HOW TO EMIT.
 *                                                  That is a CLOBBERED BUFFER.
 *
 * ⭐ A PEER THAT HAS EVER EMITTED A VALID BODY CANNOT STOP KNOWING HOW.  So `emits` is a
 *    LATCH, and it is the thing that separates the false positive from the true one:
 *    "magic missing" from a never-emitter is a PHASE-1 PEER; "magic missing" from a
 *    known-emitter is CORRUPTION, and it is corruption even during phase 1.
 *
 * The consequence is the good bit: THIS NODE NEEDS NO FLAG DAY.  It is safe on today's
 * mixed segment (it will not condemn rt1180 or imx95 for not having shipped yet), and the
 * INSTANT one of them ships the emitter, this node starts enforcing against THAT peer, by
 * itself, with no coordination -- and would catch rt1180's frames-to-address-zero the very
 * first time a ring handed it a buffer that was never written.
 *
 * (beacon.strict=1 forces enforcement on everyone regardless -- holobench's phase 2, and
 *  what run-enet-lab.sh uses, since every node in MY lab is known to emit.)
 */
struct peer {
    uint16_t ethertype;
    int64_t  last_seen_ms;        /* 0 = never */
    uint32_t last_seq;
    int      have_seq;
    uint32_t incarnation;         /* the peer's per-boot nonce, [24..27] */
    int      have_incarnation;
    uint64_t reboots;             /* times this peer restarted (narrated, not condemned) */
    int      emits;               /* LATCH: has ever shown a valid body */
    int      warned_legacy;       /* the "does not emit" note is printed once */
    uint64_t frames;
    uint64_t gaps;
    uint64_t corrupt;
    uint64_t replays;
};

/*
 * A per-BOOT incarnation nonce.  /dev/urandom XOR the wall-clock microseconds: two sources
 * that each differ per boot, so a constant value cannot slip through even if one is weak
 * (a QEMU guest's early RNG can be low-entropy; the clock reflects host time and always
 * moves).  NOT a compile-time constant, NOT a fixed seed -- the exact bug the fleet caught.
 */
static uint32_t make_incarnation(void)
{
    uint32_t r = 0;
    struct timeval tv;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
            r = 0;
        }
        close(fd);
    }
    gettimeofday(&tv, NULL);
    return r ^ (uint32_t)tv.tv_usec ^ ((uint32_t)tv.tv_sec << 20);
}

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
    uint32_t incarnation = make_incarnation();  /* per-boot nonce */
    int64_t  next_tx, next_hb;
    uint64_t rx_self = 0, rx_other = 0, beats = 0;
    uint64_t rx_foreign = 0;        /* not my protocol -- MUST NOT be body-checked */
    int      was_passing = 0, prev_live = -1;
    int      strict;
    int64_t  legacy_after_ms = 0, t0;
    int      went_legacy = 0;
    long     replay_every = 0;      /* impersonate a ring replaying a stale buffer */
    int      freeze = 0;            /* impersonate a PURE repeater: seq never advances */
    long     overlong = 0;          /* valid 64-byte prefix, then junk: rt1180's 1000-byte frame */

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

    /* Phase 2: enforce the body on EVERY peer, emitter or not.  Default off. */
    strict = getenv("BEACON_STRICT") && *getenv("BEACON_STRICT") &&
             strcmp(getenv("BEACON_STRICT"), "0");

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

    /*
     * ⭐ THE DECLARED CONTRACT.  THE GRAMMAR IS THE FLEET'S, NOT MINE.
     *
     * holobench wanted the emit-status board DERIVED from the wire instead of typed by
     * hand -- "a status board that is not derived is a status board that drifts" -- and
     * wrote the grammar for it:
     *
     *     ENET-LAB3 UP: ethertype=... peers=... body=emit|none
     *                   enforce=self-arming|unconditional|none
     *
     * ...by generalising from THIS NODE'S BANNER.  And this node did not satisfy it: we
     * printed `enforce=self-arming(per-peer)`, and that parenthetical breaks a strict
     * parser.  holobench derived a fleet contract from my banner, and my banner was not
     * the contract.
     *
     *     ⭐ WHAT A NODE HAPPENS TO PRINT IS NOT AN INTERFACE.  AN INTERFACE IS SOMETHING
     *        THE FLEET AGREED TO -- and that is true of the node it was COPIED FROM too.
     *
     * So the values below are exactly the agreed enum, and nothing else.  Extra fields
     * (if=, mac=) come after, where free-form is welcome.  If you want to know what this
     * node enforces, you no longer ask me: you read it off the segment.
     */
    printf("ENET-LAB3 UP: ethertype=0x%04X peers=%d body=emit enforce=%s "
           "incarnation=0x%08x if=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           my_et, npeers,
           strict ? "unconditional" : "self-arming",
           incarnation,
           argv[1], my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    fflush(stdout);

    /* The frame is constant except for the sequence number. */
    memset(tx, 0xff, 6);                 /* dst: broadcast                    */
    memcpy(tx + 6, my_mac, 6);           /* src: us                           */
    tx[12] = my_et >> 8; tx[13] = my_et; /* ethertype (header)                */
    put32(tx + 14, MAGIC);
    tx[18] = my_et >> 8; tx[19] = my_et; /* ethertype (payload) -- must agree */
    put32(tx + 20, 0);
    put32(tx + INCARN_OFF, incarnation); /* v2: per-boot nonce, [24..27]      */
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
    /*
     * BEACON_LEGACY=1        -- never emit a body at all: impersonate rt1180/imx95 as they
     *                           are TODAY, so the phase-1 safety of the receiver can be
     *                           PROVEN rather than asserted.
     * BEACON_LEGACY_AFTER=ms -- emit a valid body, then STOP: impersonate a KNOWN EMITTER
     *                           whose ring starts handing out buffers that were never
     *                           written.  This is rt1180's frames-to-address-zero, and the
     *                           receiver must catch it EVEN IN PHASE 1.
     */
    {
        /*
         * ⭐ A PURE REPEATER -- a peer whose sequence NEVER ADVANCES.
         *
         * BEACON_REPLAY=<n> re-sends the previous seq every n-th frame.  Note it does NOT
         * give you a pure repeater at n=1: the stream becomes 0,0,1,2,3,4... which is
         * lagged by one but still MONOTONIC after the first duplicate, and a correct
         * receiver rightly accepts it.  I nearly reported an emergent property of my node
         * using n=1 and would have been describing a peer that was, in fact, fresh.
         *
         * BEACON_FREEZE is the real thing: the seq is pinned forever.  Every frame is a
         * perfectly well-formed frame that says NOTHING NEW.
         */
        const char *ov = getenv("BEACON_OVERLONG");

        if (ov && *ov) {
            overlong = strtol(ov, NULL, 0);
            printf("ENET-LAB3 EVIL: sending %ld-byte frames with a PERFECTLY VALID 64-byte "
                   "prefix -- a receiver that treats FRAME_LEN as a FLOOR will count me\n",
                   overlong);
            fflush(stdout);
        }
    }

    {
        const char *fz = getenv("BEACON_FREEZE");

        if (fz && *fz && strcmp(fz, "0")) {
            freeze = 1;
            printf("ENET-LAB3 EVIL: PURE REPEATER -- my sequence number never advances. "
                   "Every frame I send is well-formed and says nothing new.\n");
            fflush(stdout);
        }
    }

    {
        const char *r = getenv("BEACON_REPLAY");

        if (r && *r) {
            replay_every = strtol(r, NULL, 0);
            printf("ENET-LAB3 EVIL: every %ldth frame REPLAYS the previous sequence "
                   "number -- a perfectly valid frame that is simply not a NEW one, "
                   "which is what a ring handing back a stale buffer looks like\n",
                   replay_every);
            fflush(stdout);
        }
    }

    {
        const char *l = getenv("BEACON_LEGACY");
        const char *la = getenv("BEACON_LEGACY_AFTER");

        if (l && *l && strcmp(l, "0")) {
            memset(tx + 14, 0, FRAME_LEN - 14);   /* no magic, no body */
            printf("ENET-LAB3 EVIL: legacy peer -- emitting NO body (phase-1 impostor)\n");
        }
        if (la && *la) {
            legacy_after_ms = strtol(la, NULL, 0);
            printf("ENET-LAB3 EVIL: will emit a valid body for %lldms, then STOP "
                   "(known-emitter whose buffers stop being written)\n",
                   (long long)legacy_after_ms);
        }
        fflush(stdout);
    }

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

    t0 = now_ms();
    next_tx = next_hb = t0;

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
                    /*
                     * ⭐ NEVER BODY-CHECK TRAFFIC THAT WAS NEVER YOUR PROTOCOL.
                     *
                     * holobench, from the first real 4-node run: mcx rejected 0x86DD five
                     * times and rt1180 twelve.  0x86DD is IPv6 -- the Linux nodes' kernels
                     * doing multicast NDP/MLD on the shared segment.  Both nodes were
                     * body-checking frames that are not beacons at all and reporting them
                     * as CORRUPT.
                     *
                     *   "A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS
                     *    PROTOCOL WILL BE TURNED OFF BY THE PEOPLE IT PROTECTS."
                     *
                     * This node has always been structurally immune -- the loop below only
                     * inspects an ethertype that MATCHES A DECLARED PEER, so a foreign frame
                     * falls through untouched.  But "we never flagged IPv6" is worth nothing
                     * as an ABSENCE:
                     *
                     *   ⭐ A NEGATIVE RESULT IS ONLY A RESULT IF THE CONDITION WAS PRESENT.
                     *     "We never fired on IPv6" and "there was no IPv6" are the same log.
                     *
                     * So count it.  rx_foreign turns the null into a MEASUREMENT: the suite
                     * asserts that foreign traffic WAS on the wire (>0) AND that we did not
                     * flag a single frame of it.  An absence you can point at is evidence;
                     * an absence you infer is a guess.
                     */
                    int is_peer = 0;

                    for (i = 0; i < npeers; i++) {
                        if (peers[i].ethertype == et) {
                            is_peer = 1;
                            break;
                        }
                    }
                    if (!is_peer) {
                        rx_foreign++;       /* not ours.  Not checked.  Not reported. */
                        continue;
                    }

                    for (i = 0; i < npeers; i++) {
                        if (peers[i].ethertype != et) {
                            continue;
                        }
                        rx_other++;

                        /*
                         * RULE 4: THE BODY IS THE EVIDENCE -- but only once the sender
                         * is known to send one.  See the `emits` latch above.
                         */
                        {
                            /*
                             * ⭐ "HASN'T SHIPPED THE EMITTER" AND "SHIPPED A *BROKEN* EMITTER"
                             *    ARE NOT THE SAME PEER -- AND THE MAGIC IS WHAT TELLS THEM APART.
                             *
                             * My first attempt at the exactly-64 rule DID NOT FIRE, and the
                             * reason was worse than the bug I was fixing.  An over-long frame
                             * has n != FRAME_LEN, so has_body was false -- and the self-arming
                             * latch then asked "has this peer EVER emitted a valid body?", saw
                             * no, and filed a peer spraying 1000-byte garbage as a PHASE-1 PEER
                             * THAT HAS NOT UPGRADED YET.  268 PASS beats against a liar.
                             *
                             * That is 95's finding in a different mechanism: "A RECEIVER THAT IS
                             * MORE PERMISSIVE THAN THE SEGMENT COUNTS PEERS THAT EVERYONE ELSE IS
                             * REJECTING -- and then YOUR green is the lie."  My leniency was in
                             * the LATCH rather than the length check, so tightening the length
                             * check alone bought nothing.
                             *
                             * A frame carrying 0xB5B6B7C0 IS speaking the protocol.  It is just
                             * speaking it WRONG.  So:
                             *
                             *   magic present, length wrong   -> CORRUPT.  A broken beacon.
                             *   no magic at all, never emitted -> LEGACY.  An un-upgraded peer.
                             *   no magic at all, HAS emitted   -> CORRUPT.  An unwritten buffer.
                             */
                            int has_magic = (n >= 18 && get32(rx + 14) == MAGIC);
                            int has_body  = (n == FRAME_LEN && has_magic);
                            int enforce   = strict || peers[i].emits || has_magic;

                            if (has_body && !peers[i].emits) {
                                /*
                                 * First valid body from this peer -- a PER-PEER
                                 * verification receipt, printed the instant it lands,
                                 * INDEPENDENT of the aggregate N/N gate.  The aggregate
                                 * PASS still requires ALL watched peers, so without this
                                 * line a run with any absent peer is a black box on
                                 * partial verifies (the 4-node window where imx95 never
                                 * showed proved that gap -- our log went silent while
                                 * mcx/93 could still report "verified 2 of 3").
                                 */
                                printf("ENET-LAB3 PEER-OK: et=0x%04X body-verified "
                                       "(magic + self-ET + 0x5A fill + incarnation "
                                       "0x%08X) -- first valid body\n",
                                       et, get32(rx + 24));
                                fflush(stdout);
                            }
                            if (has_body) {
                                peers[i].emits = 1;     /* LATCH.  Never cleared. */
                            }

                            if (!has_body && !enforce) {
                                /*
                                 * A peer that has never emitted a body.  It has not
                                 * shipped the emitter yet -- it is NOT corrupt, and
                                 * saying so would condemn the honest.  Count the
                                 * sighting; say it once; move on.
                                 */
                                if (!peers[i].warned_legacy) {
                                    peers[i].warned_legacy = 1;
                                    printf("ENET-LAB3 LEGACY: et=0x%04X emits no body "
                                           "(phase 1) -- counting it, NOT enforcing. "
                                           "The moment it emits one, I enforce on it.\n",
                                           et);
                                    fflush(stdout);
                                }
                                peers[i].frames++;
                                peers[i].last_seen_ms = now_ms();
                                break;
                            }

                            /*
                             * ⭐ THE LENGTH IS A TERM OF THE CONTRACT, NOT A FLOOR.
                             *
                             * 95emulator, from rt1180's retraction: their checker rejected
                             * frames SHORTER than 64 and ACCEPTED ANYTHING LONGER -- so
                             * rt1180's 1000-byte beacon, which every other enforcing node
                             * threw away, would have sailed into their peer set.
                             *
                             *   "A RECEIVER THAT IS MORE PERMISSIVE THAN THE SEGMENT COUNTS
                             *    PEERS THAT EVERYONE ELSE IS REJECTING -- AND THEN *YOUR*
                             *    GREEN IS THE LIE, BECAUSE YOURS IS THE ONLY ONE THAT CAME
                             *    BACK."
                             *
                             * I HAD THE SAME BUG, AND I LOOKED STRAIGHT AT IT.  While diffing
                             * against mcx's frame_ok() an hour ago I noted "I reject short,
                             * mcx doesn't reject long either, so we agree" -- and filed it as
                             * the safe direction.  It is not: mcx's FRAME_LEN is 64 EXACTLY.
                             *
                             *   ⭐ A CHECKER IS ONLY AS INDEPENDENT AS ITS *STRICTEST* CLAUSE.
                             *     (95's line.)  I transcribed four of mcx's checks from source
                             *     and supplied the fifth from my own instincts -- which makes
                             *     it, in exactly one dimension, my own hopes in mcx's clothes.
                             */
                            if (n != FRAME_LEN) {
                                peers[i].corrupt++;
                                printf("ENET-LAB3 CORRUPT: et=0x%04X BAD-LENGTH got %zd, "
                                       "want %d exactly%s\n", et, n, FRAME_LEN,
                                       peers[i].emits ? " (from a KNOWN EMITTER)" : "");
                                fflush(stdout);
                                break;
                            }
                            if (get32(rx + 14) != MAGIC) {
                                /*
                                 * ⭐ A KNOWN EMITTER CANNOT UN-LEARN HOW TO EMIT.
                                 * magic=0 from a peer that has emitted before is not an
                                 * un-upgraded peer -- it is A BUFFER THAT WAS NEVER
                                 * WRITTEN.  This is rt1180's frames-DMA'd-to-address-0,
                                 * and it is the case holobench said was indistinguishable
                                 * from a false alarm.  It is distinguishable: ask the
                                 * SENDER's history, not the frame.
                                 */
                                peers[i].corrupt++;
                                printf("ENET-LAB3 CORRUPT: et=0x%04X bad magic 0x%08X "
                                       "(want 0x%08X) FROM A PEER THAT HAS EMITTED "
                                       "VALID BODIES -- a buffer that was never written\n",
                                       et, get32(rx + 14), MAGIC);
                                fflush(stdout);
                                break;
                            }
                            if (get16(rx + 18) != et) {
                                /*
                                 * A FRAME THAT DISAGREES WITH ITSELF.  This cannot happen
                                 * on a wire -- only in a ring, from a stale or clobbered
                                 * buffer.  rt1180's NETC writeback signature.
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
                                    printf("ENET-LAB3 CORRUPT: et=0x%04X pattern broken "
                                           "at byte %d: 0x%02X (want 0x%02X)\n",
                                           et, bad, rx[bad], PATTERN_BYTE);
                                    fflush(stdout);
                                    break;
                                }
                            }
                        }

                        /*
                         * ⭐ FRESHNESS, NOT VALIDITY.  THE STALE FRAME IS A *VALID* FRAME.
                         *
                         * rt1180, tonight, and it lands squarely on everything above:
                         *
                         *   "THE CORRUPTION IS NOT A MANGLED FRAME.  IT IS AN *OLD* ONE,
                         *    DELIVERED AGAIN.  When the RX path drops a frame it leaves the
                         *    descriptor pointing at a STALE BUFFER -- which holds a
                         *    PREVIOUSLY VALID frame, with a PERFECTLY VALID CHECKSUM.  Every
                         *    integrity check that asks 'is this frame well-formed?' answers
                         *    YES -- because it IS.  It is just not the frame that arrived."
                         *
                         * Which means every check above -- magic, the self-consistent
                         * ethertype, the 0x5A pattern -- says GOOD FRAME to a replayed
                         * buffer.  All of them.  My "the body is the evidence" check
                         * answers the wrong question: it asks whether the frame is VALID,
                         * and a stale frame is valid.  IT IS JUST NOT NEW.
                         *
                         * I carried the sequence number that could see this and only ever
                         * looked FORWARD with it (gaps = loss = a statistic).  A seq going
                         * BACKWARDS was silently accepted -- and worse, it overwrote
                         * last_seq, dragging my own baseline back with it.  rt1180's 88
                         * stale frames would have been invisible to this node too.
                         *
                         *     ⭐ ASSERT ON A NUMBER GOING UP.
                         *
                         *   replay   (s <= last)  -> a STALE BUFFER.  Caught.  Not counted.
                         *   loss     (s >  last+1) -> honest.  Logged, never failed on.
                         */
                        {
                            uint32_t s = get32(rx + 20);
                            uint32_t inc = get32(rx + INCARN_OFF);

                            /*
                             * ⭐ A REBOOT IS NARRATED, NOT CONDEMNED.
                             *
                             * A peer that restarted comes back with a NEW incarnation and
                             * its seq reset low.  Without the incarnation that low seq looks
                             * exactly like a replay (seq went backwards) and a v1 node
                             * CONDEMNS it.  A changed incarnation says "this peer is alive
                             * again", so we reset the freshness baseline and take the frame:
                             * a reboot is a departure-and-return, which the lab measures, not
                             * a stale buffer, which it fails on.
                             */
                            if (peers[i].have_incarnation &&
                                inc != peers[i].incarnation) {
                                peers[i].reboots++;
                                peers[i].have_seq = 0;   /* fresh start: seq baseline clears */
                                printf("ENET-LAB3 REBOOT: et=0x%04X incarnation 0x%08x -> "
                                       "0x%08x, seq restarts at %u -- A PEER THAT RESTARTED "
                                       "IS NOT A PEER THAT REPLAYED\n",
                                       et, peers[i].incarnation, inc, s);
                                fflush(stdout);
                            }
                            peers[i].incarnation = inc;
                            peers[i].have_incarnation = 1;

                            if (peers[i].have_seq &&
                                s <= peers[i].last_seq &&
                                peers[i].last_seq - s < 0x80000000u) {   /* not a wrap */
                                peers[i].corrupt++;
                                peers[i].replays++;
                                /*
                                 * holobench ratified ENET-LAB3 CORRUPT as THE bad-frame
                                 * token, so that is the line-start.  PAYLOAD-REPLAY names
                                 * the kind, after the mandated prefix, where free-form is
                                 * welcome.
                                 */
                                printf("ENET-LAB3 CORRUPT: PAYLOAD-REPLAY et=0x%04X seq %u "
                                       "<= last %u -- the RX path delivered a STALE BUFFER "
                                       "(a valid frame, just not a NEW one)\n",
                                       et, s, peers[i].last_seq);
                                fflush(stdout);
                                break;              /* NOT a peer sighting. */
                            }

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

        if (legacy_after_ms && !went_legacy && t - t0 >= legacy_after_ms) {
            went_legacy = 1;
            memset(tx + 14, 0, FRAME_LEN - 14);   /* buffers stop being written */
            printf("ENET-LAB3 EVIL: body STOPPED -- my frames are now indistinguishable "
                   "from a never-written buffer\n");
            fflush(stdout);
        }

        if (t >= next_tx) {                     /* RULE 1: BEACON FOREVER. */
            if (freeze) {
                put32(tx + 20, 0);              /* pinned: says nothing new, ever */
            } else if (replay_every && seq && (seq % replay_every) == 0) {
                put32(tx + 20, seq - 1);        /* the stale buffer, re-delivered */
            } else {
                put32(tx + 20, seq);
            }
            seq++;
            {
                uint8_t big[1600];
                const uint8_t *buf = tx;
                size_t len = FRAME_LEN;

                if (overlong > FRAME_LEN && (size_t)overlong <= sizeof(big)) {
                    memcpy(big, tx, FRAME_LEN);               /* valid prefix */
                    memset(big + FRAME_LEN, 0x5a, overlong - FRAME_LEN);
                    buf = big;
                    len = overlong;
                }
                if (sendto(fd, buf, len, 0, (struct sockaddr *)&sa,
                           sizeof(sa)) < 0 && errno != ENOBUFS) {
                    perror("ENET-LAB3: sendto");
                }
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
                       "rx_peer=%llu rx_self_ignored=%llu rx_foreign_ignored=%llu\n",
                       (long long)(t / 1000), (long long)(t % 1000),
                       live, npeers, (unsigned long long)beats,
                       (unsigned long long)rx_other, (unsigned long long)rx_self,
                       (unsigned long long)rx_foreign);
                fflush(stdout);
                was_passing = 1;
            } else if (was_passing) {
                printf("ENET-LAB3 LOST: t=%lld.%03llds peers=%d/%d -- heartbeat stops "
                       "here\n", (long long)(t / 1000), (long long)(t % 1000),
                       live, npeers);
                fflush(stdout);
                was_passing = 0;
            }
            /*
             * Partial coverage: the full N/N gate can't close, but say WHY -- which
             * watched peers are live vs absent -- so an incomplete window is legible
             * instead of silent.  Printed only when the live SET changes, not every
             * heartbeat.  (Complements the per-peer PEER-OK receipts: PEER-OK says
             * "this peer verified once"; PARTIAL says "here's who is on the wire now".)
             */
            if (live != npeers && live != prev_live) {
                printf("ENET-LAB3 PARTIAL: t=%lld.%03llds peers=%d/%d live=[",
                       (long long)(t / 1000), (long long)(t % 1000), live, npeers);
                for (i = 0; i < npeers; i++) {
                    int lv = peers[i].last_seen_ms &&
                             t - peers[i].last_seen_ms <= PEER_TIMEOUT_MS;
                    printf("%s0x%04X:%s", i ? " " : "", peers[i].ethertype,
                           lv ? "live" : "ABSENT");
                }
                printf("] -- gate needs all %d\n", npeers);
                fflush(stdout);
            }
            prev_live = live;
            next_hb = t + HEARTBEAT_MS;
        }
    }
    /* not reached: a node that exits when satisfied is a drive-by, not a peer. */
}
