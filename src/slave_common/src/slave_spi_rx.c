#include "slave_spi_rx.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"

#include "vgm_spi_protocol.h"

#if VGM_SLAVE_VERBOSE_LOG
#include <stdio.h>
#include <stdbool.h>
#include "pico/time.h"
#endif

void slave_spi_rx_run(uint spi_index, uint pin_sck, uint pin_mosi, uint pin_cs,
                      uint32_t valid_opcode_mask) {
    spi_inst_t *spi = (spi_index == 0) ? spi0 : spi1;

    // Connect the pins to the peripheral BEFORE enabling it (spi_init()
    // enables the SPI block internally): configuring it first left its
    // SCK/MOSI/CS inputs floating/disconnected for a few cycles, during
    // which the hardware could latch garbage into the bit/byte counter or
    // RX FIFO -- a plausible source of a persistent per-frame offset.
    gpio_set_function(pin_sck, GPIO_FUNC_SPI);
    gpio_set_function(pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(pin_cs, GPIO_FUNC_SPI);
    // CS is active-low and idles high, driven by the master -- but nothing
    // here reinforces that idle level, so if the master's drive is ever
    // marginal at this slave's end (long shared-bus wire, noise), the input
    // could momentarily read as asserted and this slave would shift in
    // bytes actually meant for a *different* CS line on the same shared
    // SCK/MOSI bus (observed: an AY-3-8910 slave decoding SCC/SegaPCM/
    // YM2151 opcodes that were never sent to it). A pull-up costs nothing
    // (it never fights the master's active drive, whichever level that is)
    // and only helps CS settle cleanly back to its idle-high state.
    gpio_pull_up(pin_cs);

    spi_init(spi, VGM_SPI_BAUD_HZ);
    spi_set_slave(spi, true);
    // SPI mode 1 (CPOL=0, CPHA=1), NOT mode 0. With CPHA=1 the PL022 slave
    // samples each byte's first bit on a clock edge rather than on the CS
    // assertion, so it can clock a CS-held continuous 3-byte burst without
    // dropping bytes 2-3 -- which mode 0 could not (verified 2026-09-02 on
    // the Pico2 YM2151 link: mode-0 burst delivered only byte 1 of every 3;
    // mode-1 burst delivered all three). This lets the master send a whole
    // frame under one CS assertion (~6us at 4 MHz) instead of pulsing CS per
    // byte with a gap (~370us), which FM-dense music (OutRun on YM2151)
    // needs. The master (slave_bus.c) MUST use the same mode -- a mode-0
    // slave against a mode-1 master reads garbage.
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    // Discard anything that may have accumulated in the RX FIFO before we
    // start trusting frame boundaries (belt-and-braces alongside the
    // reordering above).
    while (spi_is_readable(spi)) (void)spi_get_hw(spi)->dr;

#if VGM_SLAVE_VERBOSE_LOG
    // Capture into a RAM ring buffer instead of printf()-ing inline: a
    // printf() per byte (over UART/USB) can itself block for milliseconds,
    // which starves this loop of CPU time and overflows the SPI hardware's
    // own 8-byte RX FIFO -- i.e. logging like that corrupts the very thing
    // it's trying to observe. Recording {seq, val, timestamp, resynced}
    // is just a few RAM writes (sub-microsecond), so it doesn't perturb
    // real reception; all entries are dumped via printf in one shot only
    // once the buffer fills, well after the window of interest was
    // captured.
    typedef struct {
        uint32_t seq;
        uint8_t val;
        uint64_t t_us;
        bool resync_discard; // true if this byte was thrown away while
                              // hunting for a valid opcode (see below)
    } rx_trace_entry_t;
    #define RX_TRACE_LEN 900
    static rx_trace_entry_t trace[RX_TRACE_LEN];
    uint32_t trace_count = 0;
    bool dumped = false;
    uint32_t byte_seq = 0; // monotonic count of raw bytes actually clocked in
    #define RX_TRACE(v, discard) \
        do { \
            if (trace_count < RX_TRACE_LEN) { \
                trace[trace_count++] = (rx_trace_entry_t){ \
                    .seq = byte_seq, .val = (v), .t_us = time_us_64(), .resync_discard = (discard), \
                }; \
            } else if (!dumped) { \
                dumped = true; \
                printf("[RXTRACE] dumping %d captured bytes...\n", RX_TRACE_LEN); \
                for (uint32_t k = 0; k < RX_TRACE_LEN; k++) { \
                    printf("[RX  ] seq=%lu val=0x%02X t=%lluus%s\n", \
                           (unsigned long)trace[k].seq, trace[k].val, \
                           (unsigned long long)trace[k].t_us, \
                           trace[k].resync_discard ? " DISCARDED(resync)" : ""); \
                } \
                printf("[RXTRACE] dump done, no further capture this run\n"); \
            } \
            byte_seq++; \
        } while (0)
#endif

    // Hand-rolled instead of spi_read_blocking(): that helper also pushes
    // dummy bytes into our TX FIFO (repeated_tx_data) to pace itself the
    // way a *master* would -- logic we don't want on a write-only slave
    // (MISO unconnected) and that's one less moving part to rule out while
    // debugging frame corruption. Each spi_get_hw(spi)->dr just waits for
    // the next byte to actually land in the RX FIFO; the hardware's own
    // 8-entry RX FIFO absorbs the gap while we forward the previous frame
    // to core1, so a brief stall here does not lose bytes.
    //
    // The frame boundary is NOT assumed from position alone (unlike the
    // original "always read exactly 3 bytes = one frame" version): real
    // hardware testing showed that even with the per-byte CS-pulse fix and
    // a tuned gap (master/src/slave_bus.c), a single byte can still, rarely,
    // fail to arrive -- and blind 3-at-a-time grouping has no way to
    // recover from that: every later byte shifts by one position and gets
    // misinterpreted for the rest of the run (observed as garbled register
    // writes, or a whole song muted by a corrupted MUTE opcode that a
    // one-byte protocol has no way to distinguish from a real one). Instead,
    // the byte in the "opcode" position is validated against the known
    // opcode range (< VGMSPI_OP__COUNT, see protocol/vgm_spi_protocol.h)
    // before its reg/data bytes are trusted; an out-of-range byte there is discarded
    // and the next byte is tried instead, so a lost byte corrupts at most
    // one frame instead of every frame until the stream happens to drift
    // back into alignment on its own.
    for (;;) {
        uint8_t opcode;
        for (;;) {
            while (!spi_is_readable(spi)) tight_loop_contents();
            opcode = (uint8_t)spi_get_hw(spi)->dr;
            if (opcode < 32 && ((valid_opcode_mask >> opcode) & 1u)) {
#if VGM_SLAVE_VERBOSE_LOG
                RX_TRACE(opcode, false);
#endif
                break;
            }
#if VGM_SLAVE_VERBOSE_LOG
            RX_TRACE(opcode, true);
#endif
        }
        while (!spi_is_readable(spi)) tight_loop_contents();
        uint8_t reg = (uint8_t)spi_get_hw(spi)->dr;
#if VGM_SLAVE_VERBOSE_LOG
        RX_TRACE(reg, false);
#endif
        while (!spi_is_readable(spi)) tight_loop_contents();
        uint8_t data = (uint8_t)spi_get_hw(spi)->dr;
#if VGM_SLAVE_VERBOSE_LOG
        RX_TRACE(data, false);
#endif
        uint32_t event = ((uint32_t)opcode << 16) | ((uint32_t)reg << 8) | data;
        multicore_fifo_push_blocking(event);
    }
}
