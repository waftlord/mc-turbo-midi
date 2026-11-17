#include <avr/io.h>
#include "FastUartLayout.h"

/* Keep the fast path within the byte-time budget on all three USARTs.
   These non-nesting ISRs own GPIOR0/1/2 for register saves. */
.macro uart_save
    out _SFR_IO_ADDR(GPIOR0), r18
    in r18, _SFR_IO_ADDR(SREG)
    out _SFR_IO_ADDR(GPIOR1), r18
    out _SFR_IO_ADDR(GPIOR2), r19
    push r30
    push r31
.endm

.macro uart_restore
    pop r31
    pop r30
    in r19, _SFR_IO_ADDR(GPIOR2)
    in r18, _SFR_IO_ADDR(GPIOR1)
    out _SFR_IO_ADDR(SREG), r18
    in r18, _SFR_IO_ADDR(GPIOR0)
    reti
.endm

.macro uart_complete
    /* Refill a pending test before another higher-priority UART vector
       can postpone it. The register saves already belong to this ISR. */
    lds r19, fastTestUart
    tst r19
    breq .Lrestore\@
    cpi r19, 1
    brne .LcheckTest2\@
    lds r19, _SFR_MEM_ADDR(UCSR1A)
    sbrc r19, RXC1
    rjmp .LserviceTest1
    rjmp .Lrestore\@
.LcheckTest2\@:
    lds r19, _SFR_MEM_ADDR(UCSR2A)
    sbrc r19, RXC2
    rjmp .LserviceTest2
.Lrestore\@:
    uart_restore
.endm

.macro uart_rx vector, uart, reg, extended=0, testPort=0, otherPort=0, otherReg=0
.global \vector
\vector:
    uart_save
.if \extended
    lds r19, \reg + 2
    sbrc r19, UMSEL01
    rjmp .LserviceTest\testPort
    clt
.LserviceDin\testPort:
.endif
    lds r19, \reg
    lds r18, \reg + 6
    andi r19, 0x1c
    brne 2f
    lds r30, \uart
    inc r30
.if \extended
    lds r31, \uart + UART_RX_EPOCH
    brne 4f
    inc r31
    cpi r31, DIN_RX_PAGES
    brlo 4f
    clr r31
4:
.endif
    lds r19, \uart + 1
    cp r19, r30
    brne 5f
.if \extended
    lds r19, \uart + UART_TAIL_EPOCH
    cp r19, r31
    brne 5f
.endif
    ldi r19, 0x80
2:
    lds r18, \uart + 4
.if \extended
    /* Mark the gap between valid queued bytes. A burst of errors before
       the reader reaches the gap extends it through the most recent loss. */
    lds r30, \uart
    lds r31, \uart + UART_RX_EPOCH
    sts \uart + UART_LAST_RX_LOSS, r30
    sts \uart + UART_LAST_RX_LOSS + 1, r31
    tst r18
    brne .LextendLoss\@
    sts \uart + UART_FIRST_RX_LOSS, r30
    sts \uart + UART_FIRST_RX_LOSS + 1, r31
.LextendLoss\@:
.endif
    or r19, r18
    sts \uart + 4, r19
    rjmp 1f
5:
    /* The consumer runs with interrupts enabled, outside this ISR. */
    sts \uart, r30
.if \extended
    tst r30
    brne 6f
    sts \uart + UART_RX_EPOCH, r31
    subi r31, 1
    brsh 6f
    ldi r31, DIN_RX_PAGES - 1
6:
    dec r30
    subi r31, hi8(-\extended)
.else
    dec r30
    ldi r31, 0
    subi r30, lo8(-(\uart + 5))
    sbci r31, hi8(-(\uart + 5))
.endif
    st Z, r18
1:
.if \extended
    /* A DIN receive chained from the test must return to the interrupted
       code, even if another sample became ready while storing the byte. */
    brts .LrestoreDin\@
.endif
    uart_complete
.if \extended
.LrestoreDin\@:
    uart_restore
