/*****************************************************************************
 * dvblast.c
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include "dvblast.h"

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <bitstream/dvb/si.h>
#include <bitstream/ietf/rtp.h>

#include "mrtg-cnt.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
mtime_t i_wallclock = 0;
output_t **pp_outputs = NULL;
int i_nb_outputs = 0;
output_t output_dup;
static char *psz_conf_file = NULL;
char *psz_srv_socket = NULL;
static int i_priority = -1;
int i_adapter = 0;
int i_fenum = 0;
int i_canum = 0;
char *psz_delsys = NULL;
int i_frequency = 0;
int i_inversion = -1;
int i_srate = 27500000;
int i_fec = 999;
int i_rolloff = 35;
int i_satnum = 0;
int i_uncommitted = 0;
int i_voltage = 13;
int b_tone = 0;
int i_bandwidth = 8;
char *psz_modulation = NULL;
int i_pilot = -1;
int i_mis = 0;
int i_fec_lp = 999;
int i_guard = -1;
int i_transmission = -1;
int i_hierarchy = -1;
mtime_t i_frontend_timeout_duration = DEFAULT_FRONTEND_TIMEOUT;
mtime_t i_quit_timeout = 0;
mtime_t i_quit_timeout_duration = 0;
int b_budget_mode = 0;
int b_any_type = 0;
int b_select_pmts = 0;
int b_random_tsid = 0;
uint16_t i_network_id = 0xffff;
uint8_t *p_network_name;
size_t i_network_name_size;
char *psz_udp_src = NULL;
int i_asi_adapter = 0;
const char *psz_native_charset = "UTF-8";
const char *psz_dvb_charset = "ISO_8859-1";
const char *psz_provider_name = NULL;
print_type_t i_print_type = -1;

volatile sig_atomic_t b_conf_reload = 0;
volatile sig_atomic_t b_exit_now = 0;
int i_verbose = DEFAULT_VERBOSITY;
int i_syslog = 0;
char *psz_syslog_ident = NULL;

bool b_enable_emm = false;
bool b_enable_ecm = false;

uint8_t pi_ssrc_global[4] = { 0, 0, 0, 0 };
static bool b_udp_global = false;
static bool b_dvb_global = false;
static bool b_epg_global = false;
static mtime_t i_latency_global = DEFAULT_OUTPUT_LATENCY;
static mtime_t i_retention_global = DEFAULT_MAX_RETENTION;
static int i_ttl_global = 64;

/* TPS Input log filename */
char * psz_mrtg_file = NULL;

/* PID mapping */
bool b_do_remap = false;
uint16_t pi_newpids[ N_MAP_PIDS ];  /* pmt, audio, video, spu */

void (*pf_Open)( void ) = NULL;
block_t * (*pf_Read)( mtime_t i_poll_timeout ) = NULL;
void (*pf_Reset)( void ) = NULL;
int (*pf_SetFilter)( uint16_t i_pid ) = NULL;
void (*pf_UnsetFilter)( int i_fd, uint16_t i_pid ) = NULL;

/*****************************************************************************
 * Configuration files
 *****************************************************************************/
void config_Init( output_config_t *p_config )
{
    memset( p_config, 0, sizeof(output_config_t) );

    p_config->psz_displayname = NULL;
    p_config->psz_service_name = NULL;
    p_config->psz_service_provider = NULL;

    p_config->i_family = AF_UNSPEC;
    p_config->connect_addr.ss_family = AF_UNSPEC;
    p_config->bind_addr.ss_family = AF_UNSPEC;
    p_config->i_if_index_v6 = -1;

    p_config->pi_pids = NULL;
    p_config->b_do_remap = false;
    unsigned int i;
    for ( i = 0; i < N_MAP_PIDS; i++ ) {
        p_config->pi_confpids[i]  = UNUSED_PID;
    }
}

void config_Free( output_config_t *p_config )
{
    free( p_config->psz_displayname );
    free( p_config->psz_service_name );
    free( p_config->psz_service_provider );
    free( p_config->pi_pids );
}

static void config_Defaults( output_config_t *p_config )
{
    config_Init( p_config );

    p_config->i_config = (b_udp_global ? OUTPUT_UDP : 0) |
                         (b_dvb_global ? OUTPUT_DVB : 0) |
                         (b_epg_global ? OUTPUT_EPG : 0);
    p_config->i_max_retention = i_retention_global;
    p_config->i_output_latency = i_latency_global;
    p_config->i_tsid = -1;
    p_config->i_ttl = i_ttl_global;
    memcpy( p_config->pi_ssrc, pi_ssrc_global, 4 * sizeof(uint8_t) );
}

static char *config_stropt( char *psz_string )
{
    char *ret, *tmp;
    if ( !psz_string || strlen( psz_string ) == 0 )
        return NULL;
    ret = tmp = strdup( psz_string );
    while (*tmp) {
        if (*tmp == '_')
            *tmp = ' ';
        if (*tmp == '/') {
            *tmp = '\0';
            break;
        }
        tmp++;
    }
    return ret;
}

