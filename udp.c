/*****************************************************************************
 * udp.c: UDP input for DVBlast
 *****************************************************************************
 * Copyright (C) 2009 VideoLAN
 * $Id: udp.c 9 2007-03-15 16:58:05Z cmassiot $
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
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
static int i_handle;

/*****************************************************************************
 * udp_Open
 *****************************************************************************/
void udp_Open( void )
{
    i_handle = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sin;
    int i = 1;

    setsockopt( i_handle, SOL_SOCKET, SO_REUSEADDR, (void *) &i, sizeof( i ) );

    /* Increase the receive buffer size to 1/2MB (8Mb/s during 1/2s) to avoid
     * packet loss caused by scheduling problems */
    i = 0x80000;
    setsockopt( i_handle, SOL_SOCKET, SO_RCVBUF, (void *) &i, sizeof( i ) );

    sin.sin_family = AF_INET;
    sin.sin_port = htons(i_src_port);
    sin.sin_addr.s_addr = i_src_addr;

    if ( bind( i_handle, (struct sockaddr *)&sin, sizeof( sin ) ) < 0 )
    {
        msg_Err( NULL, "couldn't bind\n" );
        close( i_handle );
        exit(EXIT_FAILURE);
    }

    /* Join the multicast group if the socket is a multicast address */
    if ( IN_MULTICAST( ntohl(sin.sin_addr.s_addr)) )
    {
        struct ip_mreq imr;

        imr.imr_multiaddr.s_addr = sin.sin_addr.s_addr;
        imr.imr_interface.s_addr = INADDR_ANY; /* FIXME could be an option */

        /* Join Multicast group without source filter */
        if ( setsockopt( i_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         (char *)&imr, sizeof(struct ip_mreq) ) == -1 )
        {
            msg_Err( NULL, "couldn't join multicast group" );
            exit(EXIT_FAILURE);
        }
    }

    msg_Dbg( NULL, "binding socket to %s:%u in %s mode",
             inet_ntoa( sin.sin_addr ), i_src_port,
             b_src_rawudp ? "UDP" : "RTP" );
}

/*****************************************************************************
 * udp_Read
 *****************************************************************************/
block_t *udp_Read( void )
{
    int i = 0, i_len;
    struct iovec p_iov[NB_BLOCKS + !b_src_rawudp];
    block_t *p_ts, **pp_current = &p_ts;
    uint8_t p_rtp_hdr[RTP_SIZE];

    if ( !b_src_rawudp )
    {
        /* FIXME : this is wrong if RTP header > 12 bytes */
        p_iov[i].iov_base = p_rtp_hdr;
        p_iov[i].iov_len = RTP_SIZE;
        i++;
    }

    for ( ; i < NB_BLOCKS + !b_src_rawudp; i++ )
    {
        *pp_current = block_New();
        p_iov[i].iov_base = (*pp_current)->p_ts;
        p_iov[i].iov_len = TS_SIZE;
        pp_current = &(*pp_current)->p_next;
    }

    if ( (i_len = readv( i_handle, p_iov, NB_BLOCKS + !b_src_rawudp )) < 0 )
    {
        msg_Err( NULL, "couldn't read from network (%s)",
                 strerror(errno) );
        i_len = 0;
    }
    if ( !b_src_rawudp )
        i_len -= RTP_SIZE;
    i_len /= TS_SIZE;

    pp_current = &p_ts;
    while ( i_len && *pp_current )
    {
        pp_current = &(*pp_current)->p_next;
        i_len--;
    }

    if ( *pp_current )
        msg_Dbg( NULL, "partial buffer received" );
    block_DeleteChain( *pp_current );
    *pp_current = NULL;

    return p_ts;
}

/* From now on these are just stubs */

/*****************************************************************************
 * udp_SetFilter
 *****************************************************************************/
int udp_SetFilter( uint16_t i_pid )
{
    return -1;
}

/*****************************************************************************
 * udp_UnsetFilter: normally never called
 *****************************************************************************/
void udp_UnsetFilter( int i_fd, uint16_t i_pid )
{
}
