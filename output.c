/*****************************************************************************
 * output.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2010 VideoLAN
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
#include <stdbool.h>
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

#include <bitstream/mpeg/ts.h>
#include <bitstream/ietf/rtp.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
struct packet_t
{
    struct packet_t *p_next;
    mtime_t i_dts;
    int i_depth;
    block_t **pp_blocks;
    /* PRIVATE - this MUST be at the end of the structure: */
    block_t *p_blocks; /* actually an array of pointers */
};

static uint8_t p_pad_ts[TS_SIZE] = {
    0x47, 0x1f, 0xff, 0x10, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*****************************************************************************
 * RawFillHeaders - fill ipv4/udp headers for RAW socket
 *****************************************************************************/
static void RawFillHeaders(struct udprawpkt *dgram,
                        in_addr_t ipsrc, in_addr_t ipdst,
                        uint16_t portsrc, uint16_t portdst,
                        uint8_t ttl, uint8_t tos, uint16_t len)
{
    struct iphdr *iph = &(dgram->iph);
    struct udpheader *udph = &(dgram->udph);

#ifdef DEBUG_SOCKET
    char ipsrc_str[16], ipdst_str[16];
    struct in_addr insrc, indst;
    insrc.s_addr = ipsrc;
    indst.s_addr = ipdst;
    strncpy(ipsrc_str, inet_ntoa(insrc), 16);
    strncpy(ipdst_str, inet_ntoa(indst), 16);
    printf("Filling raw header (%p) (%s:%u -> %s:%u)\n", dgram, ipsrc_str, portsrc, ipdst_str, portdst);
#endif

    // Fill ip header
    iph->ihl      = 5;              // ip header with no specific option
    iph->version  = 4;
    iph->tos      = tos;
    iph->tot_len  = sizeof(struct udprawpkt) + len; // auto-htoned ?
    iph->id       = htons(0);       // auto-generated if frag_off (flags) = 0 ?
    iph->frag_off = 0;
    iph->ttl      = ttl;
    iph->protocol = IPPROTO_UDP;
    iph->check    = 0;
    iph->saddr    = ipsrc;
    iph->daddr    = ipdst;

    // Fill udp header
    udph->source = htons(portsrc);
    udph->dest   = htons(portdst);
    udph->len    = htons(sizeof(struct udpheader) + len);
    udph->check  = 0;

    // Compute ip header checksum. Computed by kernel when frag_off = 0 ?
    //iph->check = csum((unsigned short *)iph, sizeof(struct iphdr));
}

/*****************************************************************************
 * output_Create : create and insert the output_t structure
 *****************************************************************************/
output_t *output_Create( const output_config_t *p_config )
{
    int i;
    output_t *p_output = NULL;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( !( pp_outputs[i]->config.i_config & OUTPUT_VALID ) )
        {
            p_output = pp_outputs[i];
            break;
        }
    }

    if ( p_output == NULL )
    {
        p_output = malloc( sizeof(output_t) );
        i_nb_outputs++;
        pp_outputs = realloc( pp_outputs, i_nb_outputs * sizeof(output_t *) );
        pp_outputs[i] = p_output;
    }

    if ( output_Init( p_output, p_config ) < 0 )
        return NULL;

    return p_output;
}

/* Init the mapped pids to unused */
void init_pid_mapping( output_t *p_output )
{
    unsigned int i;
    for ( i = 0; i < MAX_PIDS; i++ ) {
        p_output->pi_newpids[i]  = UNUSED_PID;
        p_output->pi_freepids[i] = UNUSED_PID;
    }
}

/*****************************************************************************
 * output_Init : set up the output initial config
 *****************************************************************************/
