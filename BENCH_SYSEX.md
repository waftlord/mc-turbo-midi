# SysEx bench results for v1.0.1

The 4,038-byte control contains Kit 8, Kit 9, Pattern D05 and Pattern D06 from a Machinedrum running MDX X.20C. All four stored lengths and checksums validated.

| Capture | Record sizes in bytes |
| --- | --- |
| Control | 1176, 1176, 873, 813 |
| Early C6/64 | 250, 55, 73, 70 |
| First SysEx Librarian | 34, 52, 37, 40 |
| SysEx Librarian after Turbo → 1× | 118, 145, 169, 115 |
| Latest C6/64 at 1× | 208, 307, 325, 52 |

All 16 failed records matched the control up to the last complete three-byte group, then ended with an early F7. A genuine DIN loss can produce that shape. To distinguish the causes, passive capture recorded the legacy CoreMIDI byte stream with clock on and off on macOS 26.6.2 (25G83).

### Physical results

| Port 1 at 1× | CoreMIDI result |
| --- | --- |
| Installed firmware, clock on | Legacy capture inserted 24 extra F7s. The first apparent record sizes were 316, 118, 19 and 16 bytes. |
| Installed firmware, tempo and CTRL OUT disabled | Legacy capture and a separate CoreMIDI MIDI 1.0 UMP capture both delivered all 4,038 bytes exactly. |
| ATmega2560 candidate `b42a7e51`, clock and CTRL OUT enabled | Legacy capture returned all 4,038 bytes exactly, with 73 interleaved F8 clocks and zero extra F7s. |

On the original clock-on run, every extra F7 was appended to a three-byte SysEx fragment in a legacy MIDIPacket, immediately before a separate F8 packet with the same timestamp. All original continuation bytes still arrived, which rules out a packetizer abort for that run. The earliest observed divergence was the first legacy F7 at offset 315 of Kit 8. The AVR regression isolates the USB bank pattern that triggers this host behavior. The MC OLED showed no active fault during the baseline reproduction. After the candidate replay, the read-only status command returned a settled 1× independently on both ports.

The candidate starts a new USB IN bank before a realtime event when the current bank already holds open SysEx data for that cable. The firmware preserves emitted event order and USB-MIDI 1.0 event encoding, including CIN F for clock. A clock at the start of a bank can still share that bank with later SysEx data. The ATmega2560 image was written and verified on the bench through the serial bootloader. The rebuilt ATmega16U2 image has the same SHA-256 as the prior release image.

Physical items pending qualification are Port 2 dump capture, independent capture or loopback of DIN OUT on both ports, hardware Turbo rates and return to 1×, concurrent physical send/receive, host stalls and forced faults.

### Paired AVR model results

The [replay tests](tests/README.md) disassemble and run both candidate HEX files. They capture USB IN bank boundaries and compare outgoing DIN bytes from modeled USART shift registers. They verify the following cases.

| Case | Result |
| --- | --- |
| Cold 1× and return to 1× after 8× | All 4,038 bytes in each direction on each port. DIN OUT adjacent bytes at cold 1× spaced 320–320.375 µs in the model. |
| Negotiated 2×, 4× and 8×, both ports active | Each port delivered 4,041 exact DIN-to-host bytes with three realtime inserts and 4,039 exact host-to-DIN bytes with one realtime insert. Minimum modeled DIN OUT spacing was 160, 80 and 40 µs respectively. |
| Concurrent long dump at 8× with a 128 ms host IN stall | Each port delivered 24,070 exact DIN-to-host bytes and 24,068 exact host-to-DIN bytes. Maximum adjacent within-SysEx outgoing payload gaps were 2.553 ms and 2.480 ms. There were zero unexpected UART overruns or USB link resets. |
| Genuine interruption | Injected UART framing and overrun faults preserved the valid prefix, emitted one recovery F7, and preserved the next complete dump. On the send path, an injected internal CRC error closed the interrupted DIN message and the next 1,176-byte record was exact on each port. |

The old images fail the USB IN bank regression because a bank includes same-cable SysEx followed by realtime. They also fail the separate UART-gap regression because the receive loop drops all unread bytes, including the start of the next clean record. The candidate records the gap position in the DIN raw ring, serves queued bytes before the gap and resumes from bytes queued after it. Multiple faults that accumulate before the reader reaches the first gap are treated as one gap through the last fault.

Receive audit covered the USART status bits and raw ring, the packetizer's handling of unexpected status and rate transitions, queue limits, and internal CRC/credit/USB reset recovery. The 16U2's USB IN path expands compact continuations and commits a bank only after CRC validation. Reset clears the 2560 receive queue and parser. On the send path the 16U2 validates cable/CIN, canonicalizes USB event padding and reserves credit before sending to the 2560. The 2560 enqueues DIN bytes in order and the USART paces them at the settled rate.
