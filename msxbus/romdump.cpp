/**
 * Real MSX Cartridge ROM Dump & Hardware Bus Diagnostics UI for Pico2MSX.
 *
 * Provides live memory dumping (0x4000 - 0xBFFF) from real MSX cartridge slots
 * with multi-read stability checks, header identification, and interactive navigation.
 */

#include "romdump.h"
#include "msxbus.h"
#include "pico_dsp.h"
#include "usb_kbd/kbd.h"
#include <stdio.h>
#include <string.h>

extern "C" {
    #include "emuapi.h"
}

extern PICO_DSP tft;

#define DUMP_BASE        0x4000
#define DUMP_END         0xC000
#define DUMP_SIZE        (DUMP_END - DUMP_BASE) // 32KB
#define DUMP_TRIES       10
#define DUMP_COLS        8
#define DUMP_TOTAL_LINES (DUMP_SIZE / DUMP_COLS) // 4096 lines
#define DUMP_VIS_LINES   20
#define DUMP_PAGE_BYTES  (DUMP_VIS_LINES * DUMP_COLS) // 160 bytes

// Colors (16-bit RGB565)
#define C_BLACK         RGBVAL16(0x00, 0x00, 0x00)
#define C_MIDNIGHT_BLUE RGBVAL16(0x00, 0x10, 0x48)
#define C_NAVY          RGBVAL16(0x00, 0x20, 0x80)
#define C_CYAN          RGBVAL16(0x00, 0xFF, 0xFF)
#define C_YELLOW        RGBVAL16(0xFF, 0xFF, 0x00)
#define C_WHITE         RGBVAL16(0xFF, 0xFF, 0xFF)
#define C_RED           RGBVAL16(0xFF, 0x20, 0x20)
#define C_GREEN         RGBVAL16(0x20, 0xFF, 0x20)
#define C_GRAY          RGBVAL16(0x90, 0x90, 0x90)
#define C_DARK_GRAY     RGBVAL16(0x40, 0x40, 0x40)

static bool s_dumpActive = false;
static bool s_dumpRunning = false;
static bool s_dumpDirty = false;
static int  s_dumpSlot = 0;         // 0: Slot 1, 1: Slot 2
static int  s_dumpScrollLine = 0;   // Current start line
static int  s_dumpErrors = 0;       // Unstable read count on current page

static uint8_t s_dumpData[DUMP_PAGE_BYTES];
static uint8_t s_dumpUnstable[DUMP_PAGE_BYTES];
static uint8_t s_romHeader[16];

bool RomDump_IsActive(void) {
    return s_dumpActive;
}

void RomDump_Open(int slot) {
    s_dumpActive = true;
    s_dumpSlot = (slot == 1) ? 1 : 0;
    s_dumpScrollLine = 0;
    s_dumpDirty = true;
    RomDump_Perform();
}

void RomDump_Close(void) {
    s_dumpActive = false;
    s_dumpDirty = false;
}

void RomDump_ToggleSlot(void) {
    s_dumpSlot = (s_dumpSlot == 0) ? 1 : 0;
    s_dumpDirty = true;
    RomDump_Perform();
}

void RomDump_Perform(void) {
    if (!s_dumpActive) return;
    s_dumpRunning = true;

    MsxBus_Init();
    const int slt_rd = s_dumpSlot ? RD_SLTSL2 : RD_SLTSL1;

    // Bounds check scroll line
    if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
    if (s_dumpScrollLine > DUMP_TOTAL_LINES - DUMP_VIS_LINES)
        s_dumpScrollLine = DUMP_TOTAL_LINES - DUMP_VIS_LINES;

    // 1. Read MSX ROM header from 0x4000
    for (int i = 0; i < 16; i++) {
        s_romHeader[i] = MsxBus_Read(slt_rd, (uint16_t)(0x4000 + i));
    }

    // 2. Read visible page bytes with multi-read stability verification
    int startOffset = s_dumpScrollLine * DUMP_COLS;
    int readBytes = DUMP_PAGE_BYTES;
    if (startOffset + readBytes > DUMP_SIZE) {
        readBytes = DUMP_SIZE - startOffset;
    }

    int pageErrors = 0;
    for (int i = 0; i < readBytes; i++) {
        uint16_t addr = (uint16_t)(DUMP_BASE + startOffset + i);
        uint8_t b = 0;
        uint8_t c = 0;
        bool unstable = false;

        for (int j = 0; j < DUMP_TRIES; j++) {
            b = MsxBus_Read(slt_rd, addr);
            if (j > 0 && c != b) {
                unstable = true;
            }
            c = b;
        }

        s_dumpData[i] = b;
        s_dumpUnstable[i] = unstable ? 1 : 0;
        if (unstable) {
            pageErrors++;
        }
    }

    // Fill remaining bytes if at edge
    for (int i = readBytes; i < DUMP_PAGE_BYTES; i++) {
        s_dumpData[i] = 0xFF;
        s_dumpUnstable[i] = 0;
    }

    s_dumpErrors = pageErrors;
    s_dumpRunning = false;
    s_dumpDirty = true;
}

