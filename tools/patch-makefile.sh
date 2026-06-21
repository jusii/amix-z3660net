#!/bin/sh
# On-box integration of z3660eth into amiga/driver/Makefile (run on the WinUAE box).
# Idempotent.  Inserts "z3660eth/exp \" before the aen/exp OBJ entry (which is the
# LAST entry with no trailing backslash) + appends the build rule.  Uses ed.
M=/usr/sys/amiga/driver/Makefile
if grep z3660eth $M >/dev/null 2>&1; then echo "MAKEFILE ALREADY PATCHED"; grep -n z3660eth $M; exit 0; fi
cp $M $M.bnkorig
chmod 644 $M
ed - $M <<'EOF'
/aen\/exp #/i
	  z3660eth/exp \
.
$a

z3660eth/exp :
	cd z3660eth; $(MAKE)
.
w
q
EOF
echo "MAKEFILE PATCHED:"
grep -n z3660eth $M
