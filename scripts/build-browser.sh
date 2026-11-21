#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build a real NetSurf framebuffer browser and a self-contained guest root tree.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK="$ROOT/build/browser"
SRC="$WORK/netsurf-all-3.11"
CURL="$WORK/curl-8.12.1"
PREFIX="$WORK/prefix"
PAYLOAD="$WORK/payload"
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
mkdir -p "$WORK/tmp" "$PREFIX" "$PAYLOAD"
export TMPDIR="$WORK/tmp"
rm -f "$WORK/payload.stamp"
export LC_ALL=C

fetch() {
    local file=$1 url=$2 digest=$3
    if [[ ! -f "$file" ]]; then
        curl -q -fL --retry 2 "$url" -o "$file.download"
        mv "$file.download" "$file"
    fi
    printf '%s  %s\n' "$digest" "$file" | sha256sum --check --status
}
fetch "$WORK/netsurf-all-3.11.tar.gz" \
    https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz \
    4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b
fetch "$WORK/curl-8.12.1.tar.xz" https://curl.se/download/curl-8.12.1.tar.xz \
    0341f1ed97a26c811abaebd37d62b833956792b7607ea3f15d001613c76de202
[[ -d "$SRC" ]] || tar -xzf "$WORK/netsurf-all-3.11.tar.gz" -C "$WORK"
[[ -d "$CURL" ]] || tar -xJf "$WORK/curl-8.12.1.tar.xz" -C "$WORK"
fetch "$WORK/gperf-3.1.tar.gz" https://ftp.gnu.org/gnu/gperf/gperf-3.1.tar.gz \
    588546b945bba4b70b6a3a616e80b4ab466e3f33024a352fc2198112cdbb3ae2
if [[ ! -x "$PREFIX/bin/gperf" ]]; then
    [[ -d "$WORK/gperf-3.1" ]] || tar -xzf "$WORK/gperf-3.1.tar.gz" -C "$WORK"
    (cd "$WORK/gperf-3.1"
     ./configure --prefix="$PREFIX"
     make -j"$JOBS"
     make install)
fi
export PATH="$PREFIX/bin:$PATH"

# c-ares provides non-threaded DNS without requiring Linux ICMP error queues.
# glibc's resolver requires IP_RECVERR, which the guest does not implement.
pkg-config --exists libcares || {
    echo "Missing build dependency: c-ares headers/library (libcares)" >&2
    exit 1
}
if [[ ! -f "$PREFIX/lib/libcurl.a" || ! -f "$PREFIX/lib/rebane-curl-cares.stamp" ]]; then
    (cd "$CURL"
     ./configure --prefix="$PREFIX" --disable-shared --enable-static \
        --disable-threaded-resolver --enable-ares --with-openssl \
        --without-libpsl --without-libidn2 --without-nghttp2 --without-nghttp3 \
        --without-libssh2 --without-brotli --without-zstd --without-librtmp \
        --disable-ldap --disable-ldaps --disable-ftp --disable-file --disable-dict \
        --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
        --disable-rtsp --disable-gopher --disable-mqtt --disable-smb \
        --disable-websockets --disable-manual --disable-docs \
        --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path
     make -j"$JOBS"
     make install)
    touch "$PREFIX/lib/rebane-curl-cares.stamp"
fi
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cp "$ROOT/tools/browser/rebane.c" "$SRC/libnsfb/src/surface/rebane.c"
cp "$ROOT/tools/browser/surface.mk" "$SRC/libnsfb/src/surface/Makefile"
cp "$ROOT/tools/browser/Makefile.config" "$SRC/netsurf/Makefile.config"
# The vendored libraries and binding generator install only to the source tree.
# Disable detection of unused platform surfaces (and their dynamic dependencies).
make -C "$SRC" -j"$JOBS" TARGET=framebuffer PREFIX="$PREFIX" \
    NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no NSFB_VNC_AVAILABLE=no \
    NSFB_WLD_AVAILABLE=no NETSURF_CONFIG="NETSURF_FRAMEBUFFER_RESOURCES=/usr/share/netsurf"
# The aggregate makefile stamps the first build; explicitly revisit the browser
# so adapter/config changes are picked up on subsequent invocations too.
make -C "$SRC/libnsfb" -j"$JOBS" install \
    PREFIX="$SRC/inst-framebuffer" NSSHARED="$SRC/buildsystem" \
    NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no NSFB_VNC_AVAILABLE=no \
    NSFB_WLD_AVAILABLE=no WARNFLAGS='-Wall -Wextra -Wno-error'
