/*
 * zen -- Z3660 ethernet presence / status tool (build on-box: cc -o zen zen.c).
 *
 *   zen -S    Silent presence check: exit 0 if the Z3660 ethernet is present,
 *             1 if absent (open() returns ENXIO because no Z3660 GEM was found).
 *             Used to gate slink/ifconfig in /etc/inet/network-config so a
 *             board-less boot (or the WinUAE build box) does not stall.
 *   zen       Print the driver status counters (Z3660ETH_GET_STATUS).
 *
 * Mirrors the stock `aen -S` / Hydra `hya -S` presence convention.  Install as
 * /usr/amiga/bin/zen.
 */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include "z3660ethuser.h"

#define DEV "/dev/zen0"

extern int errno;

main(argc, argv)
int argc;
char **argv;
{
	int fd, silent;
	z3660eth_status_t st;

	silent = (argc > 1 && strcmp(argv[1], "-S") == 0);

	if ((fd = open(DEV, O_RDWR)) < 0) {
		if (!silent)
			fprintf(stderr, "zen: %s: %s\n", DEV,
				errno == ENXIO ? "no Z3660 ethernet present"
					       : strerror(errno));
		exit(1);
	}

	if (silent) {			/* presence check only */
		close(fd);
		exit(0);
	}

	if (ioctl(fd, Z3660ETH_GET_STATUS, &st) == 0) {
		printf("zen0: state=%u  tx=%u rx=%u  txerr=%u nolink=%u rxerr=%u  drains=%u\n",
		       st.board_state, st.packets_sent, st.packets_received,
		       st.tx_errors, st.tx_nolink, st.rx_errors, st.drain_calls);
		printf("      last_tx_sap=0x%04x last_rx_sap=0x%04x  last_serial=%u last_rx_len=%u\n",
		       st.last_tx_sap, st.last_rx_sap, st.last_serial, st.last_rx_len);
		printf("      arp(tx=%u rx=%u) ip(tx=%u rx=%u)  delivered=%u no_sap=%u not_for_us=%u overrun=%u\n",
		       st.tx_arp, st.rx_arp_seen, st.tx_ip, st.rx_ip_seen,
		       st.rx_delivered, st.rx_no_sap, st.rx_not_for_us, st.rx_overrun);
	} else
		printf("zen0: present (GET_STATUS ioctl failed: %s)\n", strerror(errno));

	close(fd);
	exit(0);
}
