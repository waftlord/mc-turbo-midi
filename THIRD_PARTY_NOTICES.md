# Third-party notices

| Source | Use in this project | License |
| --- | --- | --- |
| [MCL 5.00](https://github.com/jmamma/MCL/tree/45beceb4624c477a3c73bd839f2c090a564678c8) | MegaCommand AVR panel configuration and USB serial recovery behavior. | [MCL license](LICENSES/MCL.txt) |
| [Historical MCL / MidiCtrl TurboMIDI implementation](https://github.com/jmamma/MCL/blob/312e9b44dd988cfa9597f156decd81b7cc3e24c1/avr/cores/megacommand/Midi/TurboMidi.cpp) | DIN negotiation reference for `src/TurboLink.cpp`. | [MidiCtrl license at that revision](LICENSES/MidiCtrl.txt) |
| [Adafruit SSD1305, MCL fork](https://github.com/jmamma/Adafruit_SSD1305/blob/d6151d249f5b7e072a73f1b394977e235767aa4c/Adafruit_SSD1305.cpp) | OLED initialization values and timing in `src/Panel.cpp`. | [Adafruit SSD1305 license and source notice](LICENSES/Adafruit_SSD1305.txt) |
| [Adafruit GFX classic font](https://github.com/adafruit/Adafruit-GFX-Library/blob/master/glcdfont.c) | Matching 5×7 glyph data in `src/Panel.cpp`. | [Adafruit GFX license](LICENSES/Adafruit_GFX.txt) |

Include these notices and license texts with source or binary redistributions as required by the applicable terms.

The USB builder uses the MegaCommand DIY 1.0.4 firmware distributed in the [MCL 5.00 USB DFU bundle](https://github.com/jmamma/MCL/releases/tag/5.00). It applies the patch locally to that upstream image. The package also includes the bundle’s unmodified DIY USB ICSP header photograph, `megacommand_diy_enter_dfu_mode.jpg`, for the hardware DFU instructions. The project's MIT license covers the original patch code. The upstream firmware retains its applicable terms.
