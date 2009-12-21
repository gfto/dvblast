/*****************************************************************************
 * demux.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2009 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Andy Gatward <a.j.gatward@reading.ac.uk>
 *          Marian Ďurkovič <md@bts.sk>
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
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dvblast.h"
#include "en50221.h"

#include <dvbpsi/demux.h>
#include <dvbpsi/pat.h>
#include <dvbpsi/eit.h>
#include <dvbpsi/sdt.h>
#include <dvbpsi/dr.h>
#include <dvbpsi/psi.h>

/*****************************************************************************
 * Local declarations
 *****************************************************************************/

#define PAT_PID        0x00
#define SDT_PID        0x11
#define EIT_PID        0x12
#define TDT_PID        0x14

typedef struct ts_pid_t
{
    int i_refcount;
    int b_pes;
    int i_last_cc;
    int i_demux_fd;

    output_t **pp_outputs;
    int i_nb_outputs;
} ts_pid_t;

typedef struct sid_t
{
    uint16_t i_sid, i_pmt_pid;
    dvbpsi_handle p_dvbpsi_handle;
    dvbpsi_pmt_t *p_current_pmt;
} sid_t;

ts_pid_t p_pids[8192];
static sid_t **pp_sids = NULL;
static int i_nb_sids = 0;

static dvbpsi_handle p_pat_dvbpsi_handle;
static dvbpsi_handle p_sdt_dvbpsi_handle;
static dvbpsi_handle p_eit_dvbpsi_handle;
static dvbpsi_pat_t *p_current_pat = NULL;
static dvbpsi_sdt_t *p_current_sdt = NULL;
static int i_demux_fd;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void demux_Handle( block_t *p_ts );
static void SetPID( uint16_t i_pid );
static void UnsetPID( uint16_t i_pid );
static void StartPID( output_t *p_output, uint16_t i_pid );
static void StopPID( output_t *p_output, uint16_t i_pid );
static void SelectPID( uint16_t i_sid, uint16_t i_pid );
static void UnselectPID( uint16_t i_sid, uint16_t i_pid );
static void SelectPSI( uint16_t i_sid, uint16_t i_pid );
static void UnselectPSI( uint16_t i_sid, uint16_t i_pid );
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids );
static int SIDIsSelected( uint16_t i_sid );
int PIDWouldBeSelected( dvbpsi_pmt_es_t *p_es );
static int PMTNeedsDescrambling( dvbpsi_pmt_t *p_pmt );
static void SendPAT( void );
static void SendSDT( void );
static void SendPMT( sid_t *p_sid );
static void NewPAT( output_t *p_output );
static void NewSDT( output_t *p_output );
static void NewPMT( output_t *p_output );
static void PATCallback( void *_unused, dvbpsi_pat_t *p_pat );
static void SDTCallback( void *_unused, dvbpsi_sdt_t *p_sdt );
static void PMTCallback( void *_unused, dvbpsi_pmt_t *p_pmt );
static void PSITableCallback( void *_unused, dvbpsi_handle h_dvbpsi,
                              uint8_t i_table_id, uint16_t i_extension );

/*****************************************************************************
 * demux_Open
 *****************************************************************************/
void demux_Open( void )
{
    int i;

    memset( p_pids, 0, sizeof(p_pids) );

    pf_Open();

    for ( i = 0; i < 8192; i++ )
    {
        p_pids[i].i_last_cc = -1;
        p_pids[i].i_demux_fd = -1;
    }

    if ( b_budget_mode )
        i_demux_fd = pf_SetFilter(8192);

    SetPID(PAT_PID); /* PAT */
    p_pat_dvbpsi_handle = dvbpsi_AttachPAT( PATCallback, NULL );

    if( b_enable_epg )
    {
        SetPID(SDT_PID); /* SDT */
        p_sdt_dvbpsi_handle = dvbpsi_AttachDemux( PSITableCallback, NULL );

        SetPID(EIT_PID); /* EIT */
        p_eit_dvbpsi_handle = dvbpsi_AttachDemux( PSITableCallback, NULL );

        SetPID(TDT_PID); /* TDT */
    }
}

/*****************************************************************************
 * demux_Run
 *****************************************************************************/
void demux_Run( void )
{
    block_t *p_ts = pf_Read();

    while ( p_ts != NULL )
    {
        block_t *p_next = p_ts->p_next;
        p_ts->p_next = NULL;
        demux_Handle( p_ts );
        p_ts = p_next;
    }
}

/*****************************************************************************
 * demux_Handle
 *****************************************************************************/
