/*****************************************************************************
 * dvblast.h
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011, 2015-2016 VideoLAN
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
#include <sys/types.h> /* u_int16_t */
#include <netinet/udp.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#include "config.h"

#ifndef container_of
#   define container_of(ptr, type, member) ({                               \
        const typeof( ((type *)0)->member ) *_mptr = (ptr);                 \
        (type *)( (char *)_mptr - offsetof(type,member) );})
#endif

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

#if defined(__FreeBSD__) || defined(__APPLE__)
struct iphdr {
    unsigned int ihl:4;
    unsigned int version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};
#endif

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

typedef struct dvb_string_t
{
    uint8_t *p;
    size_t i;
} dvb_string_t;

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
    uint16_t i_network_id;
    dvb_string_t network_name;
    dvb_string_t service_name;
    dvb_string_t provider_name;
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
    uint16_t i_new_sid;
    uint16_t i_onid;
    bool b_passthrough;

    /* for pidmap from config file */
    bool b_do_remap;
    uint16_t pi_confpids[N_MAP_PIDS];
} output_config_t;

typedef struct output_t
{
    output_config_t config;

    /* output */
    int i_handle;
    packet_t *p_packets, *p_last_packet;
    packet_t *p_packet_lifo;
    unsigned int i_packet_count;
    uint16_t i_seqnum;

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
    /* incomplete PID (only PCR packets) */
    uint16_t i_pcr_pid;
    // Arrays used for mapping pids.
    // newpids is indexed using the original pid
    uint16_t pi_newpids[MAX_PIDS];
    uint16_t pi_freepids[MAX_PIDS];   // used where multiple streams of the same type are used

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

extern struct ev_loop *event_loop;
extern int i_syslog;
extern int i_verbose;
extern output_t **pp_outputs;
extern int i_nb_outputs;
extern output_t output_dup;
extern bool b_passthrough;
extern char *psz_srv_socket;
extern int i_adapter;
extern int i_fenum;
extern int i_canum;
extern char *psz_delsys;
extern int i_dvr_buffer_size;
extern int i_frequency;
extern char *psz_lnb_type;
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
extern mtime_t i_quit_timeout_duration;
extern int b_budget_mode;
extern int b_any_type;
extern int b_select_pmts;
extern int b_random_tsid;
extern int dvb_plp_id;
extern bool b_enable_emm;
extern bool b_enable_ecm;
extern mtime_t i_wallclock;
extern char *psz_udp_src;
extern int i_asi_adapter;
extern const char *psz_native_charset;
extern enum print_type_t i_print_type;
extern bool b_print_enabled;
extern FILE *print_fh;
extern mtime_t i_print_period;
extern mtime_t i_es_timeout;

/* pid mapping */
extern bool b_do_remap;
extern uint16_t pi_newpids[N_MAP_PIDS];
extern void init_pid_mapping( output_t * );

extern void (*pf_Open)( void );
extern void (*pf_Reset)( void );
extern int (*pf_SetFilter)( uint16_t i_pid );
extern void (*pf_UnsetFilter)( int i_fd, uint16_t i_pid );

/*****************************************************************************
 * Prototypes
 *****************************************************************************/

void config_Init( output_config_t *p_config );
void config_Free( output_config_t *p_config );

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

void dvb_string_init(dvb_string_t *p_dvb_string);
void dvb_string_clean(dvb_string_t *p_dvb_string);
void dvb_string_copy(dvb_string_t *p_dst, const dvb_string_t *p_src);
int dvb_string_cmp(const dvb_string_t *p_1, const dvb_string_t *p_2);

mtime_t mdate( void );
void msleep( mtime_t delay );
void hexDump( uint8_t *p_data, uint32_t i_len );
struct addrinfo *ParseNodeService( char *_psz_string, char **ppsz_end,
                                   uint16_t i_default_port );
char *config_stropt( const char *psz_string );
void config_ReadFile(void);

uint8_t *psi_pack_section( uint8_t *p_sections, unsigned int *pi_size );
uint8_t *psi_pack_sections( uint8_t **pp_sections, unsigned int *pi_size );
uint8_t **psi_unpack_sections( uint8_t *p_flat_sections, unsigned int i_size );

void dvb_Open( void );
void dvb_Reset( void );
int dvb_SetFilter( uint16_t i_pid );
void dvb_UnsetFilter( int i_fd, uint16_t i_pid );
uint8_t dvb_FrontendStatus( uint8_t *p_answer, ssize_t *pi_size );

void udp_Open( void );
void udp_Reset( void );
int udp_SetFilter( uint16_t i_pid );
void udp_UnsetFilter( int i_fd, uint16_t i_pid );

void asi_Open( void );
void asi_Reset( void );
int asi_SetFilter( uint16_t i_pid );
void asi_UnsetFilter( int i_fd, uint16_t i_pid );

#ifdef HAVE_ASI_DELTACAST_SUPPORT
void asi_deltacast_Open( void );
void asi_deltacast_Reset( void );
int asi_deltacast_SetFilter( uint16_t i_pid );
void asi_deltacast_UnsetFilter( int i_fd, uint16_t i_pid );
#endif

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
uint8_t *demux_get_packed_EIT_pf( uint16_t service_id, unsigned int *pi_pack_size );
uint8_t *demux_get_packed_EIT_schedule( uint16_t service_id, unsigned int *pi_pack_size );
void demux_get_PID_info( uint16_t i_pid, uint8_t *p_data );
void demux_get_PIDS_info( uint8_t *p_data );

output_t *output_Create( const output_config_t *p_config );
int output_Init( output_t *p_output, const output_config_t *p_config );
void output_Close( output_t *p_output );
void output_Put( output_t *p_output, block_t *p_block );
output_t *output_Find( const output_config_t *p_config );
void output_Change( output_t *p_output, const output_config_t *p_config );
void outputs_Init( void );
void outputs_Close( int i_num_outputs );

void comm_Open( void );
void comm_Close( void );

block_t *block_New( void );
void block_Delete( block_t *p_block );
void block_Vacuum( void );

/*****************************************************************************
 * block_DeleteChain
 *****************************************************************************/
static inline void block_DeleteChain( block_t *p_block )
{
    while ( p_block != NULL )
    {
        block_t *p_next = p_block->p_next;
        block_Delete( p_block );
        p_block = p_next;
    }
}
