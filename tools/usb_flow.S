/* MIDI-only hooks for the pinned MegaCommand DIY 1.0.4 ATmega16U2 image.
 * Flash addresses are byte addresses. USB endpoint numbers come from that
 * image's MIDI descriptors. The CDC and EP0 paths use their original code.
 */

.equ SREG_IO, 0x3f
.equ USB_STATE, 0x1e
.equ CONFIGURED, 4
.equ USB_MODE, 0x011b
.equ MIDI_MODE, 2
.equ MIDI_OUT_ENDPOINT, 1
.equ MIDI_IN_ENDPOINT, 2

.equ UDINT, 0xe1
.equ UEINTX, 0xe8
.equ UENUM, 0xe9
.equ UECONX, 0xeb
.equ UEDATX, 0xf1
.equ UEBCLX, 0xf2
.equ SUSPEND_OR_RESET, 0x09
.equ STALL_OR_ENABLE, 0x21
.equ EPEN, 0
.equ TXINI, 0
.equ RXOUTI, 2
.equ RWAL, 5

.equ UART_TX_COUNT, 0x01dc
.equ UART_TX_BUFFER, 0x0158
.equ UCSR1B, 0x00c9
.equ UDRIE1, 5
.equ ADMISSION_LIMIT, 124
.equ ENQUEUE_BYTE, 0x0744
.equ RELOAD_SENSE_TIMER, 0x07b6

.section .midi_out_hook, "ax", @progbits
    jmp midi_out_flow

.section .active_sense_hook, "ax", @progbits
    jmp active_sense_flow
    nop

.section .midi_in_wait_hook, "ax", @progbits
    jmp midi_in_ready

.section .midi_in_write_hook, "ax", @progbits
    jmp midi_in_write

.section .midi_in_flush_hook, "ax", @progbits
    jmp midi_in_flush

.section .midi_in_realtime_hook, "ax", @progbits
    /* Keep the parser's fixed event pointer in r16:r17. */
    rjmp MIDI_IN_LOOP
    .space 8

.section .midi_in_event_hook, "ax", @progbits
    /* Both event paths submit at the end of the receive pass. */
    rjmp MIDI_IN_LOOP
    .space 2

.section .usb_flow, "ax", @progbits
midi_out_flow:
    /* The stock baud change can leave bytes queued with UDRIE1 clear. */
    lds r24, UART_TX_COUNT
    tst r24
    breq .Lselect_out
    lds r24, UCSR1B
    sbrc r24, UDRIE1
    rjmp .Lcheck_room
    ori r24, (1 << UDRIE1)
    sts UCSR1B, r24
.Lcheck_room:
    /* Allow up to 3 bytes plus one timer enqueue in the 128-byte ring. */
    lds r24, UART_TX_COUNT
    cpi r24, ADMISSION_LIMIT
    brsh .Lservice_in

.Lselect_out:
    in r21, SREG_IO
    cli
    ldi r24, MIDI_OUT_ENDPOINT
    rcall select_midi_endpoint
    brne .Lskip_out
    lds r25, UEINTX
    sbrs r25, RXOUTI
    rjmp .Lskip_out
    lds r24, UEBCLX
    cpi r24, 4
    brlo .Lrelease_out
    sbrs r25, RWAL
    rjmp .Lskip_out
    movw r30, r28
    adiw r30, 5
    ldi r25, 4
.Lread_out_byte:
    lds r24, UEDATX
    st Z+, r24
    dec r25
    brne .Lread_out_byte

    /* A bus reset can disable the endpoint even with interrupts masked. */
    lds r24, UECONX
    sbrs r24, EPEN
    rjmp .Lskip_out
    lds r24, UEBCLX
    cpse r24, r1
    rjmp .Ldecode_out
    rcall release_out_bank
.Ldecode_out:
    out SREG_IO, r21
    rjmp DECODE_OUT_EVENT
.Lrelease_out:
    /* Free a zero-length bank or discard a final incomplete event. */
    rcall release_out_bank
.Lskip_out:
    out SREG_IO, r21
.Lservice_in:
    rjmp SERVICE_MIDI_IN

release_out_bank:
    /* Write ones to preserve the unrelated write-zero-to-clear flags. */
    ldi r24, 0xfb             /* Acknowledge RXOUTI first. */
    sts UEINTX, r24
    ldi r24, 0x7f             /* Then release FIFOCON. */
    sts UEINTX, r24
    ret

active_sense_flow:
    /* The stock timer can remain enabled across a switch to CDC. */
    lds r24, USB_MODE
    cpi r24, MIDI_MODE
    brne .Lreload_sense
    lds r24, UART_TX_COUNT
    cpi r24, ADMISSION_LIMIT
    brsh .Ldrain_uart
    ldi r24, lo8(UART_TX_BUFFER)
    ldi r25, hi8(UART_TX_BUFFER)
    /* r22 is 0xfe from the instruction before the hook. */
    call ENQUEUE_BYTE
.Ldrain_uart:
    lds r24, UCSR1B
    ori r24, (1 << UDRIE1)
    sts UCSR1B, r24
.Lreload_sense:
    jmp RELOAD_SENSE_TIMER

midi_in_ready:
    /* Check bank room before the stock parser pops another UART byte. */
    in r21, SREG_IO
    cli
    rcall select_midi_in
    out SREG_IO, r21
    /* SBRS tests the saved endpoint flags without using SREG. */
    sbrs r24, RWAL
    rjmp MIDI_IN_LOOP_FINISH
    dec r12
    ldi r24, 0xff
    rjmp MIDI_IN_LOOP_CONTINUE

midi_in_write:
    in r21, SREG_IO
    cli
    rcall select_midi_in
    sbrs r24, RWAL
    rjmp .Lfinish_in
    /* Events are written four bytes at a time to an aligned 64-byte FIFO. */
    movw r30, r22
    ldi r24, 4
.Lwrite_in_byte:
    ld r25, Z+
    sts UEDATX, r25
    dec r24
    brne .Lwrite_in_byte
    rjmp .Lfinish_in

midi_in_flush:
    in r21, SREG_IO
    cli
    rcall select_midi_in
    sbrs r24, TXINI
    rjmp .Lfinish_in
    lds r24, UEBCLX
    cpse r24, r1
    rjmp .Lsubmit_in
.Lfinish_in:
    out SREG_IO, r21
    ret
.Lsubmit_in:
    /* Clear TXINI before FIFOCON and keep the IN KILLBK command clear. */
    ldi r24, 0xfa
    sts UEINTX, r24
    ldi r24, 0x7b
    sts UEINTX, r24
    rjmp .Lfinish_in

select_midi_in:
    ldi r24, MIDI_IN_ENDPOINT
    rcall select_midi_endpoint
    brne .Lno_in_bank
    lds r24, UEINTX
    sbrc r24, TXINI
    ret
.Lno_in_bank:
    clr r24
    ret

/* Call with I clear. Return Z set for a usable endpoint. r21 holds saved
 * SREG in the caller. r22:r23 (IN event pointer), Y and r1 stay intact.
 */
select_midi_endpoint:
    in r25, USB_STATE
    cpi r25, CONFIGURED
    brne .Lreturn_endpoint
    lds r25, UDINT
    andi r25, SUSPEND_OR_RESET
    brne .Lreturn_endpoint
    sts UENUM, r24
    lds r25, UECONX
    andi r25, STALL_OR_ENABLE
    cpi r25, (1 << EPEN)
.Lreturn_endpoint:
    ret
