/*
 * z3660eth.c -- Z3660 accelerator onboard ethernet for Amix (SVR4 / 68030).
 *
 * The native STREAMS/DLPI ethernet analogue of the z3660.c SCSI driver.  The
 * Z3660 firmware already moves whole ethernet frames between the 68k guest and
 * the Zynq GEM for AmigaOS's SANA-II Z3660Net.device; this driver speaks the
 * SAME firmware mailbox (a tiny MMIO register + two shared DDR frame windows)
 * but presents it to Amix as a DLPI Style-1 connectionless ethernet provider.
 * It is NOT NIC emulation.  The board is the same Zorro III combo board the
 * SCSI/RTG side enumerates (manuf 0x144B, product 0x01).
 *
 * Protocol (board_base-relative, all big-endian MMIO -- see z3660eth.h, verified
 * against z3660-drivers/common/z3660_regs.h + Z3660/src/{ethernet.c,rtg/rtg.c}):
 *   ZZ_CONFIG(0x104)  enable=1 / ack-ETH=24 / disable=0   (mutually exclusive)
 *   ZZ_ETH_TX(0x190)  W byte-length = trigger TX (synchronous ~1ms); R = result
 *   ZZ_ETH_RX(0x194)  W any = advance/free the current RX backlog slot
 *   ZZ_ETH_MAC_HI/LO  the firmware-owned station MAC
 *   ZZ_ETH_RX_ADDR(0x1A4) R = board-relative byte offset of the current RX slot
 *   ZZ_INT_STATUS(0x1A8)  R = pending bits (ETH=1)
 *   TX frame  @ board+0x07EE0000 (bare frame), RX backlog ring @ board+0x07ED0000
 *   (32 usable slots x 2048; each slot = 4-byte [len][serial] header + payload).
 *
 * RX delivery: the firmware raises the receive interrupt as Amiga INT6/EXTER,
 * for which Amix has NO driver hook (only int2_tbl, walked at level 2).  So this
 * driver DRAINS the backlog from a clock-level timeout() poll callout by default
 * (z3660eth_poll -> z3660eth_drain).  z3660ethintr() is provided for a future
 * interrupt-driven path (int2_tbl via board-mod, or a p6int/int6_tbl change) but
 * is not wired into kernel.c.  TX is synchronous and needs no interrupt.
 *
 * Cache coherency: the eth windows are CACHEABLE on the 030 (under TT0, CI=0) and
 * per-page cache-inhibit is impossible (TT match bypasses the PTE; no PG_CI bit).
 * Explicit 68030 line-granular cpushl ops are provided behind Z3660ETH_CACHE_FLUSH
 * (default OFF -- matching the proven aen/z3660 no-flush behaviour, which works
 * because the 030 data cache is effectively off; enable + re-soak if the real-HW
 * coherency campaign shows torn frames).  See ETHERNET-SCOPING.md sections 5/10.
 *
 * Detection mirrors z3660.c exactly: autocon(0x144B0001) first; on a miss, and
 * only on AGA (VPOSR>=0x22, so the ECS build box never touches it), probe the
 * fixed combo base 0x10000000 and verify the firmware MAC register answers.  On
 * the WinUAE build box (which emulates an a2065, not the Z3660 GEM) detection
 * misses and open() returns ENXIO -- the compile/link/boot/register gate.
 *
 * Refs: hydra-amix aen/aen.c + hydra/hydra.c (STREAMS/DLPI skeleton),
 *       amix-z3660/src/z3660.c (Z3660 mailbox/sptalloc idiom), ETHERNET-SCOPING.md.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/signal.h"
#include "sys/errno.h"
#include "sys/psw.h"
#include "sys/pcb.h"
#include "sys/user.h"
#include "sys/sysmacros.h"	/* makedevice, getminor, getmajor */
#include "sys/immu.h"		/* PG_V, phystopfn, paddr_t */
#include "sys/stream.h"
#include "sys/stropts.h"
#include "sys/queue.h"
#include "sys/sbd.h"
#include "sys/debug.h"
#include "sys/systm.h"
#include "sys/cred.h"
#include "sys/dlpi.h"
#include "sys/ddi.h"
#include "sys/cmn_err.h"
#include "sys/socket.h"
#include "sys/sockio.h"
#include "net/if.h"
#include "z3660eth.h"

extern int	autocon();
extern caddr_t	sptalloc();
extern int	timeout();
extern void	untimeout();
extern void	bcopy(), bzero();

#ifndef min
#define min(a,b)	((a) < (b) ? (a) : (b))
#endif

extern char *panicstr;

void z3660eth_autoconfig();
void z3660ethinit();
void z3660ethintr();
static void z3660eth_poll();
static void z3660eth_drain();
static void z3660eth_tx_drain();
static void toss_packet_up_stream();
static int  z3660ethopen(), z3660ethclose(), z3660ethwput(), z3660ethwsrv();
static void z3660ethioctl();
static int  z3660ethproto(), z3660ethxmit();
static int  z3660eth_map(), z3660eth_initialize();

static struct module_info z3660eth_minfo =
{
    0x7A65, "zen", Z3660ETH_MIN, Z3660ETH_MAX, Z3660ETH_HIWAT, Z3660ETH_LOWAT,
};

