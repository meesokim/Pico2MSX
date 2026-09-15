// PicoDVI display backend for Waveshare RP2350 HDMI
// Implements HDMI output via PIO instead of HSTX

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "dvi_timing.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "display_backend.h"

// DVI instance
static struct dvi_inst dvi0;

// Framebuffer (8bpp RGB332 to match existing VGA paths)
static uint8_t *framebuffer = NULL;
static uint16_t fb_width = 0;
static uint16_t fb_height = 0;
static uint16_t fb_stride = 0; // bytes per line

static volatile bool vsync_flag = false;

// ---------------------------------------------------------------------------
// HDMI Audio support
// Audio samples are fed into the DVI audio ring buffer from Core0 via a
// repeating hardware timer.  This avoids any conflict with Core1 which is
// fully occupied running the TMDS encoder.
// ---------------------------------------------------------------------------
#define AUDIO_BUFFER_SIZE 1024  // samples per DVI audio buffer

// Application-supplied callback: fills `len` stereo 16-bit samples into buf.
// Signature matches pico_dsp begin_audio() callback (mono short*).
static void (*audio_fill_callback)(short *stream, int len) = NULL;

// Intermediate ring buffer shared between the app (Core0 game loop) and the
// timer ISR that drains it into the DVI audio ring.
#define SND_RING_BITS   12
#define SND_RING_SIZE   (1u << SND_RING_BITS)   // 4096 samples
#define SND_RING_MASK   (SND_RING_SIZE - 1)
static int16_t  snd_ring[SND_RING_SIZE];
static volatile uint32_t snd_wr = 0;   // written by game loop / timer
static volatile uint32_t snd_rd = 0;   // consumed by timer ISR → DVI ring

static struct repeating_timer audio_timer;
static audio_sample_t audio_buf[AUDIO_BUFFER_SIZE];

// Core1 scanline callback - feeds lines to DVI encoder
static void __not_in_flash_func(core1_scanline_callback)(uint scanline_id) {
    // Discard any scanline pointers passed back
    uint8_t *bufptr;
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
        ;

    // Advance sequentially through scanlines (decoupled from v_ctr value)
    static uint next_line = 2; // 0 and 1 were queued before start
    bufptr = &framebuffer[fb_stride * next_line];
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    next_line = (next_line + 1) % fb_height;
    // Signal vsync at start of each frame (scanline 0 from VGA timing)
    if (scanline_id == 0) {
        vsync_flag = !vsync_flag;
    }
}

// Core1 main loop - runs DVI encoder
static void __not_in_flash_func(dvi_core1_main)(void) {
    // Use DMA IRQ 0 for DVI to avoid conflicts with audio I2S which uses IRQ 1
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    // Use 8bpp path (RGB332): matches existing framebuffer writes
    dvi_scanbuf_main_8bpp(&dvi0);
}

// ---------------------------------------------------------------------------
// Audio timer ISR (Core0) – runs every ~2 ms, drains snd_ring into DVI ring
// ---------------------------------------------------------------------------
static bool __not_in_flash_func(audio_timer_cb)(struct repeating_timer *t) {
    (void)t;

    // ① 콜백이 있으면 snd_ring 여유 공간만큼 미리 채움
    if (audio_fill_callback) {
        uint32_t free_slots = SND_RING_SIZE - (uint32_t)(snd_wr - snd_rd);
        if (free_slots >= 64) {
            int16_t tmp[256];
            uint32_t chunk = free_slots > 256 ? 256 : free_slots;
            audio_fill_callback(tmp, (int)chunk);
            for (uint32_t i = 0; i < chunk; i++) {
                snd_ring[snd_wr & SND_RING_MASK] = tmp[i];
                snd_wr++;
            }
        }
    }

    // ② snd_ring → DVI 오디오 링으로 드레인 (기존 코드 유지)
    uint32_t avail = get_write_size(&dvi0.audio_ring, false);
    if (avail == 0) return true;

    audio_sample_t *dst = get_write_pointer(&dvi0.audio_ring);
    uint32_t written = 0;

    for (uint32_t i = 0; i < avail; i++) {
        int16_t s = 0;
        uint32_t rd = snd_rd;
        if (rd != snd_wr) {
            s = snd_ring[rd & SND_RING_MASK];
            snd_rd = rd + 1;
        }
        dst->channels[0] = s;
        dst->channels[1] = s;
        dst++;
        written++;
    }
    increase_write_pointer(&dvi0.audio_ring, written);
    return true;
}

// ---------------------------------------------------------------------------
// Public audio API (called by pico_dsp begin_audio / WriteAudio)
// ---------------------------------------------------------------------------

// Register the app-level fill callback (optional – direct WriteAudio also works).
void display_backend_audio_begin(void (*callback)(short *stream, int len),
                                 int samplesize) {
    audio_fill_callback = callback;
    // pre-fill 제거 - 타이머 ISR에서 처리
                                 }

