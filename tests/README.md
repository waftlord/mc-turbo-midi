# SysEx replay tests

The paired AVR replay disassembles both firmware HEX files and runs their instructions with modeled USARTs, USB endpoints and Turbo peers. DIN OUT is read from the modeled transmit shift register. USB IN banks are recorded individually before decoding. See the [bench report](../BENCH_SYSEX.md) for physical DIN and host-driver checks.

Use Python 3.10+ and AVR binutils. Put the matched HEX files in `build/`, or pass their directory with `--images`. The scripts find `avr-objdump` on `PATH` or in PlatformIO's default AVR toolchain cache. Use `--objdump` to select another copy.

Run these commands from the bridge directory. The first command builds the native transport regression with a host C++ compiler.

```sh
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -Isrc tests/transport_test.cpp src/TurboLink.cpp -o /tmp/mc-transport-test && /tmp/mc-transport-test
for case in cold return 2x 4x 8x mixed stress burst interruption realtime; do
  python3 -B tests/paired_avr_replay.py --case "$case" || exit
done
python3 -B tests/din_rx_fault_replay.py
python3 -B tests/active_usb_setup.py
```

The default fixture generates four synthetic records with Machinedrum framing and sizes of 1,176, 1,176, 873 and 813 bytes. Both commands also accept a four-record `.syx` capture with those sizes as an optional positional argument.

The `realtime` case checks USB IN bank boundaries and byte order after a bounded host read pause. The pre-v1.0.1 images deliver the complete fixture bytes at the USB boundary but place a realtime event after a same-cable SysEx continuation in one bank, which triggers legacy CoreMIDI truncation. The DIN fault test injects UART faults with valid data queued on either side. The pre-v1.0.1 main image discards those bytes.

`mixed` keeps Port 1 at 1×, negotiates Port 2 to 8× and replays the four records in opposite orders on the two cables. `burst` overlaps three distinct 8,068-byte records per port in both directions at 8×, reverses the record order on Port 2 and pauses host reads for 128 ms. The model checks the byte stream and the legacy host's bank-splitting condition.

`active_usb_setup.py` adds a read-only USB device-descriptor request every 31 ms to the stress workload. Each request sets the endpoint's SETUP flag. The interrupt mask and CPU interrupt enable determine when the stock USB handler runs. The test checks every descriptor reply, all four MIDI streams and the 16U2 stack boundary.

### Comparing compiled images

Build each revision with its matching 2560 ELF. Run the same copy of the harness against both pairs. The profiler accepts explicit `--images`, `--elf` and `--objdump` paths. For an unpaused 4,038-byte workload use `--case 8x`.

```sh
python3 -B tests/profile_avr.py --case stress --output /tmp/dual-turbo-profile.json
```

The four transfers start together. Throughput for a direction includes initial buffering from submission until the last byte arrives. Each DIN-to-host latency starts at the scheduled end of a DIN byte and ends at the host's IN poll. Each host-to-DIN latency starts when that USB OUT bank enters the endpoint and ends when its DIN byte leaves the modeled shift register. It omits time a bank waits in the host's submission queue. Gap statistics use consecutive payload bytes within a record, including interleaved user realtime and excluding the bridge's active sensing. Percentiles use nearest rank.

CPU totals cover the replay and its 12 ms drain. Both processors poll continuously. The profiler counts instruction cycles and vector-entry time, attributes IRQ and foreground cycles separately, and measures maximum observed ISR duration. It samples the 2560 queues on entry and return from `serviceUsbOut` to avoid reading a half-updated 16-bit index. Those high-water values are lower bounds. The 16U2 TX ring counter is observed on every write. SRAM margin uses the lowest modeled stack pointer since boot and reserved static memory. These timings assume the model's interrupt, XMEM, SPI and endpoint behavior and 1 ms host polling. They measure neither real-time host scheduling nor a hardware worst-case stack bound.

Run the peer-failure replay and the isolated front-panel probe from the same directory.

```sh
python3 -B tests/negotiation_fault_replay.py
python3 -B tests/panel_replay.py
```

The peer replay uses the matched HEX pair. It checks the next clean SysEx, running-status and clock traffic on both ports after a missing reply, an incompatible answer or simultaneous requests. It also tests a consumed DIN control between channel messages, and loss of a 2× peer while the other link keeps delivering at 4×. The UART fault replay includes cutoffs with zero, one and two valid SysEx payload bytes still awaiting a USB packet when the error arrives. It also queues a complete note between two errors before the foreground can read the ring and verifies the exact coalesced loss interval.

The panel probe compiles `src/Panel.cpp` and a small driver for the ATmega2560 with AVR GCC, then runs them in the instruction model. It compares OLED pixel data, the cycles for the first activity service call and all SPI bytes until the indicator settles. SPI-ready is immediate in the model. This measures renderer work. It does not qualify physical panel timing or contrast. Pass `--source /path/to/other/src` to measure another version against the same checks.

### USB transport and fault recovery

Run `python3 -B tests/usb_transport_replay.py` with the same matched images and AVR binutils as the paired replay. Use `--case` for one scenario and `--help` for the list. Each scenario boots both images and exercises the compiled USB and DIN paths.

