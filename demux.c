/*****************************************************************************
 * demux.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011, 2015-2017 VideoLAN
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
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>

#include "dvblast.h"
#include "en50221.h"
#include "mrtg-cnt.h"

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/si.h>
#include <bitstream/dvb/si_print.h>
#include <bitstream/mpeg/psi_print.h>

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define MIN_SECTION_FRAGMENT    PSI_HEADER_SIZE_SYNTAX1

typedef struct ts_pid_t
{
    int i_refcount;
    int i_psi_refcount;
    bool b_pes;
    int8_t i_last_cc;
    int i_demux_fd;
    /* b_emm is set to true when PID carries EMM packet
       and should be outputed in all services */
    bool b_emm;

    /* PID info and stats */
    mtime_t i_bytes_ts;
    unsigned long i_packets_passed;
    ts_pid_info_t info;

    /* biTStream PSI section gathering */
    uint8_t *p_psi_buffer;
    uint16_t i_psi_buffer_used;

    output_t **pp_outputs;
    int i_nb_outputs;

    int i_pes_status; /* pes + unscrambled */
    struct ev_timer timeout_watcher;
} ts_pid_t;

struct eit_sections {
    PSI_TABLE_DECLARE(data);
};

/* EIT is carried in several separate tables, we need to track each table
   separately, otherwise one table overwrites sections of another table */
#define MAX_EIT_TABLES ( EIT_TABLE_ID_SCHED_ACTUAL_LAST - EIT_TABLE_ID_PF_ACTUAL )

typedef struct sid_t
{
    uint16_t i_sid, i_pmt_pid;
    uint8_t *p_current_pmt;
    struct eit_sections eit_table[MAX_EIT_TABLES];
} sid_t;

mtime_t i_wallclock = 0;

static ts_pid_t p_pids[MAX_PIDS];
static sid_t **pp_sids = NULL;
static int i_nb_sids = 0;

static PSI_TABLE_DECLARE(pp_current_pat_sections);
static PSI_TABLE_DECLARE(pp_next_pat_sections);
static PSI_TABLE_DECLARE(pp_current_cat_sections);
static PSI_TABLE_DECLARE(pp_next_cat_sections);
static PSI_TABLE_DECLARE(pp_current_nit_sections);
static PSI_TABLE_DECLARE(pp_next_nit_sections);
static PSI_TABLE_DECLARE(pp_current_sdt_sections);
static PSI_TABLE_DECLARE(pp_next_sdt_sections);
static mtime_t i_last_dts = -1;
static int i_demux_fd;
static uint64_t i_nb_packets = 0;
static uint64_t i_nb_invalids = 0;
static uint64_t i_nb_discontinuities = 0;
static uint64_t i_nb_errors = 0;
static int i_tuner_errors = 0;
static mtime_t i_last_error = 0;
static mtime_t i_last_reset = 0;
static struct ev_timer print_watcher;

#ifdef HAVE_ICONV
static iconv_t iconv_handle = (iconv_t)-1;
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void demux_Handle( block_t *p_ts );
static void SetDTS( block_t *p_list );
static void SetPID( uint16_t i_pid );
static void SetPID_EMM( uint16_t i_pid );
static void UnsetPID( uint16_t i_pid );
static void StartPID( output_t *p_output, uint16_t i_pid );
static void StopPID( output_t *p_output, uint16_t i_pid );
static void SelectPID( uint16_t i_sid, uint16_t i_pid, bool b_pcr );
static void UnselectPID( uint16_t i_sid, uint16_t i_pid );
static void SelectPMT( uint16_t i_sid, uint16_t i_pid );
static void UnselectPMT( uint16_t i_sid, uint16_t i_pid );
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t *pi_pcr_pid, uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids );
static bool SIDIsSelected( uint16_t i_sid );
static bool PIDWouldBeSelected( uint8_t *p_es );
static bool PMTNeedsDescrambling( uint8_t *p_pmt );
static void FlushEIT( output_t *p_output, mtime_t i_dts );
static void SendTDT( block_t *p_ts );
static void SendEMM( block_t *p_ts );
static void NewPAT( output_t *p_output );
static void NewPMT( output_t *p_output );
static void NewNIT( output_t *p_output );
static void NewSDT( output_t *p_output );
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts );
static const char *get_pid_desc(uint16_t i_pid, uint16_t *i_sid);

/*
 * Remap an ES pid to a fixed value.
 * Multiple streams of the same type use sequential pids
 * Returns the new pid and updates the map tables
*/
static uint16_t map_es_pid(output_t * p_output, uint8_t *p_es, uint16_t i_pid)
{
    uint16_t i_newpid = i_pid;
    uint16_t i_stream_type = pmtn_get_streamtype(p_es);

    if ( !b_do_remap && !p_output->config.b_do_remap )
        return i_pid;

    msg_Dbg(NULL, "REMAP: Found elementary stream type 0x%02x with original PID 0x%x (%u):", i_stream_type, i_pid, i_pid);

    switch ( i_stream_type )
    {
        case 0x03: /* audio MPEG-1 */
        case 0x04: /* audio MPEG-2 */
        case 0x0f: /* audio AAC ADTS */
        case 0x11: /* audio AAC LATM */
        case 0x81: /* ATSC AC-3 */
        case 0x87: /* ATSC Enhanced AC-3 */
            if ( b_do_remap )
                i_newpid = pi_newpids[I_APID];
            else
                i_newpid = p_output->config.pi_confpids[I_APID];
            break;
        case 0x01: /* video MPEG-1 */
        case 0x02: /* video MPEG-2 */
        case 0x10: /* video MPEG-4 */
        case 0x1b: /* video H264 */
        case 0x24: /* video H265 */
        case 0x42: /* video AVS */
            if ( b_do_remap )
                i_newpid = pi_newpids[I_VPID];
            else
                i_newpid = p_output->config.pi_confpids[I_VPID];
            break;
        case 0x06: { /* PES Private Data - We must check the descriptors */
            /* By default, nothing identified */
            uint8_t SubStreamType = N_MAP_PIDS;
            uint16_t j = 0;
            const uint8_t *p_desc;
            /* Loop over the descriptors */
            while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), j )) != NULL )
            {
                /* Get the descriptor tag */
                uint8_t i_tag = desc_get_tag( p_desc );
                j++;
                /* Check if the tag is: A/52, Enhanced A/52, DTS, AAC */
                if (i_tag == 0x6a || i_tag == 0x7a || i_tag == 0x7b || i_tag == 0x7c)
                    SubStreamType=I_APID;
                /* Check if the tag is: VBI + teletext, teletext, dvbsub */
                if (i_tag == 0x46 || i_tag == 0x56 || i_tag == 0x59)
                    SubStreamType=I_SPUPID;
            }
            /* Audio found */
            if (SubStreamType==I_APID) {
                msg_Dbg(NULL, "REMAP: PES Private Data stream identified as [Audio]");
                if ( b_do_remap )
                    i_newpid = pi_newpids[I_APID];
                else
                    i_newpid = p_output->config.pi_confpids[I_APID];
            }
            /* Subtitle found */
            if (SubStreamType==I_SPUPID) {
                msg_Dbg(NULL, "REMAP: PES Private Data stream identified as [Subtitle]");
                if ( b_do_remap )
                    i_newpid = pi_newpids[I_SPUPID];
                else
                    i_newpid = p_output->config.pi_confpids[I_SPUPID];
            }
            break;
        }
    }

    if (!i_newpid)
        return i_pid;

    /* Got the new base for the mapped pid. Find the next free one
       we do this to ensure that multiple audios get unique pids */
    while (p_output->pi_freepids[i_newpid] != UNUSED_PID)
        i_newpid++;
    p_output->pi_freepids[i_newpid] = i_pid;  /* Mark as in use */
    p_output->pi_newpids[i_pid] = i_newpid;   /* Save the new pid */

    msg_Dbg(NULL, "REMAP: => Elementary stream is remapped to PID 0x%x (%u)", i_newpid, i_newpid);

    return i_newpid;
}

/*****************************************************************************
 * FindSID
 *****************************************************************************/
static inline sid_t *FindSID( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_sids; i++ )
    {
        sid_t *p_sid = pp_sids[i];
        if ( p_sid->i_sid == i_sid )
            return p_sid;
    }
    return NULL;
}

/*****************************************************************************
 * Print info
 *****************************************************************************/
static void PrintCb( struct ev_loop *loop, struct ev_timer *w, int revents )
{
    uint64_t i_bitrate = i_nb_packets * TS_SIZE * 8 * 1000000 / i_print_period;
    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"bitrate\" status=\"%d\" value=\"%"PRIu64"\" />\n",
                    i_bitrate ? 1 : 0, i_bitrate);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "bitrate: %"PRIu64"\n", i_bitrate);
            break;
        default:
            break;
    }
    i_nb_packets = 0;

    if ( i_nb_invalids )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"invalid_ts\" number=\"%"PRIu64"\" />\n",
                        i_nb_invalids);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "invalids: %"PRIu64"\n", i_nb_invalids);
                break;
            default:
                break;
        }
        i_nb_invalids = 0;
    }

    if ( i_nb_discontinuities )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"invalid_discontinuity\" number=\"%"PRIu64"\" />\n",
                        i_nb_discontinuities);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "discontinuities: %"PRIu64"\n",
                        i_nb_discontinuities);
                break;
            default:
                break;
        }
        i_nb_discontinuities = 0;
    }

    if ( i_nb_errors )
    {
        switch (i_print_type)
        {
            case PRINT_XML:
                fprintf(print_fh,
                        "<ERROR type=\"transport_error\" number=\"%"PRIu64"\" />\n",
                        i_nb_errors);
                break;
            case PRINT_TEXT:
                fprintf(print_fh, "errors: %"PRIu64"\n", i_nb_errors);
                break;
            default:
                break;
        }
        i_nb_errors = 0;
    }
}

static void PrintESCb( struct ev_loop *loop, struct ev_timer *w, int revents )
{
    ts_pid_t *p_pid = container_of( w, ts_pid_t, timeout_watcher );
    uint16_t i_pid = p_pid - p_pids;

    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"pid\" pid=\"%"PRIu16"\" status=\"0\" />\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "pid: %"PRIu16" down\n", i_pid);
            break;
        default:
            break;
    }

    ev_timer_stop( loop, w );
    p_pid->i_pes_status = -1;
}

static void PrintES( uint16_t i_pid )
{
    const ts_pid_t *p_pid = &p_pids[i_pid];

    switch (i_print_type)
    {
        case PRINT_XML:
            fprintf(print_fh,
                    "<STATUS type=\"pid\" pid=\"%"PRIu16"\" status=\"1\" pes=\"%d\" />\n",
                    i_pid, p_pid->i_pes_status == 1 ? 1 : 0);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "pid: %"PRIu16" up%s\n",
                    i_pid, p_pid->i_pes_status == 1 ? " pes" : "");
            break;
        default:
            break;
    }
}

/*****************************************************************************
 * demux_Open
 *****************************************************************************/
void demux_Open( void )
{
    int i;

    memset( p_pids, 0, sizeof(p_pids) );

    pf_Open();

    for ( i = 0; i < MAX_PIDS; i++ )
    {
        p_pids[i].i_last_cc = -1;
        p_pids[i].i_demux_fd = -1;
        psi_assemble_init( &p_pids[i].p_psi_buffer,
                           &p_pids[i].i_psi_buffer_used );
        p_pids[i].i_pes_status = -1;
    }

    if ( b_budget_mode )
        i_demux_fd = pf_SetFilter(8192);

    psi_table_init( pp_current_pat_sections );
    psi_table_init( pp_next_pat_sections );
    SetPID(PAT_PID);
    p_pids[PAT_PID].i_psi_refcount++;

    if ( b_enable_emm )
    {
        psi_table_init( pp_current_cat_sections );
        psi_table_init( pp_next_cat_sections );
        SetPID_EMM(CAT_PID);
        p_pids[CAT_PID].i_psi_refcount++;
    }

    SetPID(NIT_PID);
    p_pids[NIT_PID].i_psi_refcount++;

    psi_table_init( pp_current_sdt_sections );
    psi_table_init( pp_next_sdt_sections );
    SetPID(SDT_PID);
    p_pids[SDT_PID].i_psi_refcount++;

    SetPID(EIT_PID);
    p_pids[EIT_PID].i_psi_refcount++;

    SetPID(RST_PID);

    SetPID(TDT_PID);

    if ( i_print_period )
    {
        ev_timer_init( &print_watcher, PrintCb,
                       i_print_period / 1000000., i_print_period / 1000000. );
        ev_timer_start( event_loop, &print_watcher );
    }
}

