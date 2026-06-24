# amix-z3660net — native AMIX ethernet driver for the Z3660

A native **Commodore Amiga Unix (AMIX, SVR4.0, m68k 68030) STREAMS/DLPI ethernet
driver** for the **Z3660 accelerator's onboard ethernet** — the network analogue
of the [`amix-z3660scsi`](https://github.com/jusii/amix-z3660scsi) SCSI driver. It is **not** NIC emulation: the
Z3660 firmware already moves whole ethernet frames between the 68k guest and the
Zynq GEM (for AmigaOS's SANA-II `Z3660Net.device`); this driver speaks that same
firmware mailbox and presents it to AMIX as a DLPI Style‑1 connectionless
ethernet provider bound up by `slink` + `ifconfig`.

The full design + the source‑grounded scoping (protocol contract, cache‑coherency
analysis, INT6 finding, phased plan) lives in
[`docs/ETHERNET-SCOPING.md`](docs/ETHERNET-SCOPING.md), with the supporting
evidence dossier in [`docs/ETHERNET-SCOPING-evidence.md`](docs/ETHERNET-SCOPING-evidence.md).

## Scope / responsibility

This repo is **the AMIX ethernet driver (`z3660eth`) for the Z3660, plus its design
docs, and nothing else.** Its siblings: SCSI driver → [`amix-z3660scsi`](https://github.com/jusii/amix-z3660scsi);
firmware / 68k-emulator → [`Z3660-amix`](https://github.com/jusii/Z3660-amix); build harness + golden image +
host-ops → [`amix-kerntools`](https://github.com/jusii/amix-kerntools).

Compilation and kernel integration are handled by the **`amix-kerntools`** build hub,
which reads this repo's `driver.conf` by relative path. This repo carries only the
driver source, its `driver.conf`, the userland bring-up, and the design docs.

## Status — ✅ WORKS ON REAL HARDWARE (2026-06-21)

AMIX has full bidirectional TCP/IP over `zen0` on a real **A4000 + Z3660**:
`zen0` UP @ `192.168.2.39`; laptop→box flood ping **40/40, 0% loss**; box pings
both the laptop and the gateway; `telnet`/`ftp` over `zen0` work; `netstat -in`
shows **0 input errors, 0 output errors, 0 collisions**; sustained FTP throughput
**~185 KiB/s** (stable across runs). GEM MAC `00:80:51:01:02:03`.

The **only** real-HW blocker was an **INT6 interrupt storm**: the firmware raises
Amiga INT6 on every received frame, but AMIX has no level-6 ethernet handler, so
the LAN's ARP broadcasts storm the box into a hard lock. Fix = the driver writes
`ZZ_CONFIG_DISABLE` so the firmware never raises INT6; RX is serviced by the bounded
poll/drain callout instead (commit `b06cf45`). No firmware change was needed, and
the optional `Z3660ETH_CACHE_FLUSH` was **not** required (the 030 data cache is
transparent to the eth DDR windows here — left OFF).

Auto-bring-up at boot = `userland/S99zen` (→ `/etc/rc2.d/S99zen`). Note: the
bring-up can race the inet base streams on a fast clean boot — S99zen runs a bare
`slink` first and retries `slink addaen` to make it reliable.

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
- `BUILD.md` — the exact on‑box build + kernel‑integration steps (the manual
  procedure that `amix-kerntools` automates).

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

## License
MIT — see [`LICENSE`](LICENSE). The STREAMS/DLPI scaffolding was originally adapted
from Commodore's stock AMIX `aen`/`hydra` drivers; the MIT grant covers this repo's
own work. AMIX itself (SVR4.0) is proprietary — you need a licensed AMIX system and a
Z3660 accelerator to build and run this driver.
