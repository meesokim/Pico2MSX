#ifndef ROMDUMP_H
#define ROMDUMP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if the ROM dump UI is currently active (emulator paused).
 */
bool RomDump_IsActive(void);

/**
 * Open the ROM dump view on the specified slot (0 for Slot 1, 1 for Slot 2).
 */
void RomDump_Open(int slot);

/**
 * Close the ROM dump view and resume MSX emulation.
 */
void RomDump_Close(void);

/**
 * Toggle between Slot 1 and Slot 2.
 */
void RomDump_ToggleSlot(void);

/**
 * Perform/refresh the ROM dump read transaction for the current visible page.
 */
void RomDump_Perform(void);

/**
 * Update and redraw the ROM dump screen if dirty.
 */
void RomDump_Update(void);

/**
 * Handle USB keyboard events while ROM dump is active.
 * Returns true if the key was handled.
 */
bool RomDump_HandleKey(int keycode, int code, int codeshifted, int flags, int pressed);

/**
 * Handle gamepad/local pad button clicks while ROM dump is active.
 */
bool RomDump_HandlePad(uint16_t bClick);

#ifdef __cplusplus
}
#endif

#endif // ROMDUMP_H
