/* ps2facts - dump everything this console will tell us about itself.
 *
 * Exists because a phat SCPH-30003 (ROM 1.20) and a slim SCPH-70003 diverge
 * somewhere in kernelreloaded's boot, and inferring which console took which
 * branch from a photograph of a hung screen is slow and unreliable. This asks
 * the hardware directly, prints the answers, and writes them to a file so two
 * consoles can be diffed rather than remembered.
 *
 * Deliberately minimal about what it touches:
 *
 *   - it does NOT reset the IOP. Launched from wLaunchELF, the modules that
 *     serve mc0:/mc1:/mass: are already resident, so saving a report needs no
 *     module loading at all. Resetting would throw that away and is exactly
 *     the operation that has been unreliable on the phat.
 *   - it does NOT load modules. Anything unavailable is reported as such
 *     rather than being made available.
 *   - the DEV9 revision register is read on a phat only. Reading it from the
 *     EE hangs a slim PSTwo outright -- not slowly, dead -- so the same
 *     romver test the loader uses guards it here.
 *
 * Output goes to the screen and, when a writable device is found, to
 * ps2facts-<romver>.txt on the first of mass0:, mc0:, mc1: that accepts it.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <debug.h>
#include <kernel.h>
#include <libcdvd.h>
#include <sifrpc.h>
#include <sio.h>
#include <libpad.h>
#include <loadfile.h>
#include <fcntl.h>
#include <unistd.h>
#include <tamtypes.h>

/* Report buffer. Everything is written here first, then printed and saved, so
 * the screen copy and the file copy cannot disagree. */
static char report[16384];
static int reportLen;

static void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print immediately as well as buffering.
 *
 * The first version collected the whole report and printed it at the end, so a
 * hang anywhere produced a completely blank screen and told us nothing -- the
 * same failure mode the loader's on-screen boot log was built to fix. Printing
 * as we go means the screen always shows how far it got.
 *
 * Two destinations, which between them cover every way this gets run:
 *
 *   screen - a console with nothing attached but a TV
 *   printf - reaches SIO through ps2sdk's newlib _write(), and ps2link
 *            forwards it to ps2client, so the report can be collected over
 *            the network with no card, stick or camera
 */

/* Set to 0 around output that should reach the file and printf but not the TV.
 * Currently nothing uses it -- the whole report fits on screen -- but the
 * ROMDIR listing needed it before the lines were packed to the full width. */
static int screenEcho = 1;

