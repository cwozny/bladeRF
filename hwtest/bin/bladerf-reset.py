#!/usr/bin/env python3
"""Soft-reset a bladeRF without touching its flash.

  bladerf-reset.py fx3     Reboot the FX3 USB controller (bladerf_device_reset):
                           it reloads its firmware from flash and the FPGA comes
                           back unconfigured, like after a power cycle.
  bladerf-reset.py usb     USB port reset of the bladeRF (USBDEVFS_RESET), i.e.
                           a re-enumeration without power-cycling the board.

The device is opened with BLADERF_FORCE_NO_FPGA_PRESENT=1, so libbladeRF does
not talk to the FPGA or its Nios CPU (which may be hung) before the reset.
Exit status 0 on success.
"""
import ctypes
import fcntl
import glob
import os
import sys
import time

BLADERF2 = (0x2CF0, 0x5250)
USBDEVFS_RESET = ord("U") << 8 | 20


def find_usb(vid, pid):
    """Return /dev/bus/usb/BBB/DDD paths of matching devices."""
    found = []
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(d + "/idVendor") as f:
                v = int(f.read(), 16)
            with open(d + "/idProduct") as f:
                p = int(f.read(), 16)
            if (v, p) != (vid, pid):
                continue
            with open(d + "/busnum") as f:
                bus = int(f.read())
            with open(d + "/devnum") as f:
                dev = int(f.read())
            found.append("/dev/bus/usb/%03d/%03d" % (bus, dev))
        except (OSError, ValueError):
            pass
    return found


def usb_reset():
    nodes = find_usb(*BLADERF2)
    if len(nodes) != 1:
        print("expected exactly one bladeRF 2.0 on USB, found %d" % len(nodes))
        return 1
    with open(nodes[0], "wb") as f:
        fcntl.ioctl(f, USBDEVFS_RESET, 0)
    print("USB reset sent to " + nodes[0])
    return 0


def fx3_reset():
    os.environ["BLADERF_FORCE_NO_FPGA_PRESENT"] = "1"
    libdir = os.environ.get("HOST_BUILD", "") + "/output"
    name = os.path.join(libdir, "libbladeRF.so")
    lib = ctypes.CDLL(name if os.path.exists(name) else "libbladeRF.so")
    lib.bladerf_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p]
    lib.bladerf_device_reset.argtypes = [ctypes.c_void_p]
    lib.bladerf_close.argtypes = [ctypes.c_void_p]
    lib.bladerf_strerror.restype = ctypes.c_char_p
    dev = ctypes.c_void_p()
    rc = lib.bladerf_open(ctypes.byref(dev), None)
    if rc != 0:
        print("bladerf_open failed: %s" % lib.bladerf_strerror(rc).decode())
        return 1
    rc = lib.bladerf_device_reset(dev)
    lib.bladerf_close(dev)
    if rc != 0:
        print("bladerf_device_reset failed: %s" % lib.bladerf_strerror(rc).decode())
        return 1
    print("FX3 reset requested")
    return 0


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("fx3", "usb"):
        print(__doc__)
        return 2
    rc = fx3_reset() if sys.argv[1] == "fx3" else usb_reset()
    time.sleep(1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
