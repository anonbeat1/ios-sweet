/*
 * Spacewar GPU probe 1 (Adreno 642L = mainline "qcom,adreno-635.0", chip id
 * 0x06030500, family A6XX_GEN4; firmware a660_sqe.fw / a660_gmu.bin /
 * a660_zap.mbn). Addresses from mainline kodiak.dtsi (gpu@3d00000,
 * gmu@3d6a000, gpucc@3d90000, adreno_smmu@3da0000) and gcc-sc7280.c /
 * gpucc-sc7280.c; NOT yet compared with the stock Spacewar DT.
 *
 * Page 1 (read-only): GCC GPU branches, then (after turning on
 * GCC_GPU_CFG_AHB_CLK, which Linux keeps always on) GPU_CC GDSCs, branches
 * and RCGs. Page 2: GCC GPU bus clocks + GPLL0 votes, CX GDSC on, GPU_CC CX
 * branches on (as the GMU's clock list in kodiak.dtsi), then GMU CX/AO
 * registers. The GX domain (GX GDSC, RBBM, CP) is left alone: the GMU
 * firmware powers it (probe 2). GMU registers are only read once the CX GDSC
 * reports PWR_ON and the AHB/GMU clocks run: an unclocked access can hang the
 * bus.
 */
#include <stdint.h>
#include "console.h"
#include "gpuprobe.h"

#define GCC		0x00100000UL
#define GCC_GPU_CFG_AHB	0x71004
#define GCC_GPU_MEMNOC	0x7100c		/* halt voted */
#define GCC_GPU_SNOC_DVM 0x71018
#define GCC_DDRSS_GPU_AXI 0x71154	/* halt skip */
#define GCC_VOTE	0x52000		/* bit 15 gpu_gpll0, 16 gpu_gpll0_div */

#define GPUCC		0x03d90000UL
#define GX_GDSCR	0x100c
#define CX_GDSCR	0x106c
#define CX_GDS_HW_CTRL	0x1540
#define GX_CLAMP_IO	0x1508
#define CC_CXO_AON	0x1004
#define CC_GX_GMU	0x1064
#define CC_AHB		0x1078
#define CC_CRC_AHB	0x107c
#define CC_SNOC_DVM	0x108c
#define CC_SLEEP	0x1090
#define CC_CX_GMU	0x1098
#define CC_CXO		0x109c
#define CC_GMU_RCG	0x1120		/* CMD, CFG at +4 */
#define CC_CB		0x1170
#define CC_HUB_AON	0x1178
#define CC_HUB_RCG	0x117c
#define CC_HUB_CX_INT	0x1204
#define CC_SMMU_VOTE	0x5000

/* GMU registers: dword offsets from the GPU base (a6xx_gmu.xml) */
#define GPU		0x03d00000UL
#define GMU(r)		(GPU + 4UL * (r))
#define GMU_CM3_SYSRESET	0x1f800
#define GMU_CM3_FW_INIT_RESULT	0x1f81c
#define GMU_RPMH_POWER_STATE	0x1f8ec
#define GMU_SPTPRAC_PWR_CLK	0x1f8d0
#define GMU_PWR_COL_KEEPALIVE	0x1f8c3
#define GMU_HFI_CTRL_STATUS	0x1f980
#define GMU_GENERAL_0		0x1f9c5
#define GMU_GENERAL_7		0x1f9cc
#define GMU_AO_HOST_IRQ_STATUS	0x23b05
#define GPU_CC_GX_DOMAIN_MISC	0x24542

#define CBCR_EN		(1u << 0)
#define CBCR_OFF	(1u << 31)
#define GDSC_COLLAPSE	(1u << 0)
#define GDSC_RETAIN_FF	(1u << 11)
#define GDSC_PWR_ON	(1u << 31)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static void udelay(unsigned us)
{
	uint64_t t, f, now;
	__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(t), "=r"(f));
	uint64_t end = t + f / 1000000 * us;
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	while (now < end);
}

/* poll until (reg & mask) == want; 1 ok, 0 timeout */
static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if ((rd(a) & mask) == want)
			return 1;
		udelay(1);
	}
	return (rd(a) & mask) == want;
}

/* branch on, wait for CLK_OFF to clear (halt check skipped when !check) */
static int branch_on(uintptr_t a, int check)
{
	wr(a, rd(a) | CBCR_EN);
	return check ? poll(a, CBCR_OFF, 0, 200) : 1;
}

static void show2(const char *l, const char *na, uintptr_t a, const char *nb, uintptr_t b)
{
	con_hex2(l, na, rd(a), nb, b ? rd(b) : 0);
}