static void demux_Handle( block_t *p_ts )
{
    mtime_t i_wallclock = mdate();
    uint16_t i_pid = block_GetPID( p_ts );
    uint8_t i_cc = block_GetCC( p_ts );
    int i;

    if ( block_GetSync( p_ts ) != 0x47 )
    {
        msg_Warn( NULL, "invalid sync (0x%x)", p_ts->p_ts[0] );
        block_Delete( p_ts );
        return;
    }

    if ( i_pid != PADDING_PID && p_pids[i_pid].i_last_cc != -1
          && p_pids[i_pid].i_last_cc != i_cc /* dup */
          && (p_pids[i_pid].i_last_cc + 17 - i_cc) % 16 )
        msg_Warn( NULL, "discontinuity for PID %d", i_pid );
    p_pids[i_pid].i_last_cc = i_cc;

    if ( block_HasTransportError( p_ts ) )
        msg_Warn( NULL, "transport_error_indicator" );

    if ( p_pids[i_pid].i_refcount )
    {
        if ( i_pid == PAT_PID )
        {
            dvbpsi_PushPacket( p_pat_dvbpsi_handle, p_ts->p_ts );
            if ( block_UnitStart( p_ts ) )
                SendPAT();
        }
        else if ( b_enable_epg && i_pid == EIT_PID )
        {
            dvbpsi_PushPacket( p_eit_dvbpsi_handle, p_ts->p_ts );
        }
        else if ( b_enable_epg && i_pid == SDT_PID )
        {
            dvbpsi_PushPacket( p_sdt_dvbpsi_handle, p_ts->p_ts );
            if ( block_UnitStart( p_ts ) )
                SendSDT();
        }
        else if ( b_enable_epg && i_pid == TDT_PID )
        {
            for ( i = 0; i < i_nb_outputs; i++ )
            {
                if ( pp_outputs[i]->i_maddr && pp_outputs[i]->p_sdt_section )
                    output_Put( pp_outputs[i], p_ts );
            }
        }
        else
        {
            for ( i = 0; i < i_nb_sids; i++ )
            {
                if ( pp_sids[i]->i_sid && pp_sids[i]->i_pmt_pid == i_pid )
                {
                    dvbpsi_PushPacket( pp_sids[i]->p_dvbpsi_handle,
                                       p_ts->p_ts );
                    if ( block_UnitStart( p_ts ) )
                        SendPMT( pp_sids[i] );
                }
            }
        }
    }

    if ( block_HasPCR( p_ts ) )
    {
        mtime_t i_timestamp = block_GetPCR( p_ts );

        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid && pp_sids[i]->p_current_pmt != NULL
                  && pp_sids[i]->p_current_pmt->i_pcr_pid == i_pid )
            {
                uint16_t i_sid = pp_sids[i]->i_sid;
                int j;

                for ( j = 0; j < i_nb_outputs; j++ )
                {
                    if ( pp_outputs[j]->i_sid == i_sid )
                    {
                        pp_outputs[j]->i_ref_timestamp = i_timestamp;
                        pp_outputs[j]->i_ref_wallclock = 0;
                    }
                }
            }
        }
    }

    for ( i = 0; i < p_pids[i_pid].i_nb_outputs; i++ )
    {
        output_t *p_output = p_pids[i_pid].pp_outputs[i];
        if ( p_output != NULL )
        {
            if ( i_ca_handle && p_output->b_watch )
            {
                uint8_t *p_payload;

                if ( block_GetScrambling( p_ts ) ||
                     ( block_UnitStart( p_ts ) && p_pids[i_pid].b_pes
                        && (p_payload = block_GetPayload( p_ts )) != NULL
                        && p_payload + 3 < p_ts->p_ts + TS_SIZE
                          && (p_payload[0] != 0 || p_payload[1] != 0
                               || p_payload[2] != 1) ) )
                {
                    p_output->i_nb_errors++;
                    p_output->i_last_error = i_wallclock;
                }
                else if ( i_wallclock > p_output->i_last_error + WATCHDOG_WAIT )
                    p_output->i_nb_errors = 0;

                if ( p_output->i_nb_errors > MAX_ERRORS )
                {
                    struct in_addr s;
                    int j;
                    for ( j = 0; j < i_nb_outputs; j++ )
                        pp_outputs[j]->i_nb_errors = 0;

                    s.s_addr = p_output->i_maddr;
                    msg_Warn( NULL,
                             "too many errors for stream %s:%d, resetting",
                             inet_ntoa( s ), p_output->i_port );
                    en50221_Reset();
                }
            }

            output_Put( p_output, p_ts );
        }
    }

    if ( output_dup.i_maddr )
        output_Put( &output_dup, p_ts );

    p_ts->i_refcount--;
    if ( !p_ts->i_refcount )
        block_Delete( p_ts );
}

/*****************************************************************************
 * demux_Change : called from main thread
 *****************************************************************************/
static int IsIn( uint16_t *pi_pids, int i_nb_pids, uint16_t i_pid )
{
    int i;
    for ( i = 0; i < i_nb_pids; i++ )
        if ( i_pid == pi_pids[i] ) break;
    return ( i != i_nb_pids );
}

