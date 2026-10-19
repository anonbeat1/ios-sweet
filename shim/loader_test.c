/* Host check of find_payload (cc -I. loader_test.c && ./a.out). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "loader.c"

/* Link stubs for what loader_boot uses. */
volatile uint32_t *con_fb;
void con_hex2(const char *l, const char *a, uint32_t va, const char *b, uint32_t vb) { (void)l, (void)a, (void)va, (void)b, (void)vb; }
void con_hex(const char *l, uint64_t v) { (void)l, (void)v; }
void con_str(const char *l, const char *s) { (void)l, (void)s; }
int fdt_chosen_u64(uintptr_t dtb, const char *name, uint64_t *out) { (void)dtb, (void)name, (void)out; return 0; }

int main(void)
{
	static uint64_t buf[4096];
	uint8_t *b = (uint8_t *)buf;

	/* v3: payload 4 bytes past an 8-byte boundary, after a fake vendor ramdisk */
	memset(b, 0xa5, sizeof(buf));
	uint64_t hi = (uintptr_t)b + 0x8000;
	memcpy(b + 0x1234, "SWEETXNU", 8);
	b[0x1234 + 8] = 1;
	assert(find_payload((uintptr_t)b, &hi) == (uintptr_t)b + 0x1230);
	assert(memcmp(b + 0x1230, "SWEETXNU", 8) == 0 && b[0x1238] == 1);
	assert(hi == (uintptr_t)b + 0x8000 - 4);

	/* sweet: payload at initrd start stays put */
	hi = (uintptr_t)b + 0x8000;
	memcpy(b, "SWEETXNU", 8);
	assert(find_payload((uintptr_t)b, &hi) == (uintptr_t)b && hi == (uintptr_t)b + 0x8000);

	/* absent */
	memset(b, 0, sizeof(buf));
	assert(find_payload((uintptr_t)b, &hi) == 0);
	puts("find_payload ok");
	return 0;
}
