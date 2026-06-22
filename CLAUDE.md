# CLAUDE.md — amix-z3660net

The native Amix STREAMS/DLPI ethernet driver **`z3660eth`** (interface `zen0`, cdevsw major 48) for the
Z3660's onboard ethernet. The in-repo `GRIMOIRE-HANDOFF.md` (reviewed), `BUILD.md`, and
`docs/ETHERNET-SCOPING.md` are authoritative.

**Status:** ✅ works on real HW (2026-06-21) — full bidirectional TCP/IP over `zen0` @ `192.168.2.39`.

## Knowledge base (Obsidian vault)

Via the **obsidian-vault MCP server**, read at session start:
- `CLAUDE.md` (vault root) + `Machine/Coding Standards.md`
- `Machine/Personal/Amix/Overview.md`, `Contracts.md`, `Dev-loop-and-build.md`
- `Machine/Personal/Amix/z3660net.md` (this repo) + `Z3660-firmware.md` (the protocol owner)

## Load-bearing essentials

- **INT6 interrupt storm** was the only real-HW blocker: firmware raises Amiga INT6 per RX frame, Amix
  has no level-6 ethernet handler → hard-lock. **Fix (commit `b06cf45`): the driver writes
  `ZZ_CONFIG_DISABLE` (0)** so INT6 never fires; RX is a bounded **polled drain** callout. Cache-flush
  (`Z3660ETH_CACHE_FLUSH`) **not needed**.
- **Amix has no `spl6()`/`splx()`** — no-op them (stock `aen` does zero interrupt masking). A STREAMS
  driver must not use rico.h `uchar`/`ulong` types — spell out `unsigned char/long`.
- **`nm -u` clean-gate** is the bar (the lesson that unblocked the first boot — `checkunix` alone is
  insufficient, `sum -r` doesn't converge). **Work on a COPY**; `zen0` comes up only after the full
  boot. Full detail in the vault Dev-loop note.
- Auto-bring-up: `/etc/rc2.d/S99zen` (hardened `slink` retry loop). Bring-up is `slink addaen` +
  `ifconfig zen0 <literal-ip> up -trailers` (NO `plumb`; SVR4.0).

## Edges
`driver.conf` → kerntools `build-net-kernel.sh` (STREAMS path, `cdevsw[48]`, not `sd.c`). Follows the
Z3660 firmware ethernet protocol one-way. `GRIMOIRE-HANDOFF.md` → grimoire via **`/amix-brief`**.

## Grimoire access

You may read `../grimoire-amix/` freely — especially `llms-full.txt` (the whole published corpus) —
as your reference knowledge base; consult it before documenting Amix internals. You may **write** to
grimoire **only** via `/amix-brief`, which drops a confidence-tagged brief into the gitignored
`import/` (no commit). Run `/amix-brief` with no arguments to have me scan this repo for findings
grimoire doesn't yet have and propose what to export. (Enforced by `.claude/settings.json`.)

## Commit discipline
`Machine/Coding Standards.md`: **no AI attribution**. Identity `Jusii <jussi@alanara.fi>`.
