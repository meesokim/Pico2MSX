/**
 * Real MSX Cartridge Bus Implementation for Raspberry Pi Pico 2 (RP2350).
 *
 * Hardware Profiles:
 *   MsxBusHwGpio   — Blueberry GPIO Board (SIO, 74HC139+374, LVC4245) - Default
 *   MsxBusHwZemmix — Zemmix Mini / MSX-Pi / RPMP2 40-Pin Latch Board
 */

#include "msxbus.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/sio.h"
#include <stdio.h>
#include <string.h>

int g_RealSlot[2] = {0, 0};
volatile unsigned g_GpioAccessCount = 0;

static MsxBusHardware s_hw = MsxBusHwGpio;
static bool s_msxbus_inited = false;
static spin_lock_t *s_bus_spinlock = NULL;

/* Fast GPIO single-cycle register macros on RP2350 SIO */
#define SIO_GPIO_SET(mask)   (sio_hw->gpio_set = (mask))
#define SIO_GPIO_CLR(mask)   (sio_hw->gpio_clr = (mask))
#define SIO_GPIO_OUT(val)    (sio_hw->gpio_out = (val))
#define SIO_GPIO_IN()        (sio_hw->gpio_in)
#define SIO_GPIO_OE_SET(m)   (sio_hw->gpio_oe_set = (m))
#define SIO_GPIO_OE_CLR(m)   (sio_hw->gpio_oe_clr = (m))

static inline void gpio_barrier(void) {
    __dmb();
}

static inline void DELAY_NOPS(int n) {
    for (volatile int i = 0; i < n; i++) {
        __nop();
    }
}

#define SYNC_NOPS_GPIO    80
#define SYNC_NOPS_ZEMMIX  80
#define WAIT_TIMEOUT      50
#define PULSE_HIGH_NOPS   16
#define LATCH_NOPS        8

static inline void PULSE_PIN(uint32_t pin) {
    SIO_GPIO_CLR(pin);
    SIO_GPIO_SET(pin);
    DELAY_NOPS(PULSE_HIGH_NOPS);
    SIO_GPIO_CLR(pin);
    gpio_barrier();
}

#ifndef BOARD_WAVESHARE
#define BOARD_WAVESHARE
#endif

/* ==============================================================================
 * Blueberry GPIO Board (2 Slots) — ARM SIO (Default)
 * ==============================================================================
 *   GPIO 0-7   : multiplexed A/D (74HC374 D + LVC4245 B)
 *   GPIO 8     : MODE0 = 74HC139 1A
 *   GPIO 9     : MODE1 = 74HC139 1B
 *                139 Y is active-low; 74HC374 clocks on the rising edge:
 *                  00→01 latch A0-A7 / 01→11 latch A8-A15 / 10 = 4245 /OE
 *                LVC4245 DIR = /WR: 1=Pico→cart, 0=cart→Pico
 *   Waveshare RP2350-PiZero header vs Raspberry Pi (physical pads):
 *     MODE1 GPIO12, /MREQ GPIO11, /IORQ GPIO10, /RD GPIO9,
 *     /SLTSL1 GPIO4, /SLTSL2 GPIO5. Data D4/D5 sit on GPIO14/15.
 * ============================================================================== */
namespace GpioHw {
#if defined(BOARD_WAVESHARE)
    #define PIN_DATA        0x0000C0CFu /* GPIO 0-3,6,7,14,15 */
    #define PIN_MODE0       (1u << 8)
    #define PIN_MODE1       (1u << 12)
    #define PIN_MREQ        (1u << 11)
    #define PIN_IORQ        (1u << 10)
    #define PIN_RD          (1u << 9)
    #define PIN_WR          (1u << 13)
    #define PIN_SLTSL1      (1u << 4)
    #define PIN_SLTSL2      (1u << 5)
#else
    #define PIN_DATA        0x000000FFu
    #define PIN_MODE0       (1u << 8)
    #define PIN_MODE1       (1u << 9)
    #define PIN_MREQ        (1u << 10)
    #define PIN_IORQ        (1u << 11)
    #define PIN_RD          (1u << 12)
    #define PIN_WR          (1u << 13)
    #define PIN_SLTSL1      (1u << 14)
    #define PIN_SLTSL2      (1u << 15)
#endif
    #define PIN_MODE        (PIN_MODE0 | PIN_MODE1)
    #define PIN_WAIT        (1u << 16)
    #define PIN_RESET       (1u << 24)
    #define CTRL_IDLE       (PIN_MREQ | PIN_IORQ | PIN_RD | PIN_WR | PIN_SLTSL1 | PIN_SLTSL2)

