# Dual-Turbo bridge interface

## Connect any host or DIN peer

Each MIDI pair on the host corresponds to one physical DIN IN/OUT pair. Select the input and output for the same cable. Port 1 and Port 2 have no fixed instrument roles in firmware. A native app, browser with Web MIDI SysEx access, DAW, or librarian can use either pair. A host application can implement its own routing, clock, editor, librarian or controller on top.

The bridge passes channel voice, system common, realtime, and manufacturer SysEx through the selected port. It does not interpret Elektron kits, patterns, songs, or extension data. The bridge reserves the specific control frames below and manages Turbo keepalive on DIN. Realtime messages can occur during SysEx and should be handled separately by a byte-stream parser. Each DIN port starts at standard MIDI speed, 31.25 kbaud. A peer that implements compatible Elektron TurboMIDI can negotiate a faster common wire rate over a paired IN and OUT connection. Stock firmware and custom firmware use the same byte path. A particular firmware's Turbo rate and SysEx behavior must be confirmed on that build.

## Per-port control

Send these messages on the USB MIDI destination paired with the DIN port being controlled. Replies come back on that port's USB MIDI source. Ordinary MIDI applications can use the negotiated port without issuing any of these commands.

| Direction | Bytes | Meaning |
| --- | --- | --- |
| Host → bridge | `F0 00 20 3C 04 00 01 F7` | TM-1-compatible probe. Start Turbo on an idle port when its cap allows it, then report the settled DIN rate. An active link reports its current rate. |
| Host → bridge | `F0 7D 4D 43 4C 01 01 F7` | Read the settled DIN rate without starting Turbo. |
| Host → bridge | `F0 00 20 3C 04 00 03 ss F7` | Set the rate limit and initiate negotiation where applicable. |
| Bridge → host | `F0 00 20 3C 04 00 02 ss F7` | The settled rate, after a query, reconnect or rate change. Turbo confirmation requires a status byte received from the peer at the final baud rate. A batch containing data bytes alone cannot confirm the link. |

Use limit code `01` or legacy `00` for 1×, `02` for 2×, `04` for 4× or `07` for 8×. A 1× cap keeps the port at standard MIDI speed until the cap changes or the panel starts Turbo. The result may be below the cap. When the bridge initiates and an attempted Turbo rate fails its test or lacks final-rate confirmation, it can retry at a lower rate. If negotiation fails or an active link is lost, the affected port returns to 1×. Reapplying a cap above the current rate asks that port to negotiate again. The rate is the DIN wire rate. The two ports share a USB transport, so their concurrent sustained throughput can be lower.

Change speeds between transfers with one initiator per DIN pair. A simultaneous attempt can time out and settle at 1×. Retry from one end once both peers are idle. Speed replies are asynchronous, carry no request ID, and may wait for a MIDI message boundary. Treat an absent report as unknown. On disconnect, invalidate in-flight requests and the displayed rate. On reconnect, rebind both endpoints and query again. The live DIN link may have kept its previous rate. Retry interrupted instrument transfers at the application level.

For stock equipment or an unknown model, begin at 1× or use a conservative 4× cap during qualification. Raise the cap only after the peer is stable. For unidirectional connections, use 1×. Active Sensing used for Turbo is link-local. The bridge also consumes exact DIN TurboMIDI handshake frames and a host-side USB recovery command. These reserved frames are not a general-purpose tunnel.

## Web MIDI examples

[The passive capture example](examples/passive-capture.html) listens to either or both MIDI inputs and saves raw SysEx without sending MIDI. For applications that control bridge speed, [the Web MIDI adapter](examples/web-midi.mjs) takes a paired input and output obtained with `navigator.requestMIDIAccess({sysex: true})` in a secure context. Call `querySpeed()` or `setLimit(4)` as needed and receive ordinary messages through `onMidi`. Other MIDI backends can send and parse the same bytes.

## Transfer behavior

If a pending speed change is blocked by an incomplete MIDI message, the bridge abandons the incomplete message after 750 ms without payload and once buffered bytes have drained. Realtime clock and Active Sensing do not extend the wait. A USB transport reset discards queued traffic, so a librarian must restart any interrupted transfer. DIN cannot slow an upstream sender when host reads stall long enough to exhaust the receive buffers.

The control filter briefly buffers prefixes that could address the bridge. If the prefix turns out to be ordinary SysEx, up to four interleaved realtime messages retain their byte positions through this filter. While the foreground is servicing the stream, a two-millisecond deadline or a fifth realtime byte releases the waiting realtime messages even if the prefix remains ambiguous. The filter can still recognize the eventual control. This bounds the delay on a clock when the sender stops mid-header. On DIN input, USB-MIDI emits SysEx in groups of three bytes. A realtime event arriving inside an incomplete group can reach USB before that group. A realtime event following a completed same-cable SysEx group still starts a fresh USB IN bank.

On a DIN UART fault in a SysEx that has already reached USB, the bridge emits any remaining buffered bytes from before the fault together with a recovery F7. A receiver must validate payload lengths and content before treating the transfer as complete. If several UART errors arrive before the foreground reaches the first error, the raw DIN ring marks one lost interval from the first error through the last. A complete message within that interval is discarded. The next valid status after the last error resumes parsing. A recognized DIN Turbo control also ends any preceding channel running status.

### USB buffering and recovery

Each DIN pair has its own input ring, packetizer and output byte queue. The two pairs share the internal 1 Mbit/s UART and the USB endpoints. The sender escapes the internal frame markers, attaches a CRC-8 and observes per-port byte credits and shared bank credits. The receiver checks the CRC before making a bank visible. An internal framing error, invalid grant, USB bus reset or a host selecting the USB configuration again can discard in-flight traffic on both cables. Wait for enumeration and a read-only speed reply on each pair, then resend the interrupted message from its start.

For host-to-DIN SysEx, the bridge can prefill an open message to reduce gaps during concurrent dumps. It releases a complete record immediately when its turn reaches the head of the queue. It also releases the pending start when a subsequent channel status ends the record. For an open record the prefill ends at 7,168 bytes, after 150 ms without new payload, or after 1,250 ms from its start. After 150 ms it also releases a queue that accumulated fewer than one byte per elapsed millisecond. These are limits on deliberate buffering. Queued earlier MIDI, negotiation, the DIN serialization time and USB scheduling add to the time a host observes.

During a host IN pause, the main MCU can keep receiving into external SRAM. While the packet queue is full, fresh writes to the raw DIN ring maintain the negotiated link. Once these buffers fill, the affected port can lose part of the current message. A received F7 after a fault can be a recovery terminator. Verify the expected length and contents before accepting a dump.
