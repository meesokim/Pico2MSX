/**
 * Real MSX Cartridge Bus Implementation for Raspberry Pi Pico 2 (RP2350).
 *
 * Hardware Profiles:
 *   MsxBusHwGpio   — Blueberry GPIO Board (2 slots, single PIO SM:
 *                    GPIO 0-15 + side-set MODE 01/11/10, hardware /WAIT) - Default
 *   MsxBusHwZemmix — Zemmix Mini / MSX-Pi / RPMP2 40-Pin Latch Board
 */

#include "msxbus.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/structs/sio.h"
#include "msxbus.pio.h"
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

#define SYNC_NOPS_ZEMMIX  80
#define WAIT_TIMEOUT      50
#define PULSE_HIGH_NOPS   16

static inline void PULSE_PIN(uint32_t pin) {
    SIO_GPIO_CLR(pin);
    SIO_GPIO_SET(pin);
    DELAY_NOPS(PULSE_HIGH_NOPS);
    SIO_GPIO_CLR(pin);
    gpio_barrier();
}

/* ==============================================================================
 * Blueberry GPIO Board (2 Slots) — single PIO SM (Default)
 * ==============================================================================
 * Direct PIO Pin Mapping (GPIO 0-16):
 *   GPIO 0-7   : Data / Address Multiplexed Bus (PIO OUT / IN)
 *   GPIO 8     : MODE0 (2-to-4 selector LSB, PIO Side-Set bit 0)
 *   GPIO 9     : MODE1 (2-to-4 selector MSB, PIO Side-Set bit 1)
 *                00 Idle / 01 A0-A7 / 11 A8-A15 / 10 Data
 *                Each selector channel drives the matching 74HC373 LE.
 *   GPIO 10    : /MREQ    (Active LOW, Memory Request)      -> Directly driven by PIO
 *   GPIO 11    : /IORQ    (Active LOW, I/O Request)         -> Directly driven by PIO
 *   GPIO 12    : /RD      (Active LOW, Memory / IO Read)    -> Directly driven by PIO
 *   GPIO 13    : /WR      (Active LOW, Memory / IO Write)   -> Directly driven by PIO
 *   GPIO 14    : /SLTSL1  (Active LOW, Slot 1 Select)       -> Directly driven by PIO
 *   GPIO 15    : /SLTSL2  (Active LOW, Slot 2 Select)       -> Directly driven by PIO
 *   GPIO 16    : /WAIT    (MSX Wait in, Active LOW, Pull-up) -> Directly monitored by PIO 'wait 1 gpio 16'
 *   GPIO 17    : /INT     (MSX Interrupt in, Active LOW, Pull-up)
 *   GPIO 24    : /RESET   (MSX Reset out, Active LOW)
 * ============================================================================== */
namespace GpioHw {
    #define PIN_MREQ_BIT    (1u << 10)
    #define PIN_IORQ_BIT    (1u << 11)
    #define PIN_RD_BIT      (1u << 12)
    #define PIN_WR_BIT      (1u << 13)
    #define PIN_SLTSL1_BIT  (1u << 14)
    #define PIN_SLTSL2_BIT  (1u << 15)
    #define CTRL_IDLE_BITS  (0xFC00u) // GPIO 10-15 all HIGH (1111 1100 0000 0000)

    static PIO  s_pio = pio2;
    static uint s_sm     = 1;  /* pio2 SM0 is PSRAM SPI */
    static uint s_offset = 0;
    static bool s_pio_loaded = false;

    static uint32_t PackTx(uint16_t addr, uint32_t ctrl, uint8_t data) {
        return (uint32_t)addr | ((ctrl | (uint32_t)data) << 16);
    }

    static void ExecEntry(uint entry) {
        msxbus_pio_exec_entry(s_pio, s_sm, s_offset, entry);
    }

    static void Init(void) {
        gpio_init(24);
        gpio_set_dir(24, GPIO_OUT);
        gpio_put(24, 1);

        gpio_init(16);
        gpio_set_dir(16, GPIO_IN);
        gpio_pull_up(16);

        gpio_init(17);
        gpio_set_dir(17, GPIO_IN);
        gpio_pull_up(17);

        if (!s_pio_loaded) {
            pio_sm_claim(s_pio, s_sm);
            s_offset = pio_add_program(s_pio, &msxbus_program);
            s_pio_loaded = true;
        }

        msxbus_pio_init(s_pio, s_sm, s_offset);

        printf("[MsxBus] Hardware: PIO2 SM%u 2-to-4 MODE 01/11/10, 74HC373 (GPIO 0-15)\n",
               s_sm);
    }