static void RomDump_Draw(void) {
    if (!s_dumpActive) return;

    // Clear entire screen (320x240)
    tft.fillScreenNoDma(C_BLACK);

    // 1. Title bar (Y=0..9, Midnight Blue background)
    tft.drawRectNoDma(0, 0, 320, 10, C_MIDNIGHT_BLUE);
    char title[64];
    const char* busLabel = (MsxBus_GetHardware() == MsxBusHwZemmix) ? "ZEMMIX" : "GPIO";
    uint16_t pageStart = (uint16_t)(DUMP_BASE + s_dumpScrollLine * DUMP_COLS);
    uint16_t pageEnd = (uint16_t)(pageStart + DUMP_PAGE_BYTES - 1);
    snprintf(title, sizeof(title), "ROM DUMP [%s] SLOT %d (0x%04X-0x%04X)",
             busLabel, s_dumpSlot + 1, pageStart, pageEnd);
    tft.drawTextNoDma(4, 1, title, C_YELLOW, C_MIDNIGHT_BLUE, false);

    // 2. ROM Header Info (Y=11)
    char hdr[64];
    bool hasId = (s_romHeader[0] == 0x41 && s_romHeader[1] == 0x42);
    snprintf(hdr, sizeof(hdr), "Hdr:%02X %02X (%s) INIT:%02X%02X STMT:%02X%02X",
             s_romHeader[0], s_romHeader[1], hasId ? "AB" : "--",
             s_romHeader[3], s_romHeader[2],
             s_romHeader[5], s_romHeader[4]);
    tft.drawTextNoDma(4, 11, hdr, hasId ? C_GREEN : C_CYAN, C_BLACK, false);

    // 3. Page & Error Status (Y=20)
    char status[64];
    int curPage = (s_dumpScrollLine / DUMP_VIS_LINES) + 1;
    int totalPages = ((DUMP_TOTAL_LINES + DUMP_VIS_LINES - 1) / DUMP_VIS_LINES);
    snprintf(status, sizeof(status), "Page %d/%d (Addr 0x%04X, Err:%d, %dx)",
             curPage, totalPages, pageStart, s_dumpErrors, DUMP_TRIES);
    tft.drawTextNoDma(4, 20, status, s_dumpErrors ? C_RED : C_WHITE, C_BLACK, false);

    // 4. Column Header (Y=29)
    tft.drawTextNoDma(4, 29, "ADDR   0  1  2  3  4  5  6  7  ASCII", C_GRAY, C_BLACK, false);

    // 5. 20 Data Rows (Y=38..217)
    for (int row = 0; row < DUMP_VIS_LINES; row++) {
        int line = s_dumpScrollLine + row;
        if (line >= DUMP_TOTAL_LINES) break;
        int y = 38 + row * 9;
        uint16_t addr = (uint16_t)(DUMP_BASE + line * DUMP_COLS);
        int off = row * DUMP_COLS;

        // Address label
        char addrBuf[8];
        snprintf(addrBuf, sizeof(addrBuf), "%04X:", addr);
        tft.drawTextNoDma(4, y, addrBuf, C_CYAN, C_BLACK, false);

        // Hex data and ASCII
        char ascii[DUMP_COLS + 1];
        for (int col = 0; col < DUMP_COLS; col++) {
            uint8_t b = s_dumpData[off + col];
            bool bad = (s_dumpUnstable[off + col] != 0);
            char hex[4];
            snprintf(hex, sizeof(hex), "%02X", b);
            tft.drawTextNoDma(48 + col * 24, y, hex, bad ? C_RED : C_WHITE, C_BLACK, false);
            ascii[col] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        ascii[DUMP_COLS] = '\0';
        tft.drawTextNoDma(48 + DUMP_COLS * 24 + 8, y, ascii, C_GRAY, C_BLACK, false);
    }

    // 6. Footer help bar (Y=218..239, Midnight Blue background)
    tft.drawRectNoDma(0, 218, 320, 22, C_MIDNIGHT_BLUE);
    tft.drawTextNoDma(4, 220, "[ALT+D/1/2] Slot [H] HW Profile (GPIO/ZEMMIX)", C_YELLOW, C_MIDNIGHT_BLUE, false);
    tft.drawTextNoDma(4, 229, "[Up/Dn/Pg] Scroll [R/Ent] Re-read [ESC] Exit", C_CYAN, C_MIDNIGHT_BLUE, false);
}

void RomDump_Update(void) {
    if (s_dumpActive && s_dumpDirty) {
        RomDump_Draw();
        s_dumpDirty = false;
    }
}

bool RomDump_HandleKey(int keycode, int code, int codeshifted, int flags, int pressed) {
    (void)codeshifted;
    if (!s_dumpActive) return false;
    if (pressed != KEY_PRESSED) return true; // Consume releases while active

    bool alt_held = (flags & (KBD_FLAG_LALT | KBD_FLAG_RALT)) != 0;

    // ESC (0x29 / 1008 / 0x1B): Close dump and return to emulator
    if (code == KBD_KEY_ESC || code == 0x1B || keycode == 0x29) {
        RomDump_Close();
        return true;
    }

    // H: Toggle Hardware Profile (GPIO <-> ZEMMIX)
    if (code == 'h' || code == 'H' || keycode == 0x0B) {
        MsxBus_ToggleHardware();
        RomDump_Perform();
        return true;
    }

    // ALT+D: Toggle Slot 1 / Slot 2
    if (alt_held && (code == 'd' || code == 'D' || keycode == 0x07)) {
        RomDump_ToggleSlot();
        return true;
    }

    // 1: Select Slot 1
    if (code == '1' || keycode == 0x1E) {
        RomDump_Open(0);
        return true;
    }

    // 2: Select Slot 2
    if (code == '2' || keycode == 0x1F) {
        RomDump_Open(1);
        return true;
    }

    // UP Arrow: Scroll up 1 line
    if (code == KBD_KEY_UP || keycode == 0x52) {
        if (s_dumpScrollLine > 0) {
            s_dumpScrollLine--;
            RomDump_Perform();
        }
        return true;
    }

    // DOWN Arrow: Scroll down 1 line
    if (code == KBD_KEY_DOWN || keycode == 0x51) {
        if (s_dumpScrollLine < DUMP_TOTAL_LINES - DUMP_VIS_LINES) {
            s_dumpScrollLine++;
            RomDump_Perform();
        }
        return true;
    }

    // PAGE UP / LEFT Arrow: Scroll up 1 page (20 lines)
    if (code == KBD_KEY_PGUP || code == KBD_KEY_LEFT || keycode == 0x4B || keycode == 0x50) {
        s_dumpScrollLine -= DUMP_VIS_LINES;
        if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }

    // PAGE DOWN / RIGHT Arrow: Scroll down 1 page (20 lines)
    if (code == KBD_KEY_PGDN || code == KBD_KEY_RIGHT || keycode == 0x4E || keycode == 0x4F) {
        s_dumpScrollLine += DUMP_VIS_LINES;
        if (s_dumpScrollLine > DUMP_TOTAL_LINES - DUMP_VIS_LINES)
            s_dumpScrollLine = DUMP_TOTAL_LINES - DUMP_VIS_LINES;
        if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }

    // HOME: Jump to start (0x4000)
    if (code == KBD_KEY_HOME || keycode == 0x4A) {
        s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }

    // END: Jump to end (0xBFE0)
    if (code == KBD_KEY_END || keycode == 0x4D) {
        s_dumpScrollLine = DUMP_TOTAL_LINES - DUMP_VIS_LINES;
        if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }

    // ENTER / R: Re-read / refresh current page
    if (code == KBD_KEY_ENTER || code == '\r' || code == 'r' || code == 'R' || keycode == 0x28 || keycode == 0x15) {
        RomDump_Perform();
        return true;
    }

    // F5: Bus Reset
    if (code == KBD_KEY_F5 || keycode == 0x3E) {
        MsxBus_Reset(50);
        RomDump_Perform();
        return true;
    }

    return true;
}

bool RomDump_HandlePad(uint16_t bClick) {
    if (!s_dumpActive) return false;

    if ((bClick & MASK_JOY2_UP) || (bClick & MASK_JOY1_UP)) {
        if (s_dumpScrollLine > 0) {
            s_dumpScrollLine--;
            RomDump_Perform();
        }
        return true;
    }
    if ((bClick & MASK_JOY2_DOWN) || (bClick & MASK_JOY1_DOWN)) {
        if (s_dumpScrollLine < DUMP_TOTAL_LINES - DUMP_VIS_LINES) {
            s_dumpScrollLine++;
            RomDump_Perform();
        }
        return true;
    }
    if ((bClick & MASK_JOY2_LEFT) || (bClick & MASK_JOY1_LEFT)) {
        s_dumpScrollLine -= DUMP_VIS_LINES;
        if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }
    if ((bClick & MASK_JOY2_RIGHT) || (bClick & MASK_JOY1_RIGHT)) {
        s_dumpScrollLine += DUMP_VIS_LINES;
        if (s_dumpScrollLine > DUMP_TOTAL_LINES - DUMP_VIS_LINES)
            s_dumpScrollLine = DUMP_TOTAL_LINES - DUMP_VIS_LINES;
        if (s_dumpScrollLine < 0) s_dumpScrollLine = 0;
        RomDump_Perform();
        return true;
    }
    if (bClick & MASK_KEY_USER1) {
        RomDump_Open(0);
        return true;
    }
    if (bClick & MASK_KEY_USER2) {
        RomDump_Open(1);
        return true;
    }
    if (bClick & MASK_JOY2_BTN) {
        RomDump_Perform();
        return true;
    }

    return false;
}
