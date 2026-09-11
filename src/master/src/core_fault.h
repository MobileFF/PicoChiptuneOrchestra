// See core_fault.c. Diagnostic-only fault/panic catcher.
#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t count; // faults seen (0 = none)
    volatile uint32_t pc;    // stacked return address (the faulting instruction)
    volatile uint32_t lr;    // stacked LR (usual caller)
    volatile uint32_t psr;   // stacked xPSR
    volatile uint32_t r0, r1, r2, r3, r12;
    volatile uint8_t  core;  // SIO CPUID: which core faulted
    volatile uint8_t  vect;  // exception vector number (3 = HardFault)
} core_fault_info_t;

extern core_fault_info_t g_core_fault;
