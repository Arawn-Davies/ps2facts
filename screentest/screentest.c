/* screentest - measure the usable text area of ps2sdk's debug screen.
 *
 * Exists because every figure used for it so far has been a guess read off a
 * photograph: 28 rows, then 26, then 21, none of them measured. The library
 * itself says MX = 80, MY = 40 (ee/debug/src/scr_printf.c), but how much of
 * that a television actually shows depends on the display offset the library
 * hardcodes -- DY = 50 scanlines, an NTSC figure -- and on overscan, which
 * varies by set and by cable.
 *
 * So: write each row's own number into all 40 rows, and a column ruler across
 * three of them. The first and last readable row numbers are the vertical
 * extent; the ruler gives the horizontal one. No inference required.
 *
 * Runs standalone from wLaunchELF or over ps2link, loads nothing, resets
 * nothing.
 */

#include <stdio.h>
#include <debug.h>
#include <sifrpc.h>

int main(int argc, char **argv)
{
	int r;

	init_scr();
	/* The cursor block would sit on top of a measurement. */
	scr_setCursor(0);
	SifInitRpc(0);

	for (r = 0; r < 40; r++) {
		scr_setXY(0, r);
		/* Row number in its own row: whichever numbers are readable on the
		 * TV are the rows that exist as far as anything using this screen is
		 * concerned. */
		scr_printf("row %02d", r);

		/* Column ruler on three rows, spread top to bottom so horizontal
		 * cropping can be seen to differ with vertical position if it does.
		 * Every tenth column carries its tens digit. */
		if (r == 5 || r == 20 || r == 35) {
			int c;

			scr_setXY(7, r);
			for (c = 7; c < 80; c++) {
				scr_printf("%c", ((c % 10) == 0) ? (char) ('0' + (c / 10)) : '.');
			}
		}
	}

	/* Printed last so it is not overwritten by the loop above. */
	scr_setXY(8, 0);
	scr_printf("SCREEN TEST  library says MX=80 MY=40");
	scr_setXY(8, 1);
	scr_printf("read off the first and last visible row numbers");

	/* Nothing to return to, and no countdown: the whole point is to leave a
	 * still picture on screen long enough to photograph. Power cycle, or
	 * reset back into whatever launched this. */
	for (;;) {
		;
	}

	(void) argc;
	(void) argv;
	return 0;
}
