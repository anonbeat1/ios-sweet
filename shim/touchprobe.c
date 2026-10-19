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

static int touch_gsi_init(void);
static int gsi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n);
static int use_gsi;

static int ts_xfer(const uint8_t *tx, uint8_t *rx, unsigned n, int explicit_cs)
{
	return use_gsi ? gsi_xfer(tx, rx, n) : spi_xfer(tx, rx, n, explicit_cs);
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
		con_str("SE13", "GSI mode (FIFO switch is TZ-only): GPI DMA");
		if (touch_gsi_init())
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
		int rc = ts_xfer(start1, rx, sizeof(start1), cs);
		uint32_t irq1 = dbg_irq, st1 = dbg_status;
		udelay(8000);
		static const uint8_t rid[11] = { 0x90, 0xa0, 0x00, 0x02 };
		uint8_t id[11] = { 0 };
		int rc2 = ts_xfer(rid, id, sizeof(rid), cs);
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

/* ---- GSI path: apps_smmu context bank + polled GPI DMA (GPII 1) ---- */

/*
 * SE13 runs GSI firmware and TZ owns its FIFO switch, so transfers go through
 * gpi_dma1. Its stream (SID 0x56) faults by default and the hypervisor turns
 * an S2CR bypass into fault (a6d4d5a), so give it a stage-1 context bank with
 * an identity map of DRAM, as Linux arm-smmu does. ID0 shows no stage 2
 * (S1TS only): the hypervisor's own stage 2 sits behind it.
 * GPI programming follows gpi.c (gpi_config MSM_GPI_INIT, polled instead of
 * the IRQ), TREs follow spi-msm-geni.c setup_gsi_xfer.
 */
#define SMMU		0x15000000UL			/* apps_smmu */
#define GPI_SID		0x56
#define GPI_EE		(0x00a00000UL - 0x10000UL)	/* gpi-top - ee-offset */
/*
 * SE13's dmas cells end in 2 = STATIC_GPII_BMSK (msm_gpi.h): Linux takes it
 * from qcom,static-gpii-mask 0x1, i.e. GPII 0. On GPII 1 (touch5-9) NOP and
 * CONFIG0 completed but a GO never reached the SE (or wedged the bus).
 */
#define GPII		0UL
#define G(o)		(GPI_EE + (o) + 0x4000UL * GPII)
#define CH_CTX(k)	G(0x20000 + 0x80 * (k))
#define CH_DB(k)	G(0x22000 + 0x8 * (k))
#define EV_CTX		G(0x21000)
#define EV_DB		G(0x22100)
#define SE_SEID		5				/* SE13 = wrapper 1 SE5 */
#define NTRE		64				/* dmas <... 0x40 ...> */

struct tre { uint32_t d[4]; };

static struct tre tx_ring[NTRE] __attribute__((aligned(NTRE * 16)));
static struct tre rx_ring[NTRE] __attribute__((aligned(NTRE * 16)));
static struct tre ev_ring[NTRE] __attribute__((aligned(NTRE * 16)));
static uint8_t dma_tx[64] __attribute__((aligned(64))), dma_rx[64] __attribute__((aligned(64)));
static uint64_t smmu_l1[512] __attribute__((aligned(4096)));
static unsigned tx_wp, rx_wp, ev_rp, gsi_cfg_done;
static int smmu_cb = -1;
static uintptr_t smmu_cbase;

/* GPI is not cache coherent (no dma-coherent in its node): clean / invalidate by line */
static void dc_civac(const void *p, unsigned len)
{
	for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline void wr64(uintptr_t a, uint64_t v) { *(volatile uint64_t *)a = v; }

static int smmu_setup(void)
{
	uint32_t id0 = rd(SMMU + 0x20), id1 = rd(SMMU + 0x24);
	unsigned pgshift = (id1 >> 31) ? 16 : 12, ncb = id1 & 0xff, ns2cb = (id1 >> 16) & 0xff;
	unsigned nsmr = id0 & 0xff;
	uintptr_t gr1 = SMMU + (1UL << pgshift);
	smmu_cbase = SMMU + ((1UL << (((id1 >> 28) & 7) + 1)) << pgshift);
	con_hex2("SMMU", "id1=", id1, "cb0=", (uint32_t)smmu_cbase);

	/* context banks in use by a valid translating stream match group */
	uint32_t used[8] = { 0 };
	int free_smr = -1;
	for (unsigned i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd(SMMU + 0x800 + 4 * i), s2cr = rd(SMMU + 0xc00 + 4 * i);
		if (!(smr & (1u << 31))) {
			if (free_smr < 0)
				free_smr = (int)i;
			continue;
		}
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 32] |= 1u << ((s2cr & 0xff) % 32);
	}
	for (unsigned c = ns2cb; c < ncb && smmu_cb < 0; c++)
		if (!(used[c / 32] & (1u << (c % 32))) && !(rd(smmu_cbase + ((uintptr_t)c << pgshift)) & 1))
			smmu_cb = (int)c;
	con_hex2("SMMU", "free cb=", (uint32_t)smmu_cb, "free smr=", (uint32_t)free_smr);
	if (smmu_cb < 0 || free_smr < 0)
		return -1;

	/* 39-bit VA, 4K granule, level-1 1 GiB blocks: DRAM 0x80000000-0x27fffffff, Normal NC, RW */
	for (unsigned i = 2; i < 10; i++)
		smmu_l1[i] = (uint64_t)i << 30 | 1u << 10 | 2u << 8 | 1u << 6 | 0u << 2 | 1;
	dc_civac(smmu_l1, sizeof(smmu_l1));

	uintptr_t cb = smmu_cbase + ((uintptr_t)smmu_cb << pgshift);
	unsigned asid = (unsigned)smmu_cb;
	wr(cb + 0x0, 0);					/* SCTLR off while programming */
	wr(cb + 0x58, 0xffffffffu);				/* FSR clear */
	wr(gr1 + 0x800 + 4 * (unsigned)smmu_cb, 1);		/* CBA2R VA64 */
	wr(gr1 + 4 * (unsigned)smmu_cb, 1u << 16 | 0xfu << 12 | 3u << 8);	/* CBAR S1 + S2 bypass, WB, NSH */
	wr(cb + 0x10, 7u << 15 | 2);				/* TCR2: SEP upstream, PASize 40 bit */
	wr(cb + 0x30, 25 | 1u << 23);				/* TCR: T0SZ 25, TG0 4K, NC walks, EPD1 */
	wr(cb + 0x34, asid);					/* CONTEXTIDR */
	wr64(cb + 0x20, (uintptr_t)smmu_l1 | (uint64_t)asid << 48);
	wr64(cb + 0x28, 0);
	wr(cb + 0x38, 0x44);					/* MAIR0 attr0 Normal NC */
	wr(cb + 0x618, 0);					/* TLBIALL */
	wr(cb + 0x7f0, 0);					/* TLBSYNC */
	for (int t = 0; t < 100000 && (rd(cb + 0x7f4) & 1); t++)
		udelay(1);
	wr(cb + 0x0, 1u << 6 | 1u << 5 | 1u << 2 | 1u << 1 | 1);	/* CFIE CFRE AFE TRE M */
	uintptr_t s2cr = SMMU + 0xc00 + 4 * (unsigned)free_smr, smr = SMMU + 0x800 + 4 * (unsigned)free_smr;
	wr(s2cr, (uint32_t)smmu_cb);				/* TYPE translate, cbndx */
	wr(smr, 1u << 31 | GPI_SID);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2(" cb", "sctlr=", rd(cb), "cbar=", rd(gr1 + 4 * (unsigned)smmu_cb));
	con_hex2(" cb", "tcr=", rd(cb + 0x30), "ttbr0=", rd(cb + 0x20));
	con_hex2(" sid56", "smr=", rd(smr), "s2cr=", rd(s2cr));
	return ((rd(s2cr) >> 16) & 3) == 0 && (rd(s2cr) & 0xff) == (uint32_t)smmu_cb && (rd(cb) & 1) ? 0 : -1;
}

static void smmu_fault_report(void)
{
	if (smmu_cb < 0)
		return;
	uint32_t id1 = rd(SMMU + 0x24);
	uintptr_t cb = smmu_cbase + ((uintptr_t)smmu_cb << ((id1 >> 31) ? 16 : 12));
	con_hex2("", "fsr=", rd(cb + 0x58), "gfsr=", rd(SMMU + 0x48));
}

/* gpi_send_cmd, polled: wait for the context state field [23:20] */
static int gpi_cmd(uintptr_t reg, uint32_t cmd, uintptr_t ctx, uint32_t state, const char *what)
{
	wr(reg, cmd);
	for (int t = 0; t < 250000; t++) {
		if (((rd(ctx) >> 20) & 0xf) == state) {
			wr(G(0x230a0), rd(G(0x23090)));		/* SRC_CH_IRQ_CLR */
			wr(G(0x230a4), rd(G(0x23094)));		/* SRC_EV_CH_IRQ_CLR */
			return 0;
		}
		udelay(1);
	}
	con_hex2(what, "ctx=", rd(ctx), "irq=", rd(G(0x23080)));
	con_hex2(" gpii", "glob=", rd(G(0x23100)), "err=", rd(G(0x23200)));
	return -1;
}

static int gpi_setup(void)
{
	/* geni_se_select_gsi_mode (msm-geni-se.c) */
	wr(SE(0xe1c), 0);					/* SE_IRQ_EN */
	wr(SE(0x644), rd(SE(0x644)) & ~1u);			/* S_IRQ_EN: S_CMD_DONE off */
	wr(SE(0x614), rd(SE(0x614)) & ~(1u | 1u << 26 | 1u << 27 | 1u << 30));
	wr(SE(0x618), 0xffffffffu), wr(SE(0x648), 0xffffffffu);
	wr(SE(0xc44), 0xffffffffu), wr(SE(0xd44), 0xffffffffu);
	wr(SE(0x258), rd(SE(0x258)) | 1);			/* GENI_DMA_MODE_EN */
	wr(SE(0xe18), rd(SE(0xe18)) | 0xf);			/* GSI_EVENT_EN: DMA rx/tx, M, S */

	/* gpi_config_interrupts DEFAULT_IRQ_SETTINGS (nothing wired to the GIC: we poll) */
	wr(G(0x23088), 0x4f), wr(G(0x230b8), 1), wr(G(0x23098), 3), wr(G(0x2309c), 1);
	wr(G(0x23108), 1), wr(G(0x23120), 0xf);
	wr(G(0x23188), 0), wr(G(0x2318c), 0), wr(G(0x23400), 0), wr(G(0x23404), 0);
	wr(G(0x23180), 1), wr(G(0x23200), 0);

	for (unsigned i = 0; i < NTRE; i++)
		tx_ring[i] = rx_ring[i] = ev_ring[i] = (struct tre){ { 0 } };
	dc_civac(tx_ring, sizeof(tx_ring)), dc_civac(rx_ring, sizeof(rx_ring)), dc_civac(ev_ring, sizeof(ev_ring));

	/* GPII 0 is the static (LE-VM) one: only take it if nobody allocated it */
	con_hex2("GPII 0", "was ch0=", rd(CH_CTX(0)), "ev=", rd(EV_CTX));
	if ((rd(CH_CTX(0)) | rd(CH_CTX(1)) | rd(EV_CTX)) & 0x00f00000u) {
		con_str("GPII 0", "already allocated: stop");
		return -1;
	}

	/* gpi_alloc_ev_chan: ALLOCATE, then program the context */
	if (gpi_cmd(G(0x23010), 0, EV_CTX, 1, "EV ALLOC"))
		return -1;
	wr(EV_CTX + 0x0, 16u << 24 | 1u << 16 | 2);		/* el 16, IRQ, GPI_EV */
	wr(EV_CTX + 0x4, sizeof(ev_ring));
	wr(EV_CTX + 0x8, (uint32_t)(uintptr_t)ev_ring), wr(EV_CTX + 0xc, (uint32_t)((uintptr_t)ev_ring >> 32));
	wr(EV_DB + 0x4, (uint32_t)((uintptr_t)ev_ring >> 32));
	wr(EV_CTX + 0x20, 0), wr(EV_CTX + 0x28, 0), wr(EV_CTX + 0x2c, 0);
	wr(EV_CTX + 0x30, 0), wr(EV_CTX + 0x34, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(EV_DB, (uint32_t)(uintptr_t)&ev_ring[NTRE - 1]);	/* hand all but one element to HW */

	/* gpi_alloc_chan x2: ch0 TX (dir out), ch1 RX (dir in), SPI on SE5 */
	struct tre *rings[2] = { tx_ring, rx_ring };
	for (unsigned k = 0; k < 2; k++) {
		if (gpi_cmd(G(0x23008), k, CH_CTX(k), 1, k ? "CH1 ALLOC" : "CH0 ALLOC"))
			return -1;
		uintptr_t r = (uintptr_t)rings[k];
		wr(CH_CTX(k) + 0x0, 16u << 24 | (k ? 0u : 1u) << 3 | 2);
		wr(CH_CTX(k) + 0x4, NTRE * 16);
		wr(CH_CTX(k) + 0x8, (uint32_t)r), wr(CH_CTX(k) + 0xc, (uint32_t)(r >> 32));
		wr(CH_DB(k) + 0x4, (uint32_t)(r >> 32));
		wr(CH_CTX(k) + 0x60, (k ? 0u : 1u) << 16 | 1u << 4 | SE_SEID);	/* SCRATCH_0 pair, proto SPI */
		wr(CH_CTX(k) + 0x64, 0), wr(CH_CTX(k) + 0x68, 0), wr(CH_CTX(k) + 0x6c, 0);
		wr(CH_CTX(k) + 0x5c, 1);				/* QOS */
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned k = 0; k < 2; k++)
		if (gpi_cmd(G(0x23008), 1u << 24 | k, CH_CTX(k), 2, k ? "CH1 START" : "CH0 START"))
			return -1;
	con_hex2("GPII 0", "ch0=", rd(CH_CTX(0)), "ch1=", rd(CH_CTX(1)));
	con_hex2(" ev", "ctx=", rd(EV_CTX), "rp=", rd(EV_CTX + 0x10));
	return 0;
}

static void ring_put(struct tre *ring, unsigned *wp, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	ring[*wp] = (struct tre){ { d0, d1, d2, d3 } };
	dc_civac(&ring[*wp], 16);
	*wp = (*wp + 1) % NTRE;
}

/*
 * One full-duplex SPI transfer (<= 64 bytes): TX ring CONFIG0 (first time),
 * GO, DMA tx; RX ring DMA rx. Done when the RX channel's read pointer
 * reaches its write pointer. Event ring printed on failure.
 */
static uint32_t gsi_dbg[4];

static void gsi_dump(const char *what)
{
	dc_civac(ev_ring, sizeof(ev_ring));
	struct tre *e = ev_rp ? &ev_ring[ev_rp - 1] : &ev_ring[0];
	con_hex2(what, "ch0 rp=", rd(CH_CTX(0) + 0x10), "ch1 rp=", rd(CH_CTX(1) + 0x10));
	con_hex2("", "ev rp=", rd(EV_CTX + 0x10), "irq=", rd(G(0x23080)));
	con_hex2("", "ev d2=", e->d[2], "d3=", e->d[3]);
	con_hex2("", "err=", rd(G(0x23200)), "gl|gen=", rd(G(0x23100)) << 16 | rd(G(0x23118)));
	con_hex2("", "geni=", rd(SE(0x40)), "m_irq=", rd(SE(0x610)));
	con_hex2("", "m_cmd0=", rd(SE(0x600)), "dmatx=", rd(SE(0xc40)));
	smmu_fault_report();
}

static int gsi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		dma_tx[i] = tx[i], dma_rx[i] = 0xee;
	dc_civac(dma_tx, sizeof(dma_tx)), dc_civac(dma_rx, sizeof(dma_rx));

	ring_put(rx_ring, &rx_wp, (uint32_t)(uintptr_t)dma_rx, 0, n, 1u << 20);	/* DMA_W_BUF */
	if (!gsi_cfg_done) {
		/* CONFIG0: tx/rx pack, mode 0, 8-bit words; clk idx 0 (XO 19.2 MHz) / 4 = 4.8 MHz; chain */
		ring_put(tx_ring, &tx_wp, 3u << 24 | (8 - 4), 0, 0u << 16 | 4, 2u << 20 | 2u << 16 | 1);
		gsi_cfg_done = 1;
	}
	ring_put(tx_ring, &tx_wp, 3, 0, n, 2u << 20 | 1u << 11 | 1);	/* GO full duplex, cs 0, link_rx, chain */
	ring_put(tx_ring, &tx_wp, (uint32_t)(uintptr_t)dma_tx, 0, n, 1u << 20 | 1u << 9);	/* DMA_W_BUF, ieot */
	wr(CH_DB(1), (uint32_t)(uintptr_t)&rx_ring[rx_wp]);
	wr(CH_DB(0), (uint32_t)(uintptr_t)&tx_ring[tx_wp]);

	uint32_t want = (uint32_t)(uintptr_t)&rx_ring[rx_wp];
	int rc = -1;
	for (int t = 0; t < 100000; t++) {
		if (rd(CH_CTX(1) + 0x10) == want && rd(CH_CTX(0) + 0x10) == (uint32_t)(uintptr_t)&tx_ring[tx_wp]) {
			rc = 0;
			break;
		}
		udelay(1);
	}
	udelay(150);						/* focaltech CS_HIGH_DELAY */
	dc_civac(dma_rx, sizeof(dma_rx));
	for (unsigned i = 0; i < n; i++)
		rx[i] = dma_rx[i];

	/* consume events (xfer completions), remember the last one */
	dc_civac(ev_ring, sizeof(ev_ring));
	uint32_t hw_rp = rd(EV_CTX + 0x10);
	while ((uint32_t)(uintptr_t)&ev_ring[ev_rp] != hw_rp && ev_rp < NTRE - 1) {
		for (int j = 0; j < 4; j++)
			gsi_dbg[j] = ev_ring[ev_rp].d[j];
		ev_rp++;
	}
	if (rc)
		gsi_dump("timeout");
	con_hex2(" ev", "d2=", gsi_dbg[2], "d3=", gsi_dbg[3]);
	return rc;
}

/*
 * touch6: the RX doorbell is harmless, the TX doorbell (CONFIG0 + GO + DMA)
 * ends in CrashDump. Split it: a NOP TRE (GPI fetch + event write, no SE),
 * then CONFIG0 alone (SE programming, no bus traffic), each with IEOB.
 */
static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	return t;
}

