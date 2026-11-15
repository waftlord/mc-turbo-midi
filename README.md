# MegaCommand DIY Dual Turbo MIDI Bridge

- Two independent USB MIDI input/output pairs, each routed to its own DIN IN/OUT pair.
- Turbo up to 8× per port with compatible instruments. Press **PAGE** for Port 1 or **SHIFT** for Port 2, or control either port over USB with [compatible SysEx software](PROTOCOL.md).
- The OLED shows the negotiated speed and MIDI input/output activity for each port.

## Install

Download the HEX files and `SHA256SUMS` from the [GitHub release](https://github.com/waftlord/mcl-turbo-midi/releases/latest). Run `shasum -a 256 -c SHA256SUMS` in the download folder.

1. **ATmega2560:** Hold **PAGE** at power-on to enter USB serial update mode. If switching from full MCL, select **OS UPGRADE** in its boot menu. Release PAGE before uploading the ATmega2560 update HEX with a Wiring/STK500v2 uploader at 115200 baud. With the matching USB firmware installed, the board returns to USB MIDI automatically after a successful upload.
2. **ATmega16U2, first install only:** If your board needs the matching USB firmware, flash the DFU HEX once using the [MegaCommand DIY DFU instructions](https://github.com/jmamma/MCL/releases/tag/5.00), then power-cycle. Boards already running it only need step 1.

Use the standard ATmega2560 HEX unless you want [optional MCDT encoder reports](PROTOCOL.md#optional-encoder-reports).

## Build from source

With PlatformIO Core and Python 3.9+, download `megacmd_usb_dfu_1.0.4.zip` from [MCL 5.00](https://github.com/jmamma/MCL/releases/tag/5.00), then run `python3 tools/build_package.py --bundle /path/to/megacmd_usb_dfu_1.0.4.zip`.

[License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