int output_Init( output_t *p_output, const output_config_t *p_config )
{
    socklen_t i_sockaddr_len = (p_config->i_family == AF_INET) ?
                               sizeof(struct sockaddr_in) :
                               sizeof(struct sockaddr_in6);

    memset( p_output, 0, sizeof(output_t) );
    config_Init( &p_output->config );

    /* Init run-time values */
    p_output->p_packets = p_output->p_last_packet = NULL;
    p_output->i_seqnum = rand() & 0xffff;
    p_output->i_pat_cc = rand() & 0xf;
    p_output->i_pmt_cc = rand() & 0xf;
    p_output->i_nit_cc = rand() & 0xf;
    p_output->i_sdt_cc = rand() & 0xf;
    p_output->i_eit_cc = rand() & 0xf;
    p_output->i_pat_version = rand() & 0xff;
    p_output->i_pmt_version = rand() & 0xff;
    p_output->i_nit_version = rand() & 0xff;
    p_output->i_sdt_version = rand() & 0xff;
    p_output->p_pat_section = NULL;
    p_output->p_pmt_section = NULL;
    p_output->p_nit_section = NULL;
    p_output->p_sdt_section = NULL;
    p_output->p_eit_ts_buffer = NULL;
    if ( b_random_tsid )
        p_output->i_tsid = rand() & 0xffff;

    /* Init the mapped pids to unused */
    init_pid_mapping( p_output );

    /* Init socket-related fields */
    p_output->config.i_family = p_config->i_family;
    memcpy( &p_output->config.connect_addr, &p_config->connect_addr,
            sizeof(struct sockaddr_storage) );
    memcpy( &p_output->config.bind_addr, &p_config->bind_addr,
            sizeof(struct sockaddr_storage) );
    p_output->config.i_if_index_v6 = p_config->i_if_index_v6;

    if ( (p_config->i_config & OUTPUT_RAW) ) {
        p_output->config.i_config |= OUTPUT_RAW;
        p_output->i_handle = socket( AF_INET, SOCK_RAW, IPPROTO_RAW );
    } else {
        p_output->i_handle = socket( p_config->i_family, SOCK_DGRAM, IPPROTO_UDP );
    }
    if ( p_output->i_handle < 0 )
    {
        msg_Err( NULL, "couldn't create socket (%s)", strerror(errno) );
        p_output->config.i_config &= ~OUTPUT_VALID;
        return -errno;
    }

    int ret = 0;
    if ( p_config->bind_addr.ss_family != AF_UNSPEC )
    {
        if ( bind( p_output->i_handle, (struct sockaddr *)&p_config->bind_addr,
                   i_sockaddr_len ) < 0 )
            msg_Warn( NULL, "couldn't bind socket (%s)", strerror(errno) );

        if ( p_config->i_family == AF_INET )
        {
            struct sockaddr_in *p_connect_addr =
                (struct sockaddr_in *)&p_output->config.connect_addr;
            struct sockaddr_in *p_bind_addr =
                (struct sockaddr_in *)&p_output->config.bind_addr;

            if ( IN_MULTICAST( ntohl( p_connect_addr->sin_addr.s_addr ) ) )
                ret = setsockopt( p_output->i_handle, IPPROTO_IP,
                                  IP_MULTICAST_IF,
                                  (void *)&p_bind_addr->sin_addr.s_addr,
                                  sizeof(p_bind_addr->sin_addr.s_addr) );
        }
    }

    if ( (p_config->i_config & OUTPUT_RAW) )
    {
        struct sockaddr_in *p_connect_addr =
            (struct sockaddr_in *)&p_output->config.connect_addr;
        RawFillHeaders(&p_output->raw_pkt_header, inet_addr(p_config->psz_srcaddr),
                p_connect_addr->sin_addr.s_addr,
                (uint16_t) p_config->i_srcport, ntohs(p_connect_addr->sin_port),
                p_config->i_ttl, p_config->i_tos, 0);
    }

    if ( p_config->i_family == AF_INET6 && p_config->i_if_index_v6 != -1 )
    {
        struct sockaddr_in6 *p_addr =
            (struct sockaddr_in6 *)&p_output->config.connect_addr;
        if ( IN6_IS_ADDR_MULTICAST( &p_addr->sin6_addr ) )
            ret = setsockopt( p_output->i_handle, IPPROTO_IPV6,
                              IPV6_MULTICAST_IF,
                              (void *)&p_config->i_if_index_v6,
                              sizeof(p_config->i_if_index_v6) );
    }

    if (ret == -1)
        msg_Warn( NULL, "couldn't join multicast address (%s)",
                  strerror(errno) );

    if ( connect( p_output->i_handle,
                  (struct sockaddr *)&p_output->config.connect_addr,
                  i_sockaddr_len ) < 0 )
    {
        msg_Err( NULL, "couldn't connect socket (%s)", strerror(errno) );
        close( p_output->i_handle );
        p_output->config.i_config &= ~OUTPUT_VALID;
        return -errno;
    }

    p_output->config.i_config |= OUTPUT_VALID;

    return 0;
}

/*****************************************************************************
 * output_Close
 *****************************************************************************/
