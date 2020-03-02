/*****************************************************************************
 * util.c
 *****************************************************************************
 * Copyright (C) 2004, 2015 VideoLAN
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
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <syslog.h>

#include <bitstream/mpeg/psi.h>

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define MAX_BLOCKS 500
#define MAX_MSG 1024
#define VERB_DBG  4
#define VERB_INFO 3
#define VERB_WARN 2
#define VERB_ERR 1

static block_t *p_block_lifo = NULL;
static unsigned int i_block_count = 0;

/*****************************************************************************
 * block_New
 *****************************************************************************/
block_t *block_New( void )
{
    block_t *p_block;

    if (i_block_count)
    {
        p_block = p_block_lifo;
        p_block_lifo = p_block->p_next;
        i_block_count--;
    }
    else
    {
        p_block = malloc(sizeof(block_t));
    }

    p_block->p_next = NULL;
    p_block->i_refcount = 1;
    return p_block;
}

/*****************************************************************************
 * block_Delete
 *****************************************************************************/
void block_Delete( block_t *p_block )
{
    if (i_block_count >= MAX_BLOCKS )
    {
        free( p_block );
        return;
    }

    p_block->p_next = p_block_lifo;
    p_block_lifo = p_block;
    i_block_count++;
}

/*****************************************************************************
 * block_Vacuum
 *****************************************************************************/
void block_Vacuum( void )
{
    while (i_block_count)
    {
        block_t *p_block = p_block_lifo;
        p_block_lifo = p_block->p_next;
        free(p_block);
        i_block_count--;
    }
}

/*****************************************************************************
 * msg_Connect
 *****************************************************************************/
void msg_Connect( const char *ident )
{
    i_syslog = 1;
    openlog( ident, LOG_NDELAY | LOG_PID, LOG_USER );
}

/*****************************************************************************
 * msg_Disconnect
 *****************************************************************************/
void msg_Disconnect( void )
{
    i_syslog = 0;
    closelog();
}

/*****************************************************************************
 * msg_Info
 *****************************************************************************/
