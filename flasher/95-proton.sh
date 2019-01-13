#!/system/bin/sh

#
# Second-stage Proton Kernel init script by @kdrag0n
#
# Self-destructs when Proton is uninstalled, no need to manually delete
# DTBO and PowerHAL config are restored as well
#

#
# Self-destruct and fully uninstall Proton Kernel if necessary
#

# Remove the opposite slot's DTBO if it exists
slot="$(getprop ro.boot.slot_suffix)"
if [ "$slot" = "_a" ]; then
	rm -f /data/adb/dtbo_b.orig.img
else
	rm -f /data/adb/dtbo_a.orig.img
fi

# Check if Proton is no longer installed
if ! grep -q Proton /proc/version; then
	# Remove the custom PowerHAL config
	rm -f /data/adb/magisk_simple/vendor/etc/powerhint.json

	# Restore the current slot's DTBO backup if available
	if [ -f "/data/adb/dtbo${slot}.orig.img" ]; then
		dtbb="/dev/block/by-name/dtbo$slot"
		dd if=/dev/zero of=$dtbb
    	dd if=/data/adb/dtbo${slot}.orig.img of=$dtbb
	fi

	# Remove backup DTBOs
	rm -f /data/adb/dtbo_a.orig.img /data/adb/dtbo_b.orig.img

	# Remove this init script
	rm -f /data/adb/service.d/95-proton.sh

	# Abort and do not apply tweaks
	exit 0
fi

#
# Wait for Android to finish boot
#

# Wait for Android to complete boot (to override boot_completed=1 settings)
while [ "$(getprop sys.boot_completed)" != 1 ]; do
	sleep 2
done

# Wait for init to finish processing all boot_completed actions
sleep 2

#
# Apply overrides and tweaks:
#

# Reduce swappiness: 100 -> 85 (since December update)
# This reduces kswapd0 CPU usage, leading to better performance and battery due to less CPU time used
echo 85 > /proc/sys/vm/swappiness

# Disable I/O statistics accounting on important block devices (others disabled in kernel)
# According to Jens Axboe, this adds 0.5-1% of system CPU time to block IO operations - not desirable
# This could break apps that rely on stats via storaged (which reads them); however, I have not seen any cases of this
echo 0 > /sys/block/sda/queue/iostats
echo 0 > /sys/block/sdf/queue/iostats

# Make the PowerHAL INTERACTION boost duration match the in-kernel CPU Input Boost driver
# libperfmgr polls idle_state for its INTERACTION hint, which is controlled by idle_timeout_ms
# This does not have any effect on the actual display stack as one might assume
echo $(cat /sys/module/cpu_input_boost/parameters/input_boost_duration) > /sys/class/drm/card0/device/idle_timeout_ms

# Tune the CPU affinities assigned to certain processes in the system, including apps
# Thanks to xFirefly93 @ XDA for the original tuned values in his BlackenedMod script
echo "0-3" > /dev/cpuset/background/cpus
echo "0-3" > /dev/cpuset/foreground/cpus
echo "4-5" > /dev/cpuset/kernel/cpus

# Enable suspending of printk while the system is suspended for a negligible increase in power consumption when idle
# This is disabled by init.sdm845.power.rc for better debugging of panics around suspend/resume events
echo 1 > /sys/module/printk/parameters/console_suspend

# Enable deep in-memory sleep when suspending for less idle battery drain when the system decides to enter suspend
# This is disabled by init.sdm845.power.rc to reduce suspend/resume latency but I haven't noticed a significant difference
echo deep > /sys/power/mem_sleep
