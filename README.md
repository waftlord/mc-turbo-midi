# MegaCommand DIY Dual Turbo MIDI Bridge

- Two independent USB MIDI input/output pairs, each routed to its own DIN IN/OUT pair.
- Turbo up to 8× per port with compatible instruments. Press **PAGE** for Port 1 or **SHIFT** for Port 2, or control either port over USB with [compatible SysEx software](PROTOCOL.md).
- The OLED shows the negotiated speed and MIDI input/output activity for each port.

## Install

1. Hold **PAGE** while powering on. Release it when the OLED shows **USB UPDATE**. If the full MCL boot menu opens, choose **OS UPGRADE**.
2. Select the board’s USB serial port in your uploader and flash the [ATmega2560 update HEX](https://github.com/waftlord/mc-turbo-midi/releases/download/v1.0.0/megacommand-diy-dual-atmega2560-update.hex) using **Wiring/STK500v2 at 115200 baud**. With the matching USB firmware installed, the board returns to USB MIDI automatically.

**Need the matching USB firmware?** After step 2, flash the [ATmega16U2 DFU HEX](https://github.com/waftlord/mc-turbo-midi/releases/download/v1.0.0/megacommand-diy-dual-atmega16u2-dfu.hex) once using the [MegaCommand DIY hardware DFU instructions](https://github.com/jmamma/MCL/releases/tag/5.00), then turn the board off and back on.

To verify the HEX files, download [SHA256SUMS](https://github.com/waftlord/mc-turbo-midi/releases/download/v1.0.0/SHA256SUMS) to the same folder and run `shasum -a 256 -c SHA256SUMS`.

**This firmware replaces MCL and turns your MegaCommand DIY into a standalone USB-to-DIN MIDI bridge. To return to MCL, reinstall MCL on the ATmega2560, then restore the original ATmega16U2 USB firmware via DFU.**

## Build from source

With PlatformIO Core and Python 3.9+, download `megacmd_usb_dfu_1.0.4.zip` from [MCL 5.00](https://github.com/jmamma/MCL/releases/tag/5.00), then run `python3 tools/build_package.py --bundle /path/to/megacmd_usb_dfu_1.0.4.zip`.

[License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