    /* Shuffle D4/D5 onto GPIO14/15 (Waveshare). Control pins are already remapped above. */
    static inline uint32_t DataHw(uint8_t v) {
#if defined(BOARD_WAVESHARE)
        uint32_t m = (uint32_t)(v & 0xCFu);
        if (v & 0x10u) m |= (1u << 14);
        if (v & 0x20u) m |= (1u << 15);
        return m;
#else
        return (uint32_t)v;
#endif
    }

    static inline uint8_t DataPi(uint32_t gpio_in) {
#if defined(BOARD_WAVESHARE)
        uint8_t v = (uint8_t)(gpio_in & 0xCFu);
        if (gpio_in & (1u << 14)) v |= 0x10u;
        if (gpio_in & (1u << 15)) v |= 0x20u;
        return v;
#else
        return (uint8_t)(gpio_in & 0xFFu);
#endif
    }

    static inline void SYNC(void) {
        DELAY_NOPS(SYNC_NOPS_GPIO);
        gpio_barrier();
    }

    static inline void WaitReady(void) {
        int timeout = WAIT_TIMEOUT;
        while (!(SIO_GPIO_IN() & PIN_WAIT) && --timeout > 0) {
            gpio_barrier();
        }
    }

    static void SetAddress(uint16_t addr) {
        SIO_GPIO_OE_SET(PIN_DATA | PIN_MODE | CTRL_IDLE);
        SIO_GPIO_CLR(PIN_DATA | PIN_MODE);
        gpio_barrier();
        SIO_GPIO_SET(DataHw((uint8_t)addr));
        DELAY_NOPS(LATCH_NOPS);
        SIO_GPIO_SET(PIN_MODE0);
        DELAY_NOPS(LATCH_NOPS);
        SIO_GPIO_CLR(PIN_DATA);
        SIO_GPIO_SET(DataHw((uint8_t)(addr >> 8)));
        DELAY_NOPS(LATCH_NOPS);
        SIO_GPIO_SET(PIN_MODE1);
        DELAY_NOPS(LATCH_NOPS);
        gpio_barrier();
    }

    static void Init(void) {
        for (uint i = 0; i <= 15; i++) {
            gpio_init(i);
            gpio_set_dir(i, GPIO_OUT);
            gpio_disable_pulls(i);
            gpio_set_input_enabled(i, true);
        }

        gpio_init(16);
        gpio_set_dir(16, GPIO_IN);
        gpio_pull_up(16);

        gpio_init(17);
        gpio_set_dir(17, GPIO_IN);
        gpio_pull_up(17);

        gpio_init(24);
        gpio_set_dir(24, GPIO_OUT);
        gpio_put(24, 1);

        SIO_GPIO_OE_SET(PIN_DATA | PIN_MODE | CTRL_IDLE | PIN_RESET);
        SIO_GPIO_CLR(PIN_DATA);
        SIO_GPIO_SET(PIN_MODE | CTRL_IDLE | PIN_RESET);
        gpio_barrier();

        printf("[MsxBus] Hardware: SIO 74HC139+374, LVC4245, Waveshare GPIO remap\n");
    }

    static uint8_t ReadRaw(int cmd, uint16_t addr) {
        SetAddress(addr);

        SIO_GPIO_OE_CLR(PIN_DATA);
        gpio_barrier();

        /* 11→10 enables 4245. /WR low so DIR = cart → Pico. */
        uint32_t clr = PIN_MODE0 | PIN_RD | PIN_WR;
        switch (cmd) {
            case RD_SLTSL1: clr |= PIN_MREQ | PIN_SLTSL1; break;
            case RD_SLTSL2: clr |= PIN_MREQ | PIN_SLTSL2; break;
            case RD_MEM:    clr |= PIN_MREQ; break;
            case RD_IO:     clr |= PIN_IORQ; break;
            default: break;
        }
        SIO_GPIO_CLR(clr);
        gpio_barrier();

        SYNC();
        WaitReady();
        uint8_t data = DataPi(SIO_GPIO_IN());

        SIO_GPIO_SET(PIN_MODE | CTRL_IDLE);
        SIO_GPIO_OE_SET(PIN_DATA);
        gpio_barrier();
        return data;
    }

