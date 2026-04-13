/*
 * load_timing.h -- map load profiling
 *
 * Set LOAD_LOGGING to 0 to compile out all profiling with zero overhead.
 * Set LOAD_LOGGING to 1 to enable loadlog.txt output on every map load.
 */

#pragma once

#define LOAD_LOGGING 1

#if LOAD_LOGGING
#   include <stdio.h>
#   include <stdarg.h>
static inline void LoadLog_Append( const char *fmt, ... ) {
    FILE *f = fopen( "loadlog.txt", "a" );
    if ( !f ) return;
    va_list ap;
    va_start( ap, fmt );
    vfprintf( f, fmt, ap );
    va_end( ap );
    fclose( f );
}
#else
#   define LoadLog_Append(...) ((void)0)
#endif
