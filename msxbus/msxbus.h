/**
 * Real MSX cartridge bus over Raspberry Pi RP2350 SIO (GPIO).
 *
 * Hardware Profiles:
 *   MsxBusHwGpio   — Blueberry GPIO Board (2 slots, MODE multiplexed: 01=LowAddr, 11=HighAddr, 10=Data) - Default
 *   MsxBusHwZemmix — Zemmix Mini / MSX-Pi / RPMP2 40-Pin Latch Board
 */
#ifndef MSXBUS_H
#define MSXBUS_H

#include <stdint.h>
#include <stdbool.h>

#define REAL_CARTRIDGE_SUPPORTED 1

#ifdef __cplusplus
extern "C" {
#endif

/* MSX Bus Commands */
#define RD_SLTSL1 0x00
#define RD_SLTSL2 0x10
#define RD_MEM    0x20
#define RD_IO     0x02
#define WR_SLTSL1 0x01
#define WR_SLTSL2 0x11
#define WR_MEM    0x21
#define WR_IO     0x03
#define RESET_CMD 0x40

typedef enum {
    MsxBusHwGpio   = 0,  /* Blueberry GPIO Board (2 slots, MODE: 01=LowAddr, 11=HighAddr, 10=Data) - Default */
    MsxBusHwZemmix = 1   /* Zemmix Mini 1-Slot Latch Board */
} MsxBusHardware;

extern int g_RealSlot[2];
extern volatile unsigned g_GpioAccessCount;

void MsxBus_SetHardware(MsxBusHardware hw);
MsxBusHardware MsxBus_GetHardware(void);
void MsxBus_ToggleHardware(void);

void MsxBus_Init(void);
void MsxBus_Reset(int ms);
uint8_t MsxBus_Read(int cmd, uint16_t addr);
void MsxBus_Write(int cmd, uint16_t addr, uint8_t value);
int MsxBus_IsActive(int slot);
void MsxBus_InvalidateCache(void);
void MsxBus_PrefetchWorker(void);

#ifdef __cplusplus
}
#endif

#endif // MSXBUS_H
