/*
 * bmp280.c — Bosch BMP280 temperature and pressure sensor (I2C1, address 0x76).
 *
 * Design decisions (agreed before writing; to be recorded in an ADR when the module
 * is complete):
 *   - NORMAL mode: the sensor measures continuously by itself and we read whenever
 *     we like (simpler than forced mode, and the IIR filter works best this way);
 *   - settings of the original reference code: temperature and pressure
 *     oversampling x16, IIR filter 16, 0.5 ms standby;
 *   - integer results (temperature in hundredths of a degree, pressure in Pa): no
 *     float printing is needed, and the later SPI frame can carry fixed-size integers;
 *   - the compensation maths are separate pure functions, so they can be checked
 *     without the sensor;
 *   - soft reset at init, so that every start-up begins from the same state;
 *   - the status codes keep the I2C cause (NACK / TIMEOUT / BUS) visible, and add
 *     "wrong chip ID" and "no data yet". A programming error (a NULL pointer, a read
 *     before init) is an assertion, as in i2c.c;
 *   - ONE task owns the sensor (and the bus): no mutex, no handle, the state lives
 *     in this file.
 *
 * Steps, in the order they are built and checked on the board:
 *
 *   1) soft reset, wait, chip ID                                        <- DONE
 *   2) read the 24 trimming bytes and unpack them                       <- DONE
 *   3) write config and ctrl_meas (no read-back)                        <- DONE
 *   4) the 6-byte burst and the two 20-bit raw values ("no data" check)  <- DONE
 *   5) temperature compensation                                          <- DONE
 *   6) pressure compensation                                             <- DONE
 *   7) status handling in the sensor task, and the formatter for the output
 */

#include "FreeRTOS.h"
#include "task.h"
#include "bmp280.h"
#include "i2c.h"

/* ==========================================================================
 * STEP 1) SOFT RESET AND CHIP ID
 * ==========================================================================
 *
 * The address. The BMP280 answers at 0x76 when its SDO pin is connected to GND, and
 * at 0x77 when SDO is connected to the supply. Its CSB pin must be HIGH: a falling
 * edge on CSB selects the SPI interface instead, and the sensor then stays in SPI
 * mode, ignoring I2C, until its power is cycled. Verified on the board: with the
 * sensor powered, moving CSB to GND gave NACK, and moving it back to 3.3 V still
 * gave NACK until the USB cable was unplugged and plugged in again. So CSB must be
 * wired firmly to 3.3 V (a loose wire is enough to lock the sensor out). Both pins are
 * wired on the breakout board. The caller of i2c_*_reg() passes the plain 7-bit
 * address; the driver adds the R/W bit.
 *
 * The registers used here:
 *   0xD0  id     read-only, always 0x58 for a BMP280. (The BME280 gives 0x60, and
 *                early samples of the BMP280 were 0x56 or 0x57.) It proves that the
 *                bus works AND that the chip is the one we wrote the maths for.
 *   0xE0  reset  write-only. Writing the magic value 0xB6 restarts the sensor as
 *                after a power-up: every register goes back to its reset value,
 *                the calibration data is copied again from the non-volatile
 *                memory, and the sensor goes to sleep mode. Any other value does
 *                nothing, so a stray write cannot reset it by accident.
 *
 * WHY RESET AT ALL. When only the MCU is reset (a debugger reset, the reset
 * button, a watchdog), the sensor stays powered and keeps running with whatever
 * settings the previous run gave it, and possibly in the middle of a measurement.
 * Starting every run with a reset makes each start-up identical, whatever happened
 * before, which is what makes a bug reproducible.
 *
 * WHY WAIT AFTER THE RESET. The sensor needs a short time to restart and to reload
 * its calibration data; the datasheet gives about 2 ms. Accessing it
 * earlier can be answered with a NACK, or read garbage. The wait is 10 ms, five
 * times the figure: a fixed delay is simpler than polling the sensor's status
 * register (0xF3, bit 0 "im_update"), and nothing here needs the 8 ms back.
 *
 * vTaskDelay() puts the calling task to sleep, so the CPU runs other tasks (or Idle)
 * during the wait, instead of spinning. With the 1 ms tick it lasts between 9 and 10
 * ms (the first tick is partial), still far above 2 ms. This is also why init has to
 * run inside a task, not in main(): before the scheduler starts, the tick does not
 * advance. The first I2C call below asserts that the scheduler is running, so a call
 * from main() stops there, at once, instead of misbehaving later.
 *
 * The order. If the reset write fails (nobody answers, or a bus problem), init
 * returns that error at once: the following steps cannot work either. The ID is read
 * after the wait, never before: right after a reset the sensor is not ready.
 *
 * Expected results on the board:
 *   sensor connected  OK
 *   sensor unplugged  NACK (from the reset write), and the task does not hang
 *   wrong ID          ERR_ID (can be provoked by temporarily changing
 *                     BMP280_CHIP_ID below to 0x59)
 */
