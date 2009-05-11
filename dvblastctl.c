/*****************************************************************************
 * dvblastctl.c
 *****************************************************************************
 * Copyright (C) 2008 the VideoLAN team
 * $Id: dvblast.c 11 2007-08-09 18:54:37Z cmassiot $
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
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <errno.h>

#include "dvblast.h"
#include "en50221.h"
#include "comm.h"

void usage()
{
    msg_Err( NULL, "Usage: dvblastctl -r <remote socket> reload|shutdown|fe_status|mmi_status|mmi_open|mmi_close|mmi_get|mmi_send_text|mmi_send_choice [<CAM slot>] [<text/choice>]" );
    exit(1);
}

int main( int i_argc, char **ppsz_argv )
{
    char psz_client_socket[FILENAME_MAX];
    char *psz_srv_socket = NULL;
    int i_fd, i_mask;
    int i = 65535;
    ssize_t i_size;
    struct sockaddr_un sun_client, sun_server;
    uint8_t p_buffer[COMM_BUFFER_SIZE];

    for ( ; ; )
    {
        char c;

        if ( (c = getopt(i_argc, ppsz_argv, "r:h")) == -1 )
            break;

        switch ( c )
        {
        case 'r':
            psz_srv_socket = optarg;
            break;

        case 'h':
        default:
            usage();
        }
    }

    if ( ppsz_argv[optind] == NULL || psz_srv_socket == NULL )
        usage();

    if ( strcmp(ppsz_argv[optind], "reload")
          && strcmp(ppsz_argv[optind], "shutdown")
          && strcmp(ppsz_argv[optind], "fe_status")
          && strcmp(ppsz_argv[optind], "mmi_status")
          && strcmp(ppsz_argv[optind], "mmi_slot_status")
          && strcmp(ppsz_argv[optind], "mmi_open")
          && strcmp(ppsz_argv[optind], "mmi_close")
          && strcmp(ppsz_argv[optind], "mmi_get")
          && strcmp(ppsz_argv[optind], "mmi_send_text")
          && strcmp(ppsz_argv[optind], "mmi_send_choice") )
        usage();

    if ( (!strcmp(ppsz_argv[optind], "mmi_slot_status")
           || !strcmp(ppsz_argv[optind], "mmi_open")
           || !strcmp(ppsz_argv[optind], "mmi_close")
           || !strcmp(ppsz_argv[optind], "mmi_get")
           || !strcmp(ppsz_argv[optind], "mmi_send_text")
           || !strcmp(ppsz_argv[optind], "mmi_send_choice"))
          && ppsz_argv[optind + 1] == NULL )
        usage();

    if ( !strcmp(ppsz_argv[optind], "mmi_send_choice")
          && ppsz_argv[optind + 2] == NULL )
        usage();

#warning expect brain-dead gcc warning about tmpnam here
    tmpnam(psz_client_socket);
    if ( (i_fd = socket( AF_UNIX, SOCK_DGRAM, 0 )) < 0 )
    {
        msg_Err( NULL, "cannot create UNIX socket (%s)", strerror(errno) );
        return -1;
    }

    setsockopt( i_fd, SOL_SOCKET, SO_RCVBUF, &i, sizeof(i) );

    memset( &sun_client, 0, sizeof(sun_client) );
    sun_client.sun_family = AF_UNIX;
    strncpy( sun_client.sun_path, psz_client_socket,
             sizeof(sun_client.sun_path) );
    sun_client.sun_path[sizeof(sun_client.sun_path) - 1] = '\0';
    i_mask = umask(077);

    if ( bind( i_fd, (struct sockaddr *)&sun_client,
               SUN_LEN(&sun_client) ) < 0 )
    {
        msg_Err( NULL, "cannot bind (%s)", strerror(errno) );
        umask( i_mask );
        close( i_fd );
        exit(255);
    }
    umask( i_mask );

    memset( &sun_server, 0, sizeof(sun_server) );
    sun_server.sun_family = AF_UNIX;
    strncpy( sun_server.sun_path, psz_srv_socket, sizeof(sun_server.sun_path) );
    sun_server.sun_path[sizeof(sun_server.sun_path) - 1] = '\0';

    p_buffer[0] = COMM_HEADER_MAGIC;
    p_buffer[2] = 0;
    p_buffer[3] = 0;
    i_size = COMM_HEADER_SIZE;

    if ( !strcmp(ppsz_argv[optind], "reload") )
        p_buffer[1] = CMD_RELOAD;
    else if ( !strcmp(ppsz_argv[optind], "shutdown") )
        p_buffer[1] = CMD_SHUTDOWN;
    else if ( !strcmp(ppsz_argv[optind], "fe_status") )
        p_buffer[1] = CMD_FRONTEND_STATUS;
    else if ( !strcmp(ppsz_argv[optind], "mmi_status") )
        p_buffer[1] = CMD_MMI_STATUS;
    else
    {
        p_buffer[4] = atoi(ppsz_argv[optind + 1]);
        i_size = COMM_HEADER_SIZE + 1;

        if ( !strcmp(ppsz_argv[optind], "mmi_slot_status") )
            p_buffer[1] = CMD_MMI_SLOT_STATUS;
        else if ( !strcmp(ppsz_argv[optind], "mmi_open") )
            p_buffer[1] = CMD_MMI_OPEN;
        else if ( !strcmp(ppsz_argv[optind], "mmi_close") )
            p_buffer[1] = CMD_MMI_CLOSE;
        else if ( !strcmp(ppsz_argv[optind], "mmi_get") )
            p_buffer[1] = CMD_MMI_RECV;
        else
        {
            struct cmd_mmi_send *p_cmd = (struct cmd_mmi_send *)&p_buffer[4];
            p_buffer[1] = CMD_MMI_SEND;
            p_cmd->i_slot = atoi(ppsz_argv[optind + 1]);

            if ( !strcmp(ppsz_argv[optind], "mmi_send_text") )
            {
                en50221_mmi_object_t object;
                object.i_object_type = EN50221_MMI_ANSW;
                if ( ppsz_argv[optind + 2] == NULL
                      || ppsz_argv[optind + 2][0] == '\0' )
                {
                     object.u.answ.b_ok = 0;
                     object.u.answ.psz_answ = "";
                }
                else
                {
                     object.u.answ.b_ok = 1;
                     object.u.answ.psz_answ = ppsz_argv[optind + 2];
                }
                i_size = COMM_BUFFER_SIZE - COMM_HEADER_SIZE
                          - ((void *)&p_cmd->object - (void *)p_cmd);
                if ( en50221_SerializeMMIObject( (uint8_t *)&p_cmd->object,
                                                 &i_size, &object ) == -1 )
                {
                    msg_Err( NULL, "buffer too small" );
                    close( i_fd );
                    unlink( psz_client_socket );
                    exit(255);
                }
                i_size += COMM_HEADER_SIZE
                           + ((void *)&p_cmd->object - (void *)p_cmd);
            }
            else /* mmi_send_choice */
            {
                i_size = COMM_HEADER_SIZE + sizeof(struct cmd_mmi_send);
                p_cmd->object.i_object_type = EN50221_MMI_MENU_ANSW;
                p_cmd->object.u.menu_answ.i_choice
                    = atoi(ppsz_argv[optind + 2]);
            }
        }
    }

    if ( sendto( i_fd, p_buffer, i_size, 0, (struct sockaddr *)&sun_server,
                 SUN_LEN(&sun_server) ) < 0 )
    {
        msg_Err( NULL, "cannot send comm socket (%s)", strerror(errno) );
        close( i_fd );
        unlink( psz_client_socket );
        exit(255);
    }

    i_size = recv( i_fd, p_buffer, COMM_BUFFER_SIZE, 0 );
    close( i_fd );
    unlink( psz_client_socket );
    if ( i_size < COMM_HEADER_SIZE )
    {
        msg_Err( NULL, "cannot recv comm socket (%d:%s)", i_size,
                 strerror(errno) );
        exit(255);
    }

    if ( p_buffer[0] != COMM_HEADER_MAGIC )
    {
        msg_Err( NULL, "wrong protocol version 0x%x", p_buffer[0] );
        exit(255);
    }

    switch ( p_buffer[1] )
    {
    case RET_OK:
        exit(0);
        break;

    case RET_ERR:
        msg_Err( NULL, "request failed" );
        exit(255);
        break;

    case RET_HUH:
        msg_Err( NULL, "internal error" );
        exit(255);
        break;

    case RET_FRONTEND_STATUS:
    {
        struct ret_frontend_status *p_ret =
            (struct ret_frontend_status *)&p_buffer[COMM_HEADER_SIZE];
        if ( i_size != COMM_HEADER_SIZE + sizeof(struct ret_frontend_status) )
        {
            msg_Err( NULL, "bad frontend status" );
            exit(255);
        }

        switch ( p_ret->info.type )
        {
        case FE_QPSK: printf("type: QPSK\n"); break;
        case FE_QAM: printf("type: QAM\n"); break;
        case FE_OFDM: printf("type: OFDM\n"); break;
        default: printf("type: UNKNOWN\n"); break;
        }

#define PRINT_INFO( x )                                                 \
        printf( STRINGIFY(x) ": %u\n", p_ret->info.x );
        PRINT_INFO( frequency_min );
        PRINT_INFO( frequency_max );
        PRINT_INFO( frequency_stepsize );
        PRINT_INFO( frequency_tolerance );
        PRINT_INFO( symbol_rate_min );
        PRINT_INFO( symbol_rate_max );
        PRINT_INFO( symbol_rate_tolerance );
        PRINT_INFO( notifier_delay );
#undef PRINT_INFO

        printf("\ncapability list:\n");

#define PRINT_CAPS( x )                                                 \
        if ( p_ret->info.caps & (FE_##x) )                              \
            printf( STRINGIFY(x) "\n" );
        PRINT_CAPS( IS_STUPID );
        PRINT_CAPS( CAN_INVERSION_AUTO );
        PRINT_CAPS( CAN_FEC_1_2 );
        PRINT_CAPS( CAN_FEC_2_3 );
        PRINT_CAPS( CAN_FEC_3_4 );
        PRINT_CAPS( CAN_FEC_4_5 );
        PRINT_CAPS( CAN_FEC_5_6 );
        PRINT_CAPS( CAN_FEC_6_7 );
        PRINT_CAPS( CAN_FEC_7_8 );
        PRINT_CAPS( CAN_FEC_8_9 );
        PRINT_CAPS( CAN_FEC_AUTO );
        PRINT_CAPS( CAN_QPSK );
        PRINT_CAPS( CAN_QAM_16 );
        PRINT_CAPS( CAN_QAM_32 );
        PRINT_CAPS( CAN_QAM_64 );
        PRINT_CAPS( CAN_QAM_128 );
        PRINT_CAPS( CAN_QAM_256 );
        PRINT_CAPS( CAN_QAM_AUTO );
        PRINT_CAPS( CAN_TRANSMISSION_MODE_AUTO );
        PRINT_CAPS( CAN_BANDWIDTH_AUTO );
        PRINT_CAPS( CAN_GUARD_INTERVAL_AUTO );
        PRINT_CAPS( CAN_HIERARCHY_AUTO );
        PRINT_CAPS( CAN_8VSB );
        PRINT_CAPS( CAN_16VSB );
#if DVB_API_VERSION >= 5
        PRINT_CAPS( HAS_EXTENDED_CAPS );
        /* FIXME: apparently this doesn't exist in some versions of the
         * linux-dvb headers */
        /* PRINT_CAPS( CAN_2G_MODULATION ); */
#endif
        PRINT_CAPS( NEEDS_BENDING );
        PRINT_CAPS( CAN_MUTE_TS );
        PRINT_CAPS( CAN_RECOVER );
#undef PRINT_CAPS

        printf("\nstatus:\n");

#define PRINT_STATUS( x )                                               \
        if ( p_ret->i_status & (FE_##x) )                               \
            printf( STRINGIFY(x) "\n" );
        PRINT_STATUS( HAS_SIGNAL );
        PRINT_STATUS( HAS_CARRIER );
        PRINT_STATUS( HAS_VITERBI );
        PRINT_STATUS( HAS_SYNC );
        PRINT_STATUS( HAS_LOCK );
        PRINT_STATUS( REINIT );
#undef PRINT_STATUS

        if ( p_ret->i_status & FE_HAS_LOCK )
        {
            printf("\nBit error rate: %d\n", p_ret->i_ber);
            printf("Signal strength: %d\n", p_ret->i_strength);
            printf("SNR: %d\n", p_ret->i_snr);
            exit(0);
        }

        exit(1);
        break;
    }

    case RET_MMI_STATUS:
    {
        struct ret_mmi_status *p_ret =
            (struct ret_mmi_status *)&p_buffer[COMM_HEADER_SIZE];
        if ( i_size != COMM_HEADER_SIZE + sizeof(struct ret_mmi_status) )
        {
            msg_Err( NULL, "bad MMI status" );
            exit(255);
        }

        printf("CA interface with %d %s, type:\n", p_ret->caps.slot_num,
               p_ret->caps.slot_num == 1 ? "slot" : "slots");
#define PRINT_CAPS( x, s )                                              \
        if ( p_ret->caps.slot_type & (CA_##x) )                         \
            printf(s "\n");
        PRINT_CAPS( CI, "CI high level interface" );
        PRINT_CAPS( CI_LINK, "CI link layer level interface" );
        PRINT_CAPS( CI_PHYS, "CI physical layer level interface (not supported)" );
        PRINT_CAPS( DESCR, "built-in descrambler" );
        PRINT_CAPS( SC, "simple smartcard interface" );
#undef PRINT_CAPS

        printf("\n%d available %s\n", p_ret->caps.descr_num,
            p_ret->caps.descr_num == 1 ? "descrambler (key)" :
                                         "descramblers (keys)");
#define PRINT_DESC( x )                                                 \
        if ( p_ret->caps.descr_type & (CA_##x) )                        \
            printf( STRINGIFY(x) "\n" );
        PRINT_DESC( ECD );
        PRINT_DESC( NDS );
        PRINT_DESC( DSS );
#undef PRINT_DESC

        exit( p_ret->caps.slot_num );
        break;
    }

    case RET_MMI_SLOT_STATUS:
    {
        struct ret_mmi_slot_status *p_ret =
            (struct ret_mmi_slot_status *)&p_buffer[COMM_HEADER_SIZE];
        if ( i_size < COMM_HEADER_SIZE + sizeof(struct ret_mmi_slot_status) )
        {
            msg_Err( NULL, "bad MMI slot status" );
            exit(255);
        }

        printf("CA slot #%u: ", p_ret->sinfo.num);

#define PRINT_TYPE( x, s )                                                  \
        if ( p_ret->sinfo.type & (CA_##x) )                                 \
            printf(s);

        PRINT_TYPE( CI, "high level, " );
        PRINT_TYPE( CI_LINK, "link layer level, " );
        PRINT_TYPE( CI_PHYS, "physical layer level, " );
#undef PRINT_TYPE

        if ( p_ret->sinfo.flags & CA_CI_MODULE_READY )
        {
            printf("module present and ready\n");
            exit(0);
        }

        if ( p_ret->sinfo.flags & CA_CI_MODULE_PRESENT )
            printf("module present, not ready\n");
        else
            printf("module not present\n");

        exit(1);
        break;
    }

    case RET_MMI_RECV:
    {
        struct ret_mmi_recv *p_ret =
            (struct ret_mmi_recv *)&p_buffer[COMM_HEADER_SIZE];
        if ( i_size < COMM_HEADER_SIZE + sizeof(struct ret_mmi_recv) )
        {
            msg_Err( NULL, "bad MMI recv" );
            exit(255);
        }

        en50221_UnserializeMMIObject( &p_ret->object, i_size
          - COMM_HEADER_SIZE - ((void *)&p_ret->object - (void *)p_ret) );

        switch ( p_ret->object.i_object_type )
        {
        case EN50221_MMI_ENQ:
            printf("%s\n", p_ret->object.u.enq.psz_text);
            printf("(empty to cancel)\n");
            exit(p_ret->object.u.enq.b_blind ? 253 : 254);
            break;

        case EN50221_MMI_MENU:
            printf("%s\n", p_ret->object.u.menu.psz_title);
            printf("%s\n", p_ret->object.u.menu.psz_subtitle);
            printf("0 - Cancel\n");
            for ( i = 0; i < p_ret->object.u.menu.i_choices; i++ )
                printf("%d - %s\n", i + 1,
                       p_ret->object.u.menu.ppsz_choices[i]);
            printf("%s\n", p_ret->object.u.menu.psz_bottom);
            exit(p_ret->object.u.menu.i_choices);
            break;

        case EN50221_MMI_LIST:
            printf("%s\n", p_ret->object.u.menu.psz_title);
            printf("%s\n", p_ret->object.u.menu.psz_subtitle);
            for ( i = 0; i < p_ret->object.u.menu.i_choices; i++ )
                printf("%s\n", p_ret->object.u.menu.ppsz_choices[i]);
            printf("%s\n", p_ret->object.u.menu.psz_bottom);
            printf("(0 to cancel)\n");
            exit(0);
            break;

        default:
            printf("unknown MMI object\n");
            exit(255);
            break;
        }

        exit(255);
        break;
    }

    default:
        msg_Err( NULL, "wrong answer %u", p_buffer[1] );
        exit(255);
    }
}
