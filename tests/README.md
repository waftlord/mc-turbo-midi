# SysEx replay tests

The paired AVR replay disassembles both firmware HEX files and runs their instructions with modeled USARTs, USB endpoints and Turbo peers. DIN OUT is read from the modeled transmit shift register. USB IN banks are recorded individually before decoding. See the [bench report](../BENCH_SYSEX.md) for physical DIN and host-driver checks.

Use Python 3 and AVR binutils. Put the matched HEX files in `build/`, or pass their directory with `--images`. The scripts find `avr-objdump` on `PATH` or in PlatformIO's default AVR toolchain cache. Use `--objdump` to select another copy.

Run these commands from the bridge directory.

```sh
for case in cold return 2x 4x 8x stress interruption realtime; do
  python3 -B tests/paired_avr_replay.py --case "$case" || exit
done
python3 -B tests/din_rx_fault_replay.py
```

The default fixture generates four synthetic records with Machinedrum framing and sizes of 1,176, 1,176, 873 and 813 bytes. Both commands also accept a four-record `.syx` capture with those sizes as an optional positional argument.

The `realtime` case checks USB IN bank boundaries and byte order after a bounded host read pause. The old images deliver the complete fixture bytes at the USB boundary but place a realtime event after a same-cable SysEx continuation in one bank, which triggers legacy CoreMIDI truncation. The DIN fault test injects UART faults with valid data queued on either side. The old main image discards those bytes.
