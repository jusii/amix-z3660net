# Building & integrating z3660eth into the AMIX kernel

The driver is a **STREAMS/DLPI network driver**, so it does **not** use the SCSI
`build-kernel.sh` (which generates `scsicard[]` rows into `sd.c`). It needs three
kernel-integration edits + its own driver subtree. `../amix-kerntools/build-net-kernel.sh`
automates everything below; this file is the authoritative manual procedure and the
fallback if the automation hits anything unexpected on the live tree.

The build/clean-gate runs **on a networked AMIX box** (the WinUAE/Amiberry build box
at `192.168.2.38`). The real A4000+Z3660 has no network — the clean `relocunix` is
shuttled to it via `transfer.hdf` (see the locked two-tier routine in the project notes).

## 0. Prereqs
- Build box up: `amiberry --config ../amix-kerntools/configs/amix-golden.uae` + the
  grimoire `amix-lan-up.sh` bridge. Verify: `python3 ../amix-kerntools/tools/amixsync.py ls /usr/sys`.
- Tools: `../amix-kerntools/tools/amixsync.py` (FTP push/pull), `../grimoire-amix/tools/host-net/amixsh.py` (run commands).

## 1. Push the driver subtree
Create `/usr/sys/amiga/driver/z3660eth/` and push `src/z3660eth.c`, `src/z3660eth.h`,
`src/z3660ethuser.h`, `src/Makefile` into it.
```
amixsh.py "mkdir /usr/sys/amiga/driver/z3660eth"
amixsync.py push src/z3660eth.c       /usr/sys/amiga/driver/z3660eth/z3660eth.c
amixsync.py push src/z3660eth.h       /usr/sys/amiga/driver/z3660eth/z3660eth.h
amixsync.py push src/z3660ethuser.h   /usr/sys/amiga/driver/z3660eth/z3660ethuser.h
amixsync.py push src/Makefile         /usr/sys/amiga/driver/z3660eth/Makefile
```

