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
 *   1      status       0    the reading is valid
 *                       1..5 the BMP280 module's failure code (bmp280_status_t:
 *                            NACK, TIMEOUT, BUS, ERR_ID, ERR_NO_DATA); the
 *                            temperature and the pressure are 0 and mean nothing
 *                       0xFF no reading since reset (FRAME_STATUS_NO_READING)
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

/* Status byte value for "no reading since reset" (see the table above). */
#define FRAME_STATUS_NO_READING     ( 0xFFU )

/*
 * Writes one frame into pucFrame, which must have room for FRAME_SIZE bytes (the
 * caller sizes it; nothing is checked here). Pure function: no state, no RTOS call, no
 * allocation, so any task can call it. The sequence number is the CALLER's: it keeps
 * the counter and passes the value in, which keeps this module stateless.
 *
 * For a status other than 0 the caller passes 0 for the temperature and the pressure.
 */
void frame_build( uint8_t * pucFrame,
                  uint8_t ucStatus,
                  uint16_t usSequence,
                  int32_t lTemperatureCentiC,
                  uint32_t ulPressurePa );

#endif /* FRAME_H */
