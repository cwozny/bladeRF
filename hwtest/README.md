# bladeRF 2.0 micro hardware regression harness

Scripts for comparing FPGA images (e.g. Nios II vs Nios V builds) on a real
bladeRF 2.0 micro xA5, unattended, without any risk of bricking it.

## Safety rules

* The harness **never writes the bladeRF's SPI flash**. FPGA images are only
  loaded into the FPGA (volatile; the xA5 image does not even fit in the
  flash), so a power cycle always returns the board to a clean state.
* All bladeRF-cli use goes through `bin/bladerf-cli-safe`, which refuses
  flash-writing commands and options (`flash_image`, `flash_restore`,
  `flash_init_cal`, `erase`, `jump_to_boot`, `load fx3`, `-f`, `-L`, ...)
  and interactive mode.
* RF: at least 30 dB of attenuation between every transmitter and receiver,
  no antennas, bias-tees off.

## One-time setup (XPS 13)

    sudo bash hwtest/setup/sudo-setup.sh     # packages + udev rules
    hwtest/bin/build-host.sh                 # libbladeRF, bladeRF-cli, tests

Data (host build, backups, results) goes to `~/workarea/hwtest-data/`.

## Cabling (fixed for the whole campaign)

    bladeRF TX1  -> 30 dB -> bladeRF RX1        self-loop
    bladeRF TX2  -> 30 dB -> B200mini RX2       bladeRF TX measured by the USRP
    B200mini TX/RX -> 30 dB -> bladeRF RX2      USRP tone measured by the bladeRF

## Scripts

| Script | What it does |
| :----- | :----------- |
| `bin/inventory.sh` | First contact: versions, device info, full read-only flash backup |
| `bin/recover.sh` | Recovery ladder: reload the official image, then progressively stronger soft resets; asks for a replug only as a last resort |
| `bin/bladerf-reset.py` | FX3 reboot (`bladerf_device_reset`) or USB port reset, without talking to the FPGA |
| `bin/rf-check.py` | CW tone checks against the B200mini across bands and sample rates |
| `bin/compare.py` | Compares result directories and flags regressions |