/*****************************************************************************
 * demux_Close
 *****************************************************************************/
void demux_Close( void )
{
    int i, r;

    psi_table_free( pp_current_pat_sections );
    psi_table_free( pp_next_pat_sections );
    psi_table_free( pp_current_cat_sections );
    psi_table_free( pp_next_cat_sections );
    psi_table_free( pp_current_nit_sections );
    psi_table_free( pp_next_nit_sections );
    psi_table_free( pp_current_sdt_sections );
    psi_table_free( pp_next_sdt_sections );

    for ( i = 0; i < MAX_PIDS; i++ )
    {
        ev_timer_stop( event_loop, &p_pids[i].timeout_watcher );
        free( p_pids[i].p_psi_buffer );
        free( p_pids[i].pp_outputs );
    }

    for ( i = 0; i < i_nb_sids; i++ )
    {
        sid_t *p_sid = pp_sids[i];
        for ( r = 0; r < MAX_EIT_TABLES; r++ ) {
            psi_table_free( p_sid->eit_table[r].data );
        }
        free( p_sid->p_current_pmt );
        free( p_sid );
    }
    free( pp_sids );

#ifdef HAVE_ICONV
    if (iconv_handle != (iconv_t)-1) {
        iconv_close(iconv_handle);
        iconv_handle = (iconv_t)-1;
    }
#endif

    if ( i_print_period )
        ev_timer_stop( event_loop, &print_watcher );
}

/*****************************************************************************
 * demux_Run
 *****************************************************************************/
void demux_Run( block_t *p_ts )
{
    i_wallclock = mdate();
    mrtgAnalyse( p_ts );
    SetDTS( p_ts );

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
    uint16_t i_pid = ts_get_pid( p_ts->p_ts );
    ts_pid_t *p_pid = &p_pids[i_pid];
    uint8_t i_cc = ts_get_cc( p_ts->p_ts );
    int i;

    i_nb_packets++;

    if ( !ts_validate( p_ts->p_ts ) )
    {
        msg_Warn( NULL, "lost TS sync" );
        block_Delete( p_ts );
        i_nb_invalids++;
        return;
    }

    if ( i_pid != PADDING_PID )
        p_pid->info.i_scrambling = ts_get_scrambling( p_ts->p_ts );

    p_pid->info.i_last_packet_ts = i_wallclock;
    p_pid->info.i_packets++;

    p_pid->i_packets_passed++;

    /* Calculate bytes_per_sec */
    if ( i_wallclock > p_pid->i_bytes_ts + 1000000 ) {
        p_pid->info.i_bytes_per_sec = p_pid->i_packets_passed * TS_SIZE;
        p_pid->i_packets_passed = 0;
        p_pid->i_bytes_ts = i_wallclock;
    }

    if ( p_pid->info.i_first_packet_ts == 0 )
        p_pid->info.i_first_packet_ts = i_wallclock;

    if ( i_pid != PADDING_PID && p_pid->i_last_cc != -1
          && !ts_check_duplicate( i_cc, p_pid->i_last_cc )
          && ts_check_discontinuity( i_cc, p_pid->i_last_cc ) )
    {
        unsigned int expected_cc = (p_pid->i_last_cc + 1) & 0x0f;
        uint16_t i_sid = 0;
        const char *pid_desc = get_pid_desc(i_pid, &i_sid);

        p_pid->info.i_cc_errors++;
        i_nb_discontinuities++;

        msg_Warn( NULL, "TS discontinuity on pid %4hu expected_cc %2u got %2u (%s, sid %d)",
                i_pid, expected_cc, i_cc, pid_desc, i_sid );
    }

    if ( ts_get_transporterror( p_ts->p_ts ) )
    {
        uint16_t i_sid = 0;
        const char *pid_desc = get_pid_desc(i_pid, &i_sid);

        p_pid->info.i_transport_errors++;

        msg_Warn( NULL, "transport_error_indicator on pid %hu (%s, sid %u)",
                   i_pid, pid_desc, i_sid );

        i_nb_errors++;
        i_tuner_errors++;
        i_last_error = i_wallclock;
    }
    else if ( i_wallclock > i_last_error + WATCHDOG_WAIT )
        i_tuner_errors = 0;

    if ( i_tuner_errors > MAX_ERRORS )
    {
        i_tuner_errors = 0;
        msg_Warn( NULL,
                 "too many transport errors, tuning again" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<EVENT type=\"reset\" cause=\"transport\" />\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "reset cause: transport\n");
            break;
        default:
            break;
        }
        pf_Reset();
    }

    if ( i_es_timeout )
    {
        int i_pes_status = -1;
        if ( ts_get_scrambling( p_ts->p_ts ) )
            i_pes_status = 0;
        else if ( ts_get_unitstart( p_ts->p_ts ) )
        {
            uint8_t *p_payload = ts_payload( p_ts->p_ts );
            if ( p_payload + 3 < p_ts->p_ts + TS_SIZE )
                i_pes_status = pes_validate( p_payload ) ? 1 : 0;
        }

        if ( i_pes_status != -1 )
        {
            if ( p_pid->i_pes_status == -1 )
            {
                p_pid->i_pes_status = i_pes_status;
                PrintES( i_pid );

                if ( i_pid != TDT_PID )
                {
                    ev_timer_init( &p_pid->timeout_watcher, PrintESCb,
                                   i_es_timeout / 1000000.,
                                   i_es_timeout / 1000000. );
                    ev_timer_start( event_loop, &p_pid->timeout_watcher );
                }
                else
                {
                    ev_timer_init( &p_pid->timeout_watcher, PrintESCb, 30, 30 );
                    ev_timer_start( event_loop, &p_pid->timeout_watcher );
                }
            }
            else
            {
                if ( p_pid->i_pes_status != i_pes_status )
                {
                    p_pid->i_pes_status = i_pes_status;
                    PrintES( i_pid );
                }

                ev_timer_again( event_loop, &p_pid->timeout_watcher );
            }
        }
    }

    if ( !ts_get_transporterror( p_ts->p_ts ) )
    {
        /* PSI parsing */
        if ( i_pid == TDT_PID || i_pid == RST_PID )
            SendTDT( p_ts );
        else if ( p_pid->i_psi_refcount )
            HandlePSIPacket( p_ts->p_ts, p_ts->i_dts );

        if ( b_enable_emm && p_pid->b_emm )
            SendEMM( p_ts );
    }

    p_pid->i_last_cc = i_cc;

    /* Output */
    for ( i = 0; i < p_pid->i_nb_outputs; i++ )
    {
        output_t *p_output = p_pid->pp_outputs[i];
        if ( p_output != NULL )
        {
            if ( i_ca_handle && (p_output->config.i_config & OUTPUT_WATCH) &&
                 ts_get_unitstart( p_ts->p_ts ) )
            {
                uint8_t *p_payload;

                if ( ts_get_scrambling( p_ts->p_ts ) ||
                     ( p_pid->b_pes
                        && (p_payload = ts_payload( p_ts->p_ts )) + 3
                             < p_ts->p_ts + TS_SIZE
                          && !pes_validate(p_payload) ) )
                {
                    if ( i_wallclock >
                            i_last_reset + WATCHDOG_REFRACTORY_PERIOD )
                    {
                        p_output->i_nb_errors++;
                        p_output->i_last_error = i_wallclock;
                    }
                }
                else if ( i_wallclock > p_output->i_last_error + WATCHDOG_WAIT )
                    p_output->i_nb_errors = 0;

                if ( p_output->i_nb_errors > MAX_ERRORS )
                {
                    int j;
                    for ( j = 0; j < i_nb_outputs; j++ )
                        pp_outputs[j]->i_nb_errors = 0;

                    msg_Warn( NULL,
                             "too many errors for stream %s, resetting",
                             p_output->config.psz_displayname );

                    switch (i_print_type) {
                    case PRINT_XML:
                        fprintf(print_fh, "<EVENT type=\"reset\" cause=\"scrambling\" />\n");
                        break;
                    case PRINT_TEXT:
                        fprintf(print_fh, "reset cause: scrambling");
                        break;
                    default:
                        break;
                    }
                    i_last_reset = i_wallclock;
                    en50221_Reset();
                }
            }

            if ( p_output->i_pcr_pid != i_pid
                  || (ts_has_adaptation(p_ts->p_ts)
                       && ts_get_adaptation(p_ts->p_ts)
                       && tsaf_has_pcr(p_ts->p_ts)) )
                output_Put( p_output, p_ts );

            if ( p_output->p_eit_ts_buffer != NULL
                  && p_ts->i_dts > p_output->p_eit_ts_buffer->i_dts
                                    + MAX_EIT_RETENTION )
                FlushEIT( p_output, p_ts->i_dts );
        }
    }

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( !(p_output->config.i_config & OUTPUT_VALID) ||
             !p_output->config.b_passthrough )
            continue;

        output_Put( p_output, p_ts );
    }

    if ( output_dup.config.i_config & OUTPUT_VALID )
        output_Put( &output_dup, p_ts );

    p_ts->i_refcount--;
    if ( !p_ts->i_refcount )
        block_Delete( p_ts );
}

/*****************************************************************************
 * demux_Change : called from main thread
 *****************************************************************************/
static bool IsIn( const uint16_t *pi_pids, int i_nb_pids, uint16_t i_pid )
{
    int i;
    for ( i = 0; i < i_nb_pids; i++ )
        if ( i_pid == pi_pids[i] ) break;
    return ( i != i_nb_pids );
}

