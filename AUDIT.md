# Dual Turbo MIDI candidate qualification

This review started at GitHub main and v1.0.1, both `39bc248daa31e7a1cab6e9db508be1df3c4175fe`. The older local bench checkout has separate history. Its recordings supplied the earlier physical evidence. The pinned PlatformIO environment (Atmel AVR 5.1.0, avr-gcc 7.3.0) reproduced both v1.0.1 HEX files byte-for-byte. The v1.0.0 2560 rebuild also matched the published `28632909…` image. Both releases shipped the same `dfc3b0a1…` 16U2 image. The current candidate images are identified by [SHA256SUMS](SHA256SUMS). The initial audit and extended hardware qualification below were performed with ATmega2560 `d5dd98ae` and ATmega16U2 `85941b0d` at `1044182`. The panel revision uses ATmega2560 `56c423d0` with the same 16U2 image.

## Current OLED revision

The panel shows the settled rate above each port and leaves its I/O indicators visible. In normal operation the small per-port label reads READY at 1× or TURBO at a confirmed faster rate. MAX appears only while editing or negotiating, with LINKING on the lower line during negotiation. An explicit limit edit starts a 1.5-second preview on that port. Latched faults take priority over USB wait and negotiation details.

The compiled pixel regression checks both columns, the LEDs and all four activity indicators. It exercises independent cap expiry, repeated edits and timer wrap. The production-image regression operates SAVE and LOAD through the modeled button shift register and sends a host limit on one cable. A read-only probe left both ports READY and both DIN outputs quiet. After a missing peer on Port 2, both ports passed fresh notes in both directions.

A full redraw for each preview transition took 608 SPI bytes in the first implementation. The final renderer uses three 16-column writes for a single label (57 SPI bytes including commands). Each activity edge still uses seven SPI bytes. The button scan skips debounce work when the sampled and debounced states already match. The ATmega2560 uses 22,310 bytes of flash and 2,789 bytes of static internal SRAM.

| Dual 8× stress, Port 1 / Port 2 | v1.0.1 | 1044182 | Current OLED |
| --- | ---: | ---: | ---: |
| DIN OUT maximum gap (ms) | 2.553 / 2.480 | 2.474 / 1.983 | 2.034 / 1.974 |
| DIN OUT p99 gap (ms) | 0.797 / 0.788 | 0.889 / 0.875 | 0.728 / 0.722 |
| DIN → USB throughput (B/s) | 10,842 / 10,852 | 11,071 / 11,076 | 11,211 / 11,211 |
| USB → DIN throughput (B/s) | 9,680 / 9,680 | 10,002 / 10,001 | 9,977 / 9,976 |
| First admitted USB OUT bank to first DIN byte (ms) | 1,079.6 / 1,081.3 | 932.0 / 934.3 | 1,030.2 / 1,032.0 |

The stress results use the same paired AVR workload and 128 ms host stall. All 32 paired qualification jobs and the native transport regression passed on the revised pair, including the public fixture and the local 4,038-byte dump. Byte counts matched in both directions on both ports. Compared with the installed 1044182 build, the final image reduced the largest and 99th-percentile within-record gaps, while the first DIN byte arrived about 98 ms later under this load. [Current stress profile](tests/measurements/candidate-stress.json) · [Current short profile](tests/measurements/candidate-short.json)

On the connected unit, the local USB update command selected serial mode. AVRDUDE verified the installed 21,848-byte image, wrote the new 22,310-byte ATmega2560 image in 88 pages and verified the programmed bytes by read-back. The previously verified ATmega16U2 stayed installed. The board returned to USB MIDI and each port answered the read-only speed query exactly once at 1× on its own input. The operator confirmed READY on both physical OLED columns. Fault-specific OLED screens and timing were checked in the model.

## Findings on v1.0.1

