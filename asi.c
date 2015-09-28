/*****************************************************************************
 * asi.c: support for Computer Modules ASI cards
 *****************************************************************************
 * Copyright (C) 2004, 2009, 2015 VideoLAN
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
#include "config.h"

#ifdef HAVE_ASI_SUPPORT

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include <ev.h>

#include <bitstream/common.h>

#include "asi.h"

#include "dvblast.h"

/*
 * The problem with hardware filtering is that on startup, when you only
 * set a filter on PID 0, it can take a very long time for a large buffer
 * (typically ~100 TS packets) to fill up. And the buffer size cannot be
 * adjusted afer startup. --Meuuh
 */
//#define USE_HARDWARE_FILTERING

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define ASI_DEVICE "/dev/asirx%u"
#define ASI_TIMESTAMPS_FILE "/sys/class/asi/asirx%u/timestamps"
#define ASI_BUFSIZE_FILE "/sys/class/asi/asirx%u/bufsize"
#define ASI_LOCK_TIMEOUT 5000000 /* 5 s */

static int i_handle;
static struct ev_io asi_watcher;
static struct ev_timer mute_watcher;
static int i_bufsize;
static uint8_t p_pid_filter[8192 / 8];
static bool b_sync = false;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void asi_Read(struct ev_loop *loop, struct ev_io *w, int revents);
static void asi_MuteCb(struct ev_loop *loop, struct ev_timer *w, int revents);

/*****************************************************************************
 * Local helpers
 *****************************************************************************/
#define MAXLEN 256

static int ReadULSysfs( const char *psz_fmt, unsigned int i_link )
{
    char psz_file[MAXLEN], psz_data[MAXLEN];
    char *psz_tmp;
    int i_fd;
    ssize_t i_ret;
    unsigned int i_data;

    snprintf( psz_file, sizeof(psz_file), psz_fmt, i_link );
    psz_file[sizeof(psz_file) - 1] = '\0';

    if ( (i_fd = open( psz_file, O_RDONLY )) < 0 )
        return i_fd;

    i_ret = read( i_fd, psz_data, sizeof(psz_data) );
    close( i_fd );

    if ( i_ret < 0 )
        return i_ret;

    i_data = strtoul( psz_data, &psz_tmp, 0 );
    if ( *psz_tmp != '\n' )
        return -1;

    return i_data;
}

static ssize_t WriteULSysfs( const char *psz_fmt, unsigned int i_link,
                             unsigned int i_buf )
{
    char psz_file[MAXLEN], psz_data[MAXLEN];
    int i_fd;
    ssize_t i_ret;

    snprintf( psz_file, sizeof(psz_file), psz_fmt, i_link );
    psz_file[sizeof(psz_file) - 1] = '\0';

    snprintf( psz_data, sizeof(psz_data), "%u\n", i_buf );
    psz_file[sizeof(psz_data) - 1] = '\0';

    if ( (i_fd = open( psz_file, O_WRONLY )) < 0 )
        return i_fd;

    i_ret = write( i_fd, psz_data, strlen(psz_data) + 1 );
    close( i_fd );
    return i_ret;
}

/*****************************************************************************
 * asi_Open
 *****************************************************************************/
void asi_Open( void )
{
    char psz_dev[MAXLEN];

    /* No timestamp - we wouldn't know what to do with them */
    if ( WriteULSysfs( ASI_TIMESTAMPS_FILE, i_asi_adapter, 0 ) < 0 )
    {
        msg_Err( NULL, "couldn't write file " ASI_TIMESTAMPS_FILE,
                 i_asi_adapter );
        exit(EXIT_FAILURE);
    }

    if ( (i_bufsize = ReadULSysfs( ASI_BUFSIZE_FILE, i_asi_adapter )) < 0 )
    {
        msg_Err( NULL, "couldn't read file " ASI_BUFSIZE_FILE, i_asi_adapter );
        exit(EXIT_FAILURE);
    }

    if ( i_bufsize % TS_SIZE )
    {
        msg_Err( NULL, ASI_BUFSIZE_FILE " must be a multiple of 188",
                 i_asi_adapter );
        exit(EXIT_FAILURE);
    }

    snprintf( psz_dev, sizeof(psz_dev), ASI_DEVICE, i_asi_adapter );
    psz_dev[sizeof(psz_dev) - 1] = '\0';
    if ( (i_handle = open( psz_dev, O_RDONLY, 0 )) < 0 )
    {
        msg_Err( NULL, "couldn't open device " ASI_DEVICE " (%s)",
                 i_asi_adapter, strerror(errno) );
        exit(EXIT_FAILURE);
    }

#ifdef USE_HARDWARE_FILTERING
    memset( p_pid_filter, 0x0, sizeof(p_pid_filter) );
#else
    memset( p_pid_filter, 0xff, sizeof(p_pid_filter) );
#endif
    if ( ioctl( i_handle, ASI_IOC_RXSETPF, p_pid_filter ) < 0 )
    {
        msg_Warn( NULL, "couldn't filter padding" );
    }

    fsync( i_handle );

    ev_io_init(&asi_watcher, asi_Read, i_handle, EV_READ);
    ev_io_start(event_loop, &asi_watcher);

    ev_timer_init(&mute_watcher, asi_MuteCb,
                  ASI_LOCK_TIMEOUT / 1000000., ASI_LOCK_TIMEOUT / 1000000.);
}

