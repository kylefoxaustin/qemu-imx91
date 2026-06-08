#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /mnt
echo "===== IMX91 A-VALIDATION ====="

echo "----- CAN (loopback TX/RX on can0) -----"
modprobe can 2>/dev/null; modprobe can-dev 2>/dev/null; modprobe can-raw 2>/dev/null; modprobe flexcan 2>/dev/null
sleep 1
ip link set can0 type can bitrate 500000 loopback on 2>&1
ip link set can0 up 2>&1 && echo "can0 UP (loopback)"
candump -T 3000 can0 > /tmp/can.out 2>&1 &
sleep 1
cansend can0 1AB#C0FFEE
sleep 2
echo "candump can0:"; cat /tmp/can.out

echo "----- USB host (kbd + storage) -----"
sleep 1
lsusb 2>/dev/null
echo "usb input/storage in dmesg:"; dmesg | grep -iE "usb .*: new|usb-storage|sd [0-9].*sda|input:.*USB" | head
for d in /dev/sda /dev/sdb; do
  [ -b "$d" ] && mount -t ext4 "$d" /mnt 2>/dev/null && { echo "$d mounted; marker: $(cat /mnt/usbmarker.txt 2>/dev/null)"; umount /mnt; }
done

echo "----- runtime I2C attach (tmp105 @ i2c-0 0x49) -----"
i2cdetect -y -r 0 2>/dev/null | grep -E "40:|^40" 
echo "i2cget 0 0x49 (tmp105 temp reg):"; i2cget -y 0 0x49 0x00 w 2>&1

echo "===== IMX91 A-VALIDATION DONE ====="
sleep 3
