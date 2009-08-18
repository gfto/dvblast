/*****************************************************************************
 * util.c
 *****************************************************************************
 * Copyright (C) 2004 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define MAX_MSG 1024
#define VERB_DBG  3
#define VERB_INFO 2
#define VERB_WARN 1

/*****************************************************************************
 * msg_Info
 *****************************************************************************/
void msg_Info( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose >= VERB_INFO )
    {
        va_list args;
        char psz_fmt[MAX_MSG];
        va_start( args, psz_format );

        snprintf( psz_fmt, MAX_MSG, "info: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
}

/*****************************************************************************
 * msg_Err
 *****************************************************************************/
void msg_Err( void *_unused, const char *psz_format, ... )
{
    va_list args;
    char psz_fmt[MAX_MSG];
    va_start( args, psz_format );

    snprintf( psz_fmt, MAX_MSG, "error: %s\n", psz_format );
    vfprintf( stderr, psz_fmt, args );
}

/*****************************************************************************
 * msg_Warn
 *****************************************************************************/
void msg_Warn( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose >= VERB_WARN )
    {
        va_list args;
        char psz_fmt[MAX_MSG];
        va_start( args, psz_format );

        snprintf( psz_fmt, MAX_MSG, "warning: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
}

/*****************************************************************************
 * msg_Dbg
 *****************************************************************************/
void msg_Dbg( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose >= VERB_DBG )
    {
        va_list args;
        char psz_fmt[MAX_MSG];
        va_start( args, psz_format );

        snprintf( psz_fmt, MAX_MSG, "debug: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
}

/*****************************************************************************
 * msg_Raw
 *****************************************************************************/
void msg_Raw( void *_unused, const char *psz_format, ... )
{
    va_list args;
    char psz_fmt[MAX_MSG];
    va_start( args, psz_format );

    snprintf( psz_fmt, MAX_MSG, "%s\n", psz_format );
    vfprintf( stderr, psz_fmt, args );
}

/*****************************************************************************
 * mdate
 *****************************************************************************/
mtime_t mdate( void )
{
    struct timeval tv_date;

    /* gettimeofday() could return an error, and should be tested. However, the
     * only possible error, according to 'man', is EFAULT, which can not happen
     * here, since tv is a local variable. */
    gettimeofday( &tv_date, NULL );
    return( (mtime_t) tv_date.tv_sec * 1000000 + (mtime_t) tv_date.tv_usec );
}

/*****************************************************************************
 * msleep
 *****************************************************************************/
void msleep( mtime_t delay )
{
    struct timespec ts_delay;

    ts_delay.tv_sec = delay / 1000000;
    ts_delay.tv_nsec = (delay % 1000000) * 1000;

    nanosleep( &ts_delay, NULL );
}

/*****************************************************************************
 * hexDump
 *****************************************************************************/
void hexDump( uint8_t *p_data, uint32_t i_len )
{
    uint16_t i, j;

    char *p_outline;
    char *p_hrdata;

    p_outline = malloc(69);
    p_hrdata  = malloc(17);

    for( i = 0; i < i_len; i += 16 )
    {

        sprintf( p_outline, "%03x: ", i );

        for( j = 0; j < 16; j++ )
        {
            if( i + j < i_len )
            {
                sprintf( &p_outline[5 + (3 * j)], "%02x ", p_data[i + j] );

                if( p_data[i + j] >= 32 && p_data[i + j] <= 136 )
                {
                    sprintf( &p_hrdata[j], "%c", p_data[i + j] );
                }

                else {
                    sprintf( &p_hrdata[j], "." );
                }
            }

            else
            {
                sprintf( &p_outline[5 + (3 * j)], "   " );
                sprintf( &p_hrdata[j], " " );
            }
        }

        sprintf( &p_outline[53], "%16s", p_hrdata );
        msg_Dbg( NULL, p_outline );
    }

    free( p_hrdata );
    free( p_outline );
}

