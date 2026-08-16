# ps2facts - standalone hardware report, run from wLaunchELF.
#
# Built like tools/hello: a plain ps2sdk EE binary with no dependency on the
# loader, so it can be run on a console kernelreloaded will not boot on.

EE_BIN = ps2facts.elf
EE_OBJS = ps2facts.o
EE_LIBS = -ldebug -lcdvd -lpad -lc

all: $(EE_BIN)

clean:
	rm -f *.elf *.o *.a

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
