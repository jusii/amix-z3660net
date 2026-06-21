#ifndef _AMIGA_Z3660ETH_H
#define _AMIGA_Z3660ETH_H
/*
 * z3660eth.h -- kernel-side definitions for the Z3660 native AMIX ethernet
 * driver (the STREAMS/DLPI analogue of the z3660.c SCSI driver).  Includes the
 * Z3660 ethernet register map + frame-window layout, all VERIFIED against the
 * firmware/guest source (z3660-drivers/common/z3660_regs.h, Z3660/src/
 * ethernet.c + memorymap.h + rtg/rtg.c).  See ETHERNET-SCOPING.md.
 */

#include "z3660ethuser.h"

/* ---- board identity / discovery (same combo board as the z3660.c SCSI) ---- */
#define Z3660_PROD	0x144B0001	/* autocon pc = (manuf<<16)|product */
#define Z3660_FIXED	0x10000000	/* combo window base when not autoconfigured */
#define VPOSR		0xDFF004	/* Agnus/Alice id: bits 8-14 >= 0x22 => AGA */

/* ---- Z3660 ethernet registers (board_base-relative, 32-bit big-endian MMIO).
 * These live in the RTG register page at board_base+0x000 (piscsi is +0x2000). */
#define ZZ_CONFIG	0x104	/* W: enable=1, ack ETH=24 (8|16), disable=0 */
#define ZZ_ETH_TX	0x190	/* W byte-length = trigger TX; R = TX result */
#define ZZ_ETH_RX	0x194	/* W (any value) = advance/free current RX slot */
#define ZZ_ETH_MAC_HI	0x198	/* R/W: MAC[0..1] = (b0<<8)|b1 */
#define ZZ_ETH_MAC_LO	0x19C	/* R/W: MAC[2..5]; write triggers GEM SetMac */
#define ZZ_ETH_RX_ADDR	0x1A4	/* R: board-relative byte offset of cur RX slot */
#define ZZ_INT_STATUS	0x1A8	/* R: pending bits ETH=1, AUDIO=2, USB=4 */

/* REG_ZZ_CONFIG transactions (mutually exclusive firmware branches, rtg.c:1150) */
#define ZZ_CONFIG_ENABLE	1	/* enable eth interrupt delivery */
#define ZZ_CONFIG_DISABLE	0
#define ZZ_CONFIG_ACK_ETH	(8|16)	/* = 24: bit3 gated ack/clear of ETH */

#define ZZ_INT_ETH	1	/* REG_ZZ_INT_STATUS: ethernet RX pending */

/* ---- frame windows (board_base-relative; real DDR through the Zorro III win).
 * memorymap.h: RX_BACKLOG=0x07ED0000 ("32*2048 = 64kB"), TX_FRAME=0x07EE0000,
 * GEM RX_FRAME=0x07EF0000.  FRAME_MAX_BACKLOG(64) overruns the 32-slot ring --
 * latent firmware bug; mitigated by eager draining + mapping the full range. */
#define ZZ_RXBACKLOG_OFF 0x07ED0000	/* start of the RX backlog ring */
#define ZZ_TXFRAME_OFF	 0x07EE0000	/* TX staging frame (one frame) */
#define ZZ_FRAME_SIZE	 2048		/* per-slot stride */
#define ZZ_RX_FRAME_PAD	 4		/* per-slot header: [0..1]=len [2..3]=serial */
#define ZZ_RX_RING_SLOTS 32		/* real usable slots before TX_FRAME */
#define ZZ_RX_MAX_SLOTS	 64		/* firmware FRAME_MAX_BACKLOG (overrun range) */

/*
 * sptalloc page counts (Amix NBPP = 2048, NOT 4096 -- see z3660.c).
 *   regs page : 1 page at board_base+0 covers 0x000..0x7FF (regs 0x104..0x1A8)
 *   frame rgn : map board_base+0x07ED0000 for the full 64-slot overrun range
 *               (128 KB = 64 pages) so any ZZ_ETH_RX_ADDR value is mapped;
 *               txwin = frame_region + (0x07EE0000-0x07ED0000) = +0x10000.
 */
#define ZZ_REGS_PAGES	1
#define ZZ_FRAME_PAGES	64			/* 64 * 2048 = 128 KB */
#define ZZ_TXWIN_OFF	(ZZ_TXFRAME_OFF - ZZ_RXBACKLOG_OFF)	/* 0x10000 */