void output_Close( output_t *p_output )
{
    packet_t *p_packet = p_output->p_packets;
    while ( p_packet != NULL )
    {
        int i;

        for ( i = 0; i < p_packet->i_depth; i++ )
        {
            p_packet->pp_blocks[i]->i_refcount--;
            if ( !p_packet->pp_blocks[i]->i_refcount )
                block_Delete( p_packet->pp_blocks[i] );
        }
        p_output->p_packets = p_packet->p_next;
        free( p_packet );
        p_packet = p_output->p_packets;
    }

    p_output->p_packets = p_output->p_last_packet = NULL;
    free( p_output->p_pat_section );
    free( p_output->p_pmt_section );
    free( p_output->p_nit_section );
    free( p_output->p_sdt_section );
    free( p_output->p_eit_ts_buffer );
    p_output->config.i_config &= ~OUTPUT_VALID;

    close( p_output->i_handle );

    config_Free( &p_output->config );
}

/*****************************************************************************
 * output_BlockCount
 *****************************************************************************/
static int output_BlockCount( output_t *p_output )
{
    int i_mtu = p_output->config.i_mtu;
    if ( !(p_output->config.i_config & OUTPUT_UDP) )
        i_mtu -= RTP_HEADER_SIZE;
    return i_mtu / TS_SIZE;
}

/*****************************************************************************
 * output_Flush
 *****************************************************************************/
static void output_Flush( output_t *p_output )
{
    packet_t *p_packet = p_output->p_packets;
    int i_block_cnt = output_BlockCount( p_output );
    struct iovec p_iov[i_block_cnt + 2];
    uint8_t p_rtp_hdr[RTP_HEADER_SIZE];
    int i_iov = 0, i_payload_len, i_block;

    if ( (p_output->config.i_config & OUTPUT_RAW) )
    {
        p_iov[i_iov].iov_base = &p_output->raw_pkt_header;
        p_iov[i_iov].iov_len = sizeof(struct udprawpkt);
        i_iov++;
    }

    if ( !(p_output->config.i_config & OUTPUT_UDP) )
    {
        p_iov[i_iov].iov_base = p_rtp_hdr;
        p_iov[i_iov].iov_len = sizeof(p_rtp_hdr);

        rtp_set_hdr( p_rtp_hdr );
        rtp_set_type( p_rtp_hdr, RTP_TYPE_TS );
        rtp_set_seqnum( p_rtp_hdr, p_output->i_seqnum++ );
        rtp_set_timestamp( p_rtp_hdr,
                           p_output->i_ref_timestamp
                            + (p_packet->i_dts - p_output->i_ref_wallclock)
                               * 9 / 100 );
        rtp_set_ssrc( p_rtp_hdr, p_output->config.pi_ssrc );

        i_iov++;
    }

    for ( i_block = 0; i_block < p_packet->i_depth; i_block++ )
    {
        /* Do pid mapping here if needed.
         * save the original pid in the block.
         * set the pid to the new pid
         * later we re-instate the old pid for the next output
         */
        if ( b_do_remap || p_output->config.b_do_remap ) {
            block_t *p_block = p_packet->pp_blocks[i_block];
            uint16_t i_pid = ts_get_pid( p_block->p_ts );
            p_block->tmp_pid = UNUSED_PID;
            if ( p_output->pi_newpids[i_pid] != UNUSED_PID )
            {
                uint16_t i_newpid = p_output->pi_newpids[i_pid];
                /* Need to map this pid to the new pid */
                ts_set_pid( p_block->p_ts, i_newpid );
                p_block->tmp_pid = i_pid;
            }
        }

        p_iov[i_iov].iov_base = p_packet->pp_blocks[i_block]->p_ts;
        p_iov[i_iov].iov_len = TS_SIZE;
        i_iov++;
    }

    for ( ; i_block < i_block_cnt; i_block++ )
    {
        p_iov[i_iov].iov_base = p_pad_ts;
        p_iov[i_iov].iov_len = TS_SIZE;
        i_iov++;
    }

    
    if ( (p_output->config.i_config & OUTPUT_RAW) )
    {
        i_payload_len = 0;
        for ( i_block = 1; i_block < i_iov; i_block++ ) {
            i_payload_len += p_iov[i_block].iov_len; 
        }
        p_output->raw_pkt_header.udph.len = htons(sizeof(struct udpheader) + i_payload_len);
    }

    if ( writev( p_output->i_handle, p_iov, i_iov ) < 0 )
    {
        msg_Err( NULL, "couldn't writev to %s (%s)",
                 p_output->config.psz_displayname, strerror(errno) );
    }
    /* Update the wallclock because writev() can take some time. */
    i_wallclock = mdate();

    for ( i_block = 0; i_block < p_packet->i_depth; i_block++ )
    {
        p_packet->pp_blocks[i_block]->i_refcount--;
        if ( !p_packet->pp_blocks[i_block]->i_refcount )
            block_Delete( p_packet->pp_blocks[i_block] );
        else if ( b_do_remap || p_output->config.b_do_remap ) {
            /* still referenced so re-instate the orignial pid if remapped */
            block_t * p_block = p_packet->pp_blocks[i_block];
            if (p_block->tmp_pid != UNUSED_PID)
                ts_set_pid( p_block->p_ts, p_block->tmp_pid );
        }
    }
    p_output->p_packets = p_packet->p_next;
    free( p_packet );
    if ( p_output->p_packets == NULL )
        p_output->p_last_packet = NULL;
}

