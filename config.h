/*****************************************************************************
 * config.h
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011, 2020 VideoLAN
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

#ifndef _DVBLAST_CONFIG_H_
#define _DVBLAST_CONFIG_H_

#if defined(__linux__)
#define DVBAPI_VERSION ((DVB_API_VERSION)*100+(DVB_API_VERSION_MINOR))
#define HAVE_DVB_SUPPORT
#define HAVE_ASI_SUPPORT
#define HAVE_CLOCK_NANOSLEEP
#endif

#define HAVE_ICONV

#define DEFAULT_PORT 3001
#define TS_SIZE 188
#define MAX_PIDS 8192
#define DEFAULT_IPV4_MTU 1500
#define DEFAULT_IPV6_MTU 1280
#define PADDING_PID 8191
#define WATCHDOG_WAIT 10000000LL
#define WATCHDOG_REFRACTORY_PERIOD 60000000LL
#define MAX_ERRORS 1000
#define DEFAULT_VERBOSITY 4
#define MAX_POLL_TIMEOUT 100000 /* 100 ms */
#define MIN_POLL_TIMEOUT 100 /* 100 us */
#define DEFAULT_OUTPUT_LATENCY 200000 /* 200 ms */
#define DEFAULT_MAX_RETENTION 40000 /* 40 ms */
#define MAX_EIT_RETENTION 500000 /* 500 ms */
#define DEFAULT_FRONTEND_TIMEOUT 30000000 /* 30 s */
#define EXIT_STATUS_FRONTEND_TIMEOUT 100
#define DEFAULT_UDP_LOCK_TIMEOUT 5000000 /* 5 s */

// Compatability defines
#if defined(__APPLE__)
#define ip_mreqn ip_mreq
#endif

#ifndef IPV6_ADD_MEMBERSHIP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif

#endif
