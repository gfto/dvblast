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
#include <stdbool.h>
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

#include <bitstream/ietf/rtp.h>

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
static int i_handle;
static bool b_udp = false;
static int i_block_cnt;
static uint8_t pi_ssrc[4] = { 0, 0, 0, 0 };
static uint16_t i_cc = 0;

/*****************************************************************************
 * udp_Open
 *****************************************************************************/
void udp_Open( void )
{
    int i_family;
    struct addrinfo *p_connect_ai = NULL, *p_bind_ai;
    int i_if_index = 0;
    in_addr_t i_if_addr = INADDR_ANY;
    int i_mtu = 0;

    char *psz_bind, *psz_string = strdup( psz_udp_src );
    char *psz_save = psz_string;
    int i = 1;

    /* Parse configuration. */

    if ( (psz_bind = strchr( psz_string, '@' )) != NULL )
    {
        *psz_bind++ = '\0';
        p_connect_ai = ParseNodeService( psz_string, NULL, 0 );
    }
    else
        psz_bind = psz_string;

    p_bind_ai = ParseNodeService( psz_bind, &psz_string, DEFAULT_PORT );
    if ( p_bind_ai == NULL )
    {
        msg_Err( NULL, "couldn't parse %s", psz_bind );
        exit(EXIT_FAILURE);
    }
    i_family = p_bind_ai->ai_family;

    if ( p_connect_ai != NULL && p_connect_ai->ai_family != i_family )
    {
        msg_Warn( NULL, "invalid connect address" );
        freeaddrinfo( p_connect_ai );
        p_connect_ai = NULL;
    }

    while ( (psz_string = strchr( psz_string, '/' )) != NULL )
    {
        *psz_string++ = '\0';

#define IS_OPTION( option ) (!strncasecmp( psz_string, option, strlen(option) ))
#define ARG_OPTION( option ) (psz_string + strlen(option))

        if ( IS_OPTION("udp") )
            b_udp = true;
        else if ( IS_OPTION("mtu=") )
            i_mtu = strtol( ARG_OPTION("mtu="), NULL, 0 );
        else if ( IS_OPTION("ifindex=") )
            i_if_index = strtol( ARG_OPTION("ifindex="), NULL, 0 );
        else if ( IS_OPTION("ifaddr=") )
            i_if_addr = inet_addr( ARG_OPTION("ifaddr=") );
        else
            msg_Warn( NULL, "unrecognized option %s", psz_string );

#undef IS_OPTION
#undef ARG_OPTION
    }

    if ( !i_mtu )
        i_mtu = i_family == AF_INET6 ? DEFAULT_IPV6_MTU : DEFAULT_IPV4_MTU;
    i_block_cnt = (i_mtu - (b_udp ? 0 : RTP_HEADER_SIZE)) / TS_SIZE;


    /* Do stuff. */

    if ( (i_handle = socket( i_family, SOCK_DGRAM, IPPROTO_UDP )) < 0 )
    {
        msg_Err( NULL, "couldn't create socket (%s)", strerror(errno) );
        exit(EXIT_FAILURE);
    }

    setsockopt( i_handle, SOL_SOCKET, SO_REUSEADDR, (void *) &i, sizeof( i ) );

    /* Increase the receive buffer size to 1/2MB (8Mb/s during 1/2s) to avoid
     * packet loss caused by scheduling problems */
    i = 0x80000;

    setsockopt( i_handle, SOL_SOCKET, SO_RCVBUF, (void *) &i, sizeof( i ) );

    if ( bind( i_handle, p_bind_ai->ai_addr, p_bind_ai->ai_addrlen ) < 0 )
    {
        msg_Err( NULL, "couldn't bind (%s)", strerror(errno) );
        close( i_handle );
        exit(EXIT_FAILURE);
    }

    if ( p_connect_ai != NULL )
    {
        uint16_t i_port;
        if ( i_family == AF_INET6 )
            i_port = ((struct sockaddr_in6 *)p_connect_ai->ai_addr)->sin6_port;
        else
            i_port = ((struct sockaddr_in *)p_connect_ai->ai_addr)->sin_port;

        if ( i_port != 0 && connect( i_handle, p_connect_ai->ai_addr,
                                     p_connect_ai->ai_addrlen ) < 0 )
            msg_Warn( NULL, "couldn't connect socket (%s)", strerror(errno) );
    }

    /* Join the multicast group if the socket is a multicast address */
    if ( i_family == AF_INET6 )
    {
        struct sockaddr_in6 *p_addr =
            (struct sockaddr_in6 *)p_bind_ai->ai_addr;
        if ( IN6_IS_ADDR_MULTICAST( p_addr->sin6_addr.s6_addr ) )
        {
            struct ipv6_mreq imr;
            imr.ipv6mr_multiaddr = p_addr->sin6_addr;
            imr.ipv6mr_interface = i_if_index;
            if ( i_if_addr != INADDR_ANY )
                msg_Warn( NULL, "ignoring ifaddr option in IPv6" );

            if ( setsockopt( i_handle, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                             (char *)&imr, sizeof(struct ipv6_mreq) ) < 0 )
                msg_Warn( NULL, "couldn't join multicast group (%s)",
                          strerror(errno) );
        }
    }
    else
    {
        struct sockaddr_in *p_addr =
            (struct sockaddr_in *)p_bind_ai->ai_addr;
        if ( IN_MULTICAST( ntohl(p_addr->sin_addr.s_addr)) )
        {
            if ( p_connect_ai != NULL )
            {
                /* Source-specific multicast */
                struct sockaddr_in *p_src =
                    (struct sockaddr_in *)&p_connect_ai->ai_addr;
                struct ip_mreq_source imr;
                imr.imr_multiaddr = p_addr->sin_addr;
                imr.imr_interface.s_addr = i_if_addr;
                imr.imr_sourceaddr = p_src->sin_addr;
                if ( i_if_index )
                    msg_Warn( NULL, "ignoring ifindex option in SSM" );

                if ( setsockopt( i_handle, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP,
                            (char *)&imr, sizeof(struct ip_mreq_source) ) < 0 )
                    msg_Warn( NULL, "couldn't join multicast group (%s)",
                              strerror(errno) );
            }
            else if ( i_if_index )
            {
                /* Linux-specific interface-bound multicast */
                struct ip_mreqn imr;
                imr.imr_multiaddr = p_addr->sin_addr;
                imr.imr_address.s_addr = i_if_addr;
                imr.imr_ifindex = i_if_index;

                if ( setsockopt( i_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                 (char *)&imr, sizeof(struct ip_mreqn) ) < 0 )
                    msg_Warn( NULL, "couldn't join multicast group (%s)",
                              strerror(errno) );
            }
            else
            {
                /* Regular multicast */
                struct ip_mreq imr;
                imr.imr_multiaddr = p_addr->sin_addr;
                imr.imr_interface.s_addr = i_if_addr;

                if ( setsockopt( i_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                 (char *)&imr, sizeof(struct ip_mreq) ) == -1 )
                    msg_Warn( NULL, "couldn't join multicast group (%s)",
                              strerror(errno) );
            }
        }
    }

    freeaddrinfo( p_bind_ai );
    if ( p_connect_ai != NULL )
        freeaddrinfo( p_connect_ai );
    free( psz_save );

    msg_Dbg( NULL, "binding socket to %s", psz_udp_src );
}

