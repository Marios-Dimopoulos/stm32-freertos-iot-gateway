/*
 * frame.c — builds the 14-byte frame for the gateway (layout and meaning: frame.h,
 * decision: ADR-0010).
 *
 * WHY EVERY BYTE IS PLACED BY HAND AND NOT WITH A STRUCT. A C struct would be shorter,
 * but three things about it are decided by the compiler and not by us: the padding
 * between fields, the alignment, and the byte order in memory. The frame is a contract
 * with another machine (the Pi, and a kernel driver written later), so each byte goes
 * to a numbered position, least significant byte first. It is also the same layout on
 * any compiler and any chip.
 *
 * WHY A BIT-BY-BIT CRC AND NOT A TABLE. A table makes the CRC about eight times faster
 * and costs 512 bytes of flash. This one runs on 12 bytes once a second: 96 passes of a
 * tiny loop, a few microseconds, so the smaller code wins. (If frames ever became
 * large or frequent, that is the first thing to revisit.)
 */

#include "frame.h"

/* Positions of the fields (the table is in frame.h). */
#define FRAME_OFFSET_MARKER         ( 0U )
#define FRAME_OFFSET_STATUS         ( 1U )
#define FRAME_OFFSET_SEQUENCE       ( 2U )
#define FRAME_OFFSET_TEMPERATURE    ( 4U )
#define FRAME_OFFSET_PRESSURE       ( 8U )
#define FRAME_OFFSET_CRC            ( 12U )

#define FRAME_MARKER                ( 0xA5U )

/* The CRC covers everything before the CRC field itself. */
#define FRAME_CRC_COVERED_BYTES     ( FRAME_OFFSET_CRC )

_Static_assert( FRAME_OFFSET_CRC + 2U == FRAME_SIZE,
                "the field offsets and FRAME_SIZE do not describe the same frame" );

/*
 * CRC-16/CCITT-FALSE, one bit at a time.
 *
 * The idea: treat the message as one long binary number and keep the remainder of its
 * division by a fixed 16-bit "polynomial" (0x1021). The division is done with XOR in
 * place of subtraction, so it needs no carries. For each byte:
 *   1) XOR the byte into the TOP 8 bits of the running remainder;
 *   2) do 8 steps: if the top bit is 1, shift left by one and XOR the polynomial;
 *      otherwise only shift left. The bit that falls off the top is the one that was
 *      just "divided out".
 * Because every bit of every byte passes through the top of the register, the position
 * of each bit affects the result: swapped bytes, shifted data and flipped bits all give
 * a different CRC.
 *
 * The start value is 0xFFFF and not 0: with a start of 0, a message of all zeros would
 * have the CRC 0, so a dead slave reading 00 00 ... could look valid. (The marker byte
 * already prevents that; the start value is a second line of defence for free.)
 *
 * The casts to uint16_t are there because C promotes the operands to int before the
 * shift; the value is cut back to 16 bits (the bit shifted out at the top is dropped).
 */
static uint16_t prvCrc16( const uint8_t * pucData, uint32_t ulLength )
{
    uint16_t usCrc = 0xFFFFU;
    uint32_t ulByte;
    uint32_t ulBit;

    for( ulByte = 0U; ulByte < ulLength; ulByte++ )
    {
        usCrc = ( uint16_t ) ( usCrc ^ ( ( uint16_t ) pucData[ ulByte ] << 8 ) );

        for( ulBit = 0U; ulBit < 8U; ulBit++ )
        {
            if( ( usCrc & 0x8000U ) != 0U )
            {
                usCrc = ( uint16_t ) ( ( ( uint32_t ) usCrc << 1 ) ^ 0x1021U );
            }
            else
            {
                usCrc = ( uint16_t ) ( usCrc << 1 );
            }
        }
    }

    return usCrc;
}

void frame_build( uint8_t * pucFrame,
                  uint8_t ucStatus,
                  uint16_t usSequence,
                  int32_t lTemperatureCentiC,
                  uint32_t ulPressurePa )
{
    /* A negative temperature is stored as its 32-bit two's complement pattern: the
     * conversion to unsigned is defined (modulo 2^32) and the Pi reads the same four
     * bytes back as an int32. The shifts are done on the unsigned copy, because a right
     * shift of a negative signed number is implementation-defined. */
    const uint32_t ulTemperature = ( uint32_t ) lTemperatureCentiC;
    uint16_t usCrc;

    pucFrame[ FRAME_OFFSET_MARKER ] = FRAME_MARKER;
    pucFrame[ FRAME_OFFSET_STATUS ] = ucStatus;

    pucFrame[ FRAME_OFFSET_SEQUENCE ]     = ( uint8_t ) ( usSequence & 0xFFU );
    pucFrame[ FRAME_OFFSET_SEQUENCE + 1U ] = ( uint8_t ) ( usSequence >> 8 );

    pucFrame[ FRAME_OFFSET_TEMPERATURE ]      = ( uint8_t ) ( ulTemperature & 0xFFU );
    pucFrame[ FRAME_OFFSET_TEMPERATURE + 1U ] = ( uint8_t ) ( ( ulTemperature >> 8 ) & 0xFFU );
    pucFrame[ FRAME_OFFSET_TEMPERATURE + 2U ] = ( uint8_t ) ( ( ulTemperature >> 16 ) & 0xFFU );
    pucFrame[ FRAME_OFFSET_TEMPERATURE + 3U ] = ( uint8_t ) ( ( ulTemperature >> 24 ) & 0xFFU );

    pucFrame[ FRAME_OFFSET_PRESSURE ]      = ( uint8_t ) ( ulPressurePa & 0xFFU );
    pucFrame[ FRAME_OFFSET_PRESSURE + 1U ] = ( uint8_t ) ( ( ulPressurePa >> 8 ) & 0xFFU );
    pucFrame[ FRAME_OFFSET_PRESSURE + 2U ] = ( uint8_t ) ( ( ulPressurePa >> 16 ) & 0xFFU );
    pucFrame[ FRAME_OFFSET_PRESSURE + 3U ] = ( uint8_t ) ( ( ulPressurePa >> 24 ) & 0xFFU );

    usCrc = prvCrc16( pucFrame, FRAME_CRC_COVERED_BYTES );

    pucFrame[ FRAME_OFFSET_CRC ]      = ( uint8_t ) ( usCrc & 0xFFU );
    pucFrame[ FRAME_OFFSET_CRC + 1U ] = ( uint8_t ) ( usCrc >> 8 );
}
