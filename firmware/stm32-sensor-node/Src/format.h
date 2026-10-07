/*
 * format.h — tiny text formatting for the debug output: no printf, no malloc.
 *
 * A line for the terminal is built in a buffer of the caller, piece by piece, and then
 * sent with ONE uart_write() call, so it is a single message (the UART's "whole
 * message or nothing" rule then applies to the whole line). Each function writes at
 * the start of the buffer it is given and returns how many characters it wrote:
 *
 *     pos  = format_text( line,       size,       "T=" );
 *     pos += format_fixed2( line + pos, size - pos, lTemperature );
 *     pos += format_text( line + pos, size - pos, " C\r\n" );
 *     uart_write( ( const uint8_t * ) line, pos );
 *
 * The text is NOT terminated with a zero byte: uart_write() takes a length, and a
 * terminator would only waste a byte.
 *
 * If the text does not fit in the capacity given, NOTHING is written and 0 is
 * returned, never a cut number (the same policy as the UART). A caller that sizes its
 * buffer for the worst case, as the sensor task does, never sees this.
 */

#ifndef FORMAT_H
#define FORMAT_H

#include <stdint.h>

/* Copies a zero-terminated string (the terminator is not copied). */
uint32_t format_text( char * pcBuffer, uint32_t ulCapacity, const char * pcText );

/* An unsigned number in decimal: 0 gives "0", 4294967295 gives "4294967295"
 * (at most 10 characters). */
uint32_t format_u32( char * pcBuffer, uint32_t ulCapacity, uint32_t ulValue );

/*
 * A signed number that stands for hundredths, with two decimals: 2364 gives "23.64",
 * -5 gives "-0.05", 101969 gives "1019.69" (at most 12 characters). Both values of
 * the BMP280 fit this shape: the temperature is in hundredths of a degree, and the
 * pressure in pascals is hectopascals times one hundred.
 */
uint32_t format_fixed2( char * pcBuffer, uint32_t ulCapacity, int32_t lHundredths );

#endif /* FORMAT_H */