void demux_Change( output_t *p_output, const output_config_t *p_config )
{
    uint16_t *pi_wanted_pids, *pi_current_pids;
    int i_nb_wanted_pids, i_nb_current_pids;
    uint16_t i_wanted_pcr_pid, i_current_pcr_pid;

    uint16_t i_old_sid = p_output->config.i_sid;
    uint16_t i_sid = p_config->i_sid;
    uint16_t *pi_old_pids = p_output->config.pi_pids;
    uint16_t *pi_pids = p_config->pi_pids;
    int i_old_nb_pids = p_output->config.i_nb_pids;
    int i_nb_pids = p_config->i_nb_pids;

    bool b_sid_change = i_sid != i_old_sid;
    bool b_pid_change = false, b_tsid_change = false;
    bool b_dvb_change = !!((p_output->config.i_config ^ p_config->i_config)
                             & OUTPUT_DVB);
    bool b_epg_change = !!((p_output->config.i_config ^ p_config->i_config)
                             & OUTPUT_EPG);
    bool b_network_change =
        (dvb_string_cmp(&p_output->config.network_name, &p_config->network_name) ||
         p_output->config.i_network_id != p_config->i_network_id);
    bool b_service_name_change =
        (dvb_string_cmp(&p_output->config.service_name, &p_config->service_name) ||
         dvb_string_cmp(&p_output->config.provider_name, &p_config->provider_name));
    bool b_remap_change = p_output->config.i_new_sid != p_config->i_new_sid ||
        p_output->config.i_onid != p_config->i_onid ||
        p_output->config.b_do_remap != p_config->b_do_remap ||
        p_output->config.pi_confpids[I_PMTPID] != p_config->pi_confpids[I_PMTPID] ||
        p_output->config.pi_confpids[I_APID] != p_config->pi_confpids[I_APID] ||
        p_output->config.pi_confpids[I_VPID] != p_config->pi_confpids[I_VPID] ||
        p_output->config.pi_confpids[I_SPUPID] != p_config->pi_confpids[I_SPUPID];
    int i;

    p_output->config.i_config = p_config->i_config;
    p_output->config.i_network_id = p_config->i_network_id;
    p_output->config.i_new_sid = p_config->i_new_sid;
    p_output->config.i_onid = p_config->i_onid;
    p_output->config.b_do_remap = p_config->b_do_remap;
    memcpy(p_output->config.pi_confpids, p_config->pi_confpids,
           sizeof(uint16_t) * N_MAP_PIDS);

    /* Change output settings related to names. */
    dvb_string_clean( &p_output->config.network_name );
    dvb_string_clean( &p_output->config.service_name );
    dvb_string_clean( &p_output->config.provider_name );
    dvb_string_copy( &p_output->config.network_name,
                     &p_config->network_name );
    dvb_string_copy( &p_output->config.service_name,
                     &p_config->service_name );
    dvb_string_copy( &p_output->config.provider_name,
                     &p_config->provider_name );

    if ( p_config->i_tsid != -1 && p_output->config.i_tsid != p_config->i_tsid )
    {
        p_output->i_tsid = p_output->config.i_tsid = p_config->i_tsid;
        b_tsid_change = true;
    }
    if ( p_config->i_tsid == -1 && p_output->config.i_tsid != -1 )
    {
        p_output->config.i_tsid = p_config->i_tsid;
        if ( psi_table_validate(pp_current_pat_sections) && !b_random_tsid )
            p_output->i_tsid =
                psi_table_get_tableidext(pp_current_pat_sections);
        else
            p_output->i_tsid = rand() & 0xffff;
        b_tsid_change = true;
    }

    if ( p_config->b_passthrough == p_output->config.b_passthrough &&
         !b_sid_change && p_config->i_nb_pids == p_output->config.i_nb_pids &&
         (!p_config->i_nb_pids ||
          !memcmp( p_output->config.pi_pids, p_config->pi_pids,
                   p_config->i_nb_pids * sizeof(uint16_t) )) )
        goto out_change;

    GetPIDS( &pi_wanted_pids, &i_nb_wanted_pids, &i_wanted_pcr_pid,
             i_sid, pi_pids, i_nb_pids );
    GetPIDS( &pi_current_pids, &i_nb_current_pids, &i_current_pcr_pid,
             i_old_sid, pi_old_pids, i_old_nb_pids );

    if ( b_sid_change && i_old_sid )
    {
        sid_t *p_old_sid = FindSID( i_old_sid );
        p_output->config.i_sid = p_config->i_sid;

        if ( p_old_sid != NULL )
        {
            if ( i_sid != i_old_sid )
                UnselectPMT( i_old_sid, p_old_sid->i_pmt_pid );

            if ( i_ca_handle && !SIDIsSelected( i_old_sid )
                  && p_old_sid->p_current_pmt != NULL
                  && PMTNeedsDescrambling( p_old_sid->p_current_pmt ) )
                en50221_DeletePMT( p_old_sid->p_current_pmt );
        }
    }

    for ( i = 0; i < i_nb_current_pids; i++ )
    {
        if ( !IsIn( pi_wanted_pids, i_nb_wanted_pids, pi_current_pids[i] ) )
        {
            StopPID( p_output, pi_current_pids[i] );
            b_pid_change = true;
        }
    }

    if ( b_sid_change && i_ca_handle && i_old_sid &&
         SIDIsSelected( i_old_sid ) )
    {
        sid_t *p_old_sid = FindSID( i_old_sid );
        if ( p_old_sid != NULL && p_old_sid->p_current_pmt != NULL
              && PMTNeedsDescrambling( p_old_sid->p_current_pmt ) )
            en50221_UpdatePMT( p_old_sid->p_current_pmt );
    }

    for ( i = 0; i < i_nb_wanted_pids; i++ )
    {
        if ( !IsIn( pi_current_pids, i_nb_current_pids, pi_wanted_pids[i] ) )
        {
            StartPID( p_output, pi_wanted_pids[i] );
            b_pid_change = true;
        }
    }

    free( pi_wanted_pids );
    free( pi_current_pids );
    p_output->i_pcr_pid = i_wanted_pcr_pid;

    if ( b_sid_change && i_sid )
    {
        sid_t *p_sid = FindSID( i_sid );
        p_output->config.i_sid = i_old_sid;

        if ( p_sid != NULL )
        {
            if ( i_sid != i_old_sid )
                SelectPMT( i_sid, p_sid->i_pmt_pid );

            if ( i_ca_handle && !SIDIsSelected( i_sid )
                  && p_sid->p_current_pmt != NULL
                  && PMTNeedsDescrambling( p_sid->p_current_pmt ) )
                en50221_AddPMT( p_sid->p_current_pmt );
        }
    }

    if ( i_ca_handle && i_sid && SIDIsSelected( i_sid ) )
    {
        sid_t *p_sid = FindSID( i_sid );
        if ( p_sid != NULL && p_sid->p_current_pmt != NULL
              && PMTNeedsDescrambling( p_sid->p_current_pmt ) )
            en50221_UpdatePMT( p_sid->p_current_pmt );
    }

    p_output->config.b_passthrough = p_config->b_passthrough;
    p_output->config.i_sid = i_sid;
    free( p_output->config.pi_pids );
    p_output->config.pi_pids = malloc( sizeof(uint16_t) * i_nb_pids );
    memcpy( p_output->config.pi_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
    p_output->config.i_nb_pids = i_nb_pids;

out_change:
    if ( b_sid_change || b_pid_change || b_tsid_change || b_dvb_change ||
         b_network_change || b_service_name_change || b_remap_change )
    {
        msg_Dbg( NULL, "change %s%s%s%s%s%s%s",
                 b_sid_change ? "sid " : "",
                 b_pid_change ? "pid " : "",
                 b_tsid_change ? "tsid " : "",
                 b_dvb_change ? "dvb " : "",
                 b_network_change ? "network " : "",
                 b_service_name_change ? "service_name " : "",
                 b_remap_change ? "remap " : "" );
    }

    if ( b_sid_change || b_remap_change )
    {
        NewSDT( p_output );
        NewNIT( p_output );
        NewPAT( p_output );
        NewPMT( p_output );
    }
    else
    {
        if ( b_tsid_change )
        {
            NewSDT( p_output );
            NewNIT( p_output );
            NewPAT( p_output );
        }
        else if ( b_dvb_change )
        {
            NewNIT( p_output );
            NewPAT( p_output );
        }
        else if ( b_network_change )
            NewNIT( p_output );

        if ( !b_tsid_change && (b_service_name_change || b_epg_change) )
            NewSDT( p_output );

        if ( b_pid_change )
            NewPMT( p_output );
    }
}

/*****************************************************************************
 * SetDTS
 *****************************************************************************/
static void SetDTS( block_t *p_list )
{
    int i_nb_ts = 0, i;
    mtime_t i_duration;
    block_t *p_ts = p_list;

    while ( p_ts != NULL )
    {
        i_nb_ts++;
        p_ts = p_ts->p_next;
    }

    /* We suppose the stream is CBR, at least between two consecutive read().
     * This is especially true in budget mode */
    if ( i_last_dts == -1 )
        i_duration = 0;
    else
        i_duration = i_wallclock - i_last_dts;

    p_ts = p_list;
    i = i_nb_ts - 1;
    while ( p_ts != NULL )
    {
        p_ts->i_dts = i_wallclock - i_duration * i / i_nb_ts;
        i--;
        p_ts = p_ts->p_next;
    }

    i_last_dts = i_wallclock;
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

static void SetPID_EMM( uint16_t i_pid )
{
    SetPID( i_pid );
    p_pids[i_pid].b_emm = true;
}

static void UnsetPID( uint16_t i_pid )
{
    p_pids[i_pid].i_refcount--;

    if ( !b_budget_mode && !p_pids[i_pid].i_refcount
          && p_pids[i_pid].i_demux_fd != -1 )
    {
        pf_UnsetFilter( p_pids[i_pid].i_demux_fd, i_pid );
        p_pids[i_pid].i_demux_fd = -1;
        p_pids[i_pid].b_emm = false;
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
static void SelectPID( uint16_t i_sid, uint16_t i_pid, bool b_pcr )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
        {
            if ( pp_outputs[i]->config.i_nb_pids &&
                !IsIn( pp_outputs[i]->config.pi_pids,
                       pp_outputs[i]->config.i_nb_pids, i_pid ) )
            {
                if ( b_pcr )
                    pp_outputs[i]->i_pcr_pid = i_pid;
                else
                    continue;
            }
            StartPID( pp_outputs[i], i_pid );
        }
    }
}

static void UnselectPID( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid
              && !pp_outputs[i]->config.i_nb_pids )
            StopPID( pp_outputs[i], i_pid );
}

/*****************************************************************************
 * SelectPMT/UnselectPMT
 *****************************************************************************/
static void SelectPMT( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    p_pids[i_pid].i_psi_refcount++;
    p_pids[i_pid].b_pes = false;

    if ( b_select_pmts )
        SetPID( i_pid );
    else for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
            SetPID( i_pid );
}

static void UnselectPMT( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    p_pids[i_pid].i_psi_refcount--;
    if ( !p_pids[i_pid].i_psi_refcount )
        psi_assemble_reset( &p_pids[i_pid].p_psi_buffer,
                            &p_pids[i_pid].i_psi_buffer_used );

    if ( b_select_pmts )
        UnsetPID( i_pid );
    else for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
            UnsetPID( i_pid );
}

/*****************************************************************************
 * GetPIDS
 *****************************************************************************/
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t *pi_wanted_pcr_pid, uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids )
{
    sid_t *p_sid;
    uint8_t *p_pmt;
    uint16_t i_pmt_pid, i_pcr_pid;
    uint8_t *p_es;
    uint8_t j;
    const uint8_t *p_desc;

    *pi_wanted_pcr_pid = 0;

    if ( i_nb_pids || i_sid == 0 )
    {
        *pi_nb_wanted_pids = i_nb_pids;
        *ppi_wanted_pids = malloc( sizeof(uint16_t) * i_nb_pids );
        memcpy( *ppi_wanted_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
        if ( i_sid == 0 )
            return;
    }
    else
    {
        *pi_nb_wanted_pids = 0;
        *ppi_wanted_pids = NULL;
    }

    p_sid = FindSID( i_sid );
    if ( p_sid == NULL )
        return;

    p_pmt = p_sid->p_current_pmt;
    i_pmt_pid = p_sid->i_pmt_pid;
    if ( p_pmt == NULL ) {
        msg_Dbg(NULL, "no current PMT on sid %d\n", i_sid);
        return;
    }

    i_pcr_pid = pmt_get_pcrpid( p_pmt );
    j = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        j++;

        uint16_t i_pid = pmtn_get_pid( p_es );
        bool b_select;
        if ( i_nb_pids )
            b_select = IsIn( pi_pids, i_nb_pids, i_pid );
        else
        {
            b_select = PIDWouldBeSelected( p_es );
            if ( b_select )
            {
                *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                        (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
                (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = i_pid;
            }
        }

        if ( b_select && b_enable_ecm )
        {
            uint8_t k = 0;

            while ((p_desc = descs_get_desc( pmtn_get_descs( p_es ), k++ )) != NULL)
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;
                *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                                      (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
                (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] =  desc09_get_pid( p_desc );
            }
        }
    }


    if ( b_enable_ecm )
    {
        j = 0;

        while ((p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j++ )) != NULL)
        {
            if ( desc_get_tag( p_desc ) != 0x09 ||
                 !desc09_validate( p_desc ) )
                continue;
            *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                                  (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
            (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = desc09_get_pid( p_desc );
        }
    }

    if ( i_pcr_pid != PADDING_PID && i_pcr_pid != i_pmt_pid
          && !IsIn( *ppi_wanted_pids, *pi_nb_wanted_pids, i_pcr_pid ) )
    {
        *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                              (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
        (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = i_pcr_pid;
        /* We only need the PCR packets of this stream (incomplete) */
        *pi_wanted_pcr_pid = i_pcr_pid;
        msg_Dbg( NULL, "Requesting partial PCR PID %"PRIu16, i_pcr_pid );
    }
}

/*****************************************************************************
 * OutputPSISection
 *****************************************************************************/
static void OutputPSISection( output_t *p_output, uint8_t *p_section,
                              uint16_t i_pid, uint8_t *pi_cc, mtime_t i_dts,
                              block_t **pp_ts_buffer,
                              uint8_t *pi_ts_buffer_offset )
{
    uint16_t i_section_length = psi_get_length(p_section) + PSI_HEADER_SIZE;
    uint16_t i_section_offset = 0;

    do
    {
        block_t *p_block;
        uint8_t *p;
        uint8_t i_ts_offset;
        bool b_append = (pp_ts_buffer != NULL && *pp_ts_buffer != NULL);

        if ( b_append )
        {
            p_block = *pp_ts_buffer;
            i_ts_offset = *pi_ts_buffer_offset;
        }
        else
        {
            p_block = block_New();
            p_block->i_dts = i_dts;
            i_ts_offset = 0;
        }
        p = p_block->p_ts;

        psi_split_section( p, &i_ts_offset, p_section, &i_section_offset );

        if ( !b_append )
        {
            ts_set_pid( p, i_pid );
            ts_set_cc( p, *pi_cc );
            (*pi_cc)++;
            *pi_cc &= 0xf;
        }

        if ( i_section_offset == i_section_length )
        {
            if ( i_ts_offset < TS_SIZE - MIN_SECTION_FRAGMENT
                  && pp_ts_buffer != NULL )
            {
                *pp_ts_buffer = p_block;
                *pi_ts_buffer_offset = i_ts_offset;
                break;
            }
            else
                psi_split_end( p, &i_ts_offset );
        }

        p_block->i_dts = i_dts;
        p_block->i_refcount--;
        output_Put( p_output, p_block );
        if ( pp_ts_buffer != NULL )
        {
            *pp_ts_buffer = NULL;
            *pi_ts_buffer_offset = 0;
        }
    }
    while ( i_section_offset < i_section_length );
}

/*****************************************************************************
 * SendPAT
 *****************************************************************************/
static void SendPAT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( !(p_output->config.i_config & OUTPUT_VALID) ||
             p_output->config.b_passthrough )
            continue;

        if ( p_output->p_pat_section == NULL &&
             psi_table_validate(pp_current_pat_sections) )
        {
            /* SID doesn't exist - build an empty PAT. */
            uint8_t *p;
            p_output->i_pat_version++;

            p = p_output->p_pat_section = psi_allocate();
            pat_init( p );
            pat_set_length( p, 0 );
            pat_set_tsid( p, p_output->i_tsid );
            psi_set_version( p, p_output->i_pat_version );
            psi_set_current( p );
            psi_set_section( p, 0 );
            psi_set_lastsection( p, 0 );
            psi_set_crc( p_output->p_pat_section );
        }


        if ( p_output->p_pat_section != NULL )
            OutputPSISection( p_output, p_output->p_pat_section, PAT_PID,
                              &p_output->i_pat_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendPMT
 *****************************************************************************/
static void SendPMT( sid_t *p_sid, mtime_t i_dts )
{
    int i;
    int i_pmt_pid = p_sid->i_pmt_pid;

    if ( b_do_remap )
        i_pmt_pid = pi_newpids[ I_PMTPID ];

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && p_output->config.i_sid == p_sid->i_sid
               && p_output->p_pmt_section != NULL )
        {
            if ( p_output->config.b_do_remap && p_output->config.pi_confpids[I_PMTPID] )
                i_pmt_pid = p_output->config.pi_confpids[I_PMTPID];

            OutputPSISection( p_output, p_output->p_pmt_section,
                              i_pmt_pid, &p_output->i_pmt_cc, i_dts,
                              NULL, NULL );
        }
    }
}

/*****************************************************************************
 * SendNIT
 *****************************************************************************/
static void SendNIT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && !p_output->config.b_passthrough
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_nit_section != NULL )
            OutputPSISection( p_output, p_output->p_nit_section, NIT_PID,
                              &p_output->i_nit_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendSDT
 *****************************************************************************/
static void SendSDT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && !p_output->config.b_passthrough
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_sdt_section != NULL )
            OutputPSISection( p_output, p_output->p_sdt_section, SDT_PID,
                              &p_output->i_sdt_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendEIT
 *****************************************************************************/
static bool handle_epg( int i_table_id )
{
    return (i_table_id == EIT_TABLE_ID_PF_ACTUAL ||
       (i_table_id >= EIT_TABLE_ID_SCHED_ACTUAL_FIRST &&
        i_table_id <= EIT_TABLE_ID_SCHED_ACTUAL_LAST));
}

static void SendEIT( sid_t *p_sid, mtime_t i_dts, uint8_t *p_eit )
{
    uint8_t i_table_id = psi_get_tableid( p_eit );
    bool b_epg = handle_epg( i_table_id );
    uint16_t i_onid = eit_get_onid(p_eit);
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && !p_output->config.b_passthrough
               && (p_output->config.i_config & OUTPUT_DVB)
               && (!b_epg || (p_output->config.i_config & OUTPUT_EPG))
               && p_output->config.i_sid == p_sid->i_sid )
        {
            eit_set_tsid( p_eit, p_output->i_tsid );

            if ( p_output->config.i_new_sid )
                eit_set_sid( p_eit, p_output->config.i_new_sid );
            else
                eit_set_sid( p_eit, p_output->config.i_sid );

            if ( p_output->config.i_onid )
                eit_set_onid( p_eit, p_output->config.i_onid );

            psi_set_crc( p_eit );

            OutputPSISection( p_output, p_eit, EIT_PID, &p_output->i_eit_cc,
                              i_dts, &p_output->p_eit_ts_buffer,
                              &p_output->i_eit_ts_buffer_offset );

            if ( p_output->config.i_onid )
                eit_set_onid( p_eit, i_onid );
        }
    }
}

/*****************************************************************************
 * FlushEIT
 *****************************************************************************/
static void FlushEIT( output_t *p_output, mtime_t i_dts )
{
    block_t *p_block = p_output->p_eit_ts_buffer;

    psi_split_end( p_block->p_ts, &p_output->i_eit_ts_buffer_offset );
    p_block->i_dts = i_dts;
    p_block->i_refcount--;
    output_Put( p_output, p_block );
    p_output->p_eit_ts_buffer = NULL;
    p_output->i_eit_ts_buffer_offset = 0;
}

/*****************************************************************************
 * SendTDT
 *****************************************************************************/
static void SendTDT( block_t *p_ts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && !p_output->config.b_passthrough
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_sdt_section != NULL )
            output_Put( p_output, p_ts );
    }
}
/*****************************************************************************
 * SendEMM
 *****************************************************************************/
