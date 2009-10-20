/*****************************************************************************
 * dvb.c
 *****************************************************************************
 * Copyright (C) 2008-2009 VideoLAN
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

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>

#include "dvblast.h"
#include "en50221.h"
#include "comm.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define FRONTEND_LOCK_TIMEOUT 10000000 /* 10 s */
#define COUNTER_WRAP 200 /* we make 200 read calls per second */
#define MAX_READ_ONCE 50

static int i_frontend, i_dvr;
static fe_status_t i_last_status;
static mtime_t i_frontend_timeout;
static unsigned int i_read_once = 1;
static unsigned int i_read_counter = 0;
static mtime_t i_last_counter = 0;
static mtime_t i_ca_next_event = 0;
mtime_t i_ca_timeout = 0;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static block_t *DVRRead( void );
static void FrontendPoll( void );
static void FrontendSet( void );
#if DVB_API_VERSION < 5
static void FrontendSetQPSK( struct dvb_frontend_parameters *p_fep );
static void FrontendSetQAM( struct dvb_frontend_parameters *p_fep );
static void FrontendSetOFDM( struct dvb_frontend_parameters *p_fep );
static void FrontendSetATSC( struct dvb_frontend_parameters *p_fep );
#endif

/*****************************************************************************
 * dvb_Open
 *****************************************************************************/
void dvb_Open( void )
{
    char psz_tmp[128];

    sprintf( psz_tmp, "/dev/dvb/adapter%d/frontend%d", i_adapter, i_fenum );
    if( (i_frontend = open(psz_tmp, O_RDWR | O_NONBLOCK)) < 0 )
    {
        msg_Err( NULL, "opening device %s failed (%s)", psz_tmp,
                 strerror(errno) );
        exit(1);
    }

    FrontendSet();

    sprintf( psz_tmp, "/dev/dvb/adapter%d/dvr%d", i_adapter, i_fenum );

    if( (i_dvr = open(psz_tmp, O_RDONLY)) < 0 )
    {
        msg_Err( NULL, "opening device %s failed (%s)", psz_tmp,
                 strerror(errno) );
        exit(1);
    }

    if( fcntl( i_dvr, F_SETFL, O_NONBLOCK ) == -1 )
    {
        msg_Warn( NULL, "couldn't set %s non-blocking mode (%s)", psz_tmp,
                  strerror(errno) );
    }

    en50221_Init();
    i_ca_next_event = mdate() + i_ca_timeout;
}

/*****************************************************************************
 * dvb_Read
 *****************************************************************************/
block_t *dvb_Read( void )
{
    struct pollfd ufds[3];
    int i_ret, i_nb_fd = 2;

    memset( ufds, 0, sizeof(ufds) );
    ufds[0].fd = i_dvr;
    ufds[0].events = POLLIN;
    ufds[1].fd = i_frontend;
    ufds[1].events = POLLERR | POLLPRI;
    if ( i_comm_fd != -1 )
    {
        ufds[2].fd = i_comm_fd;
        ufds[2].events = POLLIN;
        i_nb_fd = 3;
    }

    i_ret = poll( ufds, i_nb_fd, 100 );

    if ( i_ret < 0 )
    {
        if( errno != EINTR )
            msg_Err( NULL, "poll error: %s", strerror(errno) );
        return NULL;
    }

    if ( i_ca_handle && i_ca_type == CA_CI_LINK
          && mdate() > i_ca_next_event )
    {
        en50221_Poll();
        i_ca_next_event = mdate() + i_ca_timeout;
    }

    if ( ufds[1].revents )
        FrontendPoll();

    if ( i_frontend_timeout && mdate() > i_frontend_timeout )
    {
        msg_Warn( NULL, "no lock, tuning again" );
        FrontendSet();
    }

    if ( i_comm_fd != -1 && ufds[2].revents )
        comm_Read();

    if ( ufds[0].revents )
        return DVRRead();

    return NULL;
}

/*****************************************************************************
 * DVRRead
 *****************************************************************************/
