#!/usr/bin/env sh
set -eu
# Build celup_lab. libwebp is pulled from pkg-config if available, otherwise
# from the /tmp/webpshim shim (headers + bundled libwebp .so) used in the
# arena sandbox.  Compile the xBR/xBRZ sources together with the main lab.
if pkg-config --exists libwebp 2>/dev/null; then
  cc -O3 -DNDEBUG -std=c99 -march=native \
     celup_lab.c celup_lab_xbr.c celup_lab_xbrz.c -o celup_lab \
     $(pkg-config --cflags --libs libwebp) -lm
else
  cc -O3 -DNDEBUG -std=c99 \
     celup_lab.c celup_lab_xbr.c celup_lab_xbrz.c -o celup_lab \
     -I/tmp/webpshim -L/tmp/webpshim -lwebp -Wl,-rpath,/tmp/webpshim -lm
fi