static void SendEMM( block_t *p_ts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && !p_output->config.b_passthrough )
            output_Put( p_output, p_ts );
    }
}

/*****************************************************************************
 * NewPAT
 *****************************************************************************/
static void NewPAT( output_t *p_output )
{
    const uint8_t *p_program;
    uint8_t *p;
    uint8_t k = 0;

    free( p_output->p_pat_section );
    p_output->p_pat_section = NULL;
    p_output->i_pat_version++;

    if ( !p_output->config.i_sid ) return;
    if ( !psi_table_validate(pp_current_pat_sections) ) return;

    p_program = pat_table_find_program( pp_current_pat_sections,
                                        p_output->config.i_sid );
    if ( p_program == NULL ) return;

    p = p_output->p_pat_section = psi_allocate();
    pat_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    pat_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_pat_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    if ( p_output->config.i_config & OUTPUT_DVB )
    {
        /* NIT */
        p = pat_get_program( p_output->p_pat_section, k++ );
        patn_init( p );
        patn_set_program( p, 0 );
        patn_set_pid( p, NIT_PID );
    }

    p = pat_get_program( p_output->p_pat_section, k++ );
    patn_init( p );
    if ( p_output->config.i_new_sid )
    {
        msg_Dbg( NULL, "Mapping PAT SID %d to %d", p_output->config.i_sid,
                 p_output->config.i_new_sid );
        patn_set_program( p, p_output->config.i_new_sid );
    }
    else
        patn_set_program( p, p_output->config.i_sid );

    if ( b_do_remap )
    {
        msg_Dbg( NULL, "Mapping PMT PID %d to %d", patn_get_pid( p_program ), pi_newpids[I_PMTPID] );
        patn_set_pid( p, pi_newpids[I_PMTPID]);
    } else if ( p_output->config.b_do_remap && p_output->config.pi_confpids[I_PMTPID] ) {
        msg_Dbg( NULL, "Mapping PMT PID %d to %d", patn_get_pid( p_program ), p_output->config.pi_confpids[I_PMTPID] );
        patn_set_pid( p, p_output->config.pi_confpids[I_PMTPID]);
    } else {
        patn_set_pid( p, patn_get_pid( p_program ) );
    }

    p = pat_get_program( p_output->p_pat_section, k );
    pat_set_length( p_output->p_pat_section,
                    p - p_output->p_pat_section - PAT_HEADER_SIZE );
    psi_set_crc( p_output->p_pat_section );
}

/*****************************************************************************
 * NewPMT
 *****************************************************************************/
