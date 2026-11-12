#!/bin/bash
# Build libbladeRF, bladeRF-cli and the libbladeRF test programs (no install)
# into $HOST_BUILD. Needs libusb-1.0-0-dev (see setup/sudo-setup.sh).
#
#   build-host.sh [bladeRF source tree]   (default: this checkout)
#
# The host software is the same for every FPGA image under test, so this only
# needs rebuilding when the host code changes.
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"

src="$(cd "${1:-$HWTEST_DIR/..}" && pwd)"
[ -f "$src/host/CMakeLists.txt" ] || die "not a bladeRF tree: $src"

log "Building host software from $src ($(git -C "$src" describe --always --dirty 2>/dev/null)) into $HOST_BUILD"
cmake -S "$src/host" -B "$HOST_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_DOCUMENTATION=OFF \
    -DINSTALL_UDEV_RULES=OFF \
    -DTEST_LIBBLADERF=ON \
    -DCMAKE_INSTALL_PREFIX="$HWTEST_HOME/prefix"
cmake --build "$HOST_BUILD" -j "$(nproc)"
"$BLADERF_CLI_BIN" --version
ls "$HOST_BUILD/output"
