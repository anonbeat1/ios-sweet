/*
 * Spacewar touch probe (read-only): what does ABL leave for the Focaltech
 * FT3680 on QUP SE13 (SPI, 0xa94000)? Offsets from the Nothing kernel
 * sources (gcc-yupik.c, pinctrl-yupik.c) and the live DT; see NOTES.md.
 *
 * GCC branch CBCR bit 31 = CLK_OFF. The SE registers are only read when
 * every clock on their path is on: an unclocked access would hang the bus.
 * Probe 2 (the only writes): vote SE13 + wrapper-1 AHB clocks on in the APCS
 * branch vote register, as Linux does. No rail, pin or SE programming.
 * The AHB branches are hardware clock gated (CBCR HW_CTL, bit 1): they read
 * CLK_OFF while idle and Linux skips their halt check, so only the SE and
 * core clocks gate the SE register reads (probe 2 showed sahb 0x8000000A).
 */
#include <stdint.h>
#include "console.h"
#include "touchprobe.h"

#define GCC		0x00100000UL
#define GCC_VOTE	0x52008		/* APCS branch enable vote */
#define TLMM_GPIO(n)	(0x0f100000UL + 0x1000UL * (n))	/* ctl; +4 io */
#define SE13		0x00a94000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

void test_touch(void)
{
	/* name, CBCR offset, vote bit, hardware gated */
	static const struct { char name[8]; uint32_t cbcr, bit, hwcg; } clk[5] = {
		{ "se13", 0x185fc, 27, 0 }, { "w1 mahb", 0x18004, 20, 1 }, { "w1 sahb", 0x18008, 21, 1 },
		{ "w1 core", 0x23138, 19, 0 }, { "w1 c2x", 0x23140, 18, 0 },
	};
	uint32_t vote = rd(GCC + GCC_VOTE);
	int all_on = 1;

	con_str("TOUCH", "probe 3 (clock vote only), FT3680 on SE13");
	con_hex2("GCC", "vote=", vote, 0, 0);
	for (int i = 0; i < 5; i++) {
		uint32_t cbcr = rd(GCC + clk[i].cbcr);
		con_hex2(clk[i].name, "cbcr=", cbcr, "voted=", (vote >> clk[i].bit) & 1);
		if ((cbcr & (1U << 31)) && !clk[i].hwcg)
			all_on = 0;
	}
	/* SPI pins 52-55 (trusted-touch io bases), irq 81, reset 105 */
	static const uint8_t pins[6] = { 52, 53, 54, 55, 81, 105 };
	for (int i = 0; i < 6; i++) {
		char l[] = "GPIO  0";
		l[4] = '0' + pins[i] / 100, l[5] = '0' + pins[i] / 10 % 10, l[6] = '0' + pins[i] % 10;
		con_hex2(l, "ctl=", rd(TLMM_GPIO(pins[i])), "io=", rd(TLMM_GPIO(pins[i]) + 4));
	}
	if (!all_on || !((vote >> 20) & (vote >> 21) & 1)) {
		wr(GCC + GCC_VOTE, vote | 1U << 27 | 1U << 21 | 1U << 20);
		__asm__ volatile("dsb sy" ::: "memory");
		all_on = 1;
		for (int i = 0; i < 5; i++) {
			uint32_t cbcr = 0;
			for (int t = 0; t < 100000 && !clk[i].hwcg && ((cbcr = rd(GCC + clk[i].cbcr)) & (1U << 31)); t++)
				;
			if ((cbcr & (1U << 31)) && !clk[i].hwcg)
				all_on = 0;
		}
		con_hex2("VOTED", "vote=", rd(GCC + GCC_VOTE), "all_on=", all_on);
		con_hex2(" se13", "cbcr=", rd(GCC + 0x185fc), "sahb=", rd(GCC + 0x18008));
	}
	con_hex2("RCG s5", "cmd=", rd(GCC + 0x18600), "cfg=", rd(GCC + 0x18604));
	if (!all_on) {
		con_str("SE13", "a clock is still off: SE registers not read");
		return;
	}
	/* GENI: FW_REVISION_RO [15:8] protocol (1 SPI, 2 UART, 3 I2C), [7:0] version */
	con_hex2("SE13", "fw_rev=", rd(SE13 + 0x68), "status=", rd(SE13 + 0x40));
	con_hex2("SE13", "clk_sel=", rd(SE13 + 0x7c), "m_clk=", rd(SE13 + 0x48));
	con_hex2("SE13", "spi_cfg=", rd(SE13 + 0x22c), "word=", rd(SE13 + 0x24c));
	con_hex2("SE13", "irq_en=", rd(SE13 + 0x614), "m_cmd0=", rd(SE13 + 0x600));
}

