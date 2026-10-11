/*
 * frame.h — the 14-byte frame that carries one reading to the gateway (ADR-0010).
 *
 * This file is the CONTRACT between the STM32 and the Raspberry Pi: the test script
 * and the Phase 4 kernel driver must read exactly this layout. All multi-byte fields
 * are little-endian (least significant byte first).
 *
 *   byte   field        meaning
 *   ----   -----        -------
 *   0      marker       always 0xA5. A dead or unplugged slave reads as 0xFF or 0x00
 *                       and can never look like a frame; a read that started in the
 *                       middle of a frame shows up here too.
 *   1      status       a frame_status_t value (below). 0 means the temperature and
 *                       the pressure are valid; any other value means they are 0
 *                       and mean nothing.
 *   2-3    sequence     uint16, goes up by one for every frame built, wraps to 0.
 *                       A repeated number means "nothing new"; a jump means frames
 *                       were skipped.
 *   4-7    temperature  int32, hundredths of a degree Celsius (2364 is 23.64 C)
 *   8-11   pressure     uint32, pascals (101969 is 1019.69 hPa)
 *   12-13  CRC          CRC-16/CCITT-FALSE of bytes 0..11 (polynomial 0x1021, start
 *                       value 0xFFFF, bits not reflected, no final XOR), low byte
 *                       first.
 *
 * Test vector: the body A5 00 01 00 3C 09 00 00 51 8E 01 00 (status OK, sequence 1,
 * 23.64 C, 101969 Pa) has the CRC 0x4D5E, so the frame ends with 5E 4D.
 */

#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

#define FRAME_SIZE                  ( 14U )

/*
 * What the status byte can say. These numbers are part of the wire contract with the
 * gateway, so each one is written out and must never change meaning (new codes go at
 * the end). They are the frame's own vocabulary: they do not depend on the order of
 * any other module's enum (the sensor task translates the BMP280 module's status into
 * one of these).
 */
typedef enum
{
    FRAME_STATUS_OK = 0,                /* temperature and pressure are valid */
    FRAME_STATUS_NO_READING = 1,        /* the node has no reading yet: just started (the
                                         * placeholder frame) or just initialised the sensor */
    FRAME_STATUS_SENSOR_NACK = 2,       /* the sensor did not answer (unplugged, or not listening) */
    FRAME_STATUS_SENSOR_TIMEOUT = 3,    /* a hardware flag did not arrive in time (stuck bus) */
    FRAME_STATUS_SENSOR_BUS = 4,        /* bus error or arbitration lost */
    FRAME_STATUS_SENSOR_ID = 5,         /* the chip ID is wrong, or the sensor is not in I2C mode */
    FRAME_STATUS_SENSOR_NO_DATA = 6     /* the sensor answered but holds no measurement yet */
} frame_status_t;

/*
 * Writes one frame into pucFrame, which must have room for FRAME_SIZE bytes (the
 * caller sizes it; nothing is checked here). Pure function: no state, no RTOS call, no
 * allocation, so any task can call it. The sequence number is the CALLER's: it keeps
 * the counter and passes the value in, which keeps this module stateless.
 *
 * For a status other than FRAME_STATUS_OK the caller passes 0 for the temperature and
 * the pressure.
 */
void frame_build( uint8_t * pucFrame,
                  frame_status_t xStatus,
                  uint16_t usSequence,
                  int32_t lTemperatureCentiC,
                  uint32_t ulPressurePa );

#endif /* FRAME_H */
