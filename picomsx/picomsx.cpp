#include "pico.h"
#include "pico/stdlib.h"

extern "C" {
  #include "iopins.h"
  #include "emuapi.h"
}
#include "keyboard_osd.h"

extern "C" {
#include "fmsx.h"
}
#include <stdio.h>

#include <stdio.h>
#include "pico_dsp.h"

extern "C" unsigned int sAudioSamples;

volatile bool vbl=true;

bool repeating_timer_callback(struct repeating_timer *t) {
    if (vbl) {
        vbl = false;
    } else {
        vbl = true;
    }
    return true;
}

PICO_DSP tft;
static int skip=0;

#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "hdmi_framebuffer.h"

#include "msxbus.h"
#include "romdump.h"

int main(void) {
//    vreg_set_voltage(VREG_VOLTAGE_1_05);
//    set_sys_clock_khz(125000, true);
//    set_sys_clock_khz(150000, true);
//    set_sys_clock_khz(133000, true);
//    set_sys_clock_khz(200000, true);
//    set_sys_clock_khz(210000, true);
//    set_sys_clock_khz(230000, true);
//    set_sys_clock_khz(225000, truxe);
//    set_sys_clock_khz(250000, true);

#if defined(BOARD_WAVESHARE) || !defined(HAS_USBPIO)
    // 252 MHz sysclk for standard 60Hz DVI (25.2 MHz pixel clock)
    set_sys_clock_khz(252000, true);
    *((uint32_t *)(0x40010000+0x58)) = 2 << 16;
#else
    // PIO USB requires multiple of 48 MHz for USB timing
    set_sys_clock_khz(240000, true);
    *((uint32_t *)(0x40010000+0x58)) = 8 << 16; // HSTX clock/8 = 30.0MHz
#endif

/*
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_40);
    sleep_ms(10);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(120000, false);
    *qmi_m0_timing = 0x60007303;
*/

     emu_init();
     MsxBus_Init();
     g_RealSlot[0] = 1; // Enable Real Cartridge Slot 1




    char * filename;
#ifdef FILEBROWSER
    while (true) {
        if (menuActive()) {
            uint16_t bClick = emu_DebounceLocalKeys();
            int action = handleMenu(bClick);
            filename = menuSelection();
            if (action == ACTION_RUN) {
              break;
            }
            tft.waitSync();
        }
    }
#endif
    emu_start();
    emu_Init(filename);
    tft.startRefresh();
    //printf("[DBG] About to call emu_sndInit\n");
    //emu_sndInit();
    //printf("[DBG] emu_sndInit done\n");
    struct repeating_timer timer;
    add_repeating_timer_ms(25, repeating_timer_callback, NULL, &timer);
    while (true) {
        uint16_t bClick = emu_DebounceLocalKeys();
        if (RomDump_IsActive()) {
            RomDump_HandlePad(bClick);
            RomDump_Update();
            tft.waitSync();
            continue;
        }
        emu_Input(bClick);
        emu_Step();
    }
}

static unsigned short palette16[PALETTE_SIZE];
void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int index)
{
    if (index<PALETTE_SIZE) {
        palette16[index]  = RGBVAL16(r,g,b);
    }
}

void emu_DrawLinePal16(unsigned char * VBuf, int width, int height, int line)
{
    if (skip == 0) {
         tft.writeLinePal(width,height,line, VBuf, palette16);
    }
}

void emu_DrawLine16(unsigned short * VBuf, int width, int height, int line)
{
    if (skip == 0) {
        tft.writeLine(width,height,line, VBuf);
    }
}

int emu_IsVga(void)
{
    return (tft.getMode() == MODE_VGA_320x240?1:0);
}

void emu_DrawVsync(void)
{
    skip += 1;
    skip &= VID_FRAME_SKIP;
    tft.waitSync();
}

/*
void emu_DrawLine8(unsigned char * VBuf, int width, int height, int line)
{
    if (skip == 0) {
#ifdef USE_VGA
      tft.writeLine(width,height,line, VBuf);
#endif
    }
}

void emu_DrawLine16(unsigned short * VBuf, int width, int height, int line)
{
    if (skip == 0) {
#ifdef USE_VGA
        tft.writeLine16(width,height,line, VBuf);
#else
        tft.writeLine(width,height,line, VBuf);
#endif
    }
}

void emu_DrawScreen(unsigned char * VBuf, int width, int height, int stride)
{
    if (skip == 0) {
#ifdef USE_VGA
        tft.writeScreen(width,height-TFT_VBUFFER_YCROP,stride, VBuf+(TFT_VBUFFER_YCROP/2)*stride, palette8);
#else
        tft.writeScreen(width,height-TFT_VBUFFER_YCROP,stride, VBuf+(TFT_VBUFFER_YCROP/2)*stride, palette16);
#endif
    }
}

int emu_FrameSkip(void)
{
    return skip;
}

void * emu_LineBuffer(int line)
{
    return (void*)tft.getLineBuffer(line);
}
*/


#ifdef USE_LIBDVI

// ---------------------------------------------------------------------------
// Audio bridge between fMSX Sound.c and the DVI HDMI audio ring.
//
// Sound.c calls:
//   GetFreeAudio() - how many samples can we accept right now?
//   WriteAudio(buf, n) - push n int16 samples into the audio system
//
// We forward these directly to display_backend (display_picodvi.c).
// No intermediate ring buffer is needed: display_picodvi.c already has
// its own ring (snd_ring) and a Core0 timer that drains it into the DVI
// audio ring every 2 ms.
// ---------------------------------------------------------------------------

extern "C" {
    #include "Sound.h"
    #include "display_backend.h"

}
#include "AudioPlaySystem.h"
static AudioPlaySystem audioPlayer;

extern "C" unsigned int GetFreeAudio(void) {
    static int cnt = 0;
    if (cnt++ < 5) printf("[Audio] GetFreeAudio called, free=%u\n", display_backend_get_free_audio());
    return display_backend_get_free_audio();
}

extern "C" unsigned int WriteAudio(short *buf, unsigned int n) {
    static int cnt = 0;
    if (cnt++ < 5) printf("[Audio] WriteAudio called: n=%u, SndRate=%u\n", n, GetSndRate());
    return display_backend_write_audio(buf, n);
}

void emu_sndInit() {
    display_backend_audio_begin(NULL, 0);
    audioPlayer.begin();
    audioPlayer.start();
}

void emu_sndPlaySound(int, int, int) { /* not used on DVI path */ }
void emu_sndPlayBuzz(int, int)       { /* not used on DVI path */ }

#endif



