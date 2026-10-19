/*
 * Minimal RPMh ACTIVE vote for one PMIC5 LDO, MMU off (all Device memory).
 * Port of xnu bsd/dev/sweet_rpmh.c (patch 0062/0087) for the Spacewar shim:
 * same RSC geometry, cmd-db and conflict checks, apps DRV2, one ACTIVE TCS,
 * no sleep/wake sets. Only the rails rpmh_vreg_enable() whitelists.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"

#define RH_RSC_PA	0x18220000UL	/* apps_rsc drv-2 (live DT, same as sweet) */
#define RH_TCS_PA	0x18220d00UL	/* qcom,tcs-offset 0xd00 */
#define RH_DB_PA	0x80860000UL	/* reserved-memory cmd_db (Spacewar) */
#define RH_DB_SIZE	0x20000u
#define RH_DRV		2u
#define RH_ACTIVE_N	2u		/* tcs-config: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N	8u
#define RH_TCS_STRIDE	0x2a0u
#define RH_CMD_STRIDE	0x14u
#define RH_IRQ_EN	0x00u
#define RH_IRQ_STATUS	0x04u
#define RH_IRQ_CLEAR	0x08u
#define RH_WAIT		0x10u
#define RH_CONTROL	0x14u
#define RH_STATUS	0x18u
#define RH_ENABLE	0x1cu
#define RH_MSGID	0x30u
#define RH_ADDR		0x34u
#define RH_DATA		0x38u
#define RH_CMD_STATUS	0x3cu
#define RH_AMC_EN	(1u << 16)
#define RH_TRIGGER	(1u << 24)
#define RH_ISSUED	(1u << 8)
#define RH_COMPLETE	(1u << 16)
#define RH_WRITE_ACK	(8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC	0x0c0330dbu
#define RH_DB_HEADER	144u
#define RH_DB_ENTRY	24u
#define RH_VRM		4u
#define RH_TIMEOUT_US	100000u

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

static void udelay(unsigned us)
{
	uint64_t t, f;
	__asm__ volatile("isb; mrs %0, cntpct_el0; mrs %1, cntfrq_el0" : "=r"(t), "=r"(f));
	uint64_t end = t + f / 1000000 * us, now;
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
	while (now < end);
}

static unsigned rh_ncpt;
static int rh_poisoned;

static int rh_sync(uint32_t off, uint32_t v)
{
	wr(RH_TCS_PA + off, v);
	for (int i = 0; i < 1000; i++) {
		if (rd(RH_TCS_PA + off) == v)
			return 1;
		udelay(1);
	}
	return 0;
}

static uint16_t db16(uint32_t off)
{
	return (uint16_t)(rd(RH_DB_PA + (off & ~3u)) >> (8u * (off & 3u)));
}

static int rh_check(void)
{
	uint32_t id = rd(RH_RSC_PA), solver = rd(RH_RSC_PA + 4), config = rd(RH_RSC_PA + 0x0c);
	unsigned major = (id >> 16) & 0xff, count = (config >> (6u * RH_DRV)) & 0x3f;
	rh_ncpt = (config >> 27) & 0x1f;
	if ((major != 1 && major != 2) || config == 0xffffffffu || solver == 0xffffffffu ||
	    count < RH_NORMAL_N || count > 0x3000 / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		con_hex2("RPMH", "bad id=", id, "cfg=", config);
		return -1;
	}
	if (rd(RH_DB_PA + 4) != RH_DB_MAGIC || rd(RH_DB_PA) != 1 || (rd(RH_DB_PA + 140) & 1)) {
		con_hex2("RPMH", "bad cmd-db ver=", rd(RH_DB_PA), "magic=", rd(RH_DB_PA + 4));
		return -1;
	}
	return 0;
}

