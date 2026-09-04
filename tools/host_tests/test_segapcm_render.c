// Host-native sanity test for slave_segapcm/src/chip_segapcm.c. Unlike the
// FM slaves, this chip has no ymfm dependency, so it builds and runs
// directly on the host with no shims at all -- see README.md.
//
// Uploads a tiny 8-byte ramp "sample", configures channel 0 to play it
// looping, and checks that: playback starts silent (the sample's first
// byte is 0x80 = digital silence), the 16.8 fixed-point address
// accumulator advances at the rate `delta` implies, and disabling the
// channel silences it immediately.
//
// test_rebased_window() then does the same with a non-zero
// VGMSPI_OP_SEGAPCM_ROM_BASE (port 9): the ROM chunk is uploaded at a high
// absolute address and a channel reads it back through the bank machinery,
// while channels pointing outside the [base, base+budget) window read as
// silence.
#include <stdio.h>
#include <stdint.h>
#include "chip_segapcm.h"

static const uint8_t RAMP[8] = {0x80, 0xA0, 0xC0, 0xE0, 0xFF, 0xE0, 0xC0, 0xA0};

// Point channel `ch` at bank `bank` (goes in CTRL bits & 0x70) with running
// 16-bit sample address `addr16`, full volume, delta 0x20, looping, enabled.
static void arm_channel_at(int ch, uint8_t bank, uint16_t addr16) {
    segapcm_write(0, 8 * ch + 2, 0x7F);
    segapcm_write(0, 8 * ch + 3, 0x7F);
    segapcm_write(0, 8 * ch + 7, 0x20);
    segapcm_write(0, 8 * ch + 4, 0x00);
    segapcm_write(0, 8 * ch + 5, 0x00);
    segapcm_write(0, 8 * ch + 6, 0x00);
    // segapcm_render reads ROM at ((addr>>8)&0xFFFF); the running 24-bit
    // addr's mid/hi bytes ARE that 16-bit word, so load addr16 there.
    segapcm_write(0, 0x84 + 8 * ch, (uint8_t)(addr16 & 0xFF));       // addr mid
    segapcm_write(0, 0x85 + 8 * ch, (uint8_t)(addr16 >> 8));         // addr hi
    segapcm_write(0, 0x86 + 8 * ch, (uint8_t)(bank & 0x70)); // bit0=0 enabled, bit1=0 loop
}

static int test_rebased_window(void) {
    int fail = 0;
    segapcm_reset(0);

    // ROM window base 0x040000 (matches "01 Magical Sound Shower"'s lowest
    // block, 64KB-aligned). Upload the ramp at absolute ROM 0x040090 the way
    // the master now does for a byte-granular chunk start: seek to the
    // 256-aligned page, pad (0x90) bytes of 0x80, then the data.
    segapcm_write(9, 0x04, 0x00);            // VGMSPI_OP_SEGAPCM_ROM_BASE = 0x040000
    segapcm_write(6, 0x04, 0x00);            // upload_seek page 0x040000 -> cursor 0
    for (int i = 0; i < 0x90; i++) segapcm_write(7, 0, 0x80); // sub-256 pad
    for (int i = 0; i < 8; i++) segapcm_write(7, 0, RAMP[i]); // ramp now at rom_rel 0x90

    // defaults: bankshift 12, bankmask 0x70. bank 0x40 + addr16 0x0090 ->
    // rom_idx 0x040090 -> rom_rel 0x90 -> reads the ramp we just uploaded.
    arm_channel_at(0, 0x40, 0x0090);
    // bank 0x30 -> rom_idx 0x030090, below base -> must be silent.
    arm_channel_at(1, 0x30, 0x0090);
    // bank 0x70 -> rom_idx 0x070090, past base + 192KB budget -> must be silent.
    arm_channel_at(2, 0x70, 0x0090);

    for (int i = 0; i < 40; i++) {
        int16_t s = segapcm_render();
        int expect_zero = (RAMP[i / 8] == 0x80); // ch0 drives the mix
        if (expect_zero && s != 0) { printf("FAIL rebased: sample %d expected silence, got %d\n", i, s); fail = 1; }
        if (!expect_zero && s <= 0) { printf("FAIL rebased: sample %d expected >0 (rebased read), got %d\n", i, s); fail = 1; }
    }

    // Now disable ch0; ch1 (below window) and ch2 (above window) are the
    // only ones left -- output must be pure silence.
    segapcm_write(0, 0x86 + 0, 0x01);
    for (int i = 0; i < 16; i++) {
        if (segapcm_render() != 0) { printf("FAIL rebased: out-of-window channels not silent\n"); fail = 1; break; }
    }
    return fail;
}

int main(void) {
    segapcm_reset(0);

    segapcm_write(6, 0, 0); // upload_reset
    const uint8_t *sample = RAMP;
    for (int i = 0; i < 8; i++) segapcm_write(7, 0, sample[i]);

    // Channel 0: full volume, delta=0x20 (one ROM byte advances every
    // 256/0x20 = 8 renders), looped, starting at address 0.
    segapcm_write(0, 8 * 0 + 2, 0x7F); // vol L
    segapcm_write(0, 8 * 0 + 3, 0x7F); // vol R
    segapcm_write(0, 8 * 0 + 7, 0x20); // delta
    segapcm_write(0, 8 * 0 + 4, 0x00); // loop mid
    segapcm_write(0, 8 * 0 + 5, 0x00); // loop hi
    segapcm_write(0, 8 * 0 + 6, 0x00); // end
    segapcm_write(0, 0x84 + 8 * 0, 0x00); // addr mid
    segapcm_write(0, 0x85 + 8 * 0, 0x00); // addr hi
    segapcm_write(0, 0x86 + 8 * 0, 0x00); // ctrl: enabled, loop enabled

    int fail = 0;
    for (int i = 0; i < 40; i++) {
        int16_t s = segapcm_render();
        int expect_byte = i / 8; // which sample[] byte should be sounding
        int expect_zero = (sample[expect_byte] == 0x80);
        if (expect_zero && s != 0) {
            printf("FAIL: sample %d expected silence, got %d\n", i, s);
            fail = 1;
        }
        if (!expect_zero && s <= 0) {
            printf("FAIL: sample %d expected positive output, got %d\n", i, s);
            fail = 1;
        }
    }

    segapcm_write(0, 0x86 + 8 * 0, 0x01); // disable
    int16_t after_disable = segapcm_render();
    if (after_disable != 0) {
        printf("FAIL: expected silence after disabling channel, got %d\n", after_disable);
        fail = 1;
    }

    fail |= test_rebased_window();

    printf(fail ? "FAILED\n" : "ok\n");
    return fail;
}
