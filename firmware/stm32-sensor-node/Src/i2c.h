/*
 * i2c.h — I2C1 master driver: the BMP280 sensor on PB8 (SCL) / PB9 (SDA).
 *
 * WORK IN PROGRESS, built one step at a time (see i2c.c). So far i2c_init()
 * enables the clocks, resets the peripheral, sets up the two pins, programs the
 * 100 kHz timing and enables I2C1, and i2c_probe() can check whether a device
 * answers an address.
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

typedef enum
{
    I2C_OK = 0,
    I2C_ERR_NACK,       /* nobody acknowledged: no such device, or it is not answering */
    I2C_ERR_TIMEOUT,    /* a hardware flag did not arrive in time (stuck bus or peripheral) */
    I2C_ERR_BUS         /* bus error or arbitration lost */
} i2c_status_t;

/* Call once from main(), before the scheduler starts and AFTER
 * SystemCoreClockUpdate(): the bus timing is computed from the real APB1 clock. */
void i2c_init( void );

/*
 * Checks whether a device answers at the given 7-bit address (for the BMP280:
 * 0x76, or 0x77 with SDO high): START, the address with the write bit, and then
 * STOP. Nothing is transferred. Returns I2C_OK if the device acknowledged,
 * I2C_ERR_NACK if nobody did (also the result with the sensor unplugged),
 * I2C_ERR_TIMEOUT or I2C_ERR_BUS on a hardware problem.
 *
 * BLOCKING (temporary, as the project rules ask for any polling loop): the calling
 * task busy-waits on hardware flags, each wait limited to about 10 ms by the RTOS
 * tick. Call it from ONE task only (the bus owner), after the scheduler has
 * started. It never hangs.
 */
i2c_status_t i2c_probe( uint8_t ucDevice );

#endif /* I2C_H */