void demux_Change( output_t *p_output, uint16_t i_sid,
                   uint16_t *pi_pids, int i_nb_pids )
{
    int i;
    uint16_t *pi_wanted_pids, *pi_current_pids;
    int i_nb_wanted_pids, i_nb_current_pids;
    uint16_t i_old_sid = p_output->i_sid;
    int sid_change = ( i_sid != i_old_sid );
    int pid_change = 0;

    if ( i_sid == p_output->i_sid && i_nb_pids == p_output->i_nb_pids &&
         (!i_nb_pids ||
          !memcmp( p_output->pi_pids, pi_pids, i_nb_pids * sizeof(uint16_t) )) )
        return; /* No change */

    GetPIDS( &pi_wanted_pids, &i_nb_wanted_pids, i_sid, pi_pids, i_nb_pids );
    GetPIDS( &pi_current_pids, &i_nb_current_pids, p_output->i_sid,
             p_output->pi_pids, p_output->i_nb_pids );

    if ( sid_change && i_old_sid )
    {
        p_output->i_sid = i_sid;
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_old_sid )
            {
                UnsetPID( pp_sids[i]->i_pmt_pid );

                if ( i_ca_handle && !SIDIsSelected( i_old_sid )
                      && pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_DeletePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    for ( i = 0; i < i_nb_current_pids; i++ )
    {
        if ( pi_current_pids[i] != EMPTY_PID &&
             !IsIn( pi_wanted_pids, i_nb_wanted_pids, pi_current_pids[i] ) )
        {
            StopPID( p_output, pi_current_pids[i] );
            pid_change = 1;
        }
    }

    if ( sid_change &&
         i_ca_handle && i_old_sid && SIDIsSelected( i_old_sid ) )
    {
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_old_sid )
            {
                if ( pp_sids[i]->p_current_pmt != NULL 
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_UpdatePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    for ( i = 0; i < i_nb_wanted_pids; i++ )
    {
        if ( pi_wanted_pids[i] != EMPTY_PID &&
             !IsIn( pi_current_pids, i_nb_current_pids, pi_wanted_pids[i] ) )
        {
            StartPID( p_output, pi_wanted_pids[i] );
            pid_change = 1;
        }
    }

    free( pi_wanted_pids );
    free( pi_current_pids );

    if ( sid_change && i_sid )
    {
        p_output->i_sid = i_old_sid;
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_sid )
            {
                SetPID( pp_sids[i]->i_pmt_pid );

                if ( i_ca_handle && !SIDIsSelected( i_sid )
                      && pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_AddPMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    if ( i_ca_handle && i_sid && SIDIsSelected( i_sid ) )
    {
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_sid )
            {
                if ( pp_sids[i]->p_current_pmt != NULL 
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_UpdatePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    p_output->i_sid = i_sid;
    free( p_output->pi_pids );
    p_output->pi_pids = malloc( sizeof(uint16_t) * i_nb_pids );
    memcpy( p_output->pi_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
    p_output->i_nb_pids = i_nb_pids;

    if ( sid_change )
    {
        NewSDT( p_output );
        NewPAT( p_output );
        NewPMT( p_output );
    }
    else if ( pid_change )
        NewPMT( p_output );
}

/*****************************************************************************
 * SetPID/UnsetPID
 *****************************************************************************/
static void SetPID( uint16_t i_pid )
{
    p_pids[i_pid].i_refcount++;

    if ( !b_budget_mode && p_pids[i_pid].i_refcount
          && p_pids[i_pid].i_demux_fd == -1 )
        p_pids[i_pid].i_demux_fd = pf_SetFilter( i_pid );
}

static void UnsetPID( uint16_t i_pid )
{
    p_pids[i_pid].i_refcount--;

    if ( !b_budget_mode && !p_pids[i_pid].i_refcount
          && p_pids[i_pid].i_demux_fd != -1 )
    {
        pf_UnsetFilter( p_pids[i_pid].i_demux_fd, i_pid );
        p_pids[i_pid].i_demux_fd = -1;
    }
}

/*****************************************************************************
 * StartPID/StopPID
 *****************************************************************************/
static void StartPID( output_t *p_output, uint16_t i_pid )
{
    int j;

    for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
        if ( p_pids[i_pid].pp_outputs[j] == p_output )
            break;

    if ( j == p_pids[i_pid].i_nb_outputs )
    {
        for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
            if ( p_pids[i_pid].pp_outputs[j] == NULL )
                break;

        if ( j == p_pids[i_pid].i_nb_outputs )
        {
            p_pids[i_pid].i_nb_outputs++;
            p_pids[i_pid].pp_outputs = realloc( p_pids[i_pid].pp_outputs,
                                                sizeof(output_t *)
                                                * p_pids[i_pid].i_nb_outputs );
        }

        p_pids[i_pid].pp_outputs[j] = p_output;
        SetPID( i_pid );
    }
}

static void StopPID( output_t *p_output, uint16_t i_pid )
{
    int j;

    for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
    {
        if ( p_pids[i_pid].pp_outputs[j] != NULL )
        {
            if ( p_pids[i_pid].pp_outputs[j] == p_output )
                break;
        }
    }

    if ( j != p_pids[i_pid].i_nb_outputs )
    {
        p_pids[i_pid].pp_outputs[j] = NULL;
        UnsetPID( i_pid );
    }
}

/*****************************************************************************
 * SelectPID/UnselectPID
 *****************************************************************************/
static void SelectPID( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid
              && !pp_outputs[i]->i_nb_pids )
            StartPID( pp_outputs[i], i_pid );
}

static void UnselectPID( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid
              && !pp_outputs[i]->i_nb_pids )
            StopPID( pp_outputs[i], i_pid );
}

/*****************************************************************************
 * SelectPSI/UnselectPSI
 *****************************************************************************/
static void SelectPSI( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
            SetPID( i_pid );
}

static void UnselectPSI( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
            UnsetPID( i_pid );
}

/*****************************************************************************
 * GetPIDS
 *****************************************************************************/
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids )
{
    dvbpsi_pmt_t *p_pmt = NULL;
    uint16_t i_pmt_pid;
    dvbpsi_pmt_es_t *p_es;
    int i;

    if ( i_nb_pids || i_sid == 0 )
    {
        *pi_nb_wanted_pids = i_nb_pids;
        *ppi_wanted_pids = malloc( sizeof(uint16_t) * i_nb_pids );
        memcpy( *ppi_wanted_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
        return;
    }

    *pi_nb_wanted_pids = 0;
    *ppi_wanted_pids = NULL;

    for ( i = 0; i < i_nb_sids; i++ )
    {
        if ( pp_sids[i]->i_sid == i_sid )
        {
            p_pmt = pp_sids[i]->p_current_pmt;
            i_pmt_pid = pp_sids[i]->i_pmt_pid;
            break;
        }
    }

    if ( p_pmt == NULL )
        return;

    i = 0;
    for ( p_es = p_pmt->p_first_es; p_es != NULL; p_es = p_es->p_next )
        if ( PIDWouldBeSelected( p_es ) )
        {
            *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                                  (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
            (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = p_es->i_pid;
        }

    if ( p_pmt->i_pcr_pid != PADDING_PID && p_pmt->i_pcr_pid != i_pmt_pid
          && !IsIn( *ppi_wanted_pids, *pi_nb_wanted_pids, p_pmt->i_pcr_pid ) )
    {
        *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                              (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
        (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = p_pmt->i_pcr_pid;
    }
}

/*****************************************************************************
 * WritePSISection
 *****************************************************************************/
static block_t *WritePSISection( dvbpsi_psi_section_t *p_section,
                                 uint16_t i_pid, uint8_t *pi_cc )
{
    block_t *p_block, **pp_last = &p_block;
    uint32_t i_length;
    uint8_t *p_data = p_section->p_data;
    int b_first = 1;

    i_length = (uint32_t)( p_section->p_payload_end - p_section->p_data ) +
               ( p_section->b_syntax_indicator ? 4 : 0 );

    do
    {
        uint32_t i_copy = i_length > (184 - b_first) ? (184 - b_first) :
                          i_length;
        int i;
        block_t *p_ts;

        p_ts = *pp_last = block_New();
        pp_last = &p_ts->p_next;

        /* write header
         * 8b   0x47    sync byte
         * 1b           transport_error_indicator
         * 1b           payload_unit_start
         * 1b           transport_priority
         * 13b          pid
         * 2b           transport_scrambling_control
         * 2b           if adaptation_field 0x03 else 0x01
         * 4b           continuity_counter
         */

        p_ts->p_ts[0] = 0x47;
        p_ts->p_ts[1] = ( b_first ? 0x40 : 0x00 ) | ( ( i_pid >> 8 ) & 0x1f );
        p_ts->p_ts[2] = i_pid & 0xff;
        p_ts->p_ts[3] = 0x10 | *pi_cc;
        (*pi_cc)++;
        *pi_cc &= 0xf;

        if ( b_first )
            p_ts->p_ts[4] = 0; /* pointer */

        /* copy payload */
        memcpy( &p_ts->p_ts[4 + b_first], p_data, i_copy );

        /* stuffing */
        for( i = 4 + b_first + i_copy; i < 188; i++ )
            p_ts->p_ts[i] = 0xff;

        b_first = 0;
        i_length -= i_copy;
        p_data += i_copy;
    }
    while ( i_length > 0 );

    return p_block;
}

/*****************************************************************************
 * SendPAT
 *****************************************************************************/
static void SendPAT( void )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( !pp_outputs[i]->i_maddr )
            continue;

        if ( pp_outputs[i]->p_pat_section == NULL &&
             pp_outputs[i]->p_sdt_section != NULL && p_current_pat != NULL )
        {
            dvbpsi_pat_t pat;

            if ( b_unique_tsid )
                dvbpsi_InitPAT( &pat, pp_outputs[i]->i_ts_id,
                                pp_outputs[i]->i_pat_version, 1 );
            else
                dvbpsi_InitPAT( &pat, p_current_pat->i_ts_id,
                                pp_outputs[i]->i_pat_version, 1 );

            pp_outputs[i]->p_pat_section = dvbpsi_GenPATSections( &pat, 0 );
        }


        if ( pp_outputs[i]->p_pat_section != NULL )
        {
            block_t *p_block;

            p_block = WritePSISection( pp_outputs[i]->p_pat_section, PAT_PID,
                                       &pp_outputs[i]->i_pat_cc );
            while ( p_block != NULL )
            {
                block_t *p_next = p_block->p_next;
                p_block->i_refcount--;
                output_Put( pp_outputs[i], p_block );
                p_block = p_next;
            }
        }
    }
}

/*****************************************************************************
 * SendSDT
 *****************************************************************************/
static void SendSDT( void )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->p_sdt_section != NULL )
        {
            block_t *p_block;

            p_block = WritePSISection( pp_outputs[i]->p_sdt_section, SDT_PID,
                                       &pp_outputs[i]->i_sdt_cc );
            while ( p_block != NULL )
            {
                block_t *p_next = p_block->p_next;
                p_block->i_refcount--;
                output_Put( pp_outputs[i], p_block );
                p_block = p_next;
            }
        }
    }
}

/*****************************************************************************
 * SendPMT
 *****************************************************************************/
static void SendPMT( sid_t *p_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == p_sid->i_sid )
        {
            if ( pp_outputs[i]->p_pmt_section != NULL )
            {
                block_t *p_block;

                p_block = WritePSISection( pp_outputs[i]->p_pmt_section,
                                           p_sid->i_pmt_pid,
                                           &pp_outputs[i]->i_pmt_cc );
                while ( p_block != NULL )
                {
                    block_t *p_next = p_block->p_next;
                    p_block->i_refcount--;
                    output_Put( pp_outputs[i], p_block );
                    p_block = p_next;
                }
            }
        }
    }
}

/*****************************************************************************
 * SendEIT
 *****************************************************************************/
static void SendEIT( dvbpsi_psi_section_t *p_section, uint16_t i_sid,
                     uint8_t i_table_id )
{
    int i;


    for( i = 0; i < i_nb_outputs; i++ )
    {
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
        {
            block_t *p_block;

            if( b_unique_tsid )
            {
                p_section->p_data[8]  = (pp_outputs[i]->i_ts_id >> 8) & 0xff;
                p_section->p_data[9]  = pp_outputs[i]->i_ts_id & 0xff;
                dvbpsi_BuildPSISection( p_section );
            }

            p_block = WritePSISection( p_section,
                                       EIT_PID,
                                       &pp_outputs[i]->i_eit_cc );
            while ( p_block != NULL )
            {
                block_t *p_next = p_block->p_next;
                p_block->i_refcount--;
                output_Put( pp_outputs[i], p_block );
                p_block = p_next;
            }
        }
    }
}

/*****************************************************************************
 * NewPAT
 *****************************************************************************/
static void NewPAT( output_t *p_output )
{
    dvbpsi_pat_t pat;
    dvbpsi_pat_program_t *p_program;

    if ( p_output->p_pat_section != NULL )
        dvbpsi_DeletePSISections( p_output->p_pat_section );
    p_output->p_pat_section = NULL;
    p_output->i_pat_version++;

    if ( !p_output->i_sid ) return;
    if ( p_current_pat == NULL ) return;

    for( p_program = p_current_pat->p_first_program; p_program != NULL;
         p_program = p_program->p_next )
        if ( p_program->i_number == p_output->i_sid )
            break;

    if ( p_program == NULL ) return;

    if ( b_unique_tsid )
        dvbpsi_InitPAT( &pat, p_output->i_ts_id, p_output->i_pat_version, 1 );
    else
        dvbpsi_InitPAT( &pat, p_current_pat->i_ts_id, p_output->i_pat_version, 1 );

    dvbpsi_PATAddProgram( &pat, p_output->i_sid, p_program->i_pid );

    p_output->p_pat_section = dvbpsi_GenPATSections( &pat, 0 );
    dvbpsi_EmptyPAT( &pat );
}

/*****************************************************************************
 * NewSDT
 *****************************************************************************/
static void NewSDT( output_t *p_output )
{
    dvbpsi_sdt_t sdt;
    dvbpsi_sdt_service_t *p_service, *p_new_service ;
    dvbpsi_descriptor_t *p_descriptor;

    if ( p_output->p_sdt_section != NULL )
        dvbpsi_DeletePSISections( p_output->p_sdt_section );
    p_output->p_sdt_section = NULL;
    p_output->i_sdt_version++;

    if ( !p_output->i_sid ) return;
    if ( p_current_sdt == NULL ) return;

    for( p_service = p_current_sdt->p_first_service; p_service != NULL;
         p_service = p_service->p_next )
        if ( p_service->i_service_id == p_output->i_sid )
            break;

    if ( p_service == NULL )
    {
        if ( p_output->p_pat_section != NULL &&
             p_output->p_pat_section->i_length == 9 )
        {
            dvbpsi_DeletePSISections( p_output->p_pat_section );
            p_output->p_pat_section = NULL;
            p_output->i_pat_version++;
        }
        return;
    }

    if ( b_unique_tsid )
        dvbpsi_InitSDT( &sdt, p_output->i_ts_id,
                        p_output->i_sdt_version, 1,
                        p_current_sdt->i_network_id );
    else
        dvbpsi_InitSDT( &sdt, p_current_sdt->i_ts_id,
                        p_output->i_sdt_version, 1,
                        p_current_sdt->i_network_id );

    p_new_service = dvbpsi_SDTAddService( &sdt,
        p_service->i_service_id, p_service->b_eit_schedule,
        p_service->b_eit_present, p_service->i_running_status, 0 );

    for( p_descriptor = p_service->p_first_descriptor; p_descriptor != NULL;
         p_descriptor = p_descriptor->p_next )
        dvbpsi_SDTServiceAddDescriptor( p_new_service,
                  p_descriptor->i_tag, p_descriptor->i_length,
                  p_descriptor->p_data );

    p_output->p_sdt_section = dvbpsi_GenSDTSections( &sdt );

    dvbpsi_EmptySDT( &sdt );
}

/*****************************************************************************
 * NewPMT
 *****************************************************************************/
static void NewPMT( output_t *p_output )
{
    dvbpsi_pmt_t pmt, *p_current_pmt;
    dvbpsi_pmt_es_t *p_current_es;
    dvbpsi_descriptor_t *p_dr;
    int i;

    if ( p_output->p_pmt_section != NULL )
        dvbpsi_DeletePSISections( p_output->p_pmt_section );
    p_output->p_pmt_section = NULL;
    p_output->i_pmt_version++;

    if ( !p_output->i_sid ) return;

    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->i_sid == p_output->i_sid )
            break;

    if ( i == i_nb_sids )
        return;

    if ( pp_sids[i]->p_current_pmt == NULL ) return;
    p_current_pmt = pp_sids[i]->p_current_pmt;

    dvbpsi_InitPMT( &pmt, p_output->i_sid, p_output->i_pmt_version, 1,
                    p_current_pmt->i_pcr_pid );

    for ( p_dr = p_current_pmt->p_first_descriptor; p_dr != NULL;
          p_dr = p_dr->p_next )
        dvbpsi_PMTAddDescriptor( &pmt, p_dr->i_tag, p_dr->i_length,
                                 p_dr->p_data );

    for( p_current_es = p_current_pmt->p_first_es; p_current_es != NULL;
         p_current_es = p_current_es->p_next )
    {
        if ( (!p_output->i_nb_pids && PIDWouldBeSelected( p_current_es ))
              || IsIn( p_output->pi_pids, p_output->i_nb_pids,
                       p_current_es->i_pid ) )
        {
            dvbpsi_pmt_es_t *p_es = dvbpsi_PMTAddES( &pmt, p_current_es->i_type,
                                                     p_current_es->i_pid );

            for ( p_dr = p_current_es->p_first_descriptor; p_dr != NULL;
                  p_dr = p_dr->p_next )
                dvbpsi_PMTESAddDescriptor( p_es, p_dr->i_tag, p_dr->i_length,
                                           p_dr->p_data );
        }
    }

    p_output->p_pmt_section = dvbpsi_GenPMTSections( &pmt );
    dvbpsi_EmptyPMT( &pmt );
}

/*****************************************************************************
 * UpdatePAT
 *****************************************************************************/
static void UpdatePAT( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
            NewPAT( pp_outputs[i] );
}

/*****************************************************************************
 * UpdatePMT
 *****************************************************************************/
static void UpdatePMT( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
            NewPMT( pp_outputs[i] );
}

/*****************************************************************************
 * SIDIsSelected
 *****************************************************************************/
static int SIDIsSelected( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( pp_outputs[i]->i_maddr && pp_outputs[i]->i_sid == i_sid )
            return 1;

    return 0;
}

/*****************************************************************************
 * PIDIsSelected
 *****************************************************************************/
int PIDIsSelected( uint16_t i_pid )
{
    int i;

    for ( i = 0; i < p_pids[i_pid].i_nb_outputs; i++ )
        if ( p_pids[i_pid].pp_outputs[i] != NULL )
            return 1;

    return 0;
}

/*****************************************************************************
 * PIDWouldBeSelected
 *****************************************************************************/
int PIDWouldBeSelected( dvbpsi_pmt_es_t *p_es )
{
    dvbpsi_descriptor_t *p_dr;

    switch ( p_es->i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0xf: /* audio AAC */
    case 0x1b: /* video H264 */
        return 1;
        break;

    case 0x6:
        for( p_dr = p_es->p_first_descriptor; p_dr != NULL;
             p_dr = p_dr->p_next )
        {
            if( p_dr->i_tag == 0x56 /* ttx */
                 || p_dr->i_tag == 0x59 /* dvbsub */
                 || p_dr->i_tag == 0x6a /* A/52 */ )
                return 1;
        }
        break;

    default:
        break;
    }

    /* FIXME: also parse IOD */
    return 0;
}

/*****************************************************************************
 * PIDCarriesPES
 *****************************************************************************/
int PIDCarriesPES( dvbpsi_pmt_es_t *p_es )
{
    switch ( p_es->i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0x6: /* private PES data */
    case 0xf: /* audio AAC */
    case 0x1b: /* video H264 */
        return 1;
        break;

    default:
        return 0;
        break;
    }
}

/*****************************************************************************
 * PMTNeedsDescrambling
 *****************************************************************************/
static int PMTNeedsDescrambling( dvbpsi_pmt_t *p_pmt )
{
    dvbpsi_descriptor_t *p_dr;
    dvbpsi_pmt_es_t *p_es;

    for( p_dr = p_pmt->p_first_descriptor; p_dr != NULL; p_dr = p_dr->p_next )
        if( p_dr->i_tag == 0x9 )
            return 1;

    for( p_es = p_pmt->p_first_es; p_es != NULL; p_es = p_es->p_next )
        for( p_dr = p_es->p_first_descriptor; p_dr != NULL;
             p_dr = p_dr->p_next )
            if( p_dr->i_tag == 0x9 )
                return 1;

    return 0;
}

/*****************************************************************************
 * demux_ResendCAPMTs
 *****************************************************************************/
void demux_ResendCAPMTs( void )
{
    int i;
    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->p_current_pmt != NULL
              && SIDIsSelected( pp_sids[i]->i_sid )
              && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
            en50221_AddPMT( pp_sids[i]->p_current_pmt );
}

/*****************************************************************************
 * DeleteProgram
 *****************************************************************************/
static void DeleteProgram( dvbpsi_pat_program_t *p_program )
{
    int i_pmt;

    UnselectPSI( p_program->i_number, p_program->i_pid );

    for ( i_pmt = 0; i_pmt < i_nb_sids; i_pmt++ )
    {
        if ( pp_sids[i_pmt]->i_sid == p_program->i_number )
        {
            dvbpsi_pmt_t *p_pmt = pp_sids[i_pmt]->p_current_pmt;

            if ( p_pmt != NULL )
            {
                dvbpsi_pmt_es_t *p_es;

                if ( i_ca_handle
                     && SIDIsSelected( p_program->i_number )
                     && PMTNeedsDescrambling( p_pmt ) )
                    en50221_DeletePMT( p_pmt );

                if ( p_pmt->i_pcr_pid != PADDING_PID
                     && p_pmt->i_pcr_pid != pp_sids[i_pmt]->i_pmt_pid )
                    UnselectPID( p_program->i_number, p_pmt->i_pcr_pid );

                for( p_es = p_pmt->p_first_es; p_es != NULL;
                     p_es = p_es->p_next )
                {
                    if ( PIDWouldBeSelected( p_es ) )
                        UnselectPID( p_program->i_number, p_es->i_pid );
                }

                dvbpsi_DeletePMT( p_pmt );
            }
            pp_sids[i_pmt]->p_current_pmt = NULL;
            pp_sids[i_pmt]->i_sid = 0;
            pp_sids[i_pmt]->i_pmt_pid = 0;
            dvbpsi_DetachPMT( pp_sids[i_pmt]->p_dvbpsi_handle );
            break;
        }
    }
}

/*****************************************************************************
 * dvbpsi callbacks
 *****************************************************************************/
static void PATCallback( void *_unused, dvbpsi_pat_t *p_pat )
{
    dvbpsi_pat_program_t *p_program, *p_old_program;
    dvbpsi_pat_t *p_old_pat = p_current_pat;

    if( p_current_pat != NULL &&
        ( !p_pat->b_current_next ||
          p_pat->i_version == p_current_pat->i_version ) )
    {
        dvbpsi_DeletePAT( p_pat );
        return;
    }

    msg_Dbg( NULL, "new PAT ts_id=%d version=%d current_next=%d",
             p_pat->i_ts_id, p_pat->i_version, p_pat->b_current_next );
    p_current_pat = p_pat;

    for( p_program = p_pat->p_first_program; p_program != NULL;
         p_program = p_program->p_next )
    {
        int i_pmt;

        msg_Dbg( NULL, "  * number=%d pid=%d", p_program->i_number,
                 p_program->i_pid );

        if( p_program->i_number == 0 )
            continue;

        if ( p_old_pat != NULL )
        {
            for ( p_old_program = p_old_pat->p_first_program;
                  p_old_program != NULL;
                  p_old_program = p_old_program->p_next )
                if ( p_old_program->i_number == p_program->i_number )
                    break;

            if ( p_old_program != NULL &&
                 p_old_program->i_pid == p_program->i_pid )
                continue; /* No change */

            if ( p_old_program != NULL &&
                 p_old_program->i_pid != p_program->i_pid )
                DeleteProgram( p_old_program );
        }

        SelectPSI( p_program->i_number, p_program->i_pid );

        for ( i_pmt = 0; i_pmt < i_nb_sids; i_pmt++ )
            if ( pp_sids[i_pmt]->i_sid == 0 )
                break;

        if ( i_pmt == i_nb_sids )
        {
            sid_t *p_sid = malloc( sizeof(sid_t) );
            p_sid->p_current_pmt = NULL;
            i_nb_sids++;
            pp_sids = realloc( pp_sids, sizeof(sid_t *) * i_nb_sids );
            pp_sids[i_pmt] = p_sid;
        }

        pp_sids[i_pmt]->i_sid = p_program->i_number;
        pp_sids[i_pmt]->i_pmt_pid = p_program->i_pid;
        pp_sids[i_pmt]->p_dvbpsi_handle = dvbpsi_AttachPMT( p_program->i_number,
                                                            PMTCallback, NULL );

        UpdatePAT( p_program->i_number );
    }

    if ( p_old_pat != NULL )
    {
        for ( p_old_program = p_old_pat->p_first_program;
              p_old_program != NULL;
              p_old_program = p_old_program->p_next )
        {
            if( p_old_program->i_number == 0 )
                continue;

            for( p_program = p_pat->p_first_program; p_program != NULL;
                 p_program = p_program->p_next )
                if ( p_program->i_number == p_old_program->i_number )
                    break;

            if ( p_program == NULL )
            {
                msg_Dbg( NULL, "  * removed number=%d pid=%d",
                         p_old_program->i_number,
                         p_old_program->i_pid );

                DeleteProgram( p_old_program );
                UpdatePAT( p_old_program->i_number );
            }
        }

        dvbpsi_DeletePAT( p_old_pat );
    }
}

static void PMTCallback( void *_unused, dvbpsi_pmt_t *p_pmt )
{
    dvbpsi_pmt_t *p_current_pmt = NULL;
    dvbpsi_pmt_es_t *p_es, *p_current_es;
    int b_needs_descrambling = PMTNeedsDescrambling( p_pmt );
    int b_needed_descrambling = 0;
    int b_is_selected = SIDIsSelected( p_pmt->i_program_number );
    int i_pmt;

    for ( i_pmt = 0; i_pmt < i_nb_sids; i_pmt++ )
    {
        if ( pp_sids[i_pmt]->i_sid == p_pmt->i_program_number )
        {
            p_current_pmt = pp_sids[i_pmt]->p_current_pmt;
            if ( p_current_pmt != NULL )
                b_needed_descrambling = PMTNeedsDescrambling( p_current_pmt );
            break;
        }
    }

    if ( i_pmt == i_nb_sids )
    {
        msg_Err( NULL, "unknown service %d", p_pmt->i_program_number );
        dvbpsi_DeletePMT( p_pmt );
        return;
    }

    if ( p_current_pmt != NULL &&
         ( !p_pmt->b_current_next ||
           p_pmt->i_version == p_current_pmt->i_version ) )
    {
        dvbpsi_DeletePMT( p_pmt );
        return;
    }

    if ( i_ca_handle && b_is_selected &&
         !b_needs_descrambling && b_needed_descrambling )
        en50221_DeletePMT( p_current_pmt );

    msg_Dbg( NULL, "new PMT program number=%d version=%d pid_pcr=%d",
             p_pmt->i_program_number, p_pmt->i_version, p_pmt->i_pcr_pid );

    if ( p_pmt->i_pcr_pid != PADDING_PID
          && p_pmt->i_pcr_pid != pp_sids[i_pmt]->i_pmt_pid )
        SelectPID( p_pmt->i_program_number, p_pmt->i_pcr_pid );

    for( p_es = p_pmt->p_first_es; p_es != NULL; p_es = p_es->p_next )
    {
        msg_Dbg( NULL, "  * es pid=%d type=%d",
                 p_es->i_pid, p_es->i_type );

        if ( PIDWouldBeSelected( p_es ) )
            SelectPID( p_pmt->i_program_number, p_es->i_pid );
        p_pids[p_es->i_pid].b_pes = PIDCarriesPES( p_es );
    }

    if ( p_current_pmt != NULL )
    {
        if ( p_current_pmt->i_pcr_pid != p_pmt->i_pcr_pid
              && p_current_pmt->i_pcr_pid != PADDING_PID
              && p_current_pmt->i_pcr_pid != pp_sids[i_pmt]->i_pmt_pid )
            UnselectPID( p_pmt->i_program_number, p_current_pmt->i_pcr_pid );

        for( p_current_es = p_current_pmt->p_first_es; p_current_es != NULL;
             p_current_es = p_current_es->p_next )
        {
            if ( PIDWouldBeSelected( p_current_es ) )
            {
                for( p_es = p_pmt->p_first_es; p_es != NULL;
                     p_es = p_es->p_next )
                    if ( p_es->i_pid == p_current_es->i_pid )
                        break;

                if ( p_es == NULL )
                {
                    msg_Dbg( NULL, "  * removed es pid=%d type=%d",
                             p_current_es->i_pid, p_current_es->i_type );
                    UnselectPID( p_pmt->i_program_number, p_current_es->i_pid );
                }
            }
        }

        dvbpsi_DeletePMT( p_current_pmt );
    }

    pp_sids[i_pmt]->p_current_pmt = p_pmt;

    if ( i_ca_handle && b_is_selected )
    {
        if ( b_needs_descrambling && !b_needed_descrambling )
            en50221_AddPMT( p_pmt );
        else if ( b_needs_descrambling && b_needed_descrambling )
            en50221_UpdatePMT( p_pmt );
    }

    UpdatePMT( p_pmt->i_program_number );
}

static void PSITableCallback( void *_unused, dvbpsi_handle h_dvbpsi,
                              uint8_t i_table_id, uint16_t i_extension )
{
    /* EIT tables */

    if ( i_table_id == 0x4e || ( i_table_id >= 0x50 && i_table_id <= 0x5f ) )
    {
        SendEIT( h_dvbpsi->p_current_section, i_extension, i_table_id );
    }

    /* SDT tables */

    if ( i_table_id == 0x42 )
    {
        dvbpsi_AttachSDT( h_dvbpsi, i_table_id, i_extension, SDTCallback,
                          NULL );
    }
}

static void SDTCallback( void *_unused, dvbpsi_sdt_t *p_sdt )
{
    dvbpsi_sdt_t *p_old_sdt = p_current_sdt;
    dvbpsi_sdt_service_t *p_srv;
    dvbpsi_descriptor_t *p_dr;
    int i;

    if( p_current_sdt != NULL &&
        ( !p_sdt->b_current_next ||
          p_sdt->i_version == p_current_sdt->i_version ) )
    {
        dvbpsi_DeleteSDT( p_sdt );
        return;
    }

    msg_Dbg( NULL, "new SDT ts_id=%d version=%d current_next=%d "
             "network_id=%d",
             p_sdt->i_ts_id, p_sdt->i_version, p_sdt->b_current_next,
             p_sdt->i_network_id );

    for( p_srv = p_sdt->p_first_service; p_srv; p_srv = p_srv->p_next )
    {
        msg_Dbg( NULL, "  * service id=%d eit schedule=%d present=%d "
                 "running=%d free_ca=%d",
                 p_srv->i_service_id, p_srv->b_eit_schedule,
                 p_srv->b_eit_present, p_srv->i_running_status,
                 p_srv->b_free_ca );

        for( p_dr = p_srv->p_first_descriptor; p_dr; p_dr = p_dr->p_next )
        {
            if( p_dr->i_tag == 0x48 )
            {
                 dvbpsi_service_dr_t *pD = dvbpsi_DecodeServiceDr( p_dr );
                 char str1[256];
                 char str2[256];

                 memcpy( str1, pD->i_service_provider_name,
                         pD->i_service_provider_name_length );
                 str1[pD->i_service_provider_name_length] = '\0';
                 memcpy( str2, pD->i_service_name, pD->i_service_name_length );
                 str2[pD->i_service_name_length] = '\0';
 
                 msg_Dbg( NULL, "    - type=%d provider=%s name=%s",
                          pD->i_service_type, str1, str2 );
            }
        }
    }

    p_current_sdt = p_sdt;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( pp_outputs[i]->i_maddr ) 
            NewSDT( pp_outputs[i] );
    }

    if ( p_old_sdt != NULL )
        dvbpsi_DeleteSDT( p_old_sdt );
}
