#include "slave_bus.h"

#include <stdio.h>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "vgm_spi_protocol.h"

// ---------------------------------------------------------------------
// DEFAULT routing table -- which slave boards are wired up and to which
// GPIO each one's CS line goes. These are the fallback values; the SD
// card's vgmplay.ini can override `present`/`cs`/`gap` per chip at boot
// (see player_config.c and docs/circuit.md). `present = false` makes that
// chip's VGM commands silently dropped instead of sent nowhere -- useful
// for VGM files that use chips you haven't built a slave for yet.
// See docs/circuit.md for the default CS pin layout (GPIO 12-15, 20-22,
// 26 on SPI1).
//
// `gap_us` is the per-byte CS-pulse gap for send_frame() below (see its
// comment for why this exists at all). It's tunable PER SLAVE because the
// minimum reliable gap turned out to depend on that slave's specific
// physical link (wire length/quality/position on the breadboard), not just
// on the protocol -- e.g. the SN76489 link here stayed reliable at 20us,
// but the AY-3-8910 link needed more margin before its byte-loss (heard as
// stuck/garbled register values, or a whole song going silent from a
// corrupted MUTE) went away. If a slave you add is unreliable, raise its
// own `gap_us` before touching anyone else's -- lower values keep tempo
// tighter for a chip whose link is solid, so don't raise the default just
// because one particular slave's wiring is marginal.
#define GAP_US_DEFAULT 40
static struct {
    bool present;
    uint cs_gpio;
    uint32_t gap_us;
} s_routes[VGM_CHIP_COUNT] = {
    [VGM_CHIP_SN76489] = {.present = true, .cs_gpio = 12, .gap_us = GAP_US_DEFAULT},
    [VGM_CHIP_YM2612]  = {.present = true, .cs_gpio = 13, .gap_us = GAP_US_DEFAULT},
    [VGM_CHIP_AY8910]  = {.present = true, .cs_gpio = 14, .gap_us = 80}, // needed extra margin, see above
    [VGM_CHIP_YM2413]  = {.present = true, .cs_gpio = 15, .gap_us = GAP_US_DEFAULT},
    [VGM_CHIP_YM2151]  = {.present = true, .cs_gpio = 20, .gap_us = 0}, // 0 = BURST (whole frame under one CS assertion, ~6us). FM-dense music (OutRun, ~30-write bursts inside one 22us VGM wait) can't be delivered by the per-byte-CS path in time -- the master falls seconds behind and rushes -> wrong pitch/tempo. Raising gap made it WORSE. Burst is only reliable because the bus now runs SPI mode 1 (CPHA=1); see slave_bus_init() / send_frame(). vgmplay.ini can override to a nonzero gap if this link ever proves marginal.
    [VGM_CHIP_YM2203]  = {.present = true, .cs_gpio = 21, .gap_us = GAP_US_DEFAULT},
    [VGM_CHIP_SCC]     = {.present = true, .cs_gpio = 22, .gap_us = GAP_US_DEFAULT},
    [VGM_CHIP_SEGAPCM] = {.present = true, .cs_gpio = 26, .gap_us = GAP_US_DEFAULT},
};
// ---------------------------------------------------------------------

static const char *chip_label(vgm_chip_id_t c) {
    switch (c) {
        case VGM_CHIP_SN76489: return "SN76489";
        case VGM_CHIP_YM2413:  return "YM2413";
        case VGM_CHIP_YM2612:  return "YM2612";
        case VGM_CHIP_AY8910:  return "AY-3-8910";
        case VGM_CHIP_YM2151:  return "YM2151";
        case VGM_CHIP_YM2203:  return "YM2203";
        case VGM_CHIP_SCC:     return "SCC";
        case VGM_CHIP_SEGAPCM: return "SegaPCM";
        default:               return "?";
    }
}

void slave_bus_set_present(vgm_chip_id_t chip, bool present) {
    if (chip < VGM_CHIP_COUNT) s_routes[chip].present = present;
}

void slave_bus_set_cs_gpio(vgm_chip_id_t chip, unsigned cs_gpio) {
    if (chip >= VGM_CHIP_COUNT) return;
    if (cs_gpio > 28) { // Pico header exposes GP0-22, GP26-28
        printf("config: %s cs=%u out of range (0-28), ignored\n", chip_label(chip), cs_gpio);
        return;
    }
    s_routes[chip].cs_gpio = cs_gpio;
}

