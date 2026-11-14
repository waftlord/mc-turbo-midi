# MCL Turbo MIDI

MCL Turbo MIDI turns MegaCommand DIY hardware into a USB-to-DIN MIDI bridge with TurboMIDI negotiation on MIDI port 1.

## Hardware

| Chip | Image | Role |
| --- | --- | --- |
| ATmega2560 | `megacommand-diy-atmega2560.hex` | Main application for MIDI routing, rate negotiation, and panel controls. |
| ATmega16U2 | `megacommand-diy-atmega16u2.hex` | USB MIDI and USB serial interface with the bridge's USB flow-control patch. |

## Build both images

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) and Python 3.9 or later. Download `megacmd_usb_dfu_1.0.4.zip` from [MCL 5.00](https://github.com/jmamma/MCL/releases/tag/5.00) into the repository root. Keep the archive intact. The patch builder reads and verifies the MegaCommand DIY image inside it.

From the repository root on macOS or Linux, run:

```sh
pio run -e megacommand_diy
mkdir -p build
cp .pio/build/megacommand_diy/firmware.hex build/megacommand-diy-atmega2560.hex
pio pkg exec -p platformio/toolchain-atmelavr@1.70300.191015 -- \
  python3 tools/build_usb_firmware.py --bundle megacmd_usb_dfu_1.0.4.zip
shasum -a 256 -c SHA256SUMS
```

Both images are written to `build/`. `SHA256SUMS` lists the expected checksum for each image. The USB builder verifies its output before writing it. `platformio.ini` pins the AVR platform and toolchain.

## Install

1. Use the [MegaCommand DIY DFU instructions in the upstream bundle](https://github.com/jmamma/MCL/releases/tag/5.00) with `build/megacommand-diy-atmega16u2.hex` as the file to flash. This image goes on the ATmega16U2 in hardware DFU mode.
2. Put the device into serial bootloader mode for the ATmega2560. With MCL installed, select **OS UPGRADE** from its boot menu. With the bridge installed, hold **PAGE** at power-on to request USB serial mode.
3. Upload the ATmega2560 application via USB serial with PlatformIO.

```sh
pio run -e megacommand_diy -t upload
```

If PlatformIO selects the wrong port, specify the device's serial port with `--upload-port`.

## Operation

Connect the host over USB and use **MIDI IN 1** and **MIDI OUT 1** for the DIN connection. The bridge starts at standard MIDI speed. Press **PAGE** to request TurboMIDI negotiation and press it again to return to standard speed. The OLED displays the DIN rate and MIDI activity.

## License and sources

Original code is available under the [MIT License](LICENSE). The firmware targets the [MegaCommand DIY hardware design](https://github.com/jmamma/MegaCommand_Design). See [Third-party notices](THIRD_PARTY_NOTICES.md) for source references and license terms.
