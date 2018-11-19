# Toolchain paths

# Path to the root of the Clang toolchain
tc_clang=$HOME/toolchains/clang-8.x

# Path to the root of the 64-bit GCC toolchain
tc_gcc=$HOME/toolchains/gcc-4.x

# Path to the root of the 32-bit GCC toolchain
tc_gcc32=$HOME/toolchains/gcc32-4.x

# Optional: target prefix of the 64-bit GCC toolchain
# Leave blank for autodetection
prefix_gcc=aarch64-linux-android-

# Optional: target prefix of the 32-bit GCC toolchain
# Leave blank for autodetection
prefix_gcc32=arm-linux-androideabi-

# Number of parallel jobs to run
# Do not remove, set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# ----------------------------

# Load the shared helpers early to prevent duplication
source helpers.sh

gcc_bin=$tc_gcc/bin
gcc32_bin=$tc_gcc32/bin
clang_bin=$tc_clang/bin
[ -z $prefix_gcc ] && prefix_gcc=$(get_gcc_prefix $gcc_bin)
[ -z $prefix_gcc32 ] && prefix_gcc32=$(get_gcc_prefix $gcc32_bin)

export LD_LIBRARY_PATH=$tc_clang/lib64:$LD_LIBRARY_PATH
export PATH=$clang_bin:$PATH

export CROSS_COMPILE=$gcc_bin/$prefix_gcc
export CROSS_COMPILE_ARM32=$gcc32_bin/$prefix_gcc32
export CLANG_TRIPLE=aarch64-linux-gnu-
MAKEFLAGS+=(
    CC=clang
    O=out
    CROSS_COMPILE=$gcc_bin/$prefix_gcc
    CROSS_COMPILE_ARM32=$gcc32_bin/$prefix_gcc32
    CLANG_TRIPLE=aarch64-linux-gnu-

    HOSTCC=clang
    KBUILD_COMPILER_STRING="$(get_clang_version clang)"
)