    static void Write(int cmd, uint16_t addr, uint8_t value) {
        SetAddress(addr);

        SIO_GPIO_OE_SET(PIN_DATA);
        SIO_GPIO_CLR(PIN_DATA | PIN_MODE0);
        SIO_GPIO_SET(DataHw(value));

        uint32_t clr = 0;
        switch (cmd) {
            case WR_SLTSL1: clr = PIN_MREQ | PIN_SLTSL1; break;
            case WR_SLTSL2: clr = PIN_MREQ | PIN_SLTSL2; break;
            case WR_MEM:    clr = PIN_MREQ; break;
            case WR_IO:     clr = PIN_IORQ; break;
            default: break;
        }
        if (clr) {
            SIO_GPIO_CLR(clr);
        }
        gpio_barrier();

        /* /WR active-low pulse: assert (LOW), hold, then deassert (HIGH) to latch */
        SIO_GPIO_CLR(PIN_WR);
        DELAY_NOPS(PULSE_HIGH_NOPS);
        SIO_GPIO_SET(PIN_WR);
        gpio_barrier();

        SYNC();
        WaitReady();

        SIO_GPIO_SET(PIN_MODE | CTRL_IDLE);
        gpio_barrier();
    }

    static void Reset(int ms) {
        gpio_put(24, 0); // Assert /RESET
        sleep_ms(ms > 0 ? ms : 50);
        gpio_put(24, 1); // Deassert /RESET
        sleep_ms(20);
        gpio_barrier();
    }

    #undef PIN_DATA
    #undef PIN_MODE0
    #undef PIN_MODE1
    #undef PIN_MODE
    #undef PIN_MREQ
    #undef PIN_IORQ
    #undef PIN_RD
    #undef PIN_WR
    #undef PIN_SLTSL1
    #undef PIN_SLTSL2
    #undef PIN_WAIT
    #undef PIN_RESET
    #undef CTRL_IDLE
} // namespace GpioHw

/* ==============================================================================
 * Zemmix Mini / MSX-Pi / RPMP2 40-Pin Latch Board Profile
 * ============================================================================== */
namespace ZemmixHw {
    #define PIN_DATA_MASK  0x000000FFu
    #define PIN_SLTSL1     (1u << 8)
    #define PIN_SLTSL2     (1u << 9)
    #define PIN_CS2        (1u << 10)
    #define PIN_CS1        (1u << 11)
    #define PIN_RD         (1u << 12)
    #define PIN_WR         (1u << 13)
    #define PIN_IORQ       (1u << 14)
    #define PIN_MREQ       (1u << 15)
    #define PIN_LE_A       (1u << 16)
    #define PIN_LE_C       (1u << 17)
    #define PIN_LE_D       (1u << 18)
    #define PIN_RESET      (1u << 19)
    #define PIN_CLK        (1u << 20)
    #define PIN_DAT_DIR    (1u << 21)
    #define PIN_INT        (1u << 24)
    #define PIN_WAIT       (1u << 25)
    #define PIN_SW1        (1u << 27)

    #define CTRL_IDLE      (0x0000FF00u)

    static inline uint32_t CsBits(uint16_t addr) {
        uint32_t page = (uint32_t)addr & 0xC000u;
        if (page == 0x4000u) return PIN_CS1;
        if (page == 0x8000u) return PIN_CS2;
        return 0;
    }

    static inline void SYNC(void) {
        DELAY_NOPS(SYNC_NOPS_ZEMMIX);
        gpio_barrier();
    }

    static inline void WaitReady(void) {
        int timeout = WAIT_TIMEOUT;
        while (!(SIO_GPIO_IN() & PIN_WAIT) && --timeout > 0) {
            gpio_barrier();
        }
    }

    static void SetAddress(uint16_t addr) {
        SIO_GPIO_OE_SET(0x0000FFFFu | PIN_LE_A | PIN_LE_C | PIN_LE_D | PIN_DAT_DIR);
        SIO_GPIO_CLR(0x0000FFFFu);
        SIO_GPIO_SET(((uint32_t)addr & 0xFFFFu) | PIN_LE_A);
        DELAY_NOPS(25);
        SIO_GPIO_CLR(PIN_LE_A);
        DELAY_NOPS(20);
        SIO_GPIO_SET(CTRL_IDLE | PIN_LE_C);
        SIO_GPIO_CLR(PIN_DATA_MASK);
        DELAY_NOPS(15);
        gpio_barrier();
    }