void slave_bus_set_gap_us(vgm_chip_id_t chip, uint32_t gap_us) {
    if (chip < VGM_CHIP_COUNT) s_routes[chip].gap_us = gap_us;
}

#define PIN_SPI1_SCK 10
#define PIN_SPI1_MOSI 11

static void send_frame(uint cs_gpio, uint32_t gap_us, uint8_t opcode, uint8_t reg, uint8_t data) {
    uint8_t buf[VGM_SPI_FRAME_SIZE] = {opcode, reg, data};
#if VGM_MASTER_VERBOSE_LOG
    // Exactly the 3 bytes sent below -- compare against a slave's own
    // VGM_SLAVE_VERBOSE_LOG output to see whether corruption happens in
    // this software or on the wire.
    printf("[SPI ] cs=GPIO%u opcode=0x%02X reg=0x%02X data=0x%02X\n", cs_gpio, opcode, reg, data);
#endif
    // Two paths: gap_us == 0 sends the whole frame under ONE CS assertion
    // (burst); gap_us > 0 pulses CS per byte with a gap between.
    //
    // History: under SPI mode 0 (CPHA=0), holding CS low across a 3-byte
    // burst reliably delivered only the first byte on the RP2040/RP2350
    // slave hardware here -- bytes 2-3 were silently never clocked in, even
    // though the master's transmission was provably correct (verified with
    // tools/spi_bringup_test on 2026-09-02). Re-pulsing CS per byte was the
    // fix, at the cost of ~370us/frame at gap_us 120. The bus now runs SPI
    // mode 1 (CPHA=1, see slave_bus_init()), where the PL022 slave samples
    // each byte on a clock edge instead of on the CS assertion and so DOES
    // clock a continuous burst correctly (also hardware-verified). Burst is
    // ~6us/frame -- the per-byte path stays available (vgmplay.ini `gap` >
    // 0) for any link that turns out marginal, but 0/burst is the default
    // for FM-dense chips (YM2151).
    //
    // The inter-pulse gap is per-slave (see `gap_us` in s_routes above) --
    // it was originally a single shared 50us (a comfortable value chosen
    // during bring-up, not a measured minimum). At 3 pulses/frame that's
    // >=150us/frame, which is enough to visibly perturb playback tempo
    // during write-dense passages (e.g. chords/arpeggios issuing several
    // frames between two VGM wait commands) -- wait_samples() paces against
    // wall-clock deadlines, so frames that take longer than their available
    // slack make that section audibly rush/stutter. 5us was tried and was
    // too short (byte loss reappeared -- corrupted register writes heard as
    // noise). 20-40us was reliable for the SN76489 slave, but a second
    // slave (AY-3-8910, different physical link/wiring) needed 80us before
    // its byte-loss (garbled register writes, or a whole song silenced by a
    // corrupted MUTE) went away -- the needed margin depends on the
    // specific physical link, not the protocol, hence this being tunable
    // per slave instead of a single global constant. Any slave whose link
    // is solid can instead take `gap = 0` (burst, see above) for tighter
    // timing; switch it per chip in vgmplay.ini and listen for garbled
    // notes / noise (= byte loss) before trusting it.
    if (gap_us == 0) {
        gpio_put(cs_gpio, 0);
        spi_write_blocking(spi1, buf, VGM_SPI_FRAME_SIZE);
        gpio_put(cs_gpio, 1);
        return;
    }
    for (int i = 0; i < VGM_SPI_FRAME_SIZE; i++) {
        gpio_put(cs_gpio, 0);
        spi_write_blocking(spi1, &buf[i], 1);
        gpio_put(cs_gpio, 1);
        sleep_us(gap_us);
    }
}

// GPIO the master already uses for something else -- a CS line landing here
// (via a bad vgmplay.ini) would fight another peripheral. Warn, don't block.
static bool gpio_is_reserved(uint g, const char **what) {
    switch (g) {
        case 0: case 1:  *what = "OLED I2C0";      return true;
        case 2:          *what = "skip button";    return true;
        case 10: case 11:*what = "slave bus SPI1"; return true;
        case 16: case 17: case 18: case 19: *what = "SD card SPI0"; return true;
        default: return false;
    }
}

