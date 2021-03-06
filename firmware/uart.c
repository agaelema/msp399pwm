/*
 * uart.c
 *
 *  Created on: Mar 20, 2019
 *      Author: nconrad
 */

#include <stdlib.h>
#include "driverlib.h"
#include "ring_buffer.h"
#include "uart.h"

#define TXBUF_BITS 5

volatile bool cmdComplete = false;
volatile size_t rxBufLen = 0;
volatile uint8_t rxbuf[RXBUF_SIZE+1]; // leave a spot for a null terminator

volatile bool tx_active = false;
volatile uint8_t txbuf[1<<TXBUF_BITS];
struct ring_buffer tx_rb;

void uart_init() {
    GPIO_setAsPeripheralModuleFunctionOutputPin(GPIO_PORT_P1, GPIO_PIN1);
    GPIO_setAsPeripheralModuleFunctionInputPin (GPIO_PORT_P1, GPIO_PIN2);
    USCI_A_UART_initParam  params =
    {
      // 8 MHz, 38400, 8N1
      //
     .selectClockSource = USCI_A_UART_CLOCKSOURCE_ACLK,
     .clockPrescalar = 26,
     .firstModReg = 0,
     .secondModReg = 0,
     .overSampling = 0,
     .parity = USCI_A_UART_NO_PARITY,
     .msborLsbFirst = USCI_A_UART_LSB_FIRST,
     .numberofStopBits = USCI_A_UART_ONE_STOP_BIT,
     .uartMode = USCI_A_UART_MODE
    };

    if ( STATUS_FAIL == USCI_A_UART_init ( USCI_A0_BASE, &params )) {
        return;
    }
    USCI_A_UART_enable(USCI_A0_BASE);
    USCI_A_UART_enableInterrupt (USCI_A0_BASE, USCI_A_UART_RECEIVE_INTERRUPT | USCI_A_UART_TRANSMIT_INTERRUPT); // Interrupt can't be enabled before enable is called...
    tx_rb.head = 0;
    tx_rb.tail = 0;
    tx_rb.buf = txbuf;
    tx_rb.n_bits = TXBUF_BITS;
}
// 0 for success
uint8_t uart_putc(uint8_t c) {
    uint8_t r = rb_put(&tx_rb, c);
    if(!tx_active) { // data can be lost here, if the buffer overflowed.
        tx_active = true;
        if(0 == rb_get(&tx_rb, &c))
            USCI_A_UART_transmitData(USCI_A0_BASE, c);
    }
    return r;
}
// 0 on success
uint8_t uart_write(uint8_t *data, size_t len) {
    uint8_t r = 0;
    uint8_t i;
    for(i=0; i<len; i++) {
        if(rb_put(&tx_rb, data[i])) {
            r = 1;
            break; // give up on overflow
        }
    }
    if(!tx_active) { // data can be lost here, if the buffer overflowed.
        uint8_t c;
        tx_active = true;
        if(0 == rb_get(&tx_rb, &c))
            USCI_A_UART_transmitData(USCI_A0_BASE, c);
    }
    return r;
}
static uint8_t baseFourChar(uint8_t baseFour) {
    baseFour = baseFour & 0x0F;
    if(baseFour < 10)
        return '0' + baseFour;
    return 'A' + baseFour - 10;
}
// Convert 16-bit hex to string.
// Does not append NULL.
void u16hex(uint32_t value, char* result, uint8_t bits) {
    uint8_t i = 0;
    for( ; bits>0 ; bits-=4) {
        result[i++] = baseFourChar(value >> (bits-4));
    }
    return;
}

#pragma vector=USCI_A0_VECTOR
__attribute__((ramfunc))
__interrupt
void USCI_A0_ISR(void) {
    uint8_t d, r;

    //PAOUT_H |= (1 << (6)); // set p2.6
    switch(__even_in_range(UCA0IV,4))  {
    // Vector 2 - RXIFG
    case USCI_UCRXIFG:

        d =HWREG8(USCI_A0_BASE + OFS_UCAxRXBUF);
        if(cmdComplete) // disregard if previous CMD has not been handled
            return;

        if(rxBufLen < RXBUF_SIZE)
            rxbuf[rxBufLen++] = d;
        else
            rxBufLen = 0; /* Overflow, clear buffer */

        if(d == '\n' || d == '\r') {
            if(rxBufLen == 1) {
                rxBufLen = 0;
            } else {
                // Line complete
                cmdComplete = true;
                __bic_SR_register_on_exit (LPM0_bits);
            }
        }
        break;
    case USCI_UCTXIFG:
        // tx_active must be true
        r = rb_get(&tx_rb, &d);
        if(r == 0) { // success
            HWREG8(USCI_A0_BASE + OFS_UCAxTXBUF) = d;
        } else {
            tx_active = false;
        }
        break;
    default:
        break;
    }

    //PAOUT_H &= ~(1 << (6)); // clear p2.6
}