    static void Init(void) {
        for (uint i = 0; i <= 21; i++) {
            gpio_init(i);
            gpio_set_dir(i, GPIO_OUT);
            gpio_put(i, 0);
        }
        gpio_init(26);
        gpio_set_dir(26, GPIO_OUT);
        gpio_put(26, 0);

        const uint in_pins[] = {24, 25, 27};
        for (uint pin : in_pins) {
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            gpio_pull_up(pin);
        }

        SIO_GPIO_OE_SET(0x003FFFFFu & ~((1u << 24) | (1u << 25) | (1u << 27)));
        SIO_GPIO_CLR(PIN_LE_A | PIN_DAT_DIR);
        SIO_GPIO_SET(PIN_LE_D | PIN_RESET);
        SIO_GPIO_SET(CTRL_IDLE | PIN_LE_C);
        DELAY_NOPS(25);
        SIO_GPIO_CLR(PIN_LE_C);
        DELAY_NOPS(15);
        gpio_barrier();
    }

    static uint8_t ReadRaw(int cmd, uint16_t addr) {
        if (addr > 0xC000) return 0xFF;

        SetAddress(addr);
        SIO_GPIO_SET(PIN_DAT_DIR);
        SIO_GPIO_OE_CLR(PIN_DATA_MASK);
        gpio_barrier();

        uint32_t clr_mask = PIN_RD | PIN_LE_D;
        switch (cmd) {
            case RD_SLTSL1: clr_mask |= PIN_MREQ | PIN_SLTSL1 | CsBits(addr); break;
            case RD_SLTSL2: clr_mask |= PIN_MREQ | PIN_SLTSL2 | CsBits(addr); break;
            case RD_MEM:    clr_mask |= PIN_MREQ; break;
            case RD_IO:     clr_mask |= PIN_IORQ; break;
            default: break;
        }

        SIO_GPIO_CLR(clr_mask);
        gpio_barrier();

        SYNC();
        WaitReady();

        uint8_t data = (uint8_t)(SIO_GPIO_IN() & PIN_DATA_MASK);

        SIO_GPIO_SET(CTRL_IDLE | PIN_LE_D);
        DELAY_NOPS(20);
        SIO_GPIO_CLR(PIN_LE_C);
        DELAY_NOPS(10);
        SIO_GPIO_CLR(PIN_DAT_DIR);
        SIO_GPIO_OE_SET(PIN_DATA_MASK);
        gpio_barrier();

        return data;
    }

    static void Write(int cmd, uint16_t addr, uint8_t value) {
        if (addr > 0xC000) return;

        SetAddress(addr);
        SIO_GPIO_OE_SET(PIN_DATA_MASK);
        SIO_GPIO_CLR(PIN_DAT_DIR | PIN_LE_D | PIN_DATA_MASK | PIN_WR);
        SIO_GPIO_SET((uint32_t)value & 0xFFu);

        uint32_t clr_mask = 0;
        switch (cmd) {
            case WR_SLTSL1: clr_mask = PIN_MREQ | PIN_SLTSL1 | CsBits(addr); break;
            case WR_SLTSL2: clr_mask = PIN_MREQ | PIN_SLTSL2 | CsBits(addr); break;
            case WR_MEM:    clr_mask = PIN_MREQ; break;
            case WR_IO:     clr_mask = PIN_IORQ; break;
            default: break;
        }
        if (clr_mask) {
            SIO_GPIO_CLR(clr_mask);
        }
        gpio_barrier();

        PULSE_PIN(PIN_WR);
        SYNC();
        WaitReady();

        SIO_GPIO_SET(CTRL_IDLE | PIN_LE_D);
        DELAY_NOPS(20);
        SIO_GPIO_CLR(PIN_LE_C);
        gpio_barrier();
    }

    static void Reset(int ms) {
        SIO_GPIO_SET(CTRL_IDLE | PIN_LE_C);
        DELAY_NOPS(20);
        SIO_GPIO_CLR(PIN_LE_C);
        SIO_GPIO_CLR(PIN_RESET);
        sleep_ms(ms > 0 ? ms : 50);
        SIO_GPIO_SET(PIN_RESET);
        sleep_ms(20);
        gpio_barrier();
    }

    #undef PIN_DATA_MASK
    #undef PIN_SLTSL1
    #undef PIN_SLTSL2
    #undef PIN_CS2
    #undef PIN_CS1
    #undef PIN_RD
    #undef PIN_WR
    #undef PIN_IORQ
    #undef PIN_MREQ
    #undef PIN_LE_A
    #undef PIN_LE_C
    #undef PIN_LE_D
    #undef PIN_RESET
    #undef PIN_CLK
    #undef PIN_DAT_DIR
    #undef PIN_INT
    #undef PIN_WAIT
    #undef PIN_SW1
    #undef CTRL_IDLE
} // namespace ZemmixHw

/* ==============================================================================
 * Shared Cache & Public API
 * ============================================================================== */

