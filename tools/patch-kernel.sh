#!/bin/sh
# On-box integration of z3660eth into master.d/kernel.c (run on the WinUAE box).
# Idempotent.  Adds the streamtab extern + claims cdevsw slot 48.  Uses ed
# (portable on SVR4) since FTP pull-back of the 14KB kernel.c is unreliable.
K=/usr/sys/master.d/kernel.c
if grep z3660eth $K >/dev/null 2>&1; then echo "KERNEL.C ALREADY PATCHED"; grep -n z3660eth $K; exit 0; fi
cp $K $K.bnkorig
chmod 644 $K
ed - $K <<'EOF'
/extern struct streamtab aeninfo;/a
extern struct streamtab z3660ethinfo;
.
/\/\*48\*\//c
ND,ND,ND,ND,ND,ND,ND,ND,ND,ND,notty,&z3660ethinfo,nullflag,		/*48=z3660eth*/
.
w
q
EOF
echo "KERNEL.C PATCHED:"
grep -n z3660eth $K
