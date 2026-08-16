# ps2facts - a plain ps2sdk EE binary, no dependencies beyond the SDK.
#
# Two outputs, wanted for different things:
#
#   ps2facts.elf        with symbols, for nm/addr2line when something faults
#   ps2facts-small.elf  stripped, for a memory card or a release
#
# The difference is not marginal -- 1374096 bytes against 184596, the same
# program either way. A memory card is 8MB and shared with saves, so the
# stripped one is what belongs on it.

EE_BIN = ps2facts.elf
EE_BIN_SMALL = ps2facts-small.elf
EE_OBJS = ps2facts.o
EE_LIBS = -ldebug -lcdvd -lpad -lc

all: $(EE_BIN) $(EE_BIN_SMALL)

# Stripped from the linked ELF rather than linked separately, so the two cannot
# drift apart: whatever ps2facts.elf does, this does.
$(EE_BIN_SMALL): $(EE_BIN)
	$(EE_STRIP) -o $@ $<

clean:
	rm -f *.elf *.o *.a

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
