# Configuration

# 64-bit GCC root path
tc=$HOME/toolchains/cust-gcc-9.1.0

# 32-bit GCC target triple prefix
tc32=$HOME/toolchains/cust-gcc32-9.1.0

# 64-bit GCC target triple prefix
prefix=aarch64-elf-

# 32-bit GCC target triple prefix
prefix32=arm-eabi-

# Number of parallel jobs to run
# Do not remove; set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# ----------------------------

# Load the shared helpers early to prevent duplication
source helpers.sh

gcc_bin=$tc/bin
gcc32_bin=$tc32/bin

export PATH=$gcc_bin:$gcc32_bin:$PATH

MAKEFLAGS+=(
	CROSS_COMPILE=$prefix
	CROSS_COMPILE_ARM32=$prefix32

	KBUILD_COMPILER_STRING="$(get_gcc_version ${prefix}gcc)"
)
