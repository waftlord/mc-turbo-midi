#pragma once

#define UART_TX_BUFFER 261
#define UART_RX_EPOCH 517
#define UART_TAIL_EPOCH 518
#define UART_TEST_IDLE 521
#define UART_TEST_TX_DRAIN 522
#define UART_FIRST_RX_LOSS 523
#define UART_LAST_RX_LOSS 525

// The receive rings start on page boundaries and use the rest of SRAM bank 0.
#define DIN_1_RX 0xa200
#define DIN_2_RX 0xd100
#define DIN_RX_SIZE 0x2f00
#define DIN_RX_PAGES (DIN_RX_SIZE / 256)
