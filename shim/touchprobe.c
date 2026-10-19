/*
 * Spacewar touch probe (read-only): what does ABL leave for the Focaltech
 * FT3680 on QUP SE13 (SPI, 0xa94000)? Offsets from the Nothing kernel
 * sources (gcc-yupik.c, pinctrl-yupik.c) and the live DT; see NOTES.md.
 *
 * GCC branch CBCR bit 31 = CLK_OFF. The SE registers are only read when
 * every clock on their path is on: an unclocked access would hang the bus.
 */
#include <stdint.h>
#include "console.h"
#include "touchprobe.h"

#define GCC		0x00100000UL
#define GCC_VOTE	0x52008		/* APCS branch enable vote */
#define TLMM_GPIO(n)	(0x0f100000UL + 0x1000UL * (n))	/* ctl; +4 io */
#define SE13		0x00a94000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

void test_touch(void)
{
	/* name, CBCR offset, vote bit */
	static const struct { char name[8]; uint32_t cbcr, bit; } clk[5] = {
		{ "se13", 0x185fc, 27 }, { "w1 mahb", 0x18004, 20 }, { "w1 sahb", 0x18008, 21 },
		{ "w1 core", 0x23138, 19 }, { "w1 c2x", 0x23140, 18 },
	};
	uint32_t vote = rd(GCC + GCC_VOTE);
	int all_on = 1;

	con_str("TOUCH", "probe (read-only), FT3680 on SE13");
	con_hex2("GCC", "vote=", vote, 0, 0);
	for (int i = 0; i < 5; i++) {
		uint32_t cbcr = rd(GCC + clk[i].cbcr);
		con_hex2(clk[i].name, "cbcr=", cbcr, "voted=", (vote >> clk[i].bit) & 1);
		if (cbcr & (1U << 31))
			all_on = 0;
	}
	/* SPI pins 52-55 (trusted-touch io bases), irq 81, reset 105 */
	static const uint8_t pins[6] = { 52, 53, 54, 55, 81, 105 };
	for (int i = 0; i < 6; i++) {
		char l[] = "GPIO  0";
		l[4] = '0' + pins[i] / 100, l[5] = '0' + pins[i] / 10 % 10, l[6] = '0' + pins[i] % 10;
		con_hex2(l, "ctl=", rd(TLMM_GPIO(pins[i])), "io=", rd(TLMM_GPIO(pins[i]) + 4));
	}
	if (!all_on) {
		con_str("SE13", "a clock is off: SE registers not read");
		return;
	}
	/* GENI: FW_REVISION_RO [15:8] protocol (1 SPI, 2 UART, 3 I2C), [7:0] version */
	con_hex2("SE13", "fw_rev=", rd(SE13 + 0x68), "status=", rd(SE13 + 0x40));
	con_hex2("SE13", "clk_sel=", rd(SE13 + 0x7c), "m_clk=", rd(SE13 + 0x48));
	con_hex2("SE13", "spi_cfg=", rd(SE13 + 0x22c), "word=", rd(SE13 + 0x24c));
	con_hex2("SE13", "irq_en=", rd(SE13 + 0x614), "m_cmd0=", rd(SE13 + 0x600));
}
