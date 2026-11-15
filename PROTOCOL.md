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

## Optional encoder reports

Build the `megacommand_mcdt` target to expose the MegaCommand panel. Reports travel upstream on Port 1 and do not go to either instrument.

```text
F0 7D 4D 43 4C 02 01 seq buttons A B C D F7
```

- `seq` increments modulo 128. A gap indicates reports may have been missed. Resynchronize on the first report after a disconnect.
- The low four bits of `buttons` are the current debounced pressed states of A–D.
- The four deltas use a bias of 64. `40` hex means no change, `41` means +1, and `3F` means −1.
- Idle reports arrive about twice per second. During a long Port 1 SysEx receive they wait until that stream can accept a complete message. Treat the button mask as sampled state.

[The passive capture example](examples/passive-capture.html) listens to either or both MIDI inputs and saves raw SysEx without sending MIDI. For applications that control bridge speed, [the Web MIDI adapter](examples/web-midi.mjs) takes a paired input and output obtained with `navigator.requestMIDIAccess({sysex: true})` in a secure context. Call `querySpeed()` or `setLimit(4)` as needed and receive ordinary messages through `onMidi`. Other MIDI backends can send and parse the same bytes.

## Transfer behavior

If a pending speed change is blocked by an incomplete MIDI message, the bridge abandons the incomplete message after 750 ms without payload and once buffered bytes have drained. Realtime clock and Active Sensing do not extend the wait. A USB transport reset discards queued traffic, so a librarian must restart any interrupted transfer. DIN cannot slow an upstream sender when host reads stall long enough to exhaust the receive buffers.
