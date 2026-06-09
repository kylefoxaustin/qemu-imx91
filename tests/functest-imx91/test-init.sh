#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /run /tmp /mnt
echo "===================== IMX91 FUNCTIONAL TEST ====================="
uname -a
echo "nproc=$(nproc)  model=$(cat /sys/firmware/devicetree/base/model 2>/dev/null)"

echo "----- SoC: identity (SiP SoC-info SMC -> soc-imx9) -----"
echo "soc_id=$(cat /sys/devices/soc0/soc_id 2>/dev/null)" \
     "revision=$(cat /sys/devices/soc0/revision 2>/dev/null)"

echo "----- NET: interfaces + DHCP -----"
ip -o link 2>/dev/null
for i in eth0 eth1; do
  if ip link show "$i" >/dev/null 2>&1; then
    ip link set "$i" up 2>/dev/null
    echo "[$i] udhcpc:"; udhcpc -i "$i" -n -q -t 4 -T 2 2>&1 | tail -2
    ip -o -4 addr show "$i" 2>/dev/null
  fi
done

echo "----- STORAGE: uSDHC block devices -----"
ls -l /dev/mmcblk* /dev/sd* 2>/dev/null
for d in /dev/mmcblk0 /dev/mmcblk1 /dev/mmcblk2 /dev/sda; do
  [ -b "$d" ] || continue
  if mount -t ext4 "$d" /mnt 2>/dev/null; then
    echo "[$d] MOUNTED; marker: $(cat /mnt/marker.txt 2>/dev/null)"
    if echo "imx91-wrote-ok" > /mnt/imx91-wrote.txt 2>/dev/null && sync; then
      echo "[$d] WRITE+SYNC OK: $(cat /mnt/imx91-wrote.txt)"
    fi
    umount /mnt
  fi
done

echo "----- I2C: controllers + scan -----"
i2cdetect -l 2>/dev/null | sort
for b in 0 1 2 3 4 5 6 7; do
  hits=$(i2cdetect -y -r "$b" 2>/dev/null | grep -oE " [0-9a-f]{2}" | grep -vE "^ --$" | tr -d ' ' | paste -sd, )
  [ -n "$hits" ] && echo "i2c-$b devices: $hits"
done

echo "----- DMESG: kept-subsystem bind/health -----"
dmesg | grep -iE "fec|eqos|dwmac|mmc[0-9]|wm8962|imx-i2c|lpi2c|imx-drm|micfil|sai" \
      | grep -iE "fail|error|timeout|register|added|Got CD|link|probe" | head -25

echo "===================== IMX91 FUNCTIONAL TEST DONE ====================="
sleep 2
