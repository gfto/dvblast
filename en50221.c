/*****************************************************************************
 * en50221.c : implementation of the transport, session and applications
 * layers of EN 50 221
 *****************************************************************************
 * Copyright (C) 2004-2005, 2010, 2015 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 * Based on code from libdvbci Copyright (C) 2000 Klaus Schmidinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#include "config.h"

#ifdef HAVE_DVB_SUPPORT

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <ev.h>

/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>

#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/ci.h>
#include <bitstream/dvb/si.h>

#include "dvblast.h"
#include "en50221.h"
#include "comm.h"

#define TAB_APPEND( count, tab, p )             \
    if( (count) > 0 )                           \
    {                                           \
        (tab) = realloc( tab, sizeof( void ** ) * ( (count) + 1 ) ); \
    }                                           \
    else                                        \
    {                                           \
        (tab) = malloc( sizeof( void ** ) );    \
    }                                           \
    (tab)[count] = (p);                         \
    (count)++

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#undef DEBUG_TPDU
#define CAM_INIT_TIMEOUT 15000000 /* 15 s */
#undef HLCI_WAIT_CAM_READY
#define CAPMT_WAIT 100 /* ms */
#define CA_POLL_PERIOD 100000 /* 100 ms */

typedef struct en50221_msg_t
{
    uint8_t *p_data;
    int i_size;
    struct en50221_msg_t *p_next;
} en50221_msg_t;

typedef struct en50221_session_t
{
    int i_slot;
    int i_resource_id;
    void (* pf_handle)( access_t *, int, uint8_t *, int );
    void (* pf_close)( access_t *, int );
    void (* pf_manage)( access_t *, int );
    void *p_sys;
} en50221_session_t;

typedef struct ci_slot_t
{
    bool b_active;
    bool b_expect_answer;
    bool b_has_data;
    bool b_mmi_expected;
    bool b_mmi_undisplayed;

    /* TPDU reception */
    en50221_msg_t *p_recv;

    /* TPDU emission */
    en50221_msg_t *p_send;
    en50221_msg_t **pp_send_last;
    uint8_t *p;

    /* InitSlot timer */
    struct ev_timer init_watcher;

    /* SPDUSend callback, if p_spdu is not NULL */
    /* SessionOpen callback, if not 0 */
    int i_pending_session_id;
} ci_slot_t;

int i_ca_handle = 0;
int i_ca_type = -1;

static struct ev_io cam_watcher;
static struct ev_timer slot_watcher;
static int i_nb_slots = 0;
static ci_slot_t p_slots[MAX_CI_SLOTS];
static en50221_session_t p_sessions[MAX_SESSIONS];

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void SessionOpenCb( access_t *p_access, uint8_t i_slot );
static void SPDUHandle( access_t * p_access, uint8_t i_slot,
                        uint8_t *p_spdu, int i_size );

static void ResourceManagerOpen( access_t * p_access, int i_session_id );
static void ApplicationInformationOpen( access_t * p_access, int i_session_id );
static void ConditionalAccessOpen( access_t * p_access, int i_session_id );
static void DateTimeOpen( access_t * p_access, int i_session_id );
static void ResetSlotCb(struct ev_loop *loop, struct ev_timer *w, int revents);
static void en50221_Read(struct ev_loop *loop, struct ev_io *w, int revents);
static void en50221_Poll(struct ev_loop *loop, struct ev_timer *w, int revents);
static void MMIOpen( access_t * p_access, int i_session_id );

/*****************************************************************************
 * Utility functions
 *****************************************************************************/
#define SIZE_INDICATOR 0x80

static uint8_t *GetLength( uint8_t *p_data, int *pi_length )
{
    *pi_length = *p_data++;

    if ( (*pi_length & SIZE_INDICATOR) != 0 )
    {
        int l = *pi_length & ~SIZE_INDICATOR;
        int i;

        *pi_length = 0;
        for ( i = 0; i < l; i++ )
            *pi_length = (*pi_length << 8) | *p_data++;
    }

    return p_data;
}

static uint8_t *SetLength( uint8_t *p_data, int i_length )
{
    uint8_t *p = p_data;

    if ( i_length < 128 )
    {
        *p++ = i_length;
    }
    else if ( i_length < 256 )
    {
        *p++ = SIZE_INDICATOR | 0x1;
        *p++ = i_length;
    }
    else if ( i_length < 65536 )
    {
        *p++ = SIZE_INDICATOR | 0x2;
        *p++ = i_length >> 8;
        *p++ = i_length & 0xff;
    }
    else if ( i_length < 16777216 )
    {
        *p++ = SIZE_INDICATOR | 0x3;
        *p++ = i_length >> 16;
        *p++ = (i_length >> 8) & 0xff;
        *p++ = i_length & 0xff;
    }
    else
    {
        *p++ = SIZE_INDICATOR | 0x4;
        *p++ = i_length >> 24;
        *p++ = (i_length >> 16) & 0xff;
        *p++ = (i_length >> 8) & 0xff;
        *p++ = i_length & 0xff;
    }

    return p;
}


/*
 * Transport layer
 */

#define MAX_TPDU_SIZE  4096
#define MAX_TPDU_DATA  (MAX_TPDU_SIZE - 7)

#define DATA_INDICATOR 0x80

#define T_SB           0x80
#define T_RCV          0x81
#define T_CREATE_TC    0x82
#define T_CTC_REPLY    0x83
#define T_DELETE_TC    0x84
#define T_DTC_REPLY    0x85
#define T_REQUEST_TC   0x86
#define T_NEW_TC       0x87
#define T_TC_ERROR     0x88
#define T_DATA_LAST    0xA0
#define T_DATA_MORE    0xA1

static void Dump( bool b_outgoing, uint8_t *p_data, int i_size )
{
#ifdef DEBUG_TPDU
    int i;
#define MAX_DUMP 256
    fprintf(stderr, "%s ", b_outgoing ? "-->" : "<--");
    for ( i = 0; i < i_size && i < MAX_DUMP; i++)
        fprintf(stderr, "%02X ", p_data[i]);
    fprintf(stderr, "%s\n", i_size >= MAX_DUMP ? "..." : "");
#endif
}

/*****************************************************************************
 * TPDUWrite
 *****************************************************************************/
static int TPDUWrite( access_t * p_access, uint8_t i_slot )
{
    ci_slot_t *p_slot = &p_slots[i_slot];
    en50221_msg_t *p_send = p_slot->p_send;

    if ( p_slot->b_expect_answer )
        msg_Warn( p_access,
                  "en50221: writing while expecting an answer on slot %u",
                  i_slot );
    if ( p_send == NULL )
    {
        msg_Warn( p_access, "en50221: no data to write on slot %u !", i_slot );
        return -1;
    }
    p_slot->p_send = p_send->p_next;
    if ( p_slot->p_send == NULL )
        p_slot->pp_send_last = &p_slot->p_send;

    Dump( true, p_send->p_data, p_send->i_size );

    if ( write( i_ca_handle, p_send->p_data, p_send->i_size )
          != p_send->i_size )
    {
        msg_Err( p_access, "en50221: cannot write to CAM device (%m)" );
        free( p_send->p_data );
        free( p_send );
        return -1;
    }

    free( p_send->p_data );
    free( p_send );
    p_slot->b_expect_answer = true;

    return 0;
}

/*****************************************************************************
 * TPDUSend
 *****************************************************************************/
static int TPDUSend( access_t * p_access, uint8_t i_slot, uint8_t i_tag,
                     const uint8_t *p_content, int i_length )
{
    ci_slot_t *p_slot = &p_slots[i_slot];
    uint8_t i_tcid = i_slot + 1;
    en50221_msg_t *p_send = malloc( sizeof(en50221_msg_t) );
    uint8_t *p_data = malloc( MAX_TPDU_SIZE );
    int i_size;

    i_size = 0;
    p_data[0] = i_slot;
    p_data[1] = i_tcid;
    p_data[2] = i_tag;

    switch ( i_tag )
    {
    case T_RCV:
    case T_CREATE_TC:
    case T_CTC_REPLY:
    case T_DELETE_TC:
    case T_DTC_REPLY:
    case T_REQUEST_TC:
        p_data[3] = 1; /* length */
        p_data[4] = i_tcid;
        i_size = 5;
        break;

    case T_NEW_TC:
    case T_TC_ERROR:
        p_data[3] = 2; /* length */
        p_data[4] = i_tcid;
        p_data[5] = p_content[0];
        i_size = 6;
        break;

    case T_DATA_LAST:
    case T_DATA_MORE:
    {
        /* i_length <= MAX_TPDU_DATA */
        uint8_t *p = p_data + 3;
        p = SetLength( p, i_length + 1 );
        *p++ = i_tcid;

        if ( i_length )
            memcpy( p, p_content, i_length );
        i_size = i_length + (p - p_data);
        break;
    }

    default:
        break;
    }

    p_send->p_data = p_data;
    p_send->i_size = i_size;
    p_send->p_next = NULL;

    *p_slot->pp_send_last = p_send;
    p_slot->pp_send_last = &p_send->p_next;

    if ( !p_slot->b_expect_answer )
        return TPDUWrite( p_access, i_slot );

    return 0;
}


/*****************************************************************************
 * TPDURecv
 *****************************************************************************/
