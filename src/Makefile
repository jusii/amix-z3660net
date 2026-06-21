#
# Makefile for the Z3660 native AMIX ethernet driver (z3660eth).
# Installed on the box as /usr/sys/amiga/driver/z3660eth/Makefile; mirrors the
# stock aen/ and hydra/ driver Makefiles (same CFLAGS + include paths + the
# `ld -r -o exp` convention the parent amiga/driver/Makefile links against).
#
CFLAGS	= -O -D_KERNEL -DSVR40 -DSVR4 -I../../.. -I../../inc

OBJ	= z3660eth.o

exp:	$(OBJ)
	ld -r -o exp $(OBJ)

clean:
	-rm -f $(OBJ) exp

z3660eth.o : z3660eth.h z3660ethuser.h
