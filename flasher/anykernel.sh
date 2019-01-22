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
  ui_print '  ! Magisk is not installed; some tweaks will be missing'
fi

mountpoint -q /data && {
  # Install custom PowerHAL config
  mkdir -p /data/adb/magisk_simple/vendor/etc
  rm -f /data/adb/magisk_simple/vendor/etc/powerhint.json

  # Install second-stage late init script
  mkdir -p /data/adb/service.d
  cp $TMPDIR/95-proton.sh /data/adb/service.d
  chmod +x /data/adb/service.d/95-proton.sh

  # Back up DTBO if necessary
  slot="$(getprop ro.boot.slot_suffix)"
  if [ ! -f "/data/adb/dtbo${slot}.orig.img" ]; then
    ui_print "  • Backing up existing DTBO"
    dd if=/dev/block/by-name/dtbo${slot} of=/data/adb/dtbo${slot}.orig.img
  fi

  # Optimize F2FS extension list (@arter97)
  find /sys/fs/f2fs -name extension_list | while read list; do
    HASH=$(md5sum $list | awk '{print $1}')

    if [[ $HASH == "b7b2a4563c80a66c5ec021fc7afcd8c8" ]]; then
      echo "Extensions list up-to-date: $list"
      continue
    fi

    echo "Updating extension list: $list"

    echo "Clearing extension list"

    HOT=$(cat $list | grep -n 'hot file extension' | cut -d : -f 1)
    COLD=$(($(cat $list | wc -l) - $HOT))

    COLDLIST=$(head -n$(($HOT - 1)) $list | grep -v ':')
    HOTLIST=$(tail -n$COLD $list)

    echo $COLDLIST | tr ' ' '\n' | while read cold; do
      if [ ! -z $cold ]; then
        echo "[c]!$cold" > $list
      fi
    done

    echo $HOTLIST | tr ' ' '\n' | while read hot; do
      if [ ! -z $hot ]; then
        echo "[h]!$hot" > $list
      fi
    done

    echo "Writing new extension list"

    cat $TMPDIR/f2fs-cold.list | grep -v '#' | while read cold; do
      if [ ! -z $cold ]; then
        echo "[c]$cold" > $list
      fi
    done

    cat $TMPDIR/f2fs-hot.list | while read hot; do
      if [ ! -z $hot ]; then
        echo "[h]$hot" > $list
      fi
    done
  done
}

# end ramdisk changes

write_boot;

## end install

