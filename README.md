# amix-z3660net — native AMIX ethernet driver for the Z3660

A native **Commodore Amiga Unix (AMIX, SVR4.0, m68k 68030) STREAMS/DLPI ethernet
driver** for the **Z3660 accelerator's onboard ethernet** — the network analogue
of the [`amix-z3660`](../amix-z3660) SCSI driver. It is **not** NIC emulation: the
Z3660 firmware already moves whole ethernet frames between the 68k guest and the
Zynq GEM (for AmigaOS's SANA-II `Z3660Net.device`); this driver speaks that same
firmware mailbox and presents it to AMIX as a DLPI Style‑1 connectionless
ethernet provider bound up by `slink` + `ifconfig`.

The full design + the source‑grounded scoping (protocol contract, cache‑coherency
analysis, INT6 finding, phased plan) lives in
[`../amix-z3660/ETHERNET-SCOPING.md`](../amix-z3660/ETHERNET-SCOPING.md).

## Layout
- `src/z3660eth.c` — the driver (STREAMS/DLPI skeleton adapted from the stock
  `aen`/`hydra` drivers; chip layer replaced by the Z3660 mailbox).
- `src/z3660eth.h` — register map + frame‑window layout + kernel structs (every
  offset verified against `z3660-drivers/common/z3660_regs.h` and the firmware
  `Z3660/src/{ethernet.c,memorymap.h,rtg/rtg.c}`).
- `src/z3660ethuser.h` — user‑facing ioctls + status struct (no kernel types).
- `src/Makefile` — installed as `/usr/sys/amiga/driver/z3660eth/Makefile`.
- `driver.conf` — the `net` stanza consumed by `../amix-kerntools/build-net-kernel.sh`.
- `userland/` — `zen.c` (the `zen -S` presence tool), the `/etc/inet` config
  snippet, and the manual interface bring‑up runbook.
- `BUILD.md` — the exact on‑box build + kernel‑integration steps.

## How it works (one paragraph)
Discovery mirrors `z3660.c`: `autocon(0x144B0001)`, else (AGA only) the fixed combo
base `0x10000000`, verified via the firmware MAC register. The board's eth
registers sit in the RTG register page (`board+0x104..0x1A8`); TX frames stage at
`board+0x07EE0000`, the RX backlog ring at `board+0x07ED0000` (32×2048, 4‑byte
`[len][serial]` header). **TX** copies the frame to the TX window and writes the
length to `ZZ_ETH_TX` (synchronous, ~1 ms, no completion IRQ). **RX** is drained
by re‑reading `ZZ_ETH_RX_ADDR`, breaking on the serial sentinel, and strobing
`ZZ_ETH_RX` per slot. The firmware raises RX as Amiga **INT6/EXTER**, for which
AMIX has no driver hook — so RX is drained from a **clock‑level `timeout()` poll
callout** by default (`z3660ethintr()` exists for a future interrupt path).

## Status
- **Phases 0–1 authored** (this repo): source written + reviewed; build path
  (`../amix-kerntools/build-net-kernel.sh`) prepared. The **compile/boot/ENXIO
  gate on the WinUAE build box and the real‑HW datapath (TX, RX, ARP+ping) are
  NOT yet run** — they require the build box up and the physical A4000+Z3660.
- Real‑HW interface IP: **192.168.2.39** (the WinUAE build box keeps `.38`).
- Two things to confirm on real HW (see scoping §10): the RX length/FCS boundary
  (Q4 — a one‑frame probe) and whether the 030 data‑cache flush
  (`Z3660ETH_CACHE_FLUSH`, off by default) is needed.
