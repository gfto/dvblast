/*****************************************************************************
 * comm.h
 *****************************************************************************
 * Copyright (C) 2008 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *****************************************************************************/

#ifdef HAVE_DVB_SUPPORT
/* DVB Card Drivers */
#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/ca.h>
#endif

#include <bitstream/mpeg/psi.h>

#define COMM_HEADER_SIZE 8
#define COMM_BUFFER_SIZE (COMM_HEADER_SIZE + ((PSI_PRIVATE_MAX_SIZE + PSI_HEADER_SIZE) * PSI_TABLE_MAX_SECTIONS))
#define COMM_HEADER_MAGIC 0x49

#define COMM_MAX_MSG_CHUNK 4096

typedef enum {
    CMD_INVALID             = 0,
    CMD_RELOAD              = 1,
    CMD_SHUTDOWN            = 2,
    CMD_FRONTEND_STATUS     = 3,
    CMD_MMI_STATUS          = 4,
    CMD_MMI_SLOT_STATUS     = 5, /* arg: slot */
    CMD_MMI_OPEN            = 6, /* arg: slot */
    CMD_MMI_CLOSE           = 7, /* arg: slot */
    CMD_MMI_RECV            = 8, /* arg: slot */
    CMD_GET_PAT             = 10,
    CMD_GET_CAT             = 11,
    CMD_GET_NIT             = 12,
    CMD_GET_SDT             = 13,
    CMD_GET_PMT             = 14, /* arg: service_id (uint16_t) */
    CMD_GET_PIDS            = 15,
    CMD_GET_PID             = 16, /* arg: pid (uint16_t) */
    CMD_MMI_SEND_TEXT       = 17, /* arg: slot, en50221_mmi_object_t */
    CMD_MMI_SEND_CHOICE     = 18, /* arg: slot, en50221_mmi_object_t */
    CMD_GET_EIT_PF          = 19, /* arg: service_id (uint16_t) */
    CMD_GET_EIT_SCHEDULE    = 20, /* arg: service_id (uint16_t) */
} ctl_cmd_t;

typedef enum {
    RET_OK                  = 0,
    RET_ERR                 = 1,
    RET_FRONTEND_STATUS     = 2,
    RET_MMI_STATUS          = 3,
    RET_MMI_SLOT_STATUS     = 4,
    RET_MMI_RECV            = 5,
    RET_MMI_WAIT            = 6,
    RET_NODATA              = 7,
    RET_PAT                 = 8,
    RET_CAT                 = 9,
    RET_NIT                 = 10,
    RET_SDT                 = 11,
    RET_PMT                 = 12,
    RET_PIDS                = 13,
    RET_PID                 = 14,
    RET_EIT_PF              = 15,
    RET_EIT_SCHEDULE        = 16,
    RET_HUH                 = 255,
} ctl_cmd_answer_t;

#ifdef HAVE_DVB_SUPPORT
struct ret_frontend_status
{
    struct dvb_frontend_info info;
    fe_status_t i_status;
    uint32_t i_ber;
    uint16_t i_strength, i_snr;
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
#endif

struct cmd_pid_info
{
    ts_pid_info_t pids[MAX_PIDS];
};
