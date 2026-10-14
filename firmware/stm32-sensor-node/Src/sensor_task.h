/*
 * sensor_task.h — the task that owns the BMP280 and the I2C bus.
 *
 * Once a second it reads the sensor and prints the result over USART2 (Tera Term).
 * If the sensor stops answering, it reports the cause and initialises the sensor
 * again on the next period, so a sensor that was power-cycled recovers by itself.
 * See sensor_task.c.
 */

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"

/* Creates the sensor task (static memory only: ADR-0001). Call it once from main(),
 * after clock_init_84mhz(), uart_init() and i2c_init(), and before
 * vTaskStartScheduler(). Nothing runs until the scheduler starts. */
void sensor_task_create( void );

/*
 * Copies the newest frame the sensor task has built (FRAME_SIZE bytes, see frame.h)
 * into pucDestination, and LEAVES it in the queue: a peek, not a receive, so it can be
 * read again and the next one simply replaces it. It never waits.
 *
 * Returns pdTRUE if a frame was copied, or pdFALSE if the sensor task has not produced
 * any yet (the first one comes about 20 ms after start-up): *pucDestination is then not
 * written.
 *
 * Call it from a TASK, after sensor_task_create(). The copy is made inside the
 * kernel's critical section, so the frame is always a whole one, either the previous or
 * the new, never a mix of the two.
 */
BaseType_t sensor_task_latest_frame( uint8_t * pucDestination );

#endif /* SENSOR_TASK_H */