/*****************************************************************************
 * output_Put : called from demux
 *****************************************************************************/
void output_Put( output_t *p_output, block_t *p_block )
{
    int i_block_cnt = output_BlockCount( p_output );
    packet_t *p_packet;

    p_block->i_refcount++;

    if ( p_output->p_last_packet != NULL
          && p_output->p_last_packet->i_depth < i_block_cnt
          && p_output->p_last_packet->i_dts + p_output->config.i_max_retention
              > p_block->i_dts )
    {
        p_packet = p_output->p_last_packet;
        if ( ts_has_adaptation( p_block->p_ts )
              && ts_get_adaptation( p_block->p_ts )
              && tsaf_has_pcr( p_block->p_ts ) )
            p_packet->i_dts = p_block->i_dts;
    }
    else
    {
        /* This isn't the cleanest allocation of the world. */
        p_packet = malloc( sizeof(packet_t)
                            + (i_block_cnt - 1) * sizeof(block_t *) );
        p_packet->pp_blocks = &p_packet->p_blocks;
        p_packet->i_depth = 0;
        p_packet->p_next = NULL;
        p_packet->i_dts = p_block->i_dts;
        if ( p_output->p_last_packet != NULL )
            p_output->p_last_packet->p_next = p_packet;
        else
            p_output->p_packets = p_packet;
        p_output->p_last_packet = p_packet;
    }

    p_packet->pp_blocks[p_packet->i_depth] = p_block;
    p_packet->i_depth++;
}

/*****************************************************************************
 * output_Send : called from main to flush the queues when needed
 *****************************************************************************/
mtime_t output_Send( void )
{
    mtime_t i_earliest_dts = -1;
    int i;

    if ( output_dup.config.i_config & OUTPUT_VALID )
    {
        while ( output_dup.p_packets != NULL
                 && output_dup.p_packets->i_dts
                     + output_dup.config.i_output_latency <= i_wallclock )
            output_Flush( &output_dup );

        if ( output_dup.p_packets != NULL )
            i_earliest_dts = output_dup.p_packets->i_dts;
    }

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];
        if ( !( p_output->config.i_config & OUTPUT_VALID ) )
            continue;

        while ( p_output->p_packets != NULL
                 && p_output->p_packets->i_dts
                     + p_output->config.i_output_latency <= i_wallclock )
            output_Flush( p_output );

        if ( p_output->p_packets != NULL
              && (p_output->p_packets->i_dts
                   + p_output->config.i_output_latency < i_earliest_dts
                   || i_earliest_dts == -1) )
            i_earliest_dts = p_output->p_packets->i_dts
                              + p_output->config.i_output_latency;
    }

    return i_earliest_dts == -1 ? -1 : i_earliest_dts - i_wallclock;
}

/*****************************************************************************
 * output_Find : find an existing output from a given output_config_t
 *****************************************************************************/
