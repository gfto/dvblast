/*****************************************************************************
 * asi-deltacast.c: support for Deltacast ASI cards
 *****************************************************************************
 * Copyright (C) 2004, 2009, 2015 VideoLAN
 *
 * Authors: Simon Lockhart <simon@slimey.org>
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

#ifdef HAVE_ASI_DELTACAST_SUPPORT

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

#include <StreamMaster.h>
// #include <DeltacastErrors.h>

#include "asi-deltacast.h"

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define ASI_DELTACAST_PERIOD 1000 /* ms */
#define ASI_DELTACAST_LOCK_TIMEOUT 5000000 /* 5 s */

static HANDLE h_board, h_channel;
static struct ev_timer asi_watcher, mute_watcher;
static bool b_sync = false;

static UCHAR *p_asibuf = NULL;
static ULONG i_asibuf_len;

/*****************************************************************************
 * asi_deltacast_Open
 *****************************************************************************/
void asi_deltacast_Open( void )
{
    ASICHANNELCONFIG RXConfig;
    ULONG ApiVersion, DrvVersion, NbBoards;
    ASIBOARDINFOEX BoardInfoEx;
    BOOL res;

    /* Get API information */
    res = Asi_GetApiInfo(&ApiVersion,&DrvVersion,&NbBoards);
    if (!res)
    {
        msg_Err( NULL, "couldn't get Deltacast API Info: 0x%08lX",
                 (unsigned long) Dc_GetLastError(NULL) );
        exit(EXIT_FAILURE);
    }

    msg_Dbg( NULL, "Deltacast StreamMaster DLL v%d.%d",
             (int)ApiVersion>>16,
             (int)ApiVersion&0xFFFF);
    msg_Dbg( NULL, "Deltacast Driver v%d.%d.%d",
             (DrvVersion >> 24) & 0xFF,
             (DrvVersion >> 16) & 0xFF,
             (DrvVersion >>  8) & 0xFF);
    msg_Dbg( NULL, "Deltacast Board Count: %d", NbBoards);

    /* Get the board information */
    res = Asi_GetBoardInfoEx((int)(i_asi_adapter / 100), &BoardInfoEx);
    if (!res)
    {
        msg_Err( NULL, "couldn't get Deltacast board Info: 0x%08lX",
				(unsigned long) Dc_GetLastError(NULL) );
        exit(EXIT_FAILURE);
    }

    msg_Dbg( NULL, "Deltacast Board FPGA v%08lX",
             (unsigned long)BoardInfoEx.BaseInformation.FPGAVersion);
    msg_Dbg( NULL, "Deltacast Board PLD v%08lX",
             (unsigned long)BoardInfoEx.BaseInformation.PLDVersion);
    msg_Dbg( NULL, "Deltacast Board PLX v%08lX",
             (unsigned long)BoardInfoEx.BaseInformation.PLXRevision);
    msg_Dbg( NULL, "Deltacast Board Serial %08X%08X",
             (ULONG)(BoardInfoEx.BaseInformation.SerialNb>>32),
             (ULONG)(BoardInfoEx.BaseInformation.SerialNb&0xFFFFFFFF));
    msg_Dbg( NULL, "Deltacast Board Input Count: %d", BoardInfoEx.NbRxChannels_i);
    msg_Dbg( NULL, "Deltacast Board Output Count: %d", BoardInfoEx.NbTxChannels_i);

    /* Open the board */
    h_board = Asi_SetupBoard((int)(i_asi_adapter / 100));
    if (h_board == NULL)
    {
        msg_Err( NULL, "couldn't setup deltacast board %d: 0x%08lX",
                 (int)(i_asi_adapter / 100), (unsigned long) Dc_GetLastError(NULL) );
        exit(EXIT_FAILURE);
    }

    /* Open the channel on the board */
    memset(&RXConfig,0,sizeof(ASICHANNELCONFIG));
    RXConfig.DriverBufferSize  = 188 * 2400; /* Use a driver buffer size of 2400 packets */
    RXConfig.BoardBufferSize   = 1048576;                /* Use 1MB of onboard buffer */
    RXConfig.TSPacketType      = ASI_PKT_188;             /* Use 188-bytes TS packets */
    RXConfig.NbPIDFilter       = 0;                               /* No PID filtering */
    RXConfig.pPIDFilterTable   = NULL;                            /* No PID filtering */
    RXConfig.BitrateIntPeriod  = ASI_INTPER_100;           /* Integrate over 100 msec */
    RXConfig.AllowedBRDeviation= 10000000;               /* Allow 10Mbps of deviation */
    RXConfig.RXTimeStamp       = FALSE;                            /* No timestamping */

    /* Try to open RX0 channel on the board 0 */
    h_channel = Asi_OpenChannel(h_board, ASI_CHN_RX0 + (i_asi_adapter % 100), &RXConfig);
    if (h_channel == NULL)
    {
        msg_Err( NULL, "couldn't setup deltacast channel %d: 0x%08lX",
                 (i_asi_adapter % 100), (unsigned long) Dc_GetLastError(NULL) );
        exit(EXIT_FAILURE);
    }

    ev_timer_init(&asi_watcher, asi_deltacast_Read,
                  ASI_DELTACAST_PERIOD / 1000000.,
                  ASI_DELTACAST_PERIOD / 1000000.);
    ev_timer_start(event_loop, &asi_watcher);

    ev_timer_init(&mute_watcher, asi_deltacast_MuteCb,
                  ASI_DELTACAST_LOCK_TIMEOUT / 1000000.,
                  ASI_DELTACAST_LOCK_TIMEOUT / 1000000.);
}

