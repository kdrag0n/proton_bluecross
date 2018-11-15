# Shared interactive kernel build helpers

# Determine the prefix of a cross-compiling toolchain (@nathanchance)
get_gcc_prefix() {
    local gcc_path="${1}gcc"

    # If the prefix is not already provided
    if [ ! -f "$gcc_path" ]; then
        gcc_path="$(find "$1" \( -type f -o -type l \) -name '*-gcc')"
    fi

    echo "$gcc_path" | head -n1 | sed 's@.*/@@' | sed 's/gcc//'
}

# Get the version of Clang in a user-friendly form
get_clang_version() {
    "$1" --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//'
}

# Get the version of GCC in a user-friendly form
get_gcc_version() {
    "$1" --version|head -n1|cut -d'(' -f2|tr -d ')'|awk '{$5=""; print $0}'|sed -e 's/[[:space:]]*$//'
}

# Define the flags given to make to compile the kernel
MAKEFLAGS=(
    -j$jobs
    ARCH=arm64

    KBUILD_BUILD_USER=kdrag0n
    KBUILD_BUILD_HOST=proton
)

# Make wrapper for kernel compilation
kmake() {
    make "${MAKEFLAGS[@]}" "$@"
}

_RELEASE=0

# Create a flashable zip of the current kernel image
mkzip() {
    [ $_RELEASE -eq 0 ] && vprefix=test
    [ $_RELEASE -eq 1 ] && vprefix=v

    cp out/arch/arm64/boot/Image.lz4-dtb flasher/

    [ $_RELEASE -eq 0 ] && echo "  • Installing test build $(cat out/.version)" >| flasher/version
    [ $_RELEASE -eq 1 ] && echo "  • Installing version v$(cat out/.version)" >| flasher/version
    echo "  • Built on $(date "+%a %b '%y at %H:%M")" >> flasher/version

    fn="${1:-proton_kernel.zip}"
    rm -f "$fn"
    echo "  ZIP     $fn"
    pushd flasher
    zip -qr9 "../$fn" . -x .gitignore
    popd
}

# Create a flashable release zip, ensuring the compiled kernel is up to date
rel() {
    _RELEASE=1

    # Swap out version files
    [ ! -f out/.relversion ] && echo 0 > out/.relversion
    mv out/.version out/.devversion && \
    mv out/.relversion out/.version

    # Compile kernel
    kmake oldconfig # solve a "cached" config
    kmake $@

    # Pack zip
    mkdir -p builds
    mkzip "builds/ProtonKernel-pixel3-v$(cat .version).zip"

    # Revert version
    mv out/.version out/.relversion && \
    mv out/.devversion out/.version

    _RELEASE=0
}

# Reset the version (compile number)
zerover() {
    echo 0 >| out/.version
}

# Make a clean build of the kernel and package it as a flashable zip
cleanbuild() {
    kmake clean && kmake $@ && mkzip
}

# Incrementally build the kernel and package it as a flashable zip
incbuild() {
    kmake $@ && mkzip
}

# Incrementally build the kernel and package it as a flashable beta release zip
dbuild() {
    kmake $@ && dzip
}

# Create a flashable beta release zip
dzip() {
    mkdir -p builds
    mkzip "builds/ProtonKernel-pixel3-test$(cat out/.version).zip"
}

# Flash the latest kernel zip on the connected device via ADB
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

# Incremementally build the kernel, then flash it on the connected device
inc() {
    incbuild $@ && ktest
}

# Show differences between the committed defconfig and current config
dc() {
    diff arch/arm64/configs/b1c1_defconfig out/.config
}

# Update the defconfig in the git tree
cpc() {
    # Don't use savedefconfig for readability and diffability
    cp out/.config arch/arm64/configs/b1c1_defconfig
}

# Reset the current config to the committed defconfig
mc() {
    kmake b1c1_defconfig
}

# Open an interactive config editor
cf() {
    kmake nconfig
}

# Edit the raw text config
ec() {
    ${EDITOR:-vim} out/.config
}

# Get a sorted list of the side of various objects in the kernel
osize() {
    find out -type f -name '*.o' ! -name 'built-in.o' ! -name 'vmlinux.o' -exec du -h --apparent-size {} + | sort -r -h | head -n "${1:-75}"
}