static void CopyDescriptors( uint8_t *p_descs, uint8_t *p_current_descs )
{
    uint8_t *p_desc;
    const uint8_t *p_current_desc;
    uint16_t j = 0, k = 0;

    descs_set_length( p_descs, DESCS_MAX_SIZE );

    while ( (p_current_desc = descs_get_desc( p_current_descs, j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_current_desc );

        j++;
        if ( !b_enable_ecm && i_tag == 0x9 ) continue;

        p_desc = descs_get_desc( p_descs, k );
        if ( p_desc == NULL ) continue; /* This shouldn't happen */
        k++;
        memcpy( p_desc, p_current_desc,
                DESC_HEADER_SIZE + desc_get_length( p_current_desc ) );
    }

    p_desc = descs_get_desc( p_descs, k );
    if ( p_desc == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        descs_set_length( p_descs, 0 );
    else
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
}

static void NewPMT( output_t *p_output )
{
    sid_t *p_sid;
    uint8_t *p_current_pmt;
    uint8_t *p_es, *p_current_es;
    uint8_t *p;
    uint16_t j, k;
    uint16_t i_pcrpid;

    free( p_output->p_pmt_section );
    p_output->p_pmt_section = NULL;
    p_output->i_pmt_version++;

    if ( !p_output->config.i_sid ) return;

    p_sid = FindSID( p_output->config.i_sid );
    if ( p_sid == NULL ) return;

    if ( p_sid->p_current_pmt == NULL ) return;
    p_current_pmt = p_sid->p_current_pmt;

    p = p_output->p_pmt_section = psi_allocate();
    pmt_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    if ( p_output->config.i_new_sid )
    {
        msg_Dbg( NULL, "Mapping PMT SID %d to %d", p_output->config.i_sid,
                 p_output->config.i_new_sid );
        pmt_set_program( p, p_output->config.i_new_sid );
    }
    else
        pmt_set_program( p, p_output->config.i_sid );
    psi_set_version( p, p_output->i_pmt_version );
    psi_set_current( p );
    pmt_set_desclength( p, 0 );
    init_pid_mapping( p_output );


    CopyDescriptors( pmt_get_descs( p ), pmt_get_descs( p_current_pmt ) );

    j = 0; k = 0;
    while ( (p_current_es = pmt_get_es( p_current_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_current_es );

        j++;
        if ( (p_output->config.i_nb_pids || !PIDWouldBeSelected( p_current_es ))
              && !IsIn( p_output->config.pi_pids, p_output->config.i_nb_pids,
                        i_pid ) )
            continue;

        p_es = pmt_get_es( p, k );
        if ( p_es == NULL ) continue; /* This shouldn't happen */
        k++;
        pmtn_init( p_es );
        pmtn_set_streamtype( p_es, pmtn_get_streamtype( p_current_es ) );
        pmtn_set_pid( p_es, map_es_pid(p_output, p_current_es, i_pid) );
        pmtn_set_desclength( p_es, 0 );

        CopyDescriptors( pmtn_get_descs( p_es ),
                         pmtn_get_descs( p_current_es ) );
    }

    /* Do the pcr pid after everything else as it may have been remapped */
    i_pcrpid = pmt_get_pcrpid( p_current_pmt );
    if ( p_output->pi_newpids[i_pcrpid] != UNUSED_PID ) {
        msg_Dbg( NULL, "REMAP: The PCR PID was changed from 0x%x (%u) to 0x%x (%u)",
                 i_pcrpid, i_pcrpid, p_output->pi_newpids[i_pcrpid], p_output->pi_newpids[i_pcrpid] );
        i_pcrpid = p_output->pi_newpids[i_pcrpid];
    } else {
        msg_Dbg( NULL, "The PCR PID has kept its original value of 0x%x (%u)", i_pcrpid, i_pcrpid);
    }
    pmt_set_pcrpid( p, i_pcrpid );
    p_es = pmt_get_es( p, k );
    if ( p_es == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        pmt_set_length( p, 0 );
    else
        pmt_set_length( p, p_es - p - PMT_HEADER_SIZE );
    psi_set_crc( p );
}

/*****************************************************************************
 * NewNIT
 *****************************************************************************/
static void NewNIT( output_t *p_output )
{
    uint8_t *p_ts;
    uint8_t *p_header2;
    uint8_t *p;

    free( p_output->p_nit_section );
    p_output->p_nit_section = NULL;
    p_output->i_nit_version++;

    p = p_output->p_nit_section = psi_allocate();
    nit_init( p, true );
    nit_set_length( p, PSI_MAX_SIZE );
    nit_set_nid( p, p_output->config.i_network_id );
    psi_set_version( p, p_output->i_nit_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    if ( p_output->config.network_name.i )
    {
        uint8_t *p_descs;
        uint8_t *p_desc;
        nit_set_desclength( p, DESCS_MAX_SIZE );
        p_descs = nit_get_descs( p );
        p_desc = descs_get_desc( p_descs, 0 );
        desc40_init( p_desc );
        desc40_set_networkname( p_desc, p_output->config.network_name.p,
                                p_output->config.network_name.i );
        p_desc = descs_get_desc( p_descs, 1 );
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
    }
    else
        nit_set_desclength( p, 0 );

    p_header2 = nit_get_header2( p );
    nith_init( p_header2 );
    nith_set_tslength( p_header2, NIT_TS_SIZE );

    p_ts = nit_get_ts( p, 0 );
    nitn_init( p_ts );
    nitn_set_tsid( p_ts, p_output->i_tsid );
    if ( p_output->config.i_onid )
        nitn_set_onid( p_ts, p_output->config.i_onid );
    else
        nitn_set_onid( p_ts, p_output->config.i_network_id );
    nitn_set_desclength( p_ts, 0 );

    p_ts = nit_get_ts( p, 1 );
    if ( p_ts == NULL )
        /* This shouldn't happen */
        nit_set_length( p, 0 );
    else
        nit_set_length( p, p_ts - p - NIT_HEADER_SIZE );
    psi_set_crc( p_output->p_nit_section );
}

/*****************************************************************************
 * NewSDT
 *****************************************************************************/
static void NewSDT( output_t *p_output )
{
    uint8_t *p_service, *p_current_service;
    uint8_t *p;

    free( p_output->p_sdt_section );
    p_output->p_sdt_section = NULL;
    p_output->i_sdt_version++;

    if ( !p_output->config.i_sid ) return;
    if ( !psi_table_validate(pp_current_sdt_sections) ) return;

    p_current_service = sdt_table_find_service( pp_current_sdt_sections,
                                                p_output->config.i_sid );

    if ( p_current_service == NULL )
    {
        if ( p_output->p_pat_section != NULL &&
             pat_get_program( p_output->p_pat_section, 0 ) == NULL )
        {
            /* Empty PAT and no SDT anymore */
            free( p_output->p_pat_section );
            p_output->p_pat_section = NULL;
            p_output->i_pat_version++;
        }
        return;
    }

    p = p_output->p_sdt_section = psi_allocate();
    sdt_init( p, true );
    sdt_set_length( p, PSI_MAX_SIZE );
    sdt_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_sdt_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );
    if ( p_output->config.i_onid )
        sdt_set_onid( p, p_output->config.i_onid );
    else
        sdt_set_onid( p,
            sdt_get_onid( psi_table_get_section( pp_current_sdt_sections, 0 ) ) );

    p_service = sdt_get_service( p, 0 );
    sdtn_init( p_service );
    if ( p_output->config.i_new_sid )
    {
        msg_Dbg( NULL, "Mapping SDT SID %d to %d", p_output->config.i_sid,
                 p_output->config.i_new_sid );
        sdtn_set_sid( p_service, p_output->config.i_new_sid );
    }
    else
        sdtn_set_sid( p_service, p_output->config.i_sid );

    /* We always forward EITp/f */
    if ( sdtn_get_eitpresent(p_current_service) )
        sdtn_set_eitpresent(p_service);

    if ( (p_output->config.i_config & OUTPUT_EPG) == OUTPUT_EPG &&
         sdtn_get_eitschedule(p_current_service) )
        sdtn_set_eitschedule(p_service);

    sdtn_set_running( p_service, sdtn_get_running(p_current_service) );
    /* Do not set free_ca */
    sdtn_set_desclength( p_service, sdtn_get_desclength(p_current_service) );

    if ( !p_output->config.provider_name.i &&
         !p_output->config.service_name.i ) {
        /* Copy all descriptors unchanged */
        memcpy( descs_get_desc( sdtn_get_descs(p_service), 0 ),
                descs_get_desc( sdtn_get_descs(p_current_service), 0 ),
                sdtn_get_desclength(p_current_service) );
    } else {
        int j = 0, i_total_desc_len = 0;
        uint8_t *p_desc;
        uint8_t *p_new_desc = descs_get_desc( sdtn_get_descs(p_service), 0 );
        while ( (p_desc = descs_get_desc( sdtn_get_descs( p_current_service ), j++ )) != NULL )
        {
            /* Regenerate descriptor 48 (service name) */
            if ( desc_get_tag( p_desc ) == 0x48 && desc48_validate( p_desc ) )
            {
                uint8_t i_old_provider_len, i_old_service_len;
                uint8_t i_new_desc_len = 3; /* 1 byte - type, 1 byte provider_len, 1 byte service_len */
                const uint8_t *p_old_provider = desc48_get_provider( p_desc, &i_old_provider_len );
                const uint8_t *p_old_service = desc48_get_service( p_desc, &i_old_service_len );

                desc48_init( p_new_desc );
                desc48_set_type( p_new_desc, desc48_get_type( p_desc ) );

                if ( p_output->config.provider_name.i ) {
                    desc48_set_provider( p_new_desc,
                            p_output->config.provider_name.p,
                            p_output->config.provider_name.i );
                    i_new_desc_len += p_output->config.provider_name.i;
                } else {
                    desc48_set_provider( p_new_desc, p_old_provider,
                            i_old_provider_len );
                    i_new_desc_len += i_old_provider_len;
                }

                if ( p_output->config.service_name.i ) {
                    desc48_set_service( p_new_desc,
                            p_output->config.service_name.p,
                            p_output->config.service_name.i );
                    i_new_desc_len += p_output->config.service_name.i;
                } else {
                    desc48_set_service( p_new_desc, p_old_service,
                            i_old_service_len );
                    i_new_desc_len += i_old_service_len;
                }

                desc_set_length( p_new_desc, i_new_desc_len );
                i_total_desc_len += DESC_HEADER_SIZE + i_new_desc_len;
                p_new_desc += DESC_HEADER_SIZE + i_new_desc_len;
            } else {
                /* Copy single descriptor */
                int i_desc_len = DESC_HEADER_SIZE + desc_get_length( p_desc );
                memcpy( p_new_desc, p_desc, i_desc_len );
                p_new_desc += i_desc_len;
                i_total_desc_len += i_desc_len;
            }
        }
        sdtn_set_desclength( p_service, i_total_desc_len );
    }

    p_service = sdt_get_service( p, 1 );
    if ( p_service == NULL )
        /* This shouldn't happen if the incoming SDT is valid */
        sdt_set_length( p, 0 );
    else
        sdt_set_length( p, p_service - p - SDT_HEADER_SIZE );
    psi_set_crc( p_output->p_sdt_section );
}

/*****************************************************************************
 * UpdatePAT/PMT/SDT
 *****************************************************************************/
#define DECLARE_UPDATE_FUNC( table )                                        \
static void Update##table( uint16_t i_sid )                                 \
{                                                                           \
    int i;                                                                  \
                                                                            \
    for ( i = 0; i < i_nb_outputs; i++ )                                    \
        if ( ( pp_outputs[i]->config.i_config & OUTPUT_VALID )              \
             && pp_outputs[i]->config.i_sid == i_sid )                      \
            New##table( pp_outputs[i] );                                    \
}

DECLARE_UPDATE_FUNC(PAT)
DECLARE_UPDATE_FUNC(PMT)
DECLARE_UPDATE_FUNC(SDT)

/*****************************************************************************
 * UpdateTSID
 *****************************************************************************/
static void UpdateTSID(void)
{
    uint16_t i_tsid = psi_table_get_tableidext(pp_current_pat_sections);
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
              && p_output->config.i_tsid == -1 && !b_random_tsid )
        {
            p_output->i_tsid = i_tsid;
            NewNIT( p_output );
        }
    }
}

/*****************************************************************************
 * SIDIsSelected
 *****************************************************************************/
static bool SIDIsSelected( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
             && pp_outputs[i]->config.i_sid == i_sid )
            return true;

    return false;
}

/*****************************************************************************
 * demux_PIDIsSelected
 *****************************************************************************/
bool demux_PIDIsSelected( uint16_t i_pid )
{
    int i;

    for ( i = 0; i < p_pids[i_pid].i_nb_outputs; i++ )
        if ( p_pids[i_pid].pp_outputs[i] != NULL )
            return true;

    return false;
}

/*****************************************************************************
 * PIDWouldBeSelected
 *****************************************************************************/
static bool PIDWouldBeSelected( uint8_t *p_es )
{
    if ( b_any_type ) return true;

    uint8_t i_type = pmtn_get_streamtype( p_es );

    switch ( i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0xf: /* audio AAC ADTS */
    case 0x10: /* video MPEG-4 */
    case 0x11: /* audio AAC LATM */
    case 0x1b: /* video H264 */
    case 0x24: /* video H265 */
    case 0x81: /* ATSC A/52 */
    case 0x87: /* ATSC Enhanced A/52 */
        return true;
        break;

    case 0x6:
    {
        uint16_t j = 0;
        const uint8_t *p_desc;

        while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), j )) != NULL )
        {
            uint8_t i_tag = desc_get_tag( p_desc );
            j++;

            if( i_tag == 0x46 /* VBI + teletext */
                 || i_tag == 0x56 /* teletext */
                 || i_tag == 0x59 /* dvbsub */
                 || i_tag == 0x6a /* A/52 */
                 || i_tag == 0x7a /* Enhanced A/52 */
                 || i_tag == 0x7b /* DCA */
                 || i_tag == 0x7c /* AAC */ )
                return true;
        }
        break;
    }

    default:
        break;
    }

    /* FIXME: also parse IOD */
    return false;
}

/*****************************************************************************
 * PIDCarriesPES
 *****************************************************************************/