static int TPDURecv( access_t * p_access )
{
    ci_slot_t *p_slot;
    uint8_t i_tag, i_slot;
    uint8_t p_data[MAX_TPDU_SIZE];
    int i_size;
    bool b_last = false;

    do
    {
        i_size = read( i_ca_handle, p_data, MAX_TPDU_SIZE );
    }
    while ( i_size < 0 && errno == EINTR );

    if ( i_size < 5 )
    {
        msg_Err( p_access, "en50221: cannot read from CAM device (%d:%m)",
                 i_size );
        return -1;
    }

    Dump( false, p_data, i_size );

    i_slot = p_data[1] - 1;
    i_tag = p_data[2];

    if ( i_slot >= i_nb_slots )
    {
        msg_Warn( p_access, "en50221: TPDU is from an unknown slot %u",
                  i_slot );
        return -1;
    }
    p_slot = &p_slots[i_slot];

    p_slot->b_has_data = !!(p_data[i_size - 4] == T_SB
                             && p_data[i_size - 3] == 2
                             && (p_data[i_size - 1] & DATA_INDICATOR));
    p_slot->b_expect_answer = false;

    switch ( i_tag )
    {
    case T_CTC_REPLY:
        p_slot->b_active = true;
        ev_timer_stop(event_loop, &p_slot->init_watcher);
        msg_Dbg( p_access, "CI slot %d is active", i_slot );
        break;

    case T_SB:
        break;

    case T_DATA_LAST:
        b_last = true;
        /* intended pass-through */
    case T_DATA_MORE:
    {
        en50221_msg_t *p_recv;
        int i_session_size;
        uint8_t *p_session = GetLength( &p_data[3], &i_session_size );

        if ( i_session_size <= 1 )
            break;
        p_session++;
        i_session_size--;

        if ( p_slot->p_recv == NULL )
        {
            p_slot->p_recv = malloc( sizeof(en50221_msg_t) );
            p_slot->p_recv->p_data = NULL;
            p_slot->p_recv->i_size = 0;
        }

        p_recv = p_slot->p_recv;
        p_recv->p_data = realloc( p_recv->p_data,
                                  p_recv->i_size + i_session_size );
        memcpy( &p_recv->p_data[ p_recv->i_size ], p_session, i_session_size );
        p_recv->i_size += i_session_size;

        if ( b_last )
        {
            SPDUHandle( p_access, i_slot, p_recv->p_data, p_recv->i_size );
            free( p_recv->p_data );
            free( p_recv );
            p_slot->p_recv = NULL;
        }
        break;
    }

    default:
        msg_Warn( p_access, "en50221: unhandled R_TPDU tag %u slot %u", i_tag,
                  i_slot );
        break;
    }

    if ( !p_slot->b_expect_answer && p_slot->p_send != NULL )
        TPDUWrite( p_access, i_slot );
    if ( !p_slot->b_expect_answer && p_slot->i_pending_session_id != 0 )
        SessionOpenCb( p_access, i_slot );
    if ( !p_slot->b_expect_answer && p_slot->b_has_data )
        TPDUSend( p_access, i_slot, T_RCV, NULL, 0 );

    return 0;
}


/*
 * Session layer
 */

#define ST_SESSION_NUMBER           0x90
#define ST_OPEN_SESSION_REQUEST     0x91
#define ST_OPEN_SESSION_RESPONSE    0x92
#define ST_CREATE_SESSION           0x93
#define ST_CREATE_SESSION_RESPONSE  0x94
#define ST_CLOSE_SESSION_REQUEST    0x95
#define ST_CLOSE_SESSION_RESPONSE   0x96

#define SS_OK             0x00
#define SS_NOT_ALLOCATED  0xF0

#define RI_RESOURCE_MANAGER            0x00010041
#define RI_APPLICATION_INFORMATION     0x00020041
#define RI_CONDITIONAL_ACCESS_SUPPORT  0x00030041
#define RI_HOST_CONTROL                0x00200041
#define RI_DATE_TIME                   0x00240041
#define RI_MMI                         0x00400041

static int ResourceIdToInt( uint8_t *p_data )
{
    return ((int)p_data[0] << 24) | ((int)p_data[1] << 16)
            | ((int)p_data[2] << 8) | p_data[3];
}

/*****************************************************************************
 * SPDUSend
 *****************************************************************************/
static int SPDUSend( access_t *p_access, int i_session_id,
                      uint8_t *p_data, int i_size )
{
    uint8_t *p_spdu = malloc( i_size + 4 );
    uint8_t *p = p_spdu;
    uint8_t i_slot = p_sessions[i_session_id - 1].i_slot;

    *p++ = ST_SESSION_NUMBER;
    *p++ = 0x02;
    *p++ = (i_session_id >> 8);
    *p++ = i_session_id & 0xff;

    memcpy( p, p_data, i_size );

    i_size += 4;
    p = p_spdu;

    while ( i_size > 0 )
    {
        if ( i_size > MAX_TPDU_DATA )
        {
            if ( TPDUSend( p_access, i_slot, T_DATA_MORE, p,
                           MAX_TPDU_DATA ) != 0 )
            {
                msg_Err( p_access, "couldn't send TPDU on session %d",
                         i_session_id );
                free( p_spdu );
                return -1;
            }
            p += MAX_TPDU_DATA;
            i_size -= MAX_TPDU_DATA;
        }
        else
        {
            if ( TPDUSend( p_access, i_slot, T_DATA_LAST, p, i_size )
                    != 0 )
            {
                msg_Err( p_access, "couldn't send TPDU on session %d",
                         i_session_id );
                free( p_spdu );
                return -1;
            }
            i_size = 0;
        }
    }

    free( p_spdu );
    return 0;
}

/*****************************************************************************
 * SessionOpen
 *****************************************************************************/
static void SessionOpenCb( access_t *p_access, uint8_t i_slot )
{
    ci_slot_t *p_slot = &p_slots[i_slot];
    int i_session_id = p_slot->i_pending_session_id;
    int i_resource_id = p_sessions[i_session_id - 1].i_resource_id;

    p_slot->i_pending_session_id = 0;

    switch ( i_resource_id )
    {
    case RI_RESOURCE_MANAGER:
        ResourceManagerOpen( p_access, i_session_id ); break;
    case RI_APPLICATION_INFORMATION:
        ApplicationInformationOpen( p_access, i_session_id ); break;
    case RI_CONDITIONAL_ACCESS_SUPPORT:
        ConditionalAccessOpen( p_access, i_session_id ); break;
    case RI_DATE_TIME:
        DateTimeOpen( p_access, i_session_id ); break;
    case RI_MMI:
        MMIOpen( p_access, i_session_id ); break;

    case RI_HOST_CONTROL:
    default:
        msg_Err( p_access, "unknown resource id (0x%x)", i_resource_id );
        p_sessions[i_session_id - 1].i_resource_id = 0;
    }
}

static void SessionOpen( access_t * p_access, uint8_t i_slot,
                         uint8_t *p_spdu, int i_size )
{
    ci_slot_t *p_slot = &p_slots[i_slot];
    int i_session_id;
    int i_resource_id = ResourceIdToInt( &p_spdu[2] );
    uint8_t p_response[16];
    int i_status = SS_NOT_ALLOCATED;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        if ( !p_sessions[i_session_id - 1].i_resource_id )
            break;
    }
    if ( i_session_id > MAX_SESSIONS )
    {
        msg_Err( p_access, "too many sessions !" );
        return;
    }
    p_sessions[i_session_id - 1].i_slot = i_slot;
    p_sessions[i_session_id - 1].i_resource_id = i_resource_id;
    p_sessions[i_session_id - 1].pf_close = NULL;
    p_sessions[i_session_id - 1].pf_manage = NULL;

    if ( i_resource_id == RI_RESOURCE_MANAGER
          || i_resource_id == RI_APPLICATION_INFORMATION
          || i_resource_id == RI_CONDITIONAL_ACCESS_SUPPORT
          || i_resource_id == RI_DATE_TIME
          || i_resource_id == RI_MMI )
    {
        i_status = SS_OK;
    }

    p_response[0] = ST_OPEN_SESSION_RESPONSE;
    p_response[1] = 0x7;
    p_response[2] = i_status;
    p_response[3] = p_spdu[2];
    p_response[4] = p_spdu[3];
    p_response[5] = p_spdu[4];
    p_response[6] = p_spdu[5];
    p_response[7] = i_session_id >> 8;
    p_response[8] = i_session_id & 0xff;

    if ( TPDUSend( p_access, i_slot, T_DATA_LAST, p_response, 9 ) != 0 )
    {
        msg_Err( p_access,
                 "SessionOpen: couldn't send TPDU on slot %d", i_slot );
        return;
    }

    if ( p_slot->i_pending_session_id != 0 )
        msg_Warn( p_access, "overwriting pending session %d",
                  p_slot->i_pending_session_id );
    p_slot->i_pending_session_id = i_session_id;
}

#if 0
/* unused code for the moment - commented out to keep gcc happy */
/*****************************************************************************
 * SessionCreate
 *****************************************************************************/
static void SessionCreate( access_t * p_access, int i_slot, int i_resource_id )
{
    uint8_t p_response[16];
    uint8_t i_tag;
    int i_session_id;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        if ( !p_sessions[i_session_id - 1].i_resource_id )
            break;
    }
    if ( i_session_id > MAX_SESSIONS )
    {
        msg_Err( p_access, "too many sessions !" );
        return;
    }
    p_sessions[i_session_id - 1].i_slot = i_slot;
    p_sessions[i_session_id - 1].i_resource_id = i_resource_id;
    p_sessions[i_session_id - 1].pf_close = NULL;
    p_sessions[i_session_id - 1].pf_manage = NULL;
    p_sessions[i_session_id - 1].p_sys = NULL;

    p_response[0] = ST_CREATE_SESSION;
    p_response[1] = 0x6;
    p_response[2] = i_resource_id >> 24;
    p_response[3] = (i_resource_id >> 16) & 0xff;
    p_response[4] = (i_resource_id >> 8) & 0xff;
    p_response[5] = i_resource_id & 0xff;
    p_response[6] = i_session_id >> 8;
    p_response[7] = i_session_id & 0xff;

    if ( TPDUSend( p_access, i_slot, T_DATA_LAST, p_response, 4 ) !=
            0 )
    {
        msg_Err( p_access,
                 "SessionCreate: couldn't send TPDU on slot %d", i_slot );
        return;
    }
}
#endif

/*****************************************************************************
 * SessionCreateResponse
 *****************************************************************************/
static void SessionCreateResponse( access_t * p_access, uint8_t i_slot,
                                   uint8_t *p_spdu, int i_size )
{
    int i_status = p_spdu[2];
    int i_resource_id = ResourceIdToInt( &p_spdu[3] );
    int i_session_id = ((int)p_spdu[7] << 8) | p_spdu[8];

    if ( i_status != SS_OK )
    {
        msg_Err( p_access, "SessionCreateResponse: failed to open session %d"
                 " resource=0x%x status=0x%x", i_session_id, i_resource_id,
                 i_status );
        p_sessions[i_session_id - 1].i_resource_id = 0;
        return;
    }

    switch ( i_resource_id )
    {
    case RI_RESOURCE_MANAGER:
        ResourceManagerOpen( p_access, i_session_id ); break;
    case RI_APPLICATION_INFORMATION:
        ApplicationInformationOpen( p_access, i_session_id ); break;
    case RI_CONDITIONAL_ACCESS_SUPPORT:
        ConditionalAccessOpen( p_access, i_session_id ); break;
    case RI_DATE_TIME:
        DateTimeOpen( p_access, i_session_id ); break;
    case RI_MMI:
        MMIOpen( p_access, i_session_id ); break;

    case RI_HOST_CONTROL:
    default:
        msg_Err( p_access, "unknown resource id (0x%x)", i_resource_id );
        p_sessions[i_session_id - 1].i_resource_id = 0;
    }
}