bool config_ParseHost( output_config_t *p_config, char *psz_string )
{
    struct addrinfo *p_ai;
    int i_mtu;

    p_config->psz_displayname = strdup( psz_string );

    p_ai = ParseNodeService( psz_string, &psz_string, DEFAULT_PORT );
    if ( p_ai == NULL ) return false;
    memcpy( &p_config->connect_addr, p_ai->ai_addr, p_ai->ai_addrlen );
    freeaddrinfo( p_ai );

    p_config->i_family = p_config->connect_addr.ss_family;
    if ( p_config->i_family == AF_UNSPEC ) return false;

    if ( psz_string == NULL || !*psz_string ) goto end;

    if ( *psz_string == '@' )
    {
        psz_string++;
        p_ai = ParseNodeService( psz_string, &psz_string, 0 );
        if ( p_ai == NULL || p_ai->ai_family != p_config->i_family )
            msg_Warn( NULL, "invalid bind address" );
        else
            memcpy( &p_config->bind_addr, p_ai->ai_addr, p_ai->ai_addrlen );
        freeaddrinfo( p_ai );
    }

    while ( (psz_string = strchr( psz_string, '/' )) != NULL )
    {
        *psz_string++ = '\0';

#define IS_OPTION( option ) (!strncasecmp( psz_string, option, strlen(option) ))
#define ARG_OPTION( option ) (psz_string + strlen(option))

        if ( IS_OPTION("udp") )
            p_config->i_config |= OUTPUT_UDP;
        else if ( IS_OPTION("dvb") )
            p_config->i_config |= OUTPUT_DVB;
        else if ( IS_OPTION("epg") )
            p_config->i_config |= OUTPUT_EPG;
        else if ( IS_OPTION("tsid=") )
            p_config->i_tsid = strtol( ARG_OPTION("tsid="), NULL, 0 );
        else if ( IS_OPTION("retention=") )
            p_config->i_max_retention = strtoll( ARG_OPTION("retention="),
                                                 NULL, 0 ) * 1000;
        else if ( IS_OPTION("latency=") )
            p_config->i_output_latency = strtoll( ARG_OPTION("latency="),
                                                  NULL, 0 ) * 1000;
        else if ( IS_OPTION("ttl=") )
            p_config->i_ttl = strtol( ARG_OPTION("ttl="), NULL, 0 );
        else if ( IS_OPTION("tos=") )
            p_config->i_tos = strtol( ARG_OPTION("tos="), NULL, 0 );
        else if ( IS_OPTION("mtu=") )
            p_config->i_mtu = strtol( ARG_OPTION("mtu="), NULL, 0 );
        else if ( IS_OPTION("ifindex=") )
            p_config->i_if_index_v6 = strtol( ARG_OPTION("ifindex="), NULL, 0 );
        else if ( IS_OPTION("srvname=")  )
        {
            if ( p_config->psz_service_name )
                free( p_config->psz_service_name );
            p_config->psz_service_name = config_stropt( ARG_OPTION("srvname=") );
        }
        else if ( IS_OPTION("srvprovider=") )
        {
            if ( !p_config->psz_service_provider )
                free( p_config->psz_service_provider );
            p_config->psz_service_provider = config_stropt( ARG_OPTION("srvprovider=") );
        }
        else if ( IS_OPTION("ssrc=") )
        {
            in_addr_t i_addr = inet_addr( ARG_OPTION("ssrc=") );
            memcpy( p_config->pi_ssrc, &i_addr, 4 * sizeof(uint8_t) );
        }
        else if ( IS_OPTION("pidmap=") )
        {
            char *str1;
            char *saveptr = NULL;
            char *tok = NULL;
            int i, i_newpid;
            for (i = 0, str1 = config_stropt( (ARG_OPTION("pidmap="))); i < N_MAP_PIDS; i++, str1 = NULL)
            {
                tok = strtok_r(str1, ",", &saveptr);
                if ( !tok )
                    break;
                i_newpid = strtoul(tok, NULL, 0);
                if ( !i_newpid ) {
                     msg_Warn( NULL, "Invalid output pidmap setting" );
                }
                p_config->pi_confpids[i] = i_newpid;
            }
            p_config->b_do_remap = true;
        }
        else
            msg_Warn( NULL, "unrecognized option %s", psz_string );

#undef IS_OPTION
#undef ARG_OPTION
    }

    if ( !p_config->psz_service_provider && psz_provider_name )
        p_config->psz_service_provider = strdup( psz_provider_name );

end:
    i_mtu = p_config->i_family == AF_INET6 ? DEFAULT_IPV6_MTU :
            DEFAULT_IPV4_MTU;

    if ( !p_config->i_mtu )
        p_config->i_mtu = i_mtu;
    else if ( p_config->i_mtu < TS_SIZE + RTP_HEADER_SIZE )
    {
        msg_Warn( NULL, "invalid MTU %d, setting %d", p_config->i_mtu, i_mtu );
        p_config->i_mtu = i_mtu;
    }

    return true;
}