#define BMP280_I2C_ADDRESS        ( 0x76U )
#define BMP280_REG_ID             ( 0xD0U )
#define BMP280_CHIP_ID            ( 0x58U )
#define BMP280_REG_RESET          ( 0xE0U )
#define BMP280_RESET_VALUE        ( 0xB6U )
#define BMP280_RESET_DELAY_MS     ( 10U )

/* Converts the I2C driver's status into this module's. The I2C causes keep their
 * names, so the debug output can tell a missing sensor (NACK) from a stuck bus
 * (TIMEOUT) or a wiring problem (BUS). */
static bmp280_status_t prvStatusFromI2cToBmp280( i2c_status_t xStatus )
{
    bmp280_status_t xResult;

    switch( xStatus )
    {
        case I2C_OK:
            xResult = BMP280_OK;
            break;

        case I2C_ERR_NACK:
            xResult = BMP280_ERR_NACK;
            break;

        case I2C_ERR_TIMEOUT:
            xResult = BMP280_ERR_TIMEOUT;
            break;

        default:
            xResult = BMP280_ERR_BUS;
            break;
    }

    return xResult;
}

/* ==========================================================================
 * STEP 2) THE TRIMMING BYTES
 * ==========================================================================
 *
 * What they are. The sensor's ADC does not output a temperature or a pressure: it
 * outputs a raw 20-bit number that depends on the physical properties of THIS chip.
 * At the factory Bosch measures every chip and stores 12 correction constants in its
 * non-volatile memory, 2 bytes each, 24 bytes from register 0x88. The compensation
 * formulas combine the raw number with these constants and give the true value.
 * They never change for a given chip, so they are read ONCE, at init:
 *
 *   0x88-0x89  dig_T1   unsigned     0x94-0x95  dig_P4   signed
 *   0x8A-0x8B  dig_T2   signed       0x96-0x97  dig_P5   signed
 *   0x8C-0x8D  dig_T3   signed       0x98-0x99  dig_P6   signed
 *   0x8E-0x8F  dig_P1   unsigned     0x9A-0x9B  dig_P7   signed
 *   0x90-0x91  dig_P2   signed       0x9C-0x9D  dig_P8   signed
 *   0x92-0x93  dig_P3   signed       0x9E-0x9F  dig_P9   signed
 *
 * Each constant is stored LOW byte first (the low byte at the lower address), so
 * dig_T2 = ( byte[3] << 8 ) | byte[2]. This is the sensor's byte order; it has
 * nothing to do with the MCU's.
 *
 * THE TYPES MATTER. dig_T1 and dig_P1 are UNSIGNED 16-bit, all the others SIGNED
 * 16-bit. Treating dig_T1 as signed (a value above 32767 would turn negative), or
 * dig_T2 as unsigned, gives wrong temperatures with no error of any kind. The struct
 * below uses the exact types of the datasheet.
 *
 * HOW THEY ARE UNPACKED. Field by field, with explicit shifts, through two tiny
 * helpers. The alternative would be to read the 24 bytes straight into the struct:
 * it would work here (the Cortex-M4 is little-endian and the 12 fields are 16-bit,
 * in the same order as the registers, so there is no padding), but it relies on
 * that layout without saying so. The explicit version can be compared with the table
 * above line by line.
 *
 * The cast of an unsigned 16-bit value to a signed one (prvSigned16) is
 * implementation-defined in C when the value does not fit (above 32767). Arm
 * Compiler defines it as the two's-complement wrap, which is what we need: 0xFC18
 * becomes -1000.
 *
 * READ FIRST INTO A LOCAL BUFFER, then unpack only if the read succeeded. A failed
 * read never leaves a half-written calibration behind, and running init again after
 * a failure simply tries again. The buffer lives on the task's stack (24 bytes).
 *
 * No sanity check of the values (all 0x00, all 0xFF): there is no status code for it
 * and nothing needs one yet. The dangerous case, dig_P1 = 0, makes the pressure
 * formula divide by zero; that guard belongs to the maths (step 6).
 *
 * How to check it on the board: a breakpoint at the end of bmp280_init() (inside
 * this file, because the variable is static), then watch xCalib and ucTrimming.
 *   - dig_T1 = ucTrimming[1] * 256 + ucTrimming[0], and so on for the others;
 *   - typical magnitudes (Bosch's example set, from memory; every chip differs):
 *     T1 about 27000, T2 about 26000, T3 a small negative number, P1 about 36000,
 *     P2 about -10000, P6 a small negative number, P9 about 6000;
 *   - all zeros or all -1 would mean the read did not do what we think; a negative
 *     T1 or P1 would mean the unsigned type was lost.
 */