| Priority | Reproduced behavior on v1.0.1 | Candidate change and regression |
| --- | --- | --- |
| High | In configured MIDI mode a normal USB control transfer can enter the stock 16U2 SETUP handler during deep transport work. The handler reserves 42 stack bytes and re-enables interrupts. The paired model stopped a pending nested push at 0x299, in reserved SRAM. Ordinary streaming alone had 15 bytes of measured margin. | Service USB control at shallow stack depth while allowing the UART and timer to run. Exercise asynchronous descriptor reads during four-way traffic, using the firmware's interrupt masks and a guard on stock SRAM. |
| High | Deconfigure/reconfigure without a bus reset kept the old session alive. A SysEx continuation from the former host session could complete the old DIN message. | Verify configuration changes by sending standard SET/GET requests, discard pending work, and require a new clean message on both routes. |
| High | A 600 ms host IN stall at 8× falsely timed out a live peer. Port 1 delivered 6,123 of 15,000 source bytes, a recovery F7 and a 1× report while its raw ring still had capacity. | Observe fresh ISR writes while backpressure prevents foreground reads. Compare every byte after a bounded stall and confirm a genuinely silent peer still falls back. Also exhaust both raw rings, verify the retained prefixes and recovery terminators, and compare the next clean traffic. |
| High | A channel message after an interrupted host SysEx could wait behind the prefill gate. On the baseline, the first complete note reached modeled DIN OUT after 176 ms. Sparse fragments of a still-open message could keep extending the wait. | Release prefill when host status interrupts SysEx. Bound the wait for a head-of-queue F0 at sparse and dense input rates. Time at the UART shift register and compare all trailing notes. |
| Medium | A consumed DIN Turbo frame left prior channel running status active. Two orphan data bytes became a phantom note. A UART error at an unaligned SysEx cutoff also discarded one or two known-good bytes before the gap. | Clear the channel parser when consuming the reserved SysEx and include buffered valid bytes in a CIN 6/7 recovery event. Exercise both ports and all three byte alignments. |
| Medium | A realtime byte could overtake a complete three-byte manufacturer prefix that still resembled bridge control. | Retain its position while the prefix is undecided, subject to a four-event capacity and two firmware millisecond deadline. Exercise both directions, control recognition, clock bursts and timer wrap. |
| Medium | Cancelling after the peer's rate ACK, before Test1, could resume outgoing application traffic at 1× while the peer was at the test rate. | Observe the peer recovery interval after ACK. Test cancellation, an overlapping request and incompatible-peer fallback. |
| Medium | WAIT USB hid latched faults, requested caps disappeared, and changing one activity indicator sent 608 SPI bytes. | Show settled rate, MAX and latched fault simultaneously. For an activity-only change, update the four columns of one indicator per tick. The compiled panel probe dropped from 11,417 to 963 modeled cycles on the first service and from 608 to 7 SPI bytes to settle. |
| Performance | Under simultaneous 8× transfers the 2560 collected, repacked and then encoded a compact return bank. | Encode the cable bit and SysEx continuations while emitting the internal frame. Preserve the same-cable bank split and measure the complete pair under the same workload below. |

## Data paths and recovery boundaries

- **DIN IN to host.** USART1 and USART2 feed independent, page-indexed 12,032-byte external receive rings. The RX ISR records FE/DOR and software overflow at the ring position. Foreground serves the valid prefix, terminates an already-emitted interrupted SysEx, waits for a new status and resumes on that cable. Each DIN control filter precedes the cable's running-status/SysEx packetizer and 8 KiB event queue. A rotating first choice feeds 64-byte USB-MIDI banks over the shared 1 Mbaud internal USART. A CIN F event following open SysEx on that cable starts a fresh bank. The 16U2 checks the framed CRC, expands compact events, and commits to the USB IN endpoint.
- **USB OUT to DIN.** The 16U2 validates cable, CIN, status and payload length, canonicalizes short-event padding, then reserves byte credit per DIN port and one internal batch credit. The 2560 validates framing and CRC, filters bridge control and queues application bytes in the destination's 8 KiB external TX queue. Its 256-byte local TX ring feeds the USART via UDRE. Only UDR/shift-register timing measures the modeled DIN output. An endpoint pair or a speed reply alone verifies the USB control route.
- **Limits.** An external DIN sender cannot be backpressured indefinitely. Apart from deliberate prefilling, a shared USB endpoint and ordered USB OUT stream can couple queueing latency across ports even with separate per-port byte credits. A session reset invalidates traffic in flight on both ports. Two RX gaps still pending when foreground reaches the first coalesce through the last. A valid message entirely between those gaps can be discarded. Realtime can precede a still-incomplete one- or two-byte DIN SysEx group in the USB event stream. The per-cable bank rule protects the legacy CoreMIDI EOX behavior at emitted event boundaries.
- **Qualification.** A recovery F7 only closes an interrupted message. The tests compare the valid prefix and the complete successor and check cable placement. Reconnect, query each endpoint afresh, and restart interrupted librarian transfers.

