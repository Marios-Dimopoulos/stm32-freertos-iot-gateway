/*
 * i2c.h — I2C1 master driver: the BMP280 sensor on PB8 (SCL) / PB9 (SDA).
 *
 * WORK IN PROGRESS, built one step at a time (see i2c.c). So far i2c_init()
 * enables the clocks, resets the peripheral, sets up the two pins, programs the
 * 100 kHz timing and enables I2C1, i2c_write_reg() writes registers and
 * i2c_read_reg() reads them.
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
 * Writes ulLength bytes into the registers of a device, starting at register
 * ucRegister (the device increments its register pointer after each byte):
 * START, address + write, the register address, the data bytes, STOP. Each byte
 * must be acknowledged. ulLength may be 0 (only the register address is sent).
 *
 * Returns I2C_OK if every byte was acknowledged, I2C_ERR_NACK if the device
 * refused the address or any byte, I2C_ERR_TIMEOUT / I2C_ERR_BUS on a hardware
 * problem. On every outcome the bus is left released (STOP sent, error flags
 * cleared). Blocking (the calling task busy-waits on hardware flags, each wait
 * limited to about 10 ms by the RTOS tick, so it never hangs), for ONE task only (the
 * bus owner), after the scheduler has started.
 */
i2c_status_t i2c_write_reg( uint8_t ucDevice, uint8_t ucRegister,
                            const uint8_t * pucData, uint32_t ulLength );

/*
 * Reads ulLength bytes from the registers of a device, starting at register
 * ucRegister: START, address + write, the register address, a repeated START,
 * address + read, then the device sends the bytes (the master acknowledges all
 * but the last, which it NACKs) and STOP.
 *
 * ulLength must be 1 or at least 3. A length of 2 is NOT supported (it needs a
 * third receive sequence that nothing in this project uses; to read two bytes,
 * read three and ignore one), and 0 makes no sense: both are programming errors
 * and stop at an assertion.
 *
 * Returns I2C_OK, I2C_ERR_NACK (the device did not acknowledge its address or the
 * register byte), I2C_ERR_TIMEOUT or I2C_ERR_BUS. On every outcome the bus is left
 * released. Same rules as the write: blocking (bounded), one task, after the
 * scheduler has started. Inside, a few register operations at the end of the
 * transfer run in a short critical section (a few microseconds), because they
 * must meet a deadline set by the bus clock.
 */
i2c_status_t i2c_read_reg( uint8_t ucDevice, uint8_t ucRegister,
                           uint8_t * pucBuffer, uint32_t ulLength );

#endif /* I2C_H */
