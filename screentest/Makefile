# screentest - measure the debug screen's usable text area.
#
# A subdirectory rather than a second target of the parent Makefile, because
# ps2sdk's Makefile.eeglobal is built around one EE_BIN per directory and
# forcing two through it costs more than a directory does.
#
# Plain ps2sdk, no loader dependency, so it runs on any console over ps2link
# or from wLaunchELF.

EE_BIN = screentest.elf
EE_OBJS = screentest.o
EE_LIBS = -ldebug -lc

all: $(EE_BIN)

clean:
	rm -f *.elf *.o *.a

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