static void emit(const char *fmt, ...)
{
	va_list ap;
	int start = reportLen;
	int n;

	if (reportLen >= (int) sizeof(report) - 1) {
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(report + reportLen, sizeof(report) - reportLen, fmt, ap);
	va_end(ap);

	if (n > 0) {
		const char *text;
		int i;

		reportLen += n;
		if (reportLen > (int) sizeof(report) - 1) {
			reportLen = sizeof(report) - 1;
		}
		text = report + start;

		/* Screen first: it is the output that cannot block. */
		if (screenEcho) {
			scr_printf("%s", text);
		}

		/* printf() already reaches SIO -- ps2sdk's newlib _write() sends
		 * stdout there, which is why every line appeared twice in the PCSX2
		 * log when this also wrote each character with sio_putc(). That
		 * duplicate loop is gone: sio_putc() spins waiting for room in the TX
		 * FIFO, and on a real console that spin never ended -- the phat
		 * stopped dead at "ps2facts starting..." with nothing after it. */
		printf("%s", text);
		(void) i;
	}
}

/* Kernel mode, for the duration of one hardware access.
 *
 * Everything this tool reads directly -- ROM at 0xBFC00000, the DEV9 register
 * at 0xBF80146E, the GS CSR at 0xB2001000 -- lies above 0x7FFFFFFF, and a
 * ps2sdk program runs in user mode, where those addresses raise an address
 * error the instant they are loaded from. That is the "EE Exception handler:
 * Address load/inst fetch exception, BadVAddr BFC00000" this printed on a
 * console. PCSX2 does not enforce the mode check, so it never appeared there.
 *
 * kernelreloaded does the same around its own low-level work. The sections are
 * kept short deliberately: a TLB miss cannot be serviced while in kernel mode,
 * so nothing that allocates, prints or does I/O belongs inside one. */
#define KMODE_BEGIN()	do { DIntr(); ee_kmode_enter(); } while (0)
#define KMODE_END()	do { ee_kmode_exit(); EIntr(); } while (0)

/* Interrupts off as well as kernel mode on, and that ordering matters.
 *
 * ee_kmode_enter() only clears KSU in the Status register. If an interrupt
 * arrives during the copy, the scheduler can switch threads and restore a
 * saved Status that has user mode set -- so the copy continues in user mode
 * and the next ROM read faults. That is exactly what happened dumping rom0:
 * "Address load/inst fetch exception, BadVAddr BFC8C5A0, Status 70030C13",
 * with bit 4 of Status set, i.e. user mode, in the middle of a bracketed
 * section. The first 64KB copy had survived only because nothing interrupted
 * it. Blocks stay small so interrupts are never off for long. */

/* ---------------------------------------------------------------- ROM ---- */

/* One entry in the ROMDIR table. The first is always "RESET", then "ROMDIR"
 * and "EXTINFO"; the walk ends at a zero-length name. */
typedef struct {
	char		name[10];
	u16		xi_size;
	u32		size;
} __attribute__((packed)) romentry_t;

/* A copy of the start of ROM, taken once in kernel mode.
 *
 * 64KB covers the ROMDIR table and the early files, ROMVER among them. Taking
 * one copy means exactly one kernel-mode section instead of one per parse, and
 * everything downstream is ordinary user-mode pointer work on RAM. */
static u8 romCopy[0x10000] __attribute__((aligned(64)));
static int romCopied;

static void copyRom(void)
{
	if (romCopied) {
		return;
	}
	romCopied = 1;

	/* Short and does nothing but move bytes: no allocation, no printing, no
	 * I/O, because a TLB miss cannot be serviced while in kernel mode. */
	KMODE_BEGIN();
	memcpy(romCopy, (const void *) 0xBFC00000, sizeof(romCopy));
	KMODE_END();
}

static romentry_t *findRomdir(void)
{
	romentry_t *romdir;
	u32 offset = 0;

	copyRom();
	romdir = (romentry_t *) romCopy;

	while (offset < sizeof(romCopy)) {
		if (strncmp(romdir->name, "RESET", 6) == 0) {
			if (((romdir->size + sizeof(*romdir) - 1)
				& ~(sizeof(*romdir) - 1)) == offset) {
				return romdir;
			}
		}
		romdir++;
		offset += sizeof(*romdir);
	}
	return NULL;
}

/* Read ROMVER out of the copy.
 *
 * Not through fioOpen("rom0:ROMVER"): that needs the ROM filesystem driver to
 * be resident, and the point of this tool is to depend on nothing. */
static void romver(char *out, int outLen)
{
	romentry_t *romdir;
	const u8 *fileaddr;

	out[0] = 0;

	romdir = findRomdir();
	if (romdir == NULL) {
		return;
	}

	fileaddr = romCopy;
	while (romdir->name[0] != 0) {
		if (strncmp(romdir->name, "ROMVER", 7) == 0) {
			int n = (romdir->size < (u32) outLen - 1)
				? (int) romdir->size : outLen - 1;
			char *nl;

			if (fileaddr + n > romCopy + sizeof(romCopy)) {
				return;
			}
			memcpy(out, fileaddr, n);
			out[n] = 0;
			/* ROMVER is one line plus padding on some ROMs. */
			nl = strchr(out, '\n');
			if (nl != NULL) {
				*nl = 0;
			}
			return;
		}
		fileaddr += (romdir->size + 15) & ~15;
		romdir++;
	}
}

static void listRomdir(const char *label)
{
	romentry_t *romdir;
	int n = 0;

	romdir = findRomdir();
	if (romdir == NULL) {
		emit("%s: no ROMDIR found\n", label);
		return;
	}

	{
		romentry_t *count = romdir;

		while (count->name[0] != 0) {
			n++;
			count++;
		}
		/* Counted up front so the heading carries the total, rather than
		 * spending a second row on it afterwards. */
		emit("%s %d entries:", label, n);
	}
	{
		int col = 79;	/* forces a newline before the first name */

		while (romdir->name[0] != 0) {
			char name[11];
			int len;

			memcpy(name, romdir->name, 10);
			name[10] = 0;
			len = (int) strlen(name);

			/* Packed to the line width rather than in fixed columns: names
			 * average about seven characters against a ten-character field,
			 * so fixed columns threw away a third of every line. */
			if (col + len + 1 > 78) {
				emit("\n ");
				col = 1;
			}
			emit("%s ", name);
			col += len + 1;
			romdir++;
		}
	}
	emit("\n");
}

/* Is a named file present in this ROM? Several loader decisions turn on this
 * -- ROMGSCRT decides whether video mode setting goes through the ROM routine
 * or raw DVE register writes, ADDDRV is absent on ProtoKernel consoles. */
static int romHas(const char *name)
{
	romentry_t *romdir = findRomdir();

	if (romdir == NULL) {
		return 0;
	}
	while (romdir->name[0] != 0) {
		if (strncmp(romdir->name, name, 10) == 0) {
			return 1;
		}
		romdir++;
	}
	return 0;
}

/* ------------------------------------------------------------ silicon ---- */

static u32 eePRid(void)
{
	u32 prid;

	__asm__ __volatile__("mfc0 %0, $15\n sync.p" : "=r"(prid));
	return prid;
}

static u32 eeConfig(void)
{
	u32 config;

	__asm__ __volatile__("mfc0 %0, $16\n sync.p" : "=r"(config));
	return config;
}

/* GS revision from CSR, bits 16-23 ID and 0-15 revision.
 *
 * Through KSEG1 (0xB2001000), not the bare physical address 0x12001000.
 * Reading the physical address as a plain pointer only works if something has
 * TLB-mapped it; PCSX2 tolerated it, a real console raised an EE exception.
 * KSEG1 is 0xA0000000 + physical and needs no mapping, which is the whole
 * point of using it for hardware registers. */
static u32 gsCSR(void)
{
	u32 csr;

	KMODE_BEGIN();
	csr = *((volatile u32 *) 0xB2001000);
	KMODE_END();
	return csr;
}

/* No memory-size probe here, deliberately.
 *
 * Two attempts were wrong. Writing past 32MB to test for aliasing hung the
 * console before it printed anything -- unfitted memory is unmapped, not
 * aliased. Reading 0x80000000 as "the BIOS boot info word" returned
 * 0x3c1a8001, which is a MIPS lui instruction: that address holds code.
 *
 * A retail console is 32MB and there is nothing here worth risking a hang or
 * printing a wrong number for. What matters for FAKE_EXTRA_RAM is what the
 * loader TELLS Linux, which is a build-time constant, not a hardware fact.
 */

/* ---------------------------------------------------------------- CDVD --- */

static void cdvdFacts(void)
{
	u8 mvbuf[8];
	u32 status = 0;
	u16 nvm = 0;
	u8 result = 0;
	int rv;

	/* No sceCdInit() here: doing it resets CDVD state that whatever launched
	 * this tool may be relying on, and if the driver is not up there is
	 * nothing useful to say anyway. */
	memset(mvbuf, 0, sizeof(mvbuf));
	rv = sceCdMV(mvbuf, &status);
	emit("MechaCon %d.%02d", mvbuf[1], mvbuf[0]);

	rv = sceCdReadNVM(0x180, &nvm, &result);
	if (rv == 1 && result == 0) {
		emit("  NVM180 0x%04x\n", (unsigned int) nvm);
	} else {
		emit("  NVM180 unreadable (%d/0x%02x)\n", rv, (unsigned int) result);
	}
}

/* --------------------------------------------------------------- DEV9 ---- */

/* Only ever called on a phat -- see the file header. */
static void dev9Facts(int slim)
{
	u16 rev;

	if (slim) {
		emit("DEV9        : not read (hangs a slim, built in)  ");
		return;
	}

	KMODE_BEGIN();
	rev = *((volatile u16 *) 0xBF80146E);
	KMODE_END();
	/* Mask before comparing: a real console answers 0x31 for the expansion
	 * bay, not a clean 0x30, and an exact-match decode called that "unknown".
	 * The type lives in the high nibble. */
	emit("DEV9        : 0x%04x (%-9s)  ", rev,
		(rev == 0) ? "absent" :
		((rev & 0xf0) == 0x20) ? "PCMCIA" :
		((rev & 0xf0) == 0x30) ? "exp. bay" : "unknown");
}

/* --------------------------------------------------------------- save ---- */

/* Try each writable device in turn. Nothing is loaded to make one work: if
 * whatever launched this left mass: or the cards usable, the report is saved;
 * if not, the screen copy is the report. */
static const char *saveBuffer(const char *filename, const void *data, int len)
{
	static const char *dirs[] = {
		"mass0:", "mass:", "mc0:", "mc1:", "host:"
	};
	static char path[64];
	unsigned int i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		int fd;

		snprintf(path, sizeof(path), "%s%s", dirs[i], filename);
		/* POSIX rather than fioOpen: ps2sdk's newlib port refuses fio/fileXio
		 * calls outright ("Use posix function calls instead"), because the two
		 * layers keep separate ideas of open files. */
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			int written = (int) write(fd, data, len);

			close(fd);
			if (written == len) {
				return path;
			}
		}
	}
	return NULL;
}

