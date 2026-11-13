#!/bin/sh
set -eu

cd "$(dirname "$0")"

# Apple Clang and ld produce Mach-O, not the ELF kernels required by Limine.
CC=${CC:-x86_64-elf-gcc}
LD=${LD:-x86_64-elf-ld}
export CC LD
exec ./scripts/build-unix.sh "$@"
