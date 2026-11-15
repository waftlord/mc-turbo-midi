/* Entry points used by the stock 1.0.4 image. Addresses are verified by
   build_usb_firmware.py before applying this overlay. */
.section .uart_midi_hook, "ax", @progbits
    call dualUsbInit
    jmp 0x700
    nop

.section .midi_loop_hook, "ax", @progbits
    call dualUsbPoll
    jmp 0x149a

.section .cdc_rx_hook, "ax", @progbits
    call dualCdcRead

.section .cdc_poll_hook, "ax", @progbits
    jmp dualCdcPoll
    nop
    nop
    nop

.section .text.dual_cdc, "ax", @progbits
dualCdcRead:
    /* The stock ISR has already saved the call-clobbered registers. */
    lds r24, 0xc8
    lds r22, 0xce
    call dualCdcTrackStartup
    mov r22, r24
    ret

dualCdcPoll:
    call dualCdcResumeMidi
    in r25, 0x3f
    cli
    lds r24, 0x1dc
    out 0x3f, r25
    jmp 0x1344

.section .timer_keepalive_hook, "ax", @progbits
    /* The stock timer can send raw legacy Active Sense. Framed MIDI has
       its own liveness checks. Keep the timer's system tick. */
    nop

.section .midi_desc_hook, "ax", @progbits
    jmp dualMidiDescriptor

.section .text.dual_descriptor, "ax", @progbits
dualMidiConfig:
    .byte 9, 2, dualMidiConfigEnd - dualMidiConfig, 0, 2, 1, 0, 0xc0, 0x32
    .byte 9, 4, 0, 0, 0, 1, 1, 0, 0
    .byte 9, 0x24, 1, 0, 1, 9, 0, 1, 1
    .byte 9, 4, 1, 0, 2, 1, 3, 0, 0
midiStreaming:
    .byte 7, 0x24, 1, 0, 1, dualMidiConfigEnd - midiStreaming, 0
    .byte 6, 0x24, 2, 1, 1, 4
    .byte 6, 0x24, 2, 2, 2, 0
    .byte 9, 0x24, 3, 1, 3, 1, 2, 1, 4
    .byte 9, 0x24, 3, 2, 4, 1, 1, 1, 0
    .byte 6, 0x24, 2, 1, 5, 5
    .byte 6, 0x24, 2, 2, 6, 0
    .byte 9, 0x24, 3, 1, 7, 1, 6, 1, 5
    .byte 9, 0x24, 3, 2, 8, 1, 5, 1, 0
    .byte 9, 5, 0x01, 2, 64, 0, 5, 0, 0
    .byte 6, 0x25, 1, 2, 1, 5
    .byte 9, 5, 0x82, 2, 64, 0, 5, 0, 0
    .byte 6, 0x25, 1, 2, 3, 7
dualMidiConfigEnd:
    .balign 2

dualMidiDescriptor:
    ldi r24, lo8(dualMidiConfigEnd - dualMidiConfig)
    ldi r25, 0
    ldi r30, lo8(dualMidiConfig)
    ldi r31, hi8(dualMidiConfig)
    jmp 0x0ccc

.section .usb_reset_hook, "ax", @progbits
    jmp dualUsbReset

.section .text.dual_reset, "ax", @progbits
dualUsbReset:
    push r24
    in r24, 0x3f
    push r24
    lds r24, 0xe1
    sbrs r24, 3
    rjmp 1f
    lds r24, 0x282
    inc r24
    sts 0x282, r24
1:
    pop r24
    out 0x3f, r24
    pop r24
    jmp 0x1038

.section .rx_vector_hook, "ax", @progbits
    jmp dualRx

.section .tx_vector_hook, "ax", @progbits
    jmp dualTx

.section .text.dual_uart, "ax", @progbits
/* MIDI receive ring is 64 bytes at 01dd. CDC uses the stock receive ISR. */
dualRx:
    push r18
    in r18, 0x3f
    push r18
    lds r18, 0x11b
    cpi r18, 2
    brne .LcdcRx
    push r19
    push r30
    push r31
    lds r19, 0xc8
    lds r18, 0xce
    andi r19, 0x1c
    brne .LrxError
    lds r30, 0x27f
    mov r19, r30
    inc r19
    andi r19, 63
    lds r31, 0x280
    cp r19, r31
    breq .LrxError
    ldi r31, 0
    subi r30, lo8(-0x1dd)
    sbci r31, hi8(-0x1dd)
    st Z, r18
    sts 0x27f, r19
    rjmp .LrxDone
.LrxError:
    ldi r18, 1
    sts 0x281, r18
.LrxDone:
    pop r31
    pop r30
    pop r19
    pop r18
    out 0x3f, r18
    pop r18
    reti
.LcdcRx:
    pop r18
    out 0x3f, r18
    pop r18
    jmp 0x11f0

/* The same TX ring serves CDC and the two MIDI cables. */
dualTx:
    push r18
    in r18, 0x3f
    push r18
    push r19
    push r30
    push r31
    lds r19, 0x1dc
    tst r19
    breq .LtxEmpty
    lds r30, 0x1da
    lds r31, 0x1db
    ld r18, Z+
    sts 0xce, r18
    cpi r30, 0xd8
    brne .LtxSave
    ldi r30, 0x58
.LtxSave:
    sts 0x1da, r30
    dec r19
    sts 0x1dc, r19
    brne .LtxDone
.LtxEmpty:
    lds r18, 0xc9
    andi r18, 0xdf
    sts 0xc9, r18
.LtxDone:
    pop r31
    pop r30
    pop r19
    pop r18
    out 0x3f, r18
    pop r18
    reti

.section .midi_string_hook, "ax", @progbits
    jmp dualMidiString

.section .text.dual_strings, "ax", @progbits
dualMidiString:
    cpi r24, 4
    breq 1f
    cpi r24, 5
    breq 2f
    clr r25
    cpi r24, 1
    jmp 0xd18
1:
    ldi r30, lo8(portOneName)
    ldi r31, hi8(portOneName)
    rjmp 3f
2:
    ldi r30, lo8(portTwoName)
    ldi r31, hi8(portTwoName)
3:
    lpm r24, Z
    clr r25
    jmp 0xccc
portOneName:
    .byte 14, 3
    .word 'P', 'o', 'r', 't', ' ', '1'
portTwoName:
    .byte 14, 3
    .word 'P', 'o', 'r', 't', ' ', '2'