static bool PIDCarriesPES( const uint8_t *p_es )
{
    uint8_t i_type = pmtn_get_streamtype( p_es );

    switch ( i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0x6: /* private PES data */
    case 0xf: /* audio AAC */
    case 0x10: /* video MPEG-4 */
    case 0x11: /* audio AAC LATM */
    case 0x1b: /* video H264 */
    case 0x24: /* video H265 */
    case 0x81: /* ATSC A/52 */
    case 0x87: /* ATSC Enhanced A/52 */
        return true;
        break;

    default:
        return false;
        break;
    }
}

/*****************************************************************************
 * PMTNeedsDescrambling
 *****************************************************************************/
static bool PMTNeedsDescrambling( uint8_t *p_pmt )
{
    uint8_t i;
    uint16_t j;
    uint8_t *p_es;
    const uint8_t *p_desc;

    j = 0;
    while ( (p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_desc );
        j++;

        if ( i_tag == 0x9 ) return true;
    }

    i = 0;
    while ( (p_es = pmt_get_es( p_pmt, i )) != NULL )
    {
        i++;
        j = 0;
        while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), j )) != NULL )
        {
            uint8_t i_tag = desc_get_tag( p_desc );
            j++;

            if ( i_tag == 0x9 ) return true;
        }
    }

    return false;
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

/* Find CA descriptor that have PID i_ca_pid */
static uint8_t *ca_desc_find( uint8_t *p_descl, uint16_t i_length,
                              uint16_t i_ca_pid )
{
    int j = 0;
    uint8_t *p_desc;

    while ( (p_desc = descl_get_desc( p_descl, i_length, j++ )) != NULL ) {
        if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
            continue;
        if ( desc09_get_pid( p_desc ) == i_ca_pid )
            return p_desc;
    }

    return NULL;
}

/*****************************************************************************
 * DeleteProgram
 *****************************************************************************/
static void DeleteProgram( uint16_t i_sid, uint16_t i_pid )
{
    sid_t *p_sid;
    uint8_t *p_pmt;
    uint8_t *p_desc;

    UnselectPMT( i_sid, i_pid );

    p_sid = FindSID( i_sid );
    if ( p_sid == NULL ) return;

    p_pmt = p_sid->p_current_pmt;

    if ( p_pmt != NULL )
    {
        uint16_t i_pcr_pid = pmt_get_pcrpid( p_pmt );
        uint8_t *p_es;
        uint8_t j;

        if ( i_ca_handle && SIDIsSelected( i_sid )
             && PMTNeedsDescrambling( p_pmt ) )
            en50221_DeletePMT( p_pmt );

        if ( i_pcr_pid != PADDING_PID
              && i_pcr_pid != p_sid->i_pmt_pid )
            UnselectPID( i_sid, i_pcr_pid );

        if ( b_enable_ecm )
        {
            j = 0;

            while ((p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j++ )) != NULL)
            {
                if ( desc_get_tag( p_desc ) != 0x09 ||
                     !desc09_validate( p_desc ) )
                    continue;
                UnselectPID( i_sid, desc09_get_pid( p_desc ) );
            }
        }

        j = 0;
        while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
        {
            uint16_t i_pid = pmtn_get_pid( p_es );
            j++;

            if ( PIDWouldBeSelected( p_es ) )
                UnselectPID( i_sid, i_pid );

            if ( b_enable_ecm )
            {
                uint8_t k = 0;

                while ((p_desc = descs_get_desc( pmtn_get_descs( p_es ), k++ )) != NULL)
                {
                    if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                        continue;
                    UnselectPID( i_sid, desc09_get_pid( p_desc ) );
                }
            }
        }

        free( p_pmt );
        p_sid->p_current_pmt = NULL;
    }
    p_sid->i_sid = 0;
    p_sid->i_pmt_pid = 0;

    uint8_t r;
    for ( r = 0; r < MAX_EIT_TABLES; r++ ) {
        psi_table_free( p_sid->eit_table[r].data );
        psi_table_init( p_sid->eit_table[r].data );
    }

}

/*****************************************************************************
 * demux_Iconv
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
static char *iconv_append_null(const char *p_string, size_t i_length)
{
    char *psz_string = malloc(i_length + 1);
    memcpy(psz_string, p_string, i_length);
    psz_string[i_length] = '\0';
    return psz_string;
}

char *demux_Iconv(void *_unused, const char *psz_encoding,
                  char *p_string, size_t i_length)
{
#ifdef HAVE_ICONV
    static const char *psz_current_encoding = "";

    char *psz_string, *p;
    size_t i_out_length;

    if (!strcmp(psz_encoding, psz_native_charset))
        return iconv_append_null(p_string, i_length);

    if (iconv_handle != (iconv_t)-1 &&
        strcmp(psz_encoding, psz_current_encoding)) {
        iconv_close(iconv_handle);
        iconv_handle = (iconv_t)-1;
    }

    if (iconv_handle == (iconv_t)-1)
        iconv_handle = iconv_open(psz_native_charset, psz_encoding);
    if (iconv_handle == (iconv_t)-1) {
        msg_Warn(NULL, "couldn't open converter from %s to %s (%m)", psz_encoding,
                psz_native_charset);
        return iconv_append_null(p_string, i_length);
    }
    psz_current_encoding = psz_encoding;

    /* converted strings can be up to six times larger */
    i_out_length = i_length * 6;
    p = psz_string = malloc(i_out_length);
    if (iconv(iconv_handle, &p_string, &i_length, &p, &i_out_length) == -1) {
        msg_Warn(NULL, "couldn't convert from %s to %s (%m)", psz_encoding,
                psz_native_charset);
        free(psz_string);
        return iconv_append_null(p_string, i_length);
    }
    if (i_length)
        msg_Warn(NULL, "partial conversion from %s to %s", psz_encoding,
                psz_native_charset);

    *p = '\0';
    return psz_string;
#else
    return iconv_append_null(p_string, i_length);
#endif
}

/*****************************************************************************
 * demux_Print
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
__attribute__ ((format(printf, 2, 3)))
static void demux_Print(void *_unused, const char *psz_format, ...)
{
    char psz_fmt[strlen(psz_format) + 2];
    va_list args;
    va_start(args, psz_format);
    strcpy(psz_fmt, psz_format);
    if ( i_print_type != PRINT_XML )
        strcat(psz_fmt, "\n");
    vprintf(psz_fmt, args);
    va_end(args);
}

/*****************************************************************************
 * HandlePAT
 *****************************************************************************/
static void HandlePAT( mtime_t i_dts )
{
    bool b_change = false;
    PSI_TABLE_DECLARE( pp_old_pat_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_pat_sections );
    uint8_t i, r;

    if ( psi_table_validate( pp_current_pat_sections ) &&
         psi_table_compare( pp_current_pat_sections, pp_next_pat_sections ) )
    {
        /* Identical PAT. Shortcut. */
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    if ( !pat_table_validate( pp_next_pat_sections ) )
    {
        msg_Warn( NULL, "invalid PAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pat\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pat\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    /* Switch tables. */
    psi_table_copy( pp_old_pat_sections, pp_current_pat_sections );
    psi_table_copy( pp_current_pat_sections, pp_next_pat_sections );
    psi_table_init( pp_next_pat_sections );

    if ( !psi_table_validate( pp_old_pat_sections )
          || psi_table_get_tableidext( pp_current_pat_sections )
              != psi_table_get_tableidext( pp_old_pat_sections ) )
    {
        b_change = true;
        UpdateTSID();
        /* This will trigger a universal reset of everything. */
    }

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_pat_sections, i );
        const uint8_t *p_program;
        int j = 0;

        while ( (p_program = pat_get_program( p_section, j )) != NULL )
        {
            const uint8_t *p_old_program = NULL;
            uint16_t i_sid = patn_get_program( p_program );
            uint16_t i_pid = patn_get_pid( p_program );
            j++;

            if ( i_sid == 0 )
            {
                if ( i_pid != NIT_PID )
                    msg_Warn( NULL,
                        "NIT is carried on PID %hu which isn't DVB compliant",
                        i_pid );
                continue; /* NIT */
            }

            if ( !psi_table_validate( pp_old_pat_sections )
                  || (p_old_program = pat_table_find_program(
                                       pp_old_pat_sections, i_sid )) == NULL
                  || patn_get_pid( p_old_program ) != i_pid
                  || b_change )
            {
                sid_t *p_sid;

                if ( p_old_program != NULL )
                    DeleteProgram( i_sid, patn_get_pid( p_old_program ) );

                SelectPMT( i_sid, i_pid );

                p_sid = FindSID( 0 );
                if ( p_sid == NULL )
                {
                    p_sid = malloc( sizeof(sid_t) );
                    p_sid->p_current_pmt = NULL;
                    for ( r = 0; r < MAX_EIT_TABLES; r++ ) {
                        psi_table_init( p_sid->eit_table[r].data );
                    }
                    i_nb_sids++;
                    pp_sids = realloc( pp_sids, sizeof(sid_t *) * i_nb_sids );
                    pp_sids[i_nb_sids - 1] = p_sid;
                }

                p_sid->i_sid = i_sid;
                p_sid->i_pmt_pid = i_pid;

                UpdatePAT( i_sid );
            }
        }
    }

    if ( psi_table_validate( pp_old_pat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_pat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section =
                psi_table_get_section( pp_old_pat_sections, i );
            const uint8_t *p_program;
            int j = 0;

            while ( (p_program = pat_get_program( p_section, j )) != NULL )
            {
                uint16_t i_sid = patn_get_program( p_program );
                uint16_t i_pid = patn_get_pid( p_program );
                j++;

                if ( i_sid == 0 )
                    continue; /* NIT */

                if ( pat_table_find_program( pp_current_pat_sections, i_sid )
                      == NULL )
                {
                    DeleteProgram( i_sid, i_pid );
                    UpdatePAT( i_sid );
                }
            }
        }

        psi_table_free( pp_old_pat_sections );
    }

    pat_table_print( pp_current_pat_sections, msg_Dbg, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        pat_table_print( pp_current_pat_sections, demux_Print, NULL,
                         i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_pat:
    SendPAT( i_dts );
}

/*****************************************************************************
 * HandlePATSection
 *****************************************************************************/
static void HandlePATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != PAT_PID || !pat_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid PAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pat_section\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pat_section\n");
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_pat_sections, p_section ) )
        return;

    HandlePAT( i_dts );
}

/*****************************************************************************
 * HandleCAT
 *****************************************************************************/
static void HandleCAT( mtime_t i_dts )
{
    PSI_TABLE_DECLARE( pp_old_cat_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_cat_sections );
    uint8_t i_last_section2;
    uint8_t i, r;
    uint8_t *p_desc;
    int j, k;

    if ( psi_table_validate( pp_current_cat_sections ) &&
         psi_table_compare( pp_current_cat_sections, pp_next_cat_sections ) )
    {
        /* Identical CAT. Shortcut. */
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    if ( !cat_table_validate( pp_next_cat_sections ) )
    {
        msg_Warn( NULL, "invalid CAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_cat\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_cat\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    /* Switch tables. */
    psi_table_copy( pp_old_cat_sections, pp_current_cat_sections );
    psi_table_copy( pp_current_cat_sections, pp_next_cat_sections );
    psi_table_init( pp_next_cat_sections );

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, i );

        j = 0;
        while ( (p_desc = descl_get_desc( cat_get_descl(p_section), cat_get_desclength(p_section), j++ )) != NULL )
        {
            if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                continue;

            SetPID_EMM( desc09_get_pid( p_desc ) );
        }
    }

    if ( psi_table_validate( pp_old_cat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_cat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_old_section = psi_table_get_section( pp_old_cat_sections, i );
            j = 0;
            while ( (p_desc = descl_get_desc( cat_get_descl(p_old_section), cat_get_desclength(p_old_section), j++ )) != NULL )
            {
                uint16_t emm_pid;
                int pid_found = 0;

                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;

                emm_pid = desc09_get_pid( p_desc );

                // Search in current sections if the pid exists
                i_last_section2 = psi_table_get_lastsection( pp_current_cat_sections );
                for ( r = 0; r <= i_last_section2; r++ )
                {
                    uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, r );

                    k = 0;
                    while ( (p_desc = descl_get_desc( cat_get_descl(p_section), cat_get_desclength(p_section), k++ )) != NULL )
                    {
                        if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                            continue;
                        if ( ca_desc_find( cat_get_descl(p_section), cat_get_desclength(p_section), emm_pid ) != NULL )
                        {
                            pid_found = 1;
                            break;
                        }
                    }
                }

                if ( !pid_found )
                    UnsetPID(emm_pid);
            }
        }

        psi_table_free( pp_old_cat_sections );
    }

    cat_table_print( pp_current_cat_sections, msg_Dbg, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        cat_table_print( pp_current_cat_sections, demux_Print, NULL,
                         i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_cat:
    return;
}

/*****************************************************************************
 * HandleCATSection
 *****************************************************************************/
static void HandleCATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != CAT_PID || !cat_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid CAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_cat_section\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_cat_section\n");
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_cat_sections, p_section ) )
        return;

    HandleCAT( i_dts );
}