// Write PCM samples (int16_t mono) directly into the ring buffer.
// Returns the number of samples actually written.
uint32_t display_backend_write_audio(const int16_t *samples, uint32_t count) {
    uint32_t free_slots = SND_RING_SIZE - (uint32_t)(snd_wr - snd_rd);
    if (count > free_slots) count = free_slots;
    for (uint32_t i = 0; i < count; i++) {
        snd_ring[snd_wr & SND_RING_MASK] = samples[i];
        snd_wr++;
    }
    return count;
}

// Query how many sample slots are free in our ring buffer.
uint32_t display_backend_get_free_audio(void) {
    return SND_RING_SIZE - (uint32_t)(snd_wr - snd_rd);
}

void display_backend_init(uint16_t width, uint16_t height) {
    // CRITICAL: DO NOT change sysclk here!
    // sysclk is set to 240 MHz in main() to give PIO USB an integer divider
    // (240/48 = 5.0). PicoDVI at 240 MHz bit clock works fine (~57 Hz).
    // If you change sysclk here, PIO USB dividers become stale and USB breaks.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);

    printf("[PicoDVI] Initializing display backend...\n");
    printf("[PicoDVI] Resolution: %dx%d\n", width, height);
    
    fb_width = width;
    fb_height = height;
    fb_stride = (width < 320) ? 320 : width;  // bytes per line
    
    size_t fb_size = (size_t)fb_stride * fb_height;
    framebuffer = (uint8_t*)malloc(fb_size);
    if (!framebuffer) {
        printf("[PicoDVI] ERROR: Failed to allocate framebuffer!\n");
        return;
    }
    memset(framebuffer, 0x00, fb_size);
    // Color bars test pattern
    {
        const uint8_t bars[8] = {
            0xFF, 0xFC, 0xE3, 0x1F,
            0xE0, 0x1C, 0x03, 0x00
        };
        uint band_h = fb_height / 8u;
        for (uint b = 0; b < 8; ++b) {
            uint y0 = b * band_h;
            uint y1 = (b == 7) ? fb_height : y0 + band_h;
            for (uint y = y0; y < y1; ++y)
                memset(&framebuffer[y * fb_stride], bars[b], fb_stride);
        }
    }
    printf("[PicoDVI] Framebuffer at %p (%zu bytes)\n", framebuffer, fb_size);

    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);
    
    dvi0.timing = &dvi_timing_640x480p_60hz;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    
    printf("[PicoDVI] DVI timing: %dx%d @ 60Hz (252 MHz)\n",
           dvi0.timing->h_active_pixels,
           dvi0.timing->v_active_lines);
    printf("[PicoDVI] PIO%d TMDS pins: %u,%u,%u CLK:%u invert:%d\n",
        (DVI_DEFAULT_SERIAL_CONFIG.pio == pio0) ? 0 : 1,
        DVI_DEFAULT_SERIAL_CONFIG.pins_tmds[0],
        DVI_DEFAULT_SERIAL_CONFIG.pins_tmds[1],
        DVI_DEFAULT_SERIAL_CONFIG.pins_tmds[2],
        DVI_DEFAULT_SERIAL_CONFIG.pins_clk,
        DVI_DEFAULT_SERIAL_CONFIG.invert_diffpairs);
    
    // Push first two scanlines to start the pipeline
    uint8_t *bufptr = framebuffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    bufptr += fb_stride;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    // -----------------------------------------------------------------------
    // HDMI Audio setup
    // CTS=28000 / N=6272 gives 44100 Hz for 25.2 MHz pixel clock (252 MHz bit clock).
    // -----------------------------------------------------------------------
    memset(audio_buf, 0, sizeof(audio_buf));
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buf, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, 44100, 28000, 6272);
    printf("[PicoDVI] HDMI audio enabled: 44100 Hz stereo\n");

    // Start Core0 repeating timer that drains snd_ring → DVI audio ring.
    // 2 ms period gives ~88 samples per tick at 44100 Hz – small enough
    // to keep latency low while leaving CPU time for the game loop.
    add_repeating_timer_ms(-2, audio_timer_cb, NULL, &audio_timer);
    printf("[PicoDVI] Audio timer started (2 ms period)\n");

    // Launch Core1 to run DVI encoder
    printf("[PicoDVI] Launching Core1 for DVI encoding...\n");
    multicore_launch_core1(dvi_core1_main);
    
    printf("[PicoDVI] Initialization complete.\n");
}

uint8_t* display_backend_get_framebuffer(void) {
    return framebuffer;
}

uint16_t display_backend_get_width(void) {
    return fb_width;
}

uint16_t display_backend_get_height(void) {
    return fb_height;
}

uint16_t display_backend_get_stride(void) {
    return fb_stride;
}

void display_backend_vsync(void) {
    volatile bool vb = vsync_flag;
    while (vsync_flag == vb) {
        __dmb();
    }
}