/*****************************************************************************
 * SessionSendClose
 *****************************************************************************/
static void SessionSendClose( access_t * p_access, int i_session_id )
{
    uint8_t p_response[16];
    uint8_t i_slot = p_sessions[i_session_id - 1].i_slot;

    p_response[0] = ST_CLOSE_SESSION_REQUEST;
    p_response[1] = 0x2;
    p_response[2] = i_session_id >> 8;
    p_response[3] = i_session_id & 0xff;

    if ( TPDUSend( p_access, i_slot, T_DATA_LAST, p_response, 4 ) !=
            0 )
    {
        msg_Err( p_access,
                 "SessionSendClose: couldn't send TPDU on slot %d", i_slot );
        return;
    }
}

/*****************************************************************************
 * SessionClose
 *****************************************************************************/
static void SessionClose( access_t * p_access, int i_session_id )
{
    uint8_t p_response[16];
    uint8_t i_slot = p_sessions[i_session_id - 1].i_slot;

    if ( p_sessions[i_session_id - 1].pf_close != NULL )
        p_sessions[i_session_id - 1].pf_close( p_access, i_session_id );
    p_sessions[i_session_id - 1].i_resource_id = 0;

    p_response[0] = ST_CLOSE_SESSION_RESPONSE;
    p_response[1] = 0x3;
    p_response[2] = SS_OK;
    p_response[3] = i_session_id >> 8;
    p_response[4] = i_session_id & 0xff;

    if ( TPDUSend( p_access, i_slot, T_DATA_LAST, p_response, 5 ) !=
            0 )
    {
        msg_Err( p_access,
                 "SessionClose: couldn't send TPDU on slot %d", i_slot );
        return;
    }
}

/*****************************************************************************
 * SPDUHandle
 *****************************************************************************/
static void SPDUHandle( access_t * p_access, uint8_t i_slot,
                        uint8_t *p_spdu, int i_size )
{
    int i_session_id;

    switch ( p_spdu[0] )
    {
    case ST_SESSION_NUMBER:
        if ( i_size <= 4 )
            return;
        i_session_id = (p_spdu[2] << 8) | p_spdu[3];
        if ( i_session_id <= MAX_SESSIONS
              && p_sessions[i_session_id - 1].pf_handle != NULL )
            p_sessions[i_session_id - 1].pf_handle( p_access, i_session_id,
                                                    p_spdu + 4, i_size - 4 );
        break;

    case ST_OPEN_SESSION_REQUEST:
        if ( i_size != 6 || p_spdu[1] != 0x4 )
            return;
        SessionOpen( p_access, i_slot, p_spdu, i_size );
        break;

    case ST_CREATE_SESSION_RESPONSE:
        if ( i_size != 9 || p_spdu[1] != 0x7 )
            return;
        SessionCreateResponse( p_access, i_slot, p_spdu, i_size );
        break;

    case ST_CLOSE_SESSION_REQUEST:
        if ( i_size != 4 || p_spdu[1] != 0x2 )
            return;
        i_session_id = ((int)p_spdu[2] << 8) | p_spdu[3];
        SessionClose( p_access, i_session_id );
        break;

    case ST_CLOSE_SESSION_RESPONSE:
        if ( i_size != 5 || p_spdu[1] != 0x3 )
            return;
        i_session_id = ((int)p_spdu[3] << 8) | p_spdu[4];
        if ( p_spdu[2] )
        {
            msg_Err( p_access, "closing a session which is not allocated (%d)",
                     i_session_id );
        }
        else
        {
            if ( p_sessions[i_session_id - 1].pf_close != NULL )
                p_sessions[i_session_id - 1].pf_close( p_access,
                                                              i_session_id );
            p_sessions[i_session_id - 1].i_resource_id = 0;
        }
        break;

    default:
        msg_Err( p_access, "unexpected tag in SPDUHandle (%x)", p_spdu[0] );
        break;
    }
}


/*
 * Application layer
 */

#define AOT_NONE                    0x000000
#define AOT_PROFILE_ENQ             0x9F8010
#define AOT_PROFILE                 0x9F8011
#define AOT_PROFILE_CHANGE          0x9F8012
#define AOT_APPLICATION_INFO_ENQ    0x9F8020
#define AOT_APPLICATION_INFO        0x9F8021
#define AOT_ENTER_MENU              0x9F8022
#define AOT_CA_INFO_ENQ             0x9F8030
#define AOT_CA_INFO                 0x9F8031
#define AOT_CA_PMT                  0x9F8032
#define AOT_CA_PMT_REPLY            0x9F8033
#define AOT_CA_UPDATE               0x9F8034
#define AOT_TUNE                    0x9F8400
#define AOT_REPLACE                 0x9F8401
#define AOT_CLEAR_REPLACE           0x9F8402
#define AOT_ASK_RELEASE             0x9F8403
#define AOT_DATE_TIME_ENQ           0x9F8440
#define AOT_DATE_TIME               0x9F8441
#define AOT_CLOSE_MMI               0x9F8800
#define AOT_DISPLAY_CONTROL         0x9F8801
#define AOT_DISPLAY_REPLY           0x9F8802
#define AOT_TEXT_LAST               0x9F8803
#define AOT_TEXT_MORE               0x9F8804
#define AOT_KEYPAD_CONTROL          0x9F8805
#define AOT_KEYPRESS                0x9F8806
#define AOT_ENQ                     0x9F8807
#define AOT_ANSW                    0x9F8808
#define AOT_MENU_LAST               0x9F8809
#define AOT_MENU_MORE               0x9F880A
#define AOT_MENU_ANSW               0x9F880B
#define AOT_LIST_LAST               0x9F880C
#define AOT_LIST_MORE               0x9F880D
#define AOT_SUBTITLE_SEGMENT_LAST   0x9F880E
#define AOT_SUBTITLE_SEGMENT_MORE   0x9F880F
#define AOT_DISPLAY_MESSAGE         0x9F8810
#define AOT_SCENE_END_MARK          0x9F8811
#define AOT_SCENE_DONE              0x9F8812
#define AOT_SCENE_CONTROL           0x9F8813
#define AOT_SUBTITLE_DOWNLOAD_LAST  0x9F8814
#define AOT_SUBTITLE_DOWNLOAD_MORE  0x9F8815
#define AOT_FLUSH_DOWNLOAD          0x9F8816
#define AOT_DOWNLOAD_REPLY          0x9F8817
#define AOT_COMMS_CMD               0x9F8C00
#define AOT_CONNECTION_DESCRIPTOR   0x9F8C01
#define AOT_COMMS_REPLY             0x9F8C02
#define AOT_COMMS_SEND_LAST         0x9F8C03
#define AOT_COMMS_SEND_MORE         0x9F8C04
#define AOT_COMMS_RCV_LAST          0x9F8C05
#define AOT_COMMS_RCV_MORE          0x9F8C06

/*****************************************************************************
 * APDUGetTag
 *****************************************************************************/
static int APDUGetTag( const uint8_t *p_apdu, int i_size )
{
    if ( i_size >= 3 )
    {
        int i, t = 0;
        for ( i = 0; i < 3; i++ )
            t = (t << 8) | *p_apdu++;
        return t;
    }

    return AOT_NONE;
}

/*****************************************************************************
 * APDUGetLength
 *****************************************************************************/
static uint8_t *APDUGetLength( uint8_t *p_apdu, int *pi_size )
{
    return GetLength( &p_apdu[3], pi_size );
}

/*****************************************************************************
 * APDUSend
 *****************************************************************************/
static int APDUSend( access_t * p_access, int i_session_id, int i_tag,
                      uint8_t *p_data, int i_size )
{
    uint8_t *p_apdu = malloc( i_size + 12 );
    uint8_t *p = p_apdu;
    ca_msg_t ca_msg;
    int i_ret;

    *p++ = (i_tag >> 16);
    *p++ = (i_tag >> 8) & 0xff;
    *p++ = i_tag & 0xff;
    p = SetLength( p, i_size );
    if ( i_size )
        memcpy( p, p_data, i_size );
    if ( i_ca_type == CA_CI_LINK )
    {
        i_ret = SPDUSend( p_access, i_session_id, p_apdu, i_size + p - p_apdu );
    }
    else
    {
        if ( i_size + p - p_apdu > 256 )
        {
            msg_Err( p_access, "CAM: apdu overflow" );
            i_ret = -1;
        }
        else
        {
            ca_msg.length = i_size + p - p_apdu;
            if ( i_size == 0 ) ca_msg.length=3;
            memcpy( ca_msg.msg, p_apdu, i_size + p - p_apdu );
            i_ret = ioctl(i_ca_handle, CA_SEND_MSG, &ca_msg );
            if ( i_ret < 0 )
            {
                msg_Err( p_access, "Error sending to CAM: %m" );
                i_ret = -1;
            }
        }
    }
    free( p_apdu );
    return i_ret;
}

/*
 * Resource Manager
 */

/*****************************************************************************
 * ResourceManagerHandle
 *****************************************************************************/
static void ResourceManagerHandle( access_t * p_access, int i_session_id,
                                   uint8_t *p_apdu, int i_size )
{
    int i_tag = APDUGetTag( p_apdu, i_size );

    switch ( i_tag )
    {
    case AOT_PROFILE_ENQ:
    {
        int resources[] = { htonl(RI_RESOURCE_MANAGER),
                            htonl(RI_APPLICATION_INFORMATION),
                            htonl(RI_CONDITIONAL_ACCESS_SUPPORT),
                            htonl(RI_DATE_TIME),
                            htonl(RI_MMI)
                          };
        APDUSend( p_access, i_session_id, AOT_PROFILE, (uint8_t*)resources,
                  sizeof(resources) );
        break;
    }
    case AOT_PROFILE:
        APDUSend( p_access, i_session_id, AOT_PROFILE_CHANGE, NULL, 0 );
        break;

    default:
        msg_Err( p_access, "unexpected tag in ResourceManagerHandle (0x%x)",
                 i_tag );
    }
}