void msg_Info( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose < VERB_INFO )
        return;

    va_list args;
    va_start( args, psz_format );

    if ( !i_syslog )
    {
        char psz_fmt[MAX_MSG];
        snprintf( psz_fmt, MAX_MSG, "info: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
    else
    {
        vsyslog( LOG_INFO, psz_format, args );
    }

    va_end(args);
}

/*****************************************************************************
 * msg_Err
 *****************************************************************************/
void msg_Err( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose < VERB_ERR )
        return;

    va_list args;
    va_start( args, psz_format );

    if ( !i_syslog )
    {
        char psz_fmt[MAX_MSG];
        snprintf( psz_fmt, MAX_MSG, "error: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
    else
    {
        vsyslog( LOG_ERR, psz_format, args );
    }

    va_end(args);
}

/*****************************************************************************
 * msg_Warn
 *****************************************************************************/
void msg_Warn( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose < VERB_WARN )
        return;

    va_list args;
    va_start( args, psz_format );

    if ( !i_syslog )
    {
        char psz_fmt[MAX_MSG];
        snprintf( psz_fmt, MAX_MSG, "warning: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
    else
    {
        vsyslog( LOG_WARNING, psz_format, args );
    }

    va_end(args);
}

/*****************************************************************************
 * msg_Dbg
 *****************************************************************************/
void msg_Dbg( void *_unused, const char *psz_format, ... )
{
    if ( i_verbose < VERB_DBG )
        return;

    va_list args;
    va_start( args, psz_format );

    if ( !i_syslog )
    {
        char psz_fmt[MAX_MSG];
        snprintf( psz_fmt, MAX_MSG, "debug: %s\n", psz_format );
        vfprintf( stderr, psz_fmt, args );
    }
    else
    {
        vsyslog( LOG_DEBUG, psz_format, args );
    }

    va_end(args);
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
    va_end(args);
}

/*****************************************************************************
 * streq
 *****************************************************************************/
inline bool streq(char *a, char *b) {
    if (!a && b) return false;
    if (!b && a) return false;
    if (a == b) return true;
    return strcmp(a, b) == 0 ? true : false;
}

/*****************************************************************************
 * xstrdup
 *****************************************************************************/
inline char * xstrdup(char *str) {
    return str ? strdup(str) : NULL;
}

/*****************************************************************************
 * dvb_string_init
 *****************************************************************************/
void dvb_string_init(dvb_string_t *p_dvb_string)
{
    p_dvb_string->p = NULL;
    p_dvb_string->i = 0;
}

/*****************************************************************************
 * dvb_string_clean
 *****************************************************************************/
void dvb_string_clean(dvb_string_t *p_dvb_string)
{
    free(p_dvb_string->p);
}

/*****************************************************************************
 * dvb_string_copy
 *****************************************************************************/
void dvb_string_copy(dvb_string_t *p_dst, const dvb_string_t *p_src)
{
    p_dst->p = malloc(p_src->i);
    memcpy(p_dst->p, p_src->p, p_src->i);
    p_dst->i = p_src->i;
}

/*****************************************************************************
 * dvb_string_cmp
 *****************************************************************************/
int dvb_string_cmp(const dvb_string_t *p_1, const dvb_string_t *p_2)
{
    if (p_1->i != p_2->i)
        return p_1->i - p_2->i;
    return memcmp(p_1->p, p_2->p, p_1->i);
}

/*****************************************************************************
 * mdate
 *****************************************************************************/
mtime_t mdate( void )
{
#if defined (HAVE_CLOCK_NANOSLEEP)
    struct timespec ts;

    /* Try to use POSIX monotonic clock if available */
    if( clock_gettime( CLOCK_MONOTONIC, &ts ) == EINVAL )
        /* Run-time fallback to real-time clock (always available) */
        (void)clock_gettime( CLOCK_REALTIME, &ts );

    return ((mtime_t)ts.tv_sec * (mtime_t)1000000)
            + (mtime_t)(ts.tv_nsec / 1000);
#else
    struct timeval tv_date;

    /* gettimeofday() could return an error, and should be tested. However, the
     * only possible error, according to 'man', is EFAULT, which can not happen
     * here, since tv is a local variable. */
    gettimeofday( &tv_date, NULL );
    return( (mtime_t) tv_date.tv_sec * 1000000 + (mtime_t) tv_date.tv_usec );
#endif
}

/*****************************************************************************
 * msleep
 *****************************************************************************/
void msleep( mtime_t delay )
{
    struct timespec ts;
    ts.tv_sec = delay / 1000000;
    ts.tv_nsec = (delay % 1000000) * 1000;

#if defined( HAVE_CLOCK_NANOSLEEP )
    int val;
    while ( ( val = clock_nanosleep( CLOCK_MONOTONIC, 0, &ts, &ts ) ) == EINTR );
    if( val == EINVAL )
    {
        ts.tv_sec = delay / 1000000;
        ts.tv_nsec = (delay % 1000000) * 1000;
        while ( clock_nanosleep( CLOCK_REALTIME, 0, &ts, &ts ) == EINTR );
    }
#else
    while ( nanosleep( &ts, &ts ) && errno == EINTR );
#endif
}

/*****************************************************************************
 * hexDump
 *****************************************************************************/
void hexDump( uint8_t *p_data, uint32_t i_len )
{
    uint16_t i, j;

    char *p_outline;
    char *p_hrdata;

    p_outline = malloc(70);
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
        msg_Dbg( NULL, "%s", p_outline );
    }

    free( p_hrdata );
    free( p_outline );
}

/*****************************************************************************
 * ParseNodeService: parse a host:port string
 *****************************************************************************/
struct addrinfo *ParseNodeService( char *_psz_string, char **ppsz_end,
                                   uint16_t i_default_port )
{
    int i_family = AF_INET;
    char psz_port_buffer[6];
    char *psz_string = strdup( _psz_string );
    char *psz_node, *psz_port = NULL, *psz_end;
    struct addrinfo *p_res;
    struct addrinfo hint;
    int i_ret;

    if ( psz_string[0] == '[' )
    {
        i_family = AF_INET6;
        psz_node = psz_string + 1;
        psz_end = strchr( psz_node, ']' );
        if ( psz_end == NULL )
        {
            msg_Warn( NULL, "invalid IPv6 address %s", _psz_string );
            free( psz_string );
            return NULL;
        }
        *psz_end++ = '\0';
    }
    else
    {
        psz_node = psz_string;
        psz_end = strpbrk( psz_string, "@:,/" );
    }

    if ( psz_end != NULL && psz_end[0] == ':' )
    {
        *psz_end++ = '\0';
        psz_port = psz_end;
        psz_end = strpbrk( psz_port, "@:,/" );
    }

    if ( psz_end != NULL )
    {
        *psz_end = '\0';
        if ( ppsz_end != NULL )
            *ppsz_end = _psz_string + (psz_end - psz_string);
    }
    else if ( ppsz_end != NULL )
        *ppsz_end = _psz_string + strlen(_psz_string);

    if ( i_default_port != 0 && (psz_port == NULL || !*psz_port) )
    {
        sprintf( psz_port_buffer, "%u", i_default_port );
        psz_port = psz_port_buffer;
    }

    if ( psz_node[0] == '\0' )
    {
        free( psz_string );
        return NULL;
    }

    memset( &hint, 0, sizeof(hint) );
    hint.ai_family = i_family;
    hint.ai_socktype = SOCK_DGRAM;
    hint.ai_protocol = 0;
    hint.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV | AI_ADDRCONFIG;
    if ( (i_ret = getaddrinfo( psz_node, psz_port, NULL, &p_res )) != 0 )
    {
        msg_Warn( NULL, "getaddrinfo(host=%s, port=%s) error: %s",
            psz_node, psz_port ? psz_port : "", gai_strerror(i_ret) );
        free( psz_string );
        return NULL;
    }

    free( psz_string );
    return p_res;
}

/*****************************************************************************
 * psi_pack_section: return psi section
 *  Note: Allocates the return value. The caller must free it.
 *****************************************************************************/
uint8_t *psi_pack_section( uint8_t *p_section, unsigned int *pi_size ) {
    uint8_t *p_flat_section;
    uint16_t psi_length = psi_get_length( p_section ) + PSI_HEADER_SIZE;
    *pi_size = 0;

    p_flat_section = malloc( psi_length );
    if ( !p_flat_section )
        return NULL;

    *pi_size = psi_length;
    memcpy( p_flat_section, p_section, psi_length );

    return p_flat_section;
}

/*****************************************************************************
 * psi_pack_sections: return psi sections as array
 *  Note: Allocates the return value. The caller must free it.
 *****************************************************************************/
uint8_t *psi_pack_sections( uint8_t **pp_sections, unsigned int *pi_size ) {
    uint8_t i_last_section;
    uint8_t *p_flat_section;
    unsigned int i, i_pos = 0;

    if ( !psi_table_validate( pp_sections ) )
        return NULL;

    i_last_section = psi_table_get_lastsection( pp_sections );

    /* Calculate total size */
    *pi_size = 0;
    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section = psi_table_get_section( pp_sections, i );
        *pi_size += psi_get_length( p_section ) + PSI_HEADER_SIZE;
    }

    p_flat_section = malloc( *pi_size );
    if ( !p_flat_section )
        return NULL;

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section = psi_table_get_section( pp_sections, i );
        uint16_t psi_length = psi_get_length( p_section ) + PSI_HEADER_SIZE;

        memcpy( p_flat_section + i_pos, p_section, psi_length );
        i_pos += psi_length;
    }

    return p_flat_section;

}

