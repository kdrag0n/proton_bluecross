# AnyKernel2 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
do.devicecheck=1
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=blueline
device.name2=crosshatch
'; } # end properties

# shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=1;
ramdisk_compression=auto;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. $TMPDIR/tools/ak2-core.sh;


## AnyKernel install
ui_print "  • Unpacking image"
dump_boot;

# begin ramdisk changes

rm -fr $ramdisk/overlay

if [ -d $ramdisk/.backup ]; then
  ui_print "  • Patching ramdisk"
  patch_cmdline "skip_override" "skip_override"

  mv $TMPDIR/overlay $ramdisk
  cp /system_root/init.rc $ramdisk/overlay
  insert_line $ramdisk/overlay/init.rc "init.proton.rc" after "import /init.usb.configfs.rc" "import /init.proton.rc"
else
  patch_cmdline "skip_override" ""
fi

mountpoint -q /data && {
  mkdir -p /data/adb/magisk_simple/vendor/etc
  cp $TMPDIR/powerhint.json /data/adb/magisk_simple/vendor/etc
}

# end ramdisk changes

write_boot;

## end install