/* ---- bring-up: rails, pins, GENI SPI FIFO, FT3680 boot id ---- */

#include "rpmh.h"

#define SE(o)		(SE13 + (o))
#define CTL_PULL_UP	3u
#define CTL_FUNC(f)	((uint32_t)(f) << 2)
#define CTL_DRV_MA(m)	((uint32_t)((m) / 2 - 1) << 6)
#define CTL_OE		(1u << 9)
#define IO_OUT		(1u << 1)

static void udelay(unsigned us)
{
	uint64_t t, f, now;
	__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(t), "=r"(f));
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	while (now < t + f / 1000000 * us);
}

static void ts_reset(int high)
{
	wr(TLMM_GPIO(105) + 4, high ? IO_OUT : 0);
}

static unsigned tx_depth;

static uint32_t m_cmd(uint32_t op, uint32_t params)
{
	wr(SE(0x618), 0xffffffffu);			/* M_IRQ_CLEAR */
	wr(SE(0x600), op << 27 | params);		/* M_CMD0 */
	return 0;
}

static int m_done(void)
{
	for (unsigned i = 0; i < 100000; i++) {
		if (rd(SE(0x610)) & 1u)			/* M_CMD_DONE */
			return 0;
		udelay(1);
	}
	return -1;
}

static uint32_t dbg_irq, dbg_status;	/* GENI state when a step failed */

/* Stuck command: cancel, then abort (handle_fifo_timeout order). */
static void m_recover(void)
{
	dbg_irq = rd(SE(0x610)), dbg_status = rd(SE(0x40));
	wr(SE(0x618), 0xffffffffu);
	wr(SE(0x604), 1u << 2);				/* M_GENI_CMD_CANCEL */
	if (m_done()) {
		wr(SE(0x618), 0xffffffffu);
		wr(SE(0x604), 1u << 1);			/* M_GENI_CMD_ABORT */
		for (int i = 0; i < 100000 && !(rd(SE(0x610)) & (1u << 5)); i++)	/* M_CMD_ABORT */
			udelay(1);
	}
	wr(SE(0x618), 0xffffffffu);
}