/*****************************************************************************
 * ResourceManagerOpen
 *****************************************************************************/
static void ResourceManagerOpen( access_t * p_access, int i_session_id )
{

    msg_Dbg( p_access, "opening ResourceManager session (%d)", i_session_id );

    p_sessions[i_session_id - 1].pf_handle = ResourceManagerHandle;

    APDUSend( p_access, i_session_id, AOT_PROFILE_ENQ, NULL, 0 );
}

/*
 * Application Information
 */

/*****************************************************************************
 * ApplicationInformationEnterMenu
 *****************************************************************************/
static void ApplicationInformationEnterMenu( access_t * p_access,
                                             int i_session_id )
{
    int i_slot = p_sessions[i_session_id - 1].i_slot;

    msg_Dbg( p_access, "entering MMI menus on session %d", i_session_id );
    APDUSend( p_access, i_session_id, AOT_ENTER_MENU, NULL, 0 );
    p_slots[i_slot].b_mmi_expected = true;
}

/*****************************************************************************
 * ApplicationInformationHandle
 *****************************************************************************/
static void ApplicationInformationHandle( access_t * p_access, int i_session_id,
                                          uint8_t *p_apdu, int i_size )
{
    int i_tag = APDUGetTag( p_apdu, i_size );

    switch ( i_tag )
    {
    case AOT_APPLICATION_INFO:
    {
        int i_type, i_manufacturer, i_code;
        int l = 0;
        uint8_t *d = APDUGetLength( p_apdu, &l );

        if ( l < 4 ) break;

        i_type = *d++;
        i_manufacturer = ((int)d[0] << 8) | d[1];
        d += 2;
        i_code = ((int)d[0] << 8) | d[1];
        d += 2;
        d = GetLength( d, &l );

        {
            char *psz_name = malloc(l + 1);
            memcpy( psz_name, d, l );
            psz_name[l] = '\0';
            msg_Info( p_access, "CAM: %s, %02X, %04X, %04X",
                      psz_name, i_type, i_manufacturer, i_code );
            switch (i_print_type)
            {
            case PRINT_XML:
                psz_name = dvb_string_xml_escape(psz_name);
                fprintf(print_fh, "<STATUS type=\"cam\" status=\"1\" cam_name=\"%s\" cam_type=\"%d\" cam_manufacturer=\"%d\" cam_product=\"%d\" />\n",
                        psz_name, i_type, i_manufacturer, i_code);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "CAM name: %s type: %d manufacturer: %d product: %d\n",
                        psz_name, i_type, i_manufacturer, i_code);
                break;
            default:
                break;
            }
            free(psz_name);
        }
        break;
    }
    default:
        msg_Err( p_access,
                 "unexpected tag in ApplicationInformationHandle (0x%x)",
                 i_tag );
    }
}

/*****************************************************************************
 * ApplicationInformationOpen
 *****************************************************************************/
static void ApplicationInformationOpen( access_t * p_access, int i_session_id )
{

    msg_Dbg( p_access, "opening ApplicationInformation session (%d)", i_session_id );

    p_sessions[i_session_id - 1].pf_handle = ApplicationInformationHandle;

    APDUSend( p_access, i_session_id, AOT_APPLICATION_INFO_ENQ, NULL, 0 );
}

/*
 * Conditional Access
 */

typedef struct
{
    int i_nb_system_ids;
    uint16_t *pi_system_ids;

    int i_selected_programs;
    int b_high_level;
} system_ids_t;

static bool CheckSystemID( system_ids_t *p_ids, uint16_t i_id )
{
    int i;
    if( p_ids == NULL ) return false;
    if( p_ids->b_high_level ) return true;

    for ( i = 0; i < p_ids->i_nb_system_ids; i++ )
        if ( p_ids->pi_system_ids[i] == i_id )
            return true;

    return false;
}

/*****************************************************************************
 * CAPMTBuild
 *****************************************************************************/
static bool HasCADescriptors( system_ids_t *p_ids, uint8_t *p_descs )
{
    const uint8_t *p_desc;
    uint16_t j = 0;

    while ( (p_desc = descs_get_desc( p_descs, j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_desc );
        j++;

        if ( i_tag == 0x9 && desc09_validate( p_desc )
              && CheckSystemID( p_ids, desc09_get_sysid( p_desc ) ) )
            return true;
    }

    return false;
}

static void CopyCADescriptors( system_ids_t *p_ids, uint8_t i_cmd,
                               uint8_t *p_infos, uint8_t *p_descs )
{
    const uint8_t *p_desc;
    uint16_t j = 0, k = 0;

    capmti_init( p_infos );
    capmti_set_length( p_infos, 0xfff );
    capmti_set_cmd( p_infos, i_cmd );

    while ( (p_desc = descs_get_desc( p_descs, j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_desc );
        j++;

        if ( i_tag == 0x9 && desc09_validate( p_desc )
              && CheckSystemID( p_ids, desc09_get_sysid( p_desc ) ) )
        {
            uint8_t *p_info = capmti_get_info( p_infos, k );
            k++;
            memcpy( p_info, p_desc,
                    DESC_HEADER_SIZE + desc_get_length( p_desc ) );
        }
    }

    if ( k )
    {
        uint8_t *p_info = capmti_get_info( p_infos, k );
        capmti_set_length( p_infos, p_info - p_infos - DESCS_HEADER_SIZE );
    }
    else
        capmti_set_length( p_infos, 0 );
}

static uint8_t *CAPMTBuild( access_t * p_access, int i_session_id,
                            uint8_t *p_pmt, uint8_t i_list_mgt,
                            uint8_t i_cmd, int *pi_capmt_size )
{
    system_ids_t *p_ids =
        (system_ids_t *)p_sessions[i_session_id - 1].p_sys;
    uint8_t *p_es;
    uint8_t *p_capmt, *p_capmt_n;
    uint16_t j, k;
    bool b_has_ca = HasCADescriptors( p_ids, pmt_get_descs( p_pmt ) );
    bool b_has_es = false;

    j = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_es );
        j++;

        if ( demux_PIDIsSelected( i_pid ) )
        {
            b_has_es = true;
            b_has_ca = b_has_ca
                        || HasCADescriptors( p_ids, pmtn_get_descs( p_es ) );
        }
    }

    if ( !b_has_es )
    {
        *pi_capmt_size = 0;
        return NULL;
    }

    if ( !b_has_ca )
    {
        msg_Warn( p_access,
                  "no compatible scrambling system for SID %d on session %d",
                  pmt_get_program( p_pmt ), i_session_id );
        *pi_capmt_size = 0;
        return NULL;
    }

    p_capmt = capmt_allocate();
    capmt_init( p_capmt );
    capmt_set_listmanagement( p_capmt, i_list_mgt );
    capmt_set_program( p_capmt, pmt_get_program( p_pmt ) );
    capmt_set_version( p_capmt, psi_get_version( p_pmt ) );

    CopyCADescriptors( p_ids, i_cmd, capmt_get_infos( p_capmt ),
                       pmt_get_descs( p_pmt ) );

    j = 0; k = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_es );
        j++;

        if ( !demux_PIDIsSelected( i_pid ) )
            continue;

        p_capmt_n = capmt_get_es( p_capmt, k );
        k++;

        capmtn_init( p_capmt_n );
        capmtn_set_streamtype( p_capmt_n, pmtn_get_streamtype( p_es ) );
        capmtn_set_pid( p_capmt_n, pmtn_get_pid( p_es ) );

        CopyCADescriptors( p_ids, i_cmd, capmtn_get_infos( p_capmt_n ),
                           pmtn_get_descs( p_es ) );
    }

    p_capmt_n = capmt_get_es( p_capmt, k );
    *pi_capmt_size = p_capmt_n - p_capmt;

    return p_capmt;
}

/*****************************************************************************
 * CAPMTFirst
 *****************************************************************************/
static void CAPMTFirst( access_t * p_access, int i_session_id, uint8_t *p_pmt )
{
    uint8_t *p_capmt;
    int i_capmt_size;

    msg_Dbg( p_access, "adding first CAPMT for SID %d on session %d",
             pmt_get_program( p_pmt ), i_session_id );

    p_capmt = CAPMTBuild( p_access, i_session_id, p_pmt,
                          0x3 /* only */, 0x1 /* ok_descrambling */,
                          &i_capmt_size );

    if ( i_capmt_size )
    {
        APDUSend( p_access, i_session_id, AOT_CA_PMT, p_capmt, i_capmt_size );
        free( p_capmt );
    }
}

/*****************************************************************************
 * CAPMTAdd
 *****************************************************************************/
static void CAPMTAdd( access_t * p_access, int i_session_id, uint8_t *p_pmt )
{
    system_ids_t *p_ids =
        (system_ids_t *)p_sessions[i_session_id - 1].p_sys;
    uint8_t *p_capmt;
    int i_capmt_size;

    p_ids->i_selected_programs++;
    if( p_ids->i_selected_programs == 1 )
    {
        CAPMTFirst( p_access, i_session_id, p_pmt );
        return;
    }

    msg_Dbg( p_access, "adding CAPMT for SID %d on session %d",
             pmt_get_program( p_pmt ), i_session_id );

    p_capmt = CAPMTBuild( p_access, i_session_id, p_pmt,
                          0x4 /* add */, 0x1 /* ok_descrambling */,
                          &i_capmt_size );

    if ( i_capmt_size )
    {
        APDUSend( p_access, i_session_id, AOT_CA_PMT, p_capmt, i_capmt_size );
        free( p_capmt );
    }
}

/*****************************************************************************
 * CAPMTUpdate
 *****************************************************************************/
static void CAPMTUpdate( access_t * p_access, int i_session_id, uint8_t *p_pmt )
{
    uint8_t *p_capmt;
    int i_capmt_size;

    msg_Dbg( p_access, "updating CAPMT for SID %d on session %d",
             pmt_get_program( p_pmt ), i_session_id );

    p_capmt = CAPMTBuild( p_access, i_session_id, p_pmt,
                          0x5 /* update */, 0x1 /* ok_descrambling */,
                          &i_capmt_size );

    if ( i_capmt_size )
    {
        APDUSend( p_access, i_session_id, AOT_CA_PMT, p_capmt, i_capmt_size );
        free( p_capmt );
    }
}

