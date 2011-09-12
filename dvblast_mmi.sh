#!/bin/sh
###############################################################################
# dvblast_mmi.sh
###############################################################################
# Copyright (C) 1998-2008 VideoLAN
#
# Authors: Christophe Massiot <massiot@via.ecp.fr>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
###############################################################################

BASE_DIR=`dirname $_`


#
# Check args
#

if test x"$1" != x"-r" -o -z "$2"; then
	echo "Usage: $0 -r <remote socket> [<slot 0-n>]" >&2
	exit 1
fi

SOCKET=$2
SLOT=$3

if which dvblastctl >/dev/null; then
	DVBLASTCTL="dvblastctl -r $SOCKET"
elif test -x "$PWD/dvblastctl"; then
	DVBLASTCTL="$PWD/dvblastctl -r $SOCKET"
elif test -x "$BASE_DIR/dvblastctl"; then
	DVBLASTCTL="$BASE_DIR/dvblastctl -r $SOCKET"
else
	echo "Couldn't find dvblastctl"
	exit 1
fi


#
# Check adapter status
#

$DVBLASTCTL mmi_status >/dev/null
NUM_SLOTS=$?

if test $NUM_SLOTS -eq 255; then
	echo "Unable to set up a connection" >&2
	exit 2
fi
if test $NUM_SLOTS -eq 0; then
	echo "Adapter has no available CAM module" >&2
	exit 3
fi
if test -z $SLOT; then
	echo "Defaulting to slot #0"
	SLOT=0
fi
if test "$SLOT" -ge $NUM_SLOTS; then
	echo "Slot out of range, pick in the range 0-`expr $NUM_SLOTS - 1`" >&2
	exit 3
fi


#
# Check CAM status
#

$DVBLASTCTL mmi_slot_status $SLOT >/dev/null
STATUS=$?

if test $STATUS -ne 0; then
	echo "Slot is not ready, retry later" >&2
	exit 3
fi

$DVBLASTCTL mmi_get $SLOT >/dev/null
STATUS=$?

if test $STATUS -eq 255; then
	echo "Opening MMI session..."
	$DVBLASTCTL mmi_open $SLOT
	STATUS=$?
	if test $STATUS -eq 255; then
		echo "Communication error" >&2
		exit 2
	elif test $STATUS -ne 0; then
		echo "Couldn't open MMI session" >&2
		exit 4
	fi
	sleep 3
fi


#
# Da loop
#

while :; do
	$DVBLASTCTL mmi_get $SLOT
	STATUS=$?

	case $STATUS in
	255)
		echo "Connection closed" >&2
		exit 2
		;;
	254)
		echo -n "Your choice (empty for extra choices) ? "
		;;
	253)
		echo "CAUTION: the password won't be bulleted, be alone"
		echo -n "Your choice (empty for extra choices) ? "
		;;
	252)
		sleep 1
		continue
		;;
	0)
		echo -n "Your choice: (B)ack, (C)lose or (R)etry ? "
		;;
	*)
		echo -n "Your choice: [0-$STATUS], (C)lose or (R)etry ? "
		;;
	esac

	read -r ANSWER

	case $STATUS in
	254|253)
		if test -z "$ANSWER"; then
			echo -n "(B)ack, (C)lose or (R)etry ? "
			read -r ANSWER

			case "$ANSWER" in
			B|b|Back|back|BACK)
				if ! $DVBLASTCTL mmi_send_text $SLOT; then
					echo "mmi_send_text failed, apparently" >&2
				else
					sleep 1
				fi
				;;
			C|c|Close|close|CLOSE)
				$DVBLASTCTL mmi_close $SLOT
				exit 0
				;;
			R|r|Retry|retry|RETRY)
				:
				;;
			*)
				echo "Invalid string, retry..."
				;;
			esac

		else
			if ! $DVBLASTCTL mmi_send_text $SLOT "$ANSWER"; then
				echo "mmi_send_text failed, apparently" >&2
			else
				sleep 1
			fi
		fi
		;;

	*)
		case "$ANSWER" in
		B|b|Back|back|BACK)
			if ! $DVBLASTCTL mmi_send_choice $SLOT 0; then
				echo "mmi_send_choice failed, apparently" >&2
			else
				sleep 1
			fi
			;;
		C|c|Close|close|CLOSE)
			$DVBLASTCTL mmi_close $SLOT
			exit 0
			;;
		R|r|Retry|retry|RETRY)
			:
			;;
		*)
			echo "$ANSWER" | grep -q "^[[:digit:]]\+\$"
			if test $? -ne 0; then
				echo "Invalid string, retry..."
			else
				if ! $DVBLASTCTL mmi_send_choice $SLOT "$ANSWER"; then
					echo "mmi_send_choice failed, apparently" >&2
				else
					sleep 1
				fi
			fi
			;;
		esac
		;;
	esac

	echo
done