## Physical and modeled coverage at 1044182

| Path or case | Evidence |
| --- | --- |
| Historical physical Port 1, MD Mk I, 1× | Legacy CoreMIDI trace rechecked against the local original. All 4,038 bytes in order, 73 clocks inside records and a 74th outside, zero extra F7s. The older failure trace contained 24 extra F7s. These captures identify the v1.0.1 main image. [Release bench](BENCH_SYSEX.md) |
| Physical reference on the connected board | macOS 26.6.2 (25G83). Read-only query `F0 7D 4D 43 4C 01 01 F7` on each pair gave one settled 1× report on its own input, zero on the other. MD Mk I on Port 1 and Monomachine Mk II on Port 2, paired DIN IN/OUT. No outgoing test dumps sent to either instrument. |
| Port 2 reference integrity | The first take was already in progress when capture opened. The second, fully armed take contains 3 type 0x52, 4 type 0x67 and 4 type 0x6F records (4,611 SysEx bytes). All stored lengths and checksums validate, and the independent CoreMIDI UMP rendering agrees with the legacy byte rendering. Zero realtime clocks inside these records. Both renderings observe the same host and USB path. |
| Serial upgrade on the connected board | PAGE update mode enumerated as USB serial. AVRDUDE Wiring/STK500v2 at 115200 identified `1e9801` and verified all 20,902 installed v1.0.1 bytes. Wrote and read back all 21,848 candidate bytes in 86 pages. Board automatically returned to USB MIDI with the previous USB application. Both read-only probes returned 1× on their own ports. |
| Final physical 1× replays | Port 1 MD Mk I delivered the same 4,038 bytes as the private fixture, 74 interleaved clocks, zero early or extra F7s. Port 2 MnM Mk II delivered all 4,611 bytes in the same order as the complete pre-flash reference (SysEx SHA-256 `9e13b750f7e51f40df6b47d1e32ce84a2ac8abe3e5c0011847a4cfe2e7b4c5ca`). Both legacy and UMP renderings agreed. Host-timestamp spans from first to last SysEx were 1,323 ms / 1,483 ms. The dumps in this window did not overlap. |
| Physical USB-controller upgrade | Hardware DFU (bootloader 0x20) provided a read-back of the installed USB image. All 10,930 bytes matched v1.0.1. dfu-programmer 1.1.0 erased the application area, wrote the candidate and validated the flash. The board subsequently enumerated in dual MIDI mode. An independent second read-back matched every one of its 11,144 application bytes and 120 erased padding bytes. |
| Physical rate negotiation | Settled rates from sequential independent reports (P1/P2): 1×/1×, 2×/1×, 2×/2×, 4×/2×, 4×/4×, then 4×/8× with both caps at 8×. The MD settled at 4× under the 8× cap; the speed reply does not identify the reason. Both ports then returned to 1× and each read-only check received its reply on the addressed port only. |
| Physical mixed-rate replays | The MD at 4× delivered all 4,038 source bytes with 19 clocks inside the records and zero extra F7s. The Monomachine at 8× delivered the same 11 checksummed records and 4,611 bytes as the reference. Both CoreMIDI renderings agreed on both ports. The host-timestamp spans were 332 ms and 195 ms. The instruments started 1.85 s apart, so this take qualifies mixed rates with sequential dumps. |
| Physical DIN OUT/IN loopbacks at 1× | With both instruments removed, distinct four-record 4,038-byte probes identified the effective routes as USB OUT 1 → USB IN 2 and USB OUT 2 → USB IN 1. Each payload matched on the opposite return. Simultaneous 12-record back-to-back transfers then delivered all 15,357 bytes from each output on its expected return (30,714 bytes total), in order with exact boundaries. First-to-last host callback spans were 4,910 ms on both returns. The checked-in loopback tool repeated the four-record transfers simultaneously, each with a 600 ms quiet window to detect trailing bytes. This exercises both physical DIN paths and identifies their end-to-end routing. A separate receiver is needed to timestamp individual output edges. |
| Physical interrupted SysEx at 1× | On both crossed loops at once, macOS submitted a SysEx7 Start plus 32 Continue packets (198 payload bytes per output), then held the messages open for two seconds. Before sending anything else, the legacy and UMP receivers on both inputs already had 198 matching stream bytes and no EOX. Two distinct clean records followed on each output. Both APIs received both complete records exactly once and in order. From submitting the first clean record to its final return-packet timestamp took 4.07 ms for OUT 1 and 4.87 ms for OUT 2. The interrupted record eventually contained all 198 payload bytes plus F0 and one F7. Assigning that F7 to a particular firmware or driver stage would require an independent tap. |
| Paired model, correct traffic | Synthetic four-record fixture and private 4,038-byte Machinedrum capture. Cold and fixed 1×, negotiated 2×/4×/8×, return, mixed rates, concurrent traffic and multi-record bursts. Byte comparison at USB banks and at the modeled DIN shift registers. |
| Paired model, faults | Missing/incompatible peers, simultaneous requests, loss of one active peer, malformed USB events, halt/resume, bad CRC and credit in both internal directions, USB reset, UART FE/DOR, burst overflows, and first complete message after recovery. Native tests also cover the transient faster test exchange. |
| Installer model | PAGE startup on each old release and on candidate, CDC endpoints and line coding, bidirectional serial bytes, interrupted/retried programming replies, successful leave, late host baud change and re-enumeration. STK500v2 reply traffic is modeled. Hardware bootloader flash writes and DFU entry are separate bench checks. |
| Cold boot and first traffic on the matched pair | The operator removed power. macOS observed the USB device depart and re-enumerate at the same USB location. The operator initially held PAGE and then rebooted normally. After the normal reboot, each read-only query returned 1× on its own input only. Both isolated loops simultaneously returned their first four-record 4,038-byte fixture exactly, with 600 ms of trailing quiet. |
| Physical checks outstanding | Independent wire-level DIN timing, sustained four-way load at Turbo, physical host endpoint stalls, cable loss during active Turbo, v1.0.0 upgrade, deliberately failed uploads, fresh install from stock, physical OLED/LED inspection through faults, and restore to full MCL await bench qualification. Full MCL 5.02 and its 1.0.4 bundle were checked against the upstream release listing; the local USB bundle matches its published SHA-256. |

