/*
 * format.c — tiny text formatting for the debug output (see format.h).
 *
 * WHY THIS EXISTS. The C library's printf is large, it can pull in a heap (the map
 * file of this project has no malloc, ADR-0001), and it is not safe to share between
 * tasks without care. Everything the debug output needs is a few decimal numbers, so
 * three small functions do the job, with nothing hidden: no format strings, no
 * allocation, no static buffers, no state. They are pure functions of their
 * arguments, so any task can call them at any time.
 *
 * Division by 10 and by 100 is fine on this chip: the Cortex-M4 has a hardware
 * divider, and the numbers are printed once a second.
 */

#include "format.h"

/* The longest unsigned 32-bit number, 4294967295, has 10 digits. */
#define FORMAT_MAX_DIGITS    ( 10U )

/* "-", up to 8 digits of whole units (2147483648 / 100 = 21474836), ".", 2 decimals. */
#define FORMAT_MAX_FIXED2    ( 12U )

/*
 * Writes the decimal digits of a number into pcDestination and returns how many.
 *
 * The digits come out of the number from the LAST one to the first (value % 10 is the
 * units digit, then the number is divided by 10, and so on), which is the opposite of
 * the order they are written in. So they are collected in a small local array, and
 * then copied in reverse. do/while, not while: the number 0 must still give one digit.
 */
static uint32_t prvWriteDigits( char * pcDestination, uint32_t ulValue )
{
    char acReversed[ FORMAT_MAX_DIGITS ];
    uint32_t ulCount = 0U;
    uint32_t ulIndex;

    do
    {
        acReversed[ ulCount ] = ( char ) ( '0' + ( ulValue % 10U ) );
        ulCount++;
        ulValue = ulValue / 10U;
    } while( ulValue != 0U );

    for( ulIndex = 0U; ulIndex < ulCount; ulIndex++ )
    {
        pcDestination[ ulIndex ] = acReversed[ ulCount - 1U - ulIndex ];
    }

    return ulCount;
}

/* The one place where the "fits or nothing" rule lives: copies ulLength characters
 * if they fit in the capacity, and returns the length; otherwise writes nothing and
 * returns 0. */
static uint32_t prvCopyIfFits( char * pcBuffer, uint32_t ulCapacity, const char * pcSource, uint32_t ulLength )
{
    uint32_t ulIndex;

    if( ulLength > ulCapacity )
    {
        return 0U;
    }

    for( ulIndex = 0U; ulIndex < ulLength; ulIndex++ )
    {
        pcBuffer[ ulIndex ] = pcSource[ ulIndex ];
    }

    return ulLength;
}

uint32_t format_text( char * pcBuffer, uint32_t ulCapacity, const char * pcText )
{
    uint32_t ulLength = 0U;

    while( pcText[ ulLength ] != '\0' )
    {
        ulLength++;
    }

    return prvCopyIfFits( pcBuffer, ulCapacity, pcText, ulLength );
}

uint32_t format_u32( char * pcBuffer, uint32_t ulCapacity, uint32_t ulValue )
{
    char acText[ FORMAT_MAX_DIGITS ];
    const uint32_t ulLength = prvWriteDigits( acText, ulValue );

    return prvCopyIfFits( pcBuffer, ulCapacity, acText, ulLength );
}

/*
 * The number is split into whole units and two decimals with / 100 and % 100, and
 * each part is printed separately: 2364 -> 23 and 64 -> "23.64". Three details:
 *
 *   - THE SIGN IS HANDLED APART from the digits. If the minus sign came from printing
 *     the whole part as a signed number, then -5 (that is -0.05) would print "0.05",
 *     because the whole part is 0 and has no sign. Here the sign is written first,
 *     and the rest is printed from the absolute value.
 *   - THE ABSOLUTE VALUE is computed in unsigned arithmetic: 0 - (uint32_t) value.
 *     Negating a signed int32_t at its smallest value (-2147483648) would overflow,
 *     which is undefined in C; in unsigned arithmetic the result is well defined and
 *     right (2147483648).
 *   - THE DECIMALS are always two digits, with a leading zero: 7 -> "07", so that
 *     1019.07 is not printed as 1019.7.
 */
uint32_t format_fixed2( char * pcBuffer, uint32_t ulCapacity, int32_t lHundredths )
{
    char acText[ FORMAT_MAX_FIXED2 ];
    uint32_t ulLength = 0U;
    uint32_t ulMagnitude;
    uint32_t ulDecimals;

    if( lHundredths < 0 )
    {
        acText[ ulLength ] = '-';
        ulLength++;
        ulMagnitude = 0U - ( uint32_t ) lHundredths;
    }
    else
    {
        ulMagnitude = ( uint32_t ) lHundredths;
    }

    ulDecimals = ulMagnitude % 100U;

    ulLength += prvWriteDigits( &acText[ ulLength ], ulMagnitude / 100U );

    acText[ ulLength ] = '.';
    ulLength++;
    acText[ ulLength ] = ( char ) ( '0' + ( ulDecimals / 10U ) );
    ulLength++;
    acText[ ulLength ] = ( char ) ( '0' + ( ulDecimals % 10U ) );
    ulLength++;

    return prvCopyIfFits( pcBuffer, ulCapacity, acText, ulLength );
}
