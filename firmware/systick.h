/*
 * systick.h
 *
 *  Created on: Mar 23, 2019
 *      Author: nconrad
 */

#ifndef SYSTICK_H_
#define SYSTICK_H_

#include <stdint.h>

void systick_init();
uint16_t systick_get();

#endif /* SYSTICK_H_ */
