/*****************************************************************************
 * comm.c
 *****************************************************************************
 * Copyright (C) 2008 VideoLAN
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

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
int i_comm_fd = -1;

/*****************************************************************************
 * comm_Open
 *****************************************************************************/
void comm_Open( void )
{
    int i_mask;
    int i_size = 65535;
    struct sockaddr_un sun_server;

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
    i_mask = umask(077);

    if ( bind( i_comm_fd, (struct sockaddr *)&sun_server,
               SUN_LEN(&sun_server) ) < 0 )
    {
        msg_Err( NULL, "cannot bind comm socket (%s)", strerror(errno) );
        umask( i_mask );
        close( i_comm_fd );
        i_comm_fd = -1;
        return;
    }
    umask( i_mask );
}

/*****************************************************************************
 * comm_Read
 *****************************************************************************/
void comm_Read( void )
{
    struct sockaddr_un sun_client;
    socklen_t sun_length = sizeof(sun_client);
    ssize_t i_size, i_answer_size = 0;
    uint8_t p_buffer[COMM_BUFFER_SIZE], p_answer[COMM_BUFFER_SIZE];
    uint8_t i_command, i_answer;

    i_size = recvfrom( i_comm_fd, p_buffer, COMM_BUFFER_SIZE, 0,
                       (struct sockaddr *)&sun_client, &sun_length );
    if ( i_size < COMM_HEADER_SIZE )
    {
        msg_Err( NULL, "cannot read comm socket (%d:%s)\n", i_size,
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

    switch ( i_command )
    {
    case CMD_RELOAD:
        b_hup_received = 1;
        i_answer = RET_OK;
        i_answer_size = 0;
        break;

    case CMD_FRONTEND_STATUS:
        i_answer = dvb_FrontendStatus( p_answer + COMM_HEADER_SIZE,
                                       &i_answer_size );
        break;

    case CMD_MMI_STATUS:
        i_answer = en50221_StatusMMI( p_answer + COMM_HEADER_SIZE,
                                      &i_answer_size );
        break;

    case CMD_MMI_SLOT_STATUS:
        i_answer = en50221_StatusMMISlot( p_buffer + COMM_HEADER_SIZE,
                                          i_size - COMM_HEADER_SIZE,
                                          p_answer + COMM_HEADER_SIZE,
                                          &i_answer_size );
        break;

    case CMD_MMI_OPEN:
        i_answer = en50221_OpenMMI( p_buffer + COMM_HEADER_SIZE,
                                    i_size - COMM_HEADER_SIZE );
        break;

    case CMD_MMI_CLOSE:
        i_answer = en50221_CloseMMI( p_buffer + COMM_HEADER_SIZE,
                                     i_size - COMM_HEADER_SIZE );
        break;

    case CMD_MMI_RECV:
        i_answer = en50221_GetMMIObject( p_buffer + COMM_HEADER_SIZE,
                                         i_size - COMM_HEADER_SIZE,
                                         p_answer + COMM_HEADER_SIZE,
                                         &i_answer_size );
        break;

    case CMD_MMI_SEND:
        i_answer = en50221_SendMMIObject( p_buffer + COMM_HEADER_SIZE,
                                          i_size - COMM_HEADER_SIZE );
        break;

    case CMD_SHUTDOWN:
        msg_Err( NULL, "shutdown via comm" );
        exit(EXIT_SUCCESS);
        /* this is a bit violent, but hey, closing everything cleanly
         * would do approximately the same */

    default:
        msg_Err( NULL, "wrong command %u", i_command );
        i_answer = RET_HUH;
        i_answer_size = 0;
        break;
    }

    p_answer[0] = COMM_HEADER_MAGIC;
    p_answer[1] = i_answer;
    p_answer[2] = 0;
    p_answer[3] = 0;
    msg_Dbg( NULL, "answering %d to %d with size %d", i_answer, i_command,
             i_answer_size );

    if ( sendto( i_comm_fd, p_answer, i_answer_size + COMM_HEADER_SIZE, 0,
                 (struct sockaddr *)&sun_client, sun_length ) < 0 )
        msg_Err( NULL, "cannot send comm socket (%s)", strerror(errno) );
}
