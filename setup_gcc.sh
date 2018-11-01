# Toolchain paths

# Path to the GCC toolchain, including the target prefix.
tc=/usr/bin/aarch64-linux-gnu-

# Number of parallel jobs to run
# This should be set to the number of CPU cores on your system.
# Do not remove, set to 1 for no parallelism.
jobs=6

# Do not edit below this point
# -----------------------------

export CROSS_COMPILE=$tc
export ARCH=arm64
export SUBARCH=arm64
export KBUILD_BUILD_USER=kdrag0n
export KBUILD_BUILD_HOST=proton

export CFLAGS=""
export CXXFLAGS=""
export LDFLAGS=""

cc_ver="$(${tc}gcc --version|head -n1|cut -d'(' -f2|tr -d ')'|awk '{$5=""; print $0}'|sed -e 's/[[:space:]]*$//')"

MAKEFLAGS=("KBUILD_COMPILER_STRING=${cc_ver}")

source helpers.sh
