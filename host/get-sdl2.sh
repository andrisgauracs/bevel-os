#!/bin/sh
# Unpacks the SDL2 development headers into build-host/deps without root rights, for systems that
# have the SDL2 runtime library (any desktop) but not libsdl2-dev. Debian/Ubuntu only.
set -e
cd "$(dirname "$0")/.."
mkdir -p build-host/deps
cd build-host/deps
apt-get download libsdl2-dev
dpkg-deb -x libsdl2-dev_*.deb root
echo "SDL2 headers in build-host/deps/root/usr/include/SDL2"