static int rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = { 0, 0 };
	for (unsigned i = 0; i < 8 && res[i]; i++)
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8 + 16 * i;
		unsigned slave = db16(h);
		if (!slave)
			break;
		uint32_t entries = RH_DB_HEADER + db16(h + 2);
		unsigned count = db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3) || entries + count * RH_DB_ENTRY > RH_DB_SIZE)
			return -1;
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rd(RH_DB_PA + e) != key[0] || rd(RH_DB_PA + e + 4) != key[1])
				continue;
			uint32_t addr = rd(RH_DB_PA + e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15))
				return -1;
			*address = addr;
			matches++;
		}
	}
	return matches == 1 ? 0 : -1;
}

static int rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE, st = rd(RH_TCS_PA + b + RH_STATUS);
	return st != 0 && st != 0xffffffffu && rd(RH_TCS_PA + b + RH_CONTROL) == 0 &&
	       rd(RH_TCS_PA + b + RH_ENABLE) == 0 && rd(RH_TCS_PA + b + RH_WAIT) == 0 &&
	       !(rd(RH_TCS_PA + RH_IRQ_STATUS) & (1u << t));
}

static int rh_send(uint32_t address, unsigned mv)
{
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; t++) {
		uint32_t b = t * RH_TCS_STRIDE, en = rd(RH_TCS_PA + b + RH_ENABLE);
		if (en & ~((1u << rh_ncpt) - 1))
			return -1;
		for (unsigned c = 0; c < rh_ncpt; c++)
			if ((en & (1u << c)) && (rd(RH_TCS_PA + b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address)
				return -2;	/* someone else's request for this rail is in flight */
		if (chosen < 0 && rh_free(t))
			chosen = (int)t;
	}
	if (chosen < 0)
		return -3;
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t, irq_en = rd(RH_TCS_PA + RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_sync(RH_IRQ_EN, irq_en & ~bit))
		return -4;
	rh_poisoned = 1;	/* from here an uncertain write quarantines RPMh for this boot */
	/* voltage, mode 7 (HPM), enable last */
	static const uint32_t offsets[3] = { 0, 8, 4 };
	uint32_t values[3] = { mv, 7, 1 };
	for (unsigned c = 0; c < 3; c++) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) || !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c]))
			return -5;
	}
	if (!rh_sync(b + RH_WAIT, 7) || !rh_sync(b + RH_ENABLE, 7) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_CONTROL, RH_AMC_EN))
		return -6;
	wr(RH_TCS_PA + b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	int done = 0, progress = 0;
	for (unsigned us = 0; us < RH_TIMEOUT_US && !done; us++) {
		uint32_t irq = rd(RH_TCS_PA + RH_IRQ_STATUS), idle = rd(RH_TCS_PA + b + RH_STATUS);
		int complete = 1;
		for (unsigned c = 0; c < 3; c++) {
			uint32_t s = rd(RH_TCS_PA + b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= s != 0xffffffffu && (s & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		progress |= idle == 0 || !complete;
		done = idle && idle != 0xffffffffu && irq != 0xffffffffu && ((irq & bit) || progress) && complete;
		if (!done)
			udelay(1);
	}
	if (!done)
		return -7;
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_ENABLE, 0) ||
	    !rh_sync(b + RH_WAIT, 0))
		return -8;
	wr(RH_TCS_PA + RH_IRQ_CLEAR, bit);
	if ((rd(RH_TCS_PA + RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rd(RH_TCS_PA + RH_IRQ_EN) & ~bit) | (irq_en & bit)))
		return -9;
	rh_poisoned = 0;
	return 0;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

int rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* Spacewar touch rails only: live DT + focaltech_core.c FTS_*VTG*. */
	if (!((streq(res, "ldoc8") && mv == 1800) || (streq(res, "ldoc3") && mv == 3300)))
		return -10;
	if (rh_poisoned || rh_check())
		return -11;
	uint32_t address;
	if (rh_lookup(res, &address))
		return -12;
	int rc = rh_send(address, mv);
	con_hex2(res, "addr=", address, "rc=", (uint32_t)rc);
	return rc;
}
