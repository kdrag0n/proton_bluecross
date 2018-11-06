# Toolchain paths

# Path to the root of the 64-bit GCC toolchain
tc=/usr

# Path to the root of the 32-bit GCC toolchain
tc32=$HOME/toolchains/gcc32-8.x

# Number of parallel jobs to run
# Do not remove, set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# ----------------------------

# Load the shared helpers early to prevent duplication
source helpers.sh

gcc_bin=$tc_gcc/bin
gcc32_bin=$tc_gcc32/bin
[ -z $prefix_gcc ] && prefix_gcc=$(get_gcc_prefix $gcc_bin)
[ -z $prefix_gcc32 ] && prefix_gcc32=$(get_gcc_prefix $gcc32_bin)

export PATH=$gcc_bin:$gcc32_bin:$PATH

MAKEFLAGS+=(
    CROSS_COMPILE=$prefix_gcc
    CROSS_COMPILE_ARM32=$prefix_gcc32

    KBUILD_COMPILER_STRING="$(get_gcc_version ${prefix_gcc}gcc)"
)