The cases check ordinary and raw USB-MIDI events, running status, truncated banks, prefix clocks, stalled USB data endpoints and USB bus reset. They inject an invalid credit in each internal direction and a bad CRC toward the USB processor, then verify a fresh message on every route. Host-to-DIN timing tests cover both a missing EOX followed by notes and records streamed in fragments every 48 ms.

At negotiated 8×, `long-stall` holds host IN for 600 ms while two peers send distinct 15,000-byte dumps. `overflow` extends that pause to 800 ms with 20,000 bytes per port, which exhausts the raw rings. It requires an exact surviving prefix, one recovery EOX and intact subsequent traffic on both ports. `disconnect` keeps host IN paused while one peer goes silent and the other continues sending Active Sensing. Endpoint halt and peer behavior in these cases are modeled. Verify physical DIN OUT and host-driver behavior separately.

### Installation and capture checks

Run `python3 -B tests/recovery_replay.py --images build`. Supply `--from-images /path/to/release/build` to start PAGE recovery on an older pair. This test executes both application images, sends STK500v2 sign-on over CDC OUT, checks the reply on CDC IN, models bootloader success/error replies, and restarts the 2560. It follows a held PAGE, a late host baud-rate change, re-enumeration, and the read-only speed query on both ports. The modeled DFU step substitutes the flashed USB application at power cycle. Actual flash programming and verification require the DIY hardware.

Compile `tests/passive_coremidi_capture.c` on macOS with `clang -O2 -fblocks -framework CoreMIDI -framework CoreFoundation tests/passive_coremidi_capture.c -o /tmp/mc-capture`. Run `/tmp/mc-capture /local/path/first 120`, initiate the saved dump after **Armed**, then repeat into a fresh prefix with the same selection. For the clock-interleaved Machinedrum or Monomachine dump formats used on this bench:

```sh
python3 -B tests/compare_elektron_dump.py first-port2.midistream repeat-port2.midistream --ump repeat-port2-ump.txt
```

This checks every non-realtime byte, record boundary, 14-bit length and checksum. It expects capture windows containing the instrument dump and realtime only. Store captures outside tracked source.

Run `python3 -B tests/cdc_setup_replay.py --images build` to check configured CDC class requests alongside serial bootloader traffic. The fixture retries a sign-on after a missing reply, sends four read requests and supplies four 128-byte synthetic readbacks at 115200. The host alternates GET_LINE_CODING and reapplying the current settings during those readbacks. The model dispatches setup, UART and timer interrupts during the transfer, checks every serial byte in both directions and observes stack reservations and pushes on the 16U2. The reported minimum SRAM gap applies to this schedule.

Run `python3 -B tests/usb_reconfiguration.py --images build` for three USB session changes with an open SysEx in each direction on each port. The host deconfigures and reconfigures, reselects the current configuration, then deconfigures and selects a new address before reconfiguring. Every setup uses actual firmware interrupt masks and polling. Each new session must emit one DIN recovery F7 per port, discard orphan continuations on USB and DIN, and pass the next note exactly in both directions.

### Physical 1× DIN loopback (macOS)

Use an isolated DIN cable from the sending OUT to the named return IN on the same MegaCMD. Either a same-port return or crossed cables work. The bench used OUT 1 → IN 2 and OUT 2 → IN 1. Set both participating ports to fixed 1×, stop host routing/thru and clock, and wait for traffic to settle. Host timestamps include USB and CoreMIDI scheduling.

Build the tool and generate its two distinct four-record fixtures locally. Each file has 4,038 bytes in records of 1,176, 1,176, 873 and 813 bytes. Each record starts with the noncommercial manufacturer ID `7D`, the ASCII tag `DTL` and its port and record numbers. Select the fixture by the sending DIN OUT port. The tool accepts only that port's exact generated bytes. `--generate` and `--check` work offline.

```sh
clang -std=c11 -Wall -Wextra -Werror tests/physical_loopback.c \
  -framework CoreMIDI -framework CoreFoundation -o /tmp/isolated-test
bench_dir=$(mktemp -d)
/tmp/isolated-test --generate 'MegaCMD Port 1' "$bench_dir/p1.syx"
/tmp/isolated-test --generate 'MegaCMD Port 2' "$bench_dir/p2.syx"
/tmp/isolated-test --check 'MegaCMD Port 1' 'MegaCMD Port 2' "$bench_dir/p1.syx"
```

For the two crossed cables, run these commands. For a same-port return, use that port's name for both the first argument (OUT) and the second argument (IN).

```sh
/tmp/isolated-test --run 'MegaCMD Port 1' 'MegaCMD Port 2' "$bench_dir/p1.syx" "$bench_dir/p1.midistream"
/tmp/isolated-test --run 'MegaCMD Port 2' 'MegaCMD Port 1' "$bench_dir/p2.syx" "$bench_dir/p2.midistream"
```

The utility matches the named online source and destination to the same hardware, checks the input for 500 ms of silence, then sends all four records back to back. It compares every received byte and listens for at least 600 ms of trailing silence after the expected count. A missing, reordered, duplicate or extra byte fails the test. A seven-second deadline bounds waits on disconnected or noisy loops. Input and capture files stay local. Use a fresh filename for every capture.