static void config_Print( output_config_t *p_config )
{
    const char *psz_base = "conf: %s config=0x%"PRIx64" sid=%hu pids[%d]=";
    size_t i_len = strlen(psz_base) + 6 * p_config->i_nb_pids + 1;
    char psz_format[i_len];
    int i, j = strlen(psz_base);

    strcpy( psz_format, psz_base );
    for ( i = 0; i < p_config->i_nb_pids; i++ )
        j += sprintf( psz_format + j, "%u,", p_config->pi_pids[i] );
    psz_format[j - 1] = '\0';

    msg_Dbg( NULL, psz_format, p_config->psz_displayname, p_config->i_config,
             p_config->i_sid, p_config->i_nb_pids );
}

static void config_ReadFile( char *psz_file )
{
    FILE *p_file;
    char psz_line[2048];
    int i;

    if ( psz_file == NULL )
    {
        msg_Err( NULL, "no config file" );
        return;
    }

    if ( (p_file = fopen( psz_file, "r" )) == NULL )
    {
        msg_Err( NULL, "can't fopen config file %s", psz_file );
        return;
    }

    while ( fgets( psz_line, sizeof(psz_line), p_file ) != NULL )
    {
        output_config_t config;
        output_t *p_output;
        char *psz_token, *psz_parser;

        psz_parser = strchr( psz_line, '#' );
        if ( psz_parser != NULL )
            *psz_parser-- = '\0';
        while ( psz_parser >= psz_line && isblank( *psz_parser ) )
            *psz_parser-- = '\0';
        if ( psz_line[0] == '\0' )
            continue;

        config_Defaults( &config );

        psz_token = strtok_r( psz_line, "\t\n ", &psz_parser );
        if ( psz_token == NULL || !config_ParseHost( &config, psz_token ))
        {
            config_Free( &config );
            continue;
        }

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
        {
            config_Free( &config );
            continue;
        }
        if( atoi( psz_token ) == 1 )
            config.i_config |= OUTPUT_WATCH;
        else
            config.i_config &= ~OUTPUT_WATCH;

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
        {
            config_Free( &config );
            continue;
        }
        config.i_sid = strtol(psz_token, NULL, 0);

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token != NULL )
        {
            psz_parser = NULL;
            for ( ; ; )
            {
                psz_token = strtok_r( psz_token, ",", &psz_parser );
                if ( psz_token == NULL )
                    break;
                config.pi_pids = realloc( config.pi_pids,
                                 (config.i_nb_pids + 1) * sizeof(uint16_t) );
                config.pi_pids[config.i_nb_pids++] = strtol(psz_token, NULL, 0);
                psz_token = NULL;
            }
        }

        config_Print( &config );

        p_output = output_Find( &config );

        if ( p_output == NULL )
            p_output = output_Create( &config );

        if ( p_output != NULL )
        {
            free( p_output->config.psz_displayname );
            p_output->config.psz_displayname = strdup( config.psz_displayname );

            config.i_config |= OUTPUT_VALID | OUTPUT_STILL_PRESENT;
            output_Change( p_output, &config );
            demux_Change( p_output, &config );
        }

        config_Free( &config );
    }

    fclose( p_file );

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];
        output_config_t config;

        config_Init( &config );

        if ( (p_output->config.i_config & OUTPUT_VALID) &&
             !(p_output->config.i_config & OUTPUT_STILL_PRESENT) )
        {
            msg_Dbg( NULL, "closing %s", p_output->config.psz_displayname );
            demux_Change( p_output, &config );
            output_Close( p_output );
        }

        p_output->config.i_config &= ~OUTPUT_STILL_PRESENT;
        config_Free( &config );
    }
}

/*****************************************************************************
 * Signal Handler
 *****************************************************************************/
static void SigHandler( int i_signal )
{
    if ( i_signal == SIGHUP )
        b_conf_reload = 1;
    if ( i_signal == SIGINT )
        b_exit_now = 1;
}

/*****************************************************************************
 * Version
 *****************************************************************************/
static void DisplayVersion()
{
    msg_Raw( NULL, "DVBlast %s (%s)", VERSION, VERSION_EXTRA );
}

/*****************************************************************************
 * Entry point
 *****************************************************************************/
