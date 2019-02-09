#!/system/bin/sh

#
# Wait for /data to be mounted
#

while ! mountpoint -q /data; do
	sleep 1
done

#
# Cleanup
#

# Remove old backup DTBOs
rm -f /data/adb/dtbo_a.orig.img /data/adb/dtbo_b.orig.img

# Check if Proton is no longer installed
if ! grep -q Proton /proc/version; then
	# Remove the custom PowerHAL config
	rm -f /data/adb/magisk_simple/vendor/etc/powerhint.json

	# Remove this init script
	rm -f /data/adb/service.d/95-proton.sh

	# Abort and do not apply tweaks
	exit 0
fi

#
# Wait for Android to finish booting
#

while [ "$(getprop sys.boot_completed)" != 1 ]; do
	sleep 2
done

# Wait for init to finish processing all boot_completed actions
sleep 2

#
# Apply overrides and tweaks
#

echo 85 > /proc/sys/vm/swappiness
echo 0 > /sys/block/sda/queue/iostats
echo 0 > /sys/block/sdf/queue/iostats
echo $(cat /sys/module/cpu_input_boost/parameters/input_boost_duration) > /sys/class/drm/card0/device/idle_timeout_ms
echo 1 > /sys/module/printk/parameters/console_suspend
echo 0 > /dev/stune/foreground/schedtune.prefer_idle
echo 0 > /dev/stune/top-app/schedtune.prefer_idle