void slave_bus_init(void) {
    spi_init(spi1, VGM_SPI_BAUD_HZ);
    // SPI mode 1 (CPOL=0, CPHA=1) -- must match slave_spi_rx.c. CPHA=1 lets
    // the PL022 slaves clock a CS-held 3-byte burst without dropping bytes
    // 2-3 (mode 0 can't; verified on hardware 2026-09-02), so send_frame()'s
    // gap_us==0 burst path is now reliable -- see its comment.
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(PIN_SPI1_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI1_MOSI, GPIO_FUNC_SPI);

    for (int i = 0; i < VGM_CHIP_COUNT; i++) {
        if (!s_routes[i].present) {
            printf("slave bus: %-9s disabled\n", chip_label(i));
            continue;
        }
        const char *what;
        if (gpio_is_reserved(s_routes[i].cs_gpio, &what))
            printf("slave bus: WARNING %s cs=GPIO%u collides with %s\n",
                   chip_label(i), s_routes[i].cs_gpio, what);
        for (int j = 0; j < i; j++)
            if (s_routes[j].present && s_routes[j].cs_gpio == s_routes[i].cs_gpio)
                printf("slave bus: WARNING %s and %s share cs=GPIO%u\n",
                       chip_label(j), chip_label(i), s_routes[i].cs_gpio);

        printf("slave bus: %-9s cs=GPIO%-2u gap=%luus\n",
               chip_label(i), s_routes[i].cs_gpio, (unsigned long)s_routes[i].gap_us);
        gpio_init(s_routes[i].cs_gpio);
        gpio_set_dir(s_routes[i].cs_gpio, GPIO_OUT);
        gpio_put(s_routes[i].cs_gpio, 1);
    }
}

bool slave_bus_has_chip(vgm_chip_id_t chip) {
    return chip < VGM_CHIP_COUNT && s_routes[chip].present;
}

void slave_bus_reset(vgm_chip_id_t chip, uint8_t clock_preset) {
    if (!slave_bus_has_chip(chip)) return;
    // RESET is a one-shot, unacknowledged control frame. If the slave misses
    // it -- still booting on the first song, or one dropped SPI frame -- it
    // stays at its power-on default clock preset. For the ymfm-backed slaves
    // that default is preset 0, a DIFFERENT native sample rate (55930 vs
    // 62500 Hz for YM2151), and the slave paces its whole output at that
    // rate: the entire song then plays ~2 semitones flat, with no other
    // symptom, unaffected by gap_us, and invisible to the offline renderer
    // (which computes the preset directly). Cheap insurance: send it a few
    // times with a gap. Once per song per chip -- a burst frame is ~6us, so
    // even all 8 chips x3 is a few ms, none of it in the timed playback loop.
    for (int i = 0; i < 3; i++) {
        send_frame(s_routes[chip].cs_gpio, s_routes[chip].gap_us,
                   VGMSPI_OP_RESET, clock_preset, 0);
        sleep_us(300);
    }
}

void slave_bus_set_clock(vgm_chip_id_t chip, uint8_t clock_preset) {
    if (!slave_bus_has_chip(chip)) return;
    // Non-destructive: the slave re-paces its render loop but keeps all
    // register state. Sent once per song-second (see vgm_player.c) so a
    // slave that missed the song-start RESET burst self-corrects. Single
    // frame -- it's repeated by the caller, not here.
    send_frame(s_routes[chip].cs_gpio, s_routes[chip].gap_us,
               VGMSPI_OP_CLOCK, clock_preset, 0);
}

void slave_bus_write(vgm_chip_id_t chip, uint8_t port, uint8_t reg, uint8_t data) {
    if (!slave_bus_has_chip(chip)) return;
    uint8_t opcode = (port == 0) ? VGMSPI_OP_WRITE0 : VGMSPI_OP_WRITE1;
    send_frame(s_routes[chip].cs_gpio, s_routes[chip].gap_us, opcode, reg, data);
}

void slave_bus_send(vgm_chip_id_t chip, uint8_t opcode, uint8_t reg, uint8_t data) {
    if (!slave_bus_has_chip(chip)) return;
    send_frame(s_routes[chip].cs_gpio, s_routes[chip].gap_us, opcode, reg, data);
}

void slave_bus_mute_all(void) {
    for (int i = 0; i < VGM_CHIP_COUNT; i++) {
        if (!s_routes[i].present) continue;
        send_frame(s_routes[i].cs_gpio, s_routes[i].gap_us, VGMSPI_OP_MUTE, 0, 0);
    }
}