static void mark_pmt_pids( uint8_t *p_pmt, uint8_t pid_map[], uint8_t marker )
{
    uint16_t j, k;
    uint8_t *p_es;
    uint8_t *p_desc;

    uint16_t i_pcr_pid = pmt_get_pcrpid( p_pmt );

    if ( b_enable_ecm )
    {
        j = 0;
        while ( (p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j++ )) != NULL )
        {
            if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                continue;
            pid_map[ desc09_get_pid( p_desc ) ] |= marker;
        }
    }

    if ( i_pcr_pid != PADDING_PID )
        pid_map[ i_pcr_pid ] |= marker;

    j = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_es );
        j++;

        if ( PIDWouldBeSelected( p_es ) )
            pid_map[ i_pid ] |= marker;

        p_pids[i_pid].b_pes = PIDCarriesPES( p_es );

        if ( b_enable_ecm )
        {
            k = 0;
            while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), k++ )) != NULL )
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;
                pid_map[ desc09_get_pid( p_desc ) ] |= marker;
            }
        }
    }
}

/*****************************************************************************
 * HandlePMT
 *****************************************************************************/
static void HandlePMT( uint16_t i_pid, uint8_t *p_pmt, mtime_t i_dts )
{
    uint16_t i_sid = pmt_get_program( p_pmt );
    sid_t *p_sid;
    bool b_needs_descrambling, b_needed_descrambling, b_is_selected;
    uint8_t pid_map[MAX_PIDS];

    p_sid = FindSID( i_sid );
    if ( p_sid == NULL )
    {
        /* Unwanted SID (happens when the same PMT PID is used for several
         * programs). */
        free( p_pmt );
        return;
    }

    if ( i_pid != p_sid->i_pmt_pid )
    {
        msg_Warn( NULL, "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"ghost_pmt\" program=\"%hu\n pid=\"%hu\"/>\n",
                    i_sid, i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: ghost_pmt program: %hu pid: %hu\n",
                    i_sid, i_pid);
            break;
        default:
            break;
        }
        free( p_pmt );
        return;
    }

    if ( p_sid->p_current_pmt != NULL &&
         psi_compare( p_sid->p_current_pmt, p_pmt ) )
    {
        /* Identical PMT. Shortcut. */
        free( p_pmt );
        goto out_pmt;
    }

    if ( !pmt_validate( p_pmt ) )
    {
        msg_Warn( NULL, "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_pmt_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_pmt_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_pmt );
        goto out_pmt;
    }

    memset( pid_map, 0, sizeof(pid_map) );

    b_needs_descrambling = PMTNeedsDescrambling( p_pmt );
    b_needed_descrambling = p_sid->p_current_pmt != NULL ?
                            PMTNeedsDescrambling( p_sid->p_current_pmt ) :
                            false;
    b_is_selected = SIDIsSelected( i_sid );

    if ( i_ca_handle && b_is_selected &&
         !b_needs_descrambling && b_needed_descrambling )
        en50221_DeletePMT( p_sid->p_current_pmt );

    if ( p_sid->p_current_pmt != NULL )
    {
        mark_pmt_pids( p_sid->p_current_pmt, pid_map, 0x02 );
        free( p_sid->p_current_pmt );
    }

    mark_pmt_pids( p_pmt, pid_map, 0x01 );

    uint16_t i_pcr_pid = pmt_get_pcrpid( p_pmt );
    int i;
    for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
            pp_outputs[i]->i_pcr_pid = 0;

    /* Start to stream PIDs */
    int pid;
    for ( pid = 0; pid < MAX_PIDS; pid++ )
    {
        /* The pid does not exist in the old PMT and in the new PMT. Ignore this pid. */
        if ( !pid_map[ pid ] )
            continue;

        switch ( pid_map[ pid ] & 0x03 ) {
        case 0x03: /* The pid exists in the old PMT and in the new PMT. The pid was already selected in case 0x01. */
            continue;
        case 0x02: /* The pid does not exist in the new PMT but exists in the old PMT. Unselect it. */
            UnselectPID( i_sid, pid );
            break;
        case 0x01: /* The pid exists in new PMT. Select it. */
            SelectPID( i_sid, pid, pid == i_pcr_pid );
            break;
        }
    }

    p_sid->p_current_pmt = p_pmt;

    if ( i_ca_handle && b_is_selected )
    {
        if ( b_needs_descrambling && !b_needed_descrambling )
            en50221_AddPMT( p_pmt );
        else if ( b_needs_descrambling && b_needed_descrambling )
            en50221_UpdatePMT( p_pmt );
    }

    UpdatePMT( i_sid );

    pmt_print( p_pmt, msg_Dbg, NULL, demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        pmt_print( p_pmt, demux_Print, NULL, demux_Iconv, NULL,
                   i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_pmt:
    SendPMT( p_sid, i_dts );
}

/*****************************************************************************
 * HandleNIT
 *****************************************************************************/
static void HandleNIT( mtime_t i_dts )
{
    if ( psi_table_validate( pp_current_nit_sections ) &&
         psi_table_compare( pp_current_nit_sections, pp_next_nit_sections ) )
    {
        /* Identical NIT. Shortcut. */
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    if ( !nit_table_validate( pp_next_nit_sections ) )
    {
        msg_Warn( NULL, "invalid NIT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_nit\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_nit\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    /* Switch tables. */
    psi_table_free( pp_current_nit_sections );
    psi_table_copy( pp_current_nit_sections, pp_next_nit_sections );
    psi_table_init( pp_next_nit_sections );

    nit_table_print( pp_current_nit_sections, msg_Dbg, NULL,
                     demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        nit_table_print( pp_current_nit_sections, demux_Print, NULL,
                         demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_nit:
    ;
}

/*****************************************************************************
 * HandleNITSection
 *****************************************************************************/
static void HandleNITSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != NIT_PID || !nit_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid NIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_nit_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_nit_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( psi_table_section( pp_next_nit_sections, p_section ) )
        HandleNIT( i_dts );

    /* This case is different because DVB specifies a minimum bitrate for
     * PID 0x10, even if we don't have any thing to send (for cheap
     * transport over network boundaries). */
    SendNIT( i_dts );
}


/*****************************************************************************
 * HandleSDT
 *****************************************************************************/
static void HandleSDT( mtime_t i_dts )
{
    PSI_TABLE_DECLARE( pp_old_sdt_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_sdt_sections );
    uint8_t i;
    int j;

    if ( psi_table_validate( pp_current_sdt_sections ) &&
         psi_table_compare( pp_current_sdt_sections, pp_next_sdt_sections ) )
    {
        /* Identical SDT. Shortcut. */
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    if ( !sdt_table_validate( pp_next_sdt_sections ) )
    {
        msg_Warn( NULL, "invalid SDT received" );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_sdt\"/>\n");
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_sdt\n");
            break;
        default:
            break;
        }
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    /* Switch tables. */
    psi_table_copy( pp_old_sdt_sections, pp_current_sdt_sections );
    psi_table_copy( pp_current_sdt_sections, pp_next_sdt_sections );
    psi_table_init( pp_next_sdt_sections );

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_sdt_sections, i );
        uint8_t *p_service;
        j = 0;

        while ( (p_service = sdt_get_service( p_section, j )) != NULL )
        {
            uint16_t i_sid = sdtn_get_sid( p_service );
            j++;

            UpdateSDT( i_sid );
        }
    }

    if ( psi_table_validate( pp_old_sdt_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_sdt_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section =
                psi_table_get_section( pp_old_sdt_sections, i );
            const uint8_t *p_service;
            int j = 0;

            while ( (p_service = sdt_get_service( p_section, j )) != NULL )
            {
                uint16_t i_sid = sdtn_get_sid( p_service );
                j++;

                if ( sdt_table_find_service( pp_current_sdt_sections, i_sid )
                      == NULL )
                    UpdateSDT( i_sid );
            }
        }

        psi_table_free( pp_old_sdt_sections );
    }

    sdt_table_print( pp_current_sdt_sections, msg_Dbg, NULL,
                     demux_Iconv, NULL, PRINT_TEXT );
    if ( b_print_enabled )
    {
        sdt_table_print( pp_current_sdt_sections, demux_Print, NULL,
                         demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_sdt:
    SendSDT( i_dts );
}

/*****************************************************************************
 * HandleSDTSection
 *****************************************************************************/
static void HandleSDTSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != SDT_PID || !sdt_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid SDT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_sdt_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_sdt_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_sdt_sections, p_section ) )
        return;

    HandleSDT( i_dts );
}

/*****************************************************************************
 * HandleEITSection
 *****************************************************************************/
static void HandleEIT( uint16_t i_pid, uint8_t *p_eit, mtime_t i_dts )
{
    uint8_t i_table_id = psi_get_tableid( p_eit );
    uint16_t i_sid = eit_get_sid( p_eit );
    sid_t *p_sid;

    p_sid = FindSID( i_sid );
    if ( p_sid == NULL )
    {
        /* Not a selected program. */
        free( p_eit );
        return;
    }

    if ( i_pid != EIT_PID || !eit_validate( p_eit ) )
    {
        msg_Warn( NULL, "invalid EIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_eit_section\" pid=\"%hu\"/>\n",
                    i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_eit_section pid: %hu\n",
                    i_pid);
            break;
        default:
            break;
        }
        free( p_eit );
        return;
    }

    bool b_epg = handle_epg( i_table_id );
    if ( ! b_epg )
        goto out_eit;

    /* We do not use psi_table_* primitives as the spec allows for holes in
     * section numbering, and there is no sure way to know whether you have
     * gathered all sections. */
    uint8_t i_section = psi_get_section(p_eit);
    uint8_t eit_table_id = i_table_id - EIT_TABLE_ID_PF_ACTUAL;
    if (eit_table_id >= MAX_EIT_TABLES)
        goto out_eit;
    if (p_sid->eit_table[eit_table_id].data[i_section] != NULL &&
        psi_compare(p_sid->eit_table[eit_table_id].data[i_section], p_eit)) {
        /* Identical section. Shortcut. */
        free(p_sid->eit_table[eit_table_id].data[i_section]);
        p_sid->eit_table[eit_table_id].data[i_section] = p_eit;
        goto out_eit;
    }

    free(p_sid->eit_table[eit_table_id].data[i_section]);
    p_sid->eit_table[eit_table_id].data[i_section] = p_eit;

    if ( b_print_enabled )
    {
        eit_print( p_eit, demux_Print, NULL,
                   demux_Iconv, NULL, i_print_type );
        if ( i_print_type == PRINT_XML )
            fprintf(print_fh, "\n");
    }

out_eit:
    SendEIT( p_sid, i_dts, p_eit );
    if ( ! b_epg )
        free( p_eit );
}

/*****************************************************************************
 * HandleSection
 *****************************************************************************/
static void HandleSection( uint16_t i_pid, uint8_t *p_section, mtime_t i_dts )
{
    uint8_t i_table_id = psi_get_tableid( p_section );

    if ( !psi_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid section on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            fprintf(print_fh, "<ERROR type=\"invalid_section\" pid=\"%hu\"/>\n", i_pid);
            break;
        case PRINT_TEXT:
            fprintf(print_fh, "error type: invalid_section pid: %hu\n", i_pid);
            break;
        default:
            break;
        }
        free( p_section );
        return;
    }

    if ( !psi_get_current( p_section ) )
    {
        /* Ignore sections which are not in use yet. */
        free( p_section );
        return;
    }

    switch ( i_table_id )
    {
    case PAT_TABLE_ID:
        HandlePATSection( i_pid, p_section, i_dts );
        break;

    case CAT_TABLE_ID:
        if ( b_enable_emm )
            HandleCATSection( i_pid, p_section, i_dts );
        break;

    case PMT_TABLE_ID:
        HandlePMT( i_pid, p_section, i_dts );
        break;

    case NIT_TABLE_ID_ACTUAL:
        HandleNITSection( i_pid, p_section, i_dts );
        break;

    case SDT_TABLE_ID_ACTUAL:
        HandleSDTSection( i_pid, p_section, i_dts );
        break;

    default:
        if ( handle_epg( i_table_id ) )
        {
            HandleEIT( i_pid, p_section, i_dts );
            break;
        }
        free( p_section );
        break;
    }
}

/*****************************************************************************
 * HandlePSIPacket
 *****************************************************************************/
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts )
{
    uint16_t i_pid = ts_get_pid( p_ts );
    ts_pid_t *p_pid = &p_pids[i_pid];
    uint8_t i_cc = ts_get_cc( p_ts );
    const uint8_t *p_payload;
    uint8_t i_length;

    if ( ts_check_duplicate( i_cc, p_pid->i_last_cc )
          || !ts_has_payload( p_ts ) )
        return;

    if ( p_pid->i_last_cc != -1
          && ts_check_discontinuity( i_cc, p_pid->i_last_cc ) )
        psi_assemble_reset( &p_pid->p_psi_buffer, &p_pid->i_psi_buffer_used );

    p_payload = ts_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    if ( !psi_assemble_empty( &p_pid->p_psi_buffer,
                              &p_pid->i_psi_buffer_used ) )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }

    p_payload = ts_next_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    while ( i_length )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }
}

