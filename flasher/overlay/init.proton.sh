#!/system/bin/sh

# Set default values if necessary
grep -q "persist.proton.profile" /data/property/persistent_properties || setprop persist.proton.profile 0
grep -q "persist.spectrum.profile" /data/property/persistent_properties || setprop persist.spectrum.profile 0