/*****************************************************************************
 * udp_Read
 *****************************************************************************/
block_t *udp_Read( mtime_t i_poll_timeout )
{
    struct pollfd pfd;
    int i_ret;

    pfd.fd = i_handle;
    pfd.events = POLLIN;

    i_ret = poll( &pfd, 1, (i_poll_timeout + 999) / 1000 );

    i_wallclock = mdate();

    if ( i_ret < 0 )
    {
        if( errno != EINTR )
            msg_Err( NULL, "couldn't poll from socket (%s)",
                     strerror(errno) );
        return NULL;
    }

    if ( pfd.revents )
    {
        struct iovec p_iov[i_block_cnt + 1];
        block_t *p_ts, **pp_current = &p_ts;
        int i_iov, i_block;
        ssize_t i_len;
        uint8_t p_rtp_hdr[RTP_HEADER_SIZE];

        if ( !b_udp )
        {
            /* FIXME : this is wrong if RTP header > 12 bytes */
            p_iov[0].iov_base = p_rtp_hdr;
            p_iov[0].iov_len = RTP_HEADER_SIZE;
            i_iov = 1;
        }
        else
            i_iov = 0;

        for ( i_block = 0; i_block < i_block_cnt; i_block++ )
        {
            *pp_current = block_New();
            p_iov[i_iov].iov_base = (*pp_current)->p_ts;
            p_iov[i_iov].iov_len = TS_SIZE;
            pp_current = &(*pp_current)->p_next;
            i_iov++;
        }
        pp_current = &p_ts;

        if ( (i_len = readv( i_handle, p_iov, i_iov )) < 0 )
        {
            msg_Err( NULL, "couldn't read from network (%s)", strerror(errno) );
            goto err;
        }

int meuh = i_len;

        if ( !b_udp )
        {
            uint8_t pi_new_ssrc[4];

            if ( !rtp_check_hdr(p_rtp_hdr) )
                msg_Warn( NULL, "invalid RTP packet received" );
            if ( rtp_get_type(p_rtp_hdr) != RTP_TYPE_TS )
                msg_Warn( NULL, "non-TS RTP packet received" );
            rtp_get_ssrc(p_rtp_hdr, pi_new_ssrc);
            if ( !memcmp( pi_ssrc, pi_new_ssrc, 4 * sizeof(uint8_t) ) )
            {
                if ( rtp_get_cc(p_rtp_hdr) != i_cc )
                    msg_Warn( NULL, "RTP discontinuity" );
            }
            else
            {
                struct in_addr addr;
                memcpy( &addr.s_addr, pi_new_ssrc, 4 * sizeof(uint8_t) );
                msg_Dbg( NULL, "new RTP source: %s", inet_ntoa( addr ) );
                memcpy( pi_ssrc, pi_new_ssrc, 4 * sizeof(uint8_t) );
            }
            i_cc = rtp_get_cc(p_rtp_hdr) + 1;

            i_len -= RTP_HEADER_SIZE;
        }

        i_len /= TS_SIZE;

        while ( i_len && *pp_current )
        {
            pp_current = &(*pp_current)->p_next;
            i_len--;
        }

        i_wallclock = mdate();

        if ( *pp_current )
            msg_Dbg( NULL, "partial buffer received %d", meuh );
err:
        block_DeleteChain( *pp_current );
        *pp_current = NULL;

        return p_ts;
    }

    return NULL;
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
