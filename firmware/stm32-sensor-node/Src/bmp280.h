/*
 * bmp280.h — Bosch BMP280 temperature and pressure sensor, on I2C1 (address 0x76).
 */

#ifndef BMP280_H
#define BMP280_H

#include <stdint.h>

typedef enum
{
    BMP280_OK = 0,
    BMP280_ERR_NACK,        /* I2C: the sensor did not answer (unplugged, or not listening) */
    BMP280_ERR_TIMEOUT,     /* I2C: a hardware flag did not arrive in time (stuck bus) */
    BMP280_ERR_BUS,         /* I2C: bus error or arbitration lost */
    BMP280_ERR_ID,          /* the chip ID is not 0x58: another chip, or the sensor is not in I2C mode */
    BMP280_ERR_NO_DATA      /* the data registers still hold the "no measurement yet" value (returned by bmp280_read) */
} bmp280_status_t;

/*
 * Resets the sensor and checks that a BMP280 answers: soft reset, a wait for it to
 * restart, then the chip ID is read and compared with 0x58, and finally the 24
 * factory calibration bytes are read and stored inside the module, and the sensor
 * is configured (oversampling x16, IIR filter 16) and started in normal mode.
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

/* One measurement, already corrected with the sensor's calibration constants. */
typedef struct
{
    int32_t lTemperatureCentiC;    /* hundredths of a degree Celsius: 2508 means 25.08 C */
    uint32_t ulPressurePa;         /* pascals: 100653 means 1006.53 hPa */
} bmp280_reading_t;

/*
 * Reads the latest measurement: one 6-byte burst of the data registers, the two
 * 20-bit raw values, and the integer compensation into a temperature and a
 * pressure. Call it only after bmp280_init() has returned BMP280_OK (checked by an
 * assertion), from the same task and under the same rules as bmp280_init().
 *
 * Returns BMP280_OK and fills *pxReading; or the I2C cause of a failure (NACK,
 * TIMEOUT, BUS); or BMP280_ERR_NO_DATA when the sensor has no measurement yet (the
 * first tens of milliseconds after init) or the result cannot be computed. On any
 * error *pxReading is not written.
 */
bmp280_status_t bmp280_read( bmp280_reading_t * pxReading );

/* A short text for a status, for the debug output ("OK", "NACK", ...). */
const char * bmp280_status_text( bmp280_status_t xStatus );

#endif /* BMP280_H */
