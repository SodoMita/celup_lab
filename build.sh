#!/usr/bin/env sh
set -eu
# local build uses vendored minimal webp headers if pkg-config missing
if pkg-config --cflags --libs libwebp >/dev/null 2>&1; then
  cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
    $(pkg-config --cflags --libs libwebp) -lm
else
  cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
    -I/tmp/webp_include -L/usr/lib/x86_64-linux-gnu -l:libwebp.so.7 -lm
fi