/*****************************************************************************
 * PID info functions
 *****************************************************************************/
static const char *h222_stream_type_desc(uint8_t i_stream_type) {
    /* See ISO/IEC 13818-1 : 2000 (E) | Table 2-29 - Stream type assignments, Page 66 (48) */
    if (i_stream_type == 0)
        return "Reserved stream";
    switch (i_stream_type) {
        case 0x01: return "11172-2 video (MPEG-1)";
        case 0x02: return "H.262/13818-2 video (MPEG-2) or 11172-2 constrained video";
        case 0x03: return "11172-3 audio (MPEG-1)";
        case 0x04: return "13818-3 audio (MPEG-2)";
        case 0x05: return "H.222.0/13818-1  private sections";
        case 0x06: return "H.222.0/13818-1 PES private data";
        case 0x07: return "13522 MHEG";
        case 0x08: return "H.222.0/13818-1 Annex A - DSM CC";
        case 0x09: return "H.222.1";
        case 0x0A: return "13818-6 type A";
        case 0x0B: return "13818-6 type B";
        case 0x0C: return "13818-6 type C";
        case 0x0D: return "13818-6 type D";
        case 0x0E: return "H.222.0/13818-1 auxiliary";
        case 0x0F: return "13818-7 Audio with ADTS transport syntax";
        case 0x10: return "14496-2 Visual (MPEG-4 part 2 video)";
        case 0x11: return "14496-3 Audio with LATM transport syntax (14496-3/AMD 1)";
        case 0x12: return "14496-1 SL-packetized or FlexMux stream in PES packets";
        case 0x13: return "14496-1 SL-packetized or FlexMux stream in 14496 sections";
        case 0x14: return "ISO/IEC 13818-6 Synchronized Download Protocol";
        case 0x15: return "Metadata in PES packets";
        case 0x16: return "Metadata in metadata_sections";
        case 0x17: return "Metadata in 13818-6 Data Carousel";
        case 0x18: return "Metadata in 13818-6 Object Carousel";
        case 0x19: return "Metadata in 13818-6 Synchronized Download Protocol";
        case 0x1A: return "13818-11 MPEG-2 IPMP stream";
        case 0x1B: return "H.264/14496-10 video (MPEG-4/AVC)";
        case 0x24: return "H.265/23008-2 video (HEVC)";
        case 0x42: return "AVS Video";
        case 0x7F: return "IPMP stream";
        default  : return "Unknown stream";
    }
}

static const char *get_pid_desc(uint16_t i_pid, uint16_t *i_sid) {
    int i, j, k;
    uint8_t i_last_section;
    uint8_t *p_desc;
    uint16_t i_nit_pid = NIT_PID, i_pcr_pid = 0;

    /* Simple cases */
    switch (i_pid)
    {
        case 0x00: return "PAT";
        case 0x01: return "CAT";
        case 0x11: return "SDT";
        case 0x12: return "EPG";
        case 0x14: return "TDT/TOT";
    }

    /* Detect NIT pid */
    if ( psi_table_validate( pp_current_pat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_current_pat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section = psi_table_get_section( pp_current_pat_sections, i );
            uint8_t *p_program;

            j = 0;
            while ( (p_program = pat_get_program( p_section, j++ )) != NULL )
            {
                /* Programs with PID == 0 are actually NIT */
                if ( patn_get_program( p_program ) == 0 )
                {
                    i_nit_pid = patn_get_pid( p_program );
                    break;
                }
            }
        }
    }

    /* Detect EMM pids */
    if ( b_enable_emm && psi_table_validate( pp_current_cat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_current_cat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, i );

            j = 0;
            while ( (p_desc = descl_get_desc( cat_get_descl(p_section), cat_get_desclength(p_section), j++ )) != NULL )
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;

                if ( desc09_get_pid( p_desc ) == i_pid ) {
                    return "EMM";
                }
            }
        }
    }

    /* Detect streams in PMT */
    for ( k = 0; k < i_nb_sids; k++ )
    {
        sid_t *p_sid = pp_sids[k];
        if ( p_sid->i_pmt_pid == i_pid )
        {
            if ( i_sid )
                *i_sid = p_sid->i_sid;
            return "PMT";
        }

        if ( p_sid->i_sid && p_sid->p_current_pmt != NULL )
        {
            uint8_t *p_current_pmt = p_sid->p_current_pmt;
            uint8_t *p_current_es;

            /* The PCR PID can be alone or PCR can be carried in some other PIDs (mostly video)
               so just remember the pid and if it is alone it will be reported as PCR, otherwise
               stream type of the PID will be reported */
            if ( i_pid == pmt_get_pcrpid( p_current_pmt ) ) {
                if ( i_sid )
                    *i_sid = p_sid->i_sid;
                i_pcr_pid = pmt_get_pcrpid( p_current_pmt );
            }

            /* Look for ECMs */
            j = 0;
            while ((p_desc = descs_get_desc( pmt_get_descs( p_current_pmt ), j++ )) != NULL)
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;

                if ( desc09_get_pid( p_desc ) == i_pid ) {
                    if ( i_sid )
                        *i_sid = p_sid->i_sid;
                    return "ECM";
                }
            }

            /* Detect stream types */
            j = 0;
            while ( (p_current_es = pmt_get_es( p_current_pmt, j++ )) != NULL )
            {
                if ( pmtn_get_pid( p_current_es ) == i_pid )
                {
                    if ( i_sid )
                        *i_sid = p_sid->i_sid;
                    return h222_stream_type_desc( pmtn_get_streamtype( p_current_es ) );
                }
            }
        }
    }

    /* Are there any other PIDs? */
    if (i_pid == i_nit_pid)
        return "NIT";

    if (i_pid == i_pcr_pid)
        return "PCR";

    return "...";
}

/*****************************************************************************
 * Functions that return packed sections
 *****************************************************************************/
uint8_t *demux_get_current_packed_PAT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_pat_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_CAT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_cat_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_NIT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_nit_sections, pi_pack_size );
}

uint8_t *demux_get_current_packed_SDT( unsigned int *pi_pack_size ) {
    return psi_pack_sections( pp_current_sdt_sections, pi_pack_size );
}

uint8_t *demux_get_packed_EIT( uint16_t i_sid, uint8_t start_table, uint8_t end_table, unsigned int *eit_size ) {
    unsigned int i, r;

    *eit_size = 0;
    sid_t *p_sid = FindSID( i_sid );
    if ( p_sid == NULL )
        return NULL;

    /* Calculate eit table size (sum of all sections in all tables between start_start and end_table) */
    for ( i = start_table; i <= end_table; i++ ) {
        uint8_t eit_table_idx = i - EIT_TABLE_ID_PF_ACTUAL;
        if ( eit_table_idx >= MAX_EIT_TABLES )
            continue;
        uint8_t **eit_sections = p_sid->eit_table[eit_table_idx].data;
        for ( r = 0; r < PSI_TABLE_MAX_SECTIONS; r++ ) {
            uint8_t *p_eit = eit_sections[r];
            if ( !p_eit )
                continue;
            uint16_t psi_length = psi_get_length( p_eit ) + PSI_HEADER_SIZE;
            *eit_size += psi_length;
        }
    }

    uint8_t *p_flat_section = malloc( *eit_size );
    if ( !p_flat_section )
        return NULL;

    /* Copy sections */
    unsigned int i_pos = 0;
    for ( i = start_table; i <= end_table; i++ ) {
        uint8_t eit_table_idx = i - EIT_TABLE_ID_PF_ACTUAL;
        if ( eit_table_idx >= MAX_EIT_TABLES )
            continue;
        uint8_t **eit_sections = p_sid->eit_table[eit_table_idx].data;
        for ( r = 0; r < PSI_TABLE_MAX_SECTIONS; r++ ) {
            uint8_t *p_eit = eit_sections[r];
            if ( !p_eit )
                continue;
            uint16_t psi_length = psi_get_length( p_eit ) + PSI_HEADER_SIZE;
            memcpy( p_flat_section + i_pos, p_eit, psi_length );
            i_pos += psi_length;
            /* eit_print( p_eit, msg_Dbg, NULL, demux_Iconv, NULL, PRINT_TEXT ); */
        }
    }
    return p_flat_section;
}

uint8_t *demux_get_packed_EIT_pf( uint16_t service_id, unsigned int *pi_pack_size ) {
    return demux_get_packed_EIT( service_id, EIT_TABLE_ID_PF_ACTUAL, EIT_TABLE_ID_PF_ACTUAL, pi_pack_size );
}

uint8_t *demux_get_packed_EIT_schedule( uint16_t service_id, unsigned int *pi_pack_size ) {
    return demux_get_packed_EIT( service_id, EIT_TABLE_ID_SCHED_ACTUAL_FIRST, EIT_TABLE_ID_SCHED_ACTUAL_LAST, pi_pack_size );
}

uint8_t *demux_get_packed_PMT( uint16_t i_sid, unsigned int *pi_pack_size ) {
    sid_t *p_sid = FindSID( i_sid );
    if ( p_sid != NULL && p_sid->p_current_pmt && pmt_validate( p_sid->p_current_pmt ) )
        return psi_pack_section( p_sid->p_current_pmt, pi_pack_size );
    return NULL;
}

inline void demux_get_PID_info( uint16_t i_pid, uint8_t *p_data ) {
    ts_pid_info_t *p_info = (ts_pid_info_t *)p_data;
    *p_info = p_pids[i_pid].info;
}

inline void demux_get_PIDS_info( uint8_t *p_data ) {
    int i_pid;
    for (i_pid = 0; i_pid < MAX_PIDS; i_pid++ )
        demux_get_PID_info( i_pid, p_data + ( i_pid * sizeof(ts_pid_info_t) ) );
}