void gpu_probe(void)
{
	con_str("GPU", "probe 1: clocks, CX GDSC, GMU regs");
	show2("GCC", "cfgahb=", GCC + GCC_GPU_CFG_AHB, "memnoc=", GCC + GCC_GPU_MEMNOC);
	show2("GCC", "snocdvm=", GCC + GCC_GPU_SNOC_DVM, "ddraxi=", GCC + GCC_DDRSS_GPU_AXI);
	show2("GCC", "vote=", GCC + GCC_VOTE, 0, 0);

	int ok = branch_on(GCC + GCC_GPU_CFG_AHB, 1);
	con_hex2("GCC", "cfgahb on=", ok, "now=", rd(GCC + GCC_GPU_CFG_AHB));
	if (!ok) {
		con_str("GPU", "GPU_CC config clock stuck off, stop");
		return;
	}
	show2("GDSC", "cx=", GPUCC + CX_GDSCR, "gx=", GPUCC + GX_GDSCR);
	show2("GDSC", "cxhw=", GPUCC + CX_GDS_HW_CTRL, "gxclamp=", GPUCC + GX_CLAMP_IO);
	show2("CC", "ahb=", GPUCC + CC_AHB, "cxo=", GPUCC + CC_CXO);
	show2("CC", "cxgmu=", GPUCC + CC_CX_GMU, "hubcx=", GPUCC + CC_HUB_CX_INT);
	show2("CC", "smmu=", GPUCC + CC_SMMU_VOTE, "cb=", GPUCC + CC_CB);
	show2("CC", "gxgmu=", GPUCC + CC_GX_GMU, "hubaon=", GPUCC + CC_HUB_AON);
	show2("RCG", "gmu cmd=", GPUCC + CC_GMU_RCG, "cfg=", GPUCC + CC_GMU_RCG + 4);
	show2("RCG", "hub cmd=", GPUCC + CC_HUB_RCG, "cfg=", GPUCC + CC_HUB_RCG + 4);
	con_skip();

	/* Bus side first (gcc), as a6xx_gmu_resume's clock bulk enable. */
	wr(GCC + GCC_VOTE, rd(GCC + GCC_VOTE) | (1u << 15) | (1u << 16));
	udelay(10);
	branch_on(GCC + GCC_DDRSS_GPU_AXI, 0);
	branch_on(GCC + GCC_GPU_MEMNOC, 0);	/* voted: status may lag */
	int snoc = branch_on(GCC + GCC_GPU_SNOC_DVM, 1);
	con_hex2("BUS", "snoc ok=", snoc, "memnoc=", rd(GCC + GCC_GPU_MEMNOC));

	/* CX GDSC: wait values as gdsc_init, retain FF, then un-collapse. */
	uint32_t g = rd(GPUCC + CX_GDSCR);
	if (!(g & GDSC_PWR_ON)) {
		g &= ~(0xfffu << 12);
		g |= 2u << 20 | 2u << 16 | 2u << 12 | GDSC_RETAIN_FF;
		wr(GPUCC + CX_GDSCR, g & ~GDSC_COLLAPSE);
		udelay(1);
	}
	int cx = poll(GPUCC + CX_GDS_HW_CTRL, GDSC_PWR_ON, GDSC_PWR_ON, 2000);
	con_hex2("CX GDSC", "on=", cx, "gdscr=", rd(GPUCC + CX_GDSCR));
	if (!cx) {
		con_str("GPU", "CX GDSC did not power on, stop");
		return;
	}

	/* GMU clock list (kodiak.dtsi gmu node) + the two Linux keeps on. */
	int c = 0;
	c |= branch_on(GPUCC + CC_CXO, 1) << 0;
	c |= branch_on(GPUCC + CC_AHB, 1) << 1;
	c |= branch_on(GPUCC + CC_CX_GMU, 1) << 2;
	c |= branch_on(GPUCC + CC_HUB_CX_INT, 1) << 3;
	c |= branch_on(GPUCC + CC_SMMU_VOTE, 0) << 4;
	c |= branch_on(GPUCC + CC_CB, 0) << 5;
	con_hex2("CC on", "mask=", c, "want=", 0x3f);
	show2("CC", "ahb=", GPUCC + CC_AHB, "cxgmu=", GPUCC + CC_CX_GMU);
	if ((c & 0x7) != 0x7) {
		con_str("GPU", "AHB/GMU clock off, no GMU reads");
		return;
	}
	con_skip();

	show2("GMU", "sysreset=", GMU(GMU_CM3_SYSRESET), "fwinit=", GMU(GMU_CM3_FW_INIT_RESULT));
	show2("GMU", "rpmh=", GMU(GMU_RPMH_POWER_STATE), "sptprac=", GMU(GMU_SPTPRAC_PWR_CLK));
	show2("GMU", "keepalv=", GMU(GMU_PWR_COL_KEEPALIVE), "hfi=", GMU(GMU_HFI_CTRL_STATUS));
	show2("GMU", "gen0=", GMU(GMU_GENERAL_0), "gen7=", GMU(GMU_GENERAL_7));
	show2("GMU AO", "irq=", GMU(GMU_AO_HOST_IRQ_STATUS), "gxmisc=", GMU(GPU_CC_GX_DOMAIN_MISC));
	con_str("GPU", "probe 1 done (GX untouched)");
}
