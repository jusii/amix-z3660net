#ifndef _AMIGA_Z3660ETHUSER_H
#define _AMIGA_Z3660ETHUSER_H
/*
 * z3660ethuser.h -- user-facing definitions (ioctls + status/config structs)
 * for the Z3660 native AMIX ethernet driver.  Contains NO kernel types so a
 * userland status/diagnostic tool can include it directly (mirrors hydrauser.h).
 */

#define Z3660ETHIOC		('Z'<<8)
#define Z3660ETH_CLEAR_STATUS	(Z3660ETHIOC|1)
#define Z3660ETH_GET_STATUS	(Z3660ETHIOC|2)
#define Z3660ETH_GET_CONFIG	(Z3660ETHIOC|3)
#define Z3660ETH_NUMBER_OF_BOARDS (Z3660ETHIOC|5)

#define Z3660ETH_RAW_DATA	(1)

enum Z3660ETH_BOARD_STATE
{
    Z3660ETH_BOARD_RESET,
    Z3660ETH_BOARD_RUNNING
};

/*
 * Runtime counters, readable via Z3660ETH_GET_STATUS (and the diagnostic
 * globals below).  Kept deliberately verbose like the Hydra driver so the
 * datapath can be characterised over the serial console / a /dev probe during
 * the real-HW bring-up (Phases 2-4 of ETHERNET-SCOPING.md).
 */
typedef struct
{
    unsigned int	board_state;
    unsigned int	packets_sent;
    unsigned int	packets_received;
    unsigned int	allocbs_failed;
    unsigned int	couldnt_put;
    unsigned int	tx_errors;	/* trigger returned rc 2/3/4 */
    unsigned int	tx_nolink;	/* trigger returned rc 1 (no carrier) */
    unsigned int	rx_errors;
    unsigned int	rx_delivered;
    unsigned int	rx_no_sap;
    unsigned int	rx_not_for_us;
    unsigned int	rx_bad_size;
    unsigned int	rx_overrun;	/* slot pointer past the 32-slot ring */
    unsigned int	last_rx_sap;
    unsigned int	last_tx_sap;
    unsigned int	last_bound_sap;
    unsigned int	last_serial;
    unsigned int	last_rx_len;
    unsigned int	tx_arp;
    unsigned int	tx_ip;
    unsigned int	rx_arp_seen;
    unsigned int	rx_ip_seen;
    unsigned int	drain_calls;	/* poll/ISR drain invocations */
    unsigned int	if_ipackets;
    unsigned int	if_opackets;
    unsigned int	if_ierrors;
    unsigned int	if_oerrors;
} z3660eth_status_t;

typedef struct
{
    long		board_base;
    unsigned char	paddress[6];
    int			mode;
    int			flags;
} z3660eth_config_t;

#endif /* _AMIGA_Z3660ETHUSER_H */
