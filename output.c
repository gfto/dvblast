/*****************************************************************************
 * output.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2009 VideoLAN
 * $Id$
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
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>

#include "dvblast.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int net_Open( output_t *p_output );
static void rtp_SetHdr( output_t *p_output, uint8_t *p_hdr );

/*****************************************************************************
 * output_Create : called from main thread
 *****************************************************************************/
output_t *output_Create( uint8_t i_config, const char *psz_displayname, void *p_init_data )
{
    int i;
    output_t *p_output = NULL;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( !( pp_outputs[i]->i_config & OUTPUT_VALID ) )
        {
            p_output = pp_outputs[i];
            break;
        }
    }

    if ( p_output == NULL )
    {
        p_output = malloc( sizeof(output_t) );
        memset( p_output, 0, sizeof(output_t) );
        i_nb_outputs++;
        pp_outputs = realloc( pp_outputs, i_nb_outputs * sizeof(output_t *) );
        pp_outputs[i] = p_output;
    }

    if ( output_Init( p_output, i_config, psz_displayname, p_init_data ) < 0 )
        return NULL;

    return p_output;
}

/*****************************************************************************
 * output_Init
 *****************************************************************************/
int output_Init( output_t *p_output, uint8_t i_config, const char *psz_displayname, void *p_init_data )
{
    p_output->i_sid = 0;
    p_output->i_depth = 0;
    p_output->pi_pids = NULL;
    p_output->i_nb_pids = 0;

    p_output->i_nb_errors = 0;
    p_output->i_cc = rand() & 0xffff;
    p_output->i_pat_cc = rand() & 0xf;
    p_output->i_pmt_cc = rand() & 0xf;
    p_output->i_sdt_cc = rand() & 0xf;
    p_output->i_eit_cc = rand() & 0xf;
    p_output->i_pat_version = rand() & 0xff;
    p_output->i_pmt_version = rand() & 0xff;
    p_output->i_sdt_version = rand() & 0xff;
    p_output->p_pat_section = NULL;
    p_output->p_pmt_section = NULL;
    p_output->p_sdt_section = NULL;
    if ( b_unique_tsid )
        p_output->i_ts_id = rand() & 0xffff;
    p_output->i_ref_timestamp = 0;
    p_output->i_ref_wallclock = 0;

    p_output->i_config = i_config;
    p_output->psz_displayname = strdup( psz_displayname );

    struct addrinfo *p_ai = (struct addrinfo *)p_init_data;
    p_output->i_addrlen = p_ai->ai_addrlen;
    p_output->p_addr = malloc( p_output->i_addrlen );
    memcpy( p_output->p_addr, p_ai->ai_addr,
            p_output->i_addrlen );

    if ( ( p_output->i_handle = net_Open( p_output ) ) < 0 )
    {
        p_output->i_config &= ~OUTPUT_VALID;
        return -1;
    }

    p_output->i_config |= OUTPUT_VALID;

    return 0;
}

/*****************************************************************************
 * output_Close
 *****************************************************************************/
void output_Close( output_t *p_output )
{
    int i;

    for ( i = 0; i < p_output->i_depth; i++ )
    {
        p_output->pp_blocks[i]->i_refcount--;
        if ( !p_output->pp_blocks[i]->i_refcount )
            block_Delete( p_output->pp_blocks[i] );
    }

    free( p_output->psz_displayname );
    p_output->i_depth = 0;
    p_output->i_config &= ~OUTPUT_VALID;
    close( p_output->i_handle );
}

/*****************************************************************************
 * output_Flush
 *****************************************************************************/
static void output_Flush( output_t *p_output )
{
    struct iovec p_iov[NB_BLOCKS + 1];
    uint8_t p_rtp_hdr[RTP_SIZE];
    int i;

    int i_block_cnt = ( p_output->p_addr->ss_family == AF_INET6 ) ? NB_BLOCKS_IPV6 : NB_BLOCKS;
    int i_outblocks = i_block_cnt;

    if ( !b_output_udp && !(p_output->i_config & OUTPUT_UDP) )
    {
        p_iov[0].iov_base = p_rtp_hdr;
        p_iov[0].iov_len = sizeof(p_rtp_hdr);
        rtp_SetHdr( p_output, p_rtp_hdr );

        for ( i = 1; i < i_block_cnt + 1; i++ )
        {
            p_iov[i].iov_base = p_output->pp_blocks[i - 1]->p_ts;
            p_iov[i].iov_len = TS_SIZE;
        }

        i_outblocks += 1;
    }
    else
    {
        for ( i = 0; i < i_block_cnt; i++ )
        {
            p_iov[i].iov_base = p_output->pp_blocks[i]->p_ts;
            p_iov[i].iov_len = TS_SIZE;
        }
    }

    if ( writev( p_output->i_handle, p_iov, i_outblocks ) < 0 )
    {
        msg_Err( NULL, "couldn't writev to %s (%s)",
                 p_output->psz_displayname, strerror(errno) );
    }

    for ( i = 0; i < i_block_cnt; i++ )
    {
        p_output->pp_blocks[i]->i_refcount--;
        if ( !p_output->pp_blocks[i]->i_refcount )
            block_Delete( p_output->pp_blocks[i] );
    }
    p_output->i_depth = 0;
}

