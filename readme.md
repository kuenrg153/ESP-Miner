[![](https://dcbadge.vercel.app/api/server/3E8ca2dkcC)](https://discord.gg/osmu)

![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/bitaxeorg/esp-miner/total)
![GitHub commit activity](https://img.shields.io/github/commit-activity/t/bitaxeorg/esp-miner)
![GitHub contributors](https://img.shields.io/github/contributors/bitaxeorg/esp-miner)
![Alt](https://repobeats.axiom.co/api/embed/70889479b1e002c18a184b05bc5cbf2ed3718579.svg "Repobeats analytics image")

# ESP-Miner
esp-miner is open source ESP32 firmware for the [Bitaxe](https://github.com/bitaxeorg/bitaxe)

If you are looking for premade images to load on your Bitaxe, check out the [latest release](https://github.com/bitaxeorg/ESP-Miner/releases/latest) page. Maybe you want [instructions](https://github.com/bitaxeorg/ESP-Miner/blob/master/flashing.md) for loading factory images.

# Bitaxetool
We also have a command line python tool for flashing Bitaxe and updating the config called Bitaxetool 

**Bitaxetool Requires Python3.4 or later and pip**

Install bitaxetool from pip. pip is included with Python 3.4 but if you need to install it check <https://pip.pypa.io/en/stable/installation/>

```
pip install --upgrade bitaxetool
```
The bitaxetool includes all necessary library for flashing the binaries to the Bitaxe Hardware.

**Notes**
 - The bitaxetool does not work properly with esptool v5.x.x, esptool v4.9.0 or earlier is required.
 - Bitaxetool v0.6.1 - locked to using esptool v4.9.0

```
pip install bitaxetool==0.6.1
```

- Flash a "factory" image to a Bitaxe to reset to factory settings. Make sure to choose an image built for your hardware version (401) in this case:

```
bitaxetool --firmware ./esp-miner-factory-401-v2.4.2.bin
```
- Flash just the NVS config to a bitaxe:

```
bitaxetool --config ./config-401.cvs
```
- Flash both a factory image _and_ a config to your Bitaxe: note the settings in the config file will overwrite the config already baked into the factory image:

```
bitaxetool --config ./config-401.cvs --firmware ./esp-miner-factory-401-v2.4.2.bin
```

## AxeOS API
The esp-miner UI is called AxeOS and provides an API to expose actions and information.

For more details take a look at [`main/http_server/openapi.yaml`](./main/http_server/openapi.yaml).

Available API endpoints:
  
**GET**

* `/api/system/info` Get system information
* `/api/system/asic` Get ASIC settings information
* `/api/system/statistics` Get system statistics (data logging should be activated)
* `/api/system/statistics/dashboard` Get system statistics for dashboard
* `/api/system/wifi/scan` Scan for available Wi-Fi networks

**POST**

* `/api/system/restart` Restart the system
* `/api/system/identify` Identify the device
* `/api/system/OTA` Update system firmware
* `/api/system/OTAWWW` Update AxeOS

**PATCH**

* `/api/system` Update system settings

### API examples in `curl`:

```bash
# Get system information
curl http://YOUR-BITAXE-IP/api/system/info

# Get ASIC settings information
curl http://YOUR-BITAXE-IP/api/system/asic

# Get system statistics
curl http://YOUR-BITAXE-IP/api/system/statistics

# Get dashboard statistics
curl http://YOUR-BITAXE-IP/api/system/statistics/dashboard

# Get available Wi-Fi networks
curl http://YOUR-BITAXE-IP/api/system/wifi/scan


# Restart the system
curl -X POST http://YOUR-BITAXE-IP/api/system/restart

# Let the device say Hi!
curl -X POST http://YOUR-BITAXE-IP/api/system/identify

# Update system firmware
curl -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@esp-miner.bin" \
     http://YOUR-BITAXE-IP/api/system/OTA

# Update AxeOS
curl -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@www.bin" \
     http://YOUR-BITAXE-IP/api/system/OTAWWW


# Update system settings
curl -X PATCH http://YOUR-BITAXE-IP/api/system \
     -H "Content-Type: application/json" \
     -d '{"fanspeed": "desired_speed_value"}'
```

## Administration

The firmware hosts a small web server on port 80 for administrative purposes. Once the Bitaxe device is connected to the local network, the admin web front end may be accessed via a web browser connected to the same network at `http://<IP>`, replacing `IP` with the LAN IP address of the Bitaxe device, or `http://bitaxe`, provided your network supports mDNS configuration.

### Recovery

In the event that the admin web front end is inaccessible, for example because of an unsuccessful firmware update (`www.bin`), a recovery page can be accessed at `http://<IP>/recovery`.

### Unlock Settings

In order to unlock the Input fields for ASIC Frequency and ASIC Core Voltage you need to append `?oc` to the end of the settings tab URL in your browser. Be aware that without additional cooling overclocking can overheat and/or damage your Bitaxe.

## Development using esp-miner/devcontainer

This configuration allows you to edit locally and compile the source code using a docker container so you don't have to install the ESP-IDF toolchain and other supporting software on your computer to compile the firmware.

### Prerequisites

- Docker server

### Local PC Setup

These instructions will assume an installation to your home directory.
```
cd ~
git clone https://github.com/bitaxeorg/ESP-MINER.git
cd ESP-MINER
git checkout <the branch you want>
# The next step builds the docker container that will compile the source code
# This will take several minutes to finish
docker build -t espminer-build .devcontainer
```
### Building

```
cd ~/ESP-MINER
docker run --rm -it -v $PWD:/workspace espminer-build /bin/bash
git config --global --add safe.directory /workspace    # set git permissions or build will fail; only done once
cd /workspace
idf.py build
```	
Once the build is done exit out of the docker session and flash the new firmware.

## Development

### Prerequisites

- Install the ESP-IDF toolchain from https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/
- Install nodejs/npm from https://nodejs.org/en/download
- (Optional) Install the ESP-IDF extension for VSCode from https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension

### Building

At the root of the repository, run:
```
idf.py build && ./merge_bin.sh ./esp-miner-merged.bin
```

Note: the merge_bin.sh script is a custom script that merges the bootloader, partition table, and the application binary into a single file.

Note: if using VSCode, you may have to configure the settings.json file to match your esp hardware version. For example, if your bitaxe has something other than an esp32-s3, you will need to change the version in the `.vscode/settings.json` file.

### Flashing

With the bitaxe connected to your computer via USB, run:

```
bitaxetool --config ./config-xxx.cvs --firmware ./esp-miner-merged.bin
```

where xxx is the config file for your hardware version. You can see the list of available config files in the root of the repository.

A custom board version is also possible with `config-custom.cvs`. A custom board needs to be based on an existing `devicemodel` and `asicmodel`.

**Notes:** 
  - If you are developing within a dev container, you will need to run the bitaxetool command from outside the container. Otherwise, you will get an error about the device not being found.
  - Some Bitaxe versions can't directly connect to a USB-C port. If yours is affected use a USB-A adapter as a workaround. More about it [here](https://github.com/bitaxeorg/bitaxeGamma/issues/37).
  - Only ESP32-S3-WROOM-1 module type N16R8 (16MB Flash, 8MB Octal SPI PSRAM) is supported. This model number should be visible on the ESP32 module. Other module types without PSRAM or with Quad SPI PSRAM will not work with the normal firmware. More about it [here](https://github.com/bitaxeorg/ESP-Miner/issues/826).

### Wi-Fi routers

There are some Wi-Fi routers that will block mining, ASUS Wi-Fi routers & some TP-Link Wi-Fi routers for example.
If you find that your not able to mine / have no hash rate you will need to check the Wi-Fi routers settings and disable the following;

1/ AiProtection

2/ IoT 

If your Wi-Fi router has both of these options you might have to disable them both.

If your still having problems here, check other settings within the Wi-Fi router and the bitaxe device, this includes the URL for
the Stratum Host and Stratum Port.

## KF1950 / BitshokaNini V1.1 Support

This fork adds experimental support for the **KF1950 ASIC** as used on the **BitshokaNini V1.1** hashboard.

### Hardware

- **ASIC**: KF1950 (single chip on BitshokaNini V1.1)
- **UART baud**: 363,636 bps
- **Reference clock**: 24 MHz
- **PLL N constant**: 0x80 (matches Whatsminer CSV capture)
- **Firmware target**: ESP32-S3 (same as other Bitaxe variants)

### What Is Working

- **Chip detection**: Chip is detected at power-on and responds to the ID command with an 11-byte response
- **Address assignment**: Single chip assigned address 0x01 (with correct CRC init 0x3A)
- **Full init sequence**: All init phases transmit correctly and match the WM packet capture ground truth:
  - Phase 5: PLL config (4 domains × 2 iterations = 40 commands, N=0x80)
  - Phase 6: Core enables — 80 44 / 8D D4 first, all 6 masks (0x31, 0x21, 0x11, 0x91, 0xA1, 0xB1) × 1 pass, single core reset at end
  - Phase 7: Post-config registers, `pll_phase2`, B1 second pass, final PLL
- **Chip responsiveness**: Chip replies to STATUS_READ throughout and after init (address 0x01 confirmed live)
- **Job transmission**: 228-byte job packets are built and sent correctly
- **Stratum**: WiFi, pool connection, job subscription, and authorization all work normally
- **Web UI / AxeOS**: Temperature, fan control, and API all function

### What Is Not Working

- **No nonce output**: The chip receives jobs but produces zero nonce responses. Root cause is unknown.
  - The STATUS_READ `b3` byte is always `0x01` throughout init. Working Whatsminer chips show `b3=0x80` during active hashing.
  - The init sequence has been aligned byte-for-byte with `kf1950_wm_init_job.py` (a Python script that produced 19,612 valid nonces on an aditBoard with the same chip), yet the chip does not hash on BitshokaNini.
  - Possible causes under investigation: power supply differences, crystal/clock issues on BitshokaNini vs aditBoard, or a remaining init sequence gap.

### Key Implementation Files

| File | Description |
|------|-------------|
| `components/asic/kf1950.c` | KF1950 driver: init, job send, nonce parse |
| `components/asic/include/kf1950.h` | KF1950 constants, packet sizes, debug flags |
| `components/asic/asic.c` | ASIC dispatch table (added KF1950 entry) |
| `main/device_config.h` | Device model/ASIC config (KF1950 entry) |

### Protocol Notes

- **CRC-8**: poly=0x31, init=0xFF for all commands; init=0x3A for address assignment only
- **Packet format**: `FF FF [payload] [CRC8]`; the trigger `FF FF E3` is sent raw (no CRC)
- **Job packet**: 228 bytes — header (5B), metadata (12B), 6 midstate slots × 32B, tail (15B), CRC
- **Nonce response**: 11 bytes, no preamble — `[00] [chip] [04|00] [06] [nonce 4B] [byte8] [counter] [CRC8]`

### Building for BitshokaNini

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Set `asicModel` to `KF1950` in the device config or via the AxeOS web interface.

---

## Attributions

The display font is Portfolio 6x8 from https://int10h.org/oldschool-pc-fonts/ by VileR.