static struct qinit z3660ethrinit =
{
    NULL, NULL, z3660ethopen, z3660ethclose, NULL, &z3660eth_minfo, NULL,
};

static struct qinit z3660ethwinit =
{
    z3660ethwput, z3660ethwsrv, NULL, NULL, NULL, &z3660eth_minfo, NULL,
};

struct streamtab z3660ethinfo =		/* goes in cdevsw[].d_str */
{
    &z3660ethrinit, &z3660ethwinit, NULL, NULL,
};

extern struct ifstats *ifstats;

static z3660eth_board_t z3660eth_board[Z3660ETH_MAXBOARDS];
static int z3660eth_number_of_boards;
static int z3660eth_autoconfigured;

/* diagnostics (read via /dev/mem on real HW, like z3660.c) */
ulong z3660eth_board_phys, z3660eth_last_rc, z3660eth_last_off, z3660eth_machi, z3660eth_maclo;
uchar z3660eth_present;

#define WRLONG(base,off,val) (*(volatile ulong *)((base) + (off)) = (ulong)(val))
#define RDLONG(base,off)     (*(volatile ulong *)((base) + (off)))

/* ----------------------------------------------------------------------------
 * 68030 data-cache maintenance for the shared eth DDR windows.  Line-granular
 * cpushl (push dirty + invalidate) over [addr,addr+len); cache line = 16 bytes.
 * Default OFF (see file header).  When enabled, verify the Amix `as` accepts the
 * cpushl syntax at build time.
 * ------------------------------------------------------------------------- */
#ifdef Z3660ETH_CACHE_FLUSH
static void
z3660eth_dcache(addr, len)
volatile uchar *addr;
int len;
{
    register uchar *p = (uchar *)((long)addr & ~15L);
    register uchar *end = (uchar *)((long)addr + len);

    for (; p < end; p += 16)
	asm volatile ("cpushl %%dc,(%0)" : : "a"(p));
}
#define Z3660ETH_PUSH(a,l)	z3660eth_dcache((volatile uchar *)(a), (int)(l))
#define Z3660ETH_INVAL(a,l)	z3660eth_dcache((volatile uchar *)(a), (int)(l))
#else
#define Z3660ETH_PUSH(a,l)
#define Z3660ETH_INVAL(a,l)
#endif

/* ----------------------------------------------------------------------------
 * Discovery + mapping.  Fills board->z3660eth_info {board_base, regs, frame,
 * txwin, paddress}.  0 on success; ENXIO when no Z3660 is present; ENOMEM on a
 * mapping failure.  Mirrors z3660map() in z3660.c (TT-gap-safe sptalloc, AGA
 * gate) but verifies via the firmware MAC register instead of the SCSI mailbox.
 * ------------------------------------------------------------------------- */
