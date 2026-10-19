/*
 * Spacewar CPU frequency probe: EPSS cpufreq-hw (live DT qcom,cpufreq-hw-epss,
 * domains 0x18591000 A55 / 0x18592000 A78 / 0x18593000 prime; offsets from
 * drivers/cpufreq/qcom-cpufreq-hw.c epss_std_offsets, lut-row-size 4).
 * Reads each domain's LUT and current level, times a busy loop on cpu0,
 * requests the top LUT index on every domain (what Linux does for max),
 * then times the loop again.
 */
#include <stdint.h>
#include "console.h"
#include "cpufreq.h"

#define EPSS(d)		(0x18591000UL + 0x1000UL * (d))
#define REG_ENABLE	0x000
#define REG_DOM_STATE	0x020
#define REG_LUT		0x100
#define REG_PERF_STATE	0x320

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

/* 50M dependent adds: time in us at 19.2 MHz */
static uint32_t loop_us(void)
{
	uint64_t t0 = ticks(), x = 1;
	for (uint32_t i = 0; i < 50000000u; i++)
		__asm__ volatile("add %0, %0, #1" : "+r"(x));
	return (uint32_t)((ticks() - t0) * 10 / 192);
}

/* top LUT index (last row before the repeated end marker) */
static unsigned lut_top(uintptr_t b, uint32_t *top_mhz, uint32_t *first_mhz)
{
	uint32_t prev = 0;
	unsigned i;
	for (i = 0; i < 40; i++) {
		uint32_t row = rd(b + REG_LUT + 4 * i);
		if (i > 0 && row == prev)
			break;
		uint32_t mhz = (row >> 30) ? (row & 0xff) * 192 / 10 : 300;
		if (i == 0)
			*first_mhz = mhz;
		*top_mhz = mhz;
		prev = row;
	}
	return i ? i - 1 : 0;
}

void cpufreq_probe(void)
{
	con_str("CPUFREQ", "EPSS probe: A55 / A78 / prime");
	unsigned top[3];
	for (int d = 0; d < 3; d++) {
		uintptr_t b = EPSS(d);
		uint32_t tmhz = 0, fmhz = 0;
		top[d] = lut_top(b, &tmhz, &fmhz);
		char l[] = "dom 0";
		l[4] = '0' + d;
		con_hex2(l, "en|perf=", rd(b + REG_ENABLE) << 16 | rd(b + REG_PERF_STATE), "cur MHz=",
		    (rd(b + REG_DOM_STATE) & 0xff) * 192 / 10);
		con_hex2("", "lut top=", top[d], "MHz lo|hi=", fmhz << 16 | tmhz);
	}
	con_hex2("LOOP", "before us=", loop_us(), 0, 0);
	for (int d = 0; d < 3; d++)
		wr(EPSS(d) + REG_PERF_STATE, top[d]);
	for (volatile int i = 0; i < 2000000; i++)
		;
	for (int d = 0; d < 3; d++) {
		char l[] = "dom 0";
		l[4] = '0' + d;
		con_hex2(l, "perf now=", rd(EPSS(d) + REG_PERF_STATE), "cur MHz=",
		    (rd(EPSS(d) + REG_DOM_STATE) & 0xff) * 192 / 10);
	}
	con_hex2("LOOP", "after us=", loop_us(), 0, 0);
}
