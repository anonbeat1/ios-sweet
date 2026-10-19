#pragma once
#include <stdint.h>

#define SW_KICK_CPU	7	/* PSCI context id = cpu index; MPIDR 0x700 */

extern volatile uintptr_t sw_ctl;

int sw_display_init(void);	/* 0 ok, -1 no scanout pipe, -2 unexpected format/ctl */
void sw_display_dump(void);
void __attribute__((noreturn)) sw_kick_loop(void);
