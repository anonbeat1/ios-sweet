/*
 * Nothing Phone (1) "Spacewar" probe 1: can we draw on a DSI *command mode*
 * panel (Visionox rm692e5) through the pipeline ABL leaves running?
 *   - find the SSPP that scans out the splash buffer (address, size, stride, bpp)
 *   - dump CTL / pingpong state (is autorefresh on?)
 *   - paint the buffer, then kick CTL_START on the active CTL every 20 ms
 * Downstream DT block offsets (sde-ctl-off 0x16000 ...) sit 0x1000 above the
 * mainline sc7280 catalog (0x15000 ...), so both layouts are tried and the one
 * whose SSPP points into the splash region wins.
 * No visible output at all? Count seconds to the reboot instead:
 *   ~10 s = MDP state not understood (no kick), ~40 s = kicked but invisible.
 */
#include <stdint.h>
#include "console.h"

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

#define PSCI_SYSTEM_RESET	0x84000009UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
			 "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/* Kick every 20 ms (ctl == 0: just wait), then reset; ABL boots Android again. */
static void __attribute__((noreturn)) kick_then_reset(uintptr_t ctl, unsigned secs)
{
	for (unsigned i = 0; i < secs * 50; i++) {
		if (ctl)
			wr32(ctl + CTL_START, 1);
		delay_ms(20);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET);
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	con_clear(BLUE);
	con_hex("PANIC", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	kick_then_reset(0, 10);
}

void secondary_main(uint64_t idx)
{
	(void)idx;
}

static void fill(uintptr_t base, uint32_t bytes, uint32_t v)
{
	for (uint32_t o = 0; o < bytes; o += 4)
		wr32(base + o, v);
}

void shim_main(uint64_t dtb)
{
	/* Same order as the DT lists them: VIG0, DMA0..2. */
	static const uint32_t sspp[4] = { 0x5000, 0x25000, 0x27000, 0x29000 };
	uint32_t shift = 0, addr = 0, size = 0, stride = 0, fmt = 0;
	int pipe = -1;

	/* 1. Which pipe scans out the splash buffer, and with which layout? */
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
	if (!addr)
		addr = SPLASH;

	uint32_t w = size & 0xffff, h = size >> 16;
	uint32_t bpp = ((fmt >> 9) & 3) + 1;
	int text = stride == FB_STRIDE && w == FB_W && h == FB_H && bpp == 4;

	/* 2. Paint. Even with an unexpected format a 32-bit fill changes the picture. */
	con_fb = (volatile uint32_t *)(uintptr_t)addr;
	if (text)
		con_clear(GREEN & 0xff006000U);
	else
		fill(addr, (stride ? stride : FB_STRIDE) * (h ? h : FB_H), 0xff00ff00U);

	/* 3. Active CTL: nonzero CTL_TOP (intf/mode select). */
	uintptr_t ctl = 0;
	if (text) {
		con_str("SPACEWAR", "probe 1: cmd-mode kick");
		con_hex("DTB", dtb);
		con_hex("MIDR", rsr("midr_el1"));
		con_hex("MPIDR", rsr("mpidr_el1"));
		con_hex("CURREL", rsr("CurrentEL"));
		con_hex2("PIPE", "n=", pipe, "shift=", shift);
		con_hex2("SRC", "addr=", addr, "stride=", stride);
		con_hex2("SIZE", "wh=", size, "fmt=", fmt);
	}
	if (pipe >= 0)
		for (int i = 0; i < 4; i++) {
			uintptr_t c = MDP + 0x16000 + i * 0x1000 - shift;
			uintptr_t pp = MDP + 0x6a000 + i * 0x1000 - shift;
			uint32_t top = rd32(c + CTL_TOP);
			if (top && !ctl)
				ctl = c;
			if (text) {
				char l[] = "CTL0";
				l[3] = '0' + i;
				con_hex2(l, "top=", top, "lm0=", rd32(c + CTL_LAYER0));
				l[0] = 'P', l[1] = 'P', l[2] = ' ';
				con_hex2(l, "autoref=", rd32(pp + PP_AUTOREFRESH), "flush=", rd32(c + CTL_FLUSH));
			}
		}
	if (text) {
		con_hex("KICKCTL", ctl);
		con_str("NEXT", ctl ? "reset in 40s" : "no ctl, reset in 10s");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	kick_then_reset(ctl, ctl ? 40 : 10);
}
