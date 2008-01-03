#!/bin/bash

#
#  Copyright Red Hat Inc., 2007
#
#  This program is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by the
#  Free Software Foundation; either version 2, or (at your option) any
#  later version.
#
#  This program is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#  General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; see the file COPYING.  If not, write to the
#  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
#  MA 02139, USA.
#

#
# LVM Failover Script.
# NOTE: Changes to /etc/lvm/lvm.conf are required for proper operation.

LC_ALL=C
LANG=C
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export LC_ALL LANG PATH

. $(dirname $0)/ocf-shellfuncs
. $(dirname $0)/utils/member_util.sh
. $(dirname $0)/lvm_by_lv.sh
. $(dirname $0)/lvm_by_vg.sh

rv=0

################################################################################
# clvm_check
#
################################################################################
function clvm_check
{
	if [[ $(vgs -o attr --noheadings $1) =~ .....c ]]; then
		return 1
	fi

	return 0
}

################################################################################
# ha_lvm_proper_setup_check
#
################################################################################
function ha_lvm_proper_setup_check
{
	##
	# Machine's cluster node name must be present as
	# a tag in lvm.conf:activation/volume_list
	##
	if ! lvm dumpconfig activation/volume_list >& /dev/null ||
	   ! lvm dumpconfig activation/volume_list | grep $(local_node_name); then
		ocf_log err "lvm.conf improperly configured for HA LVM."
		return $OCF_ERR_GENERIC
	fi

	##
	# Next, we need to ensure that their initrd has been updated
	# If not, the machine could boot and activate the VG outside
	# the control of rgmanager
	##
	# Fixme: we might be able to perform a better check...
	if [ "$(find /boot/*.img -newer /etc/lvm/lvm.conf)" == "" ]; then
		ocf_log err "HA LVM requires the initrd image to be newer than lvm.conf"
		return $OCF_ERR_GENERIC
	fi

	return $OCF_SUCCESS
}

################################################################################
# MAIN
################################################################################

case $1 in
start)
	##
	# We can safely ignore clustered volume groups (VGs handled by CLVM)
	##
	if ! clvm_check $OCF_RESKEY_vg_name; then
		ocf_log notice "$OCF_RESKEY_vg_name is a cluster volume.  Ignoring..."
		exit 0
	fi

	ha_lvm_proper_setup_check || exit 1

	rv=0

	if [ -z $OCF_RESKEY_lv_name ]; then
		vg_start || exit 1
	else
		lv_start || exit 1
	fi
	;;

status|monitor)
	ocf_log notice "Getting status"

	if [ -z $OCF_RESKEY_lv_name ]; then
		vg_status || exit 1
	else
		lv_status || exit 1
	fi
	rv=0
	;;
		    
stop)
	##
	# We can safely ignore clustered volume groups (VGs handled by CLVM)
	##
	if ! clvm_check $OCF_RESKEY_vg_name; then
		ocf_log notice "$OCF_RESKEY_vg_name is a cluster volume.  Ignoring..."
		exit 0
	fi

	if ! ha_lvm_proper_setup_check; then
		ocf_log err "WARNING: An improper setup can cause data corruption!"
	fi

	if [ -z $OCF_RESKEY_lv_name ]; then
		vg_stop || exit 1
	else
		lv_stop || exit 1
	fi
	rv=0
	;;

recover|restart)
	$0 stop || exit $OCF_ERR_GENERIC
	$0 start || exit $OCF_ERR_GENERIC
	rv=0
	;;

meta-data)
	cat `echo $0 | sed 's/^\(.*\)\.sh$/\1.metadata/'`
	rv=0
	;;

validate-all|verify-all)
	##
	# We can safely ignore clustered volume groups (VGs handled by CLVM)
	##
	if ! clvm_check $OCF_RESKEY_vg_name; then
		ocf_log notice "$OCF_RESKEY_vg_name is a cluster volume.  Ignoring..."
		exit 0
	fi

	if [ -z $OCF_RESKEY_lv_name ]; then
		vg_verify || exit 1
	else
		lv_verify || exit 1
	fi
	rv=0
	;;
*)
	echo "usage: $0 {start|status|monitor|stop|restart|meta-data|verify-all}"
	exit $OCF_ERR_UNIMPLEMENTED
	;;
esac

exit $rv