/*
 * touch8 t1: the TX-only GO resets the SoC within 2 s with nothing on
 * screen. Poll the GPI/SE state and print a line (time in us since the
 * doorbell) whenever it changes, to see how far the transfer gets.
 */
static void gsi_trace(unsigned us)
{
	uint32_t last[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
	uint64_t t0 = ticks(), end = t0 + 192ULL * us / 10;
	int lines = 0;
	while (ticks() < end && lines < 2) {
		/* touch8 t1 froze without a dump: mark each register block before its first read */
		if (lines == 0 && last[0] == ~0u)
			con_str(" read", "GPI regs");
		uint32_t v[6] = { rd(CH_CTX(0) + 0x10), rd(EV_CTX + 0x10), rd(G(0x23080)) << 16 | rd(G(0x23118)) };
		if (lines == 0 && last[0] == ~0u)
			con_str(" read", "SE13 regs");
		v[3] = rd(SE(0x40)), v[4] = rd(SE(0x610)), v[5] = rd(SE(0xc40));
		int changed = 0;
		for (int i = 0; i < 6; i++)
			changed |= v[i] != last[i], last[i] = v[i];
		if (changed) {
			uint32_t dt = (uint32_t)((ticks() - t0) * 10 / 192);
			con_hex2(" t us", "", dt, "rp=", v[0]);
			con_hex2("", "ev=", v[1], "ir|gn=", v[2]);
			con_hex2("", "geni=", v[3], "m_irq=", v[4]);
			lines++;
		}
	}
}

static void gsi_step(const char *what, unsigned n, const struct tre *t)
{
	con_str(what, "TX doorbell in 3 s");
	udelay(3000000);
	for (unsigned i = 0; i < n; i++)
		ring_put(tx_ring, &tx_wp, t[i].d[0], t[i].d[1], t[i].d[2], t[i].d[3]);
	wr(CH_DB(0), (uint32_t)(uintptr_t)&tx_ring[tx_wp]);
	if (n > 1)
		gsi_trace(2000000);	/* the GO steps only: screen space */
	else
		udelay(2000000);
	dc_civac(ev_ring, sizeof(ev_ring));
	while ((uint32_t)(uintptr_t)&ev_ring[ev_rp] != rd(EV_CTX + 0x10) && ev_rp < NTRE - 1)
		ev_rp++;
	/* gpi_process_events: give the consumed elements back, clear IEOB */
	wr(EV_DB, (uint32_t)(uintptr_t)&ev_ring[(ev_rp + NTRE - 1) % NTRE]);
	wr(G(0x230c0), 1);
	gsi_dump(" ->");
}

/* gpi_dump_debug_reg: microcontroller PC, SW_RF, QSB log (gpi-top relative) */
#define GPI_TOP		0x00a00000UL
static void gpi_debug_dump(void)
{
	con_hex2("GPI dbg", "pc=", rd(GPI_TOP + 0x5048), "pc=", rd(GPI_TOP + 0x5048));
	con_hex2("", "pc=", rd(GPI_TOP + 0x5048), "pc=", rd(GPI_TOP + 0x5048));
	con_hex2("", "rf10=", rd(GPI_TOP + 0x5100 + 40), "rf11=", rd(GPI_TOP + 0x5100 + 44));
	con_hex2("", "rf12=", rd(GPI_TOP + 0x5100 + 48), "rf21=", rd(GPI_TOP + 0x5100 + 84));
	con_hex2("", "qsb0=", rd(GPI_TOP + 0x5064), "qsb1=", rd(GPI_TOP + 0x5068));
	con_hex2("", "qsb2=", rd(GPI_TOP + 0x506c), "errid=", rd(GPI_TOP + 0x5060));
	con_hex2("", "ch0 wp=", rd(CH_CTX(0) + 0x18), "ch0 c1=", rd(CH_CTX(0) + 0x4));
}

/*
 * touch7: NOP and CONFIG0 TREs complete (EOB events: GPI DMA through the
 * SMMU works), but a full-duplex GO + DMA after them stays pending (ch0 RP
 * stuck on the GO, GENI idle) while touch6's chained CONFIG0 + GO + DMA
 * crashed. TOUCH_TEST picks one TX-only 0x55 write per boot (after a NOP):
 *   2: CONFIG0 + GO + DMA chained     3: GO + DMA, no CONFIG0
 * (1, CONFIG0 alone then GO, and a LOCK after it: both stall, touch10-13)
 */
#ifndef TOUCH_TEST
#define TOUCH_TEST 2
#endif
/* touch11: div 8 so a CONFIG0 that reaches the SE shows up as M_CLK_CFG 0x81 (we left 0x41) */
#define TRE_CONFIG0(chain)	{ { 3u << 24 | (8 - 4), 0, 8, 2u << 20 | 2u << 16 | (chain ? 1u : 1u << 8) } }
#define TRE_GO_TX(n)		{ { 1, 0, 0, 2u << 20 | 1 } }		/* SPI_TX_ONLY, cs 0, chain */
#define TRE_DMA_TX(n)		{ { (uint32_t)(uintptr_t)dma_tx, 0, n, 1u << 20 | 1u << 9 } }

static void gsi_tests(void)
{
	static const struct tre nop[1] = { { { 0, 0, 0, 1u << 16 | 1u << 8 } } };
	gsi_step("NOP", 1, nop);
	dma_tx[0] = 0x55, dma_tx[1] = dma_tx[2] = dma_tx[3] = 0;
	dc_civac(dma_tx, sizeof(dma_tx));
	ts_reset(0);
	udelay(1000);
	ts_reset(1);
	udelay(8000);
	/*
	 * touch13: a LOCK after the standalone CONFIG0 stalled like the GO
	 * (PC 0x547). Linux never sends CONFIG0 alone: it always chains it into
	 * the GO. 2: CONFIG0+GO+DMA chained; 3: GO+DMA only (the FIFO-mode init
	 * already left 8-bit words and the clock divider in the SE).
	 */
	con_str("TOUCH", "page 2 in 5 s");
	udelay(5000000);
	con_clear(BLUE);
	con_hex2("TOUCH", "page 2: ch0 rp=", rd(CH_CTX(0) + 0x10), "test=", TOUCH_TEST);
#if TOUCH_TEST == 2
	const struct tre all[3] = { TRE_CONFIG0(1), TRE_GO_TX(4), TRE_DMA_TX(4) };
	gsi_step("C0+GO+DMA", 3, all);
#else
	const struct tre go[2] = { TRE_GO_TX(4), TRE_DMA_TX(4) };
	gsi_step("GO+DMA", 2, go);
#endif
	if (rd(CH_CTX(0) + 0x10) != (uint32_t)(uintptr_t)&tx_ring[tx_wp]) {
		gpi_debug_dump();
		con_str("TOUCH", "GO still pending: stop");
		return;
	}
	/* 0x55 went out: read the boot id (fts_check_bootid) full duplex */
	gsi_cfg_done = 1;
	udelay(8000);
	static const uint8_t rid[11] = { 0x90, 0xa0, 0x00, 0x02 };
	uint8_t id[11] = { 0 };
	int rc = gsi_xfer(rid, id, sizeof(rid));
	uint16_t crc = crc16_kermit(&id[7], 2), got = (uint16_t)(id[10] << 8 | id[9]);
	con_hex2("FT 0x90", "rc=", (uint32_t)rc, "st=", id[3]);
	con_hex2("FT ID", "id=", id[7] << 8 | id[8], "crc ok=", crc == got);
	if (rc == 0 && crc == got && id[7] == 0x56 && id[8] == 0x62)
		con_str("FT3680", "boot id 0x5662: SPI over GPI works");
}

/* Returns 0 when transfers can go through the GPI. */
static int touch_gsi_init(void)
{
	con_clear(BLUE);
	con_str("TOUCH", "GSI path: SMMU cb + GPI DMA, GPII 0");
	if (smmu_setup()) {
		con_str("SMMU", "context bank not accepted: stop");
		return -1;
	}
	if (gpi_setup()) {
		con_str("GPI", "channel setup failed: stop");
		smmu_fault_report();
		return -1;
	}
	gsi_tests();
	return -1;	/* the boot-id loop waits for a working GO */
}