Hardware input captures stay local. The public tree includes the comparator, test cases and aggregate results.

## Reproducing the comparisons

Use the [test commands](tests/README.md) and the same version of the harness when passing each pair with `--images`. For profiling, also pass its matching ATmega2560 ELF via `--elf`. The profiler checks the ELF against the HEX before reading queue symbols. The four-record local replay input had SHA-256 `5390a83f29c8819d763b35442c051b8c40411e2c3c8ca5f2ee4776fd14913d66`. The public synthetic fixture has the same record sizes and exercises the same routes.

The stress run starts four simultaneous transfers, negotiates both ports to 8× and pauses host IN reads for 128 ms. Each port delivers 24,070 bytes to the host and 24,068 bytes to DIN. The short profile uses the public synthetic four-record fixture at 8× without the stall, with three inserted realtime bytes per incoming stream and one per outgoing stream. Throughput includes startup prefill. DIN receive latency runs from the modeled wire byte's completion to the USB host read. Transmit latency runs from admission of its USB OUT bank to the DIN byte's completion. It excludes waiting in the host's submission queue. SPI, interrupt arbitration, external RAM cycles, UART FIFOs and 1 ms USB polling are modeled. Queue maxima on the 2560 are sampled lower bounds. Neither timing nor stack measurements establish worst-case hardware limits.

### Dual 8× stress, 128 ms host stall at 1044182

