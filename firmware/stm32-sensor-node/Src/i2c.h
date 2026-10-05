/*
 * i2c.h — I2C1 master driver: the BMP280 sensor on PB8 (SCL) / PB9 (SDA).
 *
 * WORK IN PROGRESS, built one step at a time (see i2c.c). So far i2c_init() only
 * enables the clocks, resets the peripheral and sets up the two pins; the
 * peripheral itself is not configured or enabled yet.
 */

#ifndef I2C_H
#define I2C_H

/* Call once from main(), before the scheduler starts and AFTER
 * SystemCoreClockUpdate(): the bus timing, added in a later step, is computed
 * from the real APB1 clock. */
void i2c_init( void );

#endif /* I2C_H */
