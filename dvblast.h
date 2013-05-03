/*****************************************************************************
 * dvblast.h
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Andy Gatward <a.j.gatward@reading.ac.uk>
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

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/udp.h>
#include <netinet/ip.h>

#include "config.h"

/* Defines for pid mapping */
#define N_MAP_PIDS                 4
/* Offsets in the command line args for the pid mapping */
typedef enum
{
    I_PMTPID = 0, I_APID, I_VPID, I_SPUPID
} pidmap_offset;

/* Impossible PID value */
#define UNUSED_PID (MAX_PIDS + 1)

/*****************************************************************************
 * Raw udp packet structure with flexible-array payload
 *****************************************************************************/
struct udpheader { // FAVOR_BSD hell ...
  u_int16_t source;
  u_int16_t dest;
  u_int16_t len;
  u_int16_t check;
};

struct udprawpkt {
    struct  iphdr iph;
    struct  udpheader udph;
    uint8_t payload[];
} __attribute__((packed));

/*****************************************************************************
 * Output configuration flags (for output_t -> i_config) - bit values
 * Bit  0 : Set for watch mode
 * Bit  1 : Set output still present
 * Bit  2 : Set if output is valid (replaces m_addr != 0 tests)
 * Bit  3 : Set for UDP, otherwise use RTP if a network stream
 * Bit  4 : Set for file / FIFO output, unset for network (future use)
 * Bit  5 : Set if DVB conformance tables are inserted
 * Bit  6 : Set if DVB EIT schedule tables are forwarded
 * Bit  7 : Set for RAW socket output
 *****************************************************************************/

#define OUTPUT_WATCH         0x01
#define OUTPUT_STILL_PRESENT 0x02
#define OUTPUT_VALID         0x04
#define OUTPUT_UDP           0x08
#define OUTPUT_FILE          0x10
#define OUTPUT_DVB           0x20
#define OUTPUT_EPG           0x40
#define OUTPUT_RAW           0x80

typedef int64_t mtime_t;

typedef struct block_t
{
    uint8_t p_ts[TS_SIZE];
    int i_refcount;
    mtime_t i_dts;
    uint16_t tmp_pid;
    struct block_t *p_next;
} block_t;

typedef struct packet_t packet_t;

typedef struct output_config_t
{
    /* identity */
    int i_family;
    struct sockaddr_storage connect_addr;
    struct sockaddr_storage bind_addr;
    int i_if_index_v6;

    /* common config */
    char *psz_displayname;
    uint64_t i_config;

    /* output config */
    char *psz_service_name;
    char *psz_service_provider;
    uint8_t pi_ssrc[4];
    mtime_t i_output_latency, i_max_retention;
    int i_ttl;
    uint8_t i_tos;
    int i_mtu;
    char *psz_srcaddr; /* raw packets */
    int i_srcport;

    /* demux config */
    int i_tsid;
    uint16_t i_sid; /* 0 if raw mode */
    uint16_t *pi_pids;
    int i_nb_pids;

    /* for pidmap from config file */
    bool b_do_remap;
    uint16_t pi_confpids[N_MAP_PIDS];
} output_config_t;

typedef struct output_estype_options_t
{
    int i_es_type;
    int i_tag;
    char *psz_option;
} output_estype_options_t;

typedef struct output_t
{
    output_config_t config;

    /* output */
    int i_handle;
    packet_t *p_packets, *p_last_packet;
    uint16_t i_seqnum;
    mtime_t i_ref_timestamp;
    mtime_t i_ref_wallclock;

    /* demux */
    int i_nb_errors;
    mtime_t i_last_error;
    uint8_t *p_pat_section;
    uint8_t i_pat_version, i_pat_cc;
    uint8_t *p_pmt_section;
    uint8_t i_pmt_version, i_pmt_cc;
    uint8_t *p_nit_section;
    uint8_t i_nit_version, i_nit_cc;
    uint8_t *p_sdt_section;
    uint8_t i_sdt_version, i_sdt_cc;
    block_t *p_eit_ts_buffer;
    uint8_t i_eit_ts_buffer_offset, i_eit_cc;
    uint16_t i_tsid;
    // Arrays used for mapping pids.
    // newpids is indexed using the original pid
    uint16_t pi_newpids[MAX_PIDS];
    uint16_t pi_freepids[MAX_PIDS];   // used where multiple streams of the same type are used

    /* For pidmap from config file */
    bool b_do_remap;
    uint16_t pi_confpids[N_MAP_PIDS];

    struct udprawpkt raw_pkt_header;
} output_t;

typedef struct ts_pid_info {
    mtime_t  i_first_packet_ts;         /* Time of the first seen packet */
    mtime_t  i_last_packet_ts;          /* Time of the last seen packet */
    unsigned long i_packets;            /* How much packets have been seen */
    unsigned long i_cc_errors;          /* Countinuity counter errors */
    unsigned long i_transport_errors;   /* Transport errors */
    unsigned long i_bytes_per_sec;      /* How much bytes were process last second */
    uint8_t  i_scrambling;              /* Scrambling bits from the last ts packet */
    /* 0 = Not scrambled
       1 = Reserved for future use
       2 = Scrambled with even key
       3 = Scrambled with odd key */
} ts_pid_info_t;