/* --------------------------------------------------------------- dump ---- */

/* CRC32 of what was dumped, so the screen can be checked against the file.
 *
 * Standard CRC32 (the zlib/PNG polynomial), computed a nibble at a time from a
 * 16-entry table -- small enough to sit in the binary without a generator, and
 * fast enough for 4MB on a 300MHz EE. Printing a checksum matters because a
 * dump that silently wrote short bytes looks exactly like one that worked. */
static const u32 crcTable[16] = {
	0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
	0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
	0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
	0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};

static u32 crcUpdate(u32 crc, const u8 *data, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		crc = (crc >> 4) ^ crcTable[crc & 0x0f];
		crc = (crc >> 4) ^ crcTable[crc & 0x0f];
	}
	return crc;
}

/* One real second, from the console's own clock.
 *
 * The first attempt spun a volatile counter 8 million times, which on a
 * 294MHz EE is about a tenth of a second -- the whole ten-second countdown
 * went by in one. Rather than recalibrate a guess, this watches the RTC that
 * CDVD already exposes: sceCdReadClock() is the same call the browser's date
 * comes from, needs no kernel mode, and its seconds field is BCD.
 *
 * Waiting for the field to CHANGE rather than counting spins means it is a
 * real second whatever the clock speed, and the first tick is short by however
 * far into the current second we started -- which is invisible in a countdown
 * and not worth another poll to avoid. */