| Metric, Port 1 / Port 2 | v1.0.1 | Candidate |
| --- | ---: | ---: |
| DIN → USB throughput (B/s) | 10842 / 10852 | 11071 / 11076 |
| USB → DIN throughput (B/s) | 9680 / 9680 | 10002 / 10001 |
| First admitted USB OUT bank to first DIN byte (ms) | 1079.6 / 1081.3 | 932.0 / 934.3 |
| DIN RX → USB read, p50 latency (ms) | 604.9 / 604.2 | 635.6 / 635.2 |
| DIN RX → USB read, p99 latency (ms) | 1245.1 / 1244.3 | 1200.1 / 1198.5 |
| USB OUT admission → DIN, p50 latency (ms) | 6.04 / 6.15 | 5.53 / 5.66 |
| USB OUT admission → DIN, p99 latency (ms) | 1066.9 / 1068.6 | 923.1 / 925.6 |
| DIN OUT p99 within-record gap (µs) | 797.12 / 787.69 | 889.38 / 874.62 |
| DIN OUT maximum within-record gap (µs) | 2552.94 / 2480.12 | 2473.69 / 1982.75 |
| DIN OUT gap peak-to-peak jitter (µs) | 2512.94 / 2440.12 | 2433.69 / 1942.75 |
| DIN raw RX high-water (B) | 8114 / 8105 | 8129 / 8123 |
| To-host queue high-water (B) | 8104 / 8104 | 8084 / 8084 |
| To-DIN queue high-water (B) | 7150 / 7150 | 7150 / 7150 |
| DIN UART TX high-water (B) | 244 / 248 | 246 / 249 |

| Per processor, 2560 / 16U2 | v1.0.1 | Candidate |
| --- | ---: | ---: |
| Application flash (B) | 20902 / 10930 | 21848 / 11144 |
| Static internal SRAM reserved (B) | 2765 / 414 | 2763 / 414 |
| Observed stack use from boot (B) | 186 / 83 | 186 / 85 |
| Observed internal SRAM margin (B) | 5241 / 15 | 5243 / 13 |
| Total modeled CPU cycles | 39964160 / 39964158 | 38685089 / 38685088 |
| Modeled cycles in ISRs | 12623035 / 7279856 | 12599144 / 7245659 |
| Share of CPU cycles spent in ISRs (%) | 31.586 / 18.216 | 32.568 / 18.73 |

### Unpaused 4,038-byte synthetic fixture at negotiated 8× at 1044182

| Metric, Port 1 / Port 2 | v1.0.1 | Candidate |
| --- | ---: | ---: |
| DIN → USB throughput (B/s) | 9690 / 9690 | 10077 / 10077 |
| USB → DIN throughput (B/s) | 9492 / 9454 | 9773 / 9732 |
| First admitted USB OUT bank to first DIN byte (ms) | 151.3 / 153.6 | 143.5 / 145.8 |
| DIN OUT p99 within-record gap (µs) | 40.25 / 40.25 | 40.25 / 40.25 |
| DIN OUT maximum within-record gap (µs) | 40.38 / 199.56 | 40.38 / 192.88 |

With three notes submitted 12 ms apart after an interrupted dump, first-note completion fell from 176.219 / 176.118 ms to 2.228 / 2.299 ms. In the sparse-stream tests the first DIN byte moved from 1,347.073 / 1,348.664 ms to 150.655 / 1,252.264 ms for two different input rates. At 600 ms of stalled host IN, both 15,000-byte streams were exact. At 800 ms and 20,000 incoming bytes per port, the rings overflowed; the surviving 18,139 / 18,145-byte prefixes, one recovery F7 each, and both subsequent clean messages matched.

Both directions gained throughput in the concurrent stress run. The largest DIN OUT gaps fell on both ports. The 99th-percentile gaps increased on both, and median DIN-to-host latency rose by about 31 ms. The shared transport still queues roughly a second of work under this load. The higher ISR percentage reflects a shorter overall run; compare the absolute cycle counts as well.

In a separate stressed EP0 run, the candidate handled 78 device-descriptor reads, and a separate run handled 78 Port 2 string-descriptor reads. All MIDI bytes matched; the lowest observed stack margin during these transfer windows was 18 B. The ordinary stress trace, measured from boot, reached 13 B. CDC SET/GET requests at 115200 during four synthetic bootloader readbacks preserved 54 outbound and 565 inbound serial bytes, with 15 B of observed stack margin. These small margins remain relevant when changing USB firmware.

Full per-vector maxima, latency percentiles, queue samples, flash identities and CPU attribution are in the [v1.0.1 stress](tests/measurements/v1.0.1-stress.json), [1044182 stress](tests/measurements/1044182-stress.json), [v1.0.1 short](tests/measurements/v1.0.1-short.json) and [1044182 short](tests/measurements/1044182-short.json) profiles.