/* ---- ethernet framing (mirrors aen.h / hydra.h) ---- */
#define ETH_ADDRESS_SIZE	6
#define ETH_TYPE_SIZE		2
#define ETH_LENGTH_SIZE		2
#define ETH_CRC_LEN		4
#define ETH_HEADER_SIZE		(ETH_ADDRESS_SIZE + ETH_ADDRESS_SIZE + ETH_TYPE_SIZE)
#define ETH_MAXDATA		1500
#define ETH_MAXPACKET		(ETH_HEADER_SIZE + ETH_MAXDATA)
/*
 * Min frame: the Z3660 firmware delivers the raw XEmacPs_BdGetLength and never
 * strips the FCS (no XEmacPs_SetOptions FCS_STRIP) -- so the on-wire minimum the
 * driver should pad TX to is 60 (ETH_MINPACKET), and RX length is NOT reduced by
 * ETH_CRC_LEN (do NOT inherit aen's -4).  See ETHERNET-SCOPING.md Q4.
 */
#define ETH_MINDATA		46		/* 60-byte min frame - 14 hdr */
#define ETH_MINPACKET		(ETH_HEADER_SIZE + ETH_MINDATA)	/* 60 */
#define MAX_BUFFER_LENGTH	1518

#define ETHERTYPE_IP		0x0800
#define ETHERTYPE_ARP		0x0806
#define ETHERTYPE_TRAIL		0x1000
#define ETHERTYPE_NTRAILER	16

#define Z3660ETH_MTU		ETH_MAXDATA

typedef u_char ether_addr_t[6];

struct ether_header
{
    ether_addr_t	ether_dhost;
    ether_addr_t	ether_shost;
    u_short		ether_type;
};
typedef struct ether_header ether_header_t;

/* ---- driver limits + STREAMS water marks ---- */
#define Z3660ETH_MAXDEV		5	/* SAP streams per board */
#define Z3660ETH_MAXBOARDS	1	/* one Z3660 */
#define Z3660ETH_MIN		ETH_MINPACKET
#define Z3660ETH_MAX		ETH_MAXDATA
#define Z3660ETH_HIWAT		(1024 * 5)
#define Z3660ETH_LOWAT		(2 * Z3660ETH_HIWAT / 3)
#define Z3660ETH_INIT_PRI	(PZERO - 1)

/*
 * spl level.  RX is drained from a clock-level timeout() poll callout (the
 * default model -- AMIX has no level-6 driver hook for the firmware's INT6, see
 * ETHERNET-SCOPING.md Q1), so the base-level wput/proto/xmit critical sections
 * must block the callout: raise to spl6.  (If/when RX moves to an int2_tbl ISR
 * via a board-mod, this can drop to spl2 like aen/hydra.)
 */
#define splz3660eth		spl6

#define Z3660ETH_POLL_TICKS	1	/* RX poll period (clock ticks) */

#define blklen(bp)		((bp)->b_wptr - (bp)->b_rptr)
#ifndef DELAY
#define DELAY(x)		{ int _d = (x) * 100; while (_d--) ; }
#endif

/* ---- per-stream state (one per opened minor/SAP) ---- */
typedef struct
{
    queue_t		*q;
    int			state;		/* DL_UNATTACHED / DL_UNBOUND / DL_IDLE */
    int			board_index;
    int			flags;		/* Z3660ETH_RAW_DATA */
    unsigned short	sap;		/* bound ethertype */
} z3660eth_t;

/* ---- per-board hardware state ---- */
typedef struct
{
    long		 board_base;	/* physical board base (0x10000000 etc.) */
    volatile u_char	*regs;		/* mapped board_base+0 (register page) */
    volatile u_char	*frame;		/* mapped board_base+0x07ED0000 region */
    volatile u_char	*txwin;		/* frame + ZZ_TXWIN_OFF */
    unsigned char	 paddress[6];	/* station MAC */
    unsigned short	 last_serial;	/* RX ring drain cursor (firmware serial) */
    int			 tx_busy;	/* serialises the single txwin */
    int			 poll_id;	/* timeout() id for the RX poll callout */
    int			 poll_running;
} z3660eth_info_t;

typedef struct
{
    z3660eth_t		z3660eth[Z3660ETH_MAXDEV];
    z3660eth_status_t	z3660eth_status;
    z3660eth_info_t	z3660eth_info;
    struct ifstats	ifstats;
    int			if_flags;
} z3660eth_board_t;

#endif /* _AMIGA_Z3660ETH_H */
