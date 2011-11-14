/*****************************************************************************
 * mrtg-cnt.c Handle dvb TS packets and count them for MRTG
 *****************************************************************************
 * Copyright Tripleplay service 2004,2005,2011
 *
 * Author:  Andy Lindsay <a.lindsay@tripleplay-services.com>
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

/*  vim: set shiftwidth=4 tabstop=4 expandtab autoindent : */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

#include "dvblast.h"

// File handle
static FILE *mrtg_fh = NULL;

// Counts
static long long l_mrtg_packets = 0;            // Packets received
static long long l_mrtg_seq_err_packets = 0;    // Out of sequence packets received
static long long l_mrtg_error_packets = 0;      // Packets received with the error flag set
static long long l_mrtg_scram_packets = 0;      // Scrambled Packets received

// Reporting timer
#if defined( WIN32 )
static LARGE_INTEGER mrtg_time;
static LARGE_INTEGER mrtg_inc;
#else
static struct timeval mrtg_time = { 0, 0 };
#endif

// Define the dump period in seconds
#define MRTG_INTERVAL   10

// Pid sequence numbers
#define PIDS    0x2000
static signed char i_pid_seq[PIDS];

// Report the mrtg counters: bytes received, error packets & sequence errors
static void dumpCounts()
{
    unsigned int multiplier = 1;        //MRTG_INTERVAL;
    if(mrtg_fh) {
        rewind(mrtg_fh);
        fprintf(mrtg_fh, "%lld %lld %lld %lld\n",
                l_mrtg_packets * 188 * multiplier,
                l_mrtg_error_packets * multiplier,
                l_mrtg_seq_err_packets * multiplier,
                l_mrtg_scram_packets * multiplier);
        fflush(mrtg_fh);
    }
}

// analyse the input block counting packets and errors
// The input is a pointer to a block_t structure, which might be a linked list
// of blocks. Each block has one TS packet.
void mrtgAnalyse(block_t * p_ts)
{
    unsigned int i_pid;
    block_t *p_block = p_ts;

    if (mrtg_fh == NULL) return;

    while (p_block != NULL) {
        uint8_t *ts_packet = p_block->p_ts;

        char i_seq, i_last_seq;
        l_mrtg_packets++;

        if (ts_packet[0] != 0x47) {
            l_mrtg_error_packets++;
            p_block = p_block->p_next;
            continue;
        }

        if (ts_packet[1] & 0x80) {
            l_mrtg_error_packets++;
            p_block = p_block->p_next;
            continue;
        }

        i_pid = (ts_packet[1] & 0x1f) << 8 | ts_packet[2];

        // Just count null packets - don't check the sequence numbering
        if (i_pid == 0x1fff) {
            p_block = p_block->p_next;
            continue;
        }

        if (ts_packet[3] & 0xc0) {
            l_mrtg_scram_packets++;
        }
        // Check the sequence numbering
        i_seq = ts_packet[3] & 0xf;
        i_last_seq = i_pid_seq[i_pid];

        if (i_last_seq == -1) {
            // First packet - ignore the sequence
        } else if (ts_packet[3] & 0x10) {
            // Packet contains payload - sequence should be up by one
            if (i_seq != ((i_last_seq + 1) & 0x0f)) {
                l_mrtg_seq_err_packets++;
            }
        } else {
            // Packet contains no payload - sequence should be unchanged
            if (i_seq != i_last_seq) {
                l_mrtg_seq_err_packets++;
            }
        }
        i_pid_seq[i_pid] = i_seq;

        // Look at next block
        p_block = p_block->p_next;
    }

    // All blocks processed. See if we need to dump the stats
    struct timeval now;
    gettimeofday(&now, NULL);
    if (timercmp(&now, &mrtg_time, >)) {
        // Time to report the mrtg counters
        dumpCounts();

        // Set the timer for next time
        //
        // Normally we add the interval to the previous time so that if one
        // dump is a bit late, the next one still occurs at the correct time.
        // However, if there is a long gap (e.g. because the channel has
        // stopped for some time), then just rebase the timing to the current
        // time.  I've chosen MRTG_INTERVAL as the long gap - this is arbitary
        if ((now.tv_sec - mrtg_time.tv_sec) > MRTG_INTERVAL) {
            msg_Dbg(NULL, "Dump is %d seconds late - reset timing\n",
                    (int) (now.tv_sec - mrtg_time.tv_sec));
            mrtg_time = now;
        }
        mrtg_time.tv_sec += MRTG_INTERVAL;
    }
}

int mrtgInit(char *mrtg_file)
{
    if ( !mrtg_file )
        return -1;

    /* Open MRTG file */
    msg_Dbg(NULL, "Opening mrtg file %s.\n", mrtg_file);
    if ((mrtg_fh = fopen(mrtg_file, "wb")) == NULL) {
        msg_Err(NULL, "unable to open mrtg file");
        return -1;
    }
    // Initialise the file
    fprintf(mrtg_fh, "0 0 0 0\n");
    fflush(mrtg_fh);

    // Initialise the sequence numbering
    memset(&i_pid_seq[0], -1, sizeof(signed char) * PIDS);

    // Set the reporting timer
    gettimeofday(&mrtg_time, NULL);
    mrtg_time.tv_sec += MRTG_INTERVAL;

    return 0;
}

void mrtgClose()
{
    // This is only for testing when using filetest.
    if (mrtg_fh) {
        dumpCounts();
        fclose(mrtg_fh);
        mrtg_fh = NULL;
    }
}