static block_t *DVRRead( void )
{
    int i, i_len;
    block_t *p_ts, **pp_current = &p_ts;
    struct iovec p_iov[i_read_once];

    for ( i = 0; i < i_read_once; i++ )
    {
        *pp_current = block_New();
        p_iov[i].iov_base = (*pp_current)->p_ts;
        p_iov[i].iov_len = TS_SIZE;
        pp_current = &(*pp_current)->p_next;
    }

    if ( (i_len = readv(i_dvr, p_iov, i_read_once)) < 0 )
    {
        msg_Err( NULL, "couldn't read from DVR device (%s)",
                 strerror(errno) );
        i_len = 0;
    }
    i_len /= TS_SIZE;

    //msg_Err( NULL, "Meuuh %d %d", i_len, i_read_once );

    pp_current = &p_ts;
    while ( i_len && *pp_current )
    {
        pp_current = &(*pp_current)->p_next;
        i_len--;
    }

    block_DeleteChain( *pp_current );
    *pp_current = NULL;

    i_read_counter++;
    if ( i_read_counter >= COUNTER_WRAP )
    {
        mtime_t i_current_date = mdate();

        if ( i_last_counter )
        {
            /* Adjust the buffer size to keep the read() calls frequency
             * at a certain limit */
            i_read_once = (mtime_t)i_read_once * 1000000LL
                           / (i_current_date - i_last_counter);
            if ( i_read_once < 1 )
                i_read_once = 1;
            if ( i_read_once > MAX_READ_ONCE )
                i_read_once = MAX_READ_ONCE;
        }

        i_read_counter = 0;
        i_last_counter = i_current_date;
    }

    return p_ts;
}


/*
 * Demux
 */

/*****************************************************************************
 * dvb_SetFilter : controls the demux to add a filter
 *****************************************************************************/
int dvb_SetFilter( uint16_t i_pid )
{
    struct dmx_pes_filter_params s_filter_params;
    char psz_tmp[128];
    int i_fd;

    sprintf( psz_tmp, "/dev/dvb/adapter%d/demux%d", i_adapter, i_fenum );
    if( (i_fd = open(psz_tmp, O_RDWR)) < 0 )
    {
        msg_Err( NULL, "DMXSetFilter: opening device failed (%s)",
                 strerror(errno) );
        return -1;
    }

    s_filter_params.pid      = i_pid;
    s_filter_params.input    = DMX_IN_FRONTEND;
    s_filter_params.output   = DMX_OUT_TS_TAP;
    s_filter_params.flags    = DMX_IMMEDIATE_START;
    s_filter_params.pes_type = DMX_PES_OTHER;

    if ( ioctl( i_fd, DMX_SET_PES_FILTER, &s_filter_params ) < 0 )
    {
        msg_Err( NULL, "failed setting filter on %d (%s)", i_pid,
                 strerror(errno) );
        close( i_fd );
        return -1;
    }

    msg_Dbg( NULL, "setting filter on PID %d", i_pid );

    return i_fd;
}

/*****************************************************************************
 * dvb_UnsetFilter : removes a filter
 *****************************************************************************/
void dvb_UnsetFilter( int i_fd, uint16_t i_pid )
{
    if ( ioctl( i_fd, DMX_STOP ) < 0 )
        msg_Err( NULL, "DMX_STOP failed (%s)", strerror(errno) );
    else
        msg_Dbg( NULL, "unsetting filter on PID %d", i_pid );

    close( i_fd );
}


/*
 * Frontend
 */

/*****************************************************************************
 * FrontendPoll : Poll for frontend events
 *****************************************************************************/
