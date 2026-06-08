#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
echo "===== IMX91 DISPLAY TEST ====="
uname -a
echo "fb devices:"; ls -l /dev/fb* 2>/dev/null
echo "fb0 virtual_size: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
echo "fb0 bits_per_pixel: $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)"
echo "fb0 name: $(cat /sys/class/graphics/fb0/name 2>/dev/null)"
if [ -c /dev/fb0 ]; then
  # Fill fb0 with random noise -> guaranteed non-black scanout content.
  dd if=/dev/urandom of=/dev/fb0 bs=1M count=4 2>/dev/null
  sync
  echo "FB0-PATTERN-WRITTEN"
else
  echo "NO /dev/fb0"
fi
echo "===== IMX91 DISPLAY TEST DONE (sleeping for screendump) ====="
sleep 40