#define BMP280_REG_TRIMMING        ( 0x88U )
#define BMP280_TRIMMING_BYTES      ( 24U )

typedef struct
{
    uint16_t usDigT1;
    int16_t sDigT2;
    int16_t sDigT3;
    uint16_t usDigP1;
    int16_t sDigP2;
    int16_t sDigP3;
    int16_t sDigP4;
    int16_t sDigP5;
    int16_t sDigP6;
    int16_t sDigP7;
    int16_t sDigP8;
    int16_t sDigP9;
} bmp280_calib_t;

static bmp280_calib_t xCalib;

/* pdTRUE only after a complete, successful init: bmp280_read() asserts it, so a read
 * before init (or after a failed one) stops at once instead of computing with an
 * empty calibration. */
static BaseType_t xInitialised = pdFALSE;

/* Two bytes in the sensor's order (low byte first) into a 16-bit number. */
static uint16_t prvUnsigned16( const uint8_t * pucBytes )
{
    return ( uint16_t ) ( ( ( uint16_t ) pucBytes[ 1 ] << 8 ) | pucBytes[ 0 ] );
}

/* The same, for the constants that are signed. */
static int16_t prvSigned16( const uint8_t * pucBytes )
{
    return ( int16_t ) prvUnsigned16( pucBytes );
}

static void prvUnpackTrimming( const uint8_t * pucBytes )
{
    xCalib.usDigT1 = prvUnsigned16( &pucBytes[ 0 ] );
    xCalib.sDigT2 = prvSigned16( &pucBytes[ 2 ] );
    xCalib.sDigT3 = prvSigned16( &pucBytes[ 4 ] );
    xCalib.usDigP1 = prvUnsigned16( &pucBytes[ 6 ] );
    xCalib.sDigP2 = prvSigned16( &pucBytes[ 8 ] );
    xCalib.sDigP3 = prvSigned16( &pucBytes[ 10 ] );
    xCalib.sDigP4 = prvSigned16( &pucBytes[ 12 ] );
    xCalib.sDigP5 = prvSigned16( &pucBytes[ 14 ] );
    xCalib.sDigP6 = prvSigned16( &pucBytes[ 16 ] );
    xCalib.sDigP7 = prvSigned16( &pucBytes[ 18 ] );
    xCalib.sDigP8 = prvSigned16( &pucBytes[ 20 ] );
    xCalib.sDigP9 = prvSigned16( &pucBytes[ 22 ] );
}

