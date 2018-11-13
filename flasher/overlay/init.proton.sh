#!/system/bin/sh

# Set default values if necessary
if [ ! -f /data/property/persist.spectrum.profile ]; then
	setprop persist.spectrum.profile 0
fi
