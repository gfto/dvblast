# DVBlast Makefile
# Customise the path of your kernel

CFLAGS += -Wall -O3 -fomit-frame-pointer
CFLAGS += -g
CFLAGS += -I/usr/src/kernel/linux-2.6.29.1/include
LDFLAGS_DVBLAST += -ldvbpsi -lpthread

OBJ_DVBLAST = dvblast.o util.o dvb.o demux.o output.o en50221.o comm.o
OBJ_DVBLASTCTL = util.o dvblastctl.o

all: dvblast dvblastctl

$(OBJ_DVBLAST) $(OBJ_DVBLASTCTL): Makefile dvblast.h en50221.h comm.h

dvblast: $(OBJ_DVBLAST)
	$(CC) -o $@ $(OBJ_DVBLAST) $(LDFLAGS_DVBLAST)

dvblastctl: $(OBJ_DVBLASTCTL)

clean:
	-rm -f dvblast dvblastctl $(OBJ_DVBLAST) $(OBJ_DVBLASTCTL)