/* ==========================================================================
 * STEP 3) THE TWO CONFIGURATION REGISTERS
 * ==========================================================================
 *
 * Two one-byte read/write registers set how the sensor measures. Both reset to 0, which means "sleep mode".
 *
 *   ctrl_meas, 0xF4          config, 0xF5
 *     bits 7:5  osrs_t         bits 7:5  t_sb      standby time between two
 *                              measurements in normal mode
 *                                         000 = 0.5 ms, 001 = 62.5 ms, ... up to
 *                                         several seconds
 *     bits 4:2  osrs_p         bits 4:2  filter    IIR filter coefficient
 *                                         000 = off, 001 = 2, 010 = 4, 011 = 8,
 *                                         1xx = 16
 *     bits 1:0  mode           bit  0    spi3w_en  SPI 3-wire mode: irrelevant for
 *                                         I2C, left at 0
 *   osrs codes: 000 = skipped, 001 = x1, 010 = x2, 011 = x4, 100 = x8, 101 = x16.
 *   mode codes: 00 = sleep, 01 and 10 = forced, 11 = NORMAL.
 *
 * What the settings mean:
 *   - OVERSAMPLING: the sensor takes several ADC samples per measurement and
 *     averages them. More samples mean less noise and finer resolution, but a longer
 *     measurement. x16 for both is the maximum (the settings of the reference code).
 *   - NORMAL MODE: the sensor measures continuously by itself, and sleeps t_sb
 *     between two measurements; we read the result whenever we like. (In forced mode
 *     we would trigger every measurement ourselves.)
 *   - IIR FILTER: every output is mixed with the earlier ones, so short disturbances
 *     (a slammed door changes the air pressure briefly) are smoothed away, at the cost
 *     of a slower response to a real change. It works on the sensor's internal samples,
 *     not on our once-a-second reads.
 *
 * THE ORDER: config FIRST, ctrl_meas SECOND. Writing mode = normal in ctrl_meas
 * starts the measurements, and as I remember the datasheet, writes to config can be
 * ignored while the sensor is in normal mode. The two registers are neighbours, but a
 * single two-byte write from 0xF4 would write ctrl_meas first, which is the wrong
 * order, so they are two separate one-byte writes.
 *
 * The values are built from named fields instead of two bare binary constants, so
 * that every bit pair can be read as a setting:
 *   ctrl_meas = ( 5 << 5 ) | ( 5 << 2 ) | 3  = 0xB7
 *   config    = ( 0 << 5 ) | ( 4 << 2 )      = 0x10
 *
 * NO READ-BACK. The two registers are not read again to check the writes: a wrong
 * or ignored write would show up as wrong or missing measurements, and that is
 * what step 4 and the following ones check. A failed write is still reported: every
 * write returns its I2C status (a NACK, a timeout).
 *
 * Once ctrl_meas is written the sensor is measuring: from here on the data registers
 * leave their reset value (80 00 00 80 00 00) after the first measurement, which takes
 * tens of milliseconds at x16. Reading them is step 4. Because init starts with a soft
 * reset, a run that finds the sensor already measuring from a previous run still
 * begins from the same state.
 */
#define BMP280_REG_CTRL_MEAS          ( 0xF4U )
#define BMP280_REG_CONFIG             ( 0xF5U )

#define BMP280_OSRS_X16               ( 5U )
#define BMP280_MODE_NORMAL            ( 3U )
#define BMP280_FILTER_16              ( 4U )
#define BMP280_STANDBY_0_5_MS         ( 0U )

#define BMP280_CTRL_OSRS_T_POS        ( 5U )
#define BMP280_CTRL_OSRS_P_POS        ( 2U )
#define BMP280_CTRL_MODE_POS          ( 0U )
#define BMP280_CONFIG_T_SB_POS        ( 5U )
#define BMP280_CONFIG_FILTER_POS      ( 2U )

#define BMP280_CTRL_MEAS_VALUE        ( ( BMP280_OSRS_X16 << BMP280_CTRL_OSRS_T_POS ) | \
                                        ( BMP280_OSRS_X16 << BMP280_CTRL_OSRS_P_POS ) | \
                                        ( BMP280_MODE_NORMAL << BMP280_CTRL_MODE_POS ) )
#define BMP280_CONFIG_VALUE           ( ( BMP280_STANDBY_0_5_MS << BMP280_CONFIG_T_SB_POS ) | \
                                        ( BMP280_FILTER_16 << BMP280_CONFIG_FILTER_POS ) )

