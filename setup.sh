# Toolchain paths

# Path to the root of the clang toolchain
# Do not add /bin/... or anything.
tc_clang=$HOME/toolchains/clang-8.x

# Path to the root of the gcc toolchain.
# Must be recent, or you may encounter problems.
# Do not add /bin/... or anything.
tc_gcc=/usr

# Number of parallel jobs to run
# This should be set to the number of CPU cores on your system.
# Do not remove, set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# -----------------------------

export CLANG_PREBUILT_BIN=$tc_clang/bin/
export CROSS_COMPILE=aarch64-linux-gnu-
export CC=clang
export CLANG_TRIPLE=aarch64-linux-gnu-
export GCC_TOOLCHAIN=$tc_gcc
export ARCH=arm64
export LD_LIBRARY_PATH=$tc_clang/lib64:$LD_LIBRARY_PATH
export PATH=$CLANG_PREBUILT_BIN:$GCC_TOOLCHAIN/bin:$PATH
export REAL_COMPILER=clang
export KBUILD_BUILD_USER=kdrag0n
export KBUILD_BUILD_HOST=proton
export TOOL_CHAIN_PATH=$tc_gcc/bin/aarch64-linux-gnu-
export CLANG_TCHAIN=$CLANG_PREBUILT_BIN/clang
export CLANG_VERSION="$(${CLANG_TCHAIN} --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//')"
export REAL_COMPILER=clang

export CFLAGS=""
export CXXFLAGS=""
export LDFLAGS=""

unalias cat > /dev/null 2>&1
unalias zip > /dev/null 2>&1

MAKEFLAGS=(CC=$CLANG_TCHAIN CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=$TOOL_CHAIN_PATH "KBUILD_COMPILER_STRING=${CLANG_VERSION}" HOSTCC=$CLANG_TCHAIN)

# helpers
source helpers.sh
