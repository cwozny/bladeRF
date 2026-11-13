#!/bin/bash
# Build libbladeRF, bladeRF-cli and the libbladeRF test programs (no install)
# into $HOST_BUILD, plus the harness's no-TX guard (lib/no-tx-guard.c). Needs
# libusb-1.0-0-dev (see setup/sudo-setup.sh).
#
#   build-host.sh [bladeRF source tree]   (default: this checkout)
#
# The host software is the same for every FPGA image under test, so this only
# needs rebuilding when the host code changes.
set -euo pipefail
. "$(dirname "$0")/../lib/common.sh"
unset LD_PRELOAD    # the guard is not needed (and maybe not built yet) here

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

log "Building the no-TX guard"
inc="$src/host/libraries/libbladeRF/include"
cc -shared -fPIC -O2 -Wall -Wextra -Werror -I"$inc" \
    -o "$NO_TX_GUARD" "$HWTEST_DIR/lib/no-tx-guard.c" -ldl
cc -O2 -Wall -Wextra -Werror -I"$inc" \
    -o "$NO_TX_SELFTEST" "$HWTEST_DIR/lib/no-tx-selftest.c" \
    -L"$HOST_BUILD/output" -lbladeRF
# The self-test must fail without the guard and pass with it
if "$NO_TX_SELFTEST" > /dev/null 2>&1; then
    die "no-TX self-test passes without the guard"
fi
LD_PRELOAD="$NO_TX_GUARD" "$NO_TX_SELFTEST" || die "no-TX guard does not work"

"$BLADERF_CLI_BIN" --version
ls "$HOST_BUILD/output"