bmp280_status_t bmp280_init( void )
{
    /* The pointer given to i2c_write_reg() is only used during the call (the driver
     * is blocking and has finished with it when it returns), so a local variable is
     * enough. */
    const uint8_t ucResetValue = BMP280_RESET_VALUE;
    const uint8_t ucConfigValue = BMP280_CONFIG_VALUE;
    const uint8_t ucCtrlMeasValue = BMP280_CTRL_MEAS_VALUE;
    uint8_t ucId = 0U;
    uint8_t ucTrimming[ BMP280_TRIMMING_BYTES ];
    bmp280_status_t xStatus;

    xInitialised = pdFALSE;

    xStatus = prvStatusFromI2cToBmp280( i2c_write_reg( BMP280_I2C_ADDRESS, BMP280_REG_RESET, &ucResetValue, 1U ) );

    if( xStatus != BMP280_OK )
    {
        return xStatus;
    }

    vTaskDelay( pdMS_TO_TICKS( BMP280_RESET_DELAY_MS ) );

    xStatus = prvStatusFromI2cToBmp280( i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_ID, &ucId, 1U ) );

    if( ( xStatus == BMP280_OK ) && ( ucId != BMP280_CHIP_ID ) )
    {
        xStatus = BMP280_ERR_ID;
    }

    /* Step 2: the calibration constants, read into the local buffer first. */
    if( xStatus == BMP280_OK )
    {
        xStatus = prvStatusFromI2cToBmp280( i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_TRIMMING, ucTrimming, BMP280_TRIMMING_BYTES ) );
    }

    if( xStatus == BMP280_OK )
    {
        prvUnpackTrimming( ucTrimming );
    }

    /* Step 3: config first, then ctrl_meas, which starts the measurements. */
    if( xStatus == BMP280_OK )
    {
        xStatus = prvStatusFromI2cToBmp280( i2c_write_reg( BMP280_I2C_ADDRESS, BMP280_REG_CONFIG, &ucConfigValue, 1U ) );
    }

    if( xStatus == BMP280_OK )
    {
        xStatus = prvStatusFromI2cToBmp280( i2c_write_reg( BMP280_I2C_ADDRESS, BMP280_REG_CTRL_MEAS, &ucCtrlMeasValue, 1U ) );
    }

    if( xStatus == BMP280_OK )
    {
        xInitialised = pdTRUE;
    }

    return xStatus;
}

