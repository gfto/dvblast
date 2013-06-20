/*****************************************************************************
 * asi-deltacast.h: support for Deltacast ASI cards
 *****************************************************************************
 * Copyright (C) 2004, 2009 VideoLAN
 *
 * Authors: Simon Lockhart <simon@slimey.org>
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

#ifndef _ASI_DELTACAST_H
#define _ASI_DELTACAST_H

typedef enum error_source
{
   DCERR_SRC_WIN32 = 0,    /*! Win32 error code */
   DCERR_SRC_SDI_API,      /*! DELTA-sdi VIDEOMASTER error code */
   DCERR_SRC_SDI_DRIVER,   /*! DELTA-sdi DRIVER error code */
   DCERR_SRC_STREAMMASTER, /*! DELTA-asi StreamMaster error code */
   DCERR_SRC_SYSTEMSMASTER /*! DELTA-asi SystemsMaster error code */
} ERRORSOURCE;

ULONG WINAPI Dc_GetLastError (ERRORSOURCE *pErrorSource);

#define DC_ERRORCODE_MASK      0x2FFF0000                                            /* facility mask */
#define DCERR_TIMEOUT                  0x80000201  /*! A time-out occured */


#endif
