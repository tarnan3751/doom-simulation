#!/bin/sh
# Fetches the third-party sources this project builds against, at the exact
# revisions it was developed with. Nothing here is committed to the repository.
#
#   doomgeneric   the portable Doom source port (GPL-2.0)
#   coremark      EEMBC CoreMark (Apache-2.0)
#   riscv-tests   the official RISC-V ISA tests (BSD-3-Clause)
#   doom1.wad     the shareware Doom 1.9 game data (freely redistributable)
set -eu
cd "$(dirname "$0")/.."
mkdir -p third_party/wad

fetch() {  # fetch <dir> <url> <commit>
    if [ ! -d "third_party/$1/.git" ]; then
        git clone -q "$2" "third_party/$1"
    fi
    git -C "third_party/$1" checkout -q "$3"
}

fetch doomgeneric https://github.com/ozkl/doomgeneric.git              e257a09527062bfb986b6e5e0780839b6cbd5b71
fetch coremark    https://github.com/eembc/coremark.git                1f483d5b8316753a742cbf5590caf5bd0a4e4777
fetch riscv-tests https://github.com/riscv-software-src/riscv-tests.git 3df703cd52d5f0d8224e3c50bf03c6cb70efff98
git -C third_party/riscv-tests submodule update -q --init env

WAD=third_party/wad/doom1.wad
WAD_SHA1=5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
if [ ! -f "$WAD" ]; then
    tmp=$(mktemp -d)
    curl -sL -o "$tmp/wad.tar.gz" \
        "http://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed.orig.tar.gz"
    tar -xzf "$tmp/wad.tar.gz" -C "$tmp"
    mv "$tmp/doom-wad-shareware-1.9.fixed/doom1.wad" "$WAD"
    rm -rf "$tmp"
fi
echo "$WAD_SHA1  $WAD" | shasum -c -
echo "third-party sources ready"
