#include "driverlib.h"
#include "uart.h"
#include "pwm.h"
#include "main.h"
#include "cmd.h"
#include "tmp411.h"
#include <string.h>
// globals!
bool reportTemps = false;


// XT1 is 8MHz
// DCOCLKDIV (FLL) is XT1*3=24MHz
// SMCLK is (DCOCLKDIV/1) = 24 MHz (timer_d)
// ACLK is (XT1) = 8 MHz
void main(void)
{
    WDT_A_hold(WDT_A_BASE);

    // Configure XT1
    // Port select XT1
    // COREV_0 <= 12M
    // COREV_1 <= 16M
    // COREV_2 <= 20M
    // COREV_3 <= 25.5M

    PMM_setVCore (PMMCOREV_3);

#ifdef START_XTAL
    // XTAL pins
    GPIO_setAsPeripheralModuleFunctionInputPin(
          		  GPIO_PORT_PJ,
          		  GPIO_PIN4 + GPIO_PIN5
          		  );
    UCS_setExternalClockSource(8000000,0);

    UCS_turnOnHFXT1(UCS_XT1_DRIVE_2);
    // FLLREFDIV = 1
    // UCSCLT1: DCORSEL=6 (range)
    // UCSCLT2: FLLD=000b/Div1 ; FLLN=3
    // UCSCTL3: SELREF=000b/XT1CLK; FLLREFDIV=000/Div1
    UCS_initClockSignal(UCS_FLLREF, UCS_XT1CLK_SELECT, UCS_CLOCK_DIVIDER_2);

    // Initialize DCO to 12MHz
    UCS_initFLLSettle(24000,
                  6);

    UCS_initClockSignal(UCS_MCLK, UCS_DCOCLKDIV_SELECT, UCS_CLOCK_DIVIDER_2);
    UCS_initClockSignal(UCS_SMCLK, UCS_DCOCLK_SELECT, UCS_CLOCK_DIVIDER_1);
    UCS_initClockSignal(UCS_ACLK, UCS_DCOCLKDIV_SELECT, UCS_CLOCK_DIVIDER_8); // 1.5 MHz for USCI

#else

#endif


    uart_init();
    pwm_init();
    tmp411_init();
    // Enable interrupts!
    __enable_interrupt();
    while(true) {
        __delay_cycles(1000000);
        processCmds();
        uint8_t str[8];
        uint16_t lt = tmp411_getLocal();
        uint16_t rt = tmp411_getRemote();
        if(reportTemps) {
            uart_write("T",1);
            u16hex(lt,(char*)str,16);
            uart_write(str, 4);
            uart_write(",", 1);
            u16hex(rt,(char*)str,16);
            uart_write(str, 4);
            uart_write("\r",1);
        }
    }

    __bis_SR_register(LPM0_bits + GIE);       // Enter LPM0
    __no_operation();                         // For debugger
}