/*
 * One full-duplex transfer, polled FIFO. explicit_cs = 0: a single
 * SPI_FULL_DUPLEX without FRAGMENTATION, the engine frames CS itself;
 * 1: CS_ASSERT / FULL_DUPLEX+FRAGMENTATION / CS_DEASSERT (probe 1 hung in
 * the DEASSERT).
 */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n, int explicit_cs)
{
	if (explicit_cs) {
		m_cmd(8, 0);				/* SPI_CS_ASSERT */
		if (m_done()) {
			m_recover();
			return -1;
		}
	}
	wr(SE(0x25c), rd(SE(0x25c)) & ~1u);		/* TRANS_CFG: no CS toggle */
	wr(SE(0x26c), n);				/* TX_TRANS_LEN (8-bit words) */
	wr(SE(0x270), n);				/* RX_TRANS_LEN */
	m_cmd(3, explicit_cs ? 1u << 2 : 0);		/* SPI_FULL_DUPLEX (+FRAGMENTATION) */
	unsigned ti = 0, ri = 0;
	for (unsigned spin = 0; ri < n && spin < 100000; spin++) {
		while (ti < n && (rd(SE(0x800)) & 0x0fffffffu) < tx_depth) {
			uint32_t w = 0;
			for (unsigned j = 0; j < 4 && ti < n; j++)
				w |= (uint32_t)tx[ti++] << (8 * j);
			wr(SE(0x700), w);
		}
		for (uint32_t wc = rd(SE(0x804)) & 0x1ffffffu; wc && ri < n; wc--) {
			uint32_t w = rd(SE(0x780));
			for (unsigned j = 0; j < 4 && ri < n; j++)
				rx[ri++] = (uint8_t)(w >> (8 * j));
		}
		if (ri < n)
			udelay(1);
	}
	int rc = m_done() || ri < n ? -2 : 0;
	if (rc)
		m_recover();
	else
		dbg_irq = rd(SE(0x610)), dbg_status = rd(SE(0x40));
	if (explicit_cs) {
		m_cmd(9, 0);				/* SPI_CS_DEASSERT */
		if (m_done()) {
			m_recover();
			rc = -3;
		}
	}
	udelay(150);					/* focaltech CS_HIGH_DELAY */
	return rc;
}

