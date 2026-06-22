# DRAFT DESIGN (pre-review)

I have a comprehensive evidence base from the six surveys. Let me produce the authoritative design document. This is a synthesis task — the surveys already cite the source files, so I'll reconcile them into one buildable spec rather than re-reading everything.

# AMIX Native STREAMS/DLPI Ethernet Driver for the Z3660 — Design & Scoping Document (DRAFT)

Target: `z3660eth` — a native AMIX (Commodore Amiga Unix SVR4.0, m68k 68030) STREAMS/DLPI Style-1 connectionless Ethernet provider for the Z3660 accelerator's onboard GEM, spoken via the existing Z3660 firmware mailbox (the network analogue of `z3660.c` SCSI).

---

## 1. Executive Summary + Feasibility Verdict

**Verdict: FEASIBLE, and overwhelmingly AMIX-side — directly analogous to `z3660.c`.** The driver is a port, not a new design: copy the stock `aen`/`hydra` STREAMS/DLPI skeleton verbatim, delete the chip layer, and reconnect two seams to the Z3660 firmware mailbox. The firmware already moves whole Ethernet frames to/from the Zynq GEM for AmigaOS `Z3660Net.device`; the contract is OS-agnostic (Survey B §5), so **no firmware change is required** (see §3).

**Confidence: HIGH** on protocol (cross-verified guest↔firmware, Surveys A+B agree byte-for-byte on the register map, slot layout, and 4-byte header) and on the STREAMS skeleton (two working reference drivers, `aen` + `hydra`, in-tree). **MEDIUM** on first-boot integration mechanics (new kerntools STREAMS path is unbuilt; build-kernel.sh is SCSI-only). **LOW-confidence / real-HW-gated** only on the datapath and coherency (the WinUAE build box has an emulated a2065, NOT the GEM — it can prove compile/link/boot/register, never TX/RX).

**Biggest risks, ranked:**
1. **030-side cache coherency of the shared eth DDR window** (the lpsched-class trap; the one place the aen/hydra template gives NO precedent — Survey C §7, D §5.6). This is the most likely "works-then-corrupts" failure and is only resolvable on real HW.
2. **Synchronous/blocking TX** — `REG_ZZ_ETH_TX` write busy-waits in firmware up to ~1 ms (Survey A §1, B §2). A multi-ms stall inside a STREAMS write at raised spl is a latency/lock-hold hazard.
3. **RX backlog drain discipline** — firmware re-nags INT6 while backlog>0 and reuses slot addresses after drain; freshness is by **serial**, not address. Mis-drain → either missed frames or a 64-slot ring overrun into the TX window (latent firmware sizing bug, Survey A/B §3).
4. **The RX CRC/length boundary** — hydra's #1 historical bug (60-vs-64 min frame) silently killed ARP. Must confirm whether the firmware delivers frames with or without FCS.

---

## 2. The Z3660 ETH Interface AMIX Must Drive — Reconciled Protocol Contract

Surveys A (guest) and B (firmware) **agree on every value**. Below is the single authoritative spec. All offsets are relative to the Zorro III board base (`= cd->cd_BoardAddr` guest-side `= RTG_BASE 0x18000000` firmware-side). The driver gets `base` from Zorro autoconfig (mfg `0x144B`, prod `0x1` — the same board the SCSI/RTG side enumerates).

### 2.1 Register file (32-bit, big-endian, `volatile`, MMIO-emulated by firmware)
| Reg | Offset | Dir | Role |
|---|---|---|---|
| `REG_ZZ_CONFIG` | `0x104` | W | `1`=enable ETH int; `0`=disable; `8\|16`=ack+clear ETH |
| `REG_ZZ_ETH_TX` | `0x190` | R/W | W byte-length = trigger TX; R = TX result (0=ok) |
| `REG_ZZ_ETH_RX` | `0x194` | W | strobe (value ignored) = advance/free current backlog slot |
| `REG_ZZ_ETH_MAC_HI` | `0x198` | R/W | MAC[0..1] = `(b0<<8)\|b1` |
| `REG_ZZ_ETH_MAC_LO` | `0x19C` | R/W | MAC[2..5]; write triggers GEM SetMacAddress |
| `REG_ZZ_ETH_RX_ADDRESS` | `0x1A4` | R | **board-relative byte offset** of current RX slot |
| `REG_ZZ_INT_STATUS` | `0x1A8` | R | pending bits: **ETH=1**, AUDIO=2, USB=4 |

### 2.2 Frame windows (real DDR, accessed directly through the Zorro III window)
- **TX staging:** `TX_FRAME_ADDRESS = base + 0x07EE0000`. Bare Ethernet frame at offset 0 (no length/header prefix; length goes via `REG_ZZ_ETH_TX`).
- **RX backlog ring:** base `RX_BACKLOG_ADDRESS = base + 0x07ED0000`; slot stride `FRAME_SIZE = 2048`; slot N at `0x07ED0000 + N*2048`. **The driver never computes slot N itself** — it reads `REG_ZZ_ETH_RX_ADDRESS` for the current slot's offset and follows it.
- **Per-slot 4-byte header (`RX_FRAME_PAD=4`), big-endian:** `[0..1]=length` (incl. 14-byte eth header), `[2..3]=serial` (16-bit monotonic, wraps), payload at `[4..]` (`dst[6] src[6] type[2] data…`). Ethertype for demux is at slot bytes `[16..17]`.

### 2.3 TX path
Build/copy the frame into `base+0x07EE0000` (cooked: synthesize `dst|src|type` 14-byte header from DL_UNITDATA dest + own MAC + bound SAP, then payload; raw: verbatim) → write byte-length to `base+0x190` → read `base+0x190`; **nonzero = error** (1=not ready, 2=BdRingAlloc, 3=BdRingToHw, 4=TX timeout). **Synchronous/blocking** (firmware busy-waits ≤~1 ms). One shared TX buffer — no TX pipelining.

### 2.4 RX path
On INT6: read `REG_ZZ_INT_STATUS`; if `& 1` (ETH), **ack** with `REG_ZZ_CONFIG = 8|16`, then **drain loop**: read `off = REG_ZZ_ETH_RX_ADDRESS`; `slot = base+off`; `len=slot[0..1]`, `serial=slot[2..3]`; if `serial != last_serial` → demux on ethertype `slot[16..17]`, copy `len` bytes (cooked: skip 14-byte header) into an mblk, deliver, set `last_serial`; strobe `REG_ZZ_ETH_RX = 1` to advance; **repeat while serial changes / backlog non-empty** (firmware re-nags INT6 while backlog>0, so drain fully or it keeps re-firing).

### 2.5 IRQ level
**Amiga INT6** (level-6 autovector, `INTB_EXTER` chain) — the guest driver's level. **Note divergence:** the AMIX `aen`/`hydra` ISRs are wired on **INT2** (level-2, `int2_tbl[]`), and the Z3660 SCSI driver also uses INT2. **OPEN QUESTION (Q1):** does the Z3660 firmware raise the eth interrupt as Amiga INT6 (as the AmigaOS guest expects) or can/does it surface to the 030 at level 2 like the SCSI mailbox? AMIX has no `int6_tbl[]` precedent in the surveyed `kernel.c` — the surveyed interrupt-registration table is `int2_tbl[]`. This MUST be resolved against firmware `interrupt.c`/`rtg.c` and the AMIX trap vectors before wiring the ISR (see §8 Q1). The level the firmware actually asserts determines which kernel interrupt table the ISR registers in.

### 2.6 MAC source
Read 6 bytes from `REG_ZZ_ETH_MAC_HI`/`_LO` (firmware-owned; default Commodore OUI `00:80:10:00:01:00`). Optionally write back to re-apply. This is the station address advertised in `DL_INFO_ACK`/`DL_BIND_ACK`. Pack: `_HI=(m0<<8)|m1`, `_LO=(m2<<24)|(m3<<16)|(m4<<8)|m5`.

### 2.7 Cache policy
Firmware maps the whole eth window (`0x07E00000–0x08000000`) **Normal Non-Cacheable** on the ARM side and does **no 030 cache maintenance**. The 030 is a separate cache domain — coherency of these windows is **entirely the AMIX driver's responsibility** (§5).

### 2.8 A-vs-B mismatches (explicit)
- **None on values.** Every register, offset, slot stride, header field, and IRQ semantic cross-checks between guest (A) and firmware (B).
- **One latent firmware inconsistency (not a driver concern):** `FRAME_MAX_BACKLOG=64` × 2048 = 128 KB, but the map comment says "32×2048" (64 KB) and `TX_FRAME_ADDRESS` starts exactly 64 KB above the ring base — a 64-deep undrained backlog would overrun into the TX window. The guest never sizes the ring; mitigation is **drain RX eagerly** (we do).
- **Multicast/promiscuous:** **no hardware filter register exists** in this protocol (A §5). `DL_ENABMULTI_REQ`/`DL_PROMISCON_REQ` cannot be honored at the hardware level — rely on whatever the firmware GEM passes (likely broadcast+own-MAC), or NAK. Open question Q5.

---

## 3. Firmware-Change Decision

**No firmware change is required for the eth phase.** Evidence (Survey B §5): every firmware path keyed on the eth registers (`rtg.c:803-809, 1609-1646`; `ethernet.c` TX/RX) operates purely on register values + shared-buffer contents. There is **no branch on guest OS**, no `a3000_amix_mode`-style flag, no AmigaOS assumption anywhere in `ethernet.c` or the eth dispatch. The firmware copies opaque payload bytes, takes the MAC from `0x198/0x19C`, and shares only an OS-neutral serial counter + backlog cursor. An AMIX driver reproducing the same register handshake + buffer/header layout is served identically — exactly the SCSI story (`z3660.c` reusing the piscsi mailbox unchanged).