.LserviceTest\testPort:
    /* One RX interrupt refills the double-buffered SPI transmitter.
       Idle 0xff keeps sampling RX while holding MIDI TX high. */
    lds r19, \reg
    sbrc r19, TXC0
    rjmp .LtestGap\@
    lds r30, \uart + 3
    lds r19, \uart + 2
    cp r19, r30
    breq .LtestIdleTx\@
    mov r19, r30
    inc r19
    ldi r31, 0
    subi r30, lo8(-(\uart + UART_TX_BUFFER))
    sbci r31, hi8(-(\uart + UART_TX_BUFFER))
    ld r18, Z
    sts \reg + 6, r18
    sts \uart + 3, r19
    ldi r19, 2
    sts \uart + UART_TEST_TX_DRAIN, r19
    rjmp .LtestRx\@
.LtestIdleTx\@:
    ldi r18, 0xff
    sts \reg + 6, r18
    lds r19, \uart + UART_TEST_TX_DRAIN
    tst r19
    breq .LtestRx\@
    dec r19
    sts \uart + UART_TEST_TX_DRAIN, r19
.LtestRx\@:
    lds r18, \reg + 6
    cpi r18, 0xff
    brne .LtestRxSignal\@
    lds r19, \uart + UART_TEST_IDLE
    cpi r19, 4
    breq .LtestReturn\@
    inc r19
    rjmp .LtestRxStore\@
.LtestRxSignal\@:
    clr r19
.LtestRxStore\@:
    /* Keep enough trailing high samples to finish any partial UART byte. */
    sts \uart + UART_TEST_IDLE, r19
    lds r30, \uart
    lds r31, \uart + 1
    inc r30
    cp r31, r30
    breq .LtestOverrun\@
    sts \uart, r30
    dec r30
    ldi r31, 0
    subi r30, lo8(-(\uart + 5))
    sbci r31, hi8(-(\uart + 5))
    st Z, r18
.LtestReturn\@:
    /* Empty the other DIN's hardware RX FIFO before USB can claim the
       next vector. Its 8× payload continues through the normal RX ring. */
    lds r19, \otherReg
    sbrs r19, RXC0
    rjmp .LrestoreDin\@
    set
    rjmp .LserviceDin\otherPort
.LtestGap\@:
    /* A sampling gap invalidates the exchange. Let the main loop retry. */
    lds r18, \reg + 6
    ldi r18, (1 << FE0)
    rjmp .LtestFault\@
.LtestOverrun\@:
    ldi r18, 0x80
.LtestFault\@:
    lds r19, \uart + 4
    or r18, r19
    sts \uart + 4, r18
    ldi r19, (1 << RXEN0) | (1 << TXEN0)
    sts \reg + 1, r19
    lds r19, \uart + 2
    sts \uart + 3, r19
    clr r19
    sts fastTestUart, r19
    sts \uart + UART_TEST_TX_DRAIN, r19
    rjmp .LtestReturn\@
.endif
.endm

.macro uart_tx vector, uart, reg, check_idle=1
.global \vector
\vector:
    uart_save
    lds r30, \uart + 3
    lds r19, \uart + 2
    cp r19, r30
    breq 1f
    mov r19, r30
    inc r19
    ldi r31, 0
    subi r30, lo8(-(\uart + UART_TX_BUFFER))
    sbci r31, hi8(-(\uart + UART_TX_BUFFER))
    ld r18, Z
    sts \reg + 6, r18
.if \check_idle
    ldi r18, (1 << TXC0)
    sts \reg, r18
.endif
    sts \uart + 3, r19
    rjmp 2f
1:
    lds r18, \reg + 1
    andi r18, ~(1 << UDRIE0)
    sts \reg + 1, r18
2:
    uart_complete
.endm

uart_rx USART0_RX_vect, usbUart, _SFR_MEM_ADDR(UCSR0A)
uart_tx USART0_UDRE_vect, usbUart, _SFR_MEM_ADDR(UCSR0A), 0
uart_rx USART1_RX_vect, midiUart1, _SFR_MEM_ADDR(UCSR1A), DIN_1_RX, 1, 2, _SFR_MEM_ADDR(UCSR2A)
uart_tx USART1_UDRE_vect, midiUart1, _SFR_MEM_ADDR(UCSR1A)
uart_rx USART2_RX_vect, midiUart2, _SFR_MEM_ADDR(UCSR2A), DIN_2_RX, 2, 1, _SFR_MEM_ADDR(UCSR1A)
uart_tx USART2_UDRE_vect, midiUart2, _SFR_MEM_ADDR(UCSR2A)