/*****************************************************************************
 * output_Put : called from demux
 *****************************************************************************/
void output_Put( output_t *p_output, block_t *p_block )
{
    p_block->i_refcount++;

    p_output->pp_blocks[p_output->i_depth] = p_block;
    p_output->i_depth++;

    if ( ( ( p_output->p_addr->ss_family == AF_INET6 ) &&
           ( p_output->i_depth >= NB_BLOCKS_IPV6 ) )   ||
         ( ( p_output->p_addr->ss_family == AF_INET )  &&
           ( p_output->i_depth >= NB_BLOCKS ) ) )
        output_Flush( p_output );
}

/*****************************************************************************
 * net_Open
 *****************************************************************************/
static int net_Open( output_t *p_output )
{
    int i_handle = socket( p_output->p_addr->ss_family, SOCK_DGRAM, IPPROTO_UDP );

    if ( i_handle < 0 )
    {
        msg_Err( NULL, "couldn't create socket for %s (%s)",
                 p_output->psz_displayname, strerror(errno) );
        return -errno;
    }

    if ( p_output->p_addr->ss_family == AF_INET6 )
    {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)p_output->p_addr;
        if ( IN6_IS_ADDR_MULTICAST( addr->sin6_addr.s6_addr ) )
        {
            int i = i_ttl;
            setsockopt( i_handle, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                        (void *)&i, sizeof(i) );
        }
    }
    else if ( p_output->p_addr->ss_family == AF_INET )
    {
        struct sockaddr_in *addr = (struct sockaddr_in *)p_output->p_addr;
        if ( IN_MULTICAST( ntohl( addr->sin_addr.s_addr ) ) )
        {
            int i = i_ttl;
            setsockopt( i_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                        (void *)&i, sizeof(i) );
        }
    }

    if ( connect( i_handle, (struct sockaddr *)p_output->p_addr, p_output->i_addrlen ) < 0 )
    {
        msg_Err( NULL, "couldn't connect socket to %s (%s)",
                 p_output->psz_displayname, strerror(errno) );
        close( i_handle );
        return -errno;
    }

    return i_handle;
}

/*****************************************************************************
 * rtp_SetHdr
 *****************************************************************************/
/*
 * Reminder : RTP header
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                           timestamp                           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           synchronization source (SSRC) identifier            |
   +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
   |            contributing source (CSRC) identifiers             |
   |                             ....                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

static void rtp_SetHdr( output_t *p_output, uint8_t *p_hdr )
{
    mtime_t i_timestamp;

    if (!p_output->i_ref_wallclock)
    {
        i_timestamp = p_output->i_ref_timestamp;
        p_output->i_ref_wallclock = mdate();
    }
    else
    {
        i_timestamp = p_output->i_ref_timestamp
                    + (mdate() - p_output->i_ref_wallclock) * 9 / 100;
    }

    p_hdr[0] = 0x80;
    p_hdr[1] = 33;
    p_hdr[2] = p_output->i_cc >> 8;
    p_hdr[3] = p_output->i_cc & 0xff;
    p_hdr[4] = (i_timestamp >> 24) & 0xff;
    p_hdr[5] = (i_timestamp >> 16) & 0xff;
    p_hdr[6] = (i_timestamp >> 8) & 0xff;
    p_hdr[7] = i_timestamp & 0xff;
    p_hdr[8] = ((uint8_t *)&i_ssrc)[0];
    p_hdr[9] = ((uint8_t *)&i_ssrc)[1];
    p_hdr[10] = ((uint8_t *)&i_ssrc)[2];
    p_hdr[11] = ((uint8_t *)&i_ssrc)[3];
    p_output->i_cc++;
}