export PKG_CONFIG_PATH="$SRC/inst-framebuffer/lib/pkgconfig:$PKG_CONFIG_PATH"
export PATH="$SRC/inst-framebuffer/bin:$PATH"
# The static curl archive is external to NetSurf's generated dependency graph.
# Force the final link when this build recipe runs after transport changes.
rm -f "$SRC/netsurf/nsfb"
make -C "$SRC/netsurf" -j"$JOBS" TARGET=framebuffer PREFIX="$PREFIX" \
    NETSURF_FRAMEBUFFER_RESOURCES=/usr/share/netsurf

mkdir -p "$PAYLOAD/usr/bin" "$PAYLOAD/usr/share/netsurf" \
    "$PAYLOAD/etc/ssl/certs" "$PAYLOAD/usr/share/fonts/truetype/dejavu" \
    "$PAYLOAD/usr/share/licenses/rebane-browser"
cp "$SRC/netsurf/nsfb" "$PAYLOAD/usr/bin/netsurf"
for resource in adblock.css credits.html default.css internal.css licence.html \
                netsurf.png quirks.css welcome.html; do
    cp -L "$SRC/netsurf/frontends/framebuffer/res/$resource" "$PAYLOAD/usr/share/netsurf/"
done
cp "$SRC/netsurf/frontends/framebuffer/res/en/Messages" "$PAYLOAD/usr/share/netsurf/Messages"
cp "$ROOT/tools/browser/Choices" "$PAYLOAD/usr/share/netsurf/Choices"
cp -f /etc/ssl/certs/ca-certificates.crt "$PAYLOAD/etc/ssl/certs/"
FONTDIR=$(pkg-config --variable=prefix freetype2)/share/fonts
for font in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSans-Oblique.ttf \
            DejaVuSans-BoldOblique.ttf DejaVuSerif.ttf DejaVuSerif-Bold.ttf \
            DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
    path=$(find "$FONTDIR" -type f -name "$font" -print -quit)
    [[ -n "$path" ]] || { echo "Missing font: $font" >&2; exit 1; }
    cp "$path" "$PAYLOAD/usr/share/fonts/truetype/dejavu/$font"
done
# Copy the ELF interpreter and the complete resolved runtime library closure.
# ldd executes only the trusted, just-built binary's dynamic loader, not NetSurf.
{ ldd "$PAYLOAD/usr/bin/netsurf" | awk '/=> \/|^[[:space:]]*\// {for(i=1;i<=NF;i++) if ($i ~ /^\//) print $i}';
  readelf -l "$PAYLOAD/usr/bin/netsurf" | awk '/Requesting program interpreter/ {gsub(/\]/,"",$NF); print $NF}';
} | sort -u > "$WORK/host-libraries.list"
while IFS= read -r lib; do
    mkdir -p "$PAYLOAD$(dirname "$lib")"
    cp -L "$lib" "$PAYLOAD$lib"
done < "$WORK/host-libraries.list"
cp "$ROOT/tools/browser/SOURCES" "$PAYLOAD/usr/share/licenses/rebane-browser/SOURCES"
cp "$SRC/netsurf/COPYING" "$PAYLOAD/usr/share/licenses/rebane-browser/NetSurf-COPYING"
cp "$CURL/COPYING" "$PAYLOAD/usr/share/licenses/rebane-browser/curl-COPYING"
for lib in libcss libdom libhubbub libnsfb libparserutils libwapcaplet libnsbmp \
           libnsgif libnspsl libnsutils libnslog libsvgtiny nsgenbind; do
    cp "$SRC/$lib/COPYING" "$PAYLOAD/usr/share/licenses/rebane-browser/$lib-COPYING"
done
cp "$SRC/libutf8proc/LICENSE.md" "$PAYLOAD/usr/share/licenses/rebane-browser/libutf8proc-LICENSE"
awk 'NR >= 14 && NR <= 40' \
    "$SRC/netsurf/content/handlers/javascript/duktape/duktape.c" \
    > "$PAYLOAD/usr/share/licenses/rebane-browser/Duktape-LICENSE"
for license in /usr/share/licenses/ttf-dejavu/LICENSE /usr/share/doc/fonts-dejavu-core/copyright; do
    if [[ -f "$license" ]]; then cp "$license" "$PAYLOAD/usr/share/licenses/rebane-browser/DejaVu-LICENSE"; break; fi
done
find "$PAYLOAD" -type f -printf '/%P\n' | sort > "$WORK/payload.list"
touch "$WORK/payload.stamp"
printf 'Browser payload: %s\nManifest: %s\n' "$PAYLOAD" "$WORK/payload.list"