## 2. Edit `/usr/sys/master.d/kernel.c`  (pull, edit, push — keep a backup)
**(a)** Add the streamtab extern, right after the stock `aen` one:
```
extern struct streamtab aeninfo;
extern struct streamtab z3660ethinfo;     /* <-- add */
```
**(b)** Claim a free `cdevsw[]` slot. Slot **48** is `nostr` (free) on the stock /
a4091 kernel — VERIFY it is still the empty `/*48*/` row, then replace it:
```
/* was: */ ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,nostr,nullflag,         /*48*/
/* now: */ ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&z3660ethinfo,nullflag,  /*48=z3660eth*/
```
> No `int2_tbl[]` / `init_tbl[]` edit. The driver uses **lazy-open** (autoconfig on
> first `open`, like `aen`) and **polled RX** (a `timeout()` callout, since AMIX has no
> level-6 hook for the firmware's INT6). This is what makes the GEM-less build box boot
> cleanly and `open()` simply return ENXIO. (Interrupt-driven RX — adding `z3660ethintr`
> to `int2_tbl` — is a later step gated on a board-mod; see scoping §10.2.)

## 3. Edit `/usr/sys/amiga/driver/Makefile`
Add `z3660eth/exp` to the `OBJ` list (after the stock `aen/exp`) and add its build rule:
```
OBJ = \
        ... \
        aen/exp \
        z3660eth/exp \          # <-- add (before/after hydra/exp if present)
        ...

z3660eth/exp :                  # <-- add this rule
        cd z3660eth; $(MAKE)
```

## 4. Clean-gate relink (the D245 boot-breaker workaround)
On-box `ld` corrupts ~70% of kernel links under emulation, so relink until the kernel is
provably clean. For this (larger) net kernel the bar is **`nm -h -u unix` empty +
`checkunix`-clean** — `sum -r` recurrence does *not* converge at this size (the committed
`build-clean-net-kernel.sh` predates this; prefer the `nm -u` bar — see `GRIMOIRE-HANDOFF.md` §8).
The net-aware gate removes the right stale objects (the SCSI gate is hardcoded to
`alien/` and will NOT rebuild a `driver/` subdir or `kernel.c`):
```
amixsync.py push ../amix-kerntools/tools/build-clean-net-kernel.sh /tmp/build-clean-net-kernel.sh
amixsh.py "nohup sh /tmp/build-clean-net-kernel.sh z3660eth > /tmp/bcnk.log 2>&1 &"
# poll /tmp/bcnk.log for 'STABLE after N builds' (clean) or 'FAILED'
```
The gate force-removes `amiga/driver/z3660eth/{z3660eth.o,exp}`, `amiga/driver/exp`,
`master.d/kernel.o`, `master.d/exp` each round (Makefiles have no header-dep tracking),
recompiles, links, and confirms a clean `relocunix` via **`nm -h -u unix` (empty) + `checkunix`**.

## 5. Install + boot
```
amixsh.py "cd /usr/sys; make install"   # stages relocunix -> /stand, keeps OLDunix
amixsh.py "cd /stand; make"              # writes the boot partition (c6d0s3)
amixsh.py "shutdown -i6 -g0 -y"          # reboot
```

## 6. Phase-1 gate (build box) — expected result
The box boots normally; `z3660eth` is registered at cdevsw major 48 but finds no
Z3660 GEM (the build box emulates an a2065). `mknod /dev/zen0 c 48 0; cat </dev/zen0`
(or `slink addaen /dev/zen0 zen0`) should yield **ENXIO**, and the kernel must NOT
panic. That proves the whole integration + build path independent of the datapath.

## 7. Real HW (Phases 2–4) — shuttle, don't re-image
Shuttle the clean `relocunix` to the real box via `transfer.hdf` (`c5d0`), stage it,
`cd /stand; make`, reboot. Then bring the interface up at **192.168.2.39** per
`userland/bringup.sh` and test TX → RX → ARP+ping. Never re-image the root.

## Real-world build notes (validated 2026-06-21 on the WinUAE build box)
The box is **WinUAE on Xvfb :101**, config `AmigaUnix-z3660.uae` (a2065=tap:tap0, guest **192.168.2.38**), root disk
`Amix-z3660-work.hdf` = the build tree. Launch: `sudo /usr/local/sbin/amix-lan-up.sh` + `DISPLAY=:101 winuae -config=…`.
Set `AMIX_PASS` to the build-image root password → `export AMIX_HOST=192.168.2.38 AMIX_USER=root AMIX_PASS=<pw>`.

- **★ FTP asymmetry:** `amixsync.py` **push (PUT) works for 30 KB+**, but **pull (GET) is flaky >~1 KB** (the a2065
  drops large GETs); `amixsh.py` (telnet) only returns small command output reliably. So: push the driver fine, but
  patch `kernel.c` **on the box with `ed`** (`tools/patch-kernel.sh`, no pull) and patch the small `driver/Makefile`
  locally + push. **NFS is the reliable channel** (host `/Amix` = `nas:/Amix`, nas=192.168.2.4; the guest mounts it
  with `mount -F nfs 192.168.2.4:/Amix /mnt`, **nfsvers=2**) — use it to pull the clean `relocunix` and large files.
- **★ rico.h-isms:** a STREAMS driver must spell out `unsigned char/long/short` — **NOT** `uchar`/`ulong`/`ushort`
  (those are `rico.h`-only, which the SCSI driver includes but this one shouldn't). Don't re-declare `untimeout`
  (it's in `systm.h`). Always **single-compile** (`cd …/z3660eth; make`) to catch compile errors before the slow gate.
- **★ clean-gate reality:** on this (larger) kernel the D245 `ld` corruption rate is high, so relink until clean. The
  correct accept bar is **`nm -h -u unix` empty (no undefined symbols) AND `checkunix`-clean** — `sum -r` recurrence does
  *not* converge at this kernel size and is **not** the gate here (it still works for the small SCSI kernels). `checkunix`
  alone is insufficient (it only validates `.symtab`; a code/data shift passes it). The committed
  `tools/build-clean-net-kernel.sh` implements the `nm -u` bar; run it at a high cap.