/*****************************************************************************
 * CAPMTDelete
 *****************************************************************************/
static void CAPMTDelete( access_t * p_access, int i_session_id, uint8_t *p_pmt )
{
    system_ids_t *p_ids =
        (system_ids_t *)p_sessions[i_session_id - 1].p_sys;
    uint8_t *p_capmt;
    int i_capmt_size;

    p_ids->i_selected_programs--;
    msg_Dbg( p_access, "deleting CAPMT for SID %d on session %d",
             pmt_get_program( p_pmt ), i_session_id );

    p_capmt = CAPMTBuild( p_access, i_session_id, p_pmt,
                          0x5 /* update */, 0x4 /* not selected */,
                          &i_capmt_size );

    if ( i_capmt_size )
    {
        APDUSend( p_access, i_session_id, AOT_CA_PMT, p_capmt, i_capmt_size );
        free( p_capmt );
    }
}

/*****************************************************************************
 * ConditionalAccessHandle
 *****************************************************************************/
static void ConditionalAccessHandle( access_t * p_access, int i_session_id,
                                     uint8_t *p_apdu, int i_size )
{
    system_ids_t *p_ids =
        (system_ids_t *)p_sessions[i_session_id - 1].p_sys;
    int i_tag = APDUGetTag( p_apdu, i_size );

    switch ( i_tag )
    {
    case AOT_CA_INFO:
    {
        int i;
        int l = 0;
        uint8_t *d = APDUGetLength( p_apdu, &l );
        msg_Dbg( p_access, "CA system IDs supported by the application :" );

        if ( p_ids->i_nb_system_ids )
            free( p_ids->pi_system_ids );
        p_ids->i_nb_system_ids = l / 2;
        p_ids->pi_system_ids = malloc( p_ids->i_nb_system_ids
                                        * sizeof(uint16_t) );

        for ( i = 0; i < p_ids->i_nb_system_ids; i++ )
        {
            p_ids->pi_system_ids[i] = ((uint16_t)d[0] << 8) | d[1];
            d += 2;
            msg_Dbg( p_access, "- 0x%x", p_ids->pi_system_ids[i] );
        }

        demux_ResendCAPMTs();
        break;
    }

    case AOT_CA_UPDATE:
        /* http://www.cablelabs.com/specifications/OC-SP-HOSTPOD-IF-I08-011221.pdf */
    case AOT_CA_PMT_REPLY:
        /* We do not care */
        break;

    default:
        msg_Err( p_access,
                 "unexpected tag in ConditionalAccessHandle (0x%x)",
                 i_tag );
    }
}

/*****************************************************************************
 * ConditionalAccessClose
 *****************************************************************************/
static void ConditionalAccessClose( access_t * p_access, int i_session_id )
{
    system_ids_t *p_ids =
        (system_ids_t *)p_sessions[i_session_id - 1].p_sys;

    msg_Dbg( p_access, "closing ConditionalAccess session (%d)", i_session_id );

    if ( p_ids->i_nb_system_ids )
        free( p_ids->pi_system_ids );

    free( p_sessions[i_session_id - 1].p_sys );
}

/*****************************************************************************
 * ConditionalAccessOpen
 *****************************************************************************/
static void ConditionalAccessOpen( access_t * p_access, int i_session_id )
{

    msg_Dbg( p_access, "opening ConditionalAccess session (%d)", i_session_id );

    p_sessions[i_session_id - 1].pf_handle = ConditionalAccessHandle;
    p_sessions[i_session_id - 1].pf_close = ConditionalAccessClose;
    p_sessions[i_session_id - 1].p_sys = malloc(sizeof(system_ids_t));
    memset( p_sessions[i_session_id - 1].p_sys, 0,
            sizeof(system_ids_t) );

    APDUSend( p_access, i_session_id, AOT_CA_INFO_ENQ, NULL, 0 );
}

/*
 * Date Time
 */

typedef struct
{
    int i_session_id;
    int i_interval;
    struct ev_timer watcher;
} date_time_t;

/*****************************************************************************
 * DateTimeSend
 *****************************************************************************/
static void DateTimeSend( access_t * p_access, int i_session_id )
{
    date_time_t *p_date =
        (date_time_t *)p_sessions[i_session_id - 1].p_sys;

    time_t t = time(NULL);
    struct tm tm_gmt;
    struct tm tm_loc;

    if ( gmtime_r(&t, &tm_gmt) && localtime_r(&t, &tm_loc) )
    {
        int Y = tm_gmt.tm_year;
        int M = tm_gmt.tm_mon + 1;
        int D = tm_gmt.tm_mday;
        int L = (M == 1 || M == 2) ? 1 : 0;
        int MJD = 14956 + D + (int)((Y - L) * 365.25)
                    + (int)((M + 1 + L * 12) * 30.6001);
        uint8_t p_response[7];

#define DEC2BCD(d) (((d / 10) << 4) + (d % 10))

        p_response[0] = htons(MJD) >> 8;
        p_response[1] = htons(MJD) & 0xff;
        p_response[2] = DEC2BCD(tm_gmt.tm_hour);
        p_response[3] = DEC2BCD(tm_gmt.tm_min);
        p_response[4] = DEC2BCD(tm_gmt.tm_sec);
        p_response[5] = htons(tm_loc.tm_gmtoff / 60) >> 8;
        p_response[6] = htons(tm_loc.tm_gmtoff / 60) & 0xff;

        APDUSend( p_access, i_session_id, AOT_DATE_TIME, p_response, 7 );

        ev_timer_again(event_loop, &p_date->watcher);
    }
}

static void _DateTimeSend(struct ev_loop *loop, struct ev_timer *w, int revents)
{
    date_time_t *p_date = container_of(w, date_time_t, watcher);
    DateTimeSend( NULL, p_date->i_session_id );
}

/*****************************************************************************
 * DateTimeHandle
 *****************************************************************************/
static void DateTimeHandle( access_t * p_access, int i_session_id,
                            uint8_t *p_apdu, int i_size )
{
    date_time_t *p_date =
        (date_time_t *)p_sessions[i_session_id - 1].p_sys;

    int i_tag = APDUGetTag( p_apdu, i_size );

    switch ( i_tag )
    {
    case AOT_DATE_TIME_ENQ:
    {
        int l;
        const uint8_t *d = APDUGetLength( p_apdu, &l );

        if ( l > 0 )
        {
            p_date->i_interval = *d;
            msg_Dbg( p_access, "DateTimeHandle : interval set to %d",
                     p_date->i_interval );
        }
        else
            p_date->i_interval = 0;

        ev_timer_stop(event_loop, &p_date->watcher);
        ev_timer_set(&p_date->watcher, p_date->i_interval,
                     p_date->i_interval);
        DateTimeSend( p_access, i_session_id );
        break;
    }
    default:
        msg_Err( p_access, "unexpected tag in DateTimeHandle (0x%x)", i_tag );
    }
}

/*****************************************************************************
 * DateTimeClose
 *****************************************************************************/
static void DateTimeClose( access_t * p_access, int i_session_id )
{
    date_time_t *p_date =
        (date_time_t *)p_sessions[i_session_id - 1].p_sys;
    ev_timer_stop(event_loop, &p_date->watcher);

    msg_Dbg( p_access, "closing DateTime session (%d)", i_session_id );

    free( p_date );
}

/*****************************************************************************
 * DateTimeOpen
 *****************************************************************************/
static void DateTimeOpen( access_t * p_access, int i_session_id )
{
    msg_Dbg( p_access, "opening DateTime session (%d)", i_session_id );

    p_sessions[i_session_id - 1].pf_handle = DateTimeHandle;
    p_sessions[i_session_id - 1].pf_manage = NULL;
    p_sessions[i_session_id - 1].pf_close = DateTimeClose;
    p_sessions[i_session_id - 1].p_sys = malloc(sizeof(date_time_t));
    memset( p_sessions[i_session_id - 1].p_sys, 0, sizeof(date_time_t) );

    date_time_t *p_date =
        (date_time_t *)p_sessions[i_session_id - 1].p_sys;
    p_date->i_session_id = i_session_id;
    ev_timer_init(&p_date->watcher, _DateTimeSend, 0, 0);

    DateTimeSend( p_access, i_session_id );
}

/*
 * MMI
 */

/* Display Control Commands */

#define DCC_SET_MMI_MODE                          0x01
#define DCC_DISPLAY_CHARACTER_TABLE_LIST          0x02
#define DCC_INPUT_CHARACTER_TABLE_LIST            0x03
#define DCC_OVERLAY_GRAPHICS_CHARACTERISTICS      0x04
#define DCC_FULL_SCREEN_GRAPHICS_CHARACTERISTICS  0x05

/* MMI Modes */

#define MM_HIGH_LEVEL                      0x01
#define MM_LOW_LEVEL_OVERLAY_GRAPHICS      0x02
#define MM_LOW_LEVEL_FULL_SCREEN_GRAPHICS  0x03

/* Display Reply IDs */

#define DRI_MMI_MODE_ACK                              0x01
#define DRI_LIST_DISPLAY_CHARACTER_TABLES             0x02
#define DRI_LIST_INPUT_CHARACTER_TABLES               0x03
#define DRI_LIST_GRAPHIC_OVERLAY_CHARACTERISTICS      0x04
#define DRI_LIST_FULL_SCREEN_GRAPHIC_CHARACTERISTICS  0x05
#define DRI_UNKNOWN_DISPLAY_CONTROL_CMD               0xF0
#define DRI_UNKNOWN_MMI_MODE                          0xF1
#define DRI_UNKNOWN_CHARACTER_TABLE                   0xF2

/* Enquiry Flags */

#define EF_BLIND  0x01

/* Answer IDs */

#define AI_CANCEL  0x00
#define AI_ANSWER  0x01

typedef struct
{
    en50221_mmi_object_t last_object;
} mmi_t;

