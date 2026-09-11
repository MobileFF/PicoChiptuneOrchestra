// Minimal fault/panic catcher for the master, used to diagnose the OLED
// "starting..." freeze: core1's render loop stops after exactly one
// iteration and static analysis rules out the wait call actually blocking,
// so the remaining explanation is that core1 takes a HardFault (or a
// pico-sdk panic(), whose bkpt escalates to HardFault with no debugger
// attached). This records the exception-stacked PC/LR/registers of the
// first fault on either core into g_core_fault so core0 can print them.
//
// Both cores share the vector table (SDK default: core1 VTOR = core0 VTOR),
// so this strong isr_hardfault overrides the weak default for both.
#include "core_fault.h"

#include "pico/stdlib.h"
#include "hardware/regs/sio.h"
#include "hardware/regs/addressmap.h"

core_fault_info_t g_core_fault;

// Called from the naked shim below with r0 = pointer to the 8-word
// exception stack frame {r0,r1,r2,r3,r12,lr,pc,xPSR}, r1 = vector number.
void fault_record(uint32_t *frame, uint32_t vect) {
    if (g_core_fault.count == 0) {
        g_core_fault.r0   = frame[0];
        g_core_fault.r1   = frame[1];
        g_core_fault.r2   = frame[2];
        g_core_fault.r3   = frame[3];
        g_core_fault.r12  = frame[4];
        g_core_fault.lr   = frame[5];
        g_core_fault.pc   = frame[6];
        g_core_fault.psr  = frame[7];
        g_core_fault.core = (uint8_t)(*(volatile uint32_t *)(SIO_BASE + SIO_CPUID_OFFSET));
        g_core_fault.vect = (uint8_t)vect;
    }
    g_core_fault.count++;
    for (;;) tight_loop_contents();
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile (
        "movs r0, #4          \n" // EXC_RETURN bit 2: which stack was used
        "mov  r1, lr          \n"
        "tst  r0, r1          \n"
        "beq  1f              \n"
        "mrs  r0, psp         \n"
        "b    2f              \n"
        "1:                   \n"
        "mrs  r0, msp         \n"
        "2:                   \n"
        "movs r1, #3          \n" // vector 3 = HardFault
        "bl   fault_record    \n"
    );
}
