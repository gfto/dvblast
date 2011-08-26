/*****************************************************************************
 * comm.h
 *****************************************************************************
 * Copyright (C) 2008 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *****************************************************************************/

/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>

#include <bitstream/mpeg/psi.h>

#define COMM_HEADER_SIZE 8
#define COMM_BUFFER_SIZE (COMM_HEADER_SIZE + ((PSI_PRIVATE_MAX_SIZE + PSI_HEADER_SIZE) * (PSI_TABLE_MAX_SECTIONS / 2)))
#define COMM_HEADER_MAGIC 0x48

#define COMM_MAX_MSG_CHUNK 65535

#define CMD_RELOAD 1
#define CMD_SHUTDOWN 2
#define CMD_FRONTEND_STATUS 3
#define CMD_MMI_STATUS 4
#define CMD_MMI_SLOT_STATUS 5 /* arg: slot */
#define CMD_MMI_OPEN 6 /* arg: slot */
#define CMD_MMI_CLOSE 7 /* arg: slot */
#define CMD_MMI_RECV 8 /* arg: slot */
#define CMD_MMI_SEND 9 /* arg: slot, en50221_mmi_object_t */
#define CMD_GET_PAT 10
#define CMD_GET_CAT 11
#define CMD_GET_NIT 12
#define CMD_GET_SDT 13
#define CMD_GET_PMT 14 /* arg: service_id (uint16_t) */
#define CMD_GET_PIDS 15
#define CMD_GET_PID 16 /* arg: pid (uint16_t) */

#define RET_OK 0
#define RET_ERR 1
#define RET_FRONTEND_STATUS 2
#define RET_MMI_STATUS 3
#define RET_MMI_SLOT_STATUS 4
#define RET_MMI_RECV 5
#define RET_MMI_WAIT 6
#define RET_NODATA 7
#define RET_PAT 8
#define RET_CAT 9
#define RET_NIT 10
#define RET_SDT 11
#define RET_PMT 12
#define RET_PIDS 13
#define RET_PID 14
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

struct cmd_pid_info
{
    ts_pid_info_t pids[MAX_PIDS];
};
