/*
 * Nothing Phone (1) "Spacewar" display glue for the shim (built with -DSPACEWAR).
 *
 * The Visionox rm692e5 is a DSI *command mode* panel: it only shows a new
 * frame when MDP is kicked (CTL_START). Probe 1 (commit c18860f) proved that
 * kicking the CTL ABL left active refreshes the panel from the splash buffer.
 * sw_display_init() finds that pipeline; one secondary CPU then runs
 * sw_kick_loop() forever so the shared console code works unchanged.
 *
 * Downstream DT block offsets (sde-ctl-off 0x16000 ...) sit 0x1000 above the
 * mainline sc7280 catalog (0x15000 ...), so both layouts are tried and the one
 * whose SSPP points into the splash region wins.
 */
#include <stdint.h>
#include "console.h"
#include "spacewar.h"

#define MDP		0x0ae00000UL
#define SPLASH		0xe1000000UL	/* reserved-memory splash_region */
#define SPLASH_SIZE	0x02300000UL

#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT	0x30
#define CTL_LAYER0	0x00
#define CTL_TOP		0x14
#define CTL_FLUSH	0x18
#define CTL_START	0x1c
#define PP_AUTOREFRESH	0x30
#define PP_LINE_COUNT	0x2c
/*
 * DPU >= 5 keeps tear check + autorefresh in the INTF block (mainline
 * dpu_hw_intf.c INTF_TEAR_*): probe 1 read PP autorefresh as 0.
 */
#define INTF1		(MDP + 0x36000)		/* sde-intf-off[1], type dsi */
#define TE_EN		0x280
#define TE_VSYNC	0x284
#define TE_HEIGHT	0x288
#define TE_WRCOUNT	0x28c
#define TE_THRESH	0x298
#define TE_START	0x29c
#define TE_LINE_COUNT	0x2a8
#define TE_AUTOREFRESH	0x2ac

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

volatile uintptr_t sw_ctl;	/* read by the kick CPU with its MMU off */
static uint32_t shift, addr, size, stride, fmt;
static int pipe = -1;

int sw_display_init(void)
{
	/* Same order as the DT lists them: VIG0, DMA0..2. */
	static const uint32_t sspp[4] = { 0x5000, 0x25000, 0x27000, 0x29000 };

	for (uint32_t s = 0; s <= 0x1000 && pipe < 0; s += 0x1000)
		for (int i = 0; i < 4; i++) {
			uintptr_t b = MDP + sspp[i] - s;
			uint32_t a = rd32(b + SSPP_SRC0_ADDR);
			if (a >= SPLASH && a < SPLASH + SPLASH_SIZE) {
				shift = s, pipe = i, addr = a;
				size = rd32(b + SSPP_SRC_SIZE);
				stride = rd32(b + SSPP_YSTRIDE0) & 0xffff;
				fmt = rd32(b + SSPP_SRC_FORMAT);
				break;
			}
		}
	if (pipe < 0)
		return -1;
	/* Active CTL: nonzero CTL_TOP (intf/mode select). */
	for (int i = 0; i < 4 && !sw_ctl; i++) {
		uintptr_t c = MDP + 0x16000 + i * 0x1000 - shift;
		if (rd32(c + CTL_TOP))
			sw_ctl = c;
	}
	con_fb = (volatile uint32_t *)(uintptr_t)addr;
	__asm__ volatile("dsb sy" ::: "memory");
	/* The console assumes 1080x2400 a8r8g8b8 (probe 1: text was readable). */
	return sw_ctl && stride == FB_STRIDE && size == (FB_H << 16 | FB_W) &&
	       ((fmt >> 9) & 3) == 3 ? 0 : -2;
}

void sw_display_dump(void)
{
	con_hex2("PIPE", "n=", pipe, "shift=", shift);
	con_hex2("SRC", "addr=", addr, "stride=", stride);
	con_hex2("SIZE", "wh=", size, "fmt=", fmt);
	for (int i = 0; i < 4; i++) {
		uintptr_t c = MDP + 0x16000 + i * 0x1000 - shift;
		uintptr_t pp = MDP + 0x6a000 + i * 0x1000 - shift;
		char l[] = "CTL0";
		l[3] = '0' + i;
		con_hex2(l, "top=", rd32(c + CTL_TOP), "lm0=", rd32(c + CTL_LAYER0));
		l[0] = 'P', l[1] = 'P', l[2] = ' ';
		con_hex2(l, "autoref=", rd32(pp + PP_AUTOREFRESH), "flush=", rd32(c + CTL_FLUSH));
	}
	con_hex("KICKCTL", sw_ctl);
	con_hex2("TE", "en=", rd32(INTF1 + TE_EN), "vsync=", rd32(INTF1 + TE_VSYNC));
	con_hex2("TE", "height=", rd32(INTF1 + TE_HEIGHT), "wrcnt=", rd32(INTF1 + TE_WRCOUNT));
	con_hex2("TE", "start=", rd32(INTF1 + TE_START), "thresh=", rd32(INTF1 + TE_THRESH));
	con_hex2("TE", "autoref=", rd32(INTF1 + TE_AUTOREFRESH), "line=", rd32(INTF1 + TE_LINE_COUNT));
	con_hex2("PP0", "line=", rd32(MDP + 0x6a000 - shift + PP_LINE_COUNT), 0, 0);
}

void sw_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(sw_ctl + CTL_START, 1);
}

/* How often the INTF tear line counter changed in ~100 ms (0 = no frames moving). */
static int te_moves(void)
{
	uint64_t f = rsr("cntfrq_el0"), end = rsr("cntpct_el0") + f / 10;
	uint32_t last = rd32(INTF1 + TE_LINE_COUNT);
	int moves = 0;

	while (rsr("cntpct_el0") < end) {
		uint32_t v = rd32(INTF1 + TE_LINE_COUNT);
		moves += v != last;
		last = v;
	}
	return moves;
}

/* MDP refreshes the panel on every TE by itself: frame_count 1, then one kick. */
int sw_autorefresh(void)
{
	int idle = te_moves();

	wr32(INTF1 + TE_AUTOREFRESH, 1U << 31 | 1);
	sw_kick();
	return idle << 16 | te_moves();
}

void __attribute__((noreturn)) sw_kick_loop(void)
{
	uint64_t period = rsr("cntfrq_el0") / 50;	/* 20 ms */

	for (uint64_t next = rsr("cntpct_el0");; next += period) {
		wr32(sw_ctl + CTL_START, 1);
		while (rsr("cntpct_el0") < next + period)
			;
	}
}