void usage()
{
    DisplayVersion();
    msg_Raw( NULL, "Usage: dvblast [-q] [-c <config file>] [-r <remote socket>] [-t <ttl>] [-o <SSRC IP>] "
        "[-i <RT priority>] "
#ifdef HAVE_ASI_SUPPORT
        "[-A <ASI adapter>]"
#endif
#ifdef HAVE_DVB_SUPPORT
        "[-a <adapter>] [-n <frontend number>] [-S <diseqc>] [-k <uncommitted port>]"
        "[-f <frequency>]"
        "[-s <symbol rate>] [-v <0|13|18>] [-p] [-b <bandwidth>] [-I <inversion>] "
        "[-F <fec inner>] [-m <modulation] [-R <rolloff>] [-P <pilot>] [-K <fec lp>] "
        "[-G <guard interval>] [-H <hierarchy>] [-X <transmission>] [-O <lock timeout>] "
#endif
        "[-D [<src host>[:<src port>]@]<src mcast>[:<port>][/<opts>]*] "
        "[-u] [-w] [-U] [-L <latency>] [-E <retention>] [-d <dest IP>[<:port>][/<opts>]*] "
        "[-z] [-C [-e] [-M <network name] [-N <network ID>]] [-T] [-j <system charset>] "
        "[-W] [-Y] [-l] [-g <logger ident>] [-Z <mrtg file>] [-V] [-h] [-B <provider_name>]"
        "[-J <DVB charset>] [-Q <quit timeout>] [-0 pid_mapping] [-x <text|xml>]" );

    msg_Raw( NULL, "Input:" );
#ifdef HAVE_ASI_SUPPORT
    msg_Raw( NULL, "  -A --asi-adapter      read packets from an ASI adapter (0-n)" );
#endif
#ifdef HAVE_DVB_SUPPORT
    msg_Raw( NULL, "  -a --adapter          read packets from a Linux-DVB adapter (typically 0-n)" );
    msg_Raw( NULL, "  -b --bandwidth        frontend bandwith" );
#endif
    msg_Raw( NULL, "  -D --rtp-input        read packets from a multicast address instead of a DVB card" );
#ifdef HAVE_DVB_SUPPORT
    msg_Raw( NULL, "  -5 --delsys           delivery system" );
    msg_Raw( NULL, "    DVBS|DVBS2|DVBC_ANNEX_A|DVBT|ATSC (default guessed)");
    msg_Raw( NULL, "  -f --frequency        frontend frequency" );
    msg_Raw( NULL, "  -F --fec-inner        Forward Error Correction (FEC Inner)");
    msg_Raw( NULL, "    DVB-S2 0|12|23|34|35|56|78|89|910|999 (default auto: 999)");
    msg_Raw( NULL, "  -I --inversion        Inversion (-1 auto, 0 off, 1 on)" );
    msg_Raw( NULL, "  -m --modulation       Modulation type" );
    msg_Raw( NULL, "    DVB-C  qpsk|qam_16|qam_32|qam_64|qam_128|qam_256 (default qam_auto)" );
    msg_Raw( NULL, "    DVB-T  qam_16|qam_32|qam_64|qam_128|qam_256 (default qam_auto)" );
    msg_Raw( NULL, "    DVB-S2 qpsk|psk_8 (default legacy DVB-S)" );
    msg_Raw( NULL, "  -n --frontend-number <frontend number>" );
    msg_Raw( NULL, "  -p --force-pulse      force 22kHz pulses for high-band selection (DVB-S)" );
    msg_Raw( NULL, "  -P --pilot            DVB-S2 Pilot (-1 auto, 0 off, 1 on)" );
    msg_Raw( NULL, "  -R --rolloff          DVB-S2 Rolloff value" );
    msg_Raw( NULL, "    DVB-S2 35=0.35|25=0.25|20=0.20|0=AUTO (default: 35)" );
    msg_Raw( NULL, "  -1 --multistream-id   Set stream ID (0-255, default: 0)" );
    msg_Raw( NULL, "  -K --fec-lp           DVB-T low priority FEC (default auto)" );
    msg_Raw( NULL, "  -G --guard            DVB-T guard interval" );
    msg_Raw( NULL, "    DVB-T  32 (1/32)|16 (1/16)|8 (1/8)|4 (1/4)|-1 (auto, default)" );
    msg_Raw( NULL, "  -H --hierarchy        DVB-T hierarchy (0, 1, 2, 4 or -1 auto, default)" );
    msg_Raw( NULL, "  -X --transmission     DVB-T transmission (2, 4, 8 or -1 auto, default)" );
    msg_Raw( NULL, "  -s --symbol-rate" );
    msg_Raw( NULL, "  -S --diseqc           satellite number for diseqc (0: no diseqc, 1-4, A or B)" );
    msg_Raw( NULL, "  -k --uncommitted      port number for uncommitted diseqc (0: no uncommitted diseqc, 1-4)" );
    msg_Raw( NULL, "  -u --budget-mode      turn on budget mode (no hardware PID filtering)" );
    msg_Raw( NULL, "  -v --voltage          voltage to apply to the LNB (QPSK)" );
    msg_Raw( NULL, "  -w --select-pmts      set a PID filter on all PMTs" );
    msg_Raw( NULL, "  -O --lock-timeout     timeout for the lock operation (in ms)" );
    msg_Raw( NULL, "  -y --ca-number <ca_device_number>" );
#endif

    msg_Raw( NULL, "Output:" );
    msg_Raw( NULL, "  -c --config-file <config file>" );
    msg_Raw( NULL, "  -C --dvb-compliance   pass through or build the mandatory DVB tables" );
    msg_Raw( NULL, "  -d --duplicate        duplicate all received packets to a given destination" );
    msg_Raw( NULL, "  -W --emm-passthrough  pass through EMM data (CA system data)" );
    msg_Raw( NULL, "  -Y --ecm-passthrough  pass through ECM data (CA program data)" );
    msg_Raw( NULL, "  -e --epg-passthrough  pass through DVB EIT schedule tables" );
    msg_Raw( NULL, "  -E --retention        maximum retention allowed between input and output (default: 40 ms)" );
    msg_Raw( NULL, "  -L --latency          maximum latency allowed between input and output (default: 100 ms)" );
    msg_Raw( NULL, "  -M --network-name     DVB network name to declare in the NIT" );
    msg_Raw( NULL, "  -N --network-id       DVB network ID to declare in the NIT" );
    msg_Raw( NULL, "  -B --provider-name    Service provider name to declare in the SDT" );
    msg_Raw( NULL, "  -o --rtp-output <SSRC IP>" );
    msg_Raw( NULL, "  -t --ttl <ttl>        TTL of the output stream" );
    msg_Raw( NULL, "  -T --unique-ts-id     generate random unique TS ID for each output" );
    msg_Raw( NULL, "  -U --udp              use raw UDP rather than RTP (required by some IPTV set top boxes)" );
    msg_Raw( NULL, "  -z --any-type         pass through all ESs from the PMT, of any type" );
    msg_Raw( NULL, "  -0 --pidmap <pmt_pid,audio_pid,video_pid,spu_pid>");

    msg_Raw( NULL, "Misc:" );
    msg_Raw( NULL, "  -h --help             display this full help" );
    msg_Raw( NULL, "  -i --priority <RT priority>" );
    msg_Raw( NULL, "  -j --system-charset   character set used for printing messages (default UTF-8)" );
    msg_Raw( NULL, "  -J --dvb-charset      character set used in output DVB tables (default ISO_8859-1)" );
    msg_Raw( NULL, "  -l --logger           use syslog for logging messages instead of stderr" );
    msg_Raw( NULL, "  -g --logger-ident     program name that will be used in syslog messages" );
    msg_Raw( NULL, "  -x --print            print interesting events on stdout in a given format" );
    msg_Raw( NULL, "  -q --quiet            be quiet (less verbosity, repeat or use number for even quieter)" );
    msg_Raw( NULL, "  -Q --quit-timeout     when locked, quit after this delay (in ms), or after the first lock timeout" );
    msg_Raw( NULL, "  -r --remote-socket <remote socket>" );
    msg_Raw( NULL, "  -V --version          only display the version" );
    msg_Raw( NULL, "  -Z --mrtg-file <file> Log input packets and errors into mrtg-file" );
    exit(1);
}