/*****************************************************************************
 * ASI deltacast events
 *****************************************************************************/
static void asi_deltacast_Read(struct ev_loop *loop, struct ev_io *w, int revents)
{
    BOOL res;
    block_t *p_ts, **pp_current = &p_ts;
    int i;
    ULONG Err;

    res = Asi_GetInputBuffer(h_channel, &p_asibuf, &i_asibuf_len,
          ASI_DELTACAST_PERIOD);
    if (!res)
    {
        Err = Dc_GetLastError(NULL) & ~DC_ERRORCODE_MASK;

        if (Err != DCERR_TIMEOUT)
        {
          msg_Warn( NULL, "asi_deltacast_Read(): GetInputBuffer failed: 0x%08X!", Err);
        }
        return;
    }

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

    for (i = 0; i < i_asibuf_len / TS_SIZE; i++)
    {
        *pp_current = block_New();
        memcpy((*pp_current)->p_ts, p_asibuf + (i * TS_SIZE), TS_SIZE);
        pp_current = &(*pp_current)->p_next;

    }

    res = Asi_ReleaseInputBuffer(h_channel);

//    msg_Warn( NULL, "asi_deltacast_Read(): returning %d blocks", i_asibuf_len / TS_SIZE );

    demux_Run( p_ts );
}

static void asi_deltacast_MuteCb(struct ev_loop *loop, struct ev_timer *w, int revents)
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
 * asi_deltacast_SetFilter
 *****************************************************************************/
int asi_deltacast_SetFilter( uint16_t i_pid )
{
    /* TODO: Support PID filtering */
    msg_Warn( NULL, "asi_deltacast_SetFilter(%d) not yet implemented", i_pid );
    return -1;
}

/*****************************************************************************
 * asi_deltacast_UnsetFilter: normally never called
 *****************************************************************************/
void asi_deltacast_UnsetFilter( int i_fd, uint16_t i_pid )
{
    /* TODO: Support PID filtering */
    msg_Warn( NULL, "asi_deltacast_UnsetFilter() not yet implemented" );
}

/*****************************************************************************
 * asi_deltacast_Reset
 *****************************************************************************/
void asi_deltacast_Reset( void )
{
    /* Called when retune required, so nothing required */
    msg_Warn( NULL, "asi_deltacast_Reset() do nothing" );
}

#endif