static inline void en50221_MMIFree( en50221_mmi_object_t *p_object )
{
    int i;

    switch ( p_object->i_object_type )
    {
    case EN50221_MMI_ENQ:
        free( p_object->u.enq.psz_text );
        break;

    case EN50221_MMI_ANSW:
        if ( p_object->u.answ.b_ok )
        {
            free( p_object->u.answ.psz_answ );
        }
        break;

    case EN50221_MMI_MENU:
    case EN50221_MMI_LIST:
        free( p_object->u.menu.psz_title );
        free( p_object->u.menu.psz_subtitle );
        free( p_object->u.menu.psz_bottom );
        for ( i = 0; i < p_object->u.menu.i_choices; i++ )
        {
            free( p_object->u.menu.ppsz_choices[i] );
        }
        free( p_object->u.menu.ppsz_choices );
        break;

    default:
        break;
    }
}

/*****************************************************************************
 * MMISendObject
 *****************************************************************************/
static void MMISendObject( access_t *p_access, int i_session_id,
                           en50221_mmi_object_t *p_object )
{
    int i_slot = p_sessions[i_session_id - 1].i_slot;
    uint8_t *p_data;
    int i_size, i_tag;

    switch ( p_object->i_object_type )
    {
    case EN50221_MMI_ANSW:
        i_tag = AOT_ANSW;
        i_size = 1 + strlen( p_object->u.answ.psz_answ );
        p_data = malloc( i_size );
        p_data[0] = (p_object->u.answ.b_ok == true) ? 0x1 : 0x0;
        strncpy( (char *)&p_data[1], p_object->u.answ.psz_answ, i_size - 1 );
        break;

    case EN50221_MMI_MENU_ANSW:
        i_tag = AOT_MENU_ANSW;
        i_size = 1;
        p_data = malloc( i_size );
        p_data[0] = p_object->u.menu_answ.i_choice;
        break;

    default:
        msg_Err( p_access, "unknown MMI object %d", p_object->i_object_type );
        return;
    }

    APDUSend( p_access, i_session_id, i_tag, p_data, i_size );
    free( p_data );

    p_slots[i_slot].b_mmi_expected = true;
}

/*****************************************************************************
 * MMISendClose
 *****************************************************************************/
static void MMISendClose( access_t *p_access, int i_session_id )
{
    int i_slot = p_sessions[i_session_id - 1].i_slot;

    APDUSend( p_access, i_session_id, AOT_CLOSE_MMI, NULL, 0 );

    p_slots[i_slot].b_mmi_expected = true;
}

/*****************************************************************************
 * MMIDisplayReply
 *****************************************************************************/
static void MMIDisplayReply( access_t *p_access, int i_session_id )
{
    uint8_t p_response[2];

    p_response[0] = DRI_MMI_MODE_ACK;
    p_response[1] = MM_HIGH_LEVEL;

    APDUSend( p_access, i_session_id, AOT_DISPLAY_REPLY, p_response, 2 );

    msg_Dbg( p_access, "sending DisplayReply on session (%d)", i_session_id );
}

/*****************************************************************************
 * MMIGetText
 *****************************************************************************/
static char *MMIGetText( access_t *p_access, uint8_t **pp_apdu, int *pi_size )
{
    int i_tag = APDUGetTag( *pp_apdu, *pi_size );
    int l;
    uint8_t *d;

    if ( i_tag != AOT_TEXT_LAST )
    {
        msg_Err( p_access, "unexpected text tag: %06x", i_tag );
        *pi_size = 0;
        return strdup( "" );
    }

    d = APDUGetLength( *pp_apdu, &l );

    *pp_apdu += l + 4;
    *pi_size -= l + 4;

    return dvb_string_get( d, l, demux_Iconv, p_access );
}

/*****************************************************************************
 * MMIHandleEnq
 *****************************************************************************/
static void MMIHandleEnq( access_t *p_access, int i_session_id,
                          uint8_t *p_apdu, int i_size )
{
    mmi_t *p_mmi = (mmi_t *)p_sessions[i_session_id - 1].p_sys;
    int i_slot = p_sessions[i_session_id - 1].i_slot;
    int l;
    uint8_t *d = APDUGetLength( p_apdu, &l );

    en50221_MMIFree( &p_mmi->last_object );
    p_mmi->last_object.i_object_type = EN50221_MMI_ENQ;
    p_mmi->last_object.u.enq.b_blind = (*d & 0x1) ? true : false;
    d += 2; /* skip answer_text_length because it is not mandatory */
    l -= 2;
    p_mmi->last_object.u.enq.psz_text = malloc( l + 1 );
    strncpy( p_mmi->last_object.u.enq.psz_text, (char *)d, l );
    p_mmi->last_object.u.enq.psz_text[l] = '\0';

    msg_Dbg( p_access, "MMI enq: %s%s", p_mmi->last_object.u.enq.psz_text,
             p_mmi->last_object.u.enq.b_blind == true ? " (blind)" : "" );

    p_slots[i_slot].b_mmi_expected = false;
    p_slots[i_slot].b_mmi_undisplayed = true;
}

/*****************************************************************************
 * MMIHandleMenu
 *****************************************************************************/
static void MMIHandleMenu( access_t *p_access, int i_session_id, int i_tag,
                           uint8_t *p_apdu, int i_size )
{
    mmi_t *p_mmi = (mmi_t *)p_sessions[i_session_id - 1].p_sys;
    int i_slot = p_sessions[i_session_id - 1].i_slot;
    int l;
    uint8_t *d = APDUGetLength( p_apdu, &l );

    en50221_MMIFree( &p_mmi->last_object );
    p_mmi->last_object.i_object_type = (i_tag == AOT_MENU_LAST) ?
                                       EN50221_MMI_MENU : EN50221_MMI_LIST;
    p_mmi->last_object.u.menu.i_choices = 0;
    p_mmi->last_object.u.menu.ppsz_choices = NULL;

    if ( l > 0 )
    {
        l--; d++; /* choice_nb */

#define GET_FIELD( x )                                                      \
        if ( l > 0 )                                                        \
        {                                                                   \
            p_mmi->last_object.u.menu.psz_##x                               \
                            = MMIGetText( p_access, &d, &l );               \
            msg_Dbg( p_access, "MMI " STRINGIFY( x ) ": %s",                \
                     p_mmi->last_object.u.menu.psz_##x );                   \
        }

        GET_FIELD( title );
        GET_FIELD( subtitle );
        GET_FIELD( bottom );
#undef GET_FIELD

        while ( l > 0 )
        {
            char *psz_text = MMIGetText( p_access, &d, &l );
            TAB_APPEND( p_mmi->last_object.u.menu.i_choices,
                        p_mmi->last_object.u.menu.ppsz_choices,
                        psz_text );
            msg_Dbg( p_access, "MMI choice: %s", psz_text );
        }
    }

    p_slots[i_slot].b_mmi_expected = false;
    p_slots[i_slot].b_mmi_undisplayed = true;
}

/*****************************************************************************
 * MMIHandle
 *****************************************************************************/
static void MMIHandle( access_t *p_access, int i_session_id,
                       uint8_t *p_apdu, int i_size )
{
    int i_tag = APDUGetTag( p_apdu, i_size );

    switch ( i_tag )
    {
    case AOT_DISPLAY_CONTROL:
    {
        int l;
        uint8_t *d = APDUGetLength( p_apdu, &l );

        if ( l > 0 )
        {
            switch ( *d )
            {
            case DCC_SET_MMI_MODE:
                if ( l == 2 && d[1] == MM_HIGH_LEVEL )
                    MMIDisplayReply( p_access, i_session_id );
                else
                    msg_Err( p_access, "unsupported MMI mode %02x", d[1] );
                break;

            default:
                msg_Err( p_access, "unsupported display control command %02x",
                         *d );
                break;
            }
        }
        break;
    }

    case AOT_ENQ:
        MMIHandleEnq( p_access, i_session_id, p_apdu, i_size );
        break;

    case AOT_LIST_LAST:
    case AOT_MENU_LAST:
        MMIHandleMenu( p_access, i_session_id, i_tag, p_apdu, i_size );
        break;

    case AOT_CLOSE_MMI:
        SessionSendClose( p_access, i_session_id );
        break;

    default:
        msg_Err( p_access, "unexpected tag in MMIHandle (0x%x)", i_tag );
    }
}

/*****************************************************************************
 * MMIClose
 *****************************************************************************/
static void MMIClose( access_t *p_access, int i_session_id )
{
    int i_slot = p_sessions[i_session_id - 1].i_slot;
    mmi_t *p_mmi = (mmi_t *)p_sessions[i_session_id - 1].p_sys;

    en50221_MMIFree( &p_mmi->last_object );
    free( p_sessions[i_session_id - 1].p_sys );

    msg_Dbg( p_access, "closing MMI session (%d)", i_session_id );

    p_slots[i_slot].b_mmi_expected = false;
    p_slots[i_slot].b_mmi_undisplayed = true;
}

/*****************************************************************************
 * MMIOpen
 *****************************************************************************/
static void MMIOpen( access_t *p_access, int i_session_id )
{
    mmi_t *p_mmi;

    msg_Dbg( p_access, "opening MMI session (%d)", i_session_id );

    p_sessions[i_session_id - 1].pf_handle = MMIHandle;
    p_sessions[i_session_id - 1].pf_close = MMIClose;
    p_sessions[i_session_id - 1].p_sys = malloc(sizeof(mmi_t));
    p_mmi = (mmi_t *)p_sessions[i_session_id - 1].p_sys;
    p_mmi->last_object.i_object_type = EN50221_MMI_NONE;
}


/*
 * Hardware handling
 */

/*****************************************************************************
 * InitSlot: Open the transport layer
 *****************************************************************************/
static void InitSlot( access_t * p_access, int i_slot )
{
    if ( TPDUSend( p_access, i_slot, T_CREATE_TC, NULL, 0 ) != 0 )
        msg_Err( p_access, "en50221_Init: couldn't send TPDU on slot %d",
                 i_slot );
}

/*****************************************************************************
 * ResetSlot
 *****************************************************************************/
static void ResetSlot( int i_slot )
{
    ci_slot_t *p_slot = &p_slots[i_slot];
    int i_session_id;

    switch (i_print_type)
    {
    case PRINT_XML:
        fprintf(print_fh, "<STATUS type=\"cam\" status=\"0\" />\n");
        break;
    case PRINT_TEXT:
        fprintf(print_fh, "CAM none\n");
        break;
    default:
        break;
    }

    if ( ioctl( i_ca_handle, CA_RESET, 1 << i_slot ) != 0 )
        msg_Err( NULL, "en50221_Poll: couldn't reset slot %d", i_slot );
    p_slot->b_active = false;
    ev_timer_init(&p_slot->init_watcher, ResetSlotCb,
                  CAM_INIT_TIMEOUT / 1000000., 0);
    ev_timer_start(event_loop, &p_slot->init_watcher);
    p_slot->b_expect_answer = false;
    p_slot->b_mmi_expected = false;
    p_slot->b_mmi_undisplayed = false;
    if ( p_slot->p_recv != NULL )
    {
        free( p_slot->p_recv->p_data );
        free( p_slot->p_recv );
    }
    p_slot->p_recv = NULL;
    while ( p_slot->p_send != NULL )
    {
        en50221_msg_t *p_next = p_slot->p_send->p_next;
        free( p_slot->p_send->p_data );
        free( p_slot->p_send );
        p_slot->p_send = p_next;
    }
    p_slot->pp_send_last = &p_slot->p_send;

    /* Close all sessions for this slot. */
    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        if ( p_sessions[i_session_id - 1].i_resource_id
              && p_sessions[i_session_id - 1].i_slot == i_slot )
        {
            if ( p_sessions[i_session_id - 1].pf_close != NULL )
            {
                p_sessions[i_session_id - 1].pf_close( NULL, i_session_id );
            }
            p_sessions[i_session_id - 1].i_resource_id = 0;
        }
    }
}

