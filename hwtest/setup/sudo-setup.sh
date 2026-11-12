#!/bin/bash
# One-time root setup of the bladeRF test machine (Ubuntu 24.04 / Mint 22).
#
#   sudo bash hwtest/setup/sudo-setup.sh [user]
#
# Everything else in hwtest/ runs as a normal user. This script only:
#   1. installs distribution packages: libusb/ncurses headers for building
#      libbladeRF and bladeRF-cli; UHD and NumPy/SciPy for the USRP B200mini
#      used as an independent RF source/receiver; and a RISC-V GCC with
#      picolibc as a fallback Nios V toolchain if the Quartus RiscFree
#      component is not installed;
#   2. installs udev rules so members of plugdev can open the bladeRF (normal
#      and FX3-bootloader mode) and the USRP without root;
#   3. downloads the UHD FPGA/firmware images for B2xx devices;
#   4. makes sure the user is in plugdev.
# It never touches the bladeRF itself.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo: sudo bash $0" >&2
    exit 1
fi

user="${1:-${SUDO_USER:-}}"
if [ -z "$user" ] || [ "$user" = "root" ]; then
    echo "Usage: sudo bash $0 [user]   (could not tell which user to set up)" >&2
    exit 1
fi

echo "=== Installing packages"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git usbutils \
    libusb-1.0-0-dev libncurses-dev libtecla-dev libedit-dev \
    uhd-host libuhd-dev python3-uhd \
    python3-numpy python3-scipy \
    gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf picolibc-riscv64-unknown-elf

echo "=== Installing udev rules"
cat > /etc/udev/rules.d/88-nuand-bladerf2.rules <<'EOF'
# Nuand bladeRF 2.0 micro
ATTR{idVendor}=="2cf0", ATTR{idProduct}=="5250", MODE="660", GROUP="plugdev"
EOF
cat > /etc/udev/rules.d/88-nuand-bootloader.rules <<'EOF'
# Cypress FX3 Bootloader, used by bladeRF in recovery mode
ATTR{idVendor}=="04b4", ATTR{idProduct}=="00f3", MODE="660", GROUP="plugdev"
EOF
cat > /etc/udev/rules.d/88-nuand-bladerf1.rules <<'EOF'
# Nuand bladeRF
ATTR{idVendor}=="2cf0", ATTR{idProduct}=="5246", MODE="660", GROUP="plugdev"

# Nuand bladeRF, legacy VID/PID
ATTR{idVendor}=="1d50", ATTR{idProduct}=="6066", MODE="660", GROUP="plugdev"
EOF

# Ettus USRP rules: the uhd-host package normally installs them already
if ! ls /lib/udev/rules.d/*uhd* /usr/lib/udev/rules.d/*uhd* /etc/udev/rules.d/*uhd* \
        > /dev/null 2>&1; then
    for f in /usr/lib/uhd/utils/uhd-usrp.rules /usr/libexec/uhd/utils/uhd-usrp.rules; do
        if [ -f "$f" ]; then
            cp "$f" /etc/udev/rules.d/uhd-usrp.rules
            break
        fi
    done
fi
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb

echo "=== Downloading UHD images for B2xx devices (B200mini)"
if command -v uhd_images_downloader > /dev/null; then
    uhd_images_downloader -t b2xx || echo "WARNING: uhd_images_downloader failed; rerun it later" >&2
fi

echo "=== Group membership"
if id -nG "$user" | tr ' ' '\n' | grep -qx plugdev; then
    echo "$user is already in plugdev"
else
    usermod -aG plugdev "$user"
    echo "Added $user to plugdev: log out and back in (or reboot) for it to apply"
fi

echo
echo "Done. Plug in the bladeRF and the B200mini, then check with:"
echo "    lsusb | grep -Ei '2cf0|04b4:00f3|2500'"
