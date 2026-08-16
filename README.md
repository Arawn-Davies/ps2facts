# ps2facts

A PlayStation 2 homebrew ELF that asks the console what it is, prints the
answers on screen and over the serial/ps2link console, and saves them to a
file. It can also dump the BIOS ROM and NVM with checksums.

It exists because working out *why* two PS2s behave differently usually means
inferring hardware facts from symptoms. This asks the hardware directly, so
two consoles can be diffed as text rather than remembered.

`screentest/` is a second, much smaller tool in the same spirit: it measures
how much of the debug screen a television actually shows, rather than leaving
you to count rows off a photograph. See [screentest](#screentest).

## What it reports

```
ps2facts    ROMVER 0160EC20011004 (0x0160 -> phat PS2)
EE PRid/Cfg : 0x00002e30 / 0x00073443    GS CSR : 0x5519400c (rev 0c id 19)
DEV9        : 0x0031 (exp. bay )  MechaCon 2.03  NVM180 0x0000
ROMGSCRT ABSENT  -> sbcall_setdve    ADDDRV present
MC y/y MCSERV y/y SIO2 y/y PAD y/y (plain/X)
selects     : TGE/intrelay-dev9.irx  smaprpc no  ps2dev9 yes
rom0: 90 entries:
 RESET ROMDIR EXTINFO ROMVER SBIN LOGO IOPBTCONF IOPBTCON2 SYSMEM LOADCORE
 ...
```

| Row | What it tells you |
|---|---|
| `ROMVER` | the ROM build string, its version word, and phat/slim by the `> 0x0190` rule |
| `EE PRid/Cfg`, `GS CSR` | EE and GS silicon revisions |
| `DEV9` | the expansion-bay/PCMCIA network adapter: absent, PCMCIA, or expansion bay |
| `MechaCon`, `NVM180` | MechaCon version and a sample NVM word |
| `ROMGSCRT` | present or absent, which decides whether video mode setting goes through the ROM routine or raw DVE register writes |
| module inventory | `MCMAN`/`XMCMAN` style pairs — plain versus the X-prefixed slim-era modules |
| `selects` | which IOP modules a PS2 Linux loader would choose on this console |
| `rom0:` | the full ROMDIR listing |

The `selects` row encodes the module-selection rules of
[kernelreloaded](https://github.com/Arawn-Davies/kernelreloaded) (the revived
PS2 Linux loader). It is the row this tool was written for: it turns "which
interrupt relay did that console pick?" from a guess into a fact. **If those
rules change upstream, this row can drift** — it is a prediction, not a
reading.

## Should you use PS2Ident instead?

For identifying a console, **probably yes**.
[PS2Ident](https://github.com/ps2homebrew/PS2Ident) reads far more than this
does — EE, FPU, IOP, GS, SPU2 and SSBUS revisions, MECHACON firmware and
region, mainboard Model ID / EMCS ID / renewal date, i.Link and EEPROM config —
and it has a community database of mainboard models behind it. It dumps both
ROM chips and the MECHACON NVRAM too.

ps2facts is not trying to replace it. It is a loader-debugging aid that
happens to identify the console, and it keeps three properties PS2Ident does
not aim for:

- **the `selects` row** — what a PS2 Linux loader would choose on this console,
  which is the question it was written to answer
- **headless over ps2link** — `ps2client execee host:ps2facts.elf` prints the
  whole report as text you can capture and diff, and `--dump` needs no pad
- **loads no IOP modules and never resets the IOP** — so it runs on a console
  that is already misbehaving, without disturbing whatever launched it

Use PS2Ident to find out what your console is. Use this when you need to know
what a loader will do with it.

## Usage

Run it from wLaunchELF/uLaunchELF, or over the network with ps2link:

```sh
ps2client -h <console-ip> execee host:ps2facts.elf
ps2client -h <console-ip> execee host:ps2facts.elf --dump
```

`--dump` writes `rom0.bin` (4 MB) and `nvm.bin` (1 KB) with sizes and CRC32s:

```
rom0.bin  4194304 bytes  crc32 82aa5055  OK
nvm.bin      1024 bytes  crc32 73b6b991  OK
```

Files go to the first writable device of `mass0:`, `mass:`, `mc0:`, `mc1:`,
`host:` — so a USB stick when run from a launcher, or the ps2client working
directory over ps2link.

Launched from a memory card or USB stick, the pad works: **SELECT** dumps,
**START** exits. Over ps2link there is no PADMAN, so use `--dump` instead.
Either way it counts down ten seconds and reboots to the browser rather than
leaving the console stuck.

## Building

```sh
./build.sh                                   # in the ps2dev container
PS2FACTS_IMAGE=my/ps2dev:tag ./build.sh      # or your own image
make                                         # or directly, with PS2SDK set
```

Three ELFs come out:

| File | Size | For |
|---|---|---|
| `ps2facts.elf` | ~1.3 MB | has symbols — use it for `nm`/`addr2line` when something faults |
| `ps2facts-small.elf` | ~180 KB | stripped — put this on a memory card |
| `screentest/screentest.elf` | ~1.2 MB | the screen ruler, below |

The first two are the same program; the small one is stripped from the large
one so they cannot drift. A memory card is 8 MB and shared with saves, so the
size matters there.

`make` on its own builds only ps2facts. `screentest` has its own Makefile
because ps2sdk's `Makefile.eeglobal` is built around one `EE_BIN` per
directory; `build.sh` builds both.

## screentest

A second, much smaller program that answers one question: how much of the
ps2sdk debug screen a television actually shows.

It writes each row's own number into all 40 rows and a column ruler across
three of them, then stops. Whichever numbers you can read are the rows that
exist; the ruler gives the columns. No inference, no guessing from a
photograph.

```sh
ps2client -h <console-ip> execee host:screentest.elf
```

It exists because the figure had been guessed three times — 28 rows, then 26,
then 21 — each time off a photo, and ps2facts kept losing output off the bottom
of the screen as a result. Measured, it is **28 rows by 80 columns**: rows 00
to 27.

That matters more than it sounds. The library's own `MY` is 40
(`ee/debug/src/scr_printf.c`), and it will happily accept `scr_setXY(0, 35)`
and draw there. Rows 28–39 land outside the visible area, and row 40 wraps
back to row 0 — so a program that prints 30 lines silently overwrites its own
first two, and looks like it never printed them. The visible height is set by
the `SCISSOR_1` values in the library's setup template (`0,639,0,223`), not by
`MX`/`MY`, and the library's hardcoded `DY = 50` display offset is an NTSC
figure.

Any program using `scr_printf` on a real television is subject to this, which
is why the tool lives here rather than in whatever project last tripped over
it.

## Notes for anyone reading the source

Three things about a PS2 make a tool like this trickier than it looks, and all
three cost real debugging time here:

- **Kernel mode.** ROM (`0xBFC00000`), the DEV9 register (`0xBF80146E`) and the
  GS CSR (`0xB2001000`) are all above `0x7FFFFFFF`, and a ps2sdk program runs
  in user mode, where touching them raises an address error immediately.
- **Interrupts.** `ee_kmode_enter()` only clears `KSU`. If an interrupt lands
  mid-copy, the scheduler can restore a saved `Status` with user mode set and
  the next read faults — so kernel-mode sections here also disable interrupts,
  and stay short.
- **The debug screen is 28 rows by 80 columns**, though the library's own
  `MY` is 40. Rows 28–39 are drawn outside the visible area and row 40 wraps
  to row 0, so output longer than 28 rows silently overwrites its own start.
  This is measured, not assumed — `screentest/` is the ruler that measured it.

Reading the DEV9 revision register is done on a phat only. On a slim PSTwo that
read hangs the console outright, and a slim has the adapter built in anyway, so
there is nothing to probe for.

The tool loads no IOP modules and never resets the IOP, so it works on a
console that a loader will not boot on, and it does not disturb whatever
launched it.
