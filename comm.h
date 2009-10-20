/*****************************************************************************
 * comm.h
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

/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>

#define COMM_BUFFER_SIZE 4096
#define COMM_HEADER_SIZE 4
#define COMM_HEADER_MAGIC 0x48

#define CMD_RELOAD 1
#define CMD_SHUTDOWN 2
#define CMD_FRONTEND_STATUS 3
#define CMD_MMI_STATUS 4
#define CMD_MMI_SLOT_STATUS 5 /* arg: slot */
#define CMD_MMI_OPEN 6 /* arg: slot */
#define CMD_MMI_CLOSE 7 /* arg: slot */
#define CMD_MMI_RECV 8 /* arg: slot */
#define CMD_MMI_SEND 9 /* arg: slot, en50221_mmi_object_t */

#define RET_OK 0
#define RET_ERR 1
#define RET_FRONTEND_STATUS 2
#define RET_MMI_STATUS 3
#define RET_MMI_SLOT_STATUS 4
#define RET_MMI_RECV 5
#define RET_HUH 255

struct ret_frontend_status
{
    struct dvb_frontend_info info;
    fe_status_t i_status;
    int32_t i_ber, i_strength, i_snr;
};

struct ret_mmi_status
{
    ca_caps_t caps;
};

struct ret_mmi_slot_status
{
    ca_slot_info_t sinfo;
};

struct ret_mmi_recv
{
    en50221_mmi_object_t object;
};

struct cmd_mmi_send
{
    uint8_t i_slot;
    en50221_mmi_object_t object;
};