    static inline uint32_t GetReadCtrlMask(int cmd) {
        uint32_t ctrl = CTRL_IDLE_BITS;
        switch (cmd) {
            case RD_SLTSL1:
                // Direct PIO control: /SLTSL1=0, /RD=0, /MREQ=0 (Slot 2 remains 1 / inactive)
                ctrl &= ~(PIN_SLTSL1_BIT | PIN_RD_BIT | PIN_MREQ_BIT);
                break;
            case RD_SLTSL2:
                // Direct PIO control: /SLTSL2=0, /RD=0, /MREQ=0 (Slot 1 remains 1 / inactive)
                ctrl &= ~(PIN_SLTSL2_BIT | PIN_RD_BIT | PIN_MREQ_BIT);
                break;
            case RD_MEM:
                ctrl &= ~(PIN_RD_BIT | PIN_MREQ_BIT);
                break;
            case RD_IO:
                ctrl &= ~(PIN_RD_BIT | PIN_IORQ_BIT);
                break;
            default:
                break;
        }
        return ctrl;
    }

    static inline uint32_t GetWriteCtrlMask(int cmd) {
        uint32_t ctrl = CTRL_IDLE_BITS;
        switch (cmd) {
            case WR_SLTSL1:
                // Direct PIO control: /SLTSL1=0, /WR=0, /MREQ=0
                ctrl &= ~(PIN_SLTSL1_BIT | PIN_WR_BIT | PIN_MREQ_BIT);
                break;
            case WR_SLTSL2:
                // Direct PIO control: /SLTSL2=0, /WR=0, /MREQ=0
                ctrl &= ~(PIN_SLTSL2_BIT | PIN_WR_BIT | PIN_MREQ_BIT);
                break;
            case WR_MEM:
                ctrl &= ~(PIN_WR_BIT | PIN_MREQ_BIT);
                break;
            case WR_IO:
                ctrl &= ~(PIN_WR_BIT | PIN_IORQ_BIT);
                break;
            default:
                break;
        }
        return ctrl;
    }

    static void DataBusHiZ(bool enable) {
        for (uint i = 0; i < 8; i++) {
            if (enable) {
                gpio_set_oeover(i, GPIO_OVERRIDE_LOW);
                gpio_set_input_enabled(i, true);
                gpio_disable_pulls(i);
            } else {
                gpio_set_oeover(i, GPIO_OVERRIDE_NORMAL);
            }
        }
    }

    static uint8_t ReadRaw(int cmd, uint16_t addr) {
        uint32_t ctrl = GetReadCtrlMask(cmd);
        ExecEntry(msxbus_offset_entry_read);
        pio_sm_put_blocking(s_pio, s_sm, PackTx(addr, ctrl, 0));
        (void)pio_sm_get_blocking(s_pio, s_sm); /* address latched */
        DataBusHiZ(true);
        pio_sm_put_blocking(s_pio, s_sm, 0); /* /RD, MODE=10 data */
        (void)pio_sm_get_blocking(s_pio, s_sm); /* control held */
        DELAY_NOPS(40);
        gpio_barrier();
        uint8_t data = (uint8_t)(SIO_GPIO_IN() & 0xFFu);
        pio_sm_put_blocking(s_pio, s_sm, 0); /* restore */
        msxbus_pio_wait_pull(s_pio, s_sm, s_offset);
        DataBusHiZ(false);
        return data;
    }

    static void Write(int cmd, uint16_t addr, uint8_t value) {
        uint32_t ctrl = GetWriteCtrlMask(cmd);
        ExecEntry(msxbus_offset_entry_write);
        pio_sm_put_blocking(s_pio, s_sm, PackTx(addr, ctrl, value));
        msxbus_pio_wait_pull(s_pio, s_sm, s_offset);
    }

    static void Reset(int ms) {
        gpio_put(24, 0); // Assert /RESET
        sleep_ms(ms > 0 ? ms : 50);
        gpio_put(24, 1); // Deassert /RESET
        sleep_ms(20);
        gpio_barrier();
    }

    #undef PIN_MREQ_BIT
    #undef PIN_IORQ_BIT
    #undef PIN_RD_BIT
    #undef PIN_WR_BIT
    #undef PIN_SLTSL1_BIT
    #undef PIN_SLTSL2_BIT
    #undef CTRL_IDLE_BITS
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