output_t *output_Find( const output_config_t *p_config )
{
    socklen_t i_sockaddr_len = (p_config->i_family == AF_INET) ?
                               sizeof(struct sockaddr_in) :
                               sizeof(struct sockaddr_in6);
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( !(p_output->config.i_config & OUTPUT_VALID) ) continue;

        if ( p_config->i_family != p_output->config.i_family ||
             memcmp( &p_config->connect_addr, &p_output->config.connect_addr,
                     i_sockaddr_len ) ||
             memcmp( &p_config->bind_addr, &p_output->config.bind_addr,
                     i_sockaddr_len ) )
            continue;

        if ( p_config->i_family == AF_INET6 &&
             p_config->i_if_index_v6 != p_output->config.i_if_index_v6 )
            continue;

        if ( (p_config->i_config ^ p_output->config.i_config) & OUTPUT_RAW ) {
            continue;
        }

        return p_output;
    }

    return NULL;
}

/*****************************************************************************
 * output_Change : get changes from a new output_config_t
 *****************************************************************************/
void output_Change( output_t *p_output, const output_config_t *p_config )
{
    int ret = 0;
    memcpy( p_output->config.pi_ssrc, p_config->pi_ssrc, 4 * sizeof(uint8_t) );
    p_output->config.i_output_latency = p_config->i_output_latency;
    p_output->config.i_max_retention = p_config->i_max_retention;

    if ( p_output->config.i_ttl != p_config->i_ttl )
    {
        if ( p_output->config.i_family == AF_INET6 )
        {
            struct sockaddr_in6 *p_addr =
                (struct sockaddr_in6 *)&p_output->config.connect_addr;
            if ( IN6_IS_ADDR_MULTICAST( &p_addr->sin6_addr ) )
                ret = setsockopt( p_output->i_handle, IPPROTO_IPV6,
                                  IPV6_MULTICAST_HOPS, (void *)&p_config->i_ttl,
                                  sizeof(p_config->i_ttl) );
        }
        else
        {
            struct sockaddr_in *p_addr =
                (struct sockaddr_in *)&p_output->config.connect_addr;
            if ( IN_MULTICAST( ntohl( p_addr->sin_addr.s_addr ) ) )
                ret = setsockopt( p_output->i_handle, IPPROTO_IP,
                                  IP_MULTICAST_TTL, (void *)&p_config->i_ttl,
                                  sizeof(p_config->i_ttl) );
        }
        p_output->config.i_ttl = p_config->i_ttl;
        p_output->raw_pkt_header.iph.ttl = p_config->i_ttl;
    }

    if ( p_output->config.i_tos != p_config->i_tos )
    {
        if ( p_output->config.i_family == AF_INET )
            ret = setsockopt( p_output->i_handle, IPPROTO_IP, IP_TOS,
                              (void *)&p_config->i_tos,
                              sizeof(p_config->i_tos) );
        p_output->config.i_tos = p_config->i_tos;
        p_output->raw_pkt_header.iph.tos = p_config->i_tos;
    }

    if (ret == -1)
        msg_Warn( NULL, "couldn't change socket (%s)", strerror(errno) );

    if ( p_output->config.i_mtu != p_config->i_mtu
          || ((p_output->config.i_config ^ p_config->i_config) & OUTPUT_UDP) )
    {
        int i_block_cnt;
        packet_t *p_packet = p_output->p_last_packet;
        p_output->config.i_config &= ~OUTPUT_UDP;
        p_output->config.i_config |= p_config->i_config & OUTPUT_UDP;
        p_output->config.i_mtu = p_config->i_mtu;

        i_block_cnt = output_BlockCount( p_output );
        if ( p_packet != NULL && p_packet->i_depth < i_block_cnt )
        {
            p_packet = realloc( p_packet, sizeof(packet_t *)
                                 + (i_block_cnt - 1) * sizeof(block_t *) );
            p_packet->pp_blocks = &p_packet->p_blocks;
            p_output->p_last_packet = p_packet;
        }
    }

    if ( p_config->i_config & OUTPUT_RAW ) {
        p_output->raw_pkt_header.iph.saddr = inet_addr(p_config->psz_srcaddr);
        p_output->raw_pkt_header.udph.source = htons(p_config->i_srcport);
    }
}

/*****************************************************************************
 * outputs_Close : Close all outputs and free allocated memory
 *****************************************************************************/
void outputs_Close( int i_num_outputs )
{
    int i;

    for ( i = 0; i < i_num_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( p_output->config.i_config & OUTPUT_VALID )
        {
            msg_Dbg( NULL, "removing %s", p_output->config.psz_displayname );

            if ( p_output->p_packets )
                output_Flush( p_output );
            output_Close( p_output );
        }

        free( p_output );
    }

    free( pp_outputs );
}