/*****************************************************************************
 * ASI events
 *****************************************************************************/
static void asi_Read(struct ev_loop *loop, struct ev_io *w, int revents)
{
    unsigned int i_val;

    if ( ioctl(i_handle, ASI_IOC_RXGETEVENTS, &i_val) == 0 )
    {
        if ( i_val & ASI_EVENT_RX_BUFFER )
            msg_Warn( NULL, "driver receive buffer queue overrun" );
        if ( i_val & ASI_EVENT_RX_FIFO )
            msg_Warn( NULL, "onboard receive FIFO overrun" );
        if ( i_val & ASI_EVENT_RX_CARRIER )
            msg_Warn( NULL, "carrier status change" );
        if ( i_val & ASI_EVENT_RX_LOS )
            msg_Warn( NULL, "loss of packet synchronization" );
        if ( i_val & ASI_EVENT_RX_AOS )
            msg_Warn( NULL, "acquisition of packet synchronization" );
        if ( i_val & ASI_EVENT_RX_DATA )
            msg_Warn( NULL, "receive data status change" );
    }

    struct iovec p_iov[i_bufsize / TS_SIZE];
    block_t *p_ts, **pp_current = &p_ts;
    int i, i_len;

    for ( i = 0; i < i_bufsize / TS_SIZE; i++ )
    {
        *pp_current = block_New();
        p_iov[i].iov_base = (*pp_current)->p_ts;
        p_iov[i].iov_len = TS_SIZE;
        pp_current = &(*pp_current)->p_next;
    }

    if ( (i_len = readv(i_handle, p_iov, i_bufsize / TS_SIZE)) < 0 )
    {
        msg_Err( NULL, "couldn't read from device " ASI_DEVICE " (%s)",
                 i_asi_adapter, strerror(errno) );
        i_len = 0;
    }
    i_len /= TS_SIZE;

    if ( i_len )
    {
        if ( !b_sync )
        {
            msg_Info( NULL, "frontend has acquired lock" );
            switch (i_print_type) {
            case PRINT_XML:
                fprintf(print_fh, "<STATUS type=\"lock\" status=\"1\"/>\n");
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "lock status: 1\n");
                break;
            default:
                break;
            }

            b_sync = true;
        }

        ev_timer_again(loop, &mute_watcher);
    }

    pp_current = &p_ts;
    while ( i_len && *pp_current )
    {
        pp_current = &(*pp_current)->p_next;
        i_len--;
    }

    if ( *pp_current )
        msg_Dbg( NULL, "partial buffer received" );
    block_DeleteChain( *pp_current );
    *pp_current = NULL;

    demux_Run( p_ts );
}

static void asi_MuteCb(struct ev_loop *loop, struct ev_timer *w, int revents)
{
    msg_Warn( NULL, "frontend has lost lock" );
    ev_timer_stop(loop, w);

    switch (i_print_type) {
    case PRINT_XML:
        fprintf(print_fh, "<STATUS type=\"lock\" status=\"0\"/>\n");
        break;
    case PRINT_TEXT:
        fprintf(print_fh, "lock status: 0\n" );
        break;
    default:
        break;
    }
}

/*****************************************************************************
 * asi_SetFilter
 *****************************************************************************/
int asi_SetFilter( uint16_t i_pid )
{
#ifdef USE_HARDWARE_FILTERING
    p_pid_filter[ i_pid / 8 ] |= (0x01 << (i_pid % 8));
    if ( ioctl( i_handle, ASI_IOC_RXSETPF, p_pid_filter ) < 0 )
        msg_Warn( NULL, "couldn't add filter on PID %u", i_pid );

    return 1;
#else
    return -1;
#endif
}

/*****************************************************************************
 * asi_UnsetFilter: normally never called
 *****************************************************************************/
void asi_UnsetFilter( int i_fd, uint16_t i_pid )
{
#ifdef USE_HARDWARE_FILTERING
    p_pid_filter[ i_pid / 8 ] &= ~(0x01 << (i_pid % 8));
    if ( ioctl( i_handle, ASI_IOC_RXSETPF, p_pid_filter ) < 0 )
        msg_Warn( NULL, "couldn't remove filter on PID %u", i_pid );
#endif
}

/*****************************************************************************
 * asi_Reset
 *****************************************************************************/
void asi_Reset( void )
{
    msg_Warn( NULL, "asi_Reset() do nothing" );
}

#endif
