> **Moved.** This project now lives in [amigaux/amix](https://github.com/amigaux/amix) as
> [`kernel/drivers/z3660eth`](https://github.com/amigaux/amix/tree/master/kernel/drivers/z3660eth), with its
> history and later work (see `kernel/drivers/z3660eth/docs/history-map.txt` there for the old-to-new commit
> map). This repository is archived and no longer updated.

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

`z3660eth` registers at **cdevsw major 51** (renumbered from 48 on 2026-07-30 —
48 turned out to be contested; see `BUILD.md` §2(b)), and its device node is
`/dev/zen0` = `mknod /dev/zen0 c 51 0`. Those are the two integration facts
another driver author or a packager needs first.

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
poll/drain callout instead (commit `028c8d2`). No firmware change was needed, and
no cache maintenance was required either (the 030 data cache is transparent to the
eth DDR windows here). The `Z3660ETH_CACHE_FLUSH` `#ifdef` that once held that
maintenance is gone: 040/060 maintenance and an attach-time cache-inhibit assert
are now compiled in always and gated at run time by `z3660eth_cache`, whose
default 0 keeps every 040/060 opcode unreachable on the shipping images — see
`docs/BLIZZARD-F3.md`.

## Bringing it up on a real box → [`docs/ZEN0-BRINGUP.md`](docs/ZEN0-BRINGUP.md)

**★ Under the Amix-interop Z3660 firmware fork, Amix's stock `aen` ethernet driver
can never work — `zen0` is the only path.** `aen` hunts Commodore/A2065 board id
`0x02020070`; the firmware presents every emulated autoconfig board as manufacturer
`0x144B` and has A2065 emulation commented out of its embedded UAE core. A
manufacturer id is not settable by any `autoconfig_*` key, so no configuration
reaches this. Corollary worth the several hours it cost: *"that box networks on
`aen0`"* is a fact about **that box's firmware build** and never transfers across a
firmware boundary.

[`docs/ZEN0-BRINGUP.md`](docs/ZEN0-BRINGUP.md) is the field-proven runbook — the
evidence for the above, the five-command manual bring-up (verified first try on real
68LC060 metal, 2026-09-09), the `/etc/rc2.d/S99zen` persistence script with its exact
shipped body, the three identity files (and why `setuname -n` is *not* one of them),
and a triage section for telling which path a box is on. Auto-bring-up at boot =
`userland/S99zen` (→ `/etc/rc2.d/S99zen`); it runs a bare `slink` first and retries
`slink addaen`, because the bring-up can otherwise race the inet base streams on a
fast clean boot.

## Layout
- `src/z3660eth.c` — the driver (STREAMS/DLPI skeleton adapted from the stock
  `aen`/`hydra` drivers; chip layer replaced by the Z3660 mailbox).
- `src/z3660eth.h` — register map + frame‑window layout + kernel structs (every
  offset verified against `z3660-drivers/common/z3660_regs.h` and the firmware
  `Z3660/src/{ethernet.c,memorymap.h,rtg/rtg.c}`).
- `src/z3660ethuser.h` — user‑facing ioctls + status struct (no kernel types).
- `src/Makefile` — installed as `/usr/sys/amiga/driver/z3660eth/Makefile`.
- `driver.conf` — the `net` stanza consumed by `amix-build`.
- `userland/` — `zen.c` (the `zen -S` presence tool), the `/etc/inet` config
  snippet, and the manual interface bring‑up runbook.
- `tools/` — the two fail‑closed on‑box patch scripts (`patch-kernel.sh` claims
  the cdevsw row, `patch-makefile.sh` the driver `OBJ` list) plus
  `patch-selftest.sh`, their benchless host‑side fixture battery.
- `BUILD.md` — the exact on‑box build + kernel‑integration steps (the manual
  procedure that `amix-kerntools` automates).
- `docs/ZEN0-BRINGUP.md` — the field-proven bring-up runbook: why `aen` can never
  work under this firmware, the five-command sequence, `S99zen` persistence, the
  identity files, and how to tell which path a box is on.
- `docs/BLIZZARD-F3.md` — the cache-coherence pre-registration and its round-2
  result: why the 030 needs no maintenance and how the 040/060 gate works.
- `CHANGELOG.md` — what changed, when, and against which commit.

## How it works (one paragraph)
Discovery mirrors `z3660.c`: `autocon(0x144B0001)`, else (AGA only) the fixed combo
base `0x10000000`, verified via the firmware MAC register. When the whole window
lies below `VSECT1` (`0x40000000`) the physical base is used as the kernel VA
directly — AMIX's section‑0 early‑termination identity map, no `sptalloc`; only a
board at or above `VSECT1` takes the `sptalloc` arm, which the 040/060 cache work
declares unsupported with a live data cache. The board's eth
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

### Trademarks
The wording Amiga Corporation asked us to carry:

This project and its developers are not affiliated with, endorsed by, or sponsored by
Amiga Corporation. Amiga is a registered trademark of Amiga Corporation.

AmigaOS and Amiga UNIX are trademarks of their respective owners, and UNIX is a
registered trademark of The Open Group. See [`NOTICE`](NOTICE).