/*****************************************************************************
 * psi_unpack_sections: return psi sections
 *  Note: Allocates psi_table, the result must be psi_table_free()'ed
 *****************************************************************************/
uint8_t **psi_unpack_sections( uint8_t *p_flat_sections, unsigned int i_size ) {
    uint8_t **pp_sections;
    unsigned int i, i_offset = 0;

    pp_sections = psi_table_allocate();
    if ( !pp_sections )
    {
        msg_Err( NULL, "%s: cannot allocate PSI table\n", __func__ );
        return NULL;
    }

    psi_table_init( pp_sections );

    for ( i = 0; i < PSI_TABLE_MAX_SECTIONS; i++ ) {
        uint8_t *p_section = p_flat_sections + i_offset;
        uint16_t i_section_len = psi_get_length( p_section ) + PSI_HEADER_SIZE;

        if ( !psi_validate( p_section ) )
        {
            msg_Err( NULL, "%s: Invalid section %d\n", __func__, i );
            psi_table_free( pp_sections );
            return NULL;
        }

        /* Must use allocated section not p_flat_section + offset directly! */
        uint8_t *p_section_local = psi_private_allocate();
        if ( !p_section_local )
        {
            msg_Err( NULL, "%s: cannot allocate PSI private\n", __func__ );
            psi_table_free( pp_sections );
            return NULL;
        }
        memcpy( p_section_local, p_section, i_section_len );

        /* We ignore the return value of psi_table_section(), because it is useless
           in this case. We are building the table section by section and when we have
           more than one section in a table, psi_table_section() returns false when section
           0 is added.  */
        psi_table_section( pp_sections, p_section_local );

        i_offset += i_section_len;
        if ( i_offset >= i_size - 1 )
            break;
    }

    return pp_sections;
}
