#!/bin/sh
# Recreate the zxbpp / zxbasm / zxbc applet symlinks next to the one real
# multicall binary (bin/zxbasic-suite). The CI artifact ships symlink-free
# (the artifact zipper would otherwise explode each link into a full copy);
# this restores the runnable multicall layout after extraction.
set -e
dir=$(CDPATH= cd -- "$(dirname -- "$0")/bin" && pwd)
for applet in zxbpp zxbasm zxbc; do
    ln -sf zxbasic-suite "$dir/$applet"
done
