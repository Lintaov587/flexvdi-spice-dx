#!/usr/bin/env bash
set -euo pipefail
cd /ucrt64
pacman -S --needed --noconfirm \
  autoconf automake make patch \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-meson \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk3 \
  mingw-w64-ucrt-x86_64-libpsl \
  mingw-w64-ucrt-x86_64-sqlite3 \
  mingw-w64-ucrt-x86_64-json-glib \
  mingw-w64-ucrt-x86_64-gstreamer \
  mingw-w64-ucrt-x86_64-gst-plugins-base \
  mingw-w64-ucrt-x86_64-gst-plugins-good \
  mingw-w64-ucrt-x86_64-gst-plugins-bad \
  mingw-w64-ucrt-x86_64-poppler \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-spice-protocol \
  mingw-w64-ucrt-x86_64-usbredir \
  mingw-w64-ucrt-x86_64-lz4 \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-python-pyparsing \
  mingw-w64-ucrt-x86_64-python-six \
  mingw-w64-ucrt-x86_64-imagemagick \
  mingw-w64-ucrt-x86_64-glib-networking