static uint16_t crc16_kermit(const uint8_t *d, unsigned len)
{
	uint16_t crc = 0xffff;
	for (unsigned i = 0; i < len; i++) {
		crc ^= d[i];
		for (int j = 0; j < 8; j++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

void touch_bringup(void)
{
	con_str("TOUCH", "bring-up: rails, pins, SPI, FT3680 boot id");
	/* reset low (output), irq input pull-up: DT pmx_ts_active (8 mA, pull-up) */
	ts_reset(0);
	wr(TLMM_GPIO(105), CTL_FUNC(0) | CTL_DRV_MA(8) | CTL_PULL_UP | CTL_OE);
	wr(TLMM_GPIO(81), CTL_FUNC(0) | CTL_DRV_MA(8) | CTL_PULL_UP);
	udelay(1000);
	/* focaltech_core.c power-on: iovdd, 1 ms, vdd */
	if (rpmh_vreg_enable("ldoc8", 1800) || (udelay(1000), rpmh_vreg_enable("ldoc3", 3300))) {
		con_str("TOUCH", "rail vote failed: stop");
		return;
	}
	/* SPI pins: qup15, 6 mA, no pull (DT qupv3_se13_spi_active) */
	for (int p = 52; p <= 55; p++)
		wr(TLMM_GPIO(p), CTL_FUNC(1) | CTL_DRV_MA(6));
	wr(GCC + GCC_VOTE, rd(GCC + GCC_VOTE) | 1U << 27 | 1U << 21 | 1U << 20);
	for (int t = 0; t < 100000 && (rd(GCC + 0x185fc) & (1U << 31)); t++)
		;
	if (rd(GCC + 0x185fc) & (1U << 31)) {
		con_str("TOUCH", "SE13 clock still off: stop");
		return;
	}
	/* GENI init + FIFO mode (qcom-geni-se.c geni_se_init/select_fifo_mode) */
	uint32_t hw0 = rd(SE(0xe24));
	tx_depth = (hw0 >> 16) & 0x3f;
	wr(SE(0xe18), 0);				/* GSI_EVENT_EN */
	wr(SE(0x618), 0xffffffffu);			/* M_IRQ_CLEAR */
	wr(SE(0x648), 0xffffffffu);			/* S_IRQ_CLEAR */
	wr(SE(0x28), rd(SE(0x28)) | 0x7f);		/* CGC_CTRL default */
	wr(SE(0xe30), rd(SE(0xe30)) | 0xf);		/* DMA_GENERAL_CFG clock gating on */
	wr(SE(0x24), 0x7f);				/* OUTPUT_CTRL */
	wr(SE(0x20), 1);				/* FORCE_DEFAULT */
	wr(SE(0xe1c), rd(SE(0xe1c)) | 0xf);		/* SE_IRQ_EN */
	wr(SE(0x258), rd(SE(0x258)) & ~1u);		/* FIFO mode */
	wr(SE(0x810), 0);				/* RX watermark */
	wr(SE(0x814), tx_depth - 2);			/* RX RFR watermark */
	wr(SE(0x614), rd(SE(0x614)) | 1u | 1u << 26 | 1u << 27 | 1u << 30);
	/* 8-bit words, 4 per FIFO word, MSB first (geni_se_config_packing) */
	wr(SE(0x260), 0x0007f8fe), wr(SE(0x264), 0x000ffefe);
	wr(SE(0x284), 0x0007f8fe), wr(SE(0x288), 0x000ffefe);
	wr(SE(0x254), 0);				/* BYTE_GRAN */
	wr(SE(0x268), (rd(SE(0x268)) & ~0x3ffu) | (8 - 4));	/* WORD_LEN */
	/* mode 0, CS0, 19.2 MHz / 4 = 4.8 MHz (<= DT 6 MHz) */
	wr(SE(0x22c), 0), wr(SE(0x250), 0), wr(SE(0x224), 0), wr(SE(0x230), 0), wr(SE(0x24c), 0);
	wr(SE(0x7c), 0);
	wr(SE(0x48), 4u << 4 | 1u);
	con_hex2("SE13", "hw0=", hw0, "depth=", tx_depth);
	/* GENI_IF_DISABLE_RO bit 0 FIFO_IF_DISABLE: firmware in GSI mode (spi-msm-geni.c gsi_mode) */
	uint32_t ifdis = rd(SE(0x64));
	con_hex2("SE13", "if_dis=", ifdis, "gp_len=", rd(SE(0x644)));
	if (ifdis & 1) {
		/*
		 * Firmware set up for GSI. Mainline geni_load_se_fw() picks FIFO
		 * mode by clearing SE_FIFO_IF_DISABLE (0x2008, writable; 0x64 is
		 * its read-only mirror) with DMA_IF_EN (0x2004) set; the protocol
		 * firmware itself is the same. Try that without reloading it.
		 */
		/* touch4: even reading SE+0x2004 is an external abort (XPU, TZ-owned). */
		con_str("SE13", "GSI mode (FIFO switch is TZ-only): needs GPI DMA");
		return;
	}

	/* focaltech_flash.c fts_enter_into_boot: reset, 0x55, read boot id 0x90 */
	for (int i = 0; i < 3; i++) {
		ts_reset(0);
		udelay(1000);
		ts_reset(1);
		udelay((8 + i) * 1000);
		static const uint8_t start1[4] = { 0x55, 0x00, 0x00, 0x00 };
		uint8_t rx[16] = { 0 };
		int cs = i == 2;
		int rc = spi_xfer(start1, rx, sizeof(start1), cs);
		uint32_t irq1 = dbg_irq, st1 = dbg_status;
		udelay(8000);
		static const uint8_t rid[11] = { 0x90, 0xa0, 0x00, 0x02 };
		uint8_t id[11] = { 0 };
		int rc2 = spi_xfer(rid, id, sizeof(rid), cs);
		uint16_t crc = crc16_kermit(&id[7], 2), got = (uint16_t)(id[10] << 8 | id[9]);
		con_hex2(cs ? "55 csx" : "55 auto", "rc=", (uint32_t)rc, "st=", rx[3]);
		con_hex2(" geni", "irq=", irq1, "status=", st1);
		con_hex2("FT 0x90", "rc=", (uint32_t)rc2, "st=", id[3]);
		con_hex2(" geni", "irq=", dbg_irq, "status=", dbg_status);
		con_hex2("FT ID", "id=", id[7] << 8 | id[8], "crc ok=", crc == got);
		if (rc2 == 0 && crc == got && id[7] == 0x56 && id[8] == 0x62) {
			con_str("FT3680", "boot id 0x5662: SPI link works");
			return;
		}
	}
	con_str("FT3680", "no valid boot id");
}

/* ---- GPI DMA + SMMU state (read-only) ---- */

/*
 * gpi.c: ee_base = gpi-top - qcom,gpi-ee-offset (subtracted). GPII 0 then
 * sits at 0xa10000, one of the DT trusted-touch-io-bases (LE VM). The first
 * probe added the offset and hit GPII 9 (external abort at 0xa34000).
 */
#define GPI_EE		(0x00a00000UL - 0x10000UL)
#define SMMU		0x15000000UL			/* apps_smmu */

void gpi_smmu_probe(void)
{
	con_str("GPI", "probe (read-only): gpi_dma1 GPII 1-4, apps_smmu");
	/* GPII 0 is static/LE-VM (qcom,static-gpii-mask 1): not touched */
	for (unsigned n = 1; n <= 4; n++) {
		uintptr_t g = GPI_EE + 0x4000UL * n;
		char l[] = "GPII n";
		l[5] = '0' + n;
		/* CH_k_CNTXT_0 [23:20] state, [3:0] protocol; EV_k_CNTXT_0 [23:20] state */
		con_hex2(l, "ch0=", rd(g + 0x20000), "ch1=", rd(g + 0x20080));
		con_hex2(" ev0", "ctx=", rd(g + 0x21000), "irq=", rd(g + 0x23080));
	}
	uint32_t scr0 = rd(SMMU), id0 = rd(SMMU + 0x20);
	unsigned nsmr = id0 & 0xff;
	con_hex2("SMMU", "scr0=", scr0, "id0=", id0);
	static const uint16_t sids[2] = { 0x56, 0x43 };	/* GPI, qupv3_1 wrapper */
	for (int j = 0; j < 2; j++) {
		int hits = 0;
		for (unsigned i = 0; i < nsmr && i < 256; i++) {
			uint32_t smr = rd(SMMU + 0x800 + 4 * i);
			uint32_t id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;
			if (!(smr & (1u << 31)) || ((id ^ sids[j]) & ~mask & 0x7fff))
				continue;
			char l[] = "SID xx";
			l[4] = "0123456789abcdef"[sids[j] >> 4], l[5] = "0123456789abcdef"[sids[j] & 15];
			/* S2CR [17:16] type: 0 translate, 1 bypass, 2 fault; [7:0] cbndx */
			con_hex2(l, "smr=", smr, "s2cr=", rd(SMMU + 0xc00 + 4 * i));
			hits++;
		}
		if (!hits) {
			char l[] = "SID xx";
			l[4] = "0123456789abcdef"[sids[j] >> 4], l[5] = "0123456789abcdef"[sids[j] & 15];
			con_hex2(l, "no SMR, usfcfg=", (scr0 >> 10) & 1, 0, 0);
		}
	}

	/*
	 * USFCFG=1 faults SID 0x56 (GPI). Map it to bypass in a free stream
	 * match group, arm-smmu.c order: S2CR first, then the valid SMR.
	 * Under the Qualcomm hypervisor these writes may be emulated or refused.
	 */
	int free_i = -1;
	for (unsigned i = 0; i < nsmr && i < 256 && free_i < 0; i++)
		if (!(rd(SMMU + 0x800 + 4 * i) & (1u << 31)))
			free_i = (int)i;
	if (free_i < 0) {
		con_str("SMMU", "no free SMR");
		return;
	}
	uintptr_t s2cr = SMMU + 0xc00 + 4 * (unsigned)free_i, smr = SMMU + 0x800 + 4 * (unsigned)free_i;
	con_hex2("SMMU", "free smr=", (uint32_t)free_i, "old s2cr=", rd(s2cr));
	wr(s2cr, 1u << 16);			/* S2CR TYPE = bypass */
	wr(smr, 1u << 31 | 0x56);		/* SMR VALID, mask 0, id 0x56 */
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("SMMU", "smr now=", rd(smr), "s2cr now=", rd(s2cr));
}
