/*
 * bmp280.h — Bosch BMP280 temperature and pressure sensor, on I2C1 (address 0x76).
 *
 * WORK IN PROGRESS, built one step at a time (see bmp280.c). So far
 * bmp280_init() resets the sensor, checks its chip ID and reads its calibration
 * constants. The module never touches
 * I2C registers: it only uses i2c_read_reg() / i2c_write_reg().
 */

#ifndef BMP280_H
#define BMP280_H

typedef enum
{
    BMP280_OK = 0,
    BMP280_ERR_NACK,        /* I2C: the sensor did not answer (unplugged, or not listening) */
    BMP280_ERR_TIMEOUT,     /* I2C: a hardware flag did not arrive in time (stuck bus) */
    BMP280_ERR_BUS,         /* I2C: bus error or arbitration lost */
    BMP280_ERR_ID,          /* the chip ID is not 0x58: another chip, or the sensor is not in I2C mode */
    BMP280_ERR_NO_DATA      /* the data registers still hold the "no measurement yet" value (used from step 4) */
} bmp280_status_t;

/*
 * Resets the sensor and checks that a BMP280 answers: soft reset, a wait for it to
 * restart, then the chip ID is read and compared with 0x58, and finally the 24
 * factory calibration bytes are read and stored inside the module.
 *
 * It must be called from a TASK, after the scheduler has started, and only from
 * the task that owns the I2C bus: it sleeps with vTaskDelay() while the sensor
 * restarts (about 10 ms), and the I2C driver asserts that the scheduler is running.
 * i2c_init() must have been called before the scheduler was started.
 *
 * Returns BMP280_OK, the I2C cause of a failure (NACK, TIMEOUT, BUS), or
 * BMP280_ERR_ID. It never hangs: every I2C wait is bounded.
 */
bmp280_status_t bmp280_init( void );

/* A short text for a status, for the debug output ("OK", "NACK", ...). */
const char * bmp280_status_text( bmp280_status_t xStatus );

#endif /* BMP280_H */
