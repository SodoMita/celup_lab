#!/usr/bin/env sh
set -eu
cc -O3 -DNDEBUG -std=c99 -march=native -Iinclude celup_lab.c celup_lab_xbr.c celup_lab_xbrz.c -o celup_lab \
  /usr/lib/x86_64-linux-gnu/libwebp.so.7 -lm
