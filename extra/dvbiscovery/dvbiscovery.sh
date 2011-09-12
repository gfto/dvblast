#!/bin/sh
###############################################################################
# dvbiscovery.sh
###############################################################################
# Copyright (C) 2010 VideoLAN
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

CONF_BASE="/usr/local/share/dvblast/dvbiscovery"
#CONF_BASE="./"
DVBLAST=dvblast
LOCK_TIMEOUT=2500
QUIT_TIMEOUT=20000

usage() {
	echo "Usage: $0 [-a <adapter #>] [-n <frontend #>] [-S <diseqc sat num>] [-c <conf file>]" >&2
	exit 1
}

conf_file_passed=""
adapter=""
frontend=""
diseqc=""

TEMP=`getopt -o a:n:S:c: -n "$0" -- "$@"`

if test $? -ne 0; then
	usage
fi

eval set -- "$TEMP"

while :; do
	case "$1" in
		-a)
			adapter="-a $2"
			shift 2
			;;
		-n)
			frontend="-n $2"
			shift 2
			;;
		-c)
			conf_file_passed=$2
			shift 2
			;;
		-S)
			diseqc="-S $2"
			shift 2
			;;
		--)
			shift
			break
			;;
		*)
			usage
			;;
	esac
done

type=`$DVBLAST $diseqc $adapter $frontend -f 0 2>&1 | grep '^debug: Frontend' |  sed 's/^debug: Frontend ".*" type "\(.*\)" supports:$/\1/'`
tune=""
conf_file=""

case "$type" in
	"QPSK (DVB-S/S2)")
		conf_file="${CONF_BASE}_dvb-s.conf"
		tune=tune_sat
		;;
	"QAM  (DVB-C)")
		conf_file="${CONF_BASE}_dvb-c.conf"
		tune=tune_cable
		;;
	"OFDM (DVB-T)")
		conf_file="${CONF_BASE}_dvb-t.conf"
		tune=tune_dtt
		;;
	"ATSC")
		conf_file="${CONF_BASE}_atsc.conf"
		tune=tune_atsc
		;;
	*)
		echo "unknown frontend type $type" >&2
		exit 1
esac

if test -n "$conf_file_passed"; then
	conf_file=$conf_file_passed
fi

if ! test -r "$conf_file"; then
	echo "unable to open $conf_file" >&2
	exit 1
fi

signal_catch() {
	if test $childpid -ne 0; then
		kill $childpid
		wait $childpid
	fi
	exit 1
}

exec_dvblast() {
	tmp_file=`mktemp`

	$DVBLAST $diseqc $adapter $frontend -O $LOCK_TIMEOUT -Q $QUIT_TIMEOUT -q4 -x xml $opts >| $tmp_file &
	childpid=$!
	wait $childpid
	if test $? -eq 0; then
		cat $tmp_file
		echo "</TS>"
		rm $tmp_file
		exit 0
	fi

	childpid=0
	rm $tmp_file
}

strtofec() {
	case "$1" in
		"NONE") opts="$opts $2 0" ;;
		"1/2") opts="$opts $2 12" ;;
		"2/3") opts="$opts $2 23" ;;
		"3/4") opts="$opts $2 34" ;;
		"4/5") opts="$opts $2 45" ;;
		"5/6") opts="$opts $2 56" ;;
		"6/7") opts="$opts $2 67" ;;
		"7/8") opts="$opts $2 78" ;;
		"8/9") opts="$opts $2 89" ;;
		"AUTO"|*) ;;
	esac
}

strtomod() {
	case "$1" in
		"QPSK") opts="$opts -m qpsk" ;;
		"QAM16") opts="$opts -m qam_16" ;;
		"QAM32") opts="$opts -m qam_32" ;;
		"QAM64") opts="$opts -m qam_64" ;;
		"QAM128") opts="$opts -m qam_128" ;;
		"8VSB") opts="$opts -m vsb_8" ;;
		"16VSB") opts="$opts -m vsb_16" ;;
		"AUTO"|*) ;;
	esac
}

tune_sat() {
	childpid=0
	trap signal_catch 1 2 3 15

	while read sys freq pol srate fec what mod; do
		opts="-f $freq -s $srate"

		case "$sys" in
			"S") ;;
			"S2")
			case "$mod" in
					"QPSK") opts="$opts -m qpsk" ;;
					"8PSK") opts="$opts -m psk_8" ;;
					*)
						echo "invalid modulation $mod" >&2
						;;
				esac
				;;
			*)
				echo "incompatible file" >&2
				exit 1
				;;
		esac

		strtofec $fec "-F"

		case "$pol" in
			"V") opts="$opts -v 13" ;;
			"H") opts="$opts -v 18" ;;
			*) ;;
		esac

		exec_dvblast
	done
}

tune_cable() {
	childpid=0
	trap signal_catch 1 2 3 15

	while read sys freq srate fec mod; do
		opts="-f $freq -s $srate"

		case "$sys" in
			"C") ;;
			*)
				echo "incompatible file" >&2
				exit 1
				;;
		esac

		strtofec $fec "-F"
		strtomod $mod

		exec_dvblast
	done
}

tune_dtt() {
	childpid=0
	trap signal_catch 1 2 3 15

	while read sys freq bw fec fec2 mod mode guard hier; do
		opts="-f $freq"

		case "$sys" in
			"T"|"T2") ;;
			*)
				echo "incompatible file" >&2
				exit 1
				;;
		esac

		case "$bw" in
			"8MHz") opts="$opts -b 8" ;;
			"7MHz") opts="$opts -b 7" ;;
			"6MHz") opts="$opts -b 6" ;;
			"AUTO"|*) ;;
		esac

		strtofec $fec "-F"
		strtofec $fec2 "-K"
		strtomod $mod

		case "$mode" in
			"2k") opts="$opts -X 2" ;;
			"8k") opts="$opts -X 8" ;;
			"AUTO"|*) ;;
		esac

		case "$guard" in
			"1/32") opts="$opts -G 32" ;;
			"1/16") opts="$opts -G 16" ;;
			"1/8") opts="$opts -G 8" ;;
			"1/4") opts="$opts -G 4" ;;
			"AUTO"|*) ;;
		esac

		case "$hier" in
			"NONE") opts="$opts -H 0" ;;
			"1") opts="$opts -H 1" ;;
			"2") opts="$opts -H 2" ;;
			"4") opts="$opts -H 4" ;;
			"AUTO"|*) ;;
		esac

		exec_dvblast
	done
}

tune_atsc() {
	childpid=0
	trap signal_catch 1 2 3 15

	while read sys freq mod; do
		opts="-f $freq"

		case "$sys" in
			"A") ;;
			*)
				echo "incompatible file" >&2
				exit 1
				;;
		esac

		strtomod $mod

		exec_dvblast
	done
}

childpid=0
trap signal_catch 1 2 3 15

grep -v "^#" < "$conf_file" 2>/dev/null | $tune &
childpid=$!
wait $childpid

exit 100