static void waitOneSecond(void)
{
	sceCdCLOCK now;
	u8 startSec;
	int guard;

	if (sceCdReadClock(&now) != 1) {
		/* No RTC answer: fall back to a spin, imprecise but not stuck. */
		volatile int i;

		for (i = 0; i < 70000000; i++) {
			;
		}
		return;
	}
	startSec = now.second;

	/* Bounded so a stopped clock cannot hang the countdown for ever. */
	for (guard = 0; guard < 2000000; guard++) {
		if (sceCdReadClock(&now) == 1 && now.second != startSec) {
			return;
		}
	}
}

/* Write a file to the first device that accepts it, returning the path.
 * Shared by the report and the image dumps. */
static const char *saveBuffer(const char *filename, const void *data, int len);

/* Copy ROM to a file in 64KB chunks.
 *
 * Chunked because each chunk is one short kernel-mode section: ROM lives at
 * 0xBFC00000, which user mode cannot touch, and a TLB miss cannot be serviced
 * while in kernel mode -- so the file write happens outside it, between
 * chunks, not during.
 *
 * rom0: only. rom1: and erom sit at addresses that vary between models
 * (0x1E000000 and thereabouts), and reading a wrong one on hardware is the
 * class of mistake that has already cost this project a hung console. rom0 is
 * the one whose address is certain. */