#define USE_SLOT_CACHE 0

#if USE_SLOT_CACHE
static uint8_t s_SlotCache[2][65536];
static volatile uint8_t s_SlotCacheValid[2][65536];
#endif

extern "C" void MsxBus_InvalidateCache(void) {
#if USE_SLOT_CACHE
    memset((void *)s_SlotCacheValid, 0, sizeof(s_SlotCacheValid));
    gpio_barrier();
#endif
}

extern "C" void MsxBus_SetHardware(MsxBusHardware hw) {
    if (hw != MsxBusHwGpio && hw != MsxBusHwZemmix)
        hw = MsxBusHwGpio;
    if (s_hw == hw && s_msxbus_inited)
        return;
    s_hw = hw;
    s_msxbus_inited = false;
    MsxBus_InvalidateCache();
}

extern "C" MsxBusHardware MsxBus_GetHardware(void) {
    return s_hw;
}

extern "C" void MsxBus_ToggleHardware(void) {
    if (s_hw == MsxBusHwGpio) {
        MsxBus_SetHardware(MsxBusHwZemmix);
    } else {
        MsxBus_SetHardware(MsxBusHwGpio);
    }
    MsxBus_Init();
}

extern "C" void MsxBus_Init(void) {
    if (s_msxbus_inited) return;

    if (!s_bus_spinlock) {
        s_bus_spinlock = spin_lock_init(spin_lock_claim_unused(true));
    }

    uint32_t save = spin_lock_blocking(s_bus_spinlock);
    if (!s_msxbus_inited) {
        if (s_hw == MsxBusHwZemmix)
            ZemmixHw::Init();
        else
            GpioHw::Init();
        s_msxbus_inited = true;
        spin_unlock(s_bus_spinlock, save);
        MsxBus_Reset(100);
        return;
    }
    spin_unlock(s_bus_spinlock, save);
}

static uint8_t MsxBus_ReadRaw(int cmd, uint16_t addr) {
    g_GpioAccessCount++;
    if (s_hw == MsxBusHwZemmix)
        return ZemmixHw::ReadRaw(cmd, addr);
    return GpioHw::ReadRaw(cmd, addr);
}

extern "C" void MsxBus_PrefetchWorker(void) {
    // No-op on RP2350
}

extern "C" uint8_t MsxBus_Read(int cmd, uint16_t addr) {
    if (!s_msxbus_inited) MsxBus_Init();

#if USE_SLOT_CACHE
    int slot = (cmd == RD_SLTSL2) ? 1 : (cmd == RD_SLTSL1 ? 0 : -1);
    if (slot >= 0) {
        if (s_SlotCacheValid[slot][addr]) {
            return s_SlotCache[slot][addr];
        }

        uint32_t save = spin_lock_blocking(s_bus_spinlock);
        uint8_t val = MsxBus_ReadRaw(cmd, addr);
        s_SlotCache[slot][addr] = val;
        s_SlotCacheValid[slot][addr] = 1;
        spin_unlock(s_bus_spinlock, save);

        return val;
    }
#endif

    uint32_t save = spin_lock_blocking(s_bus_spinlock);
    uint8_t val = MsxBus_ReadRaw(cmd, addr);
    spin_unlock(s_bus_spinlock, save);
    return val;
}

extern "C" void MsxBus_Write(int cmd, uint16_t addr, uint8_t value) {
    if (!s_msxbus_inited) MsxBus_Init();
    MsxBus_InvalidateCache();
    uint32_t save = spin_lock_blocking(s_bus_spinlock);
    g_GpioAccessCount++;
    if (s_hw == MsxBusHwZemmix)
        ZemmixHw::Write(cmd, addr, value);
    else
        GpioHw::Write(cmd, addr, value);
    spin_unlock(s_bus_spinlock, save);
}

extern "C" void MsxBus_Reset(int ms) {
    MsxBus_InvalidateCache();
    if (!s_msxbus_inited) return;

    uint32_t save = spin_lock_blocking(s_bus_spinlock);
    if (s_hw == MsxBusHwZemmix)
        ZemmixHw::Reset(ms);
    else
        GpioHw::Reset(ms);
    spin_unlock(s_bus_spinlock, save);

    /* blueberry: after reset pulse, poke I/O then probe slot 1 @ 0x4000 */
    MsxBus_Write(WR_IO, 0, 0);
    (void)MsxBus_Read(RD_SLTSL1, 0x4000);
}

extern "C" int MsxBus_IsActive(int slot) {
    if (slot >= 0 && slot < 2) return g_RealSlot[slot];
    return 0;
}
