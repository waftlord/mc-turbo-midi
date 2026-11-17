# MegaCommand DIY Dual Turbo MIDI Bridge

- Two independent USB MIDI input/output pairs, each routed to its own DIN IN/OUT pair.
- Turbo up to 8× per port with compatible instruments. Press **PAGE** for Port 1 or **SHIFT** for Port 2, or control either port over USB with [compatible SysEx software](PROTOCOL.md).
- The OLED shows the negotiated speed and MIDI input/output activity for each port.

## Install on MegaCommand DIY

The bridge replaces the full MCL application. Download the complete v1.0.2 package from the [GitHub release](https://github.com/waftlord/mc-turbo-midi/releases/tag/v1.0.2). It contains both application images in `build/`. Unpack the package and open a terminal in the folder containing `SHA256SUMS`. On macOS run `shasum -a 256 -c SHA256SUMS`. On Linux run `sha256sum -c SHA256SUMS`. Both files should report **OK**. On Windows, run `Get-FileHash -Algorithm SHA256 .\build\*.hex | Format-List Path,Hash` in PowerShell and compare each result with the corresponding line in `SHA256SUMS`. Keep the two images and the manifest together. The flat assets attached to a published release use that release's own manifest.

For an offline install, have AVRDUDE available for the 2560 serial upload and `dfu-programmer` on macOS/Linux for the 16U2. Windows users also need the official USB bundle's DFU driver and tool. Keep an extracted copy of the USB bundle for serial recovery and the stock USB image. Download the full MCL application as well if you plan to restore it. [Official MCL 5.02 application and USB downloads](https://github.com/jmamma/MCL/releases/tag/5.02)

| What is installed now? | What to flash |
| --- | --- |
| Full MCL or a stock USB-to-serial Mega 2560 DIY | Upload the 2560 application, then install the dual-port 16U2 image using hardware DFU. |
| Dual bridge v1.0.0 or v1.0.1 with its matched USB firmware | Upload the 2560 application, then install the v1.0.2 16U2 image using hardware DFU. Both releases shipped the same older USB image. This update also fixes USB control handling under load. |
| Prior audit candidate with verified 16U2 image `85941b0d…` | Upload the new ATmega2560 application. The current pair uses the same verified 16U2 HEX as the earlier audit and OLED candidates. |
| Unknown or mismatched pair | Use the 2560 serial and 16U2 hardware DFU steps below to establish a known pair. |

**Upload the ATmega2560 application**

1. On a bridge, hold **PAGE** while powering on. Release it when **USB UPDATE** appears. On full MCL, hold **PAGE** at power-on and choose **OS UPGRADE**. A stock serial-only board already presents a serial port.
2. In desktop Chrome or Edge, open [MCL Hex Flasher](https://wftlrd.uk/mclhexflasher/). Select **MegaCommand DIY**, choose the board's USB serial port, load `build/megacommand-diy-dual-atmega2560-update.hex`, click **Flash** and wait for success. For a serial upload with read-back verification use AVRDUDE with the board's actual serial device in place of `PORT`.

   ```sh
   avrdude -p atmega2560 -c wiring -P PORT -b 115200 -D -U flash:w:build/megacommand-diy-dual-atmega2560-update.hex:i
   ```

3. With matched USB firmware and PAGE released, the serial port disappears after the successful upload and the board reappears as USB MIDI. For an initial install, continue with the USB controller.

**Install the ATmega16U2 using hardware DFU**

1. Open `install/megacommand_diy_enter_dfu_mode.jpg` in the extracted release package. This is the original marked header photo from Justin Mammarella's USB bundle. If using a source checkout, extract the same-named photo from the official download. With the DIY board connected, locate the **USB controller's ICSP header** shown in the photo. Momentarily connect its marked **RESET** and **GND**, then release. The host should enumerate an ATmega16U2 DFU device. The OLED is driven by the other processor. [Official USB firmware bundle](https://github.com/jmamma/MCL/releases/download/5.02/megacmd_usb_dfu_1.0.4.zip)
2. On macOS or Linux with `dfu-programmer`, run the following from the candidate package directory. Proceed from one command to the next after each succeeds. Flash validates the programmed image by default. On Windows follow the bundle's **MegaCommand DIY** instructions with its `Atmega16U2` target, substituting the v1.0.2 16U2 HEX at the flash step.

   ```sh
   dfu-programmer atmega16u2 get bootloader-version
   dfu-programmer atmega16u2 erase
   dfu-programmer atmega16u2 flash build/megacommand-diy-dual-atmega16u2-dfu.hex
   ```

3. Unplug and reconnect USB with PAGE released.

**Check both routes**

Select the matching input and output for each **MegaCMD Port 1** and **Port 2** pair in your MIDI application. Send `F0 7D 4D 43 4C 01 01 F7` on one output at a time. On a cold boot the reply on the same input should be `F0 00 20 3C 04 00 02 01 F7`. This read-only query leaves negotiation idle. For a DIN receive check, initiate a known saved dump from the instrument on that port and compare the content. For a DIN OUT check, use an isolated loopback or an independent receiver. See [per-port controls and Turbo rates](PROTOCOL.md).

Each port shows its settled DIN rate in large type. **READY** means the bridge can pass standard 1× MIDI on that port. **TURBO** means a faster rate has settled. The small **I** and **O** indicators pulse with traffic.

**PAGE / SHIFT** toggle Turbo on Ports 1 / 2. **SAVE / LOAD** cycle their limits. Editing a limit shows **MAX** on that port for 1.5 seconds. It also stays visible while a request is queued or a rate is being negotiated. During negotiation the panel shows **LINKING**, the LED blinks, and the large number continues to show the last settled rate. When Turbo settles, the LED stays lit. A read-only query leaves the display in its normal state.

**WAIT USB** indicates the USB connection is unavailable. **NO TURBO** indicates the negotiation failed. **DIN ERR**, **RX FULL**, **TX FULL**, and **USB ERR** take priority. Restart an interrupted transfer from its beginning after the port recovers.

**If an upload fails**

- If the serial port is present, close any other uploader or serial monitor, reselect **MegaCommand DIY** and the port, then write the complete 2560 image again at 115200 baud. AVRDUDE's Wiring protocol toggles DTR to enter the serial bootloader. Release PAGE for the normal restart.
- If USB MIDI is still present and the bridge runs, repeat the PAGE-held startup. If the application no longer starts and serial is unavailable, enter **16U2 hardware DFU** as above. Flash the official bundle's `usb_serial.hex` temporarily, reconnect USB, then upload and verify the full 2560 image using the serial port. Finally restore the v1.0.2 16U2 HEX with hardware DFU and reconnect normally. This path assumes an intact 2560 serial bootloader and a working 16U2 DFU bootloader.
- If the 16U2 flash was interrupted, re-enter its hardware DFU mode and repeat the erase and validated flash. If the host lists one MIDI pair after a successful install, restart the MIDI client and reselect its endpoints. On macOS a fresh configuration in Audio MIDI Setup → MIDI Studio can expose both pairs when an old single-port configuration is cached.

**Restore full MCL**

Enter PAGE-held serial update mode and write the full MCL application (for example `mcl_5_0_2.hex`) with the DIY serial profile. Then use 16U2 hardware DFU to write `megacommand-diy-usb-midi-1.0.4.hex` from the official USB bundle. Power-cycle with PAGE released and confirm the MCL application and its USB function. The stock MCL app has its own startup behavior. [MCL 5.02 application and USB bundle](https://github.com/jmamma/MCL/releases/tag/5.02)

## Build from source

With PlatformIO Core and Python 3.9+, download `megacmd_usb_dfu_1.0.4.zip` from [MCL 5.00](https://github.com/jmamma/MCL/releases/tag/5.00), then run `python3 tools/build_package.py --bundle /path/to/megacmd_usb_dfu_1.0.4.zip`.

See the [firmware audit and measurements](AUDIT.md), [v1.0.1 SysEx bench results](BENCH_SYSEX.md) and [regression instructions](tests/README.md).

[License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
