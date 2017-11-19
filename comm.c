/*****************************************************************************
 * comm.c: Handles the communication socket
 *****************************************************************************
 * Copyright (C) 2008, 2015 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <errno.h>
#include <ev.h>

#include "dvblast.h"
#include "en50221.h"
#include "comm.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
static int i_comm_fd = -1;
static struct ev_io comm_watcher;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void comm_Read(struct ev_loop *loop, struct ev_io *w, int revents);

/*****************************************************************************
 * comm_Open
 *****************************************************************************/
void comm_Open( void )
{
    int i_size = COMM_MAX_MSG_CHUNK;
    struct sockaddr_un sun_server;

    unlink( psz_srv_socket );

    if ( (i_comm_fd = socket( AF_UNIX, SOCK_DGRAM, 0 )) == -1 )
    {
        msg_Err( NULL, "cannot create comm socket (%s)", strerror(errno) );
        return;
    }

    setsockopt( i_comm_fd, SOL_SOCKET, SO_RCVBUF, &i_size, sizeof(i_size) );

    memset( &sun_server, 0, sizeof(sun_server) );
    sun_server.sun_family = AF_UNIX;
    strncpy( sun_server.sun_path, psz_srv_socket, sizeof(sun_server.sun_path) );
    sun_server.sun_path[sizeof(sun_server.sun_path) - 1] = '\0';

    if ( bind( i_comm_fd, (struct sockaddr *)&sun_server,
               SUN_LEN(&sun_server) ) < 0 )
    {
        msg_Err( NULL, "cannot bind comm socket (%s)", strerror(errno) );
        close( i_comm_fd );
        i_comm_fd = -1;
        return;
    }

    ev_io_init(&comm_watcher, comm_Read, i_comm_fd, EV_READ);
    ev_io_start(event_loop, &comm_watcher);
}

/*****************************************************************************
 * comm_Read
 *****************************************************************************/
