# MCL Turbo MIDI

MCL Turbo MIDI is firmware for using MegaCommand DIY hardware as a USB-to-DIN MIDI bridge with TurboMIDI negotiation on MIDI port 1. It replaces the main application on the ATmega2560.

## Hardware

| Component | Role |
| --- | --- |
| ATmega2560 | Runs the bridge application built from this repository. |
| ATmega16U2 | Handles USB MIDI and USB serial. Its firmware is obtained separately. |

The USB firmware reference is `megacommand-diy-usb-midi-1.0.4.hex` in `megacmd_usb_dfu_1.0.4.zip` from [MCL 5.00](https://github.com/jmamma/MCL/releases/tag/5.00). The stock image provides USB MIDI and the UART rate-switch command used by the bridge. It is the baseline for hardware validation. End-to-end USB MIDI and sustained transfers at standard and TurboMIDI rates are pending validation on that baseline.

## Build and upload

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), then run:

```sh
pio run -e megacommand_diy
```

The build writes the ATmega2560 image to `.pio/build/megacommand_diy/firmware.hex`. With the device in serial bootloader mode, upload over USB serial:

```sh
pio run -e megacommand_diy -t upload
```

If PlatformIO selects the wrong port, specify the device's serial port with `--upload-port`. With MCL installed, select **OS UPGRADE** from its boot menu. With the bridge installed, hold **PAGE** at power-on to request USB serial mode. Follow the upstream DFU instructions for ATmega16U2 updates. Use the PlatformIO output only with the ATmega2560 serial bootloader.

## Operation

Connect the host over USB and use **MIDI IN 1** and **MIDI OUT 1** for the DIN connection. The bridge starts at standard MIDI speed. Press **PAGE** to request TurboMIDI negotiation and press it again to return to standard speed. The OLED displays the DIN rate and MIDI activity.

## License and sources

Original code is available under the [MIT License](LICENSE). The firmware targets the [MegaCommand DIY hardware design](https://github.com/jmamma/MegaCommand_Design). See [Third-party notices](THIRD_PARTY_NOTICES.md) for source references and license terms.
