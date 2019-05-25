# Configuration

# Clang root path
tc_clang=$HOME/toolchains/aosp-clang-9.0.3

# 64-bit GCC root path
tc_gcc=$HOME/toolchains/aosp-gcc-4.9

# 32-bit GCC root path
tc_gcc32=$HOME/toolchains/aosp-gcc32-4.9

# 64-bit GCC target triple prefix
prefix_gcc=aarch64-linux-android-

# 32-bit GCC target triple prefix
prefix_gcc32=arm-linux-androideabi-

# Number of parallel jobs to run
# Do not remove; set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# ----------------------------

# Load the shared helpers early to prevent duplication
source helpers.sh

gcc_bin=$tc_gcc/bin
gcc32_bin=$tc_gcc32/bin
clang_bin=$tc_clang/bin

export LD_LIBRARY_PATH=$tc_clang/lib64:$LD_LIBRARY_PATH
export PATH=$clang_bin:$PATH

MAKEFLAGS+=(
	CC=clang
	CROSS_COMPILE=$gcc_bin/$prefix_gcc
	CROSS_COMPILE_ARM32=$gcc32_bin/$prefix_gcc32
	CLANG_TRIPLE=aarch64-linux-gnu-

	KBUILD_COMPILER_STRING="$(get_clang_version clang)"
)