static void comm_Read(struct ev_loop *loop, struct ev_io *w, int revents)
{
    struct sockaddr_un sun_client;
    socklen_t sun_length = sizeof(sun_client);
    ssize_t i_size, i_answer_size = 0;
    uint8_t p_buffer[COMM_BUFFER_SIZE], p_answer[COMM_BUFFER_SIZE];
    uint8_t i_command, i_answer;
    uint8_t *p_packed_section;
    unsigned int i_packed_section_size;
    uint8_t *p_input = p_buffer + COMM_HEADER_SIZE;
    uint8_t *p_output = p_answer + COMM_HEADER_SIZE;

    i_size = recvfrom( i_comm_fd, p_buffer, COMM_BUFFER_SIZE, 0,
                       (struct sockaddr *)&sun_client, &sun_length );
    if ( i_size < COMM_HEADER_SIZE )
    {
        msg_Err( NULL, "cannot read comm socket (%zd:%s)\n", i_size,
                 strerror(errno) );
        return;
    }
    if ( sun_length == 0 || sun_length > sizeof(sun_client) )
    {
        msg_Err( NULL, "anonymous packet from comm socket\n" );
        return;
    }

    if ( p_buffer[0] != COMM_HEADER_MAGIC )
    {
        msg_Err( NULL, "wrong protocol version 0x%x", p_buffer[0] );
        return;
    }

    i_command = p_buffer[1];

    if ( i_frequency == 0 ) /* ASI or UDP, disable DVB only commands */
    {
        switch ( i_command )
        {
            case CMD_FRONTEND_STATUS:
            case CMD_MMI_STATUS:
            case CMD_MMI_SLOT_STATUS:
            case CMD_MMI_OPEN:
            case CMD_MMI_CLOSE:
            case CMD_MMI_RECV:
            case CMD_MMI_SEND_TEXT:
            case CMD_MMI_SEND_CHOICE:
                i_answer = RET_NODATA;
                i_answer_size = 0;
                goto return_answer;
        }
    }

    switch ( i_command )
    {
    case CMD_RELOAD:
        config_ReadFile();
        i_answer = RET_OK;
        i_answer_size = 0;
        break;

#ifdef HAVE_DVB_SUPPORT
    case CMD_FRONTEND_STATUS:
        i_answer = dvb_FrontendStatus( p_answer + COMM_HEADER_SIZE,
                                       &i_answer_size );
        break;

    case CMD_MMI_STATUS:
        i_answer = en50221_StatusMMI( p_answer + COMM_HEADER_SIZE,
                                      &i_answer_size );
        break;

    case CMD_MMI_SLOT_STATUS:
        i_answer = en50221_StatusMMISlot( p_input, i_size - COMM_HEADER_SIZE,
                                          p_answer + COMM_HEADER_SIZE,
                                          &i_answer_size );
        break;

    case CMD_MMI_OPEN:
        i_answer = en50221_OpenMMI( p_input, i_size - COMM_HEADER_SIZE );
        break;

    case CMD_MMI_CLOSE:
        i_answer = en50221_CloseMMI( p_input, i_size - COMM_HEADER_SIZE );
        break;

    case CMD_MMI_RECV:
        i_answer = en50221_GetMMIObject( p_input, i_size - COMM_HEADER_SIZE,
                                         p_answer + COMM_HEADER_SIZE,
                                         &i_answer_size );
        break;

    case CMD_MMI_SEND_TEXT:
    case CMD_MMI_SEND_CHOICE:
        i_answer = en50221_SendMMIObject( p_input, i_size - COMM_HEADER_SIZE );
        break;
#endif

    case CMD_SHUTDOWN:
        ev_break(loop, EVBREAK_ALL);
        i_answer = RET_OK;
        i_answer_size = 0;
        break;

    case CMD_GET_PAT:
    case CMD_GET_CAT:
    case CMD_GET_NIT:
    case CMD_GET_SDT:
    {
#define CASE_TABLE(x) \
        case CMD_GET_##x: \
        { \
            i_answer = RET_##x; \
            p_packed_section = demux_get_current_packed_##x(&i_packed_section_size); \
            break; \
        }
        switch ( i_command )
        {
            CASE_TABLE(PAT)
            CASE_TABLE(CAT)
            CASE_TABLE(NIT)
            CASE_TABLE(SDT)
        }
#undef CASE_TABLE

        if ( p_packed_section && i_packed_section_size )
        {
            if ( i_packed_section_size <= COMM_BUFFER_SIZE - COMM_HEADER_SIZE )
            {
                i_answer_size = i_packed_section_size;
                memcpy( p_answer + COMM_HEADER_SIZE, p_packed_section, i_packed_section_size );
            } else {
                msg_Err( NULL, "section size is too big (%u)\n", i_packed_section_size );
                i_answer = RET_NODATA;
            }
            free( p_packed_section );
        } else {
            i_answer = RET_NODATA;
        }

        break;
    }

    case CMD_GET_EIT_PF:
    case CMD_GET_EIT_SCHEDULE:
    case CMD_GET_PMT:
    {
        if ( i_size < COMM_HEADER_SIZE + 2 )
        {
            msg_Err( NULL, "command packet is too short (%zd)\n", i_size );
            return;
        }

        uint16_t i_sid = (uint16_t)((p_input[0] << 8) | p_input[1]);
        if ( i_command == CMD_GET_EIT_PF ) {
            i_answer = RET_EIT_PF;
            p_packed_section = demux_get_packed_EIT_pf( i_sid, &i_packed_section_size );
        } else if ( i_command == CMD_GET_EIT_SCHEDULE ) {
            i_answer = RET_EIT_SCHEDULE;
            p_packed_section = demux_get_packed_EIT_schedule( i_sid, &i_packed_section_size );
        } else {
            i_answer = RET_PMT;
            p_packed_section = demux_get_packed_PMT(i_sid, &i_packed_section_size);
        }

        if ( p_packed_section && i_packed_section_size )
        {
            i_answer_size = i_packed_section_size;
            memcpy( p_answer + COMM_HEADER_SIZE, p_packed_section, i_packed_section_size );
            free( p_packed_section );
        } else {
            i_answer = RET_NODATA;
        }

        break;
    }

    case CMD_GET_PIDS:
    {
        i_answer = RET_PIDS;
        i_answer_size = sizeof(struct cmd_pid_info);
        demux_get_PIDS_info( p_output );
        break;
    }

    case CMD_GET_PID:
    {
        if ( i_size < COMM_HEADER_SIZE + 2 )
        {
            msg_Err( NULL, "command packet is too short (%zd)\n", i_size );
            return;
        }

        uint16_t i_pid = (uint16_t)((p_input[0] << 8) | p_input[1]);
        if ( i_pid >= MAX_PIDS ) {
            i_answer = RET_NODATA;
        } else {
            i_answer = RET_PID;
            i_answer_size = sizeof(ts_pid_info_t);
            demux_get_PID_info( i_pid, p_output );
        }
        break;
    }

    default:
        msg_Err( NULL, "wrong command %u", i_command );
        i_answer = RET_HUH;
        i_answer_size = 0;
        break;
    }

 return_answer:
    p_answer[0] = COMM_HEADER_MAGIC;
    p_answer[1] = i_answer;
    p_answer[2] = 0;
    p_answer[3] = 0;
    uint32_t *p_size = (uint32_t *)&p_answer[4];
    *p_size = i_answer_size + COMM_HEADER_SIZE;

/*    msg_Dbg( NULL, "answering %d to %d with size %zd", i_answer, i_command,
             i_answer_size ); */

#define min(a, b) (a < b ? a : b)
    ssize_t i_sended = 0;
    ssize_t i_to_send = i_answer_size + COMM_HEADER_SIZE;
    do {
        ssize_t i_sent = sendto( i_comm_fd, p_answer + i_sended,
                     min(i_to_send, COMM_MAX_MSG_CHUNK), 0,
                     (struct sockaddr *)&sun_client, sun_length );

        if ( i_sent < 0 ) {
            msg_Err( NULL, "cannot send comm socket (%s)", strerror(errno) );
            break;
        }

        i_sended += i_sent;
        i_to_send -= i_sent;
    } while ( i_to_send > 0 );
#undef min
}

/*****************************************************************************
 * comm_Close
 *****************************************************************************/
void comm_Close( void )
{
    if (i_comm_fd > -1)
    {
        ev_io_stop(event_loop, &comm_watcher);
        close(i_comm_fd);
        unlink(psz_srv_socket);
    }
}