static int
z3660eth_map(board_index)
int board_index;
{
    z3660eth_board_t *board = &z3660eth_board[board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    long base, size;
    ulong hi, lo;

    if (info->regs)
	return 0;

    if (!autocon(Z3660_PROD, board_index, &base, &size)) {
	if (board_index != 0) {
	    z3660eth_present = 0;
	    return ENXIO;
	}
	if ((((*(volatile ushort *)VPOSR) >> 8) & 0x7F) < 0x22) {
	    z3660eth_present = 0;
	    return ENXIO;		/* ECS/OCS build box -- no Z3660 here */
	}
	base = Z3660_FIXED;		/* AGA: probe the fixed combo window */
    }

    info->board_base = base;
    z3660eth_board_phys = (ulong)base;

    info->regs = (volatile uchar *)sptalloc(ZZ_REGS_PAGES, PG_V,
		    phystopfn((paddr_t)base), 0);
    info->frame = (volatile uchar *)sptalloc(ZZ_FRAME_PAGES, PG_V,
		    phystopfn((paddr_t)base + ZZ_RXBACKLOG_OFF), 0);
    if (info->regs == 0 || info->frame == 0) {
	info->regs = 0;
	return ENOMEM;
    }
    info->txwin = info->frame + ZZ_TXWIN_OFF;

    /* verify the firmware ethernet register file answers (open bus => absent) */
    hi = RDLONG(info->regs, ZZ_ETH_MAC_HI);
    lo = RDLONG(info->regs, ZZ_ETH_MAC_LO);
    z3660eth_machi = hi;
    z3660eth_maclo = lo;
    if (hi == 0xFFFFFFFF || (hi & 0xFFFF) == 0) {
	info->regs = 0;			/* open bus / not a Z3660 window */
	z3660eth_present = 0;
	return ENXIO;
    }

    info->paddress[0] = (uchar)(hi >> 8);
    info->paddress[1] = (uchar)hi;
    info->paddress[2] = (uchar)(lo >> 24);
    info->paddress[3] = (uchar)(lo >> 16);
    info->paddress[4] = (uchar)(lo >> 8);
    info->paddress[5] = (uchar)lo;

    z3660eth_present = 1;
    return 0;
}

void
z3660eth_autoconfig()
{
    if (z3660eth_autoconfigured)
	return;
    z3660eth_autoconfigured = 1;
    z3660eth_number_of_boards = 0;

    if (z3660eth_map(0) == 0)
	z3660eth_number_of_boards = 1;
    else
	cmn_err(CE_NOTE, "zen: no Z3660 ethernet found");
}

/*
 * Bring a board up: ensure mapped, init the per-stream array, enable firmware
 * eth interrupt delivery, and start the RX poll callout.  Returns 1 / 0.
 */
static int
z3660eth_initialize(board_index)
int board_index;
{
    z3660eth_board_t *board = &z3660eth_board[board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    z3660eth_t *zp;
    int n;

    if (z3660eth_map(board_index))
	return 0;

    for (n = 0, zp = board->z3660eth; n < Z3660ETH_MAXDEV; ++n, ++zp) {
	zp->q = (queue_t *)NULL;
	zp->sap = 0;
	zp->state = DL_UNBOUND;
    }

    info->last_serial = 0;		/* firmware's first serial is 1 */
    info->tx_busy = 0;

    bzero((caddr_t)&board->z3660eth_status, sizeof(board->z3660eth_status));
    board->z3660eth_status.board_state = Z3660ETH_BOARD_RUNNING;

    /* enable firmware eth interrupt generation (we poll the backlog regardless) */
    WRLONG(info->regs, ZZ_CONFIG, ZZ_CONFIG_ENABLE);

    if (!info->poll_running) {
	info->poll_running = 1;
	info->poll_id = timeout(z3660eth_poll, (caddr_t)(long)board_index,
				Z3660ETH_POLL_TICKS);
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * STREAMS open/close -- clone/dev_t machinery copied from hydra/aen (the double
 * open under /dev/ip and /dev/arp depends on the makedevice() minor rewrite).
 * ------------------------------------------------------------------------- */
static int
z3660ethopen(q, devp, flag, sflag, credp)
register queue_t *q;
dev_t *devp;
int flag, sflag;
cred_t *credp;
{
    register z3660eth_t *zp;
    register int minor_device, board_index, sap_index;
    z3660eth_board_t *board;

    if (sflag == MODOPEN || sflag == CLONEOPEN)
	return EINVAL;

    z3660eth_autoconfig();

    minor_device = getminor(*devp);
    board_index = minor_device & 0x0F;
    sap_index = (minor_device & 0xF0) >> 4;

    if (board_index >= Z3660ETH_MAXBOARDS)
	return ENODEV;
    board = &z3660eth_board[board_index];

    if (board->z3660eth_status.board_state != Z3660ETH_BOARD_RUNNING) {
	if (!z3660eth_initialize(board_index))
	    return ENXIO;		/* no Z3660 present (e.g. WinUAE build box) */
    }
    if (board->z3660eth_info.regs == 0)
	return ENXIO;

    if (sap_index == 0) {
	for (sap_index = 0, zp = board->z3660eth;
	     sap_index < Z3660ETH_MAXDEV;
	     sap_index++, zp++)
	    if (!zp->q)
		break;
    } else
	--sap_index;

    if (sap_index >= Z3660ETH_MAXDEV)
	return ENOSPC;

    *devp = makedevice(getmajor(*devp), (((sap_index + 1) << 4) | board_index));

    zp = &board->z3660eth[sap_index];
    if (zp->q)
	return EBUSY;

    q->q_ptr = (char *)zp;
    WR(q)->q_ptr = (char *)zp;
    zp->q = q;
    zp->board_index = board_index;
    zp->flags = 0;
    zp->state = DL_UNATTACHED;
    zp->sap = 0;

    if (!board->ifstats.ifs_name) {
	board->ifstats.ifs_name = "zen";
	board->ifstats.ifs_mtu = Z3660ETH_MTU;
	board->ifstats.ifs_next = ifstats;
	ifstats = &board->ifstats;
    }
    board->ifstats.ifs_unit = board_index;
    board->ifstats.ifs_active = 1;
    board->if_flags = IFF_BROADCAST | IFF_NOTRAILERS | IFF_RUNNING;

    return 0;
}

static int
z3660ethclose(q)
register queue_t *q;
{
    register z3660eth_t *zp = (z3660eth_t *)q->q_ptr;
    register z3660eth_board_t *board = &z3660eth_board[zp->board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    int dev;

    zp->q = 0;
    OTHERQ(q)->q_ptr = NULL;
    q->q_ptr = NULL;

    for (dev = 0, zp = board->z3660eth; dev < Z3660ETH_MAXDEV; ++dev, ++zp)
	if (zp->q)
	    break;

    if (dev >= Z3660ETH_MAXDEV) {		/* last stream closed */
	if (info->poll_running) {
	    info->poll_running = 0;
	    untimeout(info->poll_id);
	}
	if (info->regs)
	    WRLONG(info->regs, ZZ_CONFIG, ZZ_CONFIG_DISABLE);
	board->z3660eth_status.board_state = Z3660ETH_BOARD_RESET;
	board->ifstats.ifs_active = 0;
    }
    return 0;
}

static int
z3660ethwput(q, mp)
register queue_t *q;
register mblk_t *mp;
{
    register int s;

    switch (mp->b_datap->db_type) {
    case M_FLUSH:
	if (*mp->b_rptr & FLUSHW) {
	    flushq(q, FLUSHALL);
	    *mp->b_rptr &= ~FLUSHW;
	}
	if (*mp->b_rptr & FLUSHR)
	    qreply(q, mp);
	else
	    freemsg(mp);
	break;

    case M_PROTO:
    case M_PCPROTO:
	s = splz3660eth();
	(void)z3660ethproto(q, mp);
	splx(s);
	break;

    case M_IOCTL:
    case M_IOCDATA:
	z3660ethioctl(q, mp);
	break;

    default:
	freemsg(mp);
	break;
    }
    return 0;
}

static int
z3660ethwsrv(q)
queue_t *q;
{
    mblk_t *mp;

    /* drain anything deferred by xmit; no TX-completion IRQ exists */
    while ((mp = getq(q)) != NULL) {
	if (!z3660ethxmit(q, mp)) {
	    putbq(q, mp);
	    break;
	}
    }
    return 0;
}

/*
 * Transmit one DL_UNITDATA_REQ.  Returns 1 if the mblk was consumed (sent or
 * dropped), 0 if it must be re-queued (txwin busy with another transmit).  The
 * single shared txwin is serialised by info->tx_busy WITHOUT holding raised spl
 * across the ~1ms synchronous trigger (per ETHERNET-SCOPING.md Q2/Q7).
 */
static int
z3660ethxmit(q, mp)
queue_t *q;
mblk_t *mp;
{
    register z3660eth_t *zp = (z3660eth_t *)q->q_ptr;
    z3660eth_board_t *board = &z3660eth_board[zp->board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    dl_unitdata_req_t *dp = (dl_unitdata_req_t *)mp->b_rptr;
    static unsigned char tmp[ETH_MAXPACKET + 2];
    unsigned char *ap;
    mblk_t *m;
    int s, n, pktsize;
    ulong rc;

    s = splz3660eth();
    if (info->tx_busy || info->regs == 0) {
	splx(s);
	return 0;			/* defer; caller re-queues */
    }
    info->tx_busy = 1;
    splx(s);

    pktsize = ETH_HEADER_SIZE;
    ap = mp->b_rptr + dp->dl_dest_addr_offset;
    bcopy((caddr_t)ap, (caddr_t)&tmp[0], ETH_ADDRESS_SIZE);
    bcopy((caddr_t)info->paddress, (caddr_t)&tmp[6], ETH_ADDRESS_SIZE);
    bcopy((caddr_t)&zp->sap, (caddr_t)&tmp[12], ETH_TYPE_SIZE);  /* big-endian, no swap */
    board->z3660eth_status.last_tx_sap = zp->sap;

    n = ETH_MAXDATA;
    for (m = mp->b_cont; m && n > 0; m = m->b_cont) {
	int copy = min(blklen(m), n);		/* n>0 here, so copy is never negative */
	if (copy <= 0)
	    continue;				/* skip empty mblk; never overrun tmp[] */
	bcopy((caddr_t)m->b_rptr, (caddr_t)&tmp[pktsize], copy);
	pktsize += copy;
	n -= copy;
    }
    freemsg(mp);

    if (pktsize < ETH_MINPACKET) {
	bzero((caddr_t)&tmp[pktsize], ETH_MINPACKET - pktsize);
	pktsize = ETH_MINPACKET;
    }

    bcopy((caddr_t)tmp, (caddr_t)info->txwin, pktsize);
    Z3660ETH_PUSH(info->txwin, pktsize);

    /* trigger: synchronous, ~1ms -- NOT at raised spl */
    WRLONG(info->regs, ZZ_ETH_TX, pktsize);
    rc = RDLONG(info->regs, ZZ_ETH_TX);
    z3660eth_last_rc = rc;

    s = splz3660eth();
    info->tx_busy = 0;
    splx(s);

    if (rc == 0) {
	board->z3660eth_status.packets_sent++;
	board->ifstats.ifs_opackets++;
	if (zp->sap == ETHERTYPE_ARP) board->z3660eth_status.tx_arp++;
	if (zp->sap == ETHERTYPE_IP)  board->z3660eth_status.tx_ip++;
    } else if (rc == 1) {
	board->z3660eth_status.tx_nolink++;	/* no carrier yet; frame dropped */
    } else {
	board->z3660eth_status.tx_errors++;
	board->ifstats.ifs_oerrors++;
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 * RX backlog drain.  Re-reads ZZ_ETH_RX_ADDR every iteration (the cursor only
 * advances on the ZZ_ETH_RX strobe), breaks on the serial sentinel, strobes to
 * free each slot.  Callable from the poll callout OR a future ISR.
 * ------------------------------------------------------------------------- */
static void
z3660eth_drain(board_index)
int board_index;
{
    z3660eth_board_t *board = &z3660eth_board[board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    static unsigned char rxbuff[MAX_BUFFER_LENGTH];
    volatile uchar *slot;
    ulong off;
    unsigned short serial;
    int len, loop = 0;

    if (info->regs == 0)
	return;
    board->z3660eth_status.drain_calls++;

    while (++loop <= 256) {
	off = RDLONG(info->regs, ZZ_ETH_RX_ADDR);
	z3660eth_last_off = off;

	if (off < ZZ_RXBACKLOG_OFF ||
	    off >= ZZ_RXBACKLOG_OFF + ZZ_RX_MAX_SLOTS * ZZ_FRAME_SIZE) {
	    board->z3660eth_status.rx_overrun++;
	    break;
	}
	slot = info->frame + (off - ZZ_RXBACKLOG_OFF);

	Z3660ETH_INVAL(slot, ZZ_RX_FRAME_PAD);
	serial = (slot[2] << 8) | slot[3];
	if (serial == info->last_serial)
	    break;			/* ring drained */

	len = (slot[0] << 8) | slot[1];
	board->z3660eth_status.last_serial = serial;
	board->z3660eth_status.last_rx_len = len;

	if (len < ETH_HEADER_SIZE || len > MAX_BUFFER_LENGTH) {
	    board->z3660eth_status.rx_bad_size++;
	    board->z3660eth_status.rx_errors++;
	    board->ifstats.ifs_ierrors++;
	    info->last_serial = serial;
	    WRLONG(info->regs, ZZ_ETH_RX, 1);
	    continue;
	}

	Z3660ETH_INVAL(slot + ZZ_RX_FRAME_PAD, len);
	bcopy((caddr_t)(slot + ZZ_RX_FRAME_PAD), (caddr_t)rxbuff, len);

	info->last_serial = serial;
	WRLONG(info->regs, ZZ_ETH_RX, 1);	/* advance/free the slot */

	board->z3660eth_status.packets_received++;
	board->ifstats.ifs_ipackets++;

	/*
	 * NOTE (Q4, ETHERNET-SCOPING.md): `len` is the firmware-reported frame
	 * length.  Whether it includes the 4-byte FCS is real-HW-confirmable; if
	 * a one-frame probe shows trailing FCS, subtract ETH_CRC_LEN here.  The
	 * scoping verdict is "do NOT subtract by default" -- delivered as-is.
	 */
	toss_packet_up_stream(rxbuff, board_index, len);
    }
}

/* The clock-level poll callout: drain RX, then any deferred TX, then re-arm. */
static void
z3660eth_poll(arg)
caddr_t arg;
{
    int board_index = (int)(long)arg;
    z3660eth_board_t *board = &z3660eth_board[board_index];
    z3660eth_info_t *info = &board->z3660eth_info;

    if (panicstr)
	return;

    if (board->z3660eth_status.board_state == Z3660ETH_BOARD_RUNNING) {
	z3660eth_drain(board_index);
	z3660eth_tx_drain(board_index);
    }
    if (info->poll_running)
	info->poll_id = timeout(z3660eth_poll, arg, Z3660ETH_POLL_TICKS);
}

/* Drain frames deferred by xmit (txwin-busy).  Mirrors hydra transmit_interrupt. */
static void
z3660eth_tx_drain(board_index)
int board_index;
{
    z3660eth_t *zp = z3660eth_board[board_index].z3660eth;
    mblk_t *mp;
    int n;

    for (n = 0; n < Z3660ETH_MAXDEV; n++, zp++) {
	if (zp->q) {
	    queue_t *wrq = WR(zp->q);
	    while ((mp = getq(wrq)) != NULL) {
		if (!z3660ethxmit(wrq, mp)) {
		    putbq(wrq, mp);
		    return;
		}
	    }
	}
    }
}

/*
 * Interrupt service for a future interrupt-driven RX path (not wired into
 * kernel.c by default -- AMIX has no level-6 driver hook; see Q1).  Polls the
 * firmware INT_STATUS, acks ETH, and drains.
 *
 * WARNING: shares the static rxbuff in z3660eth_drain with the poll callout.  If
 * this ISR is ever registered (int2_tbl via board-mod, or an int6_tbl change),
 * DISABLE the poll first -- do not run both drainers concurrently.
 */
void
z3660ethintr()
{
    int i;

    if (panicstr)
	return;

    for (i = 0; i < Z3660ETH_MAXBOARDS; i++) {
	z3660eth_board_t *board = &z3660eth_board[i];
	z3660eth_info_t *info = &board->z3660eth_info;

	if (board->z3660eth_status.board_state == Z3660ETH_BOARD_RUNNING &&
	    info->regs != 0) {
	    if (RDLONG(info->regs, ZZ_INT_STATUS) & ZZ_INT_ETH) {
		WRLONG(info->regs, ZZ_CONFIG, ZZ_CONFIG_ACK_ETH);
		z3660eth_drain(i);
	    }
	}
    }
}

/* ----------------------------------------------------------------------------
 * DLPI protocol engine -- the seven primitives, copied from hydra (Style-1
 * connectionless ethernet).  DL_UNITDATA_REQ feeds z3660ethxmit.
 * ------------------------------------------------------------------------- */
static int
z3660ethproto(q, mp)
queue_t *q;
mblk_t *mp;
{
    register union DL_primitives *p = (union DL_primitives *)mp->b_rptr;
    register z3660eth_t *zp = (z3660eth_t *)q->q_ptr;
    z3660eth_board_t *board = &z3660eth_board[zp->board_index];

    switch (p->dl_primitive) {
    case DL_UNITDATA_REQ:
	if (q->q_first || !z3660ethxmit(q, mp))
	    putq(q, mp);
	break;

    case DL_ATTACH_REQ:
    {
	dl_ok_ack_t *okp;
	freemsg(mp);
	if (!(mp = allocb(sizeof(dl_ok_ack_t), BPRI_MED)))
	    return 1;
	okp = (dl_ok_ack_t *)mp->b_wptr;
	mp->b_datap->db_type = M_PCPROTO;
	okp->dl_primitive = DL_OK_ACK;
	okp->dl_correct_primitive = DL_ATTACH_REQ;
	mp->b_wptr += sizeof(dl_ok_ack_t);
	zp->state = DL_UNBOUND;
	qreply(q, mp);
	return 1;
    }

    case DL_DETACH_REQ:
    {
	dl_ok_ack_t *okp;
	freemsg(mp);
	if (!(mp = allocb(sizeof(dl_ok_ack_t), BPRI_MED)))
	    return 1;
	okp = (dl_ok_ack_t *)mp->b_wptr;
	mp->b_datap->db_type = M_PCPROTO;
	okp->dl_primitive = DL_OK_ACK;
	okp->dl_correct_primitive = DL_DETACH_REQ;
	mp->b_wptr += sizeof(dl_ok_ack_t);
	zp->state = DL_UNATTACHED;
	qreply(q, mp);
	return 1;
    }

    case DL_BIND_REQ:
    {
	dl_bind_req_t *reqp = (dl_bind_req_t *)mp->b_rptr;
	dl_bind_ack_t *ackp;

	zp->sap = reqp->dl_sap;
	board->z3660eth_status.last_bound_sap = zp->sap;
	zp->state = DL_IDLE;

	freemsg(mp);
	if (!(mp = allocb(sizeof(*ackp) + ETH_ADDRESS_SIZE, BPRI_MED)))
	    return 1;
	mp->b_datap->db_type = M_PCPROTO;
	ackp = (dl_bind_ack_t *)mp->b_wptr;
	ackp->dl_primitive = DL_BIND_ACK;
	ackp->dl_sap = zp->sap;
	ackp->dl_addr_length = ETH_ADDRESS_SIZE;
	ackp->dl_addr_offset = sizeof(*ackp);
	ackp->dl_max_conind = 0;
	ackp->dl_growth = 0;
	mp->b_wptr += sizeof(*ackp);
	bcopy((caddr_t)board->z3660eth_info.paddress, (caddr_t)mp->b_wptr,
	      ETH_ADDRESS_SIZE);
	mp->b_wptr += ETH_ADDRESS_SIZE;
	qreply(q, mp);
	return 1;
    }

    case DL_UNBIND_REQ:
    {
	dl_ok_ack_t *okp;
	freemsg(mp);
	if (!(mp = allocb(sizeof(dl_ok_ack_t), BPRI_MED)))
	    return 1;
	okp = (dl_ok_ack_t *)mp->b_wptr;
	mp->b_datap->db_type = M_PCPROTO;
	okp->dl_primitive = DL_OK_ACK;
	okp->dl_correct_primitive = DL_UNBIND_REQ;
	mp->b_wptr += sizeof(dl_ok_ack_t);
	zp->state = DL_UNBOUND;
	zp->sap = 0;
	qreply(q, mp);
	return 1;
    }

    case DL_INFO_REQ:
    {
	dl_info_ack_t *ackp;
	freemsg(mp);
	if (!(mp = allocb(sizeof(dl_info_ack_t) + ETH_ADDRESS_SIZE, BPRI_MED)))
	    return 1;
	mp->b_datap->db_type = M_PCPROTO;
	ackp = (dl_info_ack_t *)mp->b_wptr;
	ackp->dl_primitive = DL_INFO_ACK;
	ackp->dl_max_sdu = ETH_MAXDATA;
	ackp->dl_min_sdu = 1;
	ackp->dl_addr_length = ETH_ADDRESS_SIZE;
	ackp->dl_mac_type = DL_ETHER;
	ackp->dl_reserved = 0;
	ackp->dl_current_state = zp->state;
	ackp->dl_max_idu = ETH_MAXDATA;
	ackp->dl_service_mode = DL_CLDLS;
	ackp->dl_qos_length = 0;
	ackp->dl_qos_offset = 0;
	ackp->dl_qos_range_length = 0;
	ackp->dl_qos_range_offset = 0;
	ackp->dl_provider_style = DL_STYLE1;
	ackp->dl_growth = 0;
	if (zp->state == DL_IDLE) {
	    ackp->dl_addr_offset = sizeof(dl_info_ack_t);
	    mp->b_wptr += sizeof(dl_info_ack_t);
	    bcopy((caddr_t)board->z3660eth_info.paddress, (caddr_t)mp->b_wptr,
		  ETH_ADDRESS_SIZE);
	    mp->b_wptr += ETH_ADDRESS_SIZE;
	} else {
	    ackp->dl_addr_offset = 0;
	    mp->b_wptr += sizeof(dl_info_ack_t);
	}
	qreply(q, mp);
	return 1;
    }

    default:
	freemsg(mp);
	return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * RX -> DLPI delivery.  SAP demux, own-MAC/broadcast filter, DL_UNITDATA_IND.
 * Copied from hydra toss_packet_up_stream (trailer branch kept but inert under
 * -trailers).  `count` is the full frame length (header + payload).
 * ------------------------------------------------------------------------- */
static void
toss_packet_up_stream(packet, board_index, count)
char *packet;
int board_index, count;
{
    int j, length, trailn;
    unsigned short sap;
    z3660eth_board_t *board = &z3660eth_board[board_index];
    z3660eth_info_t *info = &board->z3660eth_info;
    z3660eth_t *zp = board->z3660eth;
    static unsigned char broadcast_address[6] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    ether_header_t *packet_header = (ether_header_t *)packet;
    int sap_matched = 0;

    sap = packet_header->ether_type;
    board->z3660eth_status.last_rx_sap = sap;
    if (sap == ETHERTYPE_ARP) board->z3660eth_status.rx_arp_seen++;
    if (sap == ETHERTYPE_IP)  board->z3660eth_status.rx_ip_seen++;

    if (sap >= ETHERTYPE_TRAIL &&
	sap <= (unsigned short)(ETHERTYPE_TRAIL + ETHERTYPE_NTRAILER)) {
	length = (sap - ETHERTYPE_TRAIL) * 512;
	trailn = (*((unsigned short *)
		    &packet[ETH_HEADER_SIZE + length + ETH_TYPE_SIZE]) -
		  ETH_TYPE_SIZE - ETH_LENGTH_SIZE);
	sap = *((unsigned short *)&packet[ETH_HEADER_SIZE + length]);
    } else {
	length = count - ETH_HEADER_SIZE;
	trailn = 0;
    }

    for (j = 0; j < Z3660ETH_MAXDEV; j++, zp++) {
	mblk_t *mp = (mblk_t *)NULL;

	if (!zp->q)
	    continue;

	if (zp->flags & Z3660ETH_RAW_DATA) {
	    if (canput(zp->q->q_next)) {
		if ((mp = allocb(count, BPRI_MED)) != NULL) {
		    mp->b_datap->db_type = M_DATA;
		    bcopy((caddr_t)packet, (caddr_t)mp->b_wptr, count);
		    mp->b_wptr += count;
		    putnext(zp->q, mp);
		} else
		    board->z3660eth_status.allocbs_failed++;
	    } else
		board->z3660eth_status.couldnt_put++;
	} else if (zp->sap == sap) {
	    sap_matched = 1;

	    if (bcmp((char *)packet_header->ether_dhost,
		     (char *)info->paddress, ETH_ADDRESS_SIZE) &&
		bcmp((char *)packet_header->ether_dhost,
		     (char *)broadcast_address, sizeof(broadcast_address))) {
		board->z3660eth_status.rx_not_for_us++;
		continue;
	    }

	    if (canput(zp->q->q_next)) {
		if ((mp = allocb(sizeof(union DL_primitives) +
				 (2 * ETH_ADDRESS_SIZE), BPRI_MED)) != NULL &&
		    (mp->b_cont = allocb(length + trailn, BPRI_MED)) != NULL) {
		    register dl_unitdata_ind_t *dp =
			(dl_unitdata_ind_t *)mp->b_wptr;

		    dp->dl_primitive = DL_UNITDATA_IND;
		    dp->dl_dest_addr_length = ETH_ADDRESS_SIZE;
		    dp->dl_dest_addr_offset = sizeof(union DL_primitives);
		    dp->dl_src_addr_length = ETH_ADDRESS_SIZE;
		    dp->dl_src_addr_offset =
			sizeof(union DL_primitives) + ETH_ADDRESS_SIZE;
		    dp->dl_reserved = 0;

		    bcopy((caddr_t)&packet_header->ether_dhost,
			  (caddr_t)mp->b_wptr + dp->dl_dest_addr_offset,
			  ETH_ADDRESS_SIZE);
		    bcopy((caddr_t)&packet_header->ether_shost,
			  (caddr_t)mp->b_wptr + dp->dl_src_addr_offset,
			  ETH_ADDRESS_SIZE);
		    mp->b_wptr += sizeof(union DL_primitives) +
			(2 * ETH_ADDRESS_SIZE);
		    mp->b_datap->db_type = M_PROTO;

		    if (trailn) {
			bcopy((caddr_t)&packet[ETH_HEADER_SIZE + length +
					       ETH_TYPE_SIZE + ETH_LENGTH_SIZE],
			      (caddr_t)mp->b_cont->b_wptr, trailn);
			mp->b_cont->b_wptr += trailn;
		    }
		    bcopy((caddr_t)&packet[ETH_HEADER_SIZE],
			  (caddr_t)mp->b_cont->b_wptr, length);
		    mp->b_cont->b_wptr += length;
		    mp->b_cont->b_datap->db_type = M_DATA;

		    putnext(zp->q, mp);
		    board->z3660eth_status.rx_delivered++;
		} else {
		    board->z3660eth_status.allocbs_failed++;
		    if (mp)
			freemsg(mp);
		}
	    } else
		board->z3660eth_status.couldnt_put++;
	}
    }

    if (!sap_matched)
	board->z3660eth_status.rx_no_sap++;
}

/* ----------------------------------------------------------------------------
 * ioctls: SIOC*IFFLAGS ack, board count, config + status snapshots.
 * ------------------------------------------------------------------------- */
static void
z3660ethioctl(q, mp)
queue_t *q;
mblk_t *mp;
{
    register struct iocblk *iocbp = (struct iocblk *)mp->b_rptr;
    register z3660eth_t *zp = (z3660eth_t *)q->q_ptr;
    register z3660eth_board_t *board = &z3660eth_board[zp->board_index];

    /*
     * M_IOCDATA: the stream head's reply to a prior M_COPYOUT/M_COPYIN.  We only
     * ever issue M_COPYOUT (cp_private==0), so just ack the completion -- never
     * fall through to the command switch (its b_cont is NULL after a copyout, so
     * re-reading the user arg would NULL-deref).  Mirrors aen/hydra.
     */
    if (mp->b_datap->db_type == M_IOCDATA) {
	register struct copyresp *csp = (struct copyresp *)mp->b_rptr;
	if (csp->cp_rval) {		/* copyout failed */
	    freemsg(mp);
	    return;
	}
	mp->b_datap->db_type = M_IOCACK;
	freemsg(unlinkb(mp));
	iocbp->ioc_count = 0;
	iocbp->ioc_rval = 0;
	iocbp->ioc_error = 0;
	putnext(RD(q), mp);
	return;
    }

    switch (iocbp->ioc_cmd) {
    case SIOCSIFFLAGS:
    case SIOCGIFFLAGS:
	mp->b_datap->db_type = M_IOCACK;
	putnext(RD(q), mp);
	return;

    case Z3660ETH_NUMBER_OF_BOARDS:
    {
	caddr_t arg = *(caddr_t *)mp->b_cont->b_rptr;
	freemsg(mp->b_cont);
	if (!(mp->b_cont = allocb(sizeof(int), BPRI_MED))) {
	    mp->b_datap->db_type = M_IOCNAK;
	    iocbp->ioc_error = ENOMEM;
	    putnext(RD(q), mp);
	    return;
	}
	*(int *)mp->b_cont->b_rptr = z3660eth_number_of_boards;
	mp->b_cont->b_wptr += sizeof(int);
	if (iocbp->ioc_count == TRANSPARENT) {
	    struct copyreq *creq = (struct copyreq *)mp->b_rptr;
	    mp->b_datap->db_type = M_COPYOUT;
	    creq->cq_addr = arg;
	    mp->b_wptr = mp->b_rptr + sizeof *creq;
	    creq->cq_size = sizeof(int);
	    creq->cq_flag = 0;
	    creq->cq_private = (mblk_t *)0;
	} else {
	    iocbp->ioc_count = sizeof(int);
	    mp->b_datap->db_type = M_IOCACK;
	}
	putnext(RD(q), mp);
	return;
    }

    case Z3660ETH_GET_STATUS:
    {
	caddr_t arg = *(caddr_t *)mp->b_cont->b_rptr;
	board->z3660eth_status.if_ipackets = board->ifstats.ifs_ipackets;
	board->z3660eth_status.if_opackets = board->ifstats.ifs_opackets;
	board->z3660eth_status.if_ierrors = board->ifstats.ifs_ierrors;
	board->z3660eth_status.if_oerrors = board->ifstats.ifs_oerrors;
	freemsg(mp->b_cont);
	if (!(mp->b_cont = allocb(sizeof(z3660eth_status_t), BPRI_MED))) {
	    mp->b_datap->db_type = M_IOCNAK;
	    iocbp->ioc_error = ENOMEM;
	    putnext(RD(q), mp);
	    return;
	}
	*(z3660eth_status_t *)mp->b_cont->b_rptr = board->z3660eth_status;
	mp->b_cont->b_wptr += sizeof(z3660eth_status_t);
	if (iocbp->ioc_count == TRANSPARENT) {
	    struct copyreq *creq = (struct copyreq *)mp->b_rptr;
	    mp->b_datap->db_type = M_COPYOUT;
	    creq->cq_addr = arg;
	    mp->b_wptr = mp->b_rptr + sizeof *creq;
	    creq->cq_size = sizeof(z3660eth_status_t);
	    creq->cq_flag = 0;
	    creq->cq_private = (mblk_t *)0;
	} else {
	    iocbp->ioc_count = sizeof(z3660eth_status_t);
	    mp->b_datap->db_type = M_IOCACK;
	}
	putnext(RD(q), mp);
	return;
    }

    default:
	mp->b_datap->db_type = M_IOCNAK;
	iocbp->ioc_count = 0;
	iocbp->ioc_error = EINVAL;
	putnext(RD(q), mp);
	return;
    }
}

/*
 * Boot init.  NOT registered in init_tbl (lazy-open model, like aen) so the
 * GEM-less WinUAE build box boots cleanly and open() simply ENXIOs.  Provided
 * for completeness / future use.
 */
void
z3660ethinit()
{
    z3660eth_autoconfig();
}