static void FrontendPoll( void )
{
    struct dvb_frontend_event event;
    fe_status_t i_status, i_diff;

    for( ;; )
    {
        int i_ret = ioctl( i_frontend, FE_GET_EVENT, &event );

        if( i_ret < 0 )
        {
            if( errno == EWOULDBLOCK )
                return; /* no more events */

            msg_Err( NULL, "reading frontend event failed (%d) %s",
                     i_ret, strerror(errno) );
            return;
        }

        i_status = event.status;
        i_diff = i_status ^ i_last_status;
        i_last_status = i_status;

        {
#define IF_UP( x )                                                          \
        }                                                                   \
        if ( i_diff & (x) )                                                 \
        {                                                                   \
            if ( i_status & (x) )

            IF_UP( FE_HAS_SIGNAL )
                msg_Dbg( NULL, "frontend has acquired signal" );
            else
                msg_Dbg( NULL, "frontend has lost signal" );

            IF_UP( FE_HAS_CARRIER )
                msg_Dbg( NULL, "frontend has acquired carrier" );
            else
                msg_Dbg( NULL, "frontend has lost carrier" );

            IF_UP( FE_HAS_VITERBI )
                msg_Dbg( NULL, "frontend has acquired stable FEC" );
            else
                msg_Dbg( NULL, "frontend has lost FEC" );

            IF_UP( FE_HAS_SYNC )
                msg_Dbg( NULL, "frontend has acquired sync" );
            else
                msg_Dbg( NULL, "frontend has lost sync" );

            IF_UP( FE_HAS_LOCK )
            {
                int32_t i_value = 0;
                msg_Dbg( NULL, "frontend has acquired lock" );
                i_frontend_timeout = 0;

                /* Read some statistics */
                if( ioctl( i_frontend, FE_READ_BER, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- Bit error rate: %d", i_value );
                if( ioctl( i_frontend, FE_READ_SIGNAL_STRENGTH, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- Signal strength: %d", i_value );
                if( ioctl( i_frontend, FE_READ_SNR, &i_value ) >= 0 )
                    msg_Dbg( NULL, "- SNR: %d", i_value );
            }
            else
            {
                msg_Dbg( NULL, "frontend has lost lock" );
                i_frontend_timeout = mdate() + FRONTEND_LOCK_TIMEOUT;
            }

            IF_UP( FE_REINIT )
            {
                /* The frontend was reinited. */
                msg_Warn( NULL, "reiniting frontend");
                FrontendSet();
            }
        }
#undef IF_UP
    }
}

#if DVB_API_VERSION >= 5
/*****************************************************************************
 * GetModulation : helper function for both APIs
 *****************************************************************************/
static fe_modulation_t GetModulation(void)
{
#define GET_MODULATION( mod )                                               \
    if ( !strcasecmp( psz_modulation, #mod ) )                              \
        return mod;

    GET_MODULATION(QPSK);
    GET_MODULATION(QAM_16);
    GET_MODULATION(QAM_32);
    GET_MODULATION(QAM_64);
    GET_MODULATION(QAM_128);
    GET_MODULATION(QAM_256);
    GET_MODULATION(QAM_AUTO);
    GET_MODULATION(VSB_8);
    GET_MODULATION(VSB_16);
#if DVB_API_VERSION >= 5
    GET_MODULATION(PSK_8);
    GET_MODULATION(APSK_16);
    GET_MODULATION(APSK_32);
    GET_MODULATION(DQPSK);
#endif

#undef GET_MODULATION
    msg_Err( NULL, "invalid modulation %s", psz_modulation );
    exit(1);
}

/*****************************************************************************
 * FrontendSet
 *****************************************************************************/
/* S2API */
static struct dtv_property dvbs_cmdargs[] = {
	{ .cmd = DTV_FREQUENCY,       .u.data = 0 },
	{ .cmd = DTV_MODULATION,      .u.data = QPSK },
	{ .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
	{ .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
	{ .cmd = DTV_VOLTAGE,         .u.data = SEC_VOLTAGE_OFF },
	{ .cmd = DTV_TONE,            .u.data = SEC_TONE_OFF },
	{ .cmd = DTV_INNER_FEC,       .u.data = FEC_AUTO },
	{ .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBS },
	{ .cmd = DTV_TUNE },
};
static struct dtv_properties dvbs_cmdseq = {
	.num = sizeof(dvbs_cmdargs)/sizeof(struct dtv_property),
	.props = dvbs_cmdargs
};

static struct dtv_property dvbs2_cmdargs[] = {
	{ .cmd = DTV_FREQUENCY,       .u.data = 0 },
	{ .cmd = DTV_MODULATION,      .u.data = PSK_8 },
	{ .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
	{ .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
	{ .cmd = DTV_VOLTAGE,         .u.data = SEC_VOLTAGE_OFF },
	{ .cmd = DTV_TONE,            .u.data = SEC_TONE_OFF },
	{ .cmd = DTV_INNER_FEC,       .u.data = FEC_AUTO },
	{ .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBS2 },
	{ .cmd = DTV_PILOT,           .u.data = PILOT_AUTO },
	{ .cmd = DTV_ROLLOFF,         .u.data = ROLLOFF_AUTO },
	{ .cmd = DTV_TUNE },
};
static struct dtv_properties dvbs2_cmdseq = {
	.num = sizeof(dvbs2_cmdargs)/sizeof(struct dtv_property),
	.props = dvbs2_cmdargs
};

static struct dtv_property dvbc_cmdargs[] = {
	{ .cmd = DTV_FREQUENCY,       .u.data = 0 },
	{ .cmd = DTV_MODULATION,      .u.data = QAM_AUTO },
	{ .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
	{ .cmd = DTV_SYMBOL_RATE,     .u.data = 27500000 },
	{ .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBC_ANNEX_AC },
	{ .cmd = DTV_TUNE },
};
static struct dtv_properties dvbc_cmdseq = {
	.num = sizeof(dvbc_cmdargs)/sizeof(struct dtv_property),
	.props = dvbc_cmdargs
};

static struct dtv_property dvbt_cmdargs[] = {
	{ .cmd = DTV_FREQUENCY,       .u.data = 0 },
	{ .cmd = DTV_MODULATION,      .u.data = QAM_AUTO },
	{ .cmd = DTV_INVERSION,       .u.data = INVERSION_AUTO },
	{ .cmd = DTV_BANDWIDTH_HZ,    .u.data = 8000000 },
	{ .cmd = DTV_CODE_RATE_HP,    .u.data = FEC_AUTO },
	{ .cmd = DTV_CODE_RATE_LP,    .u.data = FEC_AUTO },
	{ .cmd = DTV_GUARD_INTERVAL,  .u.data = GUARD_INTERVAL_AUTO },
	{ .cmd = DTV_TRANSMISSION_MODE,.u.data = TRANSMISSION_MODE_AUTO },
	{ .cmd = DTV_HIERARCHY,       .u.data = HIERARCHY_AUTO },
	{ .cmd = DTV_DELIVERY_SYSTEM, .u.data = SYS_DVBT },
	{ .cmd = DTV_TUNE },
};
static struct dtv_properties dvbt_cmdseq = {
	.num = sizeof(dvbt_cmdargs)/sizeof(struct dtv_property),
	.props = dvbt_cmdargs
};

#define FREQUENCY 0
#define MODULATION 1
#define INVERSION 2
#define SYMBOL_RATE 3
#define BANDWIDTH 3
#define VOLTAGE 4
#define TONE 5
#define INNER_FEC 6

static void FrontendSet( void )
{
    struct dvb_frontend_info info;
    struct dtv_properties *p;

    if ( ioctl( i_frontend, FE_GET_INFO, &info ) < 0 )
    {
        msg_Err( NULL, "FE_GET_INFO failed (%s)", strerror(errno) );
        exit(1);
    }

    switch ( info.type )
    {
    case FE_OFDM:
        p = &dvbt_cmdseq;
        p->props[FREQUENCY].u.data = i_frequency;
        if ( psz_modulation != NULL )
            p->props[MODULATION].u.data = GetModulation();
        p->props[BANDWIDTH].u.data = i_bandwidth * 1000000;

        msg_Dbg( NULL, "tuning OFDM frontend to f=%d bandwidth=%d modulation=%s",
                 i_frequency, i_bandwidth,
                 psz_modulation == NULL ? "qam_auto" : psz_modulation );
        break;

    case FE_QAM:
        p = &dvbc_cmdseq;
        p->props[FREQUENCY].u.data = i_frequency;
        if ( psz_modulation != NULL )
            p->props[MODULATION].u.data = GetModulation();
        p->props[SYMBOL_RATE].u.data = i_srate;

        msg_Dbg( NULL, "tuning QAM frontend to f=%d srate=%d modulation=%s",
                 i_frequency, i_srate,
                 psz_modulation == NULL ? "qam_auto" : psz_modulation );
        break;

    case FE_QPSK:
        if ( psz_modulation != NULL )
        {
            p = &dvbs2_cmdseq;
            p->props[MODULATION].u.data = GetModulation();
        }
        else
            p = &dvbs_cmdseq;
        p->props[SYMBOL_RATE].u.data = i_srate;

        switch ( i_voltage )
        {
            case 0: p->props[VOLTAGE].u.data = SEC_VOLTAGE_OFF; break;
            default:
            case 13: p->props[VOLTAGE].u.data = SEC_VOLTAGE_13; break;
            case 18: p->props[VOLTAGE].u.data = SEC_VOLTAGE_18; break;
        }

        p->props[TONE].u.data = b_tone ? SEC_TONE_ON : SEC_TONE_OFF;

        /* Automatic mode. */
        if ( i_frequency >= 950000 && i_frequency <= 2150000 )
        {
            msg_Dbg( NULL, "frequency %d is in IF-band", i_frequency );
            p->props[FREQUENCY].u.data = i_frequency;
        }
        else if ( i_frequency >= 2500000 && i_frequency <= 2700000 )
        {
            msg_Dbg( NULL, "frequency %d is in S-band", i_frequency );
            p->props[FREQUENCY].u.data = i_frequency - 3650000;
        }
        else if ( i_frequency >= 3400000 && i_frequency <= 4200000 )
        {
            msg_Dbg( NULL, "frequency %d is in C-band (lower)",
                     i_frequency );
            p->props[FREQUENCY].u.data = i_frequency - 5150000;
        }
        else if ( i_frequency >= 4500000 && i_frequency <= 4800000 )
        {
            msg_Dbg( NULL, "frequency %d is in C-band (higher)",
                     i_frequency );
            p->props[FREQUENCY].u.data = i_frequency - 5950000;
        }
        else if ( i_frequency >= 10700000 && i_frequency < 11700000 )
        {
            msg_Dbg( NULL, "frequency %d is in Ku-band (lower)",
                     i_frequency );
            p->props[FREQUENCY].u.data = i_frequency - 9750000;
        }
        else if ( i_frequency >= 11700000 && i_frequency <= 13250000 )
        {
            msg_Dbg( NULL, "frequency %d is in Ku-band (higher)",
                     i_frequency );
            p->props[FREQUENCY].u.data = i_frequency - 10600000;
            p->props[TONE].u.data = SEC_TONE_ON;
        }
        else
        {
            msg_Err( NULL, "frequency %d is out of any known band",
                     i_frequency );
            exit(1);
        }

        msg_Dbg( NULL, "tuning QPSK frontend to f=%d srate=%d v=%d p=%d modulation=%s",
                 i_frequency, i_srate, i_voltage, b_tone,
                 psz_modulation == NULL ? "legacy" : psz_modulation );
        break;

    default:
        msg_Err( NULL, "unknown frontend type %d", info.type );
        exit(1);
    }

    /* Empty the event queue */
    for ( ; ; )
    {
        struct dvb_frontend_event event;
        if ( ioctl( i_frontend, FE_GET_EVENT, &event ) < 0
              && errno == EWOULDBLOCK )
            break;
    }

    /* Now send it all to the frontend device */
    if ( ioctl( i_frontend, FE_SET_PROPERTY, p ) < 0 )
    {
        msg_Err( NULL, "setting frontend failed (%s)", strerror(errno) );
        exit(1);
    }

    i_last_status = 0;
    i_frontend_timeout = mdate() + FRONTEND_LOCK_TIMEOUT;
}

#else /* !S2API */

static void FrontendSet( void )
{
    struct dvb_frontend_info info;
    struct dvb_frontend_parameters fep;

    if ( ioctl( i_frontend, FE_GET_INFO, &info ) < 0 )
    {
        msg_Err( NULL, "FE_GET_INFO failed (%s)", strerror(errno) );
        exit(1);
    }

    switch ( info.type )
    {
    case FE_OFDM:
        FrontendSetOFDM( &fep );
        break;
    case FE_QAM:
        FrontendSetQAM( &fep );
        break;
    case FE_QPSK:
        FrontendSetQPSK( &fep );
        break;
    case FE_ATSC:
        FrontendSetATSC( &fep );
        break;
    default:
        msg_Err( NULL, "unknown frontend type %d", info.type );
        exit(1);
    }

    /* Empty the event queue */
    for ( ; ; )
    {
        struct dvb_frontend_event event;
        if ( ioctl( i_frontend, FE_GET_EVENT, &event ) < 0
              && errno == EWOULDBLOCK )
            break;
    }

    /* Now send it all to the frontend device */
    if ( ioctl( i_frontend, FE_SET_FRONTEND, &fep ) < 0 )
    {
        msg_Err( NULL, "setting frontend failed (%s)", strerror(errno) );
        exit(1);
    }

    i_last_status = 0;
    i_frontend_timeout = mdate() + FRONTEND_LOCK_TIMEOUT;
}

/*****************************************************************************
 * FrontendSetQPSK
 *****************************************************************************/
static void FrontendSetQPSK( struct dvb_frontend_parameters *p_fep )
{
    fe_sec_voltage_t fe_voltage;
    fe_sec_tone_mode_t fe_tone;

    p_fep->inversion = INVERSION_AUTO;
    p_fep->u.qpsk.symbol_rate = i_srate;
    p_fep->u.qpsk.fec_inner = FEC_AUTO;

    switch ( i_voltage )
    {
        case 0: fe_voltage = SEC_VOLTAGE_OFF; break;
        default:
        case 13: fe_voltage = SEC_VOLTAGE_13; break;
        case 18: fe_voltage = SEC_VOLTAGE_18; break;
    }

    fe_tone = b_tone ? SEC_TONE_ON : SEC_TONE_OFF;

    /* Automatic mode. */
    if ( i_frequency >= 950000 && i_frequency <= 2150000 )
    {
        msg_Dbg( NULL, "frequency %d is in IF-band", i_frequency );
        p_fep->frequency = i_frequency;
    }
    else if ( i_frequency >= 2500000 && i_frequency <= 2700000 )
    {
        msg_Dbg( NULL, "frequency %d is in S-band", i_frequency );
        p_fep->frequency = i_frequency - 3650000;
    }
    else if ( i_frequency >= 3400000 && i_frequency <= 4200000 )
    {
        msg_Dbg( NULL, "frequency %d is in C-band (lower)",
                 i_frequency );
        p_fep->frequency = i_frequency - 5150000;
    }
    else if ( i_frequency >= 4500000 && i_frequency <= 4800000 )
    {
        msg_Dbg( NULL, "frequency %d is in C-band (higher)",
                 i_frequency );
        p_fep->frequency = i_frequency - 5950000;
    }
    else if ( i_frequency >= 10700000 && i_frequency < 11700000 )
    {
        msg_Dbg( NULL, "frequency %d is in Ku-band (lower)",
                 i_frequency );
        p_fep->frequency = i_frequency - 9750000;
    }
    else if ( i_frequency >= 11700000 && i_frequency <= 13250000 )
    {
        msg_Dbg( NULL, "frequency %d is in Ku-band (higher)",
                 i_frequency );
        p_fep->frequency = i_frequency - 10600000;
        fe_tone = SEC_TONE_ON;
    }
    else
    {
        msg_Err( NULL, "frequency %d is out of any known band",
                 i_frequency );
        exit(1);
    }

    /* Switch off continuous tone. */
    if ( ioctl( i_frontend, FE_SET_TONE, SEC_TONE_OFF ) < 0 )
    {
        msg_Err( NULL, "FE_SET_TONE failed (%s)", strerror(errno) );
        exit(1);
    }

    /* Configure LNB voltage. */
    if ( ioctl( i_frontend, FE_SET_VOLTAGE, fe_voltage ) < 0 )
    {
        msg_Err( NULL, "FE_SET_VOLTAGE failed (%s)", strerror(errno) );
        exit(1);
    }

    /* Wait for at least 15 ms. */
    msleep(15000);

    if ( ioctl( i_frontend, FE_SET_TONE, fe_tone ) < 0 )
    {
        msg_Err( NULL, "FE_SET_TONE failed (%s)", strerror(errno) );
        exit(1);
    }

    /* TODO: Diseqc */

    msleep(50000);

    msg_Dbg( NULL, "tuning QPSK frontend to f=%d, srate=%d, v=%d, p=%d",
             i_frequency, i_srate, i_voltage, b_tone );
}

/*****************************************************************************
 * FrontendSetQAM
 *****************************************************************************/
static void FrontendSetQAM( struct dvb_frontend_parameters *p_fep )
{
    p_fep->frequency = i_frequency;
    p_fep->inversion = INVERSION_AUTO;
    p_fep->u.qam.symbol_rate = i_srate;
    p_fep->u.qam.fec_inner = FEC_AUTO;
    p_fep->u.qam.modulation = QAM_AUTO;

    msg_Dbg( NULL, "tuning QAM frontend to f=%d, srate=%d", i_frequency,
             i_srate );
}

/*****************************************************************************
 * FrontendSetOFDM
 *****************************************************************************/
static void FrontendSetOFDM( struct dvb_frontend_parameters *p_fep )
{
    p_fep->frequency = i_frequency;
    p_fep->inversion = INVERSION_AUTO;

    switch ( i_bandwidth )
    {
        case 6: p_fep->u.ofdm.bandwidth = BANDWIDTH_6_MHZ; break;
        case 7: p_fep->u.ofdm.bandwidth = BANDWIDTH_7_MHZ; break;
        default:
        case 8: p_fep->u.ofdm.bandwidth = BANDWIDTH_8_MHZ; break;
    }

    p_fep->u.ofdm.code_rate_HP = FEC_AUTO;
    p_fep->u.ofdm.code_rate_LP = FEC_AUTO;
    p_fep->u.ofdm.constellation = QAM_AUTO;
    p_fep->u.ofdm.transmission_mode = TRANSMISSION_MODE_AUTO;
    p_fep->u.ofdm.guard_interval = GUARD_INTERVAL_AUTO;
    p_fep->u.ofdm.hierarchy_information = HIERARCHY_AUTO;

    msg_Dbg( NULL, "tuning OFDM frontend to f=%d, bandwidth=%d", i_frequency,
             i_bandwidth );
}

/*****************************************************************************
 * FrontendSetATSC
 *****************************************************************************/
static void FrontendSetATSC( struct dvb_frontend_parameters *p_fep )
{
    p_fep->frequency = i_frequency;

    p_fep->u.vsb.modulation = QAM_AUTO;

    msg_Dbg( NULL, "tuning ATSC frontend to f=%d", i_frequency );
}

#endif /* S2API */

/*****************************************************************************
 * dvb_FrontendStatus
 *****************************************************************************/
uint8_t dvb_FrontendStatus( uint8_t *p_answer, ssize_t *pi_size )
{
    struct ret_frontend_status *p_ret = (struct ret_frontend_status *)p_answer;

    if ( ioctl( i_frontend, FE_GET_INFO, &p_ret->info ) < 0 )
    {
        msg_Err( NULL, "ioctl FE_GET_INFO failed (%s)", strerror(errno) );
        return RET_ERR;
    }
    if ( ioctl( i_frontend, FE_READ_STATUS, &p_ret->i_status ) < 0 )
    {
        msg_Err( NULL, "ioctl FE_READ_STATUS failed (%s)", strerror(errno) );
        return RET_ERR;
    }

    if ( p_ret->i_status & FE_HAS_LOCK )
    {
        if ( ioctl( i_frontend, FE_READ_BER, &p_ret->i_ber ) < 0 )
            msg_Err( NULL, "ioctl FE_READ_BER failed (%s)", strerror(errno) );

        if ( ioctl( i_frontend, FE_READ_SIGNAL_STRENGTH, &p_ret->i_strength )
              < 0 )
            msg_Err( NULL, "ioctl FE_READ_SIGNAL_STRENGTH failed (%s)",
                     strerror(errno) );

        if ( ioctl( i_frontend, FE_READ_SNR, &p_ret->i_snr ) < 0 )
            msg_Err( NULL, "ioctl FE_READ_SNR failed (%s)", strerror(errno) );
    }

    *pi_size = sizeof(struct ret_frontend_status);
    return RET_FRONTEND_STATUS;
}