/* ==========================================================================
 * STEPS 4, 5 AND 6) THE BURST, THE RAW VALUES AND THE COMPENSATION
 * ==========================================================================
 *
 * THE BURST. The six data registers sit next to each other, from 0xF7:
 *
 *   0xF7 press_msb  0xF8 press_lsb  0xF9 press_xlsb | 0xFA temp_msb  0xFB temp_lsb  0xFC temp_xlsb
 *
 * They are read in ONE transaction of 6 bytes. As I remember the datasheet, the
 * sensor freezes a copy of the result when the burst starts, so a measurement that
 * finishes in the middle of the read cannot give a new pressure with an old
 * temperature. Six separate reads would not have that guarantee.
 *
 * THE RAW VALUES. Each is 20 bits spread over three bytes; the lowest byte (xlsb)
 * only uses its UPPER 4 bits, hence the shift right by 4:
 *     adc = ( msb << 12 ) | ( lsb << 4 ) | ( xlsb >> 4 )
 * Pressure comes first in the burst, temperature second, but the temperature is
 * computed first: the pressure formula needs t_fine, which the temperature formula
 * produces. These raw numbers are only ADC counts that depend on this particular
 * chip; they are not a temperature or a pressure.
 *
 * NO DATA YET. Before the first measurement is finished, the data registers hold
 * 0x80000 (the "skipped" value; seen as 80 00 00 80 00 00 after a reset). With both
 * measurements enabled, either raw value equal to 0x80000 means "no measurement
 * yet": BMP280_ERR_NO_DATA. A genuine reading can in theory be exactly 0x80000 (one
 * count in about a million) and would be mistaken for it; that is accepted. At x16
 * the first measurement takes tens of milliseconds after init.
 *
 * THE COMPENSATION is Bosch's integer algorithm (the 32-bit one for the temperature,
 * the 64-bit one for the pressure), as given in the datasheet and in the reference
 * code of the course. It is not derived here: it is the vendor's characterisation of
 * the sensor. What matters in this file is the engineering around it:
 *
 *   - UNITS. The temperature comes out in hundredths of a degree (2508 = 25.08 C).
 *     The pressure comes out in Pa as a fixed-point number with 8 fraction bits
 *     (Q24.8: the real value is the result / 256); it is rounded to whole pascals,
 *     which is far finer than the sensor's accuracy of about +-100 Pa.
 *   - WIDTHS. The temperature arithmetic fits 32 bits (the subtraction of dig_T1
 *     brings the numbers close to zero before they are multiplied). The pressure
 *     formula multiplies several 16-bit constants with t_fine squared and shifts by
 *     up to 47 bits, so it needs 64-bit variables. The 64-bit division becomes a
 *     library call on the Cortex-M4 (tens of microseconds, irrelevant at 1 Hz).
 *   - t_fine is the temperature in an internal fine scale, 5120 counts per degree;
 *     the temperature in hundredths of a degree is t_fine * 5 / 256, rounded.
 *   - SHIFTS OF NEGATIVE NUMBERS. A right shift of a negative number is
 *     implementation-defined in C; Arm Compiler does an arithmetic shift, which is
 *     what the formulas assume. A LEFT shift of a negative number is undefined in C,
 *     and several terms can be negative (the signed constants), so the left shifts
 *     of such terms are written as multiplications by the same power of two. The
 *     result is identical.
 *   - DIVISION BY ZERO. The pressure formula divides by a value computed from
 *     dig_P1. If it is zero (only possible with corrupt calibration data) the result
 *     cannot be computed. There is no status code for that; it is reported as
 *     BMP280_ERR_NO_DATA ("no valid result"), the closest of the agreed codes. If it
 *     ever happens, a dedicated code would be justified.
 *
 * The two functions are PURE: they use only their arguments (the calibration is
 * passed in), so they can be tested without the sensor.
 *
 * How the formulas were checked: I transcribed them into Python (with C's integer
 * semantics) and fed them the example set that I remember from Bosch's datasheet
 * (dig_T1 27504, dig_T2 26435, dig_T3 -1000, dig_P1 36477, dig_P2 -10685, dig_P3 3024,
 * dig_P4 2855, dig_P5 140, dig_P6 -7, dig_P7 15500, dig_P8 -14600, dig_P9 6000,
 * adc_T 519888, adc_P 415148). Result: 2508 (25.08 C) and 100653.25 Pa, matching the
 * datasheet's 25.08 C and 100653.27 Pa to within 0.02 Pa; the example numbers are from
 * memory, and the C code itself has not been run on a PC. On the board: the
 * temperature must be a plausible room temperature, the pressure about 100 kPa at
 * low altitude, and a finger on the sensor must raise the temperature.
 */
#define BMP280_REG_DATA            ( 0xF7U )
#define BMP280_DATA_BYTES          ( 6U )
#define BMP280_RAW_SKIPPED         ( 0x80000 )

/* Returns the temperature in hundredths of a degree, and t_fine through the pointer
 * (the pressure formula needs it). */
static int32_t prvCompensateTemperature( int32_t lAdcT, const bmp280_calib_t * pxCalib, int32_t * plTFine )
{
    int32_t lVar1;
    int32_t lVar2;

    lVar1 = ( ( ( ( lAdcT >> 3 ) - ( ( int32_t ) pxCalib->usDigT1 << 1 ) ) ) *
              ( ( int32_t ) pxCalib->sDigT2 ) ) >> 11;
    lVar2 = ( ( ( ( ( lAdcT >> 4 ) - ( ( int32_t ) pxCalib->usDigT1 ) ) *
                  ( ( lAdcT >> 4 ) - ( ( int32_t ) pxCalib->usDigT1 ) ) ) >> 12 ) *
              ( ( int32_t ) pxCalib->sDigT3 ) ) >> 14;

    *plTFine = lVar1 + lVar2;

    return ( ( *plTFine * 5 ) + 128 ) >> 8;
}

/* Returns pdTRUE and the pressure in whole pascals through the pointer, or pdFALSE
 * when the result cannot be computed (division by zero). */
