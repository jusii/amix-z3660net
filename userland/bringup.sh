#!/bin/sh
# bringup.sh -- manual one-shot bring-up of the Z3660 native ethernet on real HW.
# Run ON the box (single-user is fine).  AMIX is SVR4.0: use slink, NOT `ifconfig plumb`.
# Interface zen0 @ 192.168.2.39 (the WinUAE build box keeps .38 -- different MAC, no clash).

IP=192.168.2.39
MASK=255.255.255.0
GW=192.168.2.1
MAJOR=48

# 1. device node (persists on the on-disk root -- only needed once)
[ -c /dev/zen0 ] || mknod /dev/zen0 c $MAJOR 0

# 2. presence check (driver autoconfigs the Z3660 on first open)
if /usr/amiga/bin/zen -S; then
	echo "zen0: Z3660 ethernet present"
else
	echo "zen0: NOT present (open ENXIO) -- aborting" >&2
	exit 1
fi

# 3. ensure the base inet streams are linked, then link the driver under IP+ARP
/usr/sbin/slink
/usr/sbin/slink addaen /dev/zen0 zen0

# 4. address + bring up (no trailers), default route (trailing 1 = metric, mandatory)
/usr/sbin/ifconfig zen0 $IP netmask $MASK up -trailers
/usr/sbin/route add default $GW 1

/usr/sbin/ifconfig zen0
echo
echo "now try:   ping $GW        (ARP must resolve first -- watch 'zen' counters)"
echo "status:    /usr/amiga/bin/zen"