**One non-blocking caveat (not a change):** TX blocks the ARM ≤~1 ms inside the register store. Design the AMIX TX to tolerate the stall (don't hold high spl / contended locks across it). This is a driver-side design constraint, not a firmware modification.

**Contingent change (only if Q1 resolves badly):** if the firmware asserts the eth interrupt at a level AMIX cannot cleanly service (and AMIX truly has no level-6 autovector hook), a firmware tweak to route eth notification to the same level as the SCSI mailbox would be the fallback — but this is **speculative**; investigate AMIX's INT6 handling first.

---

## 4. Driver Architecture — The Seam

**Principle (Surveys C/D):** keep the STREAMS/DLPI/`ifstats` machinery **byte-for-byte** from `hydra.c` (the cleaner of the two precedents — self-contained `init_tbl` probe); replace only the chip layer with the Z3660 mailbox datapath. Copy `hydra.c` → `z3660eth.c`, rename `hydra`→`z3660eth`.

### 4.1 Data structures (reuse, rename)
- `module_info z3660eth_minfo` (tag e.g. `'ze'`, name `"z3660eth"`, MIN/MAX/HIWAT=5120/LOWAT).
- `qinit z3660ethrinit = {NULL,NULL, z3660ethopen, z3660ethclose, NULL, &minfo, NULL}` (read side: open/close; **no rput/rsrv** — RX is `putnext` from interrupt).
- `qinit z3660ethwinit = {z3660ethwput, z3660ethwsrv, NULL,NULL,NULL, &minfo, NULL}`.
- `struct streamtab z3660ethinfo = {&z3660ethrinit, &z3660ethwinit, NULL, NULL}` — **this symbol goes in `cdevsw[].d_str`**.
- Per-stream softc `z3660eth_t` (`q`, `state` DL_UNBOUND/DL_IDLE, `board_index`, `flags` incl. RAW, `sap`).
- Per-board softc — **rewrite the hardware half**: replace LANCE/NE2000 ring pointers with `volatile uchar *base` (Zorro autoconfig base), derived register pointers (`base+0x104/0x190/0x194/0x198/0x19C/0x1A4/0x1A8`), `volatile uchar *txwin` (`base+0x07EE0000`), `volatile uchar *rxring` (`base+0x07ED0000`), `ushort last_serial`, `uchar paddress[6]`, board state.
- Aggregate `z3660eth_board[MAXBOARDS]` (MAXBOARDS=1 realistically) each containing the stream array, `aen_status`-style stats, hw softc, and `struct ifstats` (the IP-visible interface node — spliced into the global `ifstats` list on first open; there is NO BSD `ifnet`/`if_attach` in SVR4.0).

### 4.2 Entry points → kernel table
| Function | Role | Kernel table |
|---|---|---|
| `z3660ethopen/close` | minor→board/sap decode; lazy init on first open; `ifstats` splice; stop GEM on last close | (read qinit → `cdevsw[].d_str`) |
| `z3660ethwput` | write put: M_FLUSH / M_PROTO→proto / M_IOCTL→ioctl | (write qinit) |
| `z3660ethwsrv` | no-op stub (enables flow control) | (write qinit) |
| `z3660ethproto` | DLPI engine (§4.3) — **reuse almost verbatim** | (called from wput) |
| `z3660ethioctl` | SIOCS/GIFFLAGS ACK, board-count/config/status | (called from wput) |
| `z3660ethxmit` | TX (§4.4) — **rewrite hw half** | (called from proto) |
| `z3660ethintr` | ISR (§4.5) — **rewrite hw half** | **`int2_tbl[]`** (pending Q1) |
| `z3660ethinit` | boot probe → autoconfig; arm board state | **`init_tbl[]`** (mirror `hydrainit`) |
| `toss_packet_up_stream` | RX→DLPI: SAP demux + DL_UNITDATA_IND build + `putnext` — **reuse almost verbatim** | (called from ISR) |
| `transmit_interrupt` | TX-complete requeue (getq/xmit/putbq) — reuse structure | (called from ISR) |

### 4.3 DLPI primitives (irreducible set for ARP+ping)
- **DL_INFO_REQ → DL_INFO_ACK:** `dl_mac_type=DL_ETHER`, `dl_service_mode=DL_CLDLS`, `dl_provider_style=DL_STYLE1`, `dl_max_sdu=1500`, `dl_min_sdu=1`, `dl_addr_length=6`, MAC at `dl_addr_offset`.
- **DL_BIND_REQ → DL_BIND_ACK:** `sap = dl_sap` (the ethertype; ARP binds 0x0806, IP 0x0800), `state=DL_IDLE`, echo SAP+MAC.
- **DL_UNITDATA_REQ → `z3660ethxmit`** (TX).
- **DL_UNBIND_REQ → DL_OK_ACK.**
- Consider adding **DL_PHYS_ADDR_REQ** and real **DL_ENABMULTI/DL_PROMISCON** (aen/hydra bolt multicast on via private ioctl; cleaner to do proper primitives — but hardware can't honor multicast filtering here, so they'd be best-effort/NAK).

### 4.4 TX flow (DL_UNITDATA_REQ → wire)
1. Gate: is the single TX window free? (We have one shared `txwin`; serialize behind the write queue.) If busy → `putq` (deferred, flow-controlled).
2. Assemble frame **directly into `txwin` (`base+0x07EE0000`)**: dest MAC from `mp->b_rptr + dl_dest_addr_offset`; src MAC = `paddress`; ethertype = bound `sap`; then `bcopy` payload walking `mp->b_cont` (cap `ETH_MAXDATA`=1500). `freemsg`. Pad to min frame if short (boundary per Q4).
3. **Trigger:** write byte-length to `REG_ZZ_ETH_TX` (`base+0x190`); **read it back** for rc; nonzero → `oerrors`++, map to failure. Bump `opackets`/`ifs_opackets`.
4. **Blocking concern:** because the firmware busy-waits ≤~1 ms, do NOT hold raised spl across the trigger; consider a TX-serialization lock and returning the calling context promptly. (Design decision Q2.)

### 4.5 RX flow (INT6/INT2 → up the stream)
ISR (`z3660ethintr`): `if (panicstr) return`; per-board RUNNING gate; read `REG_ZZ_INT_STATUS`; if `& 1` → ack `REG_ZZ_CONFIG=8|16` → `receive_interrupt(board)`. **`receive_interrupt` rewrite:** loop with a `loop_count` guard (mirror hydra's 256-cap) — read `off=REG_ZZ_ETH_RX_ADDRESS`; `slot=base+off`; read `serial=slot[2..3]`; **if `serial==last_serial` break** (ring drained); else `len=slot[0..1]`, copy `len` bytes from `slot+4` into the static `rxbuff[]`, `last_serial=serial`, `REG_ZZ_ETH_RX=1` (advance), then fall through to `toss_packet_up_stream(rxbuff, board, len)`. **`toss_packet_up_stream` reuse almost verbatim:** read ethertype as SAP, (trailer decode — moot with `-trailers`), iterate streams, RAW streams get whole frame, else `sap`-matched streams get a `DL_UNITDATA_IND` (allocb header+data, fill dest/src MAC + payload, `canput` check, `putnext`). Broadcast (dst all-FF) flows naturally to the SAP-matched stream.

### 4.6 Init / probe (and the WinUAE ENXIO)
- `z3660ethinit` (in `init_tbl[]`) → autoconfig: find Zorro board mfg `0x144B`/prod `0x1` (reuse the **proven Z3660 SCSI discovery**, NOT raw `autocon()` — hydra §5.4 warns autocon corrupts; the board is the same one SCSI already enumerates). Read MAC from `0x198/0x19C`. Arm board state RUNNING; enable INT via `REG_ZZ_CONFIG=1`.
- **On the WinUAE build box** the Z3660 board is absent (it emulates an a2065), so autoconfig finds nothing → board count 0 → `open` returns **ENXIO** (exactly like `z3660.c` SCSI ENXIO'ing on the build box). This is the safe-to-test property: compile/link/boot/register succeed; the datapath simply has no board to drive. The `-S` presence gate returns "no board," so boot won't hang.

---

## 5. Cache-Coherency / MMU Handling (first-class concern)

**The core problem:** the firmware maps the eth window non-cacheable on the **ARM/Zynq** side and does zero **030** cache maintenance (Survey B §4). The 030 is an independent cache domain reaching these buffers through the Zorro III window. Correctness depends entirely on the **030 side** not caching the TX/RX windows and the register file, and on ordering the handshakes. This is the same class as the SCSI bounce-buffer trouble and the open "lpsched Bad vp" bug — and it is **the one place aen/hydra give no precedent** (their LANCE/NE2000 buffers are Zorro II MMIO, inherently uncached on the 030; they never had to think about it — Survey C §7, D §5.6).

**Required handling (specific):**
1. **Map the board window cache-inhibited in the 030 MMU.** The register file page (`base+0x100…`) and the frame windows (`base+0x07ED0000 … 0x07F00000`, i.e. the RX ring + TX staging) must be **CI (cache-inhibited)/serialized** in the AMIX 030 page tables. The AmigaOS guest works precisely because Zorro III space is CI in its MMU map (and it could afford a commented-out `CacheClearE` — A §6). **First action item:** confirm how AMIX maps Zorro III board space — is the whole Z3660 window already CI (it must be, for the SCSI mailbox to work), or does eth land in a cached range? **Mirror exactly what `z3660.c` does for its mailbox** — that mailbox is the proven-coherent precedent on this very board (Survey D §5.6 explicitly says: follow `z3660.c`, not hydra).
2. **Declare every shared access `volatile`** (registers and slot fields), as aen/hydra do for all descriptor/register access — load-bearing.
3. **Ordering:** the 030 lacks a `dsb`. With a strict CI mapping, ordering is implicit (CI accesses are serialized). The critical orderings are: TX = "fill `txwin` fully → write length to `0x190`"; RX = "read `0x1A4` → read slot header(serial) → read payload → strobe `0x194`". If the window is genuinely CI these are naturally ordered; if any caching slips in, add `cpushl`/`cinv` (the 030 cache-push/invalidate) around each handshake. **Do NOT rely on a `dsb`-style barrier** — the paused lpsched investigation's key finding (MEMORY) is that the dsb-acquire-barrier was a Heisenbug artifact, not a fix; the real fix is the non-cached/serialized mapping (the `wip-emulated-ram` insight: bounce buffers in mobo RAM were outside the ARM coherent domain).
4. **Avoid a cached staging bounce on the hot path where possible.** aen/hydra reassemble through a cached `rxbuff[]`; for Z3660 the safest model is to `bcopy` straight from the CI RX slot into the allocb'd mblk (the mblk is cached kernel memory, but it's never touched by hardware — only the CI shared slot is the coherency frontier). Decide whether to keep the `rxbuff[]` staging or read the slot directly (Q3).
5. **Verification is real-HW-only.** The WinUAE box cannot exercise this (no GEM). Plan an explicit coherency soak on real HW (sustained TX+RX under load, byte-verify) before declaring the datapath sound — directly analogous to the SCSI `[WVERIFY]` campaign.

---

## 6. Kernel Integration + Kerntools (build-kernel.sh STREAMS path)

### 6.1 Exact `master.d/kernel.c` edits (three insertions + externs; mirror `hydra*`)
1. **cdevsw row** — pick a **free major** (slots 48–69 are empty `nostr`; e.g. **48**). Replace that slot's `…,nostr,nullflag,/*48*/` with:
   `ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&z3660ethinfo,nullflag, /*48=z3660eth*/`
   and add `extern struct streamtab z3660ethinfo;` after `kernel.c:266` (`extern struct streamtab hydrainfo;`).
2. **int2_tbl entry** (pending Q1) — add `z3660ethintr,` before the terminating `0};` (after `hydraintr,` at `kernel.c:194`), and append `z3660ethintr` to the extern at `kernel.c:187`.
3. **init_tbl entry** — add `z3660ethinit,` before `0};` (after `hydrainit,` at `kernel.c:61`), and add `z3660ethinit` to the extern at `kernel.c:38`.

No `fmodsw[]`, no `unix.c`, no second registry — the major↔streamtab binding is purely the cdevsw slot position (Survey E §4).

### 6.2 Driver subdir + Makefile
New `amiga/driver/z3660eth/Makefile` (mirror `hydra/Makefile`, note the deeper include path):
```
CFLAGS = -O -D_KERNEL -DSVR40 -DSVR4 -I../../.. -I../../inc
OBJ    = z3660eth.o
exp:   $(OBJ)
	ld -r -o exp $(OBJ)
clean:
	-rm -f $(OBJ) exp
z3660eth.o : z3660eth.h
```
Plus two insertions in `amiga/driver/Makefile` (anchor on the `hydra/exp` lines): add `  z3660eth/exp \` to `OBJ`, and a `z3660eth/exp :\n\tcd z3660eth; $(MAKE)` rule.

### 6.3 build-kernel.sh STREAMS-path extension (concrete spec — Survey E §5)
The SCSI path generates `scsicard[]` rows into `alien/sd.c` and never touches `kernel.c`; STREAMS cannot reuse it. Add a typed `driver.conf` stanza:
```
net  z3660eth.c  major=48  streamtab=z3660ethinfo  init=z3660ethinit  intr=z3660ethintr  "Z3660 Ethernet"
```
New build-kernel.sh steps (delta):
1. Parse `net` stanzas (fields: src, major, streamtab/init/intr symbols, name); keep `scsi` stanzas on the existing path.
2. Render a new `templates/net-driver-Makefile.in` → `work/z3660eth/Makefile`.
3. Pull pristine `kernel.c` + `driver/Makefile` (or keep known-good copies); apply the **three anchored, idempotent** `kernel.c` insertions (anchor on the unique `hydra*` lines; guard with a `/* z3660eth */` marker so re-runs don't double-insert) + the two `driver/Makefile` insertions.
4. Push: driver source(+headers) → `amiga/driver/z3660eth/`; generated subdir Makefile → same; patched `kernel.c` → `master.d/`; patched `driver/Makefile` → `amiga/driver/` (reuse `amixsync.py push`, new remote paths).
5. Run the **unchanged** clean-gate (`build-clean-kernel.sh`: `cd /usr/sys; make` loop until `sum -r relocunix` recurs, then `checkunix`) — **but extend its force-touch/rm set** to include `master.d/kernel.c` (so the wiring recompiles) and `amiga/driver/z3660eth/z3660eth.c` (the subdir, not `alien/`). The gate's stabilization/sum-recurrence/`checkunix` logic is reused verbatim.

---

## 7. Userland Bring-up (real HW @ 192.168.2.39)

Assume major **48**, interface name **`zen0`** (or `z3660net0`). `slink addaen` is **generic** ("link an Ethernet DLPI driver under IP+ARP") — hydra reuses it verbatim; we do too. SVR4.0 has **no `plumb`** and **literal IP only** (never `\`uname -n\`` — the ~90 s DNS-stall trap).

**One-shot (single-user/testing):**
```sh
mknod /dev/zen0 c 48 0
/usr/sbin/slink                                  # if boot didn't: strcf boot{} (tcp/udp/icmp/rawip + loopback)
/usr/sbin/slink addaen /dev/zen0 zen0            # links driver under /dev/ip (with 'app' pushed) AND /dev/arp
/usr/sbin/ifconfig zen0 192.168.2.39 netmask 255.255.255.0 up -trailers
/usr/sbin/route add default 192.168.2.1 1        # metric 1 MANDATORY (not a flag)
ping 192.168.2.1
```

**Persistent `/etc/inet` config:**
- `/etc/inet/network-config` — add (presence-gated, LITERAL IP):
  ```sh
  /usr/amiga/bin/zen -S &&
      /usr/sbin/slink addaen /dev/zen0 zen0 &&
          /usr/sbin/ifconfig zen0 192.168.2.39 netmask 255.255.255.0 up -trailers
  ```
  (If we don't ship a `-S` presence tool, make the first command the `slink` line. **Recommended: ship a tiny `zen -S` tool** backed by a board-count ioctl — on the WinUAE box it correctly returns "no board" so boot doesn't hang.) **Edit `S69inet`/`inetinit` in place — it's a hardlink pair; never unlink/recreate, never park an `S*` backup in `rc2.d/`.**
- `/etc/inet/hosts` — add `192.168.2.39 amix-z3660 amix-z3660.alanara.fi`.
- `/etc/inet/rc.inet` — default route already present; keep.
- **Device node** persists on the on-disk root `/dev` — `mknod` once. No `strcf` change needed (`addaen` is generic).

---

## 8. Risks & Open Questions (ranked; ★ = needs real HW)

1. **★ Q-COHERENCY (highest): 030-side cache coherency of the shared eth DDR window.** The lpsched-class trap; no aen/hydra precedent. **Resolution:** confirm AMIX maps the Z3660 Zorro III window CI; mirror `z3660.c`'s mailbox handling exactly; coherency soak on real HW. The single most likely source of a "works-then-corrupts" bug.
2. **Q1: What interrupt level does the firmware actually assert for eth — INT6 (guest expectation) or something AMIX services via `int2_tbl[]`?** Determines the kernel table and whether any firmware/vector work is needed. **Resolvable from source** (firmware `interrupt.c`/`rtg.c` + AMIX trap vectors) — do this BEFORE wiring the ISR. *(Not real-HW-gated to answer; real-HW-gated to confirm.)*
3. **★ Q4: Does the firmware deliver RX frames with or without the FCS/CRC?** Sets the RX length/min-frame boundary (hydra's 60-vs-64 bug silently killed ARP). **Resolvable partly from firmware `ethernet.c`** (`XEmacPs_BdGetLength` semantics), confirmed on real HW with a packet-length probe.
4. **Q2: Blocking-TX design.** ~1 ms firmware busy-wait inside `REG_ZZ_ETH_TX` write. Decide spl/lock discipline (serialize TX behind the write queue; don't hold high spl). Design-time; validate latency on real HW.
5. **★ Q3: RX staging — cached `rxbuff[]` vs direct CI-slot read into mblk.** Affects coherency and performance. Lean toward direct read from the CI slot. Validate on real HW.
6. **Q5: Multicast/promiscuous.** No hardware filter register exists. Decide: best-effort (rely on firmware GEM passing broadcast+own-MAC) vs NAK `DL_ENABMULTI`/`DL_PROMISCON`. For ARP+ping+single-host this is moot (broadcast flows); matters for multicast-dependent services.
7. **Q6: RX backlog overrun.** 64-slot ring (128 KB) overruns into TX window at `0x07EE0000` (latent firmware sizing bug). Mitigation = eager drain (we do). Low risk unless RX is starved; flag for the coherency soak.
8. **★ Q7: Single shared TX buffer under STREAMS concurrency.** One `txwin` — must serialize. Validate no two contexts race the buffer.
9. **Q8: `/dev` node minor convention** — confirm aen's actual node/minor on the golden image (the golden mount was I/O-locked this session; verify on-box `ls -l /dev`).

---

## 9. Phased Implementation Plan

Deploy routine (LOCKED, two-tier): **build + clean-gate on WinUAE `.38`** → shuttle clean `relocunix` via `transfer.hdf` (`c5d0`, ~1 min) → **real box** `make install` + `/stand make` → reboot. Real-HW iface = `192.168.2.39`; never re-image root.

**Phase 0 — Resolve source-answerable open questions (no build).**
- Resolve **Q1** (interrupt level — firmware `interrupt.c`/`rtg.c` vs AMIX vectors) and **Q4** (FCS/CRC in RX length — firmware `ethernet.c`). Confirm AMIX Zorro III window cache policy + read `z3660.c`'s mailbox coherency handling (Q-COHERENCY prep). Confirm free cdevsw major + golden `/dev` minor convention (Q8).
- **Deliverable:** a 1-page "resolved contract" appendix fixing the IRQ table, the RX length boundary, and the coherency mapping approach. *Gate to Phase 1.*

**Phase 1 — Skeleton that compiles, links, boots, and ENXIOs (WinUAE-only).**
- Copy `hydra.c`→`z3660eth.c`, rename, **stub the two seams** (TX/RX return not-present), keep all STREAMS/DLPI/`ifstats`/`toss_packet_up_stream` machinery. Write the autoconfig probe (reuse Z3660 SCSI discovery). Build the kerntools STREAMS path (§6.3): new `driver.conf net` stanza, `net-driver-Makefile.in`, the three anchored `kernel.c` edits + two `driver/Makefile` edits, push to `amiga/driver/z3660eth/`.
- **Build/test loop:** WinUAE clean-gate (relink until `sum -r` recurs) + boot. **Gate = boots without panic, registers in cdevsw, `open` returns ENXIO** (no board, exactly like z3660.c). This proves the entire integration/kerntools path independent of the datapath.
- **First deliverable (the milestone):** a clean-gated `relocunix` that boots on WinUAE with `z3660eth` registered and ENXIO-ing — the kerntools STREAMS path proven end-to-end.

**Phase 2 — Fill the TX seam; first frame on real HW.**
- Implement `z3660ethxmit`: assemble into `txwin`, trigger `REG_ZZ_ETH_TX`, read rc; apply Q2 spl discipline and the CI mapping from Phase 0. Implement MAC read at probe.
- **Loop:** WinUAE compile/boot gate (TX path still ENXIOs there) → shuttle to real box. **Real-HW test:** `slink addaen`+`ifconfig zen0 192.168.2.39 up -trailers`; send broadcast ARP / a crafted frame; observe on the wire (another host / switch capture).
- **Deliverable:** verified TX of a frame from real HW onto the wire.

**Phase 3 — Fill the RX seam; ARP + ping.**
- Implement `z3660ethintr` + rewritten `receive_interrupt` (serial-based drain loop, ack `8|16`, advance `0x194`), reuse `toss_packet_up_stream`. Wire the ISR per Q1's resolved level.
- **Loop:** WinUAE compile/boot gate → real box. **Real-HW test:** `ping 192.168.2.1` (ARP must complete — watch the Q4 min-frame boundary); bidirectional ping; route add default.
- **Deliverable:** working ARP + ICMP both directions on real HW @ 192.168.2.39.

**Phase 4 — Coherency soak + hardening.**
- Sustained TX+RX under load with byte-verification (the eth analogue of the SCSI `[WVERIFY]` campaign) — the only way to clear Q-COHERENCY, Q3, Q6, Q7. Resolve multicast/promisc policy (Q5). Persistent `/etc/inet` config + `zen -S` presence tool. Tune spl/flow-control.
- **Deliverable:** a stable, persistent, reboot-surviving network interface, soak-clean for coherency — ready to commit on a `wip-ethernet` branch off `amix-base`.

**Critical-path note:** Phases 2–4 are **real-HW-gated** (WinUAE has no GEM). Front-load every source-answerable question into Phase 0/1 so real-HW sessions (serial+KVM, no network until this works) are spent purely on the datapath and coherency, not on integration mechanics.

---

### Sources (file:line via Surveys A–F)
Guest protocol: `z3660-drivers/eth/device.c` (78-92, 161-211, 495-850), `common/z3660_regs.h` (42-93, 278). Firmware: `rtg/rtg.c` (327-336, 788-809, 1150-1173, 1609-1646), `ethernet.c` (59, 510-668, 1028-1095), `memorymap.h` (16, 30-40), `main.c` (683-692), `Z3660_emu/src/defines.h` (2-15), `interrupt.{c,h}`. Skeleton/wiring: `hydra-amix/usr/sys/amiga/driver/aen/aen.c`, `…/hydra/hydra.c`, `…/master.d/kernel.c` (38, 61, 187, 194, 265-266, 357, 397), `…/driver/Makefile`, `…/driver/{aen,hydra}/Makefile`. Userland: `/mnt/etc/init.d/inetinit`, `/mnt/etc/inet/{network-config, strcf:87-94, rc.inet:42, hosts}`. Kerntools: `amix-kerntools/{build-kernel.sh, templates/, tools/build-clean-kernel.sh}`; SCSI contrast `amix-z3660/{src/z3660.c, driver.conf}`.


# ADVERSARIAL REVIEW (4 lenses)

## LENS — protocol-correctness
I now have full verification. Let me write up the adversarial review.

# ADVERSARIAL REVIEW — Protocol-Correctness Lens
## AMIX Z3660 Ethernet Driver Design vs. actual guest + firmware source

I re-read the actual guest (`z3660-drivers/eth/device.c`, `common/z3660_regs.h`) and firmware (`Z3660/src/ethernet.c`, `rtg/rtg.c`, `interrupt.{c,h}`, `Z3660_emu/src/memorymap.h`) source. The design's register map, offsets, header format, and IRQ semantics are **almost entirely correct** — but there are **two genuine correctness bugs** in the design's RX model that would break receive, plus several incompletenesses. Details below.

---

## BLOCKER-class findings

### B1. The design's "drain loop" RX model does NOT match the firmware/guest contract — one strobe frees one slot, and `REG_ZZ_ETH_RX_ADDRESS` does NOT re-point until after the strobe

**Design claim** (§2.4, §4.5): "drain loop: read `off = REG_ZZ_ETH_RX_ADDRESS`; … if `serial != last_serial` → … deliver … strobe `REG_ZZ_ETH_RX = 1` to advance; **repeat while serial changes / backlog non-empty**." And §4.5: "loop with a `loop_count` guard (mirror hydra's 256-cap) — read `off=REG_ZZ_ETH_RX_ADDRESS`; … read `serial`; if `serial==last_serial` break."

**Why it's wrong / dangerously incomplete:** The design conflates "loop reading the *same* slot" with "advance through the ring." In the actual contract, the consumer cursor only moves **when you strobe `REG_ZZ_ETH_RX`**:

- `ethernet_current_receive_ptr()` returns `RX_BACKLOG_ADDRESS + frames_received_from_backlog*FRAME_SIZE` (`ethernet.c:596-597`). `frames_received_from_backlog` is **only** incremented inside `ethernet_receive_frame()`, which is **only** called on the `REG_ZZ_ETH_RX` write (`rtg.c:1613`, `ethernet.c:614`).
- So within one ISR pass, if you read `0x1A4` → process → strobe `0x194` → read `0x1A4` again, the *second* read now points at the **next** slot (cursor advanced). The design's loop **must re-read `0x1A4` after every strobe** and recompute `slot = base+off`. The design text says "read `off=REG_ZZ_ETH_RX_ADDRESS`" once at the top of the loop body in §4.5's pseudocode but then describes the break test as `serial==last_serial` on a slot it doesn't re-fetch. If implemented literally (read off once, loop on serial), it reads the **same** slot forever → either spins or under-drains. The loop body **must** be: `off=read(0x1A4); slot=base+off; serial=slot[2..3]; if serial==last break; process; last=serial; strobe(0x194)` — i.e. re-read `0x1A4` each iteration. **The design's §2.4 wording is right ("read off … strobe … repeat"); §4.5's pseudocode drops the re-read and is the trap.**

**Worse — the reference guest does NOT loop at all.** `frame_proc` (`device.c:784-855`) processes **exactly one** frame per interrupt signal, then `Wait()`s again (`device.c:850` is inside the `if(serial!=old_serial)` block; the unconditional `recv = Wait(wmask)` at `device.c:851-854` runs every pass). It relies entirely on the firmware **re-nagging INT6 while `backlog>0`** (`rtg.c:327-336`) to come back for the next frame. So the design's "drain fully or it keeps re-firing" is *backwards from how the reference actually behaves* — the reference deliberately does **one-per-IRQ + re-nag**. An AMIX ISR that loop-drains is *also* valid (and better), but **only if it re-reads `0x1A4` each pass** (per above). The design should explicitly pick one model; as written it mixes both and the §4.5 pseudocode would wedge.

**Corrected fact:** RX cursor advances solely on `REG_ZZ_ETH_RX` strobe (`ethernet.c:604-639`, `rtg.c:1613`); `REG_ZZ_ETH_RX_ADDRESS` must be **re-read after each strobe** (`device.c:796-799` re-reads at the top of every `frame_proc` pass). **Severity: BLOCKER** — literal implementation of §4.5 either under-drains or spins.

---

### B2. The firmware's serial fix-up means `last_serial`-based drain detection has a real corner case the design ignores

**Design claim** (§2.2, §2.4): "freshness is by **serial**, not address … if `serial != last_serial` → deliver."

**Why it's incomplete:** When the consumer catches up (`frames_received_from_backlog >= frames_backlog`), the firmware resets the ring AND **copies the just-consumed slot's serial into the now-current (slot 0) position** so the guest won't re-process it (`ethernet.c:619-629`):
```c
frames_backlog = 0; frames_received_from_backlog = 0;
uint8_t old_serial_0=frm[2]; uint8_t old_serial_1=frm[3];
frm = ethernet_current_receive_ptr()+RTG_BASE;   // now slot 0
frm[2]=old_serial_0; frm[3]=old_serial_1;          // stamp slot 0 with last serial
```
This is load-bearing: after the last frame of a burst is strobed, `0x1A4` points back at slot 0, whose serial has been **forced equal to the last-processed serial**. So `serial == last_serial` is the *intended* "ring empty" signal — the design's break condition is correct **only because of this firmware hack**. The design must state this dependency: the empty-ring sentinel is "current slot's serial == last processed serial," produced by the firmware's stamp, **not** an inherently empty slot. A driver that, say, tracks `frames_received` counts or assumes a cleared slot will misbehave. Also note the 16-bit serial **wraps** (`frame_serial` is incremented freely, `ethernet.c:536`); `last_serial==0` as an init value collides with the very first real serial only if the firmware ever emits serial 0 — the guest inits `old_serial=0` (`device.c:777`) and the firmware's first `frame_serial++` yields 1, so 0 is a safe sentinel. The design's softc uses `ushort last_serial` (correct width) but should init to 0 and document the wrap + stamp behavior. **Severity: BLOCKER** (for a correct drain loop) — the break condition's correctness is non-obvious and undocumented.

---

## IMPORTANT findings

### I1. `REG_ZZ_CONFIG` ack semantics: the design's "`8|16` = ack+clear" is right, but **enable and ack are mutually exclusive in firmware**, and the design's enable value omits that nuance

**Design claim** (§2.1 table): "`REG_ZZ_CONFIG 0x104 W: 1=enable ETH int; 0=disable; 8|16=ack+clear ETH`."

**Firmware reality** (`rtg.c:1150-1173`): the handler branches on `zdata & 8`:
- if `zdata & 8` → it's an **ack/clear** path: `&16`→clear ETH, `&32`→clear AUDIO, `&64`→clear USB. **The `&1` enable bit is NOT examined in this branch.**
- else (no bit 8) → `interrupt_enabled_ethernet = zdata & 1`; and **writing 0 here ALSO clears the ETH interrupt** (`if(!interrupt_enabled_ethernet) amiga_interrupt_clear(...)`, `rtg.c:1166-1168`).

So: you cannot enable and ack in the same write. The values `1` (enable), `0` (disable+clear), `8|16`=`24` (ack ETH) are each correct, but the design's table presenting them as a flat bit-OR menu invites a buggy `1|8|16` write that would be taken as an ack-only (bit 8 set) and **silently fail to enable**. The design should state: **enable = write exactly `1`; ack = write exactly `24` (`8|16`); these are distinct transactions.** Also note every CONFIG write re-publishes the status word to the window (`rtg.c:1172`), which is a useful coherency property to mention. **Severity: IMPORTANT** — mis-encoding the enable/ack write is an easy, silent bug. Values themselves cross-check vs guest (`device.c:319` writes `1`, `device.c:85` writes `8|16`, `device.c:386` writes `0`).

### I2. TX result code `1` meaning is mis-described ("not ready") — it specifically means the GEM link/task is not up

**Design claim** (§2.3): "nonzero = error (1=not ready, 2=BdRingAlloc, 3=BdRingToHw, 4=TX timeout)."

**Firmware** (`ethernet.c:1031-1034`): `if (ethernet_task_state != ETH_TASK_READY) return(1);`. So `1` = **the ethernet task isn't READY** (GEM not initialized / link not up), which on AMIX will be the **normal steady state until the GEM has a link**. This matters for bring-up: the design's TX path should treat rc==1 as "interface not ready / no carrier" (retry/queue), distinctly from rc 2/3/4 (resource/HW errors → `oerrors`). Codes 2/3/4 confirmed at `ethernet.c:1053-1058`, `:1066-1070`, `:1085-1088`. **Severity: IMPORTANT** — affects how TX errors map to DLPI/`ifstats` and whether early frames are dropped vs queued.

### I3. The design does not flag that the firmware's RX producer copies from `RxFrame` into the backlog, but the backlog base used is `RTG_BASE+RX_BACKLOG_ADDRESS` — the guest's `0x1A4` value is backlog-relative, confirmed, but the design's "slot N at `0x07ED0000 + N*2048`" is only true for the firmware's internal indexing, not a guest computation

**Design claim** (§2.2): "RX backlog ring: base `RX_BACKLOG_ADDRESS = base + 0x07ED0000`; slot N at `0x07ED0000 + N*2048`. **The driver never computes slot N itself.**"

**Verification:** Correct and the caveat is right. `ethernet_current_receive_ptr()` returns `RX_BACKLOG_ADDRESS + frames_received_from_backlog*FRAME_SIZE` **without RTG_BASE** (`ethernet.c:596-597`), and `REG_ZZ_ETH_RX_ADDRESS` read returns exactly that (`rtg.c:807`). The guest adds its board base: `frm = ZZ3660_REGS + address_of_rx_buff` (`device.c:796-799`). The producer writes into `RTG_BASE+RX_BACKLOG_ADDRESS+frames_backlog*FRAME_SIZE` (`ethernet.c:551`). **This section is correct.** I flag only that the design's "slot N at 0x07ED0000+N*2048" could mislead an implementer into computing slots directly; the design already says don't — good. **Severity: MINOR/OK** (correct; phrasing risk only).

### I4. RX length field includes the 14-byte ethernet header but the FCS question (design Q4) is *answerable from source*, and the answer is: **NO FCS** in the delivered length

**Design claim** (§8 Q4, ★): "Does the firmware deliver RX frames with or without the FCS/CRC? … hydra's 60-vs-64 bug … Resolvable partly from firmware `ethernet.c`."

**Source answer (the design left this open but it is determinable):** `rx_bytes = XEmacPs_BdGetLength(cur_bd_ptr)` (`ethernet.c:540`) and that exact byte count is `memcpy`'d into the slot and stored as the length header (`ethernet.c:551-559`). On the Zynq GEM (XEmacPs), `XEmacPs_BdGetLength` returns the **received frame length excluding FCS** when FCS-strip is enabled (GEM default for normal RX BDs — the 4-byte CRC is not DMA'd to the buffer). The guest corroborates: it uses `size` directly as the SANA-II `ios2_DataLength` (`device.c:633-643`) and computes `datasize = size - 14` for cooked, with **no `-4` CRC subtraction anywhere** in `read_frame` (`device.c:622-691`). If the length included FCS, every cooked frame handed to AmigaOS would carry 4 trailing garbage bytes — it doesn't, and the driver is field-proven. **Therefore: delivered length is header+payload, NO FCS. The AMIX RX min-frame boundary is 60 (not 64), and you must NOT subtract `ETH_CRC_LEN` from the slot length** (unlike aen/hydra `length = count - ETH_CRC_LEN`, Survey C §5 / hydra.c:1212). This is the precise inverse of hydra's bug and the design should bake it in rather than leaving it real-HW-gated. **Severity: IMPORTANT** — wrong handling here silently kills ARP; and it's resolvable now, contradicting the design's "LOW-confidence/real-HW-gated" framing for Q4.

### I5. Design omits that the guest **re-reads `REG_ZZ_ETH_RX_ADDRESS` to get the slot, but the firmware never validates the slot is fresh on the address read** — the ordering "read 0x1A4 → read serial → process → strobe" is the only safe sequence, and the design's §5 ordering list has it right but the §4.5 pseudocode's single up-front read (per B1) violates it

Cross-reference to B1; calling it out separately because §5 item 3 lists the correct RX ordering ("read 0x1A4 → read slot header(serial) → read payload → strobe 0x194") while §4.5 pseudocode does not re-read. **The §5 ordering is authoritative and correct; §4.5 must be rewritten to match it.** **Severity: IMPORTANT** (internal inconsistency that will mislead implementation).

---

## MINOR / confirmed-correct

### M1. IRQ level — design Q1 ("INT6 vs INT2") — firmware unambiguously asserts **INT6**
`amiga_interrupt_set(AMIGA_INTERRUPT_ETH)` → `DiscreteSet(REG0, FPGA_INT6)` on the 0→nonzero edge (`interrupt.c:29`); cleared via `DiscreteClear(REG0, FPGA_INT6)` (`interrupt.c:50`). `AMIGA_INTERRUPT_ETH = 1` (`interrupt.h:19`). The design correctly flags the **divergence** (AMIX aen/hydra are on INT2/`int2_tbl[]`) as the genuine open question. This is real and correctly identified — the firmware raises **level-6 autovector**, and AMIX has no surveyed `int6_tbl[]`. The design's Q1 framing is accurate; I only confirm the firmware half is definitively INT6, not configurable to INT2 in this build (the INT2 path is a guest-side `DEVF_INT2MODE` board-mod, `device.c:233-242`, not a firmware option). **Severity: OK** — correctly flagged as the top wiring question.

### M2. Register offsets, MAC packing, TX window, header layout — all CONFIRMED correct
- Offsets `0x104/0x190/0x194/0x198/0x19C/0x1A4/0x1A8` exactly match `z3660_regs.h:42,86-93`. ✓
- `TX_FRAME_ADDRESS=0x07EE0000`, `RX_BACKLOG_ADDRESS=0x07ED0000`, `FRAME_SIZE=2048`, `RX_FRAME_PAD=4` match `memorymap.h:32-40`. ✓
- MAC HI/LO packing: design says `_HI=(m0<<8)|m1`, `_LO=(m2<<24)|(m3<<16)|(m4<<8)|m5`. Firmware read confirms exactly (`rtg.c:865-873`); guest write confirms (`device.c:209-211`). ✓
- 4-byte BE slot header `[len16][serial16]`, payload at +4, ethertype at slot `[16..17]`: firmware writes `[0..1]=rx_bytes, [2..3]=serial` (`ethernet.c:556-559`); guest reads ethertype at `frm[16]<<8|frm[17]` (`device.c:809`). ✓
- TX: frame into `0x07EE0000` with no length/header prefix, write byte-length to `0x190`, read back for rc; synchronous busy-wait ≤~1ms (10×`usleep(100)`, `ethernet.c:1080-1090`). ✓ All correct.

### M3. INT_STATUS bits ETH=1/AUDIO=2/USB=4 — CONFIRMED
`REG_ZZ_INT_STATUS` read returns `amiga_interrupt_get()` (`rtg.c:788`); `AMIGA_INTERRUPT_ETH=1` (`interrupt.h:19`). Guest checks `status & 1` (`device.c:84`). ✓

### M4. Backlog overrun (64×2048=128KB into TX window at +64KB) — CONFIRMED latent firmware bug, design correctly flags it
`FRAME_MAX_BACKLOG=64` (`ethernet.c:59`), map comment says "32×2048 (64kB)" (`memorymap.h:32`), `TX_FRAME_ADDRESS` is exactly 64KB above. Design §2.8/Q6 correctly identifies this as out-of-driver-scope with "drain eagerly" mitigation. Note this **interacts with B1**: if the AMIX driver does one-per-IRQ (like the reference) and RX bursts >32 frames between IRQs, the firmware's own producer overruns — another argument for the design to commit to an eager loop-drain ISR (done correctly per B1). ✓ flagged.

### M5. MAC read source — design correct, with one nuance
Design §2.6: read 6 bytes from `_HI`/`_LO`. Confirmed (`rtg.c:858-874`). The guest reads then writes-back to force `ethernet_update_mac_address()` (`device.c:202-211`, firmware `rtg.c:1645`). The design correctly notes the write-back is optional. The default MAC `00:80:10:00:01:00` claim — I did not re-verify the literal in `ethernet.c:81` this pass (Survey A asserts it); not load-bearing for the driver, which reads whatever is there. **OK.**

---

## Summary table

| # | Severity | Issue | Correct fact | Cite |
|---|---|---|---|---|
| B1 | **BLOCKER** | §4.5 RX pseudocode reads `0x1A4` once then loops on serial → under-drains/spins | Must re-read `REG_ZZ_ETH_RX_ADDRESS` after **every** `0x194` strobe; cursor only advances on strobe | `ethernet.c:596-597,604-639`; `rtg.c:1613`; `device.c:796-799` |
| B2 | **BLOCKER** | Empty-ring detection by `serial==last` undocumented dependency on firmware serial-stamp hack | Firmware stamps slot-0 serial = last-consumed on catch-up; that IS the empty sentinel | `ethernet.c:619-629` |
| I1 | IMPORTANT | CONFIG presented as OR-able bit menu; enable/ack are exclusive branches | enable=write `1`; ack=write `24`(`8\|16`); `1\|8\|16` is taken as ack-only, enable silently lost | `rtg.c:1150-1173` |
| I2 | IMPORTANT | TX rc==1 mislabeled "not ready" generically | rc==1 = `ethernet_task_state != ETH_TASK_READY` (no link/GEM down) — normal until carrier; queue, don't error-drop | `ethernet.c:1031-1034` |
| I4 | IMPORTANT | Q4 (FCS) left real-HW-gated; it's source-answerable | Length is header+payload, **NO FCS**; do NOT subtract CRC (unlike aen/hydra); min frame 60 | `ethernet.c:540,551-559`; `device.c:633-643` |
| I5 | IMPORTANT | §4.5 pseudocode contradicts §5 RX ordering | §5 ordering (re-read 0x1A4→serial→payload→strobe) is authoritative | `device.c:796-839` |
| M1 | OK | INT6 confirmed; INT2 divergence correctly flagged as Q1 | Firmware asserts FPGA_INT6, not configurable to INT2 in-firmware | `interrupt.c:29,50` |
| M2-M5 | OK | Offsets, MAC packing, header layout, TX, INT bits, overrun | All cross-verified correct | as above |

**Net:** the static contract (registers/offsets/header/TX/MAC/IRQ-level) is solid. The **RX dynamics** are where the design has real bugs: the drain loop must re-read `0x1A4` per strobe (B1), the empty-ring sentinel rests on a firmware serial-stamp hack that must be documented (B2), and the FCS/min-frame question (I4) is answerable now (no FCS, min 60) rather than deferred. Fix B1/B2/I4 before Phase 3.

## LENS — coherency-mmu
I now have a fully source-grounded review. Critically, observe the contradiction the design rests on:

- The design claims (§2.7, §5.1) the eth window is/must-be **cache-inhibited** in the 030 MMU, and that "SCSI works because the window is CI."
- But the **actual TT registers are `CI=0` (cacheable)** (`ttrap.s:327,329`), and **`sptalloc(...,PG_V,...,0)`** — the SCSI/A4091 mapping primitive — passes `PG_V` with flag `0`, with **no evidence of a CI bit** anywhere in the readable source.
- And the SCSI mailbox doesn't actually need 030-side coherency because **firmware always bounces through MMIO at `board+0x80000` and the transfer is fully synchronous** (the command write completes the whole DMA before it returns) — there is no producer/consumer race between CPU caches and an async DMA agent. The eth RX path is fundamentally different (async, interrupt-delivered, no synchronous completion).

This breaks the design's "mirror z3660.c exactly" prescription. Let me confirm one more thing: that the SCSI completion is synchronous and there's no async coherency window (already confirmed in z3660.c lines 28-32, 220, 225). I have enough. Writing the review.

# ADVERSARIAL REVIEW — "coherency-mmu" lens

**Scope:** §2.7, §5 (whole), §8 Q-COHERENCY, plus the §9 Phase-0/4 coherency claims and the §3 "no firmware change" coherency caveat. I re-read the actual AMIX MMU/TT setup, the `sptalloc` mapping primitive as used by every Zorro III driver, `z3660.c` (the cited precedent), `zz9000.c`, `a2091.c`, and the firmware/guest cache notes. The verdict: **the section's factual foundation is inverted on one load-bearing point, and the cited precedent does not transfer.** Details below.

---

## BLOCKER 1 — The "Zorro III window is cache-inhibited on the 030" claim is FALSE per the actual TT registers

**Design claim** (§2.7: *"coherency of these windows is entirely the AMIX driver's responsibility"* framed around §5.1: *"Map the board window cache-inhibited in the 030 MMU… confirm AMIX maps the Z3660 Zorro III window CI — it must be, for the SCSI mailbox to work"*; §8 Q-COHERENCY: *"confirm AMIX maps the Z3660 Zorro III window CI"*).

**Why it is wrong.** The actual transparent-translation registers AMIX loads are, verbatim from source:

```
ttrap.s:327  tt0_on:  long 0x003F0143  # 0x0000000-0x3FFFFFFF FC=Super,R/W,Disable,CI=0
ttrap.s:329  tt1_on:  long 0x807F0143  # 0x8000000-0xFFFFFFFF FC=Super,R/W,Disable,CI=0
```

The kernel's own comment states **`CI=0`** for both TT registers — i.e. the entire transparently-translated physical space (`0x00000000–0x3FFFFFFF` and `0x80000000–0xFFFFFFFF`) is **cache-ENABLED**, not cache-inhibited. The Z3660 combined window at the fixed base **`0x10000000`** (NOTES.md:167-168; `z3660.c:59 Z3660_FIXED 0x10000000`) sits squarely inside TT0's range, so on the 030 side it is **cacheable**. The design's premise — that "it must be CI, for SCSI to work" — is contradicted by the hardware config: SCSI works *over a cacheable mapping*, not a CI one.

For the page-table path (Zorro III in the `0x40000000` gap), the mapping primitive every existing Zorro III driver uses is `sptalloc(npages, PG_V, pfn, 0)` — `z3660.c:139-142`, a4091 `zorro-autoconfig.md:70-72`. The flag argument is **`0`** and the protection is **`PG_V`** (page-valid). **Nothing in any readable source sets a cache-inhibit PTE bit.** `immu.h`/`PG_*` are not in this tree (they come from the licensed sysroot), so I cannot prove the *absence* of an implicit CI — but the design asserts CI as established fact, and the only evidence available (TT `CI=0`, `flag=0`, `PG_V` only) points the opposite way. **This is an unverified assumption presented as a confirmed baseline.**

**Corrected fact:** The 030 most likely reaches the eth window through a **cacheable** mapping (TT0, `CI=0`, `ttrap.s:327`). The driver therefore **cannot** "rely on a strict CI mapping for implicit ordering" (§5.3) — because the mapping is probably not CI at all. CI-ness must be *established*, not *assumed*, and if it cannot be established the driver needs explicit `cpushl`/`cinvl` cache maintenance on every shared-buffer handshake.

**Severity: BLOCKER.** The entire coherency argument (§5.1, §5.3, §8 Q-COHERENCY) is built on "the window is CI, so accesses are serialized and ordered." If the window is cacheable (as the source indicates), then: stale RX reads (CPU reads its cached copy of a slot the firmware just rewrote), and reordered/withheld TX writes (the frame sits in the 030 data cache when the firmware reads DDR), are *live* bugs — exactly the "works-then-corrupts" class the design fears, except the design has mis-located the safeguard.

---

## BLOCKER 2 — `z3660.c` (SCSI) is NOT a transferable coherency precedent; "mirror z3660.c exactly" is unsafe

**Design claim** (§5.1: *"Mirror exactly what `z3660.c` does for its mailbox — that mailbox is the proven-coherent precedent on this very board"*; §5.3; §8: *"mirror `z3660.c`'s mailbox handling exactly"*; Survey D §5.6 echoed this).

**Why it is wrong/incomplete.** The SCSI mailbox is coherent for a reason that **does not exist on the eth RX path**:

1. **SCSI completion is fully synchronous.** `z3660.c:28-31` and the NOTES: *"That single command-register write is BOTH the trigger and the completion — the ARM intercepts the Zorro III bus cycle and finishes the whole transfer before the write returns."* There is **no asynchronous DMA agent racing the CPU cache.** By the time `WRLONG(P_READ, unit)` (`z3660.c:225`) returns, the firmware has already finished writing the bounce buffer; the subsequent `bcopy(bounce, data, len)` (`z3660.c:228`) reads data that is *guaranteed already settled*. There is never a window where the firmware writes a buffer the CPU later reads asynchronously.

2. **The shared buffer is MMIO, not cached system RAM, and the coherency problem was explicitly judged to "largely disappear."** NOTES.md:48-50: *"if we always bounce through board+0x80000 (MMIO, not cached system RAM), the coherency problem largely disappears. 🟡 (verify on HW)"*. Note the **🟡 — this is unverified even for SCSI.** The design upgraded an unverified SCSI hypothesis into a "proven-coherent precedent."

3. **`z3660.c` does ZERO cache maintenance** — no `cpush`, no `cinv`, no barrier (whole file; `z3660intr()` at `:378-381` is empty). It gets away with it *only* because of (1) synchronicity and (2) the design's reliance on the mapping. There is literally nothing in `z3660.c` to "mirror" for coherency — mirroring it gives you *no* coherency handling at all.

The eth RX path is the opposite on every axis: **asynchronous** (firmware writes a backlog slot at GEM-RX time, the CPU reads it later from an INT6/INT2 handler — Survey A §2, B §3), **no synchronous completion handshake**, and the freshness signal is a **serial word the firmware publishes *last*** (`ethernet.c:553-561`, publish-order barrier). If the 030 has a stale cached copy of that slot (cacheable mapping per BLOCKER 1), the serial-comparison drain (`design §4.5`) reads the *old* serial and either misses the frame or reads a torn header. **This is precisely the lpsched-/SCSI-bounce-class race, and the cited precedent provides no defense against it because the SCSI path never had the race.**

**Corrected guidance:** The transferable precedent for *async, cacheable shared DDR* is **not** `z3660.c`. The driver must either (a) prove the window is genuinely CI and rely on that, or (b) if cacheable, issue explicit 030 cache control: invalidate (`cinvl`/`cpushl`) the RX slot's cache lines **before** reading serial+payload each drain iteration, and push (`cpushl`) the TX frame **after** filling it and **before** writing the length to `0x190`. The AmigaOS guest's commented-out `CacheClearE(frm, …, CACRF_ClearD)` at `device.c:817` (Survey A §6) is the *real* precedent — the original author put a D-cache clear exactly there and it was disabled only because the guest's Zorro III map is CI. AMIX's map (TT0, `CI=0`) is **not** CI, so that line's logic must be *re-enabled* in driver form.

**Severity: BLOCKER.** "Mirror z3660.c exactly" applied to the eth RX path yields a driver with no coherency handling on a cacheable async buffer — the single highest-ranked risk in the design's own §8, left undefended by the design's own prescribed fix.

---

## IMPORTANT 3 — "The 030 lacks a `dsb`; with CI mapping ordering is implicit" is a non-sequitur given BLOCKER 1, and conflates two different problems

**Design claim** (§5.3: *"The 030 lacks a `dsb`. With a strict CI mapping, ordering is implicit (CI accesses are serialized)… if the window is genuinely CI these are naturally ordered… Do NOT rely on a `dsb`-style barrier."*).

**Why it is incomplete.** Two issues:

1. The premise ("strict CI mapping") is the unproven BLOCKER-1 assumption. If the map is cacheable, CI-serialization does **not** apply and there is no implicit ordering — write-allocate/copyback caching can both *reorder* and *indefinitely withhold* the TX frame relative to the `0x190` store (the store itself may hit a different cache line / the register page). The conclusion only follows from the false premise.

2. Even granting CI, the design conflates **ordering** (TX-fill-before-trigger; RX-serial-read-before-payload-read) with **visibility/coherency** (is the CPU looking at cache or DDR). CI fixes both for the *registers* (the `0x100`-page is genuinely device-emulated MMIO — `main.c:644` "*write to RTG registers is emulated*", Survey B §0). But the **frame windows are real DDR** (Survey B §0, §3), not emulated registers. Whether those DDR pages are CI on the 030 is a *separate* PTE question from the register page, and the design treats "the board window" as one uniform CI region when it is (a) an emulated-MMIO register page and (b) bulk cacheable-candidate DDR — potentially different cache treatment, definitely different coherency characteristics.

**Corrected fact:** The "don't use dsb, rely on CI" advice is only valid if CI is *proven for the DDR frame pages specifically*. The MEMORY note that "dsb was a Heisenbug artifact" is about an ARM-side barrier on the *firmware* side and does not license dropping **030-side** cache maintenance. On 68030 the correct primitives are `cpushl`/`cinvl` (or the whole-cache `CACR` CI/clear), not a barrier — the design is right that "dsb" is the wrong tool, but wrong that *no* explicit action is needed.

**Severity: IMPORTANT.** Misdiagnoses the required mechanism; will lead the implementer to ship zero cache maintenance and discover corruption only on the real-HW soak (§9 Phase 4) — the most expensive place to find it.

---

## IMPORTANT 4 — The design never establishes that AMIX even *maps* the eth sub-window, and the `sptalloc` granularity is wrong for it

**Design claim** (§4.6, §5.1: derive register pointers and `txwin=base+0x07EE0000`, `rxring=base+0x07ED0000` from "the Zorro autoconfig base," reusing "the proven Z3660 SCSI discovery").

**Why it is incomplete.** `z3660.c` only `sptalloc`s **two single pages**: the register window (`base+0x2000`, 1 page) and the bounce buffer (`base+0x80000`, `BOUNCE_PAGES=32` = 64 KB) — `z3660.c:139-142`. The eth windows are at **much higher offsets**: `base+0x07ED0000` (RX ring) and `base+0x07EE0000` (TX) — Survey A/B. These are **not covered by the SCSI driver's mappings at all.** Reusing "the SCSI discovery" gives you the *base address*, not a *mapping* of the eth offsets.

Two consequences:
- If the board lands at the fixed `0x10000000` (within TT0, `autoconfig_rtg NO`, the normal config per NOTES.md:166-168), the eth offsets resolve to `0x17ED0000`/`0x17EE0000` — **inside TT0**, reachable by direct deref (like `aen`/`a2091` do, `aen.c:320`, `a2091.c:411`), but **cacheable** (BLOCKER 1).
- If it lands at `0x40000000` (`autoconfig_rtg YES`, the gap), the eth offsets are `0x47ED0000`/`0x47EE0000` — in the **unmapped TT gap**, requiring `sptalloc`. The RX ring alone is 64 slots × 2048 = **128 KB = 64 pages** (Survey A/B §3), plus the TX window. The design's softc (§4.1) lists `volatile uchar *rxring` as a single derived pointer with no mention of mapping 64+ pages, nor of the fact that the RX ring and TX window are **non-contiguous offsets needing separate `sptalloc` calls** (the 64 KB gap between `0x07ED0000` and `0x07EE0000` — exactly the region Survey A/B flag as the 64-slot overrun hazard).

**Corrected fact:** The driver needs its own `sptalloc` of the eth windows (register page at `+0x100`, RX ring ≥64 pages at `+0x07ED0000`, TX window at `+0x07EE0000`) — this is *new* mapping work the SCSI driver never did, and the **cache flag passed to those `sptalloc` calls is the actual coherency control point the design must specify** (and currently does not — it passes the question off to "mirror z3660.c," which passes flag `0`).

**Severity: IMPORTANT.** The design's "reuse SCSI discovery, derive pointers from base" hand-waves the single place where 030 cache policy for the DDR frame buffers is actually chosen. If `sptalloc`'s 4th arg can request CI (plausible — it's literally named `flag`), **that is the fix**, and the design should mandate investigating/using it rather than assuming the base mapping is already CI.

---

## IMPORTANT 5 — RX delivery copies into a *cached* mblk/`rxbuff` and hands it up the stream with no ordering guarantee relative to the slot-advance strobe

**Design claim** (§5.4: *"bcopy straight from the CI RX slot into the allocb'd mblk (the mblk is cached kernel memory, but it's never touched by hardware — only the CI shared slot is the coherency frontier)"*; §4.5: copy `len` bytes from `slot+4` into static `rxbuff[]`, then `REG_ZZ_ETH_RX=1` advance).

**Why it is incomplete.** Two ordering hazards the design dismisses:

1. The drain sequence (§4.5) reads serial, reads payload, **then** strobes `0x194` to advance. If the slot is cacheable (BLOCKER 1) and the CPU prefetched/cached the slot line on a *prior* drain, the "read payload" step can return stale bytes for a slot whose address was *reused* after a firmware ring-reset (`ethernet.c:619-631` reuses slot-0 address; freshness is by serial *value*, not address — Survey A §2d, B §3). The serial check protects against *processing* a stale slot, but only if the **serial read itself is coherent** — which on a cacheable map it is not, unless invalidated first. The design's own §4.5 relies on serial-freshness while §5.4 declares "only the CI shared slot is the frontier" — circular: it assumes the very CI-ness that BLOCKER 1 shows is unproven.

2. "The mblk is never touched by hardware" is true but irrelevant to the *frontier*: the bug isn't the mblk, it's that the `bcopy` *source* (the slot) may be read from stale 030 cache. Calling the mblk "cached but safe" correctly identifies the destination as fine while **mis-stating that the source is automatically fine** ("only the CI slot is the frontier" assumes CI).

**Corrected fact:** Each drain iteration must **invalidate the slot's cache lines before reading the 4-byte header**, then (since the payload is in the same/adjacent lines) the payload read is coherent; only then strobe advance. On a CI map this is a no-op (free); on a cacheable map it is mandatory. The design must branch on the *established* (not assumed) cache policy.

**Severity: IMPORTANT.** Same root cause as BLOCKERs 1-2, surfacing in the exact hot loop. Listed separately because it shows the design's §5.4 "decision" (read slot directly vs stage through `rxbuff`) is moot until the cache policy is pinned — neither variant is safe on a cacheable map without invalidation.

---

## MINOR 6 — The §3 "no firmware change" coherency caveat understates the asymmetric flush the firmware already does

**Design claim** (§3 caveat: TX blocks ~1 ms, "design AMIX TX to tolerate the stall… not a firmware modification").

**Why it is incomplete (minor).** The firmware's coherency posture is **asymmetric** and the design doesn't carry it into the AMIX contract: TX *does* `Xil_L1/L2CacheFlushRange(TxFrame)` before GEM handoff (`ethernet.c:1039-1040`, Survey B §4) — belt-and-suspenders on the ARM side — but RX invalidates are **commented out** (`ethernet.c:546-548`), relying on the non-cached ARM mapping. This is fine for the *ARM* domain but means the **publish-order of the RX header (payload memcpy first, serial last, `ethernet.c:553-561`) is the ONLY barrier the 030 can lean on** — and it only works if the 030 reads are coherent/ordered. The design mentions the publish-order (§2.2) but never connects it to the 030-side invalidation requirement. It's a documentation gap, not a logic error, hence MINOR.

**Severity: MINOR.**

---

## What the section gets RIGHT

- **The frame buffers are real DDR (not emulated registers)** while the `0x100`-page registers are emulated MMIO (§2.7 implicitly; correct per `main.c:644`, Survey B §0). This distinction is sound and important.
- **"Do NOT rely on a dsb-style barrier"** — correct *conclusion*, wrong *reason*. The 68030 has no `dsb`; the right tool is `cpushl`/`cinvl`/CACR, and the firmware-side dsb was indeed a Heisenbug per MEMORY. The design is right to steer away from a barrier mindset.
- **Declaring all shared accesses `volatile`** (§5.2) — correct and load-bearing; matches `aen.h:84-93`, `a2091.c:66-74`, `z3660.c:101-102`.
- **Real-HW-only verification of coherency** (§5.5, §9 Phase 4) — correct; the WinUAE box has an emulated a2065, not the GEM, so the DDR-coherency frontier genuinely cannot be exercised there.
- **The eager-drain mitigation for the 64-slot/64 KB overrun** (§2.8, §8 Q6) — orthogonal to coherency but correctly identified.

---

## Bottom line for the coherency-mmu section

The section correctly *names* the risk (async shared-DDR coherency, lpsched-class) and correctly rejects an ARM-style barrier — but its two prescribed fixes are both unsound:

1. **"The window is CI, so it's serialized/ordered"** — contradicted by `ttrap.s:327,329` (`CI=0`) and by `sptalloc(...,PG_V,...,0)` carrying no visible CI flag. **CI is assumed, not established.**
2. **"Mirror z3660.c exactly"** — `z3660.c` has *zero* cache maintenance and is coherent only because SCSI is **synchronous + MMIO-bounced** (`z3660.c:28-31,220-228`; NOTES.md:48-50, itself 🟡-unverified). The eth RX path is **async**; the precedent does not transfer.

**The corrected fix the design must adopt:** Treat establishing the 030 cache policy of the eth **DDR frame pages** (not just the register page) as a hard Phase-0 deliverable: determine whether `sptalloc`'s `flag` arg can request a cache-inhibited PTE and **use it** for `0x07ED0000`/`0x07EE0000`; if it cannot, add explicit `cinvl` (RX slot, before each header+payload read) and `cpushl` (TX frame, after fill, before the `0x190` trigger) — re-enabling, in driver form, the very `CacheClearE` the AmigaOS guest left commented at `device.c:817`. Until that policy is *proven* (not assumed), the datapath is undefended against the design's own #1-ranked risk.

**Key file:line evidence:** `amiga/ml/ttrap.s:327,329` (TT `CI=0`); `amiga/ml/syms.s:2` (u-area `0x40000000`, why TT0 stops at `0x3FFFFFFF`); `amix-z3660/src/z3660.c:28-31,139-142,220-228,378-381` (synchronous mailbox, `sptalloc(PG_V,…,0)`, zero cache maintenance); `amix-z3660/NOTES.md:48-50` (SCSI coherency "largely disappears" 🟡 *unverified*); `hydra-amix/.../aen/aen.c:320` & `alien/a2091.c:411` (Zorro boards deref autocon base directly — cacheable via TT0); firmware `ethernet.c:1039-1040` (TX flush) vs `546-548` (RX invalidate commented out), `553-561` (serial-last publish order); guest `device.c:817` (commented-out `CacheClearE` — the real RX-coherency precedent); `zorro-autoconfig.md:58-64` (TT-gap, `sptalloc`).

## LENS — dlpi-streams-completeness
The firmware never calls `XEmacPs_SetOptions` to set FCS strip, and the guest never subtracts CRC. The most likely reality: the Xilinx EmacPs init default (via `XEmacPs_CfgInitialize` and the BSP `xemacps_example_util` flow) commonly enables `XEMACPS_NWCFG_FCSREM` in the network config, OR the guest tolerates trailing FCS. This is genuinely ambiguous from source alone — exactly why the design correctly flagged it as a real-HW-gated question. My finding refines it: the **working SANA-II guest does NOT subtract CRC**, so the aen `count - ETH_CRC_LEN` convention may be WRONG for Z3660 and would truncate 4 payload bytes. That's a concrete, important correction.

I now have all evidence needed. Let me write the adversarial review.

# ADVERSARIAL REVIEW — "dlpi-streams-completeness" lens

Scope: STREAMS/DLPI write-put + primitive set, RX SAP demux, frame-size/CRC boundary, MTU/trailers, flow control, open/close races, streamtab/qinit shape — design §4.2/§4.3/§4.4/§4.5 vs the **actual** `aen.c`/`hydra.c` source. Verdict up front: the skeleton choice is sound, but the design's DLPI primitive set is **incomplete in a way that will break IP attach**, its RX length/CRC arithmetic is **stated wrong vs both reference drivers AND the working Z3660 guest**, and several flow-control/race details are glossed. Findings below, severity-tagged.

---

## BLOCKER-class findings

### B1. DLPI primitive set omits DL_ATTACH_REQ / DL_DETACH_REQ — and the design picked the WRONG reference for it
**Design claim (§4.3):** "DLPI primitives (irreducible set for ARP+ping)" lists exactly five: DL_INFO_REQ, DL_BIND_REQ, DL_UNITDATA_REQ, DL_UNBIND_REQ, plus "consider DL_PHYS_ADDR_REQ". It explicitly says copy `hydra.c` ("the cleaner of the two precedents") "byte-for-byte."

**Why it's wrong/incomplete:** The design's own chosen skeleton, `hydra.c`, implements **seven** primitives, including `DL_ATTACH_REQ` (`hydra.c:873-892`) and `DL_DETACH_REQ` (`hydra.c:996-1015`) — both returning `DL_OK_ACK` and moving `hp->state` between `DL_UNATTACHED`/`DL_UNBOUND`. The design dropped exactly the two primitives that differ between aen and hydra, then claims to be mirroring hydra. `aen.c` (the other reference) does NOT implement them (`aenproto` `aen.c:789-926` has only UNITDATA/BIND/INFO/UNBIND). So the design's list actually mirrors **aen**, not hydra — an internal contradiction.

The functional risk: both providers advertise `dl_provider_style = DL_STYLE1` (`aen.c:869`, `hydra.c:953`). For a *strict* STYLE1 provider a PPA is bound at open and DL_ATTACH is illegal — so aen's omission is defensible. BUT the design also says (§4.3) "consider adding DL_PHYS_ADDR_REQ and real DL_ENABMULTI/DL_PROMISCON" and copies hydra. If the SVR4.0 `dlpi`/`if_dl` glue or a future STYLE2 `slink` path issues DL_ATTACH (hydra found it necessary to answer — that is *evidence* the AMIX stack can send it), an unanswered/`default:`-NAK'd ATTACH will fail attach. The safe course is hydra's: answer ATTACH/DETACH with DL_OK_ACK even under STYLE1.

**Corrected fact:** Implement DL_ATTACH_REQ → DL_OK_ACK(state=DL_UNBOUND) and DL_DETACH_REQ → DL_OK_ACK(state=DL_UNATTACHED), per `hydra.c:873-892, 996-1015`. Do not present a 5-primitive list as "what hydra does." **Severity: BLOCKER** (silent attach failure path; and the design misrepresents its own skeleton).

### B2. RX length/CRC arithmetic is stated wrong — and the two authoritative sources DISAGREE
**Design claim (§2.4 / §4.5 / §8 Q4):** RX drain "copy `len` bytes (cooked: skip 14-byte header) into an mblk"; Q4 ("does firmware deliver with/without FCS") is deferred to real HW. The TX/RX byte math in §2.2 says slot `[0..1]=length (incl. 14-byte eth header)`.

**Why it's wrong/incomplete:** The design treats `len` as the deliverable payload count, but **neither reference driver does that** and the **working Z3660 guest does something different again** — this is a live contradiction the design papers over as "real-HW-gated":

- `aen.c` (`toss_packet_up_stream`, `aen.c:1212`): `length = count - ETH_CRC_LEN;` then it pushes `length` bytes of payload (`aen.c:1335-1339`). i.e. aen assumes the buffer **includes the 4-byte FCS** and strips it. If the Z3660 driver copies the slot `len` straight up, it leaks 4 CRC bytes into every datagram → IP/UDP length mismatches, ARP malformations.
- Z3660 **firmware** (`ethernet.c:540,553,558`): `rx_bytes = XEmacPs_BdGetLength(cur_bd_ptr)` copied verbatim into the slot and written as the `[0..1]` length. The firmware calls `XEmacPs_CfgInitialize` (`ethernet.c:266`) but **never calls `XEmacPs_SetOptions(...FCS_STRIP...)`** (grep: no FCSREM/SetOptions in the eth path) — so the GEM BD length **includes the FCS** unless the BSP default has FCS-removal on.
- Z3660 **guest** (`device.c:643`): `datasize = sz - HW_ETH_HDR_SIZE` — subtracts **only the 14-byte header, NOT the CRC.** The proven-working SANA-II driver passes the trailing 4 FCS bytes up to AmigaOS (or the GEM already stripped them and `sz` excludes FCS). 

So the design's "Q4 is real-HW only" is half right but understated: the **aen template you are copying will subtract 4, the firmware delivers +4 (probably), and the guest subtracts 0** — three different conventions. If you copy `toss_packet_up_stream` verbatim (design §4.5 "reuse almost verbatim") you inherit aen's `-ETH_CRC_LEN`, which is correct ONLY if FCS is present. Given hydra's documented #1 bug was exactly this 60-vs-64 / CRC-inclusion boundary (Survey D §5.1), shipping the wrong assumption silently kills ARP.

**Corrected fact:** State explicitly that the RX copy length = `slot_len − ETH_CRC_LEN` **iff** the GEM delivers FCS (the likely case: no `SetOptions` FCS-strip in `ethernet.c`, and `XEmacPs_BdGetLength` is raw). Verify by reading `frm[0..1]` for a known-size broadcast ARP (a 64-byte-on-wire ARP → slot len should read 64 if FCS present, 60 if stripped) BEFORE wiring `toss_packet_up_stream`. Do NOT inherit aen's `-4` blindly. `ethernet.c:540`, `aen.c:1212`, `device.c:643`. **Severity: BLOCKER** (the single most likely "ARP never completes" failure; the design's own §1 risk-4 names it but §2/§4 then assert the wrong/unqualified arithmetic).

---

## IMPORTANT-class findings

### I3. SAP/ethertype demux: width-truncation and byte-order of `dl_sap` not addressed
**Design claim (§4.2/§4.3):** softc has `sap` field; BIND "sap = dl_sap (the ethertype; ARP binds 0x0806, IP 0x0800)"; RX demux "on ethertype `slot[16..17]`".

**Why it's incomplete:** `aen_t.sap` / `hydra_t.sap` is `unsigned short` (`aen.h:78`), but `reqp->dl_sap` is a `t_uscalar_t` (4 bytes). aen does `aen->sap = reqp->dl_sap;` (`aen.c:801`) — a **silent 32→16 truncation**, then on TX copies `sizeof(aen->sap)`=2 bytes into the frame's type field (`aen.c:341-342`), and on RX matches against `packet_header->ether_type` (a raw 2-byte big-endian field, `aen.c:1184,1270`). This works on the 68k **only because** the m68k is big-endian and the low 16 bits of the SAP land correctly. The design must (a) keep `sap` as `u_short` and replicate the exact truncate-then-2-byte-copy, and (b) NOT byte-swap — the ethertype goes onto the wire in network order directly from the `u_short`. If a porter "cleans this up" to a 4-byte SAP or adds an htons, demux breaks. The design's §2.2 says registers are "32-bit, big-endian" which could mislead a porter into swapping the ethertype too.

**Corrected fact:** Mirror `aen.c:801` (truncating assign) + `aen.c:341` (2-byte copy, no swap) + `aen.c:1184/1270` (compare raw `ether_type`). `aen.h:78`. **Severity: IMPORTANT** (porting-trap; the design's register-endianness note actively invites the wrong fix).

### I4. Broadcast/multicast RX delivery is under-specified vs the actual demux
**Design claim (§4.5):** "Broadcast (dst all-FF) flows naturally to the SAP-matched stream."

**Why it's incomplete:** That is true in aen ONLY because aen does NOT filter by dest MAC on the normal path — it delivers to every stream whose `sap` matches, regardless of dest (`aen.c:1270-1289`). The dest-MAC check exists **only** inside the `MODE_PROM` (promiscuous) branch (`aen.c:1272-1287`), which compares against own MAC OR `broadcast_address[6]={FF..FF}` (`aen.c:1179-1181, 1280-1285`). So "flows naturally" is correct, but the design must note: there is **no unicast-dest filtering at all** in the non-promiscuous path — the driver relies on the *hardware/firmware* to have filtered to own-MAC+broadcast already. For Z3660 the GEM does that filtering; but if the firmware GEM is in promiscuous/pass-all mode (Survey A §5 says "presumably passes multicast/broadcast", no filter register), the AMIX driver will hand IP every frame on the wire for the bound SAP, including other hosts' unicasts → IP must drop them, wasted work, and a **security/coherency note**: every such frame still triggers an allocb+putnext. The design should either (a) replicate aen's no-filter trust-the-hardware model AND confirm the firmware GEM filters to own-MAC+bcast, or (b) add an explicit dest-MAC accept-filter (own || broadcast || multicast-of-interest) in the Z3660 `toss_packet_up_stream`.

**Corrected fact:** Non-promiscuous aen does zero dest filtering (`aen.c:1270` is a pure `sap==sap` match); filtering is hardware's job. Confirm Z3660 GEM RX filter mode in firmware before trusting "flows naturally." `aen.c:1270-1287`, Survey A §5. **Severity: IMPORTANT.**

### I5. Flow control: design says `aenwsrv` is a "no-op stub … enables flow control" — but the real resume is interrupt-driven, and the design never wires it
**Design claim (§4.2 table):** "`z3660ethwsrv` — no-op stub (enables flow control)." §4.4 step 1: "If busy → putq (deferred, flow-controlled)."

**Why it's incomplete:** `aenwsrv` literally `return 0;` (`aen.c:418`) and is NEVER the thing that drains the backlog. The deferred DL_UNITDATA_REQ mblks are re-queued via `putq(q,mp)` in `aenproto` (`aen.c:792-793`) and are drained **only** by `transmit_interrupt` (`aen.c:1357-1380`): on TX-complete IRQ it walks all streams, `getq(WR(aen->q))`, retries `aenxmit`, and `putbq`+break on still-full. The write service procedure is a dummy that exists purely so STREAMS *permits* `putq`/back-pressure (a qinit with a non-NULL srv enables `q_count`/HIWAT accounting), but it does no work. 

The Z3660 trap: **there is no TX-complete interrupt** (Survey A §1, B §2 — `REG_ZZ_ETH_TX` is synchronous/blocking; the ARM busy-waits ≤1 ms and returns the result in the register read-back). So the aen model "defer on full, drain from TX-done IRQ" has **no IRQ to hang the drain on**. The design's §4.4 says "serialize behind the write queue" and "putq (deferred)" but never says **what re-fires the queued mblk.** With a synchronous TX and no TX-done interrupt, a `putq`'d frame will sit until the next *RX* interrupt or a `qenable`. The design MUST specify the resume mechanism: either (a) make TX never defer (since it's synchronous, a frame either goes or errors — there's no "ring full" state like aen's `TMD1_OWN` gate; the only "busy" is another thread mid-TX, resolved by a lock, not a queue), or (b) explicitly `qenable(WR(q))` after the blocking TX returns to drain any backlog. As written, the design copies aen's flow-control shape onto hardware that lacks the event that makes it work.

**Corrected fact:** `aenwsrv` is a no-op (`aen.c:418`); real resume = `transmit_interrupt` on TX-done IRQ (`aen.c:1373`). Z3660 has no TX-done IRQ (synchronous TX, `ethernet.c:1080-1090`), so the design must define a different drain trigger (lock-serialized immediate TX, or explicit `qenable`). **Severity: IMPORTANT** (the cited flow-control model is non-functional on this hardware as described).

### I6. `DL_UNITDATA_REQ` busy-gate semantics misstated
**Design claim (§4.4 step 1):** "Gate: is the single TX window free? … If busy → putq."

**Why it's incomplete:** In aen the gate is `q->q_first || !aenxmit(q,mp)` (`aen.c:792`). The `q->q_first` test is **ordering preservation** — if anything is already queued, this frame must queue behind it (don't reorder), independent of hardware readiness. The design's "is the TX window free" captures only the `!aenxmit` half and drops the `q_first` ordering guard. Copying without `q_first` lets a new DL_UNITDATA_REQ jump ahead of previously-deferred frames. Combined with I5 (Z3660 has no IRQ to drain the backlog), dropping `q_first` is doubly dangerous. Keep the exact `q->q_first || !z3660ethxmit(...)` test (`aen.c:792`, `hydra.c:869`).

**Severity: IMPORTANT.**

### I7. Open/close race + `OTHERQ`/`WR(q)->q_ptr` teardown not mentioned; "lazy init on first open" understates the EBUSY/clone logic
**Design claim (§4.2):** open does "minor→board/sap decode; lazy init on first open; ifstats splice."

**Why it's incomplete (several concrete gaps):**
- aenopen rejects `MODOPEN`/`CLONEOPEN` (`aen.c:107-108`) — must replicate or a STREAMS push/clone-open path mis-binds. Design omits.
- The slot-busy race: aenopen finds a free `aen_t` slot, then **rewrites `*devp`** via `makedevice(...)` (`aen.c:157`) to encode the chosen sap_index back into the minor (the pseudo-clone mechanism, minor&0xF0). Design's "minor→board/sap decode" misses that open *mutates the returned dev_t* — required for the clone-SAP (`/dev/zen0` minor 0) path to work with `slink addaen` opening the device twice (once under IP, once under ARP, `strcf:87-94`). If you don't re-encode `*devp`, the second open re-runs clone allocation and you can't address the same stream.
- `EBUSY` if `aen->q` already set (`aen.c:161-162`) — the only race guard. open/close run under STREAMS perimeter (single-threaded per queue on UP 68030), so there's no spinlock, but the design should state this reliance.
- aenclose nulls **both** `q->q_ptr` and `OTHERQ(q)->q_ptr` (`aen.c:200-201`) and only halts hardware on **last** close (`aen.c:206-231`). Design says "stop GEM on last close" (good) but omits the dual-q_ptr teardown — a porter who nulls only `q->q_ptr` leaves a dangling write-side pointer the next TX dereferences.

**Corrected fact:** Replicate MODOPEN/CLONEOPEN reject (`aen.c:107`), the `makedevice` dev_t rewrite (`aen.c:157`), the EBUSY guard (`aen.c:161`), and dual-q_ptr null in close (`aen.c:200-201`). **Severity: IMPORTANT** (the `slink addaen` double-open under IP+ARP depends on the clone/dev_t-rewrite path the design glossed).

---

## MINOR-class findings

### M8. streamtab/qinit shape — CORRECT, but note module_info bounds source
**Design claim (§4.1):** `module_info` HIWAT/LOWAT=5120/LOWAT; read qinit `{NULL,NULL,open,close,NULL,&minfo,NULL}` (no rput/rsrv); write qinit `{wput,wsrv,...}`; `streamtab={&rinit,&winit,NULL,NULL}`.

**Assessment: CORRECT.** Verified byte-for-byte against `aen.c:60-78`. The read-side has no put/srv (RX is `putnext` from interrupt — `aen.c:1342`); HIWAT=`AEN_HIWAT`=5*1024=5120 (`aen.h:44`), LOWAT=2/3 of that (`aen.h:43`). One nit: `AEN_MIN`/`AEN_MAX` in module_info are `64-ETH_CRC_LEN=60` and `AEN_MTU` (`aen.h:41-42,39`) — the design says "MIN/MAX/HIWAT=5120/LOWAT" conflating fields; module_info packet bounds are MIN=60/MAX=1500, the water marks are the separate HIWAT/LOWAT. Minor labeling slip, values are right. **Severity: MINOR.**

### M9. MTU/trailers — CORRECT, with one omission
**Design claim (§4.3):** `dl_max_sdu=1500, dl_min_sdu=1`; "(trailer decode — moot with -trailers)".

**Assessment: CORRECT.** `dl_max_sdu=ETH_MAXDATA=1500`, `dl_min_sdu=1` (`aen.c:857-858`); `-trailers` in the `ifconfig` line (Survey F) disables the trailer path so the `sap>=ETHERTYPE_TRAIL` branch (`aen.c:1187-1209`) is dead. **Omission:** the design's §4.5 says "trailer decode — moot" but `toss_packet_up_stream` still *executes* the trailer test on every frame (`aen.c:1187`); since you're reusing it "almost verbatim," the trailer arithmetic (`aen.c:1202-1207`) stays in the code path even if unused. Harmless, but the `length = count - ETH_CRC_LEN` in the *else* branch (`aen.c:1212`) is the live path and ties back to B2. **Severity: MINOR.**

### M10. `dl_min_sdu=1` vs TX min-frame padding — consistent but worth one line
aen advertises `dl_min_sdu=1` (accepts 1-byte payloads from IP) and pads short TX frames up to `ETH_MINDATA`/min frame on the wire (`aen.c:368-369` computes `max(...,ETH_MINDATA)` for the length). The design §4.4 step 2 says "Pad to min frame if short (boundary per Q4)" — correct to flag, but note aen's TX pad target is `ETH_MINDATA=50` payload (→ 64-byte frame incl header, pre-CRC), `aen.h:53`, not 60. The Z3660 firmware/GEM may auto-pad TX to 60 itself (GEMs typically do); confirm to avoid double-pad. **Severity: MINOR.**

### M11. `canput` (not `bcanput`) is the RX flow-control check — design says "canput check" — CORRECT
Both deliver paths gate on `canput(aen->q->q_next)` (`aen.c:1245,1289`) and bump `couldnt_put`/`allocbs_failed` counters on failure (`aen.c:1262,1268`). The design's §4.5 "`canput` check" is accurate. Note it's `canput` on `q_next` (the stream above), not `bcanput` with a band — fine for band-0 IP traffic. **Assessment: CORRECT.**

---

## Summary table

| # | Severity | Issue | Authoritative cite |
|---|---|---|---|
| B1 | BLOCKER | DLPI set omits DL_ATTACH/DL_DETACH; design claims "mirror hydra" but lists aen's 5 | `hydra.c:873-892, 996-1015` vs `aen.c:789-926` |
| B2 | BLOCKER | RX length/CRC: aen does `-ETH_CRC_LEN`, firmware delivers +FCS, guest does `-14 only` — 3 conventions; design asserts wrong/unqualified math | `aen.c:1212`; `ethernet.c:540,553`; `device.c:643` |
| I3 | IMPORTANT | `dl_sap`(32b)→`sap`(u_short) truncate + 2-byte no-swap copy; "32-bit big-endian" note invites wrong htons | `aen.c:801,341,1270`; `aen.h:78` |
| I4 | IMPORTANT | "broadcast flows naturally" true only because non-prom path does NO dest filter; confirm GEM filter mode | `aen.c:1270-1287` |
| I5 | IMPORTANT | `aenwsrv` no-op; real drain = TX-done IRQ which Z3660 lacks (sync TX) → design's defer model has no resume trigger | `aen.c:418,1357-1380`; `ethernet.c:1080-1090` |
| I6 | IMPORTANT | UNITDATA gate drops the `q->q_first` ordering guard | `aen.c:792` |
| I7 | IMPORTANT | open omits MODOPEN/CLONEOPEN reject, `makedevice` dev_t-rewrite (clone/double-open under IP+ARP), EBUSY, dual-q_ptr close teardown | `aen.c:107,157,161,200-201` |
| M8 | MINOR | module_info field labeling slip (values correct) | `aen.c:60-63`; `aen.h:41-44` |
| M9 | MINOR | trailer branch still executes; live path is the `-CRC` else (→B2) | `aen.c:1187-1212` |
| M10 | MINOR | TX pad target is ETH_MINDATA=50 (64-byte frame), GEM may auto-pad — avoid double-pad | `aen.c:368`; `aen.h:53` |
| M11 | MINOR/OK | `canput` RX flow-control check — correct as stated | `aen.c:1245,1289` |

**Net:** The streamtab/qinit shape (M8/M11), MTU/trailers (M9), and the "RX is putnext-from-interrupt" model are correctly captured. The two BLOCKERs are real and both concern things the design either contradicted itself on (B1: claims hydra, lists aen) or deferred too casually (B2: the CRC boundary is the documented ARP-killer and the three sources genuinely disagree — resolve it from the firmware GEM config + a one-frame length probe, not "later on real HW"). I5 is the subtlest: the design transplanted aen's flow-control idiom onto hardware with no TX-completion interrupt, so the deferred-TX path as written can stall.

## LENS — build-integration
The slot 48 row is confirmed (13 fields + trailing comma = 14 tokens). The design's choice of major 48 with the 13-field replacement is correct. The "major 7" alternative the survey mentioned is wrong (slot 7 `/*7=win?*/` is a real reserved entry, but it's `nostr` so technically replaceable — still, 48 is cleaner). 

I now have all the evidence needed. Let me compile the adversarial review.

# ADVERSARIAL REVIEW — Build-Integration Lens

I re-read the actual sources: `master.d/kernel.c`, the top-level + `amiga/` + `driver/` + `aen`/`hydra` Makefiles, `build-kernel.sh`, `build-clean-kernel.sh`, `driver.conf`, and the kerntools README/templates. Findings below, ranked.

---

## BLOCKER-1 — The whole edit plan is anchored on `hydra*` lines that may not exist on the build box

**Claim (design §6.1, §6.3 step 3):** "apply the three anchored `kernel.c` insertions (anchor on the unique `hydra*` lines)… add `extern struct streamtab z3660ethinfo;` after `kernel.c:266` (`extern struct streamtab hydrainfo;`)… add `z3660ethintr,` after `hydraintr,` at `kernel.c:194`… add `z3660ethinit,` after `hydrainit,` at `kernel.c:61`."

**Why it's wrong/incomplete:** Every cited line number and every sed anchor comes from the **`hydra-amix` reference clone** (`~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys`), which the project context itself calls "a hydra-amix clone = a real Amix /usr/sys." But the kernel the box actually builds is the **golden image's `/usr/sys`**, whose provenance is documented as "a copy of the proven `Amix-dbg.hdf` build box from the amix-a4091 project" (`amix-kerntools/README.md:21-22`). There is no evidence the golden image's `kernel.c` contains the `hydra` driver at all — `hydra` is an out-of-tree third-party driver. If the box's `kernel.c` has **no `hydrainit` / `hydraintr` / `hydrainfo` lines**, every `sed`/`awk` anchor in §6.3 silently matches nothing and inserts nothing → the driver compiles into `driver/z3660eth/exp` but is **never registered** (no cdevsw row, no ISR, no probe). The kernel links clean and boots, `open("/dev/zen0")` returns ENODEV/ENXIO from an empty cdevsw slot, and the failure looks like a driver bug, not a missing patch.

**Corrected fact:** The kerntools STREAMS path must **pull the live on-box `master.d/kernel.c` first** and detect which net drivers are present (`grep aeninfo` is the safer anchor — `aen` is stock Commodore and far more likely present than the third-party `hydra`; `aeninfo` is at `kernel.c:265`/`:357`/`int2_tbl` `aenintr` `:193`/no init_tbl entry). The design's own §6.3 step 3 hedges ("or keep known-good copies") but **no pristine `kernel.c` exists in kerntools today** (verified: `find amix-kerntools -name 'kernel.c*'` → nothing; `templates/` holds only `sd.c.in` + `alien-Makefile.in`). Phase 0 must add "pull and inspect the real box `kernel.c`; choose anchors that actually exist there" as a hard gate, and the anchor must fall back to `aen` (which has no `init_tbl` entry — see IMPORTANT-2). **Severity: BLOCKER** (the integration silently no-ops on the actual target).

---

## BLOCKER-2 — The clean-gate's force-touch is hardwired to `amiga/alien/` and cannot rebuild a `driver/` subdir or `kernel.c`

**Claim (design §6.3 step 5):** "Run the **unchanged** clean-gate… **but extend its force-touch/rm set** to include `master.d/kernel.c` … and `amiga/driver/z3660eth/z3660eth.c`. The gate's stabilization/sum-recurrence/`checkunix` logic is reused verbatim."

**Why it's wrong/incomplete:** This is not a config knob — it's a **hardcoded `cd`** in `tools/build-clean-kernel.sh:27`:
```sh
cd /usr/sys/amiga/alien; rm -f $OBJS exp; touch $SRCS
cd /usr/sys; rm -f relocunix unix OLDrelocunix amiga/exp amiga/config/unix.o master.d/exp
```
Passing `z3660eth.c` as a `$SRCS` arg makes it `touch z3660eth.c` **inside `amiga/alien/`**, where that file does not exist (the net driver lives in `amiga/driver/z3660eth/`). `touch` of a nonexistent file is a no-op/error, the real subdir `.o` is never invalidated, and the driver's edits **don't get recompiled** between gate iterations. Worse: the line removes `amiga/exp` and `master.d/exp` but **not** `master.d/kernel.o` nor `amiga/driver/exp` nor `amiga/driver/z3660eth/z3660eth.o`. So:
- After the `kernel.c` edit, `master.d/exp` is removed (forcing a master.d relink) — **but `kernel.o` itself is only rebuilt if make's timestamp dep fires**, and the project memory explicitly warns "Makefile has no header-dep tracking" (MEMORY: wip-emulated-ram). Removing `master.d/exp` alone does NOT force `kernel.c → kernel.o` recompilation; you can relink a **stale** `kernel.o` that still has the empty cdevsw slot.
- `amiga/driver/exp` and `amiga/driver/z3660eth/z3660eth.o` are never removed, so the first gate build may link a stale/absent driver object.

**Corrected fact:** `build-clean-kernel.sh` needs a real code change (not "verbatim reuse"): for net drivers it must `rm -f /usr/sys/amiga/driver/z3660eth/z3660eth.o /usr/sys/amiga/driver/z3660eth/exp /usr/sys/amiga/driver/exp /usr/sys/master.d/kernel.o /usr/sys/master.d/exp` and `touch` the **correct** subdir source path each round. The cleanest fix is to generalize the touch/rm to take **full on-box paths** rather than bare basenames `cd`'d into `alien/`. The top-level `relocunix` rule does recurse into `amiga/` (verified: `Makefile` `relocunix` target runs `cd $(PLATFORM); $(MAKE)`, and `amiga/Makefile O = … driver/exp …`, and `driver/Makefile` recurses into `aen/exp hydra/exp`), so the subtree IS reachable — but only the **explicit `rm` of the stale objects** guarantees the gate measures a kernel that actually contains your edits. **Severity: BLOCKER** (gate can certify-clean a kernel that doesn't contain the driver or the registration).

---

## IMPORTANT-1 — `driver/Makefile` `OBJ` edit is necessary AND the gate never rebuilds it

**Claim (design §6.2):** "Plus two insertions in `amiga/driver/Makefile` … add `z3660eth/exp` to `OBJ`, and a `z3660eth/exp:` rule."

**Why incomplete:** The edit itself is correct and required (verified `driver/Makefile` `OBJ = … aen/exp hydra/exp`, with per-subdir `aen/exp:\n\tcd aen; $(MAKE)` rules). But two gaps:
1. `driver/Makefile`'s `exp` rule is `ld -r -o exp $(OBJ)` and `$(OBJ)` includes `aen/exp hydra/exp z3660eth/exp` as **file targets with recipes** — make will only re-link `driver/exp` if one of those `*/exp` files is newer. The gate must `rm -f amiga/driver/exp` (see BLOCKER-2) or the new `z3660eth/exp` is built but never folded into `driver/exp`.
2. The push step (§6.3 step 4) pushes "patched `driver/Makefile` → `amiga/driver/`" — fine — but if the box `driver/Makefile` differs from the hydra-amix reference (same provenance risk as BLOCKER-1; e.g. no `hydra/exp` line on the box), the hydra-anchored `OBJ` insertion fails the same way. Anchor on `aen/exp` (stock) not `hydra/exp`. **Severity: IMPORTANT.**

---

## IMPORTANT-2 — `init_tbl[]` entry is NOT how `aen` works, and a boot-time probe that touches absent hardware is risky on the WinUAE box

**Claim (design §4.2/§6.1 #3, §9 Phase 1):** wire `z3660ethinit` into `init_tbl[]` (mirroring `hydrainit`), and "Gate = boots without panic, registers in cdevsw, `open` returns ENXIO."

**Why incomplete / subtly wrong:** Survey C/E correctly note `aen` has **NO `init_tbl[]` entry** — it autoconfigures lazily on first `open()`. Only `hydra` uses `init_tbl` (verified: `init_tbl[]` at `kernel.c:44-62` contains `hydrainit` but not `aeninit`). The design picks the hydra model. That's defensible, but it means **`z3660ethinit` runs at boot on the WinUAE build box**, which has **no Z3660 GEM** — so the probe must be bulletproof against "board absent" or it can hang/panic boot on the gate box (the very box the Phase-1 gate depends on). The design's ENXIO-safety argument (§4.6) is about `open()`, but with the hydra/init_tbl model the **boot-time probe** runs first and unconditionally. The probe must early-return cleanly when Zorro autoconfig finds no `0x144B/0x1` board. Given the box emulates an a2065 (mfg differs), this should be fine — but it is an **untested boot-path on the gate box**, not the no-op the design implies. Safer: copy `aen`'s **lazy-open** model (no `init_tbl` entry at all) for Phase 1, which makes "boots without panic" trivially true regardless of probe correctness. **Severity: IMPORTANT** (wrong-model choice raises gate-box boot risk for no benefit in Phase 1).

---

## IMPORTANT-3 — The `ifstats` symbol: driver declares `extern struct ifstats *ifstats;`, kernel.c defines `int ifstats;`

**Claim:** Surveys C/F say the driver self-attaches by splicing into the global `ifstats` list (`extern struct ifstats *ifstats;`).

**Why it matters for build/link:** In the actual `kernel.c` the global is defined as **`int ifstats;`** (`kernel.c:185`), while both `aen.c:43` and `hydra.c:36` declare `extern struct ifstats *ifstats;`. These are the **same link symbol with conflicting C types** — it links only because K&R/SVR4 `ld` doesn't type-check externs, and `sizeof(int) == sizeof(pointer)` on m68k (both 4 bytes), so `ifstats = &board->ifstats` stores a pointer into an `int` slot. This is load-bearing and **fragile**: it works today for aen/hydra so it will work for the port (the port copies the same `extern`), but the design never mentions `ifstats` at all in the kernel-integration section. If a future cleanup "fixes" the type or if the port author declares it differently (e.g. `struct ifstats ifstats;` non-pointer), the list head breaks silently and the interface never becomes visible to `ifconfig`. **Action:** copy the `extern struct ifstats *ifstats;` line verbatim from aen; do not redeclare. **Severity: IMPORTANT** (silent inet-invisibility if mishandled; currently un-documented in the plan).

---

## IMPORTANT-4 — `driver.conf` `net` stanza parser collides with the existing positional SCSI parser

**Claim (design §6.3):** add a typed line `net z3660eth.c major=48 streamtab=… init=… intr=… "Z3660 Ethernet"` and "Parse `net` stanzas … keep `scsi` stanzas on the existing path."

**Why incomplete:** The current `build-kernel.sh` loop reads **positionally**: `while read -r pn fn rest` then `base="${fn%queue}"` (`build-kernel.sh:69,84`). A `net …` line would be parsed as `pn=net`, `fn=z3660eth.c`, and `base="${fn%queue}"` = `z3660eth.c` (no `queue` suffix to strip), then `[ -f "$d/src/$src" ]` with `src` mis-extracted from `rest` → the existing loop either errors (`missing …/src/…`) or worse, silently generates a bogus `scsicard[]` row with `&z3660eth.c` as a function pointer. The design says "keep scsi stanzas on the existing path" but the existing path has **no type dispatch** — adding `net` requires a real `case "$pn" in net) … ;; *) <existing scsi> ;; esac` rewrite of the read loop, plus the z3660 repo's **own `driver.conf` currently has only the SCSI line** (verified) so the eth line lives in a *new* conf or a new repo. This is more than "parse net stanzas" — it's a parser refactor with back-compat risk to the proven A4091/Z3660 SCSI builds. **Severity: IMPORTANT** (must not regress the working SCSI path).

---

## MINOR-1 — Cross-compile vs on-box link: correctly on-box, but CFLAGS/include depth must match

**Claim (design §6.2):** subdir Makefile `CFLAGS = … -I../../.. -I../../inc`.

**Assessment — mostly correct.** Verified `aen/Makefile` and `hydra/Makefile` both use `-I../../.. -I../../inc` (one level deeper than `alien/`'s `-I../.. -I../inc`). The whole build is **on-box native `gcc`/`ld`** (`build-kernel.sh` pushes source and runs `make` on the Amix box via amixsh; there is no host cross-compiler) — so there is no cross-compile mismatch risk; the design's "build on WinUAE" is on-box-native and correct. One real gap: the hydra/aen Makefiles append **`$(KDB)`** to CFLAGS and list `../kdb/kdebug.h` as a header dep (`hydra.o : hydra.h hydrauser.h ne2000.h ../kdb/kdebug.h`). The design's proposed z3660eth Makefile drops `$(KDB)` and the kdb header dep — **fine** (KDB is empty in the non-debugger build), but the proposed `z3660eth.o : z3660eth.h` dep line means editing any *other* shared header won't trigger a rebuild — combined with BLOCKER-2's missing force-rm, header edits can ship stale objects. Mitigated entirely by the gate force-`rm` fix. **Severity: MINOR.**

---

## MINOR-2 — cdevsw row field count is correct, but the "major 7" alternative (Survey E) is wrong

**Claim (Survey E §1, echoed as an option):** "The next genuinely free low slot is **major 7**."

**Why wrong:** Slot 7 is **not empty** — it is a reserved/occupied row `ND,…,notty,nostr,nullflag, /*7=win?*/` (`kernel.c:342`). It is `nostr` (no streamtab) so technically overwritable, but it carries a `/*7=win?*/` comment suggesting an intended device. The design's actual choice — **major 48** — is correct: slot 48 is a clean `nostr` filler with no comment (`kernel.c:398`), and the 13-field replacement row the design specifies matches the char-cdevsw column count exactly (verified: slot rows in the char table are 13 fields: 10×`ND` + `notty` + `&info` + `nullflag`). Just don't fall back to 7. **Severity: MINOR** (the primary pick is right; flag the bad alternative).

---

## MINOR-3 — `make install` + `/stand make` boot path: correct, with one caveat

**Claim (design §9, §6.3):** deploy via `cd /usr/sys; make install` + `cd /stand; make`, real-HW iface `192.168.2.39`, never re-image root.

**Assessment — correct and matches the proven SCSI flow.** Verified `build-kernel.sh:160` does exactly `python3 "$AMIXSH" "cd /usr/sys; make install" "cd /stand; make"` and keeps `OLDunix`. The relocunix-shuttle-to-real-box step (transfer.hdf c5d0 → real-box `make install`) is the established A4091/Z3660 routine. One caveat the design glosses: `make install` stages the **whole kernel**, and the real box must already have the patched `kernel.c` + the `driver/z3660eth/` subtree + patched `driver/Makefile` present on its `/usr/sys` (the shuttle moves the built `relocunix`, but `/stand make` on the real box rebuilds the boot partition from the **staged** kernel — so the real box's `/usr/sys` source must match what was gated on WinUAE, or you relink a different kernel on the real box). The two-tier loop must shuttle either the **built relocunix** (and skip real-box relink) OR the **full source set** (and relink on the real box) — consistently, not mixed. The design's Phase plan says "shuttle clean relocunix … real box `make install`" which is the relink-on-real-box variant and therefore needs the source on the real box too. Worth making explicit. **Severity: MINOR.**

---

## Sections that are CORRECT

- **cdevsw row format / major 48** (§6.1 #1): the 13-field `ND×10,notty,&z3660ethinfo,nullflag` row is exactly right for a leaf STREAMS driver, `nullflag` (newstyle) not `oldflag`, matching aen/hydra (`kernel.c:357/397`). ✔
- **No `fmodsw[]` / `unix.c` / second registry** (§6.1, Survey E §4): confirmed — major↔streamtab binding is purely cdevsw slot position; a leaf driver is not a pushable module. ✔
- **int2_tbl is a NULL-terminated poll-all array; append-before-`0`** (§6.1 #2): confirmed structurally (`int2_tbl[]` `kernel.c:188-195`, terminated by `0}`). Externs precede the array (`kernel.c:187`). ✔ — *but the level (INT2 vs INT6, Q1) remains genuinely open and is correctly flagged as such; the build mechanics of adding the row are right regardless of level.*
- **Subdir build is reachable**: `amiga/Makefile O = … driver/exp …` → `driver/Makefile` recurses into subdirs → top `relocunix` rule `cd amiga; make`. The new subdir WILL be compiled and linked once the two `driver/Makefile` edits land. ✔
- **Clean-gate sum-recurrence + checkunix logic is genuinely reusable** (the stabilization loop itself) — only its hardcoded `alien/` paths need changing (BLOCKER-2), not its retry/sum logic. ✔

---

## Bottom line for the build-integration lens

The plan's **mechanics are right** (cdevsw row, int2_tbl/init_tbl append, subdir Makefile, `ld -r` exp, on-box native build, install/boot path) but rest on **two unbuilt/unsafe assumptions that will fail in practice**:

1. **BLOCKER-1**: anchoring edits on `hydra*` lines that likely don't exist on the golden build box (anchor on `aen` + pull-and-inspect the live `kernel.c` first; no pristine copy exists in kerntools yet).
2. **BLOCKER-2**: claiming the clean-gate is reused "unchanged" when its force-touch is `cd alien/`-hardcoded and cannot invalidate `driver/z3660eth/*.o` **or** `kernel.o` — so the gate can certify a clean kernel that silently lacks the driver or its registration (compounded by the known no-header-dep-tracking Makefiles).

Both are fixable in Phase 0/1 but **must** be done before any gate run is trusted. Add to Phase 0: (a) pull live `kernel.c`/`driver/Makefile` from the box and verify anchors; (b) rewrite `build-clean-kernel.sh`'s touch/rm to take full on-box paths covering `amiga/driver/z3660eth/{z3660eth.o,exp}`, `amiga/driver/exp`, and `master.d/kernel.o`; (c) decide aen-style lazy-open (safer for the gate box) vs hydra-style `init_tbl` probe. Secondary: IMPORTANT-1..4 (Makefile anchor, init model, `ifstats` extern, driver.conf parser refactor without regressing the proven SCSI path).

Key files: `~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys/master.d/kernel.c` (reference, lines 38/61/185/187/193-194/265-266/342/357/397/398); `~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys/{Makefile, amiga/Makefile, amiga/driver/Makefile, amiga/driver/{aen,hydra}/Makefile}`; `~/Devel/Omat/Amiga/Amix/amix-kerntools/{build-kernel.sh:69-145, tools/build-clean-kernel.sh:27-28, README.md:21-22, templates/}`; `~/Devel/Omat/Amiga/Amix/amix-z3660/driver.conf`.


# SURVEY EVIDENCE (6 deep-reads)

## SURVEY A — Z3660 GUEST protocol
I have everything needed for a precise protocol contract. Both the guest driver (what AMIX must speak) and the firmware (what the guest is talking to) are fully cross-verified. Here is my survey.

---

# SURVEY A — Z3660 Ethernet GUEST-SIDE Protocol Contract

The AmigaOS guest driver is **`Z3660Net.device`**, a fork of MNT's ZZ9000Net (Henryk Richter / Lukas Hartmann). The AMIX STREAMS/DLPI driver must speak the **identical** memory-mapped mailbox protocol this driver uses. All register offsets are from the shared header `z3660-drivers/common/z3660_regs.h` (identical to firmware `rtg/zzregs.h`). I cross-verified every register against the firmware dispatch in `z3660-firmware/.../rtg/rtg.c` and `ethernet.c`.

## 0. Card base and register window

- The card is found via `expansion.library` `FindConfigDev(mfg=0x144B, prod=0x1)`. The Zorro board base is captured into the global `ZZ3660_REGS = cd->cd_BoardAddr` (`device.c:161-166`). **Everything below is an absolute 68k address = `ZZ3660_REGS + offset`.**
  - AMIX equivalent: the driver must obtain this base from Zorro autoconfig (mfg `0x144B`/`0x244B`, prod `0x1`) — the same board the SCSI/RTG side enumerates. `device.c:164` comments "Find Z2 or Z3 model".
- All eth registers are **32-bit (ULONG), big-endian, accessed as `volatile`**. The firmware byte-swaps internally; the guest reads/writes plain longs.
- The `0x100`–`0x2FF` range is the RTG/control register file (memory-mapped command interface). The frame data windows live at large board-relative offsets (`0x07EE0000` etc.) — see §3.

## 1. TX — sending a frame

**Code:** `write_frame()` `device.c:693-747`, invoked from `DevBeginIO` `CMD_WRITE`/`S2_BROADCAST` at `device.c:495-508`.

Sequence the guest performs:

1. The TX frame staging buffer is the absolute address **`ZZ3660_REGS + TX_FRAME_ADDRESS`** where `TX_FRAME_ADDRESS = 0x07EE0000` (`z3660_regs.h:278`; firmware `memorymap.h:33`). `DevBeginIO` passes this pointer to `write_frame` (`device.c:499`).
2. Build the ethernet frame **in place** at that buffer (`device.c:702-710`):
   - **Cooked (normal) mode:** the driver writes a full 14-byte ethernet header itself: `dst[6]` = `ios2_DstAddr`, `src[6]` = `HW_MAC`, `type[2]` = `ios2_PacketType` (written at `frame+12` as a USHORT, big-endian). Then `bm_CopyFromBuffer` copies the payload to `frame + 14`. `sz = ios2_DataLength + 14`.
   - **Raw mode (`SANA2IOF_RAW`):** the client already supplied a complete frame including header; `sz = ios2_DataLength`, header is NOT synthesized, `bm_CopyFromBuffer` copies the whole thing to `frame`.
   - **NOTE:** TX has **no length/serial prefix** — unlike RX, the TX window holds the raw ethernet frame starting at offset 0. The length is conveyed out-of-band via the trigger register (next step).
3. **Trigger the send** by writing the frame length to `REG_ZZ_ETH_TX` (`device.c:733-736`):
   ```c
   volatile ULONG *reg = (ULONG*)(ZZ3660_REGS + REG_ZZ_ETH_TX);   // 0x190
   *reg = (ULONG) sz;        // write length => kick TX
   rc  = *reg;               // read back => completion result
   ```
   - `REG_ZZ_ETH_TX = 0x190` (`z3660_regs.h:86`).
4. **Completion signal = synchronous.** The write blocks in firmware: `REG_ZZ_ETH_TX` write → `ethernet_send_frame(zdata)` (`rtg.c:1609-1611`), which DMAs the frame and **busy-waits for TX completion** (up to ~1 ms, `ethernet.c:1080-1090`) before returning. The result is latched in `ethernet_send_result`. The guest's **read-back of `REG_ZZ_ETH_TX`** returns that result (`rtg.c:803-804`): `0` = OK, nonzero = error (1=not ready, 2=BdRingAlloc fail, 3=BdRingToHw fail, 4=TX timeout — `ethernet.c:1033/1058/1070/1088`). Guest maps nonzero → `S2ERR_NO_RESOURCES` (`device.c:740,501`).

**Contract for AMIX TX:** write the ethernet frame (cooked: build the 14-byte header yourself from dst + own MAC + type; raw: copy verbatim) to absolute `base+0x07EE0000`, then write the byte length to register `base+0x190`, then read `base+0x190` back; nonzero = failure. The call is blocking/synchronous — no TX completion interrupt is used.

## 2. RX — receiving frames

**Code:** ISR `dev_isr()` `device.c:78-92`; receiver task `frame_proc()` `device.c:749-850`; `read_frame()` `device.c:622-691`; serial helper `get_frame_serial()` `device.c:615-620`.

### 2a. Notification — interrupt-driven (with a poll fallback path present but unused)
- The card raises **Amiga INT6** (level-6 autovector, `INTB_EXTER` server chain). Optionally INT2 (`INTB_PORTS`) if `DEVF_INT2MODE`, but that needs a hardware board mod and is disabled in this build (`device.c:233-242`, `device.c:309`). **AMIX target = INT6.**
- ISR reads `REG_ZZ_INT_STATUS` (`0x1A8`) and checks **bit 0 = ethernet** (`device.c:80-83`):
  ```c
  ULONG status = *(ULONG*)(ZZ3660_REGS + REG_ZZ_INT_STATUS);  // 0x1A8
  if (status & 1) { ... }      // bit0 = AMIGA_INTERRUPT_ETH
  ```
  Firmware bit assignments (`interrupt.h:19-21`): **ETH=1, AUDIO=2, USB=4** (these are the bits in `REG_ZZ_INT_STATUS`; firmware `rtg.c:788-789` returns `amiga_interrupt_get()`).
- **Acknowledge:** write `REG_ZZ_CONFIG` (`0x104`) = `8 | 16` (`device.c:85`):
  ```c
  *(ULONG*)(ZZ3660_REGS + REG_ZZ_CONFIG) = 8 | 16;   // bit3=ack-strobe, bit4=clear ETH
  ```
  Firmware semantics (`rtg.c:1150-1172`): if `data & 8` it's an ack-strobe, then `data & 16` → `amiga_interrupt_clear(AMIGA_INTERRUPT_ETH)`; `&32` clears AUDIO, `&64` clears USB. (For enable, see §2d.) When the interrupt set returns to 0 the firmware drops the physical INT6 line (`interrupt.c:43-52`, `DiscreteClear(REG0,FPGA_INT6)`).
- The ISR then `Signal()`s the receiver task (`device.c:88-90`). In AMIX/STREAMS this maps to: ISR acks + schedules a soft-int / `qenable` that drains the backlog.

**Important coherency caveat for AMIX (level-trigger + nag):** the firmware does NOT only raise INT6 on a fresh frame edge. A background "nag" proto-thread re-asserts `AMIGA_INTERRUPT_ETH` whenever `ethernet_get_backlog() > 0` and a counter expires (`rtg.c:327-336`, `463-465`). So the line is effectively **level/backlog-driven**: the AMIX ISR must drain *all* pending backlog frames per interrupt, or it will keep re-firing. The original driver's `frame_proc` loops via serial comparison (below) rather than a backlog count, so it relies on this re-nag to pick up frames it missed.

### 2b. Reading a frame — the RX-address register + slot layout
The receiver task does (`device.c:796-840`):

1. Read **`REG_ZZ_ETH_RX_ADDRESS` (`0x1A4`)** to get the **board-relative offset** of the current RX slot:
   ```c
   uint32_t off = *(ULONG*)(ZZ3660_REGS + REG_ZZ_ETH_RX_ADDRESS);   // 0x1A4
   volatile UBYTE *frm = (UBYTE*)(ZZ3660_REGS + off);
   ```
   - **Critical base subtlety:** the firmware returns `ethernet_current_receive_ptr()` = `RX_BACKLOG_ADDRESS + frames_received_from_backlog*FRAME_SIZE` **without** RTG_BASE (`ethernet.c:596-597`, `rtg.c:806-808`). The guest adds its own `ZZ3660_REGS` board base. So the returned value is a **board-relative byte offset**, and the slot lives at `base + offset`. `RX_BACKLOG_ADDRESS = 0x07ED0000`, `FRAME_SIZE = 2048` (`memorymap.h:32,40`). So slot N is at `base + 0x07ED0000 + N*2048`.

2. **Slot header (4-byte prefix, big-endian)** — written by firmware at `ethernet.c:558-561`, parsed by guest at `device.c:631-633` and `read_frame` `device.c:630-635`:
   | bytes | field | meaning |
   |---|---|---|
   | `[0..1]` | `size` | frame length in bytes (= `rx_bytes` from the GEM, includes the 14-byte eth header) |
   | `[2..3]` | `serial` | 16-bit frame serial number (monotonic, wraps; firmware `frame_serial++` per frame) |
   | `[4..]` | frame | the raw ethernet frame: `dst[6] src[6] type[2] payload…` |

   `RX_FRAME_PAD = 4` (`memorymap.h:39`) is exactly this prefix; firmware `memcpy(frame_bl_ptr+4, frame_ptr, rx_bytes)` (`ethernet.c:553`).

3. **New-frame detection = serial comparison** (`device.c:803-810`). The task keeps `old_serial`; if `get_frame_serial(frm)` (`= frm[2]<<8 | frm[3]`, `device.c:615-619`) differs, a new frame is present. (There is no count register the guest reads; it polls the serial of the *current* slot.)

4. **Demux by ethernet type**: `packet_type = frm[16]<<8 | frm[17]` (`device.c:809`; offset 16 = 4-byte prefix + 12 into the eth header = the type field). The task walks `db_ReadList` and delivers to the first pending `IOSana2Req` whose `ios2_PacketType` matches (`device.c:813-830`). This is SANA-II per-protocol-type demux; **the AMIX/DLPI equivalent is SAP/ethertype demux** to bound streams.

5. **Copy out** via `read_frame` (`device.c:622-691`):
   - Cooked: payload pointer = `frm + 4 + 14`, `datasize = size - 14`; src/dst MACs copied from the slot (`device.c:666-667`), packet type into `ios2_PacketType`, broadcast flag set if dst == ff:ff:ff:ff:ff:ff (`device.c:677-686`).
   - Raw (`SANA2IOF_RAW`): pointer = `frm + 4`, `datasize = size` (full frame incl header).
   - Uses `bm_CopyToBuffer` callback to move bytes to the client buffer (`device.c:656`).

### 2c. Acknowledge / advance the RX slot
After processing the current slot, the guest writes **`1` to `REG_ZZ_ETH_RX` (`0x194`)** (`device.c:838-839`):
```c
*(ULONG*)(ZZ3660_REGS + REG_ZZ_ETH_RX) = 1L;   // "frame accepted", advance backlog
```
Firmware (`rtg.c:1613-1615` → `ethernet_receive_frame()` `ethernet.c:604-638`): increments `frames_received_from_backlog`; when it catches up to `frames_backlog` it resets the backlog to empty and "fakes" the serial of the now-current slot so the guest won't re-process a stale slot (`ethernet.c:619-631`). **The value written (`1`) is ignored by firmware; it's a pure "advance" strobe.**

### 2d. Backlog ring summary (what AMIX must model)
- Ring of **`FRAME_MAX_BACKLOG = 64`** slots (`ethernet.c:59`), each **`FRAME_SIZE = 2048` bytes** (`memorymap.h:40`), based at board offset `0x07ED0000`. (The memorymap comment at `:32` says "32 * 2048" but the actual code uses 64 — trust the code; total reserved window is 1 MB-class, see §3.)
- Producer (firmware ISR `xEmacPsRecvHandler` `ethernet.c:510-594`): on GEM RX, `memcpy` frame into `backlog[frames_backlog]`, writes size+serial header, `frames_backlog++`. Overflow → resets ring to empty (drops backlog) `ethernet.c:566-572`.
- Consumer (guest): reads slot at `ethernet_current_receive_ptr()`, advances via `REG_ZZ_ETH_RX` write.
- **The pointer the guest reads (`REG_ZZ_ETH_RX_ADDRESS`) always points at the consumer's current slot**, so the guest does not compute slot addresses itself — it just reads the register, processes, strobes advance, repeats while serial changes.

## 3. Complete MMIO map the guest touches

| Symbol | Offset | Dir | Role | Source |
|---|---|---|---|---|
| `REG_ZZ_CONFIG` | `0x104` | W | IRQ enable / ack-strobe. `=1` enable ETH int; `=0` disable; `=8\|16` ack+clear ETH | `z3660_regs.h:42`; guest `device.c:85,319,386`; fw `rtg.c:1150-1172` |
| `REG_ZZ_ETH_TX` | `0x190` | R/W | W length = trigger TX; R = TX result (0=ok) | `z3660_regs.h:86`; guest `device.c:734-739`; fw `rtg.c:803,1609` |
| `REG_ZZ_ETH_RX` | `0x194` | W | strobe = "frame accepted", advance backlog | `z3660_regs.h:87`; guest `device.c:838-839`; fw `rtg.c:1613` |
| `REG_ZZ_ETH_MAC_HI` | `0x198` | R/W | MAC bytes [0..1] (`(b0<<8)\|b1`) | `z3660_regs.h:88`; guest `device.c:202,209`; fw `rtg.c:865,1633` |
| `REG_ZZ_ETH_MAC_LO` | `0x19C` | R/W | MAC bytes [2..5] (`b2<<24\|b3<<16\|b4<<8\|b5`); write triggers `ethernet_update_mac_address()` | `z3660_regs.h:89`; guest `device.c:204-211`; fw `rtg.c:870,1639-1645` |
| `REG_ZZ_ETH_RX_ADDRESS` | `0x1A4` | R | board-relative offset of current RX slot | `z3660_regs.h:92`; guest `device.c:797-798`; fw `rtg.c:806-808`, `ethernet.c:596` |
| `REG_ZZ_INT_STATUS` | `0x1A8` | R | pending-interrupt bits: ETH=1, AUDIO=2, USB=4 | `z3660_regs.h:93`; guest `device.c:80`; fw `rtg.c:788`, `interrupt.h:19-21` |
| **`TX_FRAME_ADDRESS`** | **`0x07EE0000`** | W (mem) | TX frame staging buffer (raw eth frame at offset 0) | `z3660_regs.h:278`; guest `device.c:499`; fw `memorymap.h:33` |
| **`RX_BACKLOG_ADDRESS`** | **`0x07ED0000`** | R (mem) | RX backlog ring base (64 × 2048-byte slots, 4-byte size+serial prefix each) | fw `memorymap.h:32`, `ethernet.c:552,596`; guest reaches it indirectly via `REG_ZZ_ETH_RX_ADDRESS` |

`FRAME_SIZE = 2048` (`memorymap.h:40`), `FRAME_MAX_BACKLOG = 64` (`ethernet.c:59`), `RX_FRAME_PAD = 4` (`memorymap.h:39`).

Other RTG-file registers (`REG_ZZ_MODE 0x100`, etc.) are NOT touched by the eth path. `REG_ZZ_FW_VERSION 0x1A0` is available if version-gating is wanted (`rtg.c:791-793`) but the eth driver doesn't read it.

## 4. MAC address acquisition

`DevInit` (`device.c:201-211`): the **MAC is owned by the ARM/Zynq side now** (the ENV:Z3660_MAC override and Commodore-OUI defaults are commented out, `device.c:167-200`). The guest:
1. **Reads** the firmware's current MAC from `REG_ZZ_ETH_MAC_HI`/`_LO` into `HW_MAC[0..5]` (`device.c:202-207`).
2. **Writes it straight back** to `_HI`/`_LO` (`device.c:209-211`) to force a `XEmacPs_SetMacAddress` re-apply (firmware `rtg.c:1645` → `ethernet_update_mac_address()` `ethernet.c:651-668`, which Stop/SetMac/Start the GEM).

Firmware default MAC `EmacPsMAC = {0x00,0x80,0x10,0x00,0x01,0x00}` (`ethernet.c:81`) — Commodore OUI `00:80:10`. `S2_GETSTATIONADDRESS` returns `HW_MAC` (`device.c:534-536`).

**Contract for AMIX:** read `_HI`/`_LO` → 6-byte MAC; this is the station address to advertise to DLPI (`DL_INFO_ACK`/`DL_PHYS_ADDR_ACK`). Re-writing it back is optional but matches the reference (and lets AMIX override the MAC if desired by writing `_HI`/`_LO`). Byte packing: `_HI = (mac0<<8)|mac1`; `_LO = (mac2<<24)|(mac3<<16)|(mac4<<8)|mac5`.

## 5. SANA-II specifics that matter for the STREAMS/DLPI port

- **Cooked vs raw frames** (`SANA2IOF_RAW`, `sana2.h:81-89`): the hardware path always moves *whole ethernet frames* (header + payload). "Cooked" is purely a guest-side convenience where the driver synthesizes/strips the 14-byte header. **For DLPI map raw to `DL_RAW`/`DLIOCRAW`, cooked to normal `DL_UNITDATA`.** The 14-byte header layout is fixed: `dst[6] src[6] type[2]` (`device.h:132-139`, `HW_ETH_HDR_SIZE=14`).
- **Packet-type / ethertype is the demux key.** SANA-II tracks one `ios2_PacketType` per request; RX dispatch matches on `frm[16..17]` (`device.c:816`). DLPI bind-SAP is the analogue — the AMIX driver must demux incoming frames by ethertype to the bound stream, and for `type < 1500` (802.3 length) it should treat as the appropriate SAP (the reference does plain ethertype, no 802.3/SNAP special-casing).
- **Broadcast:** `S2_BROADCAST` sets dst = `ff:ff:ff:ff:ff:ff` then TX (`device.c:487-493`); RX sets `SANA2IOF_BCAST` when dst is all-FF (`device.c:677-686`). DLPI: handle `DL_UNITDATA_REQ` to the broadcast address and flag inbound broadcasts.
- **Multicast:** **NOT implemented.** `S2_ADDMULTICASTADDRESS`/`S2_MULTICAST` fall through `default:` → `S2ERR_NOT_SUPPORTED` (`device.c:573-578`). The firmware GEM is presumably in a receive mode that passes multicast/broadcast, but there is **no hardware multicast-filter programming register exposed** — so an AMIX `DL_ENABMULTI_REQ` cannot be honored at the hardware level via this protocol. (Mirror aen/hydra's behavior: accept the request but rely on promiscuous/all-multicast firmware behavior, or return not-supported.) **Flag this as an open question** — there is no multicast register in the contract.
- **Promiscuous:** `SANA2OPF_PROM` (`sana2.h:95-98`) is defined but the driver ignores it; no register for it. RX filtering is whatever the firmware GEM does.
- **MTU = 1500**, BPS = 100 Mbit advertised, HardwareType = Ethernet (`device.c:546-553`, `device.h:121 HW_ETH_MTU=1500`). Slot is 2048 bytes so a full 1500-MTU frame (+14 header +4 prefix = 1518) fits with margin. AddrFieldSize = 48 bits (`device.c:547`).
- **Buffer-copy callbacks** (`BufferManagement`, `device.h:125-130`): SANA-II hands the driver `S2_CopyToBuff`/`S2_CopyFromBuff` hooks (`device.c:262-265`) so the *client* controls payload memory. **This is the SANA-II indirection that does NOT exist in STREAMS** — in DLPI the driver `allocb`/`esballoc`s an mblk and `bcopy`s frame bytes from the shared slot into it (RX), or copies from the mblk into the TX window (TX). So the AMIX driver replaces `bm_CopyToBuffer(dst, frm+4+14, len)` with `bcopy(slot, mblk->b_wptr, len)` and `bm_CopyFromBuffer(txwin, data, len)` with `bcopy(mblk, txwin, len)`. There is **no DMA / scatter-gather to the client** — every frame is a CPU `memcpy` through the fixed TX/RX windows.

## 6. Cache-coherency contract (first-class concern, confirmed)

- The firmware maps the **entire eth window** (`0x07E00000`–`0x08000000`, covering TX_FRAME `0x07EE0000`, RX_FRAME `0x07EF0000`, RX_BACKLOG `0x07ED0000`) as **`ETHERNET_CACHE_POLICY` = Normal Non-Cacheable** on the ARM side: `main.c:683-691`, policy `= (0x14DE2 | (NC<<12) | (NC<<2))` with `NC=0b00` (`Z3660_emu/src/defines.h:15`). So on the **Zynq side the shared buffers are non-cached** — consistent with the task's `NORM_NONCACHE` note.
- **But the 68030 guest side has its own caches.** The reference guest driver accesses the windows through `volatile` only, and there is a **commented-out** `CacheClearE(frm, HW_ETH_HDR_SIZE, CACRF_ClearD)` right before `read_frame` (`device.c:817`) — i.e. the original author considered (and disabled) a D-cache flush on the RX header. The firmware TX path *does* explicitly flush ARM L1/L2 over `TxFrame` before DMA (`ethernet.c:1039-1040`).
- **Implication for AMIX:** the shared eth window must be mapped **non-cacheable / serialized in the 030 MMU** (this is the same class of bounce-buffer coherency the SCSI path and the open "lpsched Bad vp" bug touch). The protocol's correctness depends on the guest *not* caching the TX/RX windows or the `REG_ZZ_*` register file, and on ordering the "write frame → kick TX" and "read RX_ADDRESS → read slot → strobe advance" sequences. Treat `base+0x07ED0000…0x07F00000` and the `0x100`-page register file as non-cacheable device memory in the AMIX kernel mapping, and add explicit read/write ordering (the 030 has no `dsb`; rely on serialized/`cpushl`-style barriers or a non-cached mapping) around each register handshake.

---

## Protocol contract (concise, what the AMIX driver must implement)

**Init/probe:** find Zorro board mfg `0x144B` prod `0x1`; base = board addr. Read MAC from `base+0x198`/`base+0x19C`; (optionally write back). Map register file (`base+0x100…`) and frame windows (`base+0x07ED0000…0x07F00000`) **non-cacheable**.

**Bring-up:** install INT6 handler; write `base+0x104 = 1` to enable the ethernet interrupt.

**TX (per frame):** build/copy the ethernet frame to `base+0x07EE0000` (cooked: synthesize `dst|src|type` 14-byte header then payload; raw: verbatim); write byte-length to `base+0x190`; read `base+0x190` → nonzero = error. Synchronous/blocking.

**RX (per interrupt):** on INT6, if `(*(base+0x1A8) & 1)` then ack with `*(base+0x104)=8|16` and schedule drain. Drain loop: read `off=*(base+0x1A4)`; slot = `base+off`; `size=slot[0..1]`, `serial=slot[2..3]`, frame at `slot[4]`; if `serial != last_serial` → demux by ethertype `slot[16..17]`, copy `size` bytes (cooked: skip 14-byte header) into an mblk, deliver upstream, set `last_serial`, then strobe `*(base+0x194)=1` to advance; repeat while serial changes / backlog non-empty (the firmware re-nags INT6 while backlog>0, so drain fully).

**Constants:** slot stride 2048, ring 64 slots @ `0x07ED0000`; TX window `0x07EE0000`; MTU 1500; eth header 14; RX prefix 4; INT bits ETH=1/AUDIO=2/USB=4; MAC OUI default `00:80:10`.

**Gaps / open questions (state explicitly):**
- **No multicast-filter register** exists in this protocol — `DL_ENABMULTI_REQ` cannot be hardware-honored; either rely on firmware promiscuous/all-multi behavior or return not-supported.
- **No promiscuous-enable register** — RX filtering policy is whatever the firmware GEM does; not controllable from the guest.
- The `memorymap.h:32` comment says "32 × 2048" but `FRAME_MAX_BACKLOG = 64` (`ethernet.c:59`) — the code value (64 slots, 128 KB) is authoritative; the backlog window nominally runs `0x07ED0000`→`0x07EE0000` (64 KB) which only holds 32 slots, so the 64-slot ring may overrun into `TX_FRAME_ADDRESS` — a latent firmware sizing inconsistency worth noting but **outside the guest contract** (the guest never sizes the ring; it only follows `REG_ZZ_ETH_RX_ADDRESS`).
- TX is **fully synchronous** (busy-wait in firmware up to ~1 ms) — the AMIX driver's TX path will block the calling context for the round-trip; consider whether to serialize TX behind the STREAMS write-side queue to avoid holding locks across the ~ms stall.

## SURVEY B — Z3660 FIRMWARE eth path
The full contract is confirmed. The key insight: `ZZ3660_REGS = cd->cd_BoardAddr` (the Zorro III autoconfig base, which the firmware maps to `RTG_BASE`), and the guest accesses both registers (`ZZ3660_REGS + REG_ZZ_*` where REG offsets are 0x190-0x1A8) AND the frame buffers (`ZZ3660_REGS + TX_FRAME_ADDRESS` = board+0x07EE0000, and `ZZ3660_REGS + address_of_rx_buff` where `address_of_rx_buff` is the RTG-relative value returned by `REG_ZZ_ETH_RX_ADDRESS`). All consistent with the firmware's `RTG_BASE`-relative pointers. I have everything needed.

---

# SURVEY B — Z3660 FIRMWARE ethernet path (the ARM/Zynq end of the protocol)

The firmware-side contract an AMIX STREAMS/DLPI driver must speak. All paths under `~/Devel/Omat/Amiga/Z3660/z3660-firmware/Z-TURN/vitis_ide/`. The built project is **`Z3660_emu`**, but the eth sources I cite live in `Z3660/src/*` and `Z3660/src/rtg/*` (the project's `main.c` pulls `Z3660_emu/src/defines.h` via `#include "../../Z3660_emu/src/defines.h"` — `Z3660/src/main.c:42`).

## 0. The shared register/buffer window — one Zorro III BAR == `RTG_BASE`

The whole interface is a **single Zorro III memory window**. The guest's autoconfig base (`cd->cd_BoardAddr`, guest `device.c:166` → `ZZ3660_REGS`) is what the firmware calls `RTG_BASE = 0x18000000` (`memorymap.h:16`). Everything below is an **offset into that one window**, which is exactly why the same numeric offsets appear on both sides.

| Register | Offset | Defined | Firmware read | Firmware write |
|---|---|---|---|---|
| `REG_ZZ_ETH_TX` | `0x190` | `zzregs.h:73` | returns `ethernet_send_result` (`rtg.c:803-804`) | `ethernet_send_result = ethernet_send_frame(zdata)` (`rtg.c:1609-1610`) |
| `REG_ZZ_ETH_RX` | `0x194` | `zzregs.h:74` | — | `frfb = ethernet_receive_frame()` — advance/free a backlog slot (`rtg.c:1613-1615`) |
| `REG_ZZ_ETH_MAC_HI` | `0x198` | `zzregs.h:75` | bytes [0],[1] of MAC (`rtg.c:865-868`) | sets MAC[0],[1] (`rtg.c:1633-1637`) |
| `REG_ZZ_ETH_MAC_LO` | `0x19C` | `zzregs.h:76` | bytes [2..5] of MAC (`rtg.c:870-873`) | sets MAC[2..5] + `ethernet_update_mac_address()` (`rtg.c:1639-1646`) |
| `REG_ZZ_ETH_RX_ADDRESS` | `0x1A4` | `zzregs.h:79` | returns `ethernet_current_receive_ptr()` (RTG-relative) (`rtg.c:806-809`) | — |
| `REG_ZZ_INT_STATUS` | `0x1A8` | `zzregs.h:80` | live IRQ bitmask (`rtg.c:1172`, mirrored at `0x1A8` window addr) | — |
| `REG_ZZ_CONFIG` | (see below) | | | enable/ack INT6 for eth (`rtg.c:1150-1173`) |

Register writes are **emulated/trapped** by the firmware dispatcher (note `rtg_cache_policy_core0` comment "*write to RTG registers is emulated*", `main.c:644`) — the 030 store to `board+0x190` is decoded by the ARM, it is not a plain memory cell. Frame **buffers**, by contrast, are real DDR the 68k reads/writes directly through the window.

## 1. Register dispatch (`rtg.c`)

**`REG_ZZ_ETH_TX` write (`rtg.c:1609-1612`):** calls `ethernet_send_frame(zdata)`, where `zdata` = the frame length in bytes; stashes the return code in `ethernet_send_result`. **`REG_ZZ_ETH_TX` read (`rtg.c:803-805`):** returns that stored result. So TX is a **single 32-bit write of the length, then a read of the same register for completion status** — the guest confirms this exact handshake: write `sz` then `rc = *reg` (guest `device.c:736-739`).

**`REG_ZZ_ETH_RX` write (`rtg.c:1613-1632`):** calls `ethernet_receive_frame()` → advances the backlog read-cursor (frees the slot the guest just consumed). Value written is ignored (guest writes `1L`, `device.c:838-839`). **`REG_ZZ_ETH_RX_ADDRESS` read (`rtg.c:806-809`):** returns `ethernet_current_receive_ptr()` = the **RTG-relative** address of the current unread backlog slot. The guest adds its board base back: `frm = ZZ3660_REGS + address_of_rx_buff` (guest `device.c:797-802`). This relative/absolute split is a contract detail the AMIX driver must reproduce: **the value from `0x1A4` is an offset into the board window, not an absolute pointer.**

## 2. TX path — `ethernet_send_frame()` (`ethernet.c:1028-1095`)

- **Where the frame goes:** the guest writes the raw ethernet frame bytes directly into the window at `ZZ3660_REGS + TX_FRAME_ADDRESS` (guest `device.c:497-499`) **before** writing the length to `REG_ZZ_ETH_TX`. Firmware-side that same buffer is `TxFrame = RTG_BASE + TX_FRAME_ADDRESS` (`ethernet.c:99`), i.e. **`0x18000000 + 0x07EE0000` = absolute `0x1FEE0000`** (`memorymap.h:33`).
- **Length encoding:** `zdata` (the 32-bit value written to `0x190`) **is** the byte count, passed straight to `XEmacPs_BdSetLength(BdTxPtr, frame_size)` (`ethernet.c:1062`). `frame_size` is a `uint16_t` (`ethernet.c:1028`). There is **no length/header word inside `TxFrame`** — the buffer holds the bare frame starting at offset 0.
- **Max size:** the buffer is `EthernetFrame[XEMACPS_MAX_VLAN_FRAME_SIZE_JUMBO]`, 64-byte aligned (`ethernet.c:97`). The flush spans `sizeof(EthernetFrame)` (`ethernet.c:1039-1040`). The TX region (`0x07EE0000`→`0x07EF0000`) is **1 MB** so there is generous headroom; the practical cap is the jumbo-frame constant, not the window.
- **What it does with it:** flushes L1+L2 D-cache over `TxFrame` (`ethernet.c:1039-1040`), allocs a TX BD, sets address+length, clears the USED bit, marks LAST, hands to HW (`XEmacPs_BdRingToHw`), and kicks `XEmacPs_Transmit` (`ethernet.c:1051-1076`).
- **Completion/status (returned via the `0x190` read):**
  - `0` = transmitted OK (it busy-waits up to ~1 ms for `FramesTx` to increment, `ethernet.c:1080-1094`)
  - `1` = `ethernet_task_state != ETH_TASK_READY` (link/init not up) (`ethernet.c:1032-1034`)
  - `2` = `XEmacPs_BdRingAlloc` failed (`ethernet.c:1053-1058`)
  - `3` = `XEmacPs_BdRingToHw` failed (`ethernet.c:1068-1070`)
  - `4` = TX-complete timeout (`ethernet.c:1085-1088`)

  Note: TX is **synchronous and blocking** inside the register write — the ARM spins waiting for the GEM TX-done ISR before returning. The AMIX driver's TX is therefore a single mutually-exclusive register transaction; it cannot pipeline multiple frames into `TxFrame` (one shared TX buffer, no TX ring on the guest side).

## 3. RX path — the backlog ring

**Producer (GEM RX ISR), `xEmacPsRecvHandler()` (`ethernet.c:510-594`):** on each received frame the firmware copies it from the GEM BD buffer into the **backlog ring** and writes a 4-byte per-slot header.

- **Ring base:** `RX_BACKLOG_ADDRESS = 0x07ED0000` (window-relative) → absolute `RTG_BASE + 0x07ED0000 = 0x1FED0000` (`memorymap.h:32`; used at `ethernet.c:552`).
- **Slot size:** `FRAME_SIZE = 2048` bytes (`memorymap.h:40`). Slot N is at `RX_BACKLOG_ADDRESS + N*FRAME_SIZE` (`ethernet.c:552`, `ethernet_current_receive_ptr()` `ethernet.c:597`).
- **Slot count — a discrepancy to flag:** the producer guards with `frames_backlog < FRAME_MAX_BACKLOG` where **`FRAME_MAX_BACKLOG = 64`** (`ethernet.c:59`, `:550`). 64 × 2048 = 128 KB. But the memory map comment says the backlog is only **"32 * 2048 space (64 kB)"** (`memorymap.h:32`), and the next region `TX_FRAME_ADDRESS` starts at `0x07EE0000` — exactly **64 KB** above `0x07ED0000`. **So a full 64-deep backlog (128 KB) would overrun into `TX_FRAME_ADDRESS`.** This is a latent firmware bug, not something the AMIX driver triggers (it would need 64 un-drained frames), but the driver should drain RX promptly. (Documenting per the "say so explicitly" instruction — the code constant and the map comment disagree.)
- **Per-slot 4-byte header** (`ethernet.c:558-561`), big-endian:
  - byte 0..1 = `rx_bytes` (frame length, hi/lo) — `XEmacPs_BdGetLength` (`ethernet.c:540`)
  - byte 2..3 = `frame_serial` (16-bit monotonically incremented counter, hi/lo) (`ethernet.c:534, 560-561`)
  - frame payload copied at **offset `RX_FRAME_PAD = 4`** (`memorymap.h:39`; `memcpy(frame_bl_ptr+RX_FRAME_PAD, frame_ptr, rx_bytes)`, `ethernet.c:553`). So **payload starts 4 bytes into the slot**, header occupies bytes 0-3.
- **Guest cross-check (matches exactly):** `read_frame` reads `sz = frm[0]<<8 | frm[1]`, `ser = frm[2]<<8 | frm[3]`, and takes the payload from `frm + 4` (RAW) or `frm + 4 + HW_ETH_HDR_SIZE` (`device.c:630-643`); `get_frame_serial` reads `frm[2..3]` (`device.c:615-620`). Header layout and pad are mirrored byte-for-byte.

**Consumer cursor & freeing a slot:**
- The guest reads `REG_ZZ_ETH_RX_ADDRESS` to get the current slot's window offset (`ethernet_current_receive_ptr()` = `RX_BACKLOG_ADDRESS + frames_received_from_backlog*FRAME_SIZE`, `ethernet.c:596-598`), copies the frame out, then **writes `REG_ZZ_ETH_RX`** which calls `ethernet_receive_frame()` (`ethernet.c:604-639`): increments `frames_received_from_backlog`; when it reaches `frames_backlog` it **resets both counters to 0** (ring drained, `ethernet.c:619-622`). There is a serial-fix-up so a drained-then-reset slot keeps its old serial, preventing the guest from re-processing it (`ethernet.c:624-629`).
- **Edge detection is by serial, not by count:** the guest keeps `old_serial` and only processes when `serial != old_serial` (`device.c:806-810`). The AMIX driver MUST track the previous serial the same way — the firmware reuses slot 0's address after a drain, so address alone is not a freshness signal.

**Signalling the 68k (the interrupt):**
- `interrupt_thread`/`ethernet_thread` (`rtg.c:309-348`): when `interrupt_enabled_ethernet && ethernet_get_backlog() > 0`, a nag-counter trips and the firmware calls `amiga_interrupt_set(AMIGA_INTERRUPT_ETH)` (`rtg.c:327-336`). `AMIGA_INTERRUPT_ETH = 1` (`interrupt.h:19`).
- `amiga_interrupt_set` (`interrupt.c:18-37`) ORs the bit into `amiga_interrupts`; on the **0→nonzero edge** it raises the physical line via `DiscreteSet(REG0, FPGA_INT6)` — i.e. **Amiga INT6 (IRQ level 6 / autovector, the same level as CIA/serial; the highest maskable)** (`interrupt.c:27-29`, comment "*Only activate INT6*"). RX, AUDIO and USB **share INT6**; `REG_ZZ_INT_STATUS` (`0x1A8`) is the demux register the ISR reads to learn which source fired (mirrored to the window at `rtg.c:316, 1172`).
- **Enable/ack via `REG_ZZ_CONFIG`** (`rtg.c:1150-1173`): write `zdata & 1` to enable eth INT (guest sends `1`, `device.c:319`); write `8|16` to **clear/ack** the eth interrupt (`amiga_interrupt_clear(AMIGA_INTERRUPT_ETH)`, `rtg.c:1153-1157`; guest acks with `8|16` at `device.c:85`). Disable = write `0` (guest `device.c:386`). INT6 physically de-asserts only when **all** shared sources are clear (`interrupt.c:43-51`).

So the AMIX RX flow is: install an INT6 (level-6 autovector) handler → on IRQ read `REG_ZZ_INT_STATUS`, check bit `AMIGA_INTERRUPT_ETH(=1)` → loop: read `0x1A4` for slot offset, check serial vs last, copy frame out (header at +0, payload at +4), `streamtab`/DLPI-push it, write `0x194` to advance → ack via `REG_ZZ_CONFIG = 8|16`.

## 4. Cache policy — eth buffers are non-cacheable DDR (confirmed)

- The eth region is mapped **non-cacheable** at MMU-setup time. `main.c:687-692` maps the region `j = 0x07E..0x080` (i.e. `RTG_BASE + 0x07E00000` … `RTG_BASE + 0x08000000`, **2 MB**) with `ETHERNET_CACHE_POLICY`. The MPEG slot just below (`0x07A`) also uses it (`main.c:680-685`).
- `ETHERNET_CACHE_POLICY = (0x14DE2 | (NC<<OUTER) | (NC<<INNER))` with `NC = 0b00` (non-cacheable both inner & outer), `OUTER=12`, `INNER=2` (`Z3660_emu/src/defines.h:2-6, 15`). For reference `NORM_NONCACHE = 0x11DE2` (`xil_mmu.h:54`); the eth policy is the strongly-non-cacheable variant of the same TEX/C/B encoding.
- **Coverage check:** the 2 MB eth window (`0x07E00000`–`0x08000000`) contains **all** eth buffers: TX/RX BD lists `0x07E00000`/`0x07E60000`, RX backlog `0x07ED0000`, `TxFrame` `0x07EE0000`, `RxFrame` `0x07EF0000` (`memorymap.h:30-34`). All non-cached on the ARM side.
- **Cache maintenance the firmware DOES do (asymmetric — important):**
  - **TX:** *despite* the buffer being non-cacheable, `ethernet_send_frame` still issues `Xil_L1DCacheFlushRange((UINTPTR)TxFrame,…)` + `Xil_L2CacheFlushRange(...)` before handing to the GEM (`ethernet.c:1039-1040`). Belt-and-suspenders.
  - **RX:** the corresponding invalidate calls are **commented out** (`ethernet.c:546-548`) — the firmware relies on the non-cached mapping so the `memcpy` into the backlog and the header stores are immediately visible. The header-write order (payload memcpy first, then the 4-byte header incl. serial last, `ethernet.c:553-561`) is the publish barrier the guest's serial check depends on.
- **Implication for AMIX coherency (this is the first-class concern flagged in the task):** the **ARM** side is non-cached, but the **68030** side is a separate cache domain. The firmware does **nothing** to manage the 030's caches. The 030 reaches these buffers through the Zorro III window; whether that window is cache-inhibited on the 030 side is the AMIX driver's responsibility (the AmigaOS guest works because Zorro III space is CI in its MMU map, and it has a stray `CacheClearE` at `device.c:817`, currently commented). The AMIX STREAMS driver MUST ensure the eth buffer window is mapped **cache-inhibited** in the 030 MMU (or do explicit `CACHE`/`cpush`/`cinv` around every TX fill and RX drain). This is the direct analogue of the SCSI bounce-coherency issues, and is the most likely place a "works-then-corrupts" bug appears — treat byte 2-3 serial reads and the payload copy as ordered, uncached accesses.

## 5. Does the firmware need changes for AMIX? — No. The interface is guest-agnostic.

**Evidence:** every firmware code path keyed on the ethernet registers (`rtg.c:803-809, 1609-1646`; `ethernet.c` TX/RX) operates purely on the **register write/read values and the shared DDR buffer contents**. There is no branch on guest OS, no `a3000_amix_mode`-style flag, no AmigaOS-specific assumption anywhere in `ethernet.c` or the eth dispatch in `rtg.c`. The firmware:
- doesn't parse the ethernet payload (it copies opaque bytes; only `XEmacPs_BdGetLength` matters);
- the MAC is set by the guest via `0x198/0x19C` and is OS-neutral;
- the only "state" shared with the guest is the serial counter + backlog cursor, both OS-neutral.

The existing SANA-II guest driver (`z3660-drivers/eth/device.c`) is just one consumer of this contract; an AMIX STREAMS/DLPI driver that reproduces the **same register handshake and the same buffer/header layout** will be served identically. This mirrors the SCSI story (`z3660.c` reusing the piscsi mailbox unchanged).

**One caveat, not a required change:** the TX register write **blocks the ARM** for up to ~1 ms (`ethernet.c:1080-1090`). Under AMIX's STREAMS context (TX may be driven from interrupt/`srv` context), a multi-millisecond stall per frame inside a register store is a performance/latency concern to design around (e.g. don't hold spl high across it), but it does not require firmware modification.

---

## Contract summary the AMIX driver must implement (cross-checked vs guest)

| Operation | AMIX driver action | Firmware/guest cite |
|---|---|---|
| **Set MAC** | write hi to `board+0x198`, lo to `board+0x19C` | `rtg.c:1633-1646` / `device.c:197-198` |
| **Enable RX IRQ** | write `1` to `REG_ZZ_CONFIG` | `rtg.c:1166` / `device.c:319` |
| **TX** | fill `board+0x07EE0000` with bare frame (no header), write byte-count to `board+0x190`, read `board+0x190` for rc (0=ok) | `ethernet.c:1028-1094`, `:99` / `device.c:497-739` |
| **RX poll/ISR** | on INT6, read `REG_ZZ_INT_STATUS(0x1A8)` & bit 1; read slot offset from `board+0x1A4`; `frm = board + offset`; `len = frm[0..1]`, `serial = frm[2..3]`, payload at `frm+4`; process only if `serial != last_serial`; write `board+0x194` to advance | `rtg.c:806-809,1613`; `ethernet.c:534,552-561,596-639` / `device.c:797-839` |
| **Ack RX IRQ** | write `8\|16` to `REG_ZZ_CONFIG` | `rtg.c:1153-1157` / `device.c:85` |
| **Disable** | write `0` to `REG_ZZ_CONFIG` | `rtg.c:1164-1170` / `device.c:386` |

Constants the driver must hard-code (all window-relative unless noted): `RTG_BASE/window = board base`; regs `ETH_TX=0x190 ETH_RX=0x194 MAC_HI=0x198 MAC_LO=0x19C RX_ADDRESS=0x1A4 INT_STATUS=0x1A8`; `TX_FRAME=0x07EE0000`; `RX_BACKLOG=0x07ED0000`; `FRAME_SIZE=2048`; `RX_FRAME_PAD=4`; header = BE [len16][serial16]; IRQ = **Amiga INT6 / level 6 autovector**, shared with audio+USB, demuxed via `0x1A8`.

**Open risks for the coherency analysis:** (1) 030-side cache-inhibit of the board window is the driver's job — firmware does no 030 cache maintenance (§4). (2) `FRAME_MAX_BACKLOG=64` vs the 64 KB map comment = potential 128 KB overrun into `TX_FRAME_ADDRESS` if RX is not drained — driver should drain eagerly (§3). (3) TX register write blocks the ARM ~1 ms — design AMIX TX to tolerate the stall (§5).

## SURVEY C — AEN LANCE STREAMS/DLPI skeleton
I have read every relevant file in full. Below is the complete skeleton extraction.

---

# SURVEY C — The stock AMIX `aen` LANCE/a2065 STREAMS/DLPI driver (the skeleton to mirror)

All citations are to the `hydra-amix` tree (`~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys`). Driver source: `amiga/driver/aen/{aen.c, aen.h, aenuser.h, lance.h, Makefile}`.

## 0. One-paragraph orientation

`aen` is a **DLPI Style-1, connectionless (DL_CLDLS)** STREAMS provider for the Commodore A2065 (and AmerStar) LANCE Ethernet board. It is opened directly by the IP/ARP layer as cdevsw major **18**, demuxes inbound frames to multiple bound streams by **Ethernet type == SAP**, and is wired into three kernel tables: `cdevsw[18].d_str = &aeninfo`, `int2_tbl[4] = aenintr`, and (notably) it has **NO `io_init[]`/`init_tbl[]` entry** — it autoconfigures lazily on first `open()`. That last point is the single biggest structural divergence from the SCSI driver model and is called out in §6. The LANCE talks to the host through a **32 KB on-board shared RAM window** (init block + descriptor rings + packet buffers all live in board RAM, accessed via `board_base + offset`); this is the direct analogue of the Z3660 shared eth-frame DDR window and is where the cache-coherency concern lands (§7).

---

## 1. Data structures (the template to replicate)

### 1.1 STREAMS plumbing (`aen.c:60-78`)
```
module_info aen_minfo = { 0x656E /*'en'*/, "aen", AEN_MIN, AEN_MAX, AEN_HIWAT, AEN_LOWAT }   // aen.c:60
qinit aenrinit   = { NULL, NULL, aenopen, aenclose, NULL, &aen_minfo, NULL }                 // aen.c:65 (READ side: open/close live here; no rput/rsrv)
qinit aenwinit   = { aenwput, aenwsrv, NULL, NULL, NULL, &aen_minfo, NULL }                  // aen.c:70 (WRITE side: put + srv)
streamtab aeninfo = { &aenrinit, &aenwinit, NULL, NULL }                                     // aen.c:75 (the symbol kernel.c references)
```
Note: the **read qinit has NO put/srv** — RX is pushed upward via `putnext()` straight from the interrupt handler, not by a STREAMS read-side procedure. `open`/`close` are registered on the **read** qinit (standard STREAMS placement). Water marks/packet bounds from `aen.h:39-44`: `AEN_HIWAT=5120`, `AEN_LOWAT=2*HIWAT/3`, `AEN_MIN=64-CRC`, `AEN_MAX=MTU`.

### 1.2 Per-stream softc — `aen_t` (`aen.h:72-79`)
```
typedef struct {
    queue_t        *q;            // the read queue; also the "is this slot in use" flag
    int             state;        // DLPI state: DL_UNBOUND / DL_IDLE
    int             board_index;  // which board this stream rode in on
    int             flags;        // AEN_RAW_DATA etc.
    unsigned short  sap;          // bound SAP == Ethernet type; the RX demux key
} aen_t;
```

### 1.3 Per-board hardware softc — `aen_info_t` (`aen.h:81-95`)
```
long           board_base;       // autoconfig base addr of the 32K shared window
volatile u_short *lance_base;    // board_base + 0x4000  (lance.c:1561)
volatile u_short *lance_data;    // lance_base + 0       (RDP - register data port)
volatile u_short *lance_addr;    // lance_base + 1       (RAP - register address port)
u_char         paddress[6];      // physical (MAC) address
u_char         laddress[8];      // logical address filter
int            tx_buffer_index, rx_buffer_index;   // ring cursors
volatile struct initblk *initblk;// LANCE init block (in board RAM, board_base+0x8000)
volatile struct ringptr *rxptr;  // RX descriptor ring (in board RAM)
volatile struct ringptr *txptr;  // TX descriptor ring (in board RAM)
void          *lance_buff;       // board_base + 0x8000
```
**Every `volatile` here is load-bearing** — these are pointers INTO the board's shared RAM, read/written by both the 68k and the LANCE DMA engine. This is the exact structure the Z3660 driver must re-cast onto the GEM's NORM_NONCACHE DDR window.

### 1.4 Aggregate per-board container — `aen_board_t` (`aen.h:97-103`)
```
typedef struct {
    aen_t          aen[AEN_MAXDEV];   // AEN_MAXDEV=5 streams per board (aen.h:4)
    aen_status_t   aen_status;        // stats + board_state (BOARD_RESET/IN_INIT/RUNNING)
    aen_info_t     aen_info;          // the hardware softc above
    struct ifstats ifstats;           // inet per-interface stats node (linked into global list)
} aen_board_t;
aen_board_t aen_board[AEN_MAXBOARDS];           // AEN_MAXBOARDS=4 (aen.c:83, aen.h:5)
aen_autoconfig_t aen_autoconfig[AEN_MAXBOARDS]; // {long address; int type;} (aen.h:105)
int aen_number_of_boards;
```

### 1.5 The "ifnet equivalent" — `struct ifstats` (the IP linkage)
AMIX SVR4 has **no BSD `ifnet`**; the IP-visible interface node is `struct ifstats` (defined in `net/if.h`, not in this tree slice — externed at `aen.c:43` as `extern struct ifstats *ifstats;`). On first open, the driver self-attaches by **prepending its `ifstats` node to the global `ifstats` list** (`aen.c:174-181`):
```
board->ifstats.ifs_name = "aen";  ifs_mtu = AEN_MTU;  ifs_unit = board_index;
board->ifstats.ifs_next = ifstats;  ifstats = &board->ifstats;  ifs_active = 1;
```
Counters touched elsewhere: `ifs_ipackets, ifs_ierrors, ifs_opackets, ifs_oerrors, ifs_collisions` (e.g. `aen.c:388, 1002-1004`). **This list, plus the cdevsw major, is how `aen0` becomes visible to `ifconfig`/IP** — there is no separate `if_attach()`.

### 1.6 DLPI state
Only two states are ever used: `DL_UNBOUND` (after open / after unbind; set `aen.c:1535, 913`) and `DL_IDLE` (after a successful `DL_BIND_REQ`; set `aen.c:802`). Style-1, CLDLS, `DL_ETHER` (advertised in INFO_ACK, §3).

### 1.7 LANCE-specific structs (`lance.h`) — the part that does NOT carry over
`struct initblk` (`lance.h:132`: mode, padr[6], ladrf[8], rxring[2], txring[2]) and `struct ringptr` (`lance.h:145`: loaddr, mode, hiaddr, length, count) are **AM7990 LANCE descriptor formats**. The Z3660 driver replaces these wholesale with whatever the firmware mailbox/ring format is — but the *role* (a ring of descriptors, an OWN bit handing buffers between CPU and DMA/firmware) maps directly.

---

## 2. Entry points and their exact roles → which kernel table

| Function | Role | Where registered |
|---|---|---|
| `aenopen(q,devp,flag,sflag,credp)` `aen.c:93` | Open. Rejects MODOPEN/CLONEOPEN; calls `aenautoconfig()`; decodes minor→`board_index`(bits3-0)+`sap_index`(bits7-4); lazily `aen_initialize()`s the board; allocates an `aen_t` slot (minor 0 = pseudo-clone "next free sap"); wires `q->q_ptr`; self-attaches `ifstats`. | `aenrinit` (read qinit) → `aeninfo` → `cdevsw[18]` |
| `aenclose(q)` `aen.c:190` | Close. Clears slot; if it was the **last** open stream on the board, **halts the LANCE** (CSR0_STOP) and zeroes stats. | `aenrinit` |
| `aenwput(q,mp)` `aen.c:242` | Write put — the demux for everything coming DOWN from IP. `splaen()`-guarded `switch(db_type)`: M_FLUSH, M_PROTO/M_PCPROTO→`aenproto()`, M_IOCTL/M_IOCDATA→`aenioctl()`. | `aenwinit` (write qinit) |
| `aenwsrv()` `aen.c:418` | Write service — **stub `return 0`**; exists only so STREAMS flow control (HIWAT/LOWAT back-pressure + `qenable`) is available. Real deferred-TX restart happens in `transmit_interrupt()` via `getq`/`putbq`, NOT here. | `aenwinit` |
| `aenintr()` `aen.c:934` | Interrupt handler. Polls every RUNNING board's CSR0; on CSR0_RINT→`receive_interrupt()`, CSR0_TINT→`transmit_interrupt()`, CSR0_ERR→bump counters; on a board still `BOARD_IN_INIT_CODE`→`initialize_and_go()`. | `int2_tbl[4]` (`kernel.c:193`) |
| `aenautoconfig()` `aen.c:1421` | Probe. `autocon()`-scans for CBM (`AEN_BOARD`) then AmerStar (`AMBOARD`) boards, fills+sorts `aen_autoconfig[]`. **Idempotent (static guard).** Called from `aenopen`, NOT from boot. | (none — lazy, no init_tbl entry) |
| `aen_initialize(board_index)` `aen.c:1489` | Per-board bring-up: derive MAC from board serial (`get_auto_serial_number`), init `aen_t[]` slots, call `setup_lance()`. Called from `aenopen` when board not RUNNING. | (called from open) |
| `setup_lance(...)` `aen.c:1542` | Program LANCE: map register/buffer pointers, STOP, set CSR3 byte-swap, point CSR1/2 at init block, build init block + RX/TX rings in board RAM, kick CSR0_INIT, **`sleep()` until interrupt flips state to RUNNING**, then CSR0_STRT. | (called from initialize) |
| `aenioctl(q,mp)` `aen.c:423` | M_IOCTL/M_IOCDATA handler: AEN_GET/SET_CONFIG, AEN_GET/CLEAR_STATUS, AEN_NUMBER_OF_BOARDS, SIOCSIFFLAGS/SIOCGIFFLAGS (ACK no-op). Uses M_COPYIN/M_COPYOUT/TRANSPARENT ioctl protocol. | (called from wput) |
| `aenproto(q,mp)` `aen.c:777` | **The DLPI engine** (see §3). | (called from wput) |
| `receive_interrupt` `aen.c:1015`, `toss_packet_up_stream` `aen.c:1168`, `transmit_interrupt` `aen.c:1357` | RX/TX bottom halves (see §4–5). | (called from aenintr) |

**Mapping to Z3660 wiring** (from PROJECT CONTEXT facts): the Z3660 net driver needs the same three table edits in `master.d/kernel.c` — a new `cdevsw[]` row `… notty,&z3660netinfo,nullflag` (mirror `kernel.c:357`), an `int2_tbl[]` entry for its ISR (mirror `kernel.c:193`) **OR** firmware-interrupt poll hook, and the `extern struct streamtab z3660netinfo;` declaration before the array (mirror `kernel.c:265`). Whether it needs an `io_init[]` entry depends on whether it copies aen's **lazy-open autoconfig** (aen has none) or wants boot-time probe.

---

## 3. DLPI message handling in `aenproto()` (`aen.c:777-929`)

Dispatched on `((union DL_primitives *)mp->b_rptr)->dl_primitive`. Four primitives implemented; everything else → log + `freemsg`:

| Primitive | Handling | Reply |
|---|---|---|
| **DL_UNITDATA_REQ** `aen.c:791` | If write-queue already has a backlog (`q->q_first`) OR `aenxmit()` says the ring is full → `putq(q,mp)` (defer, flow-controlled). Otherwise the frame was transmitted by `aenxmit`. | (none on success; deferred on busy) |
| **DL_BIND_REQ** `aen.c:796` | `aen->sap = reqp->dl_sap; aen->state = DL_IDLE`. Frees req. | allocates M_PCPROTO **DL_BIND_ACK** carrying `dl_sap` + the 6-byte MAC (`paddress`) at `dl_addr_offset`; `qreply(q,mp)` (`aen.c:818-833`) |
| **DL_INFO_REQ** `aen.c:838` | builds **DL_INFO_ACK**: `dl_max_sdu=ETH_MAXDATA(1500)`, `dl_min_sdu=1`, `dl_addr_length=6`, `dl_mac_type=DL_ETHER`, `dl_current_state=aen->state`, `dl_service_mode=DL_CLDLS`, `dl_provider_style=DL_STYLE1`; appends MAC only if state==DL_IDLE. `qreply`. (`aen.c:854-886`) | DL_INFO_ACK |
| **DL_UNBIND_REQ** `aen.c:890` | `aen->state=DL_UNBOUND; aen->sap=0`. | **DL_OK_ACK** with `dl_correct_primitive=DL_UNBIND_REQ`; `qreply` (`aen.c:907-916`) |

**Not implemented** (gaps to be aware of when mirroring): `DL_PHYS_ADDR_REQ`, `DL_ENABMULTI_REQ`/`DL_SUBS_BIND_REQ` (multicast), `DL_DETACH_REQ`, `DL_PROMISCON_REQ`. Multicast/promisc are instead poked via the **private `AEN_SET_CONFIG` ioctl** (`aen.c:568`), which rewrites the LANCE mode/`ladrf` filter and re-runs `setup_lance`, then re-emits BIND_ACK+INFO_ACK to every still-bound stream (`aen.c:627-704`). A modern mirror would likely add real DLPI multicast primitives.

The `DL_INFO_ACK`/`DL_BIND_ACK` builder block at `aen.c:627-704` is duplicated logic — worth factoring into a helper in the Z3660 port.

---

## 4. TX flow — `DL_UNITDATA_REQ` → wire (`aenxmit`, `aen.c:292-413`)

1. `aenproto` (DL_UNITDATA_REQ) calls **`aenxmit(q,mp)`** (or defers via `putq` if busy).
2. `aenxmit` reads `i = tx_buffer_index`; checks `txptr[i].mode & TMD1_OWN` — **if the LANCE still owns this descriptor, return FALSE (ring full)** (`aen.c:311`). That FALSE is what makes `aenproto` `putq` the mblk for later.
3. Frame assembly directly into the **board-RAM TX buffer** `bufaddr = board_base + txptr[i].loaddr` (`aen.c:320`):
   - dest MAC ← copied from `mp->b_rptr + dp->dl_dest_addr_offset` (the DL_UNITDATA_REQ header) (`aen.c:325-327`)
   - src MAC ← `aen_info->paddress` (`aen.c:332`)
   - Ethernet type ← `aen->sap` (the bound SAP) (`aen.c:341`)
   - payload ← walk `mp->b_cont` chain, `bcopy` each mblk into `bufaddr`, capped at `ETH_MAXDATA` (`aen.c:352-359`)
4. `freemsg(oldmp)` the whole message (`aen.c:366`).
5. Compute length, write **`txptr[i].length = -n`** (LANCE wants two's-complement length) (`aen.c:371`), then **hand the descriptor to the LANCE: `txptr[i].mode = TMD1_ENP|TMD1_STP|TMD1_OWN`** (`aen.c:386`) — the OWN-bit store is the actual "go".
6. Bump `opackets`/`ifs_opackets`, advance `tx_buffer_index` (mod `TXCOUNT`), return TRUE.

**Deferred-TX restart**: when the LANCE later raises TINT, `transmit_interrupt()` (`aen.c:1357`) walks all streams, `getq(wrq)` the backlog, retries `aenxmit`; on still-full it `putbq` and stops. So flow-control resume is **interrupt-driven**, not `qenable`/`aenwsrv`-driven.

**Z3660 mapping**: the `bcopy`-into-shared-RAM + set-OWN-bit + bump-index pattern maps 1:1 onto poking the Z3660 firmware's TX mailbox/ring (whatever `REG_ZZ_ETH_TX` expects). The "is the slot still owned by the far side" gate is the universal ring-full check.

---

## 5. RX flow — LANCE RINT → `DL_UNITDATA_IND` up the stream

`aenintr` (RINT) → **`receive_interrupt(board_index)`** (`aen.c:1015-1166`):
1. Loop while the host owns the current RX descriptor (`!(rxptr[i].mode & RMD1_OWN)`) (`aen.c:1032`).
2. Resync to a START descriptor (`RMD1_STP`), scan forward to the END (`RMD1_ENP`) descriptor — a frame may span multiple ring buffers (RX buffers are only 150 bytes each, `aen.h:36`) (`aen.c:1038-1073`).
3. **Reassemble** the multi-descriptor frame by `bcopy`-ing each `board_base + rxptr[i].loaddr` chunk into a single static `rxbuff[MAX_BUFFER_LENGTH]` (`aen.c:1084-1145`); per-descriptor error bits (RMD1_BUFF/CRC/OFLO/FRAM) bump stats and drop the frame. As each descriptor is consumed it is **handed back to the LANCE: `rxptr[i].mode = RMD1_OWN`**.
4. Optional `aen_packet_hook()` tap (`aen.c:1154`) — a global function-pointer hook (`aen.c:46`) for packet sniffing/bridging; if it returns nonzero the frame is swallowed.
5. **`toss_packet_up_stream(rxbuff, board_index, count)`** (`aen.c:1168`).

`toss_packet_up_stream` — the **SAP demux + DLPI_IND builder** (`aen.c:1168-1355`):
- Reads `sap = packet_header->ether_type` (`aen.c:1184`).
- **Trailer-encapsulation** decode (BSD trailers, types `ETHERTYPE_TRAIL..+NTRAILER`): recomputes real `sap`, `length`, `trailn` (`aen.c:1187-1209`). Normal path: `length = count - ETH_CRC_LEN` (`aen.c:1212`).
- Iterates **all `AEN_MAXDEV` streams on the board** (`aen.c:1236`); for each open `aen->q`:
  - If `aen->flags & AEN_RAW_DATA`: deliver the **whole raw frame** as M_PROTO (status block) + M_DATA (`aen.c:1243-1269`).
  - Else if **`aen->sap == sap`** (the demux match):
    - In promiscuous mode, extra-filter by dest MAC == ours or broadcast (`aen.c:1272-1287`).
    - `canput(aen->q->q_next)` flow-control check (`aen.c:1289`); on full → `couldnt_put++`.
    - **allocb** two mblks: an M_PROTO header block sized `sizeof(union DL_primitives)+2*ETH_ADDR` and an M_DATA payload block (`aen.c:1291-1294`); allocb failure → `allocbs_failed++`.
    - Build **`dl_unitdata_ind_t`**: `dl_primitive=DL_UNITDATA_IND`, `dl_dest_addr_length/offset`, `dl_src_addr_length/offset` (`aen.c:1296-1320`); copy dest MAC (`ether_dhost`) and src MAC (`ether_shost`) into the header block.
    - Copy trailer (if any) then the payload into the M_DATA block (`aen.c:1324-1340`).
    - **`putnext(aen->q, mp)`** — pushes the DL_UNITDATA_IND up to IP/ARP (`aen.c:1342`).

Broadcast/multicast is handled implicitly: a broadcast frame's `ether_type` still matches the SAP, so it is delivered to every stream bound to that SAP (promisc mode adds the explicit dest-MAC filter above).

**Z3660 mapping**: replace steps 1-3 (LANCE ring drain + multi-buffer reassembly) with reading a complete frame from the firmware RX mailbox/ring (`REG_ZZ_ETH_RX`, `AMIGA_INTERRUPT_ETH`); steps 4-5 (`toss_packet_up_stream`) are **almost verbatim reusable** — the SAP demux, DLPI_IND construction, `canput`/`allocb`/`putnext`, and trailer handling are hardware-independent.

---

## 6. Init / probe / name registration / IP linkage

- **Probe**: `aenautoconfig()` (`aen.c:1421`) calls **`autocon(productcode, index, &base, &size)`** (`amiga/kernel/support.c:297`) — Amiga Zorro autoconfig lookup by manufacturer/product (`AEN_BOARD=0x02020070` CBM, `AMBOARD=0x041d0001`, `aen.h:66-67`). It does **not** run at boot — there is **no `io_init[]`/`init_tbl[]` entry for aen** (confirmed: `kernel.c:198-200` `io_init[]` contains only `parinit`). Probe is triggered lazily from the **first `open()`**. This is the key architectural choice for the Z3660 port to decide: lazy-open vs. boot-init.
- **MAC**: derived from the board's serial EEPROM (`get_auto_serial_number`, `aen.c:1398`, offset `0x18`) → `00:80:10:xx:xx:xx` (CBM) or `00:00:9F:...` (AmerStar). The Z3660 will instead read its MAC from the GEM/firmware.
- **LANCE bring-up**: `setup_lance()` (`aen.c:1542`) builds the init block + rings in board RAM and uses a **sleep/wakeup handshake** — it `sleep()`s on `board_state` (`aen.c:1666`) until `aenintr`→`initialize_and_go()` sees CSR0_IDON and `wakeup()`s + sets `BOARD_RUNNING` (`aen.c:1707-1708`). A firmware-handshake equivalent will be needed for Z3660 init.
- **Name + IP linkage**: registered as `ifs_name="aen"`, unit = board_index, by **splicing `board->ifstats` into the global `ifstats` list on first open** (`aen.c:174-181`). Combined with **`cdevsw[18].d_str = &aeninfo`** (`kernel.c:357`) and **`int2_tbl[4]=aenintr`** (`kernel.c:193`), this is the complete "how `aen0` exists and reaches IP." `slink`/`ifconfig aen0 <ip> up -trailers` then opens major 18 / pushes IP atop the DLPI stream.

---

## 7. Cache-coherency notes for the Z3660 port (first-class concern per PROJECT CONTEXT)

- aen's correctness rests on the LANCE shared RAM being **`volatile`-qualified and on a non-cached/IO-coherent bus** — every descriptor field (`aen.h:84-93`) and the register ports are `volatile`. There are **no explicit cache flush/invalidate calls** in aen.c; coherency is assumed from the bus (Zorro A2065 window is non-cacheable on the 030).
- The CPU↔DMA handoff is a **single ownership-bit store** (`txptr[i].mode = …|TMD1_OWN` at `aen.c:386`; `rxptr[i].mode = RMD1_OWN` at `aen.c:1044/1086`). On the Z3660, with frame buffers in the `NORM_NONCACHE` DDR window, the ordering of "fill buffer → set OWN bit" relative to the firmware reading it is exactly the lpsched-class concern: a `bcopy` into the bounce buffer followed by an OWN-bit write needs a write-barrier/`dsb` if the buffer is in cached space, or strict non-cached mapping (as aen relies on) if not. **The aen template gives you the *structure* but NOT a coherency solution** — it leans entirely on the bus being non-cacheable.
- The static `rxbuff[]` reassembly buffer (`aen.c:1023`) is plain cached kernel memory (it's the *destination* of the copy out of board RAM, then copied again into allocb'd mblks) — fine for the LANCE, but the Z3660 port should decide whether to reassemble through a cached staging buffer or read frames straight from the non-cached firmware ring.

---

## 8. Kernel-table wiring summary (exact lines to mirror for Z3660)

`~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys/master.d/kernel.c`:
- `kernel.c:187` — `extern void … aenintr() …;`
- `kernel.c:193` — `aenintr,  /* A2065 Ethernet ISR */` inside `int2_tbl[]` (index 4)
- `kernel.c:265` — `extern struct streamtab aeninfo;`
- `kernel.c:357` — `ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&aeninfo,nullflag,  /*18=aen*/` (cdevsw major 18; note `nullflag`, not `oldflag`)
- **No** `io_init[]`/`init_tbl[]` entry (`kernel.c:198-200`).

`Makefile` (`amiga/driver/aen/Makefile`): `CFLAGS=-O -D_KERNEL -DSVR40 -DSVR4 -I../../.. -I../../inc`; builds `aen.o` → `ld -r -o exp aen.o`. The Z3660 driver needs its own subdir + this same `ld -r → exp` pattern so the kernel link picks it up.

---

## 9. Concrete map: aen function → Z3660 driver function

| aen (LANCE) | Z3660 (firmware mailbox) — reuse vs. rewrite |
|---|---|
| `aeninfo`/`aenrinit`/`aenwinit`/`aen_minfo` | **Reuse verbatim** (rename) — pure STREAMS plumbing |
| `aenopen`/`aenclose` | **Reuse structure**; swap LANCE-halt for firmware-stop |
| `aenwput` | **Reuse verbatim** (rename) |
| `aenproto` (DLPI) | **Reuse almost verbatim** — DLPI is hardware-independent; consider adding DL_PHYS_ADDR_REQ + real multicast |
| `aenxmit` | **Rewrite the hardware half**: replace `txptr[i].mode&TMD1_OWN` gate + board-RAM bcopy + OWN-bit store with Z3660 TX-mailbox poke (`REG_ZZ_ETH_TX`); keep the header-assembly (dest/src/sap) logic |
| `aenintr` | **Rewrite**: poll/ack Z3660 eth interrupt (`AMIGA_INTERRUPT_ETH`) instead of LANCE CSR0 |
| `receive_interrupt` | **Rewrite**: drain firmware RX ring instead of LANCE multi-buffer reassembly |
| `toss_packet_up_stream` | **Reuse almost verbatim** — SAP demux + DL_UNITDATA_IND build + putnext are hardware-independent |
| `transmit_interrupt` | **Reuse structure** (getq/aenxmit/putbq), retarget hardware gate |
| `aenautoconfig`/`autocon`/`get_auto_serial_number` | **Rewrite**: get base + MAC from Z3660 firmware/GEM, not Zorro autoconfig EEPROM |
| `setup_lance`/`initialize_and_go` | **Rewrite**: firmware init handshake replaces LANCE CSR0_INIT + sleep/IDON/wakeup |
| `aenioctl` (GET/SET_CONFIG, stats) | **Reuse structure**; retarget the SET_CONFIG→hardware reprogram path |
| `lance.h`/`struct initblk`/`struct ringptr` | **Discard** — replace with Z3660 firmware ring/mailbox layout |

### Gotchas / sharp edges to carry forward
- **`splaen` == `spl2`** (`aen.h:69`) — must match the Z3660 eth IPL.
- RX path uses a **single static `rxbuff[]`** — not reentrant across CPUs but fine on UP 68030; keep or switch to per-board.
- The read-side qinit has **no rput/rsrv**; all RX delivery is `putnext` from interrupt context — keep this model.
- DLPI multicast/promisc is bolted on via a **private ioctl**, not standard primitives — a cleaner Z3660 port should prefer DL_ENABMULTI/DL_PROMISCON.
- `count`/`length` bookkeeping in `receive_interrupt` is subtle (`aen.c:1114-1141`) and the `count` passed to `toss_packet_up_stream` is the **last descriptor's byte count**, not total — re-derive cleanly for Z3660 rather than copying the quirk.

## SURVEY D — HYDRA driver (closest precedent)
Confirmed. The seam is identical between aen and hydra (same function-name skeleton, `spl2`, `BOARD_RUNNING` deferred-init-on-open, `toss_packet_up_stream`). I have all the evidence needed.

---

# SURVEY D — Hydra STREAMS/DLPI driver: the seam to reuse + how to fill it for Z3660

**Bottom line:** Hydra is `aen` (the stock A2065/LANCE driver) with **only the chip layer swapped** — the entire STREAMS/DLPI/DLPI-ifnet machinery is copied verbatim (same function skeleton, same `spl2`, same `toss_packet_up_stream`, same `transmit_interrupt` requeue). The seam is **four functions + a handful of accessors**. For Z3660 we keep the Hydra/aen STREAMS half unchanged and replace the DP8390/NE2000 register/ring half with Z3660-mailbox calls. This is the *exact* same move Hydra made (LANCE→NE2000), so it is the right template.

---

## 1. KEEP vs REPLACE — drawing the seam

The aen→hydra diff proves the seam empirically: every STREAMS function has the same name with the prefix swapped, same args, same body shape. **KEEP** = the STREAMS/DLPI layer (chip-agnostic); **REPLACE** = anything touching `nic`/`board_base`/the ring.

### KEEP unchanged (copy from hydra.c, rename `hydra`→`z3660eth`)
| Element | hydra.c:line | What it is |
|---|---|---|
| `module_info` / `qinit` / `streamtab` | 67–85 | STREAMS registration triplet — tag `0x6879 "hya"`, hiwat/lowat |
| `hydraopen` | 230–312 | minor→board/sap decode, deferred init-on-open, `ifstats` attach, `if_flags` |
| `hydraclose` | 314–353 | tear down stream, stop NIC if last close |
| `hydrawput` | 355–396 | the STREAMS write `put` — M_FLUSH / M_PROTO / M_IOCTL switch |
| `hydrawsrv` | 398–402 | no-op write service |
| `hydraproto` | 857–1023 | **the DLPI contract**: `DL_UNITDATA_REQ`, `DL_ATTACH/DETACH`, `DL_BIND/UNBIND`, `DL_INFO_REQ` |
| `hydraioctl` | 470–855 | SIOCSIFFLAGS/SIOCGIFFLAGS, board-count/config/status ioctls, M_COPYIN/COPYOUT plumbing |
| `toss_packet_up_stream` | 1250–1411 | **RX → DLPI delivery**: SAP demux across streams, trailer decode, dest-MAC/broadcast filter, builds `dl_unitdata_ind_t`, `putnext` |
| `transmit_interrupt` | 1413–1436 | TX-complete: dequeue next stream's queued mblk, re-call xmit |
| `hydraintr` outer loop | 1025–1047 | per-board iterate, `panicstr` guard, status fetch, RX/TX/err dispatch |

The DLPI message-construction is **completely chip-independent** — `toss_packet_up_stream` (1342–1389) builds `dl_unitdata_ind_t` from a flat `rxbuff[]`; `hydraxmit` (416–432) reads `dl_dest_addr_offset` and assembles a flat ethernet frame in `tmp[]`. **Neither knows what hardware moved the bytes.** That flat buffer (`rxbuff`/`tmp`) IS the seam.

### REPLACE (the chip layer — these all touch DP8390 registers / card SRAM)
| Element | hydra.c:line | DP8390-specific thing to rip out |
|---|---|---|
| `hydra_inb` / `hydra_outb` | 98–114 | byte register access at `nic_base + reg*2` + `NIC_DELAY()` |
| `hydra_ram_write/read[_offset]` | 116–203 | direct card-SRAM long-word access at `board_base + page<<8` |
| `hydra_test_write_reg` | 205–228 | TBCR1/FIFO-trap guard (NE2000-only hazard) |
| **`hydraxmit`** | 404–468 | assembles frame in `tmp[]` then **programs TPSR/TBCNT/CR-TXP** (452–458) |
| `receive_interrupt` | 1107–1248 | walks the **NE2000 RX ring** (CURR/BNRY, 4-byte ring header, page wrap) |
| `hydraintr` body | 1042–1102 | reads ISR, ACKs PRX/PTX/TXE/CNT bits, reads TSR/CNTR0 |
| `get_ethernet_address` | 1438–1489 | MAC from PROM (`+0xffc0` step-2) or PAR registers |
| `hydraautoconfig` | 1538–1655 | 3-method Zorro-II probe (autocon + I/O-slot + mem-space) |
| `hydra_initialize` | 1657–1687 | sets `nic_base = board_base + 0xffe1`, calls get_mac + setup |
| `setup_ne2000` | 1689–1796 | full DP8390 init: DCR/PSTART/PSTOP/BNRY/PAR/MAR/TCR/RCR/IMR |
| `hydra_reset` | 1798–1824 | DP8390 soft reset via ISR_RST poll |
| `ne2000.h` | (all) | every register/bit definition — irrelevant to Z3660 |

**The seam line, precisely:** TX seam = the flat `tmp[]` buffer in `hydraxmit` (414) just after it is filled (line 451) and before `hydra_ram_write`+register-poke (452–458) — replace 452–458 with "hand `tmp[0..pktsize]` to the Z3660 TX mailbox." RX seam = the flat `rxbuff[]` in `receive_interrupt` (1114) at the moment it is full (1224) and handed to `toss_packet_up_stream` (1246) — replace the whole ring-walk (1120–1246) with "pull one frame from the Z3660 RX mailbox into `rxbuff`, set `pktsize`, fall through to `toss_packet_up_stream`."

---

## 2. Hydra's chip layer: TX, RX, interrupt structure

### TX — `hydraxmit` (404–468)
1. **Gate** on chip-busy: read CR, if `NE_CR_TXP` set, return 0 (caller requeues) — hydra.c:419–425.
2. **Assemble flat frame** in `tmp[ETH_MAXPACKET+2]`: dest MAC from `mp->b_rptr + dp->dl_dest_addr_offset` (429–430), src MAC from `info->paddress` (431), 2-byte SAP/ethertype from `hp->sap` (432), then walk `mp->b_cont` chain copying payload (438–445), `freemsg` (447).
3. **Pad** to `ETH_MINPACKET` (60) if short — 449–450.
4. **Hand to chip**: `hydra_ram_write` into TX page (452), then program **TPSR=tx_start_page, TBCNT0/1=len, CR |= TXP** to fire (454–458).
5. Bump counters, return 1.

➤ **Z3660 fill:** keep steps 1–3 verbatim (the flat `tmp[]` is the deliverable). Replace step 4 (452–458) with a Z3660 TX-mailbox submit of `(tmp, pktsize)`. The busy-gate in step 1 becomes "is the TX mailbox slot free?".

### RX — `receive_interrupt` (1107–1248)
NE2000 ring walk: loop reading **CURR (page1) vs BNRY (page0)** (1131–1137); if `next_pkt==curr`, ring empty, break (1144); read **4-byte ring header** `[next_page, rsr, size_lo, size_hi]` (1148–1161); validate next-page range (1164) and RSR.PRX (1178) and size (1196); copy frame body out of card SRAM into `rxbuff` **with page-wrap split** (1211–1224); advance BNRY (1238–1243); `toss_packet_up_stream(rxbuff, board_index, pktsize)` (1246). A `loop_count>256` guard prevents wedging (1125).

➤ **Z3660 fill:** the *entire* ring machinery is NE2000-specific. Replace with "while the Z3660 RX mailbox has a frame: copy it into `rxbuff`, set `pktsize`, call `toss_packet_up_stream`." Keep the loop-count guard idea and the `ifs_ipackets++` bookkeeping.

### Interrupt — `hydraintr` (1025–1105)
- Non-static (kernel-visible), no args; `if (panicstr) return` first (1030).
- Per-board loop; only touch boards in `HYDRA_BOARD_RUNNING` with non-zero nic_base (1037).
- Read ISR, **ACK the bits you handle** (`hydra_outb(nic,NE_ISR,...)`), dispatch: PRX→`receive_interrupt`, PTX→`transmit_interrupt`, TXE→TSR error decode, CNT→missed-packet counter (1049–1102).
- `transmit_interrupt` (1413–1436) dequeues the next stream's flow-controlled mblk and re-fires xmit — this is how DLPI back-pressure drains.

➤ **Z3660 fill:** keep the outer skeleton (panicstr guard, per-board loop, RUNNING gate, the `receive_interrupt`/`transmit_interrupt` calls). Replace "read ISR / ACK bits" with reading/acking the **Z3660 interrupt-status register** (`AMIGA_INTERRUPT_ETH` per the project brief — verify the exact reg in firmware `rtg.c`). **Caution (§coherency):** Hydra's ISR reads chip registers; Z3660's RX status will be a *shared memory mailbox* — see §5.

**Interrupt level:** `splhydra == spl2` (hydra.h:20), wired as **INT2 level-2 autovector** via `int2_tbl[]`. Z3660 SCSI already uses an INT2 entry; the eth driver gets its own `int2_tbl[]` slot.

---

## 3. Registration — cdevsw slot 47 / tag "hya" (confirmed)

All four wiring points are in `master.d/kernel.c`. To add the Z3660 eth driver, replicate each at a **free slot** (47 is taken by hydra; aen=18, slip=19; pick the next free char-major):

1. **streamtab in cdevsw[]** — `kernel.c:397`:
   ```
   ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&hydrainfo,nullflag,    /*47=hydra*/
   ```
   (aen is `kernel.c:357` slot 18 `&aeninfo`.) Add a new row `…,&z3660ethinfo,nullflag, /*N=z3660eth*/`.
2. **extern streamtab decl** — `kernel.c:266`: `extern struct streamtab hydrainfo;` → add `extern struct streamtab z3660ethinfo;`
3. **interrupt vector in int2_tbl[]** — `kernel.c:194` `hydraintr,` (after `aenintr` at 193); extern at `kernel.c:187`. Add `z3660ethintr,` + its extern.
4. **boot probe in init_tbl[]** — `kernel.c:61` `hydrainit,`; extern at `kernel.c:38` (`extern void … hydrainit();`). Add `z3660ethinit,` + extern.

**Device node:** `mknod /dev/z3660eth0 c <major> 0` (hydra uses `mknod /dev/hya0 c 47 0`).

**Driver subdir + Makefile:** `usr/sys/amiga/driver/hydra/Makefile` (3-line: `OBJ=hydra.o`, `ld -r -o exp $(OBJ)`, header deps). Mirror as `driver/z3660eth/Makefile`. Kernel relink picks up `exp` from the subdir.

> ⚠️ **Build-tooling gap (matches the brief):** none of this is generated by `build-kernel.sh` (it only emits `scsicard[]` into `sd.c`). All four kernel.c edits + the subdir/Makefile are **hand-patches** — exactly the "new STREAMS path" the kerntools needs. The clean-gate (relink-until-`sum -r` recurs) is reusable unchanged.

---

## 4. Bring-up: slink + ifconfig, node, presence gate, frame gotchas

**Plumb sequence** (grimoire hydra.md, SVR4.0 — **no `plumb`**):
```sh
slink addaen /dev/hya0 hya0
ifconfig hya0 <ip> netmask <mask> up -trailers
route add default <gateway> 1        # trailing 1 = required metric
```
Note the `slink` subcommand is literally **`addaen`** (the stock A2065 link op — Hydra reuses it; not "addhya"). For Z3660: `slink addaen /dev/z3660eth0 z3660eth0; ifconfig z3660eth0 192.168.2.39 netmask … up -trailers`.

**Boot-time:** entries go in `/etc/inet/network-config` (sourced by `/etc/rc2.d/S69inet`), **guarded by a `-S` presence check** so the boot script doesn't hang on `slink` when no board is present.

**Presence gate `-S`** — `hya/hya.c:43–46, 82–109`: opens `/dev/hya0`, runs `HYDRA_NUMBER_OF_BOARDS` ioctl, exit 0 if a board found else 1, silent. For Z3660 ship the analogous tiny tool backed by a board-count ioctl. (On the WinUAE build box this `-S` will correctly return "no board," so boot won't hang — the safe-to-test property.)

**Frame gotchas Hydra documents (all reusable for any ethernet datapath, Z3660 included):**
- **Min frame 60 vs 64** — `ETH_MINFRAME = ETH_MINPACKET − ETH_CRC_LEN = 60` (hydra.h:31). The wire reports byte-count *including* the 4-byte CRC; after stripping CRC a minimum frame is **60**, not 64. The old code's `<64` reject dropped minimum-size ARP replies → ARP never completed → ping never started. **This was THE unlock.** RX rejects `<ETH_MINFRAME` and advances; it does **not** pad (hydra.c:1196). TX pads up to `ETH_MINPACKET=60` (hydra.c:449–450). ➤ For Z3660, confirm whether the firmware delivers frames **with or without CRC** (firmware `ethernet.c`) — the 60-vs-64 boundary depends on it.
- **Register inter-access delay** — `NIC_DELAY()=DELAY(5)` after every register write (hydra.c:96). NE2000-specific; Z3660 mailbox won't need it, but watch for an analogous "doorbell settle" requirement.
- **MTU** — `HYDRA_MTU = 1518−14−4 = 1500` (hydra.h:11); `dl_max_sdu = ETH_MAXDATA = 1500` in `DL_INFO_ACK` (hydra.c:941).

---

## 5. What Hydra got wrong/hard — anticipate for Z3660

From source comments + grimoire "Known issues":

1. **RX min-frame (60 not 64)** — already the #1 unlock above. Single biggest "got it wrong, then fixed" item; cost ARP+ping entirely. **Anticipate the exact analogue:** does the Z3660 firmware hand us the CRC? Get the boundary right or ARP silently fails.

2. **Remote-DMA hang → write straight to buffer RAM.** The standard NE2000 byte-at-the-data-port RDMA **doesn't raise RDC on Hydra**, so Hydra bypasses it and writes **directly to card SRAM** (`hydra_ram_write/read` at `board_base+(page<<8)`). ➤ **Direct lesson for Z3660:** prefer the firmware's direct shared-buffer path over any "DMA doorbell + poll completion" handshake if completion signaling is unreliable. This is structurally the same choice we'll face with the Z3660 mailbox.

3. **TBCR1 / register-6 / FIFO trap.** Raw reads/writes around DP8390 **reg 6** can **trap the Amix kernel while the NIC is stopped** (hydra.c:217–225, 522–536, 803–817). `hydra_test_write_reg` guards it; `HYDRA_TEST_GET_STATE` was **gutted to return zeros** rather than snapshot live registers (1522 comment: "diagnostics must not be able to panic the kernel by requesting a register snapshot"). ➤ **Z3660 lesson:** any diagnostic that pokes shared MMIO can panic; make the diag tool read-only/safe by construction, and never let userland trigger a live-register read that can bus-trap.

4. **`autocon()` bootinfo table corruption on 2.1p2** → 3-method probe with address validation (hydra.c:1551–1654). ➤ Z3660 is discovered the same way the SCSI side already is (it's the same Zorro/firmware device) — reuse the proven Z3660 discovery, **don't** trust raw `autocon()`.

5. **MAC PROM byte-lane** (`+0xffc0` step-2) — NE2000-specific; for Z3660 the MAC comes from the firmware/GEM, not a Zorro PROM. Drop entirely; get the MAC via a Z3660 mailbox query.

6. **★ CACHE COHERENCY (the brief's first-class risk, and the gap Hydra does NOT cover).** Hydra's TX/RX buffers are **on-card SRAM** reached via `volatile` MMIO at `board_base+offset` — Zorro II MMIO is inherently uncached on the 030, so Hydra **never had to think about cache coherency**. The Z3660 shares eth frames in a **non-cached DDR window (`ETHERNET_CACHE_POLICY = NORM_NONCACHE`)** — *if* that mapping is honored guest-side, `volatile` access is similarly safe; but the SCSI path's bounce-buffer/`dsb`-ordering troubles (and the open "lpsched Bad vp" coherency bug) show this is where Z3660 bites. **Hydra gives us NO precedent here** — it is the one place the template runs out, and where we must lean on the Z3660 SCSI driver (`z3660.c`) and firmware (`rtg.c`/`ethernet.c`) instead of Hydra. Treat the `rxbuff`/`tmp` ↔ shared-DDR boundary as the coherency frontier: confirm the guest maps the eth window non-cacheable, mirror whatever ordering `z3660.c` does on its mailbox, and verify on real HW (the WinUAE box can't exercise it — it has an emulated a2065, not the GEM).

---

### One-paragraph synthesis for the plan
Copy `hydra.c` → `z3660eth.c`, rename `hydra`→`z3660eth`, and **delete the chip half** (`hydra_inb/outb`, `hydra_ram_*`, `setup_ne2000`, `hydra_reset`, `get_ethernet_address`, the NE2000 ring-walk in `receive_interrupt`, the register-poke tail of `hydraxmit`, all of `ne2000.h`). Keep the STREAMS/DLPI half **byte-for-byte** (open/close/wput/wsrv/proto/ioctl/`toss_packet_up_stream`/`transmit_interrupt`/intr-skeleton). Fill the two seams — `tmp[]`→Z3660 TX mailbox (replace hydra.c:452–458) and Z3660 RX mailbox→`rxbuff[]`→`toss_packet_up_stream` (replace hydra.c:1120–1246) — using the Z3660 mailbox/interrupt regs from firmware `rtg.c`/`ethernet.c`, and get MAC + discovery from the Z3660 path (reuse `z3660.c`'s approach, not Hydra's PROM/autocon). Wire it into `kernel.c` at a free cdevsw slot mirroring slot-47/`int2_tbl`/`init_tbl`/extern (4 hand-edits), add a `driver/z3660eth/Makefile`, `mknod`, and bring up with `slink addaen /dev/z3660eth0 z3660eth0; ifconfig z3660eth0 192.168.2.39 … up -trailers` gated by a `-S` presence tool. Carry forward Hydra's 60-vs-64 min-frame lesson and its "trust the firmware's direct shared buffer, not a DMA-completion handshake" lesson; the **one thing Hydra cannot teach is shared-DDR cache coherency** — for that, follow `z3660.c` and verify on real hardware.

**Files cited:** `hydra-amix/usr/sys/amiga/driver/hydra/{hydra.c, hydra.h, hydrauser.h, ne2000.h, Makefile, hya/hya.c, hya/Makefile}`; `hydra-amix/usr/sys/amiga/driver/aen/{aen.c, aen.h}`; `hydra-amix/usr/sys/master.d/kernel.c` (lines 38, 61, 187, 194, 266, 357, 397); `grimoire-amix/docs/drivers/case-studies/hydra.md`.

## SURVEY E — Kernel integration + kerntools STREAMS path
I now have everything required. Producing the survey.

---

# SURVEY E — Kernel Integration + the kerntools STREAMS path we must build

All findings below are read directly from the hydra-amix tree (`~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys`) and amix-kerntools. The two reference net drivers are **`aen`** (stock LANCE/a2065 — major **18**) and **`hydra`** (DP8390/NE2000 AmigaNet — major **47**). The whole STREAMS-driver registration surface is in **`master.d/kernel.c`** — there is no separate `master`/`system` config in this 4.0 tree.

---

## 1. The `cdevsw[]` slot for a STREAMS net driver

`struct cdevsw` has 13 fields; the column layout is documented at the top of the array, `master.d/kernel.c:325-327`:

```
struct cdevsw cdevsw[] = {
/* open close read write ioctl
        mmap segmap poll xpoll xhalt ttys stream flag */
```
i.e. `{ d_open, d_close, d_read, d_write, d_ioctl, d_mmap, d_segmap, d_poll, d_xpoll, d_xhalt, d_ttys, d_str, d_flag }`.

A STREAMS net driver fills **only** `d_ttys = notty`, `d_str = &<drv>info` (the address of its `struct streamtab`), and `d_flag = nullflag` (newstyle/non-D_OLD). **Every** procedural slot (open/close/read/write/ioctl/mmap/segmap/poll/xpoll/xhalt) is `ND` (= `nodev`). The kernel routes char I/O through the streamtab's queue procedures instead of the `d_*` function pointers.

The literal rows (note `nodev`/`notty`/`nostr`/`nullflag`/`oldflag` are macros defined at `kernel.c:215-219,283-284`):

```c
/* kernel.c:357 — aen, major 18 (a2065 LANCE) */
ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&aeninfo,nullflag,		/*18=aen*/

/* kernel.c:397 — hydra, major 47 (DP8390 AmigaNet) */
ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&hydrainfo,nullflag,		/*47=hydra*/
```

For contrast, the `sl` (slip dlpi) and `loop` rows that sit beside aen confirm the same idiom (`kernel.c:358-359`):
```c
ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&sldinfo,nullflag,		/*19=slip dlpi*/
ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&loopinfo,oldflag,		/*20=loop*/
```
(`loop` uses `oldflag` = `D_OLD`; aen/hydra/sldinfo use `nullflag` = newstyle STREAMS. Our driver should use `nullflag` like aen/hydra.)

The `&aeninfo`/`&hydrainfo` symbols are declared extern at the top of the cdevsw block (`kernel.c:265-266`):
```c
extern struct streamtab aeninfo;
extern struct streamtab hydrainfo;
```

**The streamtab itself lives in the driver** (`aen/aen.c:60-78`, `hydra/hydra.c:67-85`) — this is the entire DLPI registration:
```c
/* aen/aen.c */
static struct module_info aen_minfo =
    { 0x656E /*'en'*/, "aen", AEN_MIN, AEN_MAX, AEN_HIWAT, AEN_LOWAT };
static struct qinit aenrinit =                 /* READ (downstream-from-driver) side */
    { NULL, NULL, aenopen, aenclose, NULL, &aen_minfo, NULL };
static struct qinit aenwinit =                 /* WRITE (user->driver) side */
    { aenwput, aenwsrv, NULL, NULL, NULL, &aen_minfo, NULL };
struct streamtab aeninfo =
    { &aenrinit, &aenwinit, NULL, NULL };       /* st_rdinit, st_wrinit, st_muxrinit=NULL, st_muxwinit=NULL */
```
Hydra is byte-identical in shape (`hydra.c:67-85`) — module id `0x6879`/`"hya"`, `qinit`s pointing at `hydraopen/hydraclose/hydrawput/hydrawsrv`. The module_info bounds (`aen.h:41-44`, `hydra.h:13-16`): MIN ≈ min Ethernet frame minus CRC, MAX = MTU, HIWAT = 5 KB, LOWAT = 2/3 HIWAT. **A leaf (non-mux) driver leaves `st_muxrinit`/`st_muxwinit` NULL.**

> Practical note for our driver: pick a free major. The next genuinely free low slot is **major 7** (`kernel.c:342 /*7=win?*/` is `nostr`), or any of 48/49/51-69 which are all empty `ND...,nostr,nullflag` rows (`kernel.c:398-419`). aen(18)/hydra(47) are taken.

---

## 2. `int2_tbl[]` (interrupt) and `init_tbl[]` (boot probe) entries

### 2a. Interrupt — `int2_tbl[]`

The Amiga level-2 (PORTS/Zorro) ISR `p2int` in `amiga/ml/ttrap.s:40-46` walks `int2_tbl` as a **NULL-terminated array of ISR pointers and calls every entry on every level-2 interrupt**; each ISR self-checks whether *its* board actually raised the IRQ:
```asm
	lea.l	int2_tbl,%a2
p2loop:	mov.l	(%a2)+,%d0
	beq	p2end
	mov.l	%d0,%a0
	jsr	(%a0)
	bra	p2loop
```
So registration = "append your ISR before the terminating 0". The externs MUST precede the array (`kernel.c:187`), then the array (`kernel.c:188-195`):
```c
extern void aciaaintr(), jbintr(), a2091intr(), a3091intr(), aenintr(), hydraintr();
void	(*int2_tbl[])() = {
	aciaaintr,		/* ACIAA (parallel/kbd) ISR */
	jbintr,			/* A2090 ISR */
	a2091intr,		/* A2091 ISR */
	a3091intr,		/* A3000 internal SCSI ISR */
	aenintr,		/* A2065 Ethernet ISR */
	hydraintr,		/* Hydra AmigaNet ISR */
	0};
```
Both ISRs guard with `if (panicstr) return;` then loop their `MAXBOARDS` board table checking a `board_state == *_BOARD_RUNNING` flag and the chip's own ISR/CSR0 register (`aen/aen.c:934-958`, `hydra/hydra.c:1025-1047`). **Our Z3660 ISR row would be added the same way** — append `z3660ethintr,` before the `0`, with a matching extern.

> COHERENCY HOOK (ties to the stated risk): because `p2int` polls *all* level-2 ISRs, our ISR will read the firmware's shared RX-ring/status word in the NON_NONCACHE eth window on every level-2 IRQ. That status read is the first place an 030 cache/ordering bug would bite; the self-check register read must come from the non-cached window, exactly as aen reads `*board->aen_info.lance_data` (`aen.c:944-945`) — volatile, uncached MMIO, never a cached bounce copy.

### 2b. Boot probe — `init_tbl[]` (NOT `io_init[]`)

There are several boot-time function-pointer arrays in `kernel.c`: `init_tbl[]` (44-62), `io_init[]` (198-200, only `parinit`), `io_start[]`/`io_halt[]` (202-206, empty), `io_poll[]` (209-212, `qlintr`/`slpoll`). The **net drivers use `init_tbl[]`**, walked by the kernel's startup `main()` (the consumer is in the compiled `master.d/kernel` startup object; the `init_tbl(...)` marker is visible in `master.d/kernel`). Externs precede the array (`kernel.c:38`), then:
```c
extern void strinit(),msginit(),seminit(),sadinit(),hydrainit();   /* :38 */
...
void	(*init_tbl[])() = {
	fpuinit, cinit, binit, inoinit, vfsinit, finit,
	strinit, msginit, seminit, sadinit,
	tclinit, tcoinit, tcooinit,
#ifdef KERNEL_DEBUGGER
	kdb_init_hooks,
#endif
	hydrainit,                  /* :61  <-- hydra's autoconfig/probe entry */
	0};
```
`hydrainit()` (`hydra/hydra.c:1826-1831`) is a thin wrapper that just calls `hydraautoconfig()` (Zorro probe → fills `hydra_board[]`, sets `board_state=RUNNING`). **This is the precedent our driver follows: add `z3660ethinit,` to `init_tbl[]` with a preceding extern, and have it probe the Z3660 eth and arm the board state the ISR checks.**

**Critical asymmetry to be aware of:** `aen` is in `cdevsw[]` (18) and `int2_tbl[]` but is **NOT** in `init_tbl[]`. Its `aenautoconfig()`/`aen_initialize()` are only invoked under `#ifdef INITIALIZE_AEN_DEVICE` from the kernel-debugger hook `kdb/hooks.c:107-120` (a non-default debug path). In the stock build `aen` is brought up lazily on first `open()`/DLPI attach. **`hydra` is the modern, clean precedent** (self-contained `init_tbl` probe at boot) and is the one our Z3660 driver should mirror — do NOT copy aen's kdb-gated init.

---

## 3. Driver subdir + Makefile mechanism (vs the alien/ SCSI path)

Each net driver is its **own subdirectory under `amiga/driver/`** with its own Makefile that links one `.o` into an `exp` (incremental-link "export") object, which the parent `driver/Makefile` then folds into the big `driver/exp`. This is **different** from the SCSI path (kerntools today), which adds objects flat into `amiga/alien/`.

Parent `amiga/driver/Makefile` (`:3-30`):
```make
OBJ	= \
	  acia.o amiga.o ben.o bb.o cl.o dummy.o hd.o jb.o machid.o \
	  par.o ram.o ql.o sl.o slip.o tiga.o audio.o \
	  aen/exp \
	  hydra/exp # kdb/exp
exp :	$(OBJ)
	ld -r -o exp $(OBJ)
aen/exp :
	cd aen; $(MAKE)
hydra/exp :
	cd hydra; $(MAKE)
```
Each subdir Makefile (e.g. `aen/Makefile`, `hydra/Makefile`) is a 3-include-dir compile + self incremental-link:
```make
CFLAGS	= -O -D_KERNEL -DSVR40 -DSVR4 -I../../.. -I../../inc $(KDB)
OBJ	= hydra.o
exp:	$(OBJ)
	ld -r -o exp $(OBJ)
clean:
	-rm -f $(OBJ) exp
hydra.o : hydra.h hydrauser.h ne2000.h ../kdb/kdebug.h
```
Note the **deeper include path** `-I../../..` (reaches `usr/sys`) and `-I../../inc` — because it's one level deeper than `alien/`. (The SCSI alien template uses `-I../..`/`-I../inc`, see template below.)

**Contrast with the SCSI path** — kerntools' `templates/alien-Makefile.in` puts driver `.o`s flat into one `amiga/alien/exp`:
```make
.c.o:
	$(CC) -c -O $(CFLAGS) -I../.. -I../inc $<
OBJ	= ct.o dd.o a2090.o a2091.o a3091.o@DRIVER_OBJS@ sd.o sdpart.o physdsk.o scsi.o
exp:	$(OBJ) Makefile
	ld -r -o exp $(OBJ)
```
So: **SCSI = flat object added to `alien/Makefile`'s `OBJ` + a `scsicard[]` row in the generated `alien/sd.c` (the HBA registry under the `sd` driver).** That is purely data-table registration — no `cdevsw`/`int2_tbl`/`init_tbl` edits, because the SCSI HBA is a sub-driver under the already-wired `sd`/`gsioctl` major-11 device.

A STREAMS net driver has **no such umbrella driver**. It needs (a) its own `driver/<name>/` subdir + Makefile, (b) an `<name>/exp` line in `driver/Makefile`'s `OBJ` and a `<name>/exp:` rule, and (c) **three hand edits to `master.d/kernel.c`** (cdevsw row, int2_tbl entry, init_tbl entry, each with its extern). The clean-gate links the whole kernel the same way regardless.

---

## 4. Any other tables touched? (fmodsw / name→major map / /dev nodes / master config)

Surveyed exhaustively:

- **`fmodsw[]`** (`master.d/filesys.c:71-93`) — this is for **pushable STREAMS modules** (`ldterm`, `timod`, `tirdwr`, `sockmod`, `slipmod`, `ptem`, `pckt`, `connld`...), NOT for device drivers. A leaf net **driver** is opened as a device (via its `cdevsw[].d_str`), not pushed by name. **Our driver does NOT go in `fmodsw[]`.** (`slipmod` being here is the STREAMS *module* half of SLIP; the SLIP *driver* half `sldinfo` is in `cdevsw[19]`. We are the driver half only.)

- **No name→major map / no `master`/`mdevice`/`sdevice`/`system` file.** `ls master.d/` shows per-subsystem `.c`/`.o` and module dirs, but **no SVR4-style `mdevice`/`sdevice`/`system` config** — this 4.0 tree predates the config-file driver model. The major-number↔streamtab binding is *only* the position in `cdevsw[]` (slot index = major number; the trailing `/*18=aen*/`,`/*47=hydra*/` comments are just documentation). **Confirmed: registration is wholly via `cdevsw[].d_str` (+ the two boot/intr tables). There is no second registry to keep in sync.**

- **/dev node** — required at runtime but is **userland, not a kernel table**: a `mknod /dev/<name> c <major> <minor>` (char-special, the major matching the cdevsw slot). The golden `/dev` was unreadable this session (stale NFS handle), so I could not confirm aen's exact node name/minor on the golden image; this needs an on-box `ls -l /dev` check. The driver itself does not create the node. `slink`+`ifconfig <if> <ip> up -trailers` (SVR4.0, no `plumb`) then attaches the DLPI stream over that node — that's the userland bring-up the project context already establishes.

- **`unix.c`** (`amiga/config/unix.c`) — only root/swap/dump bootobj config (`rootdev`, `dumpdev`, `swapfile`). **Not touched** by a net driver. (The task hypothesis that it carries cdevsw/streamtab refs is not borne out — those are all in `kernel.c`.)

So the complete kernel-side touch list for a STREAMS net driver is exactly: **`cdevsw[]` row, `int2_tbl[]` entry, `init_tbl[]` entry (all in `master.d/kernel.c`), plus the `driver/<name>/` subdir + its Makefile + the `<name>/exp` hook in `driver/Makefile`.** Userland adds `mknod` + `slink`/`ifconfig`.

---

## 5. CONCRETE SPEC — extending `build-kernel.sh` with a STREAMS path

### What the SCSI path does today (baseline, build-kernel.sh:63-121)
For each `driver.conf` line `<pn> <queuefn> "<name>" <src> [probe=<fn>]` it: (1) appends to `EXTERNS`/`CARDS`/`OBJS`/`PROBES`; (2) renders `templates/sd.c.in` → `work/sd.c` (`@DRIVER_EXTERNS@`,`@DRIVER_CARDS@`,`@DRIVER_PROBES@`) and `alien-Makefile.in` → `work/Makefile` (`@DRIVER_OBJS@`); (3) FTP-pushes driver sources + `sd.c` + `Makefile` to **`/usr/sys/amiga/alien/`**; (4) runs the clean-gate `build-clean-kernel.sh`. **Everything lands in `alien/`; `kernel.c` is never touched.**

A STREAMS driver cannot reuse this: it edits `kernel.c` (3 anchored insertions) and lives in its own `amiga/driver/<name>/` subdir, not `alien/`.

### New config schema — a separate stanza in `driver.conf`
The SCSI line carries SCSI-only fields (product number, queue fn, HBA name). A net driver needs: source file, on-box driver basename, the `struct streamtab` symbol, the `init`/`intr` symbol names, and a chosen major number. Proposal — a typed line, default type `scsi` for back-compat:

```
# existing (unchanged), implicitly type=scsi:
0x144B0001  z3660queue  "Z3660 SCSI"  z3660.c  probe=z3660present

# new STREAMS net stanza:
net  z3660eth.c  major=7  streamtab=z3660ethinfo  init=z3660ethinit  intr=z3660ethintr  "Z3660 Ethernet"
```
Parsed fields: `src` (under `src/`), `major` (free cdevsw slot — validate against the live `kernel.c` so we don't collide; 7 or 48+ are free), `streamtab`/`init`/`intr` symbol names, display name. The on-box basename derives from `src` (e.g. `z3660eth.c` → subdir `z3660eth/`, object `z3660eth.o`).

### Files it must generate / patch

Unlike SCSI (regenerate the whole `sd.c` from a template), the net path **anchored-inserts into a pristine `kernel.c`** (and `driver/Makefile`) — there's no template to regenerate the 70-row `cdevsw[]`. Steps:

1. **Fetch pristine `kernel.c` and `driver/Makefile`** from the box (or keep a known-good copy in kerntools) so insertions are idempotent (re-running must not double-insert — match on a `/* z3660eth */`-style guard comment and skip if present, or always start from pristine).

2. **`master.d/kernel.c` — three anchored insertions** (per net driver):
   - **cdevsw row**: at the chosen major slot, replace that slot's `ND,...,nostr,nullflag, /*N*/` line with
     `ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&<streamtab>,nullflag, /*N=<name>*/`
     and add `extern struct streamtab <streamtab>;` to the streamtab extern block (anchor: after `kernel.c:266 extern struct streamtab hydrainfo;`).
   - **int2_tbl entry**: add `<intr>,` immediately before the terminating `0};` (anchor: the line `hydraintr,` at `kernel.c:194`), and append `<intr>` to the `extern void ...aenintr(), hydraintr();` line (`kernel.c:187`).
   - **init_tbl entry**: add `<init>,` immediately before `0};` (anchor: `hydrainit,` at `kernel.c:61`), and add `<init>` to the extern at `kernel.c:38`.
   Implement as `sed`/`awk` anchored on the **stable existing `hydra*` lines** (they are unique strings), inserting our entries adjacent to hydra's. This is robust because `kernel.c` formatting is fixed.

3. **`amiga/driver/Makefile` — two insertions** (anchor on the `hydra/exp` lines):
   - add `  <name>/exp \` to the `OBJ` list (after `hydra/exp`)
   - add the rule `<name>/exp :\n\tcd <name>; $(MAKE)`

4. **New per-driver subdir Makefile** (generate from a new `templates/net-driver-Makefile.in`, mirroring `hydra/Makefile`):
   ```make
   CFLAGS = -O -D_KERNEL -DSVR40 -DSVR4 -I../../.. -I../../inc
   OBJ    = <name>.o
   exp:   $(OBJ)
           ld -r -o exp $(OBJ)
   clean:
           -rm -f $(OBJ) exp
   <name>.o : <name>.h    # plus any private headers we ship
   ```

5. **Push targets change** from `alien/` to:
   - driver source → `/usr/sys/amiga/driver/<name>/<name>.c` (+ its headers)
   - generated subdir Makefile → `/usr/sys/amiga/driver/<name>/Makefile`
   - patched `kernel.c` → `/usr/sys/master.d/kernel.c`
   - patched `driver/Makefile` → `/usr/sys/amiga/driver/Makefile`

   (Use the existing `amixsync.py push` mechanism, just different remote paths.)

### Clean-gate — reused UNCHANGED (confirmed)
`tools/build-clean-kernel.sh` only does `cd /usr/sys; make` in a loop until `sum -r relocunix` recurs, then `/root/checkunix`. It is driver-agnostic — its only driver-specific bit is the **force-touch list** of source basenames it rebuilds each round (default `a4091.c`; build-kernel.sh passes `$SRCLIST`). For a net driver the touch list just becomes the net source's on-box path. The single adjustment: `build-clean-kernel.sh` currently does `cd /usr/sys/amiga/alien; rm -f $OBJS exp; touch $SRCS` — for a net driver the obj/source live in `amiga/driver/<name>/`, so the touch/rm must target that subdir (or, simpler: pass the full on-box paths and let the top-level `cd /usr/sys; make` rebuild the subtree). **The gate's stabilization logic, sum-recurrence, and `checkunix` are reused verbatim.** Also `kernel.c` itself must be force-touched (or its `.o` removed) so the cdevsw/int2_tbl/init_tbl edits actually recompile — add `master.d/kernel.c` to the rebuild set.

### Exact new steps (delta over today's build-kernel.sh)
1. Parse `net` stanzas from `driver.conf` (new fields: `major`, `streamtab`, `init`, `intr`, name, src); keep `scsi` stanzas on the existing path.
2. For each net driver: render `net-driver-Makefile.in` → `work/<name>/Makefile`.
3. Pull pristine `kernel.c` + `driver/Makefile`; apply the 3 anchored `kernel.c` insertions and the 2 `driver/Makefile` insertions (idempotent, hydra-anchored).
4. Push: net source(+headers) → `amiga/driver/<name>/`, generated subdir Makefile → same, patched `kernel.c` → `master.d/`, patched `driver/Makefile` → `amiga/driver/`.
5. Run the **unchanged** clean-gate, but extend its force-touch/rm set to `master.d/kernel.c` + `amiga/driver/<name>/<name>.c` (so both the wiring and the driver recompile). `--install` (`/usr/sys make install` + `/stand make`) is unchanged.

---

## Key file:line references
- Registration tables — all in **`master.d/kernel.c`**: cdevsw aen `:357`, hydra `:397`; streamtab externs `:265-266`; int2_tbl externs `:187`, array `:188-195`; init_tbl externs `:38`, array `:44-62` (hydrainit `:61`); io_init `:198-200`; macros `notty/nostr/ND` `:215-219`, `oldflag/nullflag` `:283-284`.
- streamtab/qinit/module_info — `aen/aen.c:60-78`, `hydra/hydra.c:67-85`; bounds `aen.h:41-44`, `hydra.h:13-16`.
- ISR walk — `amiga/ml/ttrap.s:40-46`; aenintr `aen/aen.c:934-958`; hydraintr `hydra/hydra.c:1025-1047`.
- init probe wrapper — `hydra/hydra.c:1826-1831` (`hydrainit→hydraautoconfig`); aen's debug-only init — `kdb/hooks.c:107-120`.
- Makefiles — `driver/Makefile:3-30`; `aen/Makefile`, `hydra/Makefile` (`-I../../.. -I../../inc`).
- fmodsw (modules, NOT drivers) — `master.d/filesys.c:71-93`.
- unix.c (root/swap only) — `amiga/config/unix.c:8-22`.
- kerntools — `build-kernel.sh:63-164`; `templates/sd.c.in:24-33,92`; `templates/alien-Makefile.in`; `tools/build-clean-kernel.sh` (whole file); SCSI contrast `amix-z3660/driver.conf` and `src/z3660.c:38-41,164,258`.

**Could not confirm this session:** the golden image's actual `/dev` net-device node name/minor (stale NFS handle on `/tmp/amixgold`) — verify on-box with `ls -l /dev` for the aen node before choosing our minor convention.

## SURVEY F — Userland bring-up + DLPI/IP plumbing
I now have the complete DLPI/datapath contract. The TX path (aenxmit): dest MAC from `dl_dest_addr_offset`, source MAC from the board's own address, ethertype from `aen->sap` (the bound SAP). This confirms the full minimum contract. I have everything needed. Let me write up the survey.

# SURVEY F: USERLAND bring-up + DLPI/IP plumbing

> **MOUNT CAVEAT (read first).** The requested RO golden HDF at `/tmp/amixgold` (loop29 → `amix-kerntools/hdf/Amix-golden.hdf`) is **mounted but I/O-locked** — its UFS hit a read error and `onerror=lock` froze it (`StaleNetworkFileHandle` on every path below the root). The **identical golden image content is readable at `/mnt`** (loop33 → `amix-kerntools/hdf/Amix-golden-raw.hdf`, same Amix root FS), so every `/tmp/amixgold/...` path in my task maps 1:1 to a verified `/mnt/...` path below. **All file:line citations for runtime files are from `/mnt`.** If a later agent needs `/tmp/amixgold` live, it must be unmounted and remounted (the lock is sticky).

---

## 1. Boot/network config — how the stock `aen0` comes up

### Boot chain (who runs what, in order)
- **`/mnt/etc/rc2.d/S69inet`** is a **hardlink** to **`/mnt/etc/init.d/inetinit`** (same inode, confirmed: both 394 bytes, 4 links, identical content). `rc2` runs the `S69inet` name. **Edit in place — never unlink/recreate** (desyncs the pair), and never park an `S*`-named backup in `rc2.d/` (the `for f in /etc/rc2.d/S*` glob would execute it).
- `/mnt/etc/init.d/inetinit` (`case start`), in order:
  1. `inetinit:9` — `ifconfig lo0 >/dev/null 2>/dev/null && exit` (idempotence gate: bail if already up)
  2. `inetinit:11-12` — `/usr/sbin/slink && /usr/sbin/ifconfig lo0 127.0.0.1 up` (run the boot `strcf`, bring up loopback)
  3. `inetinit:14` — `. /etc/inet/network-config` (**source** it — runs the per-interface bring-up)
  4. `inetinit:17` — `exec /bin/sh /etc/inet/rc.inet` (routes/daemons)

### The actual interface bring-up — `/mnt/etc/inet/network-config:10-12`
```sh
/usr/amiga/bin/aen -S &&
    /usr/sbin/slink addaen /dev/aen0 aen0 &&
        /usr/sbin/ifconfig aen0 192.168.2.38 up -trailers
```
This is exactly the **presence-gate pattern**: three commands `&&`-chained so each only runs if the prior succeeds.
- **`/usr/amiga/bin/aen -S`** = the presence gate. It opens `/dev/aen0` and queries the driver (the `AEN_NUMBER_OF_BOARDS`/config ioctl path, `aen.c:709`); exit 0 only if a board answers. Its `usage:` string confirms the flags: `usage: %s [-nzsSc?] [-e addr] [-l addr] [-m mode] [-d dev]` and it has `/dev/aen0` hardcoded as the default dev (strings of `/mnt/usr/amiga/bin/aen`). **This is the "automatically determines if board is in system" gate** referenced in the file's own header comment (`network-config:1-2`).
- **`slink addaen`** links the driver stream under IP (see §3).
- **`ifconfig aen0 192.168.2.38 up -trailers`** assigns the static IP, brings the interface up, and **disables trailer encapsulation** (BSD trailer protocol — the driver supports it at `aen.c:1187-1208`, but `-trailers` is the modern default).

### Where the static IP / hostname / netmask / gateway live
| Setting | File:line | Value (this golden image) |
|---|---|---|
| **Interface IP** | `/mnt/etc/inet/network-config:12` | `192.168.2.38` (literal — see DNS note) |
| **Hostname → IP map / resolver fallback** | `/mnt/etc/inet/hosts:2` | `192.168.2.38 amix amix.alanara.fi` |
| **Gateway / default route** | `/mnt/etc/inet/rc.inet:42` | `route add default 192.168.2.1 1` (**metric `1` mandatory** — not an option flag) |
| **Loopback** | `/mnt/etc/init.d/inetinit:12` | `127.0.0.1` (also literal) |
| **Netmask** | *not set here* | No `netmask` clause; falls to default classful mask for the address. Grimoire's generic form is `ifconfig aen0 <ip> netmask 255.255.255.0 up` (`networking.md:41`) — add it explicitly for /24. |

### The literal-IP-vs-`uname -n` DNS-stall warning (already applied in this image)
- `/mnt/etc/inet/network-config.orig:5` is the **stock** form: `ifconfig aen0 \`uname -n\` up -trailers`.
- `/mnt/etc/inet/network-config:4-12` is the **fixed** form, with an 8-line comment explaining why: with DNS enabled (`libsockdns`), `\`uname -n\`` forces a `gethostbyname()` that tries DNS **first**, but this runs *before* the default route exists (route is in `rc.inet`, sourced later), so each lookup burns the full resolver retransmit (~90 s) before falling back to `/etc/hosts`. **A literal IP skips name resolution entirely.** Grimoire `networking.md:103-136` documents the same trap (boot 210 s → 29 s after the fix) and notes `lo0 localhost` must likewise become `lo0 127.0.0.1` (already done at `inetinit:12`). **Our driver must follow this: configure `aen0`/our-iface with a literal IP, never `\`uname -n\``.**

---

## 2. The device node a net driver needs

- Stock nodes already present: `/mnt/dev/aen0`, `/mnt/dev/aen1` (char devices), plus the protocol clones `/dev/ip`, `/dev/arp`, `/dev/loop`, `/dev/tcp`, `/dev/udp`, `/dev/icmp`, `/dev/rawip`, `/dev/sld0..3`.
- **Major number = the `cdevsw[]` slot.** From `/mnt`-side `hydra-amix/usr/sys/master.d/kernel.c` cdevsw table:
  - **`aen` → slot 18** (`kernel.c:357`: `... notty,&aeninfo,nullflag, /*18=aen*/`)
  - **`hydra` → slot 47** (`kernel.c:397`: `... notty,&hydrainfo,nullflag, /*47=hydra*/`)
  - (The host Linux `stat` reads every Amix node's major/minor as `0,0` — it can't decode the SVR4 rdev encoding. The authoritative major is the cdevsw slot, **not** what Linux `stat` prints.)
- **Minor-number encoding** (`aen/aen.c:88-160`, in `aenopen`): the minor packs two fields —
  - bits **0–3** = `board_index` (`board_index = minor & 0x0F`)
  - bits **4–7** = `sap_index` (`sap_index = (minor & 0xF0) >> 4`); **sap_index 0 = pseudo-clone** that auto-allocates the next free SAP stream.
  - So **`/dev/aen0` is minor 0** = board 0, clone SAP. `AEN_MAXBOARDS=4`, `AEN_MAXDEV=5` (`aen.h:4-5`).
- **Creation** = plain `mknod` (no MAKEDEV script for this in the golden tree; none found under `/mnt/etc`). Grimoire gives the exact line: `mknod /dev/hya0 c 47 0` (`writing-a-streams-driver.md:131`). Our driver follows: `mknod /dev/<if>0 c <ourmajor> 0`.

---

## 3. `slink` — what `slink addaen /dev/aenN aenN` actually does

- **Location / perms:** `/mnt/usr/sbin/slink`, mode `0700 root:bin` (I can't `strings` it — no read permission — so its behavior is read from its config file + grimoire).
- **slink is the SVR4 STREAMS auto-push/link configurator.** It is driven by **`/mnt/etc/inet/strcf`** (the STREAMS config file). Called bare (`slink` at `inetinit:11`) it runs the `boot{}` block; called `slink addaen …` it runs the `addaen{}` function with the args.
- **`addaen` is defined in `/mnt/etc/inet/strcf:87-94`:**
  ```
  addaen {
      ip  = open /dev/ip
      dev = open $1            # $1 = /dev/aen0  -> triggers aenopen()
      aplinkint ip dev $2      # push "app" module, then link dev UNDER ip, name=$2 (aen0)
      dev = open $1            # 2nd open of the driver
      arp = open /dev/arp
      linkint arp dev $2       # link the SAME driver UNDER arp too, name=aen0
  }
  ```
  So **one `slink addaen /dev/aen0 aen0` links the driver stream under *two* multiplexors: IP and ARP**, both tagged interface-name `aen0`. The helpers (`strcf:58-70`):
  - `linkint top bottom name` = `x = link $1 $2` (STREAMS `I_LINK` the bottom stream under the top mux) then `sifname $1 x $3` (assign the interface name so `ifconfig aen0` finds it).
  - `aplinkint` = same but first `push $2 app` (pushes the **`app`** ARP-packing module onto the driver stream before linking under IP).
- **Key reuse fact:** the **Hydra driver reuses `addaen` verbatim** — `slink addaen /dev/hya0 hya0` (`networking.md:174,187`; `writing-a-streams-driver.md:110`). `addaen` is **not** aen-specific; it's the generic "link an Ethernet DLPI driver under IP+ARP" recipe. **Our Z3660 driver will use the same `slink addaen /dev/<if>0 <if>0`** — no new strcf function needed.
- The `boot{}` block (`strcf:109-129`) wires the transports at boot: `tp /dev/tcp`, `tp /dev/udp`, `tp /dev/icmp`, `tp /dev/rawip` (each `link`s the transport under `/dev/ip`), then `loopback` (`/dev/ip` ← `/dev/loop` as `lo0`). `tp` is `strcf:48-52`. **This must already have run (bare `slink` at boot) before `addaen`.**
- **Teardown:** `slink -u` (`inetinit:21`, `case stop`) unlinks everything.

---

## 4. Kernel-level IP/ARP/routing glue + the minimum DLPI our driver must expose

### What connects the DLPI driver to IP/ARP
- **The connection is made by `slink`** (§3): `I_LINK` of the driver stream under `/dev/ip` (with `app` pushed) and under `/dev/arp`. There is **no `ifconfig plumb`** (SVR4.0 predates it — `writing-a-streams-driver.md:9,106`). IP/ARP/routing *logic* lives in the **binary-only `netinet/exp`** in this clone tree (`net/` dir is absent; `netinet/` has only `Makefile`+`exp` — source not redistributed). The `master.d/*.c` files are **config fragments**, not the logic:
  - `master.d/arp.c:50-53` — sizes the ARP table only (`arptab[171]`, `ARPTAB_BSIZ 9 × ARPTAB_NB 19`).
  - `master.d/ip.c:51-63` — `ip_pcb[8]`, `provider[16]`, **`ipforwarding=1`**, `ipsendredirects=1`.
  - `master.d/rt.c` — is the **RT scheduling class** dispatch table (real-time priorities), **not** IP routing — a name collision; ignore for networking.
  - The `*FLAG`/`fs` header lines (`arp.c:24-25`, `ip.c:24-25`) are the master-config table format that registers these as kernel-config modules.
- So **at the kernel level, our driver does not touch ARP/IP/routing code at all** — it only has to be a correct **DLPI Style-1 connectionless Ethernet provider**, and `slink` + the prebuilt `ip`/`arp`/`app` modules do the rest. ARP runs as the `/dev/arp` mux above us; the `app` module (pushed by `aplinkint`) packs/unpacks the ARP↔Ethernet header on the IP side.

### Minimum DLPI contract for ARP + ping (the exact skeleton to mirror, all from `aen/aen.c`)
1. **`streamtab` with read+write qinits** (`aen.c:60-78`):
   - `module_info` id `0x656E` ("en"), name, min/max/hiwat/lowat (`aen_minfo`, `aen.c:60-63`).
   - read qinit: `open=aenopen, close=aenclose` (`aen.c:65-68`).
   - write qinit: `put=aenwput, srv=aenwsrv` (`aen.c:70-73`).
   - `struct streamtab aeninfo = { &aenrinit, &aenwinit, NULL, NULL };` (`aen.c:75-78`) — **this is the `&aeninfo` placed in the cdevsw `d_str` slot.**
2. **Write-side `put` dispatch** (`aenwput`, `aen.c:243-284`): switch on `mp->b_datap->db_type` →
   - `M_PROTO`/`M_PCPROTO` → `aenproto()` (DLPI primitives)
   - `M_IOCTL`/`M_IOCDATA` → `aenioctl()`
   - `M_FLUSH` → standard flush handling.
3. **DLPI primitives — the irreducible three** (`aenproto`, `aen.c:777-923`):
   - **`DL_INFO_REQ` → `DL_INFO_ACK`** (`aen.c:838-879`): must report `dl_mac_type = DL_ETHER`, `dl_service_mode = DL_CLDLS` (connectionless), `dl_provider_style = DL_STYLE1`, `dl_max_sdu = ETH_MAXDATA (1500)`, `dl_min_sdu = 1`, `dl_addr_length = 6`, and the 6-byte MAC at `dl_addr_offset`.
   - **`DL_BIND_REQ` → `DL_BIND_ACK`** (`aen.c:796-836`): store the requested SAP (`aen->sap = reqp->dl_sap`), set `state = DL_IDLE`, echo SAP + the physical address back. **The SAP is the EtherType** the stream wants (ARP binds `0x0806`, IP binds `0x0800`).
   - **`DL_UNITDATA_REQ`** (TX, `aen.c:791-794` → `aenxmit` `aen.c:293-347`): build the Ethernet frame — **dest MAC** copied from `mp` at `dl_dest_addr_offset` (`aen.c:325-326`), **src MAC** = board's own address (`aen.c:332-334`), **EtherType = the bound `aen->sap`** (`aen.c:341`), then the payload; hand to the chip.
   - **default** → log "unknown proto request" (`aen.c:923`).
4. **RX → `DL_UNITDATA_IND` (the reverse path + the SAP demux)** (`aen.c:1184-1342`):
   - `sap = packet_header->ether_type` (`aen.c:1184`) — **the received EtherType is the SAP key**.
   - trailer handling `aen.c:1187-1208` (skip when peer used `-trailers`).
   - **demux: deliver only to the stream whose bound SAP matches** — `else if (aen->sap == sap)` (`aen.c:1270`) → build a `DL_UNITDATA_IND` (`aen.c:1296-1342`) carrying `dl_dest_addr`/`dl_src_addr` (6 bytes each) + the M_DATA payload, `putnext` up the stream. **This is what makes ARP replies reach the ARP stream and IP datagrams reach the IP stream** off the one physical NIC.
   - **broadcast** must be accepted: `broadcast_address[6]` defined `aen.c:1179`; promiscuous-mode dest check compares against it (`aen.c:1284`). For ARP to work, frames to `ff:ff:ff:ff:ff:ff` (ARP requests/replies, EtherType `0x0806` — note the literal at `aen.c:378`) must be passed up the SAP-matched stream.
5. **IF-level ioctls** the driver must ACK (`aenioctl`, `aen.c:748-752`): `SIOCSIFFLAGS`/`SIOCGIFFLAGS` → `M_IOCACK`. **Address/netmask are NOT set on the driver** — `ifconfig`'s `SIOCSIFADDR` is consumed by the IP layer above; the driver only needs flags + `DL_INFO/BIND/UNITDATA` + a working RX indication. `IF_UNITSEL` is NAK'd ("someday", `aen.c:754`).
6. **Interrupt:** add the ISR to **`int2_tbl[]`** (`kernel.c:188-195`, e.g. `aenintr`/`hydraintr`) — RX completion drains frames → step 4; TX completion dequeues next. (Our Z3660 path is mailbox/`AMIGA_INTERRUPT_ETH`-driven instead of a LANCE ring, but the upstream half — building `DL_UNITDATA_IND` and `putnext` — is identical.)
7. **Boot probe (optional):** hydra adds `hydrainit` to **`init_tbl[]`** (`kernel.c:38,44-62`) to *probe* at boot, deferring heavy init to open. `aen` has **no init_tbl entry** — it probes lazily in `aenopen`. Either is valid.

> **`-trailers` gotcha to carry over (from grimoire `writing-a-streams-driver.md:96`):** the DP8390 "min frame 60 not 64 (CRC included)" bug silently dropped ARP replies on hydra. Our Z3660 firmware delivers frames over the mailbox, not a DP8390 ring, so that exact off-by-4 won't apply — but **get the RX length/CRC boundary right or small ARP frames vanish and ARP never completes.**

---

## 5. Concrete bring-up runbook for OUR driver on real HW @ `192.168.2.39`

Assume our cdevsw major = **`M`** (pick a free slot — `master.d/kernel.c` shows **48–69 are all `nostr` free**; slot 18=aen, 47=hydra are taken; **e.g. use 48**) and interface name **`z3660net0`** (or shorter `zen0` — keep ≤ the minor/name conventions; examples below use `zen0`).

### One-shot manual bring-up (single-user / testing)
```sh
# 0. transports + loopback already linked by the boot 'slink' (strcf boot{}); if not:
/usr/sbin/slink                                   # runs strcf boot{} : tp tcp/udp/icmp/rawip + loopback
/usr/sbin/ifconfig lo0 127.0.0.1 up

# 1. device node (major = our cdevsw slot, minor 0 = board0/clone-SAP)
mknod /dev/zen0 c 48 0

# 2. presence gate is OPTIONAL on real HW (our analogue of `aen -S`); if we ship one, gate on it.
#    Otherwise just attempt the link:

# 3. link the DLPI stream under IP + ARP (reuses the stock addaen recipe verbatim)
/usr/sbin/slink addaen /dev/zen0 zen0

# 4. assign the LITERAL static IP, bring up, disable trailers
/usr/sbin/ifconfig zen0 192.168.2.39 netmask 255.255.255.0 up -trailers

# 5. default route (metric 1 is MANDATORY, not a flag)
/usr/sbin/route add default 192.168.2.1 1

# 6. verify
/usr/sbin/ifconfig zen0
ping 192.168.2.1
```

### Persistent `/etc/inet` config to add (survives reboot)
1. **`/etc/inet/network-config`** — replace/append the aen block with ours (LITERAL IP, never `uname -n`):
   ```sh
   # Z3660 onboard ethernet
   /usr/amiga/bin/zen -S &&                          # presence gate (if we ship one); else drop this line + leading &&
       /usr/sbin/slink addaen /dev/zen0 zen0 &&
           /usr/sbin/ifconfig zen0 192.168.2.39 netmask 255.255.255.0 up -trailers
   ```
   (If no presence-gate binary exists, make the first command `/usr/sbin/slink addaen …`.)
2. **`/etc/inet/hosts`** — add our address/name:
   ```
   192.168.2.39 amix-z3660 amix-z3660.alanara.fi
   ```
3. **`/etc/inet/rc.inet`** — default route is already `route add default 192.168.2.1 1` (`rc.inet:42`); keep it. Add any extra static routes here.
4. **No new `strcf` function needed** — `addaen` is generic (links under both `/dev/ip` and `/dev/arp`).
5. **Device node persistence:** `mknod /dev/zen0 c 48 0` must exist on the **root FS** (`/dev` is on-disk, not devfs) — create it once, it persists. If staging a fresh root, add it to whatever MAKEDEV/install step the kerntools deploy uses.
6. **DNS:** leave OFF (stock) unless needed; if enabled, also empty `/etc/domain` and keep the literal-IP boot config or every boot adds ~180 s (`networking.md:91-136`).

### Kernel-side edits these depend on (Survey-E territory, restated for completeness)
In `master.d/kernel.c`: (a) `extern struct streamtab z3660info;` near `aeninfo`/`hydrainfo` (`kernel.c:265-266`); (b) put `&z3660info` in cdevsw **slot 48** (`kernel.c:398`, currently `nostr /*48*/`); (c) add `z3660intr` to `int2_tbl[]` (`kernel.c:188-195`) — *externs must precede the array*; (d) optionally `z3660init` in `init_tbl[]` (`kernel.c:44-62`). Then `mknod` + `slink` + `ifconfig` as above.

---

## Files cited
- `/mnt/etc/init.d/inetinit` (== `/mnt/etc/rc2.d/S69inet`, hardlink) — boot chain, lines 9,11-12,14,17,21
- `/mnt/etc/inet/network-config:1-12` — the live aen0 bring-up (literal IP `192.168.2.38`) + DNS-stall comment
- `/mnt/etc/inet/network-config.orig:3-5` — stock `\`uname -n\`` form (the slow one)
- `/mnt/etc/inet/strcf:48-129` — `tp`, `linkint`, `aplinkint`, `loopback`, **`addaen` (87-94)**, `addslip`, `boot`
- `/mnt/etc/inet/rc.inet:42` — `route add default 192.168.2.1 1`
- `/mnt/etc/inet/hosts:1-5`, `/mnt/etc/inet/networks`, `/mnt/etc/inet/routes`
- `/mnt/dev/{aen0,aen1,ip,arp,loop,tcp,udp,icmp,rawip,sld0-3}` — stock net nodes
- `/mnt/usr/sbin/slink` (0700, unreadable), `/mnt/usr/sbin/ifconfig`, `/mnt/usr/sbin/route`, `/mnt/usr/amiga/bin/aen` (presence-gate tool; `usage:` + `/dev/aen0` strings)
- `~/Devel/Omat/Amiga/Amix/hydra-amix/usr/sys/master.d/kernel.c` — cdevsw 18=aen(357)/47=hydra(397), `int2_tbl`(188-195), `init_tbl`(44-62), externs(265-266)
- `.../hydra-amix/usr/sys/amiga/driver/aen/aen.c` — **streamtab/qinit(60-78)**, **aenwput dispatch(243-284)**, **aenproto DLPI: DL_BIND(796-836)/DL_INFO(838-879)/DL_UNITDATA_REQ→aenxmit(791-794,293-347)**, **RX SAP-demux + DL_UNITDATA_IND(1184-1342)**, broadcast(1179,1284), trailers(1187-1208), IF ioctls(748-754), minor encoding(88-160)
- `.../aen/aen.h:4-5,41,47-54` (AEN_MAXBOARDS/MAXDEV, ETH_* sizes), `.../aen/aenuser.h:4-9` (AEN_NUMBER_OF_BOARDS ioctl used by `aen -S`)
- `.../master.d/{arp.c:50-53, ip.c:51-63}` — ARP-table + IP-PCB sizing config (IP/ARP *logic* is binary-only in `netinet/exp`; `net/` absent)
- grimoire `docs/how-it-works/networking.md` (esp. 174,187-193 slink/no-plumb, 91-136 DNS/literal-IP, 56-63 route metric) and `docs/drivers/writing-a-streams-driver.md` (9,75-114,131,143 cdevsw/DLPI/slink/mknod, 96 the RX-min-frame ARP bug)

**Not found / source-unavailable (stated explicitly):** no `net/` dir and no `netinet/*.c` source in the hydra tree (only `netinet/exp` binary) — IP/ARP/route forwarding logic is not readable here; `dlpi.h`/`net/if.h`/`if_ether.h` are not in this clone tree (they come from the licensed Amix sysroot at build time); no MAKEDEV/mknod script for net nodes under `/mnt/etc` (nodes are pre-created in the on-disk `/dev`); `/tmp/amixgold` itself is I/O-locked (used `/mnt`, the same golden image, instead).
