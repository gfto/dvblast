/*****************************************************************************
 * dvblast.h
 *****************************************************************************
 * Copyright (C) 2004, 2008-2009 VideoLAN
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

#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pmt.h>

#define DEFAULT_PORT 3001
#define TS_SIZE 188
#define NB_BLOCKS 7
#define RTP_SIZE 12
#define EMPTY_PID 8192
#define PADDING_PID 8191
#define WATCHDOG_WAIT 10000000LL
#define MAX_ERRORS 100000
#define DEFAULT_VERBOSITY 3

typedef int64_t mtime_t;

typedef struct block_t
{
    uint8_t p_ts[TS_SIZE];
    int i_refcount;
    struct block_t *p_next;
} block_t;

typedef struct output_t
{
    in_addr_t i_maddr;
    uint16_t i_port;

    /* output */
    int i_handle;
    block_t *pp_blocks[NB_BLOCKS];
    int i_depth;
    uint16_t i_cc;
    mtime_t i_ref_timestamp;
    mtime_t i_ref_wallclock;

    /* demux */
    int i_nb_errors;
    mtime_t i_last_error;
    dvbpsi_psi_section_t *p_pat_section;
    uint8_t i_pat_version, i_pat_cc;
    dvbpsi_psi_section_t *p_pmt_section;
    uint8_t i_pmt_version, i_pmt_cc;
    dvbpsi_psi_section_t *p_sdt_section;
    uint8_t i_sdt_cc;
    uint8_t i_eit_cc;
    uint16_t i_ts_id;

    /* configuration */
    uint16_t i_sid; /* 0 if raw mode */
    uint16_t *pi_pids;
    int i_nb_pids;
    int b_watch;
    int b_rawudp;
    int b_still_present;
} output_t;

extern int i_verbose;
extern output_t **pp_outputs;
extern int i_nb_outputs;
extern output_t output_dup;
extern char *psz_srv_socket;
extern int i_ttl;
extern in_addr_t i_ssrc;
extern int i_adapter;
extern int i_fenum;
extern int i_frequency;
extern int i_srate;
extern int i_satnum;
extern int i_voltage;
extern int b_tone;
extern int i_bandwidth;
extern char *psz_modulation;
extern int b_budget_mode;
extern int b_slow_cam;
extern int b_output_udp;
extern int b_enable_epg;
extern int b_unique_tsid;
extern volatile int b_hup_received;
extern mtime_t i_ca_timeout;
extern int i_comm_fd;

/*****************************************************************************
 * Prototypes
 *****************************************************************************/
void msg_Info( void *_unused, const char *psz_format, ... );
void msg_Err( void *_unused, const char *psz_format, ... );
void msg_Warn( void *_unused, const char *psz_format, ... );
void msg_Dbg( void *_unused, const char *psz_format, ... );
void msg_Raw( void *_unused, const char *psz_format, ... );
mtime_t mdate( void );
void msleep( mtime_t delay );
void hexDump( uint8_t *p_data, uint32_t i_len );

void dvb_Open( void );
block_t * dvb_Read( void );
int dvb_SetFilter( uint16_t i_pid );
void dvb_UnsetFilter( int i_fd, uint16_t i_pid );
uint8_t dvb_FrontendStatus( uint8_t *p_answer, ssize_t *pi_size );

void demux_Open( void );
void demux_Run( void );
void demux_Hup( void );
void demux_Change( output_t *p_output, uint16_t i_sid,
                   uint16_t *pi_pids, int i_nb_pids );
void demux_ResendCAPMTs( void );
int PIDIsSelected( uint16_t i_pid );

output_t *output_Create( in_addr_t i_maddr, uint16_t i_port );
int output_Init( output_t *p_output, in_addr_t i_maddr, uint16_t i_port );
void output_Close( output_t *p_output );
void output_Put( output_t *p_output, block_t *p_block );

void comm_Open( void );
void comm_Read( void );

/*****************************************************************************
 * block_New
 *****************************************************************************/
static inline block_t *block_New( void )
{
    block_t *p_block = malloc(sizeof(block_t));
    p_block->p_next = NULL;
    p_block->i_refcount = 1;
    return p_block;
}

/*****************************************************************************
 * block_Delete
 *****************************************************************************/
static inline void block_Delete( block_t *p_block )
{
    free( p_block );
}

/*****************************************************************************
 * block_DeleteChain
 *****************************************************************************/
static inline void block_DeleteChain( block_t *p_block )
{
    while ( p_block != NULL )
    {
        block_t *p_next = p_block->p_next;
        free( p_block );
        p_block = p_next;
    }
}

/*****************************************************************************
 * block_GetSync
 *****************************************************************************/
static inline uint8_t block_GetSync( block_t *p_block )
{
    return p_block->p_ts[0];
}

/*****************************************************************************
 * block_HasTransportError
 *****************************************************************************/
static inline uint8_t block_HasTransportError( block_t *p_block )
{
    return p_block->p_ts[1] & 0x80;
}

/*****************************************************************************
 * block_UnitStart
 *****************************************************************************/
static inline uint8_t block_UnitStart( block_t *p_block )
{
    return p_block->p_ts[1] & 0x40;
}

/*****************************************************************************
 * block_GetPID
 *****************************************************************************/
static inline uint16_t block_GetPID( block_t *p_block )
{
    return (((uint16_t)p_block->p_ts[1] & 0x1f) << 8)
                | p_block->p_ts[2];
}

/*****************************************************************************
 * block_GetScrambling
 *****************************************************************************/
static inline uint8_t block_GetScrambling( block_t *p_block )
{
    return p_block->p_ts[3] & 0xc0;
}

/*****************************************************************************
 * block_GetCC
 *****************************************************************************/
static inline uint8_t block_GetCC( block_t *p_block )
{
    return p_block->p_ts[3] & 0xf;
}

/*****************************************************************************
 * block_HasPCR
 *****************************************************************************/
static inline int block_HasPCR( block_t *p_block )
{
    return ( p_block->p_ts[3] & 0x20 ) && /* adaptation field present */
           ( p_block->p_ts[4] >= 7 ) && /* adaptation field size */
           ( p_block->p_ts[5] & 0x10 ); /* has PCR */
}

/*****************************************************************************
 * block_GetPCR
 *****************************************************************************/
static inline mtime_t block_GetPCR( block_t *p_block )
{
    return ( (mtime_t)p_block->p_ts[6] << 25 ) |
           ( (mtime_t)p_block->p_ts[7] << 17 ) |
           ( (mtime_t)p_block->p_ts[8] << 9 ) |
           ( (mtime_t)p_block->p_ts[9] << 1 ) |
           ( (mtime_t)p_block->p_ts[10] >> 7 );
}

/*****************************************************************************
 * block_GetPayload
 *****************************************************************************/
static inline uint8_t *block_GetPayload( block_t *p_block )
{
    if ( !(p_block->p_ts[3] & 0x10) )
        return NULL;
    if ( !(p_block->p_ts[3] & 0x20) )
        return &p_block->p_ts[4];
    return &p_block->p_ts[ 5 + p_block->p_ts[4] ];
}