static void dumpRom(void)
{
	static u8 chunk[0x10000] __attribute__((aligned(64)));
	const u32 romBase = 0xBFC00000;
	const u32 romSize = 0x400000;	/* 4MB */
	char path[64];
	int fd = -1;
	u32 off;
	u32 crc = 0xffffffff;
	u32 written = 0;
	static const char *dirs[] = { "mass0:", "mass:", "mc0:", "mc1:", "host:" };
	unsigned int i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%srom0.bin", dirs[i]);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			break;
		}
	}
	if (fd < 0) {
		scr_printf("rom0 dump: no writable device\n");
		return;
	}

	emit("dumping rom0 (4MB) to %s\n", path);
	for (off = 0; off < romSize; off += sizeof(chunk)) {
		KMODE_BEGIN();
		memcpy(chunk, (const void *) (romBase + off), sizeof(chunk));
		KMODE_END();

		if (write(fd, chunk, sizeof(chunk)) != (int) sizeof(chunk)) {
			emit("rom0: WRITE FAILED at 0x%06x\n", (unsigned int) off);
			close(fd);
			return;
		}
		crc = crcUpdate(crc, chunk, sizeof(chunk));
		written += sizeof(chunk);
	}
	close(fd);
	emit("rom0.bin  %u bytes  crc32 %08x  OK\n",
		(unsigned int) written, (unsigned int) (crc ^ 0xffffffff));
}

/* The whole 1KB of NVM, a word at a time through CDVD.
 *
 * Per-console rather than per-model: region, video mode, the model string the
 * loader's System Info panel shows. The report reads two bytes of it; this
 * takes the lot. */
static void dumpNvm(void)
{
	static u8 nvm[1024];
	const char *saved;
	int i;

	emit("reading NVM (1KB)\n");
	for (i = 0; i < 512; i++) {
		u16 word = 0;
		u8 result = 0;

		if (sceCdReadNVM((u32) i, &word, &result) != 1 || result != 0) {
			emit("nvm: FAILED at word %d\n", i);
			return;
		}
		nvm[i * 2] = (u8) (word & 0xff);
		nvm[i * 2 + 1] = (u8) (word >> 8);
	}

	saved = saveBuffer("nvm.bin", nvm, sizeof(nvm));
	if (saved == NULL) {
		emit("nvm: no writable device\n");
		return;
	}
	emit("nvm.bin   %u bytes  crc32 %08x  OK  (%s)\n",
		(unsigned int) sizeof(nvm),
		(unsigned int) (crcUpdate(0xffffffff, nvm, sizeof(nvm)) ^ 0xffffffff),
		saved);
}

/* --------------------------------------------------------------- main ---- */

