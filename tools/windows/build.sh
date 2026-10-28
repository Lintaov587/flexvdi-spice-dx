#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$repo_root/.build"
cd "$repo_root/.build"
task_root="$PWD"
task_prefix="$task_root/flexvdi-prefix"
mkdir -p "$task_prefix"
export PATH="$task_prefix/bin:/ucrt64/bin:/usr/bin:/usr/bin/core_perl"
export PKG_CONFIG_PATH="$task_prefix/lib/pkgconfig:$task_prefix/share/pkgconfig"
export CC=gcc CXX=g++
stage="${1:?Specify soup, phodav, spice, protocol or client}"
case "$stage" in
  soup)
    meson setup build-soup libsoup-2.74.3 --prefix="$(cygpath -m "$task_prefix")" --buildtype=release \
      -Dc_std=gnu11 -Dtests=false -Dgnome=false -Dintrospection=disabled -Dvapi=disabled \
      -Dgtk_doc=false -Dgssapi=disabled -Dntlm=disabled -Dsysprof=disabled
    meson compile -C build-soup -j 8
    meson install -C build-soup
    ;;
  phodav)
    meson setup build-phodav phodav-2.5 --prefix="$(cygpath -m "$task_prefix")" --buildtype=release \
      -Dc_std=gnu11 -Dgtk_doc=disabled -Davahi=disabled -Dsystemd=disabled -Dudev=disabled
    meson compile -C build-phodav -j 8
    meson install -C build-phodav
    ;;
  spice)
    meson setup build-spice spice-gtk-0.42 --prefix="$(cygpath -m "$task_prefix")" --buildtype=release \
      -Dc_std=gnu11 -Dwebdav=enabled -Dgtk=enabled -Dintrospection=disabled -Dvapi=disabled \
      -Dgtk_doc=disabled -Dlibcap-ng=disabled -Dpolkit=disabled -Dwayland-protocols=disabled \
      -Dsmartcard=disabled -Dsasl=disabled -Degl=disabled -Dspice-common:manual=false
    meson compile -C build-spice -j 8
    meson install -C build-spice
    ;;
  protocol)
    cd flexdp
    autoreconf -fi
    ./configure --prefix="$task_prefix"
    make install
    ;;
  client)
    cmake --fresh -S "$repo_root" -B build-flexvdi -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$task_prefix")" -DCMAKE_C_FLAGS=-std=gnu11
    cmake --build build-flexvdi --target flexvdi-client-bin -j 8
    ;;
  *) exit 2 ;;
esac