static BaseType_t prvCompensatePressure( int32_t lAdcP, int32_t lTFine, const bmp280_calib_t * pxCalib, uint32_t * pulPascal )
{
    int64_t llVar1;
    int64_t llVar2;
    int64_t llPressure;

    llVar1 = ( int64_t ) lTFine - 128000;
    llVar2 = llVar1 * llVar1 * ( int64_t ) pxCalib->sDigP6;
    llVar2 = llVar2 + ( llVar1 * ( int64_t ) pxCalib->sDigP5 * 131072 );               /* << 17 */
    llVar2 = llVar2 + ( ( int64_t ) pxCalib->sDigP4 * 34359738368 );                   /* << 35 */
    llVar1 = ( ( llVar1 * llVar1 * ( int64_t ) pxCalib->sDigP3 ) >> 8 ) +
             ( llVar1 * ( int64_t ) pxCalib->sDigP2 * 4096 );                          /* << 12 */
    llVar1 = ( ( ( ( ( int64_t ) 1 ) << 47 ) + llVar1 ) * ( int64_t ) pxCalib->usDigP1 ) >> 33;

    if( llVar1 == 0 )
    {
        return pdFALSE;
    }

    llPressure = 1048576 - lAdcP;
    llPressure = ( ( ( llPressure << 31 ) - llVar2 ) * 3125 ) / llVar1;
    llVar1 = ( ( int64_t ) pxCalib->sDigP9 * ( llPressure >> 13 ) * ( llPressure >> 13 ) ) >> 25;
    llVar2 = ( ( int64_t ) pxCalib->sDigP8 * llPressure ) >> 19;
    llPressure = ( ( llPressure + llVar1 + llVar2 ) >> 8 ) + ( ( int64_t ) pxCalib->sDigP7 * 16 );   /* << 4 */

    /* llPressure is in Q24.8 pascals: round to whole pascals. */
    *pulPascal = ( uint32_t ) ( ( llPressure + 128 ) >> 8 );

    return pdTRUE;
}

bmp280_status_t bmp280_read( bmp280_reading_t * pxReading )
{
    uint8_t ucData[ BMP280_DATA_BYTES ];
    int32_t lAdcP;
    int32_t lAdcT;
    int32_t lTFine;
    int32_t lTemperature;
    uint32_t ulPressure;
    bmp280_status_t xStatus;

    configASSERT( pxReading != NULL );
    configASSERT( xInitialised == pdTRUE );

    xStatus = prvStatusFromI2cToBmp280( i2c_read_reg( BMP280_I2C_ADDRESS, BMP280_REG_DATA, ucData, BMP280_DATA_BYTES ) );

    if( xStatus != BMP280_OK )
    {
        return xStatus;
    }

    lAdcP = ( int32_t ) ( ( ( uint32_t ) ucData[ 0 ] << 12 ) | ( ( uint32_t ) ucData[ 1 ] << 4 ) | ( ( uint32_t ) ucData[ 2 ] >> 4 ) );
    lAdcT = ( int32_t ) ( ( ( uint32_t ) ucData[ 3 ] << 12 ) | ( ( uint32_t ) ucData[ 4 ] << 4 ) | ( ( uint32_t ) ucData[ 5 ] >> 4 ) );

    if( ( lAdcP == BMP280_RAW_SKIPPED ) || ( lAdcT == BMP280_RAW_SKIPPED ) )
    {
        return BMP280_ERR_NO_DATA;
    }

    lTemperature = prvCompensateTemperature( lAdcT, &xCalib, &lTFine );

    if( prvCompensatePressure( lAdcP, lTFine, &xCalib, &ulPressure ) == pdFALSE )
    {
        return BMP280_ERR_NO_DATA;
    }

    pxReading->lTemperatureCentiC = lTemperature;
    pxReading->ulPressurePa = ulPressure;

    return BMP280_OK;
}

const char * bmp280_status_text( bmp280_status_t xStatus )
{
    const char * pcText;

    switch( xStatus )
    {
        case BMP280_OK:
            pcText = "OK";
            break;

        case BMP280_ERR_NACK:
            pcText = "NACK";
            break;

        case BMP280_ERR_TIMEOUT:
            pcText = "TIMEOUT";
            break;

        case BMP280_ERR_BUS:
            pcText = "BUS ERROR";
            break;

        case BMP280_ERR_ID:
            pcText = "WRONG CHIP ID";
            break;

        case BMP280_ERR_NO_DATA:
            pcText = "NO DATA YET";
            break;

        default:
            pcText = "?";
            break;
    }

    return pcText;
}