static void ResetSlotCb(struct ev_loop *loop, struct ev_timer *w, int revents)
{
    ci_slot_t *p_slot = container_of(w, ci_slot_t, init_watcher);
    int i_slot = p_slot - &p_slots[0];

    if ( p_slot->b_active || !p_slot->b_expect_answer )
        return;

    msg_Warn( NULL, "no answer from CAM, resetting slot %d",
              i_slot );
    switch (i_print_type) {
    case PRINT_XML:
        fprintf(print_fh,
                "<EVENT type=\"reset\" cause=\"cam_mute\" />\n");
        break;
    case PRINT_TEXT:
       fprintf(print_fh, "reset cause: cam_mute\n");
       break;
    default:
       break;
    }

    ResetSlot( i_slot );
}


/*
 * External entry points
 */

/*****************************************************************************
 * en50221_Init : Initialize the CAM for en50221
 *****************************************************************************/
void en50221_Init( void )
{
    char psz_tmp[128];
    ca_caps_t caps;

    memset( &caps, 0, sizeof( ca_caps_t ));

    sprintf( psz_tmp, "/dev/dvb/adapter%d/ca%d", i_adapter, i_canum );
    if( (i_ca_handle = open(psz_tmp, O_RDWR | O_NONBLOCK)) < 0 )
    {
        msg_Warn( NULL, "failed opening CAM device %s (%s)",
                  psz_tmp, strerror(errno) );
        i_ca_handle = 0;
        return;
    }

    if ( ioctl( i_ca_handle, CA_GET_CAP, &caps ) != 0 )
    {
        msg_Err( NULL, "failed getting CAM capabilities (%s)",
                 strerror(errno) );
        close( i_ca_handle );
        i_ca_handle = 0;
        return;
    }

    /* Output CA capabilities */
    msg_Dbg( NULL, "CA interface with %d %s", caps.slot_num,
        caps.slot_num == 1 ? "slot" : "slots" );
    if ( caps.slot_type & CA_CI )
        msg_Dbg( NULL, "  CI high level interface type" );
    if ( caps.slot_type & CA_CI_LINK )
        msg_Dbg( NULL, "  CI link layer level interface type" );
    if ( caps.slot_type & CA_CI_PHYS )
        msg_Dbg( NULL, "  CI physical layer level interface type (not supported) " );
    if ( caps.slot_type & CA_DESCR )
        msg_Dbg( NULL, "  built-in descrambler detected" );
    if ( caps.slot_type & CA_SC )
        msg_Dbg( NULL, "  simple smart card interface" );

    msg_Dbg( NULL, "  %d available %s", caps.descr_num,
        caps.descr_num == 1 ? "descrambler (key)" : "descramblers (keys)" );
    if ( caps.descr_type & CA_ECD )
        msg_Dbg( NULL, "  ECD scrambling system supported" );
    if ( caps.descr_type & CA_NDS )
        msg_Dbg( NULL, "  NDS scrambling system supported" );
    if ( caps.descr_type & CA_DSS )
        msg_Dbg( NULL, "  DSS scrambling system supported" );

    if ( caps.slot_num == 0 )
    {
        msg_Err( NULL, "CAM module with no slots" );
        close( i_ca_handle );
        i_ca_handle = 0;
        return;
    }

    if( caps.slot_type & CA_CI_LINK )
        i_ca_type = CA_CI_LINK;
    else if( caps.slot_type & CA_CI )
        i_ca_type = CA_CI;
    else
    {
        msg_Err( NULL, "Incompatible CAM interface" );
        close( i_ca_handle );
        i_ca_handle = 0;
        return;
    }

    i_nb_slots = caps.slot_num;
    memset( p_sessions, 0, sizeof(en50221_session_t) * MAX_SESSIONS );

    if( i_ca_type & CA_CI_LINK )
    {
        ev_io_init(&cam_watcher, en50221_Read, i_ca_handle, EV_READ);
        ev_io_start(event_loop, &cam_watcher);

        ev_timer_init(&slot_watcher, en50221_Poll, CA_POLL_PERIOD / 1000000.,
                      CA_POLL_PERIOD / 1000000.);
        ev_timer_start(event_loop, &slot_watcher);
    }

    en50221_Reset();
}

/*****************************************************************************
 * en50221_Reset : Reset the CAM for en50221
 *****************************************************************************/
void en50221_Reset( void )
{
    memset( p_slots, 0, sizeof(ci_slot_t) * MAX_CI_SLOTS );

    if( i_ca_type & CA_CI_LINK )
    {
        int i_slot;
        for ( i_slot = 0; i_slot < i_nb_slots; i_slot++ )
            ResetSlot( i_slot );
    }
    else
    {
        struct ca_slot_info info;
        system_ids_t *p_ids;
        ca_msg_t ca_msg;
        info.num = 0;

        /* We don't reset the CAM in that case because it's done by the
         * ASIC. */
        if ( ioctl( i_ca_handle, CA_GET_SLOT_INFO, &info ) < 0 )
        {
            msg_Err( NULL, "en50221_Init: couldn't get slot info" );
            close( i_ca_handle );
            i_ca_handle = 0;
            return;
        }
        if( info.flags == 0 )
        {
            msg_Err( NULL, "en50221_Init: no CAM inserted" );
            close( i_ca_handle );
            i_ca_handle = 0;
            return;
        }

        /* Allocate a dummy sessions */
        p_sessions[0].i_resource_id = RI_CONDITIONAL_ACCESS_SUPPORT;
        p_sessions[0].pf_close = ConditionalAccessClose;
        if ( p_sessions[0].p_sys == NULL )
            p_sessions[0].p_sys = malloc(sizeof(system_ids_t));
        memset( p_sessions[0].p_sys, 0, sizeof(system_ids_t) );
        p_ids = (system_ids_t *)p_sessions[0].p_sys;
        p_ids->b_high_level = 1;

        /* Get application info to find out which cam we are using and make
           sure everything is ready to play */
        ca_msg.length=3;
        ca_msg.msg[0] = ( AOT_APPLICATION_INFO & 0xFF0000 ) >> 16;
        ca_msg.msg[1] = ( AOT_APPLICATION_INFO & 0x00FF00 ) >> 8;
        ca_msg.msg[2] = ( AOT_APPLICATION_INFO & 0x0000FF ) >> 0;
        memset( &ca_msg.msg[3], 0, 253 );
        APDUSend( NULL, 1, AOT_APPLICATION_INFO_ENQ, NULL, 0 );
        if ( ioctl( i_ca_handle, CA_GET_MSG, &ca_msg ) < 0 )
        {
            msg_Err( NULL, "en50221_Init: failed getting message" );
            close( i_ca_handle );
            i_ca_handle = 0;
            return;
        }

#ifdef HLCI_WAIT_CAM_READY
        while( ca_msg.msg[8] == 0xff && ca_msg.msg[9] == 0xff )
        {
            msleep(1);
            msg_Dbg( NULL, "CAM: please wait" );
            APDUSend( NULL, 1, AOT_APPLICATION_INFO_ENQ, NULL, 0 );
            ca_msg.length=3;
            ca_msg.msg[0] = ( AOT_APPLICATION_INFO & 0xFF0000 ) >> 16;
            ca_msg.msg[1] = ( AOT_APPLICATION_INFO & 0x00FF00 ) >> 8;
            ca_msg.msg[2] = ( AOT_APPLICATION_INFO & 0x0000FF ) >> 0;
            memset( &ca_msg.msg[3], 0, 253 );
            if ( ioctl( i_ca_handle, CA_GET_MSG, &ca_msg ) < 0 )
            {
                msg_Err( NULL, "en50221_Init: failed getting message" );
                close( i_ca_handle );
                i_ca_handle = 0;
                return;
            }
            msg_Dbg( NULL, "en50221_Init: Got length: %d, tag: 0x%x", ca_msg.length, APDUGetTag( ca_msg.msg, ca_msg.length ) );
        }
#else
        if( ca_msg.msg[8] == 0xff && ca_msg.msg[9] == 0xff )
        {
            msg_Err( NULL, "CAM returns garbage as application info!" );
            close( i_ca_handle );
            i_ca_handle = 0;
            return;
        }
#endif
        msg_Dbg( NULL, "found CAM %s using id 0x%x", &ca_msg.msg[12],
                 (ca_msg.msg[8]<<8)|ca_msg.msg[9] );
    }
}

/*****************************************************************************
 * en50221_Read : Read the CAM for a TPDU
 *****************************************************************************/
static void en50221_Read(struct ev_loop *loop, struct ev_io *w, int revents)
{
    TPDURecv( NULL );

    ev_timer_again(event_loop, &slot_watcher);
}

/*****************************************************************************
 * en50221_Poll : Send a poll TPDU to the CAM
 *****************************************************************************/