extern int i_syslog;
extern int i_verbose;
extern output_t **pp_outputs;
extern int i_nb_outputs;
extern output_t output_dup;
extern char *psz_srv_socket;
extern int i_comm_fd;
extern int i_adapter;
extern int i_fenum;
extern int i_canum;
extern char *psz_delsys;
extern int i_frequency;
extern int i_srate;
extern int i_satnum;
extern int i_uncommitted;
extern int i_fec;
extern int i_rolloff;
extern int i_voltage;
extern int b_tone;
extern int i_bandwidth;
extern int i_inversion;
extern char *psz_modulation;
extern int i_pilot;
extern int i_mis;
extern int i_fec_lp;
extern int i_guard;
extern int i_transmission;
extern int i_hierarchy;
extern mtime_t i_frontend_timeout_duration;
extern mtime_t i_quit_timeout;
extern mtime_t i_quit_timeout_duration;
extern int b_budget_mode;
extern int b_any_type;
extern int b_select_pmts;
extern int b_random_tsid;
extern uint16_t i_network_id;
extern uint8_t *p_network_name;
extern size_t i_network_name_size;
extern mtime_t i_wallclock;
extern volatile int b_conf_reload;
extern volatile int b_exit_now;
extern int i_comm_fd;
extern char *psz_udp_src;
extern int i_asi_adapter;
extern const char *psz_native_charset;
extern const char *psz_dvb_charset;
extern enum print_type_t i_print_type;

/* pid mapping */
extern bool b_do_remap;
extern uint16_t pi_newpids[N_MAP_PIDS];
extern void init_pid_mapping( output_t * );

extern void (*pf_Open)( void );
extern block_t * (*pf_Read)( mtime_t i_poll_timeout );
extern void (*pf_Reset)( void );
extern int (*pf_SetFilter)( uint16_t i_pid );
extern void (*pf_UnsetFilter)( int i_fd, uint16_t i_pid );

/*****************************************************************************
 * Prototypes
 *****************************************************************************/

void config_Init( output_config_t *p_config );
void config_Free( output_config_t *p_config );
bool config_ParseHost( output_config_t *p_config, char *psz_string );

/* Connect/Disconnect from syslogd */
void msg_Connect( const char *ident );
void msg_Disconnect( void );

/* */
__attribute__ ((format(printf, 2, 3))) void msg_Info( void *_unused, const char *psz_format, ... );
__attribute__ ((format(printf, 2, 3))) void msg_Err( void *_unused, const char *psz_format, ... );
__attribute__ ((format(printf, 2, 3))) void msg_Warn( void *_unused, const char *psz_format, ... );
__attribute__ ((format(printf, 2, 3))) void msg_Dbg( void *_unused, const char *psz_format, ... );
__attribute__ ((format(printf, 2, 3))) void msg_Raw( void *_unused, const char *psz_format, ... );

/* */
bool streq(char *a, char *b);
char * xstrdup(char *str);
mtime_t mdate( void );
void msleep( mtime_t delay );
void hexDump( uint8_t *p_data, uint32_t i_len );
struct addrinfo *ParseNodeService( char *_psz_string, char **ppsz_end,
                                   uint16_t i_default_port );

uint8_t *psi_pack_section( uint8_t *p_sections, unsigned int *pi_size );
uint8_t *psi_pack_sections( uint8_t **pp_sections, unsigned int *pi_size );
uint8_t **psi_unpack_sections( uint8_t *p_flat_sections, unsigned int i_size );

void dvb_Open( void );
void dvb_Reset( void );
block_t * dvb_Read( mtime_t i_poll_timeout );
int dvb_SetFilter( uint16_t i_pid );
void dvb_UnsetFilter( int i_fd, uint16_t i_pid );
uint8_t dvb_FrontendStatus( uint8_t *p_answer, ssize_t *pi_size );

void udp_Open( void );
block_t * udp_Read( mtime_t i_poll_timeout );
void udp_Reset( void );
int udp_SetFilter( uint16_t i_pid );
void udp_UnsetFilter( int i_fd, uint16_t i_pid );

void asi_Open( void );
block_t * asi_Read( mtime_t i_poll_timeout );
void asi_Reset( void );
int asi_SetFilter( uint16_t i_pid );
void asi_UnsetFilter( int i_fd, uint16_t i_pid );

void demux_Open( void );
void demux_Run( block_t *p_ts );
void demux_Change( output_t *p_output, const output_config_t *p_config );
void demux_ResendCAPMTs( void );
bool demux_PIDIsSelected( uint16_t i_pid );
char *demux_Iconv(void *_unused, const char *psz_encoding,
                  char *p_string, size_t i_length);
void demux_Close( void );

uint8_t *demux_get_current_packed_PAT( unsigned int *pi_pack_size );
uint8_t *demux_get_current_packed_CAT( unsigned int *pi_pack_size );
uint8_t *demux_get_current_packed_NIT( unsigned int *pi_pack_size );
uint8_t *demux_get_current_packed_SDT( unsigned int *pi_pack_size );
uint8_t *demux_get_packed_PMT( uint16_t service_id, unsigned int *pi_pack_size );
void demux_get_PID_info( uint16_t i_pid, uint8_t *p_data );
void demux_get_PIDS_info( uint8_t *p_data );

output_t *output_Create( const output_config_t *p_config );
int output_Init( output_t *p_output, const output_config_t *p_config );
void output_Close( output_t *p_output );
void output_Put( output_t *p_output, block_t *p_block );
mtime_t output_Send( void );
output_t *output_Find( const output_config_t *p_config );
void output_Change( output_t *p_output, const output_config_t *p_config );
void outputs_Close( int i_num_outputs );

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