int main(int argc, char **argv)
{
	char ver[64];
	char filename[64];
	int wantDump = 0;
	const char *saved;
	u32 rv = 0;
	int slim;
	int i;

	/* SIO first: it is the one output that works when nothing else does, and
	 * costs nothing if no cable is attached. 38400 8N1, matching what the
	 * loader's kprintf() uses, so the same terminal reads both. */
	sio_init(38400, 0, 0, 0, 0);

	init_scr();
	/* init_scr() turns the cursor on, which draws a filled block at the start
	 * of every line it lands on -- the white squares down the left edge. There
	 * is nothing to type at here. */
	scr_setCursor(0);
	/* Before anything is probed, so "started but hung in a probe" and "never
	 * ran at all" are different pictures on screen. */
	/* Measured, not guessed: 28 rows by 80 columns.
	 *
	 * tools/screentest writes its own row number into all 40 rows the library
	 * claims (MX=80, MY=40 in scr_printf.c). Rows 00 to 27 are readable and
	 * every column to 79 is, so the drawing area is exactly the 224-line
	 * scissor in the setup template: 28 rows of an 8-pixel font.
	 *
	 * That also explains the "top line is cut off" this went round in circles
	 * on. Rows 28 to 39 exist as far as the library is concerned but are drawn
	 * outside the visible area, and at row 40 it wraps to row 0 -- so printing
	 * more than 28 rows silently overwrites the start of the report. It was
	 * never overscan and never scrolling; the output ate its own head. Keep
	 * the report inside 28 rows and row 0 is a perfectly good place to start. */
	scr_setXY(0, 0);
	/* The per-step markers that used to print here are gone.
	 *
	 * They existed to find where this hung on a phat -- the answer was user
	 * mode versus the ROM addresses, fixed with KMODE_BEGIN -- and each one
	 * cost a row on a screen with about 21. Add them back if it ever stops
	 * mid-startup again. */
	SifInitRpc(0);
	romver(ver, sizeof(ver));

	/* Same test the loader uses (modules.c isSlimPSTwo): the first four
	 * characters of ROMVER are the version, 1.90 and above being slim. */
	if (strlen(ver) >= 4) {
		for (i = 0; i < 4; i++) {
			rv = (rv << 4) | (u32) (ver[i] - '0');
		}
	}
	slim = (rv > 0x0190);

	/* --dump: take the images without a pad.
	 *
	 * Under ps2link there is no PADMAN, so the SELECT/START menu cannot exist
	 * -- but ps2client passes arguments through, which is a perfectly good
	 * substitute for a button. */
	for (i = 0; i < argc; i++) {
		if (argv[i] != NULL && strcmp(argv[i], "--dump") == 0) {
			wantDump = 1;
		}
	}

	emit("ps2facts    ROMVER %s (0x%04x -> %s)\n",
		ver[0] ? ver : "unreadable", (unsigned int) rv,
		slim ? "slim PSTwo" : "phat PS2");
	emit("EE PRid/Cfg : 0x%08x / 0x%08x    GS CSR : 0x%08x (rev %02x id %02x)\n",
		(unsigned int) eePRid(), (unsigned int) eeConfig(),
		(unsigned int) gsCSR(),
		(unsigned int) (gsCSR() & 0xff),
		(unsigned int) ((gsCSR() >> 16) & 0xff));

	dev9Facts(slim);
	cdvdFacts();

	/* The ROM contents the loader's decisions actually turn on. */
	emit("ROMGSCRT %-7s -> %-15s  ADDDRV %s\n",
		romHas("ROMGSCRT") ? "present" : "ABSENT",
		romHas("ROMGSCRT") ? "sbcall_setgscrt" : "sbcall_setdve",
		romHas("ADDDRV") ? "present" : "ABSENT");
	/* plain/X-prefixed pairs -- the X ones are the slim-era modules. */
	emit("MC %s/%s MCSERV %s/%s SIO2 %s/%s PAD %s/%s (plain/X)\n",
		romHas("MCMAN") ? "y" : "n",
		romHas("XMCMAN") ? "y" : "n",
		romHas("MCSERV") ? "y" : "n",
		romHas("XMCSERV") ? "y" : "n",
		romHas("SIO2MAN") ? "y" : "n",
		romHas("XSIO2MAN") ? "y" : "n",
		romHas("PADMAN") ? "y" : "n",
		romHas("XPADMAN") ? "y" : "n");

	/* What kernelreloaded would choose on this console. This is the row the
	 * tool exists for: the loader picks one interrupt relay from four, across
	 * a slim/phat axis and a DEV9 axis, and until now that choice could only
	 * be inferred from a boot log. */
	{
		int dev9;

		if (slim) {
			dev9 = 1;	/* assumed, never probed */
		} else {
			u16 rev;

			KMODE_BEGIN();
			rev = *((volatile u16 *) 0xBF80146E);
			KMODE_END();
			dev9 = (rev != 0);
		}

		emit("selects     : TGE/intrelay-%s%s.irx  smaprpc %s  ps2dev9 %s\n",
			dev9 ? "dev9" : "direct", slim ? "-rpc" : "",
			(slim && dev9) ? "yes" : "no", dev9 ? "yes" : "no");
	}

	listRomdir("rom0:");

	snprintf(filename, sizeof(filename), "ps2facts-%s.txt",
		ver[0] ? ver : "unknown");
	/* Slashes and colons would make the name unopenable; ROMVER has neither,
	 * but an unreadable ROM falls back to a literal that must stay safe. */
	saved = saveBuffer(filename, report, reportLen);

	/* No reprint of the buffer here.
	 *
	 * There used to be a scr_clear() followed by scr_printf("%s", report),
	 * left over from when everything was printed at the end. Once emit()
	 * started printing as it went, that reprint put the whole report back on
	 * screen -- including every line deliberately kept off it -- and scrolled
	 * the findings away. The screen already has what it should. */
	if (saved != NULL) {
		scr_printf("Saved to %s\n", saved);
	} else {
		scr_printf("No writable device found; screen copy only.\n");
	}


	/* Wait for SELECT, then reboot, rather than sitting here forever.
	 *
	 * The pad modules are not loaded here -- nothing in this tool loads
	 * anything -- so this works only when whatever launched it left them
	 * resident, which wLaunchELF does. If they are not there, say so and
	 * leave the report up; a power cycle is then the only way out, which is
	 * what it was before this existed. */
	{
		static char padBuf[256] __attribute__((aligned(64)));
		/* Launched over ps2link? Then PADMAN is not loaded.
		 *
		 * This tool loads no modules by design, so the pad RPC has nothing to
		 * answer it -- and padInit()/padPortOpen() do not fail in that case,
		 * they block. The screen stopped dead after "Saved to ..." with
		 * neither the prompt nor the no-pad message, which is what a blocked
		 * call looks like from outside.
		 *
		 * argv[0] names how it was started: ps2link passes host:ps2facts.elf.
		 * Under wLaunchELF it is a mass0:/mc0: path and PADMAN is resident. */
		const int hosted = (argc > 0 && argv[0] != NULL &&
			strncmp(argv[0], "host:", 5) == 0);

		if (hosted) {
			/* Through emit(), not scr_printf: over ps2link the screen is the
			 * one output the person running this cannot see. */
			emit("ps2link: no pad modules, so no SELECT/START.\n");
			if (wantDump) {
				dumpRom();
				dumpNvm();
			} else {
				emit("pass --dump to write rom0.bin and nvm.bin.\n");
			}
		} else if (padInit(0) == 1 && padPortOpen(0, 0, padBuf) > 0) {
			struct padButtonStatus buttons;

			scr_printf("SELECT: dump rom0 + NVM    START: exit\n");
			for (;;) {
				u16 pressed;

				if (padRead(0, 0, &buttons) == 0) {
					continue;
				}
				/* Active low: a pressed button reads as 0. */
				pressed = 0xffff ^ buttons.btns;

				if (pressed & PAD_START) {
					/* Exit now, skipping the countdown below. */
					padPortClose(0, 0);
					padEnd();
					LoadExecPS2("rom0:OSDSYS", 0, NULL);
				}
				if (pressed & PAD_SELECT) {
					dumpRom();
					dumpNvm();
					scr_printf("START: exit\n");
					/* Wait for release so one press is one dump. */
					while (padRead(0, 0, &buttons) != 0 &&
						(0xffff ^ buttons.btns) & PAD_SELECT) {
						;
					}
				}
			}
			padPortClose(0, 0);
			padEnd();

			/* rom0:OSDSYS is the browser -- where a normal reset lands and
			 * where FMCB picks the console up. */
			LoadExecPS2("rom0:OSDSYS", 0, NULL);
		} else {
			emit("(no pad detected)\n");
		}

		/* Ten seconds, then reboot to the browser.
		 *
		 * Previously this sat in an endless loop, so every run ended with a
		 * power cycle -- and over ps2link that also meant ps2link had to be
		 * relaunched by hand before anything else could be sent. Rebooting on
		 * its own returns the console to somewhere useful without being
		 * touched. */
		{
			int left;
			int row;

			/* Overwrite one line via scr_setXY, not "\r".
			 *
			 * ps2sdk's debug screen decodes '\n' and tab; a bare carriage
			 * return is not one of its cases, so the countdown redrew nowhere
			 * and the console appeared to reboot with no warning at all.
			 * Parking the cursor at a known row and reprinting is the only
			 * in-place update this library actually supports. */
			/* Clamp to a row that is actually visible.
			 *
			 * scr_getY() is honest about where the cursor is, and after the
			 * report plus dump lines that is around row 24 -- below the ~21
			 * rows this screen displays, which is why the countdown ran
			 * invisibly while everything above it showed fine. Better to
			 * overwrite one line of a report that is already saved to file
			 * than to print where nobody can see. */
			row = scr_getY();
			if (row > 27) {
				/* Past the last visible row: draw on it rather than into the
				 * invisible region, where the previous version put it. */
				row = 27;
			}
			scr_clearline(row);
			for (left = 10; left > 0; left--) {
				scr_setXY(0, row);
				scr_printf("rebooting in %2d ...", left);
				waitOneSecond();
			}
			scr_setXY(0, row);
			scr_printf("rebooting now       \n");
		}
		LoadExecPS2("rom0:OSDSYS", 0, NULL);

		/* Only if LoadExecPS2 declined. */
		for (;;) {
			;
		}
	}

	(void) argc;
	(void) argv;
	return 0;
}