static void en50221_Poll(struct ev_loop *loop, struct ev_timer *w, int revents)
{
    int i_slot;
    int i_session_id;

    /* Check module status */
    for ( i_slot = 0; i_slot < i_nb_slots; i_slot++ )
    {
        ci_slot_t *p_slot = &p_slots[i_slot];
        ca_slot_info_t sinfo;

        sinfo.num = i_slot;
        if ( ioctl( i_ca_handle, CA_GET_SLOT_INFO, &sinfo ) != 0 )
        {
            msg_Err( NULL, "en50221_Poll: couldn't get info on slot %d",
                     i_slot );
            continue;
        }

        if ( !(sinfo.flags & CA_CI_MODULE_READY) )
        {
            if ( p_slot->b_active )
            {
                msg_Dbg( NULL, "en50221_Poll: slot %d has been removed",
                         i_slot );
                ResetSlot( i_slot );
            }
        }
        else if ( !p_slot->b_active )
        {
            if ( !p_slot->b_expect_answer )
                InitSlot( NULL, i_slot );
        }
    }

    /* Check if applications have data to send */
    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        en50221_session_t *p_session = &p_sessions[i_session_id - 1];
        if ( p_session->i_resource_id && p_session->pf_manage != NULL
              && !p_slots[ p_session->i_slot ].b_expect_answer )
            p_session->pf_manage( NULL, i_session_id );
    }

    /* Now send the poll command to inactive slots */
    for ( i_slot = 0; i_slot < i_nb_slots; i_slot++ )
    {
        ci_slot_t *p_slot = &p_slots[i_slot];

        if ( p_slot->b_active && !p_slot->b_expect_answer )
        {
            if ( TPDUSend( NULL, i_slot, T_DATA_LAST, NULL, 0 ) != 0 )
            {
                msg_Warn( NULL, "couldn't send TPDU, resetting slot %d",
                          i_slot );
                switch (i_print_type) {
                case PRINT_XML:
                    fprintf(print_fh,
                            "<EVENT type=\"reset\" cause=\"cam_error\" />\n");
                    break;
                case PRINT_TEXT:
                    fprintf(print_fh, "reset cause: cam_error\n");
                    break;
                default:
                    break;
                }
                ResetSlot( i_slot );
            }
        }
    }
}

/*****************************************************************************
 * en50221_AddPMT :
 *****************************************************************************/
void en50221_AddPMT( uint8_t *p_pmt )
{
    int i_session_id;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        if ( p_sessions[i_session_id - 1].i_resource_id
                == RI_CONDITIONAL_ACCESS_SUPPORT )
            CAPMTAdd( NULL, i_session_id, p_pmt );
}

/*****************************************************************************
 * en50221_UpdatePMT :
 *****************************************************************************/
void en50221_UpdatePMT( uint8_t *p_pmt )
{
    int i_session_id;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        if ( p_sessions[i_session_id - 1].i_resource_id
                == RI_CONDITIONAL_ACCESS_SUPPORT )
            CAPMTUpdate( NULL, i_session_id, p_pmt );
}

/*****************************************************************************
 * en50221_DeletePMT :
 *****************************************************************************/
void en50221_DeletePMT( uint8_t *p_pmt )
{
    int i_session_id;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        if ( p_sessions[i_session_id - 1].i_resource_id
                == RI_CONDITIONAL_ACCESS_SUPPORT )
            CAPMTDelete( NULL, i_session_id, p_pmt );
}

/*****************************************************************************
 * en50221_StatusMMI :
 *****************************************************************************/
uint8_t en50221_StatusMMI( uint8_t *p_answer, ssize_t *pi_size )
{
    struct ret_mmi_status *p_ret = (struct ret_mmi_status *)p_answer;

    if ( ioctl( i_ca_handle, CA_GET_CAP, &p_ret->caps ) != 0 )
    {
        msg_Err( NULL, "ioctl CA_GET_CAP failed (%s)", strerror(errno) );
        return RET_ERR;
    }

    *pi_size = sizeof(struct ret_mmi_status);
    return RET_MMI_STATUS;
}

/*****************************************************************************
 * en50221_StatusMMISlot :
 *****************************************************************************/
uint8_t en50221_StatusMMISlot( uint8_t *p_buffer, ssize_t i_size,
                               uint8_t *p_answer, ssize_t *pi_size )
{
    int i_slot;
    struct ret_mmi_slot_status *p_ret = (struct ret_mmi_slot_status *)p_answer;

    if ( i_size != 1 ) return RET_HUH;
    i_slot = *p_buffer;

    p_ret->sinfo.num = i_slot;
    if ( ioctl( i_ca_handle, CA_GET_SLOT_INFO, &p_ret->sinfo ) != 0 )
    {
        msg_Err( NULL, "ioctl CA_GET_SLOT_INFO failed (%s)", strerror(errno) );
        return RET_ERR;
    }

    *pi_size = sizeof(struct ret_mmi_slot_status);
    return RET_MMI_SLOT_STATUS;
}

/*****************************************************************************
 * en50221_OpenMMI :
 *****************************************************************************/
uint8_t en50221_OpenMMI( uint8_t *p_buffer, ssize_t i_size )
{
    int i_slot;

    if ( i_size != 1 ) return RET_HUH;
    i_slot = *p_buffer;

    if( i_ca_type & CA_CI_LINK )
    {
        int i_session_id;
        for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        {
            if ( p_sessions[i_session_id - 1].i_resource_id == RI_MMI
                  && p_sessions[i_session_id - 1].i_slot == i_slot )
            {
                msg_Dbg( NULL,
                         "MMI menu is already opened on slot %d (session=%d)",
                         i_slot, i_session_id );
                return RET_OK;
            }
        }

        for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        {
            if ( p_sessions[i_session_id - 1].i_resource_id
                    == RI_APPLICATION_INFORMATION
                  && p_sessions[i_session_id - 1].i_slot == i_slot )
            {
                ApplicationInformationEnterMenu( NULL, i_session_id );
                return RET_OK;
            }
        }

        msg_Err( NULL, "no application information on slot %d", i_slot );
        return RET_ERR;
    }
    else
    {
        msg_Err( NULL, "MMI menu not supported" );
        return RET_ERR;
    }
}

/*****************************************************************************
 * en50221_CloseMMI :
 *****************************************************************************/
uint8_t en50221_CloseMMI( uint8_t *p_buffer, ssize_t i_size )
{
    int i_slot;

    if ( i_size != 1 ) return RET_HUH;
    i_slot = *p_buffer;

    if( i_ca_type & CA_CI_LINK )
    {
        int i_session_id;
        for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
        {
            if ( p_sessions[i_session_id - 1].i_resource_id == RI_MMI
                  && p_sessions[i_session_id - 1].i_slot == i_slot )
            {
                MMISendClose( NULL, i_session_id );
                return RET_OK;
            }
        }

        msg_Warn( NULL, "closing a non-existing MMI session on slot %d",
                  i_slot );
        return RET_ERR;
    }
    else
    {
        msg_Err( NULL, "MMI menu not supported" );
        return RET_ERR;
    }
}

/*****************************************************************************
 * en50221_GetMMIObject :
 *****************************************************************************/
uint8_t en50221_GetMMIObject( uint8_t *p_buffer, ssize_t i_size,
                              uint8_t *p_answer, ssize_t *pi_size )
{
    int i_session_id, i_slot;
    struct ret_mmi_recv *p_ret = (struct ret_mmi_recv *)p_answer;

    if ( i_size != 1 ) return RET_HUH;
    i_slot = *p_buffer;

    if ( p_slots[i_slot].b_mmi_expected )
        return RET_MMI_WAIT; /* data not yet available */

    p_ret->object.i_object_type = EN50221_MMI_NONE;
    *pi_size = sizeof(struct ret_mmi_recv);

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        if ( p_sessions[i_session_id - 1].i_resource_id == RI_MMI
              && p_sessions[i_session_id - 1].i_slot == i_slot )
        {
            mmi_t *p_mmi =
                (mmi_t *)p_sessions[i_session_id - 1].p_sys;
            if ( p_mmi == NULL )
            {
                *pi_size = 0;
                return RET_ERR; /* should not happen */
            }

            *pi_size = COMM_BUFFER_SIZE - COMM_HEADER_SIZE -
                        ((void *)&p_ret->object - (void *)p_ret);
            if ( en50221_SerializeMMIObject( (uint8_t *)&p_ret->object,
                               pi_size, &p_mmi->last_object ) == -1 )
            {
                *pi_size = 0;
                msg_Err( NULL, "MMI structure too big" );
                return RET_ERR;
            }
            *pi_size += ((void *)&p_ret->object - (void *)p_ret);
            break;
        }
    }

    return RET_MMI_RECV;
}


/*****************************************************************************
 * en50221_SendMMIObject :
 *****************************************************************************/
uint8_t en50221_SendMMIObject( uint8_t *p_buffer, ssize_t i_size )
{
    int i_session_id, i_slot;
    struct cmd_mmi_send *p_cmd = (struct cmd_mmi_send *)p_buffer;

    if ( i_size < sizeof(struct cmd_mmi_send))
    {
        msg_Err( NULL, "command packet too short (%zd)\n", i_size );
        return RET_HUH;
    }

    if ( en50221_UnserializeMMIObject( &p_cmd->object, i_size -
                         ((void *)&p_cmd->object - (void *)p_cmd) ) == -1 )
         return RET_ERR;

    i_slot = p_cmd->i_slot;

    for ( i_session_id = 1; i_session_id <= MAX_SESSIONS; i_session_id++ )
    {
        if ( p_sessions[i_session_id - 1].i_resource_id == RI_MMI
              && p_sessions[i_session_id - 1].i_slot == i_slot )
        {
            MMISendObject( NULL, i_session_id, &p_cmd->object );
            return RET_OK;
        }
    }

    msg_Err( NULL, "SendMMIObject when no MMI session is opened !" );
    return RET_ERR;
}

#else
#include <inttypes.h>

int i_ca_handle = 0;
int i_ca_type = -1;

void en50221_AddPMT( uint8_t *p_pmt ) { };
void en50221_UpdatePMT( uint8_t *p_pmt ) { };
void en50221_DeletePMT( uint8_t *p_pmt ) { };
void en50221_Reset( void ) { };
#endif
