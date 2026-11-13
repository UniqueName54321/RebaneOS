#!/bin/sh
set -eu

cd "$(dirname "$0")"

# Prefer a bare-metal toolchain; native Linux GCC/binutils also build the kernels.
if command -v x86_64-elf-gcc >/dev/null 2>&1 && command -v x86_64-elf-ld >/dev/null 2>&1; then
    CC=${CC:-x86_64-elf-gcc}
    LD=${LD:-x86_64-elf-ld}
else
    CC=${CC:-gcc}
    LD=${LD:-ld}
fi
export CC LD
exec ./scripts/build-unix.sh "$@"