int main( int i_argc, char **pp_argv )
{
    const char *psz_network_name = "DVBlast - http://www.videolan.org/projects/dvblast.html";
    char *p_network_name_tmp = NULL;
    size_t i_network_name_tmp_size;
    char *psz_dup_config = NULL;
    mtime_t i_poll_timeout = MAX_POLL_TIMEOUT;
    struct sched_param param;
    int i_error;
    int c;
    struct sigaction sa;
    sigset_t set;

    int b_enable_syslog = 0;

    if ( i_argc == 1 )
        usage();

    /*
     * The only short options left are: 2346789
     * Use them wisely.
     */
    static const struct option long_options[] =
    {
        { "config-file",     required_argument, NULL, 'c' },
        { "remote-socket",   required_argument, NULL, 'r' },
        { "ttl",             required_argument, NULL, 't' },
        { "rtp-output",      required_argument, NULL, 'o' },
        { "priority",        required_argument, NULL, 'i' },
        { "adapter",         required_argument, NULL, 'a' },
        { "frontend-number", required_argument, NULL, 'n' },
        { "delsys",          required_argument, NULL, '5' },
        { "frequency",       required_argument, NULL, 'f' },
        { "fec-inner",       required_argument, NULL, 'F' },
        { "rolloff",         required_argument, NULL, 'R' },
        { "symbol-rate",     required_argument, NULL, 's' },
        { "diseqc",          required_argument, NULL, 'S' },
        { "uncommitted",     required_argument, NULL, 'k' },
        { "voltage",         required_argument, NULL, 'v' },
        { "force-pulse",     no_argument,       NULL, 'p' },
        { "bandwidth",       required_argument, NULL, 'b' },
        { "inversion",       required_argument, NULL, 'I' },
        { "modulation",      required_argument, NULL, 'm' },
        { "pilot",           required_argument, NULL, 'P' },
        { "mutistream-id",   required_argument, NULL, '1' },
        { "fec-lp",          required_argument, NULL, 'K' },
        { "guard",           required_argument, NULL, 'G' },
        { "hierarchy",       required_argument, NULL, 'H' },
        { "transmission",    required_argument, NULL, 'X' },
        { "lock-timeout",    required_argument, NULL, 'O' },
        { "budget-mode",     no_argument,       NULL, 'u' },
        { "select-pmts",     no_argument,       NULL, 'w' },
        { "udp",             no_argument,       NULL, 'U' },
        { "unique-ts-id",    no_argument,       NULL, 'T' },
        { "latency",         required_argument, NULL, 'L' },
        { "retention",       required_argument, NULL, 'E' },
        { "duplicate",       required_argument, NULL, 'd' },
        { "rtp-input",       required_argument, NULL, 'D' },
        { "asi-adapter",     required_argument, NULL, 'A' },
        { "any-type",        no_argument,       NULL, 'z' },
        { "dvb-compliance",  no_argument,       NULL, 'C' },
        { "emm-passthrough", no_argument,       NULL, 'W' },
        { "ecm-passthrough", no_argument,       NULL, 'Y' },
        { "epg-passthrough", no_argument,       NULL, 'e' },
        { "network-name",    no_argument,       NULL, 'M' },
        { "network-id",      no_argument,       NULL, 'N' },
        { "system-charset",  required_argument, NULL, 'j' },
        { "dvb-charset",     required_argument, NULL, 'J' },
        { "provider-name",   required_argument, NULL, 'B' },
        { "logger",          no_argument,       NULL, 'l' },
        { "logger-ident",    required_argument, NULL, 'g' },
        { "print",           required_argument, NULL, 'x' },
        { "quit-timeout",    required_argument, NULL, 'Q' },
        { "quiet",           no_argument,       NULL, 'q' },
        { "help",            no_argument,       NULL, 'h' },
        { "version",         no_argument,       NULL, 'V' },
        { "mrtg-file",       required_argument, NULL, 'Z' },
        { "ca-number",       required_argument, NULL, 'y' },
        { "pidmap",          required_argument, NULL, '0' },
        { 0, 0, 0, 0 }
    };

    while ( (c = getopt_long(i_argc, pp_argv, "q::c:r:t:o:i:a:n:5:f:F:R:s:S:k:v:pb:I:m:P:K:G:H:X:O:uwUTL:E:d:D:A:lg:zCWYeM:N:j:J:B:x:Q:hVZ:y:0:1:", long_options, NULL)) != -1 )
    {
        switch ( c )
        {
        case 'q':
            if ( optarg )
            {
                if ( *optarg == 'q' )  /* e.g. -qqq */
                {
                    i_verbose--;
                    while ( *optarg == 'q' )
                    {
                        i_verbose--;
                        optarg++;
                    }
                }
                else
                {
                    i_verbose -= atoi( optarg );  /* e.g. -q2 */
                }
            }
            else
            {
                i_verbose--;  /* -q */
            }
            break;

        case 'c':
            psz_conf_file = optarg;
            /*
             * When configuration file is used it is reasonable to assume that
             * services may be added/removed. If b_select_pmts is not set dvblast
             * is unable to start streaming newly added services in the config.
             */
            b_select_pmts = 1;
            break;

        case 'r':
            psz_srv_socket = optarg;
            break;

        case 't':
            i_ttl_global = strtol( optarg, NULL, 0 );
            break;

        case 'o':
        {
            struct in_addr maddr;
            if ( !inet_aton( optarg, &maddr ) )
                usage();
            memcpy( pi_ssrc_global, &maddr.s_addr, 4 * sizeof(uint8_t) );
            break;
        }

        case 'i':
            i_priority = strtol( optarg, NULL, 0 );
            break;

        case 'a':
            i_adapter = strtol( optarg, NULL, 0 );
            break;

        case 'n':
            i_fenum = strtol( optarg, NULL, 0 );
            break;

        case 'y':
            i_canum = strtol( optarg, NULL, 0 );
            break;

        case '5':
            psz_delsys = optarg;
            break;

        case 'f':
            i_frequency = strtol( optarg, NULL, 0 );
            if ( pf_Open != NULL )
                usage();
#ifdef HAVE_DVB_SUPPORT
            pf_Open = dvb_Open;
            pf_Read = dvb_Read;
            pf_Reset = dvb_Reset;
            pf_SetFilter = dvb_SetFilter;
            pf_UnsetFilter = dvb_UnsetFilter;
#else
            msg_Err( NULL, "DVBlast is compiled without DVB support.");
            exit(1);
#endif
            break;

        case 'F':
            i_fec = strtol( optarg, NULL, 0 );
            break;

        case 'R':
            i_rolloff = strtol( optarg, NULL, 0 );
            break;

        case 's':
            i_srate = strtol( optarg, NULL, 0 );
            break;

        case 'S':
            i_satnum = strtol( optarg, NULL, 16 );
            break;

        case 'k':
            i_uncommitted = strtol( optarg, NULL, 16 );
            break;

        case 'v':
            i_voltage = strtol( optarg, NULL, 0 );
            break;

        case 'p':
            b_tone = 1;
            break;

        case 'b':
            i_bandwidth = strtol( optarg, NULL, 0 );
            break;

        case 'I':
            i_inversion = strtol( optarg, NULL, 0 );
            break;

        case 'm':
            psz_modulation = optarg;
            break;

        case 'P':
            i_pilot = strtol( optarg, NULL, 0 );
            break;

        case '1':
            i_mis = strtol( optarg, NULL, 0 );
            break;

        case 'K':
            i_fec_lp = strtol( optarg, NULL, 0 );
            break;

        case 'G':
            i_guard = strtol( optarg, NULL, 0 );
            break;

        case 'X':
            i_transmission = strtol( optarg, NULL, 0 );
            break;

        case 'O':
            i_frontend_timeout_duration = strtoll( optarg, NULL, 0 ) * 1000;
            break;

        case 'H':
            i_hierarchy = strtol( optarg, NULL, 0 );
            break;

        case 'u':
            b_budget_mode = 1;
            break;

        case 'w':
            b_select_pmts = 1;
            break;

        case 'U':
            b_udp_global = true;
            break;

        case 'L':
            i_latency_global = strtoll( optarg, NULL, 0 ) * 1000;
            break;

        case 'E':
            i_retention_global = strtoll( optarg, NULL, 0 ) * 1000;
            break;

        case 'd':
            psz_dup_config = optarg;
            break;

        case 'D':
            psz_udp_src = optarg;
            if ( pf_Open != NULL )
                usage();
            pf_Open = udp_Open;
            pf_Read = udp_Read;
            pf_Reset = udp_Reset;
            pf_SetFilter = udp_SetFilter;
            pf_UnsetFilter = udp_UnsetFilter;
            break;

        case 'A':
            i_asi_adapter = strtol( optarg, NULL, 0 );
            if ( pf_Open != NULL )
                usage();
#ifdef HAVE_ASI_SUPPORT
            pf_Open = asi_Open;
            pf_Read = asi_Read;
            pf_Reset = asi_Reset;
            pf_SetFilter = asi_SetFilter;
            pf_UnsetFilter = asi_UnsetFilter;
#else
            msg_Err( NULL, "DVBlast is compiled without ASI support.");
            exit(1);
#endif
            break;

        case 'z':
            b_any_type = 1;
            break;

        case 'C':
            b_dvb_global = true;
            break;

        case 'W':
            b_enable_emm = true;
            break;

        case 'Y':
            b_enable_ecm = true;
            b_dvb_global = true;
            break;
 
        case 'e':
            b_epg_global = true;
            break;

        case 'M':
            psz_network_name = optarg;
            break;

        case 'N':
            i_network_id = strtoul( optarg, NULL, 0 );
            break;

        case 'T':
            b_random_tsid = 1;
            break;

        case 'j':
            psz_native_charset = optarg;
            break;

        case 'J':
            psz_dvb_charset = optarg;
            break;

        case 'B':
            psz_provider_name = optarg;
            break;

        case 'l':
            b_enable_syslog = 1;
            break;

        case 'g':
            psz_syslog_ident = optarg;
            break;

        case 'x':
            if ( !strcmp(optarg, "text") )
                i_print_type = PRINT_TEXT;
            else if ( !strcmp(optarg, "xml") )
                i_print_type = PRINT_XML;
            else
                msg_Warn( NULL, "unrecognized print type %s", optarg );
            /* Make stdout line-buffered */
            setvbuf(stdout, NULL, _IOLBF, 0);
            break;

        case 'Q':
            i_quit_timeout_duration = strtoll( optarg, NULL, 0 ) * 1000;
            break;

        case 'V':
            DisplayVersion();
            exit(0);
            break;

        case 'Z':
            psz_mrtg_file = optarg;
            break;

        case '0': {
            /* We expect a comma separated list of numbers.
               Put them into the pi_newpids array as they appear */
            char *str1;
            char *saveptr = NULL;
            char *tok = NULL;
            int i, i_newpid;
            for (i = 0, str1 = optarg; i < N_MAP_PIDS; i++, str1 = NULL)
            {
                tok = strtok_r(str1, ",", &saveptr);
                if ( !tok )
                    break;
                i_newpid = strtoul(tok, NULL, 0);
                if ( !i_newpid ) {
                     msg_Err( NULL, "Invalid pidmap string" );
                     usage();
                }
                pi_newpids[i] = i_newpid;
            }
            b_do_remap = true;
            break;
        }
        case 'h':
        default:
            usage();
        }
    }
    if ( optind < i_argc || pf_Open == NULL )
        usage();

    if ( b_enable_syslog )
        msg_Connect( psz_syslog_ident ? psz_syslog_ident : pp_argv[0] );

    if ( i_verbose )
        DisplayVersion();

    msg_Warn( NULL, "restarting" );
    switch (i_print_type) 
    {
        case PRINT_XML:
            printf("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
            printf("<TS>\n");
            break;
        default:
            break;
    }

    if ( b_udp_global )
    {
        msg_Warn( NULL, "raw UDP output is deprecated.  Please consider using RTP." );
        msg_Warn( NULL, "for DVB-IP compliance you should use RTP." );
    }

    if ( b_epg_global && !b_dvb_global )
    {
        msg_Dbg( NULL, "turning on DVB compliance, required by EPG information" );
        b_dvb_global = true;
    }

    memset( &output_dup, 0, sizeof(output_dup) );
    if ( psz_dup_config != NULL )
    {
        output_config_t config;

        config_Defaults( &config );
        if ( !config_ParseHost( &config, psz_dup_config ) )
            msg_Err( NULL, "Invalid target address for -d switch" );
        else
        {
            output_Init( &output_dup, &config );
            output_Change( &output_dup, &config );
        }

        config_Free( &config );
    }

    if ( strcasecmp( psz_native_charset, psz_dvb_charset ) )
    {
#ifdef HAVE_ICONV
        iconv_t iconv_system = iconv_open( psz_dvb_charset,
                                           psz_native_charset );
        if ( iconv_system != (iconv_t)-1 )
        {
            size_t i = strlen( psz_network_name );
            char *p, *psz_string;
            i_network_name_tmp_size = i * 6;
            p = psz_string = malloc(i_network_name_tmp_size);
            if ( iconv( iconv_system, (char **)&psz_network_name, &i, &p,
                        &i_network_name_tmp_size ) == -1 )
                free( psz_string );
            else
            {
                p_network_name_tmp = psz_string;
                i_network_name_tmp_size = p - psz_string;
            }
            iconv_close( iconv_system );
        }
#else
        msg_Warn( NULL,
                  "unable to convert from %s to %s (iconv is not available)",
                  psz_native_charset, psz_dvb_charset );
#endif
    }
    if ( p_network_name_tmp == NULL )
    {
        p_network_name_tmp = strdup(psz_network_name);
        i_network_name_tmp_size = strlen(psz_network_name);
    }
    p_network_name = dvb_string_set( (uint8_t *)p_network_name_tmp,
                                     i_network_name_tmp_size, psz_dvb_charset,
                                     &i_network_name_size );
    free( p_network_name_tmp );

    /* Set signal handlers */
    memset( &sa, 0, sizeof(struct sigaction) );
    sa.sa_handler = SigHandler;
    sigfillset( &set );

    if ( sigaction( SIGHUP, &sa, NULL ) == -1 || sigaction( SIGINT, &sa, NULL ) == -1 )
    {
        msg_Err( NULL, "couldn't set signal handler: %s", strerror(errno) );
        exit(EXIT_FAILURE);
    }

    srand( time(NULL) * getpid() );

    demux_Open();

    // init the mrtg logfile
    mrtgInit(psz_mrtg_file);

    if ( i_priority > 0 )
    {
        memset( &param, 0, sizeof(struct sched_param) );
        param.sched_priority = i_priority;
        if ( (i_error = pthread_setschedparam( pthread_self(), SCHED_RR,
                                               &param )) )
        {
            msg_Warn( NULL, "couldn't set thread priority: %s",
                      strerror(i_error) );
        }
    }

    config_ReadFile( psz_conf_file );

    if ( psz_srv_socket != NULL )
        comm_Open();

    for ( ; ; )
    {
        block_t *p_ts;

        if ( b_exit_now )
        {
            msg_Info( NULL, "Shutdown was requested." );
            break;
        }

        if ( b_conf_reload )
        {
            b_conf_reload = 0;
            msg_Info( NULL, "Configuration reload was requested." );
            config_ReadFile( psz_conf_file );
        }

        if ( i_quit_timeout && i_quit_timeout <= i_wallclock )
        {
            switch (i_print_type)
            {
            case PRINT_XML:
                printf("</TS>\n");
                break;
            default:
                break;
            }
            exit(EXIT_SUCCESS);
        }

        p_ts = pf_Read( i_poll_timeout );
        if ( p_ts != NULL )
        {
            mrtgAnalyse(p_ts);
            demux_Run( p_ts );
        }
        i_poll_timeout = output_Send();
        if ( i_poll_timeout == -1 || i_poll_timeout > MAX_POLL_TIMEOUT )
            i_poll_timeout = MAX_POLL_TIMEOUT;
    }

    mrtgClose();
    outputs_Close( i_nb_outputs );
    demux_Close();
    free( p_network_name );

    if ( b_enable_syslog )
        msg_Disconnect();

    if ( psz_srv_socket && i_comm_fd > -1 )
        unlink( psz_srv_socket );

    return EXIT_SUCCESS;
}
