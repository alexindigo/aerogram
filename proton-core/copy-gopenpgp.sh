#!/bin/sh
# gopenpgp-sys (Proton's OpenPGP, Go) is compiled by its cargo build
# script into a hashed target/debug/build/gopenpgp-sys-*/out/ dir.
# Copy the archive to a stable path so the C++ link can name it.
# All variants in one build are identical (same Go source); take the
# newest.
set -eu
src=$(ls -t "$(dirname "$0")"/target/debug/build/gopenpgp-sys-*/out/libgopenpgp-sys.a | head -n1)
cp "$src" "$(dirname "$0")/target/debug/libgopenpgp-sys.a"
