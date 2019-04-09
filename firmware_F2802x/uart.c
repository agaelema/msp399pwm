/*
 * uart.c
 *
 *  Created on: Apr 8, 2019
 *      Author: nconrad
 */

#include <stdlib.h>
#include <stdbool.h>

#include "F2802x_Device.h"     // F2802x Headerfile Include File
#include "main.h"

#include "common/include/clk.h"
#include "common/include/gpio.h"
#include "common/include/pie.h"
#include "common/include/sci.h"

#include "ring_buffer.h"
#include "uart.h"

#define PIE_GROUP_SCIA (PIE_GroupNumber_9)
#define TXBUF_BITS 5

static SCI_Handle mySci;
static PIE_Handle myPie;

static void scia_fifo_init(SCI_Handle mySci);
__interrupt void sciaTxFifoIsr(void);
__interrupt void sciaRxFifoIsr(void);

void error(void);

volatile bool cmdComplete = false;
volatile size_t rxBufLen = 0;
volatile RB_ELM_TYPE rxbuf[RXBUF_SIZE+1]; // leave a spot for a null terminator

volatile bool tx_active = false;
volatile RB_ELM_TYPE txbuf[1<<TXBUF_BITS];
struct ring_buffer tx_rb;



// Uses RX=GPIO28:SCIRXDA:pin48 , TX=GPIO29:SCITXDA:pin1

void uart_init(CLK_Handle myClk, GPIO_Handle myGpio, PIE_Handle pieHandle) {
    myPie = pieHandle;

    tx_rb.head = 0;
    tx_rb.tail = 0;
    tx_rb.buf = txbuf;
    tx_rb.n_bits = TXBUF_BITS;

    GPIO_setPullUp(myGpio, GPIO_Number_28, GPIO_PullUp_Enable);
    GPIO_setPullUp(myGpio, GPIO_Number_29, GPIO_PullUp_Disable);
    GPIO_setQualification(myGpio, GPIO_Number_28, GPIO_Qual_ASync);
    GPIO_setMode(myGpio, GPIO_Number_28, GPIO_28_Mode_SCIRXDA);
    GPIO_setMode(myGpio, GPIO_Number_29, GPIO_29_Mode_SCITXDA);

    mySci = SCI_init((void *)SCIA_BASE_ADDR, sizeof(SCI_Obj));

    CLK_enableSciaClock(myClk);

    SCI_disableParity(mySci);
    SCI_setNumStopBits(mySci, SCI_NumStopBits_One);
    SCI_setCharLength(mySci, SCI_CharLength_8_Bits);

    // 57600 ???
#if (CPU_FRQ_60MHZ)
    SCI_setBaudRate(mySci, (SCI_BaudRate_e)194);
#elif (CPU_FRQ_50MHZ)
    SCI_setBaudRate(mySci, (SCI_BaudRate_e)26);
#elif (CPU_FRQ_40MHZ)
    SCI_setBaudRate(mySci, (SCI_BaudRate_e)129);
#endif

    SCI_enableTx(mySci);
    SCI_enableRx(mySci);
    SCI_enableTxInt(mySci);
    SCI_enableRxInt(mySci);

    scia_fifo_init(mySci);

    EALLOW;    // This is needed to write to EALLOW protected registers
    //PieVectTable.SCIRXINTA = &sciaRxFifoIsr;
    ((PIE_Obj *)myPie)->SCIRXINTA = &sciaRxFifoIsr;
    //PieVectTable.SCITXINTA = &sciaTxFifoIsr;
    ((PIE_Obj *)myPie)->SCITXINTA = &sciaTxFifoIsr;
    EDIS;      // This is needed to disable write to EALLOW protected registers

    //
    // Register interrupt handlers in the PIE vector table
    //
    PIE_registerPieIntHandler(myPie, PIE_GROUP_SCIA, (PIE_SubGroupNumber_e)PIE_InterruptSource_SCIARX,
                              (intVec_t)&sciaRxFifoIsr);
    PIE_registerPieIntHandler(myPie, PIE_GROUP_SCIA, (PIE_SubGroupNumber_e)PIE_InterruptSource_SCIATX,
                              (intVec_t)&sciaTxFifoIsr);

    SCI_enable(mySci);
}

static void scia_fifo_init(SCI_Handle mySci)
{
    SCI_enableFifoEnh(mySci);
    SCI_resetTxFifo(mySci);
    SCI_clearTxFifoInt(mySci);
    SCI_resetChannels(mySci);
    SCI_setTxFifoIntLevel(mySci, SCI_FifoLevel_Empty);

    SCI_resetRxFifo(mySci);
    SCI_clearRxFifoInt(mySci);
    SCI_setRxFifoIntLevel(mySci, SCI_FifoLevel_1_Word);

    return;
}
// 0 for success
uint16_t uart_putc(uint16_t c) {
    uint16_t r = rb_put(&tx_rb, c);
    if(!tx_active) { // data can be lost here, if the buffer overflowed.
        tx_active = true;
        if(0 == rb_get(&tx_rb, &c))
            SCI_putDataNonBlocking(mySci, c);
    }
    return r;
}
// 0 on success
uint16_t uart_write(uint16_t *data, size_t len) {
    uint16_t r = 0;
    uint16_t i;
    for(i=0; i<len; i++) {
        if(rb_put(&tx_rb, data[i])) {
            r = 1;
            break; // give up on overflow
        }
    }
    if(!tx_active) { // data can be lost here, if the buffer overflowed.
        uint16_t c;
        tx_active = true;
        if(0 == rb_get(&tx_rb, &c))
            SCI_putData(mySci, c);
    }
    return r;
}

//
// sciaTxFifoIsr -
//
__interrupt void sciaTxFifoIsr(void)
{
    RB_ELM_TYPE r, d;

    // more bytes?
    do {
        r = rb_get(&tx_rb, &d);
        if(r == 0) { // success
            SCI_putDataBlocking(mySci, d);
        } else {
            tx_active = false;
        }
    } while(tx_active && SCI_isTxReady(mySci));

    SCI_clearTxFifoInt(mySci);
    PIE_clearInt(myPie, PIE_GROUP_SCIA);

    return;
}

//
// sciaRxFifoIsr -
//
__interrupt void sciaRxFifoIsr(void) {
    RB_ELM_TYPE d;
    bool overflowed = false;

    while(SCI_getRxFifoStatus(mySci) != SCI_FifoLevel_Empty) {
        d = SCI_getData(mySci);
        if(cmdComplete) // disregard if previous CMD has not been handled
            continue;

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
            }
        }
    }

    SCI_clearRxFifoOvf(mySci);
    SCI_clearRxFifoInt(mySci);
    PIE_clearInt(myPie, PIE_GROUP_SCIA);

    return;
}