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

/* Creates the sensor task (static memory only: ADR-0001). Call it once from main(),
 * after clock_init_84mhz(), uart_init() and i2c_init(), and before
 * vTaskStartScheduler(). Nothing runs until the scheduler starts. */
void sensor_task_create( void );

#endif /* SENSOR_TASK_H */
