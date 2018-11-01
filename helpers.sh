_RELEASE=0

mkzip() {
    [ $_RELEASE -eq 0 ] && vprefix=test
    [ $_RELEASE -eq 1 ] && vprefix=v

    cp out/arch/arm64/boot/Image.gz flasher/
    # TODO: copy the appropriate dtb and/or dtbo(s)

    echo -n $(date "+%a %b '%y at %H:%M") >| flasher/buildtime
    echo "$vprefix$(cat out/.version|tr -d '\n')" >| flasher/buildver
    cd flasher

    fn="proton_kernel.zip"
    [ "x$1" != "x" ] && fn="$1"
    rm -f "../$fn"
    echo "  ZIP     $fn"
    zip -qr9 "../$fn" . -x .gitignore
    cd ..
}

rel() {
    _RELEASE=1

    # Swap out version files
    [ ! -f out/.relversion ] && echo 0 > out/.relversion
    mv out/.version out/.devversion && \
    mv out/.relversion out/.version

    # Compile kernel
    make oldconfig # solve a "cached" config
    make "${MAKEFLAGS[@]}" -j$jobs $@

    # Pack zip
    mkdir -p builds
    mkzip "builds/ProtonKernel-pixel3-v$(cat .version).zip"

    # Revert version
    mv out/.version out/.relversion && \
    mv out/.devversion out/.version

    _RELEASE=0
}

zerover() {
    echo 0 >| out/.version
}

real_make="$(command which make)"

make() {
    "$real_make" "${MAKEFLAGS[@]}" "$@"
}

cleanbuild() {
    make clean && make -j$jobs $@ && mkzip
}

incbuild() {
    make -j$jobs $@ && mkzip
}

dbuild() {
    make -j$jobs $@ && dzip
}

dzip() {
    mkdir -p builds
    mkzip "builds/ProtonKernel-pixel3-test$(cat out/.version).zip"
}

ktest() {
    adb wait-for-any && \
    adb shell ls '/init.recovery*' > /dev/null 2>&1
    if [ $? -eq 1 ]; then
        adb reboot recovery
    fi

    fn="proton_kernel.zip"
    [ "x$1" != "x" ] && fn="$1"
    adb wait-for-usb-recovery && \
    adb push $fn /tmp/kernel.zip && \
    adb shell "twrp install /tmp/kernel.zip && reboot"
}

inc() {
    incbuild && ktest
}

dc() {
    diff arch/arm64/configs/proton_defconfig out/.config
}

cpc() {
    # Don't use savedefconfig for readability and diffability
    cp out/.config arch/arm64/configs/proton_defconfig
}

mc() {
    make proton_defconfig
}

cf() {
    make nconfig
}

ec() {
    ${EDITOR:-vim} out/.config
}
