/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021, Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board.h"
#include "tusb.h"
#include "kbd.h"
#include "platform_config.h"
#include "display/emuapi.h"
#include <string.h>

#define MAX_GAMEPAD_REPORTS 4

typedef struct {
    bool valid;
    uint8_t report_id;
    uint16_t report_bits;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t hat_offset;
    uint16_t dpad_up_offset;
    uint16_t dpad_down_offset;
    uint16_t dpad_left_offset;
    uint16_t dpad_right_offset;
    uint16_t button_offset;
    uint8_t x_size;
    uint8_t y_size;
    uint8_t hat_size;
    uint8_t dpad_size;
    uint8_t button_size;
    uint8_t button_count;
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
} gamepad_report_info_t;

static struct {
    uint8_t report_count;
    gamepad_report_info_t reports[MAX_GAMEPAD_REPORTS];
} gamepad_info[CFG_TUH_HID];

#define USB_VID_SONY 0x054c
#define USB_PID_DUALSHOCK3 0x0268

static uint8_t ps3_enable_report[4] = {0x42, 0x0c, 0x00, 0x00};

/* ==================  Keycode translation table. ==================== */

/*
 * We only need one translation table, as this simple application only
 * supports 102-key US layouts. We need separate translations for
 * non-shifted and shifted keys because shift is not just a modifier:
 * shift turns a '5' to a '$', for example. We don't need a separate
 * column for 'ctrl', because we can handle that simply by logical
 * AND operations on the keycode. Alt/Meta does not affect the keycode
 * at all -- it's just a flag. 
 */ 

// TODO: fill in the rest of the non-ASCII key codes
// There's a nice list here:
// https://gist.github.com/ekaitz-zarraga/2b25b94b711684ba4e969e5a5723969b
static int conv_table_uk[128][2] = 
{
    {0     , 0      }, /* 0x00 */ \
    {0     , 0      }, /* 0x01 */ \
    {0     , 0      }, /* 0x02 */ \
    {0     , 0      }, /* 0x03 */ \
    {'a'   , 'A'    }, /* 0x04 */ \
    {'b'   , 'B'    }, /* 0x05 */ \
    {'c'   , 'C'    }, /* 0x06 */ \
    {'d'   , 'D'    }, /* 0x07 */ \
    {'e'   , 'E'    }, /* 0x08 */ \
    {'f'   , 'F'    }, /* 0x09 */ \
    {'g'   , 'G'    }, /* 0x0a */ \
    {'h'   , 'H'    }, /* 0x0b */ \
    {'i'   , 'I'    }, /* 0x0c */ \
    {'j'   , 'J'    }, /* 0x0d */ \
    {'k'   , 'K'    }, /* 0x0e */ \
    {'l'   , 'L'    }, /* 0x0f */ \
    {'m'   , 'M'    }, /* 0x10 */ \
    {'n'   , 'N'    }, /* 0x11 */ \
    {'o'   , 'O'    }, /* 0x12 */ \
    {'p'   , 'P'    }, /* 0x13 */ \
    {'q'   , 'Q'    }, /* 0x14 */ \
    {'r'   , 'R'    }, /* 0x15 */ \
    {'s'   , 'S'    }, /* 0x16 */ \
    {'t'   , 'T'    }, /* 0x17 */ \
    {'u'   , 'U'    }, /* 0x18 */ \
    {'v'   , 'V'    }, /* 0x19 */ \
    {'w'   , 'W'    }, /* 0x1a */ \
    {'x'   , 'X'    }, /* 0x1b */ \
    {'y'   , 'Y'    }, /* 0x1c */ \
    {'z'   , 'Z'    }, /* 0x1d */ \
    {'1'   , '!'    }, /* 0x1e */ \
    {'2'   , '@'    }, /* 0x1f */ \
    {'3'   , '#'    }, /* 0x20 */ \
    {'4'   , '$'    }, /* 0x21 */ \
    {'5'   , '%'    }, /* 0x22 */ \
    {'6'   , '^'    }, /* 0x23 */ \
    {'7'   , '&'    }, /* 0x24 */ \
    {'8'   , '*'    }, /* 0x25 */ \
    {'9'   , '('    }, /* 0x26 */ \
    {'0'   , ')'    }, /* 0x27 */ \
    {KBD_KEY_ENTER  , KBD_KEY_ENTER   }, /* 0x28 */ \
    {KBD_KEY_ESC,  KBD_KEY_ESC }, /* 0x29 */ \
    {KBD_KEY_BS  , KBD_KEY_BS }, /* 0x2a */ \
    {'\t'  , '\t'   }, /* 0x2b */ \
    {' '   , ' '    }, /* 0x2c */ \
    {'-'   , '_'    }, /* 0x2d */ \
    {'='   , '+'    }, /* 0x2e */ \
    {'['   , '{'    }, /* 0x2f */ \
    {']'   , '}'    }, /* 0x30 */ \
    {'\\'  , '|'    }, /* 0x31 */ \
    {'#'   , '~'    }, /* 0x32 */ \
    {';'   , ':'    }, /* 0x33 */ \
    {'\''  , '\"'   }, /* 0x34 */ \
    {'`'   , '~'    }, /* 0x35 */ \
    {','   , '<'    }, /* 0x36 */ \
    {'.'   , '>'    }, /* 0x37 */ \
    {'/'   , '?'    }, /* 0x38 */ \
    {KBD_KEY_CAPS , KBD_KEY_CAPS      }, /* 0x39 */ \
    {KBD_KEY_F1 , KBD_KEY_F1}, /* 0x3a */ \
    {KBD_KEY_F2 , KBD_KEY_F2}, /* 0x3b */ \
    {KBD_KEY_F3 , KBD_KEY_F3}, /* 0x3c */ \
    {KBD_KEY_F4 , KBD_KEY_F4}, /* 0x3d */ \
    {KBD_KEY_F5 , KBD_KEY_F5}, /* 0x3e */ \
    {KBD_KEY_F6 , KBD_KEY_F6}, /* 0x3f */ \
    {KBD_KEY_F7 , KBD_KEY_F7}, /* 0x40 */ \
    {KBD_KEY_F8 , KBD_KEY_F8}, /* 0x41 */ \
    {KBD_KEY_F9 , KBD_KEY_F9}, /* 0x42 */ \
    {KBD_KEY_F10 , KBD_KEY_F10}, /* 0x43 */ \
    {KBD_KEY_F11 , KBD_KEY_F11}, /* 0x44 */ \
    {KBD_KEY_F12 , KBD_KEY_F12}, /* 0x45 */ \
    {0     , 0      }, /* 0x46 */ \
    {0     , 0      }, /* 0x47 */ \
    {0     , 0      }, /* 0x48 */ \
    {0     , 0      }, /* 0x49 */ \
    {KBD_KEY_HOME , KBD_KEY_HOME}, /* 0x4a */ \
    {KBD_KEY_PGUP , KBD_KEY_PGUP}, /* 0x4b */ \
    {0     , 0      }, /* 0x4c */ \
    {KBD_KEY_END, KBD_KEY_END}, /* 0x4d */ \
    {KBD_KEY_PGDN , KBD_KEY_PGDN }, /* 0x4e */ \
    {KBD_KEY_RIGHT, KBD_KEY_RIGHT }, /* 0x4f */ \
    {KBD_KEY_LEFT, KBD_KEY_LEFT }, /* 0x50 */ \
    {KBD_KEY_DOWN, KBD_KEY_DOWN }, /* 0x51 */ \
    {KBD_KEY_UP, KBD_KEY_UP }, /* 0x52 */ \
    { 0   , 0       }, /* 0x53 */ \
                                  \
    { 0   , 0       }, /* 0x54 */ \
    { 0   , 0       }, /* 0x55 */ \
    { 0   , 0       }, /* 0x56 */ \
    { 0   , 0       }, /* 0x57 */ \
    { 0   , 0       }, /* 0x58 */ \
    { 0   , 0       }, /* 0x59 */ \
    { 0   , 0       }, /* 0x5a */ \
    { 0   , 0       }, /* 0x5b */ \
    { 0   , 0       }, /* 0x5c */ \
    { 0   , 0       }, /* 0x5d */ \
    { 0   , 0       }, /* 0x5e */ \
    { 0   , 0       }, /* 0x5f */ \
    { 0   , 0       }, /* 0x60 */ \
    { 0   , 0       }, /* 0x61 */ \
    { 0   , 0       }, /* 0x62 */ \
    { 0   , 0       }, /* 0x63 */ \
    { 0   , 0       }, /* 0x64 */ \
    { 0   , 0       }, /* 0x65 */ \
    { 0   , 0       }, /* 0x66 */ \
    { 0   , 0       }, /* 0x67 */ \
};



static int conv_table_be[128][2] = 
{
    {0     , 0      }, /* 0x00 */ \
    {0     , 0      }, /* 0x01 */ \
    {0     , 0      }, /* 0x02 */ \
    {0     , 0      }, /* 0x03 */ \
    {'q'   , 'Q'    }, /* 0x04 */ \
    {'b'   , 'B'    }, /* 0x05 */ \
    {'c'   , 'C'    }, /* 0x06 */ \
    {'d'   , 'D'    }, /* 0x07 */ \
    {'e'   , 'E'    }, /* 0x08 */ \
    {'f'   , 'F'    }, /* 0x09 */ \
    {'g'   , 'G'    }, /* 0x0a */ \
    {'h'   , 'H'    }, /* 0x0b */ \
    {'i'   , 'I'    }, /* 0x0c */ \
    {'j'   , 'J'    }, /* 0x0d */ \
    {'k'   , 'K'    }, /* 0x0e */ \
    {'l'   , 'L'    }, /* 0x0f */ \
    {','   , '?'    }, /* 0x10 */ \
    {'n'   , 'N'    }, /* 0x11 */ \
    {'o'   , 'O'    }, /* 0x12 */ \
    {'p'   , 'P'    }, /* 0x13 */ \
    {'a'   , 'A'    }, /* 0x14 */ \
    {'r'   , 'R'    }, /* 0x15 */ \
    {'s'   , 'S'    }, /* 0x16 */ \
    {'t'   , 'T'    }, /* 0x17 */ \
    {'u'   , 'U'    }, /* 0x18 */ \
    {'v'   , 'V'    }, /* 0x19 */ \
    {'z'   , 'Z'    }, /* 0x1a */ \
    {'x'   , 'X'    }, /* 0x1b */ \
    {'y'   , 'Y'    }, /* 0x1c */ \
    {'w'   , 'W'    }, /* 0x1d */ \
    {'1'   , '&'    }, /* 0x1e */ \
    {'2'   , '@'    }, /* 0x1f */ \
    {'3'   , '\"'   }, /* 0x20 */ \
    {'4'   , '\''   }, /* 0x21 */ \
    {'5'   , '('    }, /* 0x22 */ \
    {'6'   , '#'      }, /* 0x23 */ \
    {'7'   , '*'      }, /* 0x24 */ \
    {'8'   , '!'    }, /* 0x25 */ \
    {'9'   , '{'    }, /* 0x26 */ \
    {'0'   , '}'    }, /* 0x27 */ \
    {KBD_KEY_ENTER  , KBD_KEY_ENTER   }, /* 0x28 */ \
    {KBD_KEY_ESC,  KBD_KEY_ESC }, /* 0x29 ESC*/ \
    {KBD_KEY_BS  , KBD_KEY_BS }, /* 0x2a */ \
    {'\t'  , '\t'   }, /* 0x2b */ \
    {' '   , ' '    }, /* 0x2c */ \
    {')'   , 0      }, /* 0x2d */ \
    {'-'   , '_'    }, /* 0x2e */ \
    {'^'   , '['    }, /* 0x2f */ \
    {'$'   , ']'    }, /* 0x30 */ \
    {0     , 0      }, /* 0x31 */ \
    {0     , 0      }, /* 0x32 */ \
    {'m'   , 'M'    }, /* 0x33 */ \
    {0     , '%'    }, /* 0x34 */ \
    {0     , 0      }, /*{'`'   , '~'    },*/ /* 0x35 */ \
    {';'   , '.'    }, /* 0x36 */ \
    {':'   , '/'    }, /* 0x37 */ \
    {'='   , '+'    }, /* 0x38 */ \
    {KBD_KEY_CAPS , KBD_KEY_CAPS      }, /* 0x39 */ \
    {KBD_KEY_F1 , KBD_KEY_F1}, /* 0x3a */ \
    {KBD_KEY_F2 , KBD_KEY_F2}, /* 0x3b */ \
    {KBD_KEY_F3 , KBD_KEY_F3}, /* 0x3c */ \
    {KBD_KEY_F4 , KBD_KEY_F4}, /* 0x3d */ \
    {KBD_KEY_F5 , KBD_KEY_F5}, /* 0x3e */ \
    {KBD_KEY_F6 , KBD_KEY_F6}, /* 0x3f */ \
    {KBD_KEY_F7 , KBD_KEY_F7}, /* 0x40 */ \
    {KBD_KEY_F8 , KBD_KEY_F8}, /* 0x41 */ \
    {KBD_KEY_F9 , KBD_KEY_F9}, /* 0x42 */ \
    {KBD_KEY_F10 , KBD_KEY_F10}, /* 0x43 */ \
    {KBD_KEY_F11 , KBD_KEY_F11}, /* 0x44 */ \
    {KBD_KEY_F12 , KBD_KEY_F12}, /* 0x45 */ \
    {0     , 0      }, /* 0x46 */ \
    {0     , 0      }, /* 0x47 */ \
    {0     , 0      }, /* 0x48 */ \
    {0     , 0      }, /* 0x49 */ \
    {KBD_KEY_HOME , KBD_KEY_HOME}, /* 0x4a */ \
    {KBD_KEY_PGUP , KBD_KEY_PGUP}, /* 0x4b */ \
    {0     , 0      }, /* 0x4c */ \
    {KBD_KEY_END, KBD_KEY_END}, /* 0x4d */ \
    {KBD_KEY_PGDN , KBD_KEY_PGDN }, /* 0x4e */ \
    {KBD_KEY_RIGHT, KBD_KEY_RIGHT }, /* 0x4f */ \
    {KBD_KEY_LEFT, KBD_KEY_LEFT }, /* 0x50 */ \
    {KBD_KEY_DOWN, KBD_KEY_DOWN }, /* 0x51 */ \
    {KBD_KEY_UP, KBD_KEY_UP }, /* 0x52 */ \
    { 0   , 0       }, /* 0x53 */ \
    { 0   , 0       }, /* 0x54 */ \
    { 0   , 0       }, /* 0x55 */ \
    { 0   , 0       }, /* 0x56 */ \
    { 0   , 0       }, /* 0x57 */ \
    { 0   , 0       }, /* 0x58 */ \
    { 0   , 0       }, /* 0x59 */ \
    { 0   , 0       }, /* 0x5a */ \
    { 0   , 0       }, /* 0x5b */ \
    { 0   , 0       }, /* 0x5c */ \
    { 0   , 0       }, /* 0x5d */ \
    { 0   , 0       }, /* 0x5e */ \
    { 0   , 0       }, /* 0x5f */ \
    { 0   , 0       }, /* 0x60 */ \
    { 0   , 0       }, /* 0x61 */ \
    { 0   , 0       }, /* 0x62 */ \
    { 0   , 0       }, /* 0x63 */ \
    { '<' , '>'     }, /* 0x64 */ \
    { 0   , 0       }, /* 0x65 */ \
    { 0   , 0       }, /* 0x66 */ \
    { 0   , 0       }, /* 0x67 */ \
};

static int conv_table_es[128][2] = 
{
    {0     , 0      }, /* 0x00 */ \
    {0     , 0      }, /* 0x01 */ \
    {0     , 0      }, /* 0x02 */ \
    {0     , 0      }, /* 0x03 */ \
    {'a'   , 'A'    }, /* 0x04 */ \
    {'b'   , 'B'    }, /* 0x05 */ \
    {'c'   , 'C'    }, /* 0x06 */ \
    {'d'   , 'D'    }, /* 0x07 */ \
    {'e'   , 'E'    }, /* 0x08 */ \
    {'f'   , 'F'    }, /* 0x09 */ \
    {'g'   , 'G'    }, /* 0x0a */ \
    {'h'   , 'H'    }, /* 0x0b */ \
    {'i'   , 'I'    }, /* 0x0c */ \
    {'j'   , 'J'    }, /* 0x0d */ \
    {'k'   , 'K'    }, /* 0x0e */ \
    {'l'   , 'L'    }, /* 0x0f */ \
    {'m'   , 'M'    }, /* 0x10 */ \
    {'n'   , 'N'    }, /* 0x11 */ \
    {'o'   , 'O'    }, /* 0x12 */ \
    {'p'   , 'P'    }, /* 0x13 */ \
    {'q'   , 'Q'    }, /* 0x14 */ \
    {'r'   , 'R'    }, /* 0x15 */ \
    {'s'   , 'S'    }, /* 0x16 */ \
    {'t'   , 'T'    }, /* 0x17 */ \
    {'u'   , 'U'    }, /* 0x18 */ \
    {'v'   , 'V'    }, /* 0x19 */ \
    {'w'   , 'W'    }, /* 0x1a */ \
    {'x'   , 'X'    }, /* 0x1b */ \
    {'y'   , 'Y'    }, /* 0x1c */ \
    {'z'   , 'Z'    }, /* 0x1d */ \
    {'1'   , '!'    }, /* 0x1e */ \
    {'2'   , '\"'   }, /* 0x1f */ \
    {'3'   , '#'    }, /* 0x20 */ \
    {'4'   , '$'    }, /* 0x21 */ \
    {'5'   , '%'    }, /* 0x22 */ \
    {'6'   , '&'    }, /* 0x23 */ \
    {'7'   , '/'    }, /* 0x24 */ \
    {'8'   , '('    }, /* 0x25 */ \
    {'9'   , ')'    }, /* 0x26 */ \
    {'0'   , '='    }, /* 0x27 */ \
    {KBD_KEY_ENTER  , KBD_KEY_ENTER   }, /* 0x28 */ \
    {KBD_KEY_ESC,  KBD_KEY_ESC }, /* 0x29 */ \
    {KBD_KEY_BS  , KBD_KEY_BS }, /* 0x2a */ \
    {'\t'  , '\t'   }, /* 0x2b */ \
    {' '   , ' '    }, /* 0x2c */ \
    {'\''  , '?'    }, /* 0x2d */ \
    {'¡'   , '¿'    }, /* 0x2e */ \
    {'`'   , '^'    }, /* 0x2f */ \
    {'+'   , '*'    }, /* 0x30 */ \
    {'ç'   , 'Ç'    }, /* 0x31 */ \
    {0     , 0      }, /* 0x32 */ \
    {'ñ'   , 'Ñ'    }, /* 0x33 */ \
    {';'   , ':'    }, /* 0x34 */ \
    {0     , 0      }, /* 0x35 */ \
    {','   , ';'    }, /* 0x36 */ \
    {'.'   , ':'    }, /* 0x37 */ \
    {'-'   , '_'    }, /* 0x38 */ \
    {KBD_KEY_CAPS , KBD_KEY_CAPS      }, /* 0x39 */ \
    {KBD_KEY_F1 , KBD_KEY_F1}, /* 0x3a */ \
    {KBD_KEY_F2 , KBD_KEY_F2}, /* 0x3b */ \
    {KBD_KEY_F3 , KBD_KEY_F3}, /* 0x3c */ \
    {KBD_KEY_F4 , KBD_KEY_F4}, /* 0x3d */ \
    {KBD_KEY_F5 , KBD_KEY_F5}, /* 0x3e */ \
    {KBD_KEY_F6 , KBD_KEY_F6}, /* 0x3f */ \
    {KBD_KEY_F7 , KBD_KEY_F7}, /* 0x40 */ \
    {KBD_KEY_F8 , KBD_KEY_F8}, /* 0x41 */ \
    {KBD_KEY_F9 , KBD_KEY_F9}, /* 0x42 */ \
    {KBD_KEY_F10 , KBD_KEY_F10}, /* 0x43 */ \
    {KBD_KEY_F11 , KBD_KEY_F11}, /* 0x44 */ \
    {KBD_KEY_F12 , KBD_KEY_F12}, /* 0x45 */ \
    {0     , 0      }, /* 0x46 */ \
    {0     , 0      }, /* 0x47 */ \
    {0     , 0      }, /* 0x48 */ \
    {0     , 0      }, /* 0x49 */ \
    {KBD_KEY_HOME , KBD_KEY_HOME}, /* 0x4a */ \
    {KBD_KEY_PGUP , KBD_KEY_PGUP}, /* 0x4b */ \
    {0     , 0      }, /* 0x4c */ \
    {KBD_KEY_END, KBD_KEY_END}, /* 0x4d */ \
    {KBD_KEY_PGDN , KBD_KEY_PGDN }, /* 0x4e */ \
    {KBD_KEY_RIGHT, KBD_KEY_RIGHT }, /* 0x4f */ \
    {KBD_KEY_LEFT, KBD_KEY_LEFT }, /* 0x50 */ \
    {KBD_KEY_DOWN, KBD_KEY_DOWN }, /* 0x51 */ \
    {KBD_KEY_UP, KBD_KEY_UP }, /* 0x52 */ \
    { 0   , 0       }, /* 0x53 */ \
                                  \
    { 0   , 0       }, /* 0x54 */ \
    { 0   , 0       }, /* 0x55 */ \
    { 0   , 0       }, /* 0x56 */ \
    { 0   , 0       }, /* 0x57 */ \
    { 0   , 0       }, /* 0x58 */ \
    { 0   , 0       }, /* 0x59 */ \
    { 0   , 0       }, /* 0x5a */ \
    { 0   , 0       }, /* 0x5b */ \
    { 0   , 0       }, /* 0x5c */ \
    { 0   , 0       }, /* 0x5d */ \
    { 0   , 0       }, /* 0x5e */ \
    { 0   , 0       }, /* 0x5f */ \
    { 0   , 0       }, /* 0x60 */ \
    { 0   , 0       }, /* 0x61 */ \
    { 0   , 0       }, /* 0x62 */ \
    { 0   , 0       }, /* 0x63 */ \
    { '<' , '>'     }, /* 0x64 */ \
    { 0   , 0       }, /* 0x65 */ \
    { 0   , 0       }, /* 0x66 */ \
    { 0   , 0       }, /* 0x67 */ \
};

static int conv_table_us[128][2] = 
{
    {0     , 0      }, /* 0x00 */ \
    {0     , 0      }, /* 0x01 */ \
    {0     , 0      }, /* 0x02 */ \
    {0     , 0      }, /* 0x03 */ \
    {'a'   , 'A'    }, /* 0x04 */ \
    {'b'   , 'B'    }, /* 0x05 */ \
    {'c'   , 'C'    }, /* 0x06 */ \
    {'d'   , 'D'    }, /* 0x07 */ \
    {'e'   , 'E'    }, /* 0x08 */ \
    {'f'   , 'F'    }, /* 0x09 */ \
    {'g'   , 'G'    }, /* 0x0a */ \
    {'h'   , 'H'    }, /* 0x0b */ \
    {'i'   , 'I'    }, /* 0x0c */ \
    {'j'   , 'J'    }, /* 0x0d */ \
    {'k'   , 'K'    }, /* 0x0e */ \
    {'l'   , 'L'    }, /* 0x0f */ \
    {'m'   , 'M'    }, /* 0x10 */ \
    {'n'   , 'N'    }, /* 0x11 */ \
    {'o'   , 'O'    }, /* 0x12 */ \
    {'p'   , 'P'    }, /* 0x13 */ \
    {'q'   , 'Q'    }, /* 0x14 */ \
    {'r'   , 'R'    }, /* 0x15 */ \
    {'s'   , 'S'    }, /* 0x16 */ \
    {'t'   , 'T'    }, /* 0x17 */ \
    {'u'   , 'U'    }, /* 0x18 */ \
    {'v'   , 'V'    }, /* 0x19 */ \
    {'w'   , 'W'    }, /* 0x1a */ \
    {'x'   , 'X'    }, /* 0x1b */ \
    {'y'   , 'Y'    }, /* 0x1c */ \
    {'z'   , 'Z'    }, /* 0x1d */ \
    {'1'   , '!'    }, /* 0x1e */ \
    {'2'   , '@'    }, /* 0x1f */ \
    {'3'   , '#'    }, /* 0x20 */ \
    {'4'   , '$'    }, /* 0x21 */ \
    {'5'   , '%'    }, /* 0x22 */ \
    {'6'   , '^'    }, /* 0x23 */ \
    {'7'   , '&'    }, /* 0x24 */ \
    {'8'   , '*'    }, /* 0x25 */ \
    {'9'   , '('    }, /* 0x26 */ \
    {'0'   , ')'    }, /* 0x27 */ \
    {KBD_KEY_ENTER  , KBD_KEY_ENTER   }, /* 0x28 */ \
    {KBD_KEY_ESC,  KBD_KEY_ESC }, /* 0x29 */ \
    {KBD_KEY_BS  , KBD_KEY_BS }, /* 0x2a */ \
    {'\t'  , '\t'   }, /* 0x2b */ \
    {' '   , ' '    }, /* 0x2c */ \
    {'-'   , '_'    }, /* 0x2d */ \
    {'='   , '+'    }, /* 0x2e */ \
    {'['   , '{'    }, /* 0x2f */ \
    {']'   , '}'    }, /* 0x30 */ \
    {'\\'  , '|'    }, /* 0x31 */ \
    {0     , 0      }, /* 0x32 (non-US #, not on US kbd) */ \
    {';'   , ':'    }, /* 0x33 */ \
    {'\''  , '\"'   }, /* 0x34 */ \
    {'`'   , '~'    }, /* 0x35 */ \
    {','   , '<'    }, /* 0x36 */ \
    {'.'   , '>'    }, /* 0x37 */ \
    {'/'   , '?'    }, /* 0x38 */ \
    {KBD_KEY_CAPS , KBD_KEY_CAPS      }, /* 0x39 */ \
    {KBD_KEY_F1 , KBD_KEY_F1}, /* 0x3a */ \
    {KBD_KEY_F2 , KBD_KEY_F2}, /* 0x3b */ \
    {KBD_KEY_F3 , KBD_KEY_F3}, /* 0x3c */ \
    {KBD_KEY_F4 , KBD_KEY_F4}, /* 0x3d */ \
    {KBD_KEY_F5 , KBD_KEY_F5}, /* 0x3e */ \
    {KBD_KEY_F6 , KBD_KEY_F6}, /* 0x3f */ \
    {KBD_KEY_F7 , KBD_KEY_F7}, /* 0x40 */ \
    {KBD_KEY_F8 , KBD_KEY_F8}, /* 0x41 */ \
    {KBD_KEY_F9 , KBD_KEY_F9}, /* 0x42 */ \
    {KBD_KEY_F10 , KBD_KEY_F10}, /* 0x43 */ \
    {KBD_KEY_F11 , KBD_KEY_F11}, /* 0x44 */ \
    {KBD_KEY_F12 , KBD_KEY_F12}, /* 0x45 */ \
    {0     , 0      }, /* 0x46 */ \
    {0     , 0      }, /* 0x47 */ \
    {0     , 0      }, /* 0x48 */ \
    {0     , 0      }, /* 0x49 */ \
    {KBD_KEY_HOME , KBD_KEY_HOME}, /* 0x4a */ \
    {KBD_KEY_PGUP , KBD_KEY_PGUP}, /* 0x4b */ \
    {0     , 0      }, /* 0x4c */ \
    {KBD_KEY_END, KBD_KEY_END}, /* 0x4d */ \
    {KBD_KEY_PGDN , KBD_KEY_PGDN }, /* 0x4e */ \
    {KBD_KEY_RIGHT, KBD_KEY_RIGHT }, /* 0x4f */ \
    {KBD_KEY_LEFT, KBD_KEY_LEFT }, /* 0x50 */ \
    {KBD_KEY_DOWN, KBD_KEY_DOWN }, /* 0x51 */ \
    {KBD_KEY_UP, KBD_KEY_UP }, /* 0x52 */ \
    { 0   , 0       }, /* 0x53 */ \
    { 0   , 0       }, /* 0x54 */ \
    { 0   , 0       }, /* 0x55 */ \
    { 0   , 0       }, /* 0x56 */ \
    { 0   , 0       }, /* 0x57 */ \
    { 0   , 0       }, /* 0x58 */ \
    { 0   , 0       }, /* 0x59 */ \
    { 0   , 0       }, /* 0x5a */ \
    { 0   , 0       }, /* 0x5b */ \
    { 0   , 0       }, /* 0x5c */ \
    { 0   , 0       }, /* 0x5d */ \
    { 0   , 0       }, /* 0x5e */ \
    { 0   , 0       }, /* 0x5f */ \
    { 0   , 0       }, /* 0x60 */ \
    { 0   , 0       }, /* 0x61 */ \
    { 0   , 0       }, /* 0x62 */ \
    { 0   , 0       }, /* 0x63 */ \
    { 0   , 0       }, /* 0x64 */ \
    { 0   , 0       }, /* 0x65 */ \
    { 0   , 0       }, /* 0x66 */ \
    { 0   , 0       }, /* 0x67 */ \
};

static KLAYOUT klayout = KEYLAYOUT;


/* =================  End keycode translation table. ================== */

/*===========================================================================
 * is_key_held 
 * Check whether the current key scancode is a repetition of the previous
 * one. One USB report can contain multiple keystrokes, if one key is pressed
 * before another is released. As each new key is pressed, a new report
 * is delivered that contains the keystrokes of the set of keys that
 * are down. As a result, the same keystroke can be found in multiple
 * successive reports.  
 * ========================================================================*/
static inline bool is_key_held (hid_keyboard_report_t const *report, uint8_t keycode)
{
  for (uint8_t i=0; i < 6; i++) 
  {
    if (report->keycode[i] == keycode) 
      return true;
  }
  return false;
}

/*===========================================================================
 * process_kbd_report 
 * Process a report from a keyboard device. The report will contain up
 * to 6 keystrokes, each representing a different key that is down.
 * We need to filter duplicate keystrokes that arise when multiple keys
 * are pressed such that one key is pressed before another is released.
 * This is quite common when typing quickly on a decent keyboard.  
 * ========================================================================*/
static void process_kbd_report (hid_keyboard_report_t const *report)
{
  static hid_keyboard_report_t prev_report = { 0, 0, {0} };
  
  // Debug: print when any key is pressed (limit to first 5 to avoid spam)
  static uint8_t debug_counter = 0;
  if (report->keycode[0] != 0 && debug_counter < 5) {
    printf("KBD: modifier=0x%02X, keys:", report->modifier);
    for (int i = 0; i < 6; i++) {
      if (report->keycode[i]) printf(" %02X", report->keycode[i]);
    }
    printf("\r\n");
    debug_counter++;
  }
  
  bool is_lshift_pressed = report->modifier 
    & KEYBOARD_MODIFIER_LEFTSHIFT;
  bool is_lctrl_pressed = report->modifier 
    & KEYBOARD_MODIFIER_LEFTCTRL;
  bool is_lalt_pressed = report->modifier 
    & KEYBOARD_MODIFIER_LEFTALT;  
  bool is_rshift_pressed = report->modifier 
    & (KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool is_rctrl_pressed = report->modifier 
    & KEYBOARD_MODIFIER_RIGHTCTRL;
  bool is_ralt_pressed = report->modifier 
    & KEYBOARD_MODIFIER_RIGHTALT;
  
  // Any shift key pressed
  bool is_shift_pressed = is_lshift_pressed || is_rshift_pressed;
  
  // Process RELEASED keys BEFORE PRESSED keys so that in a single report
  // transition (e.g. RIGHT→LEFT) the new PRESSED event is the last one
  // to set iusbhk — otherwise RELEASED would clear the new key's code.
  for (uint8_t i=0; i < 6; i++) 
  {
    if (prev_report.keycode[i]) 
    {
      bool found = is_key_held (report, prev_report.keycode[i]);
      if (!found) 
      {
        int ch; 
        int chshift;
        if (klayout == KLAYOUT_UK) {
          ch = conv_table_uk[prev_report.keycode[i]][0];
          chshift = conv_table_uk[prev_report.keycode[i]][(is_shift_pressed?1:0)];
        }
        else if (klayout == KLAYOUT_BE) {
          ch = conv_table_be[prev_report.keycode[i]][0];
          chshift = conv_table_be[prev_report.keycode[i]][(is_shift_pressed?1:0)];
        }
        else if (klayout == KLAYOUT_ES) {
          ch = conv_table_es[prev_report.keycode[i]][0];
          chshift = conv_table_es[prev_report.keycode[i]][(is_shift_pressed?1:0)];
        }
        else { /* KLAYOUT_US */
          ch = conv_table_us[prev_report.keycode[i]][0];
          chshift = conv_table_us[prev_report.keycode[i]][(is_shift_pressed?1:0)];
        }
        int flags = 0;
        if (is_lshift_pressed) flags |= KBD_FLAG_LSHIFT;
        if (is_lctrl_pressed) flags |= KBD_FLAG_LCONTROL;
        if (is_lalt_pressed) flags |= KBD_FLAG_LALT;
        if (is_rshift_pressed) flags |= KBD_FLAG_RSHIFT;
        if (is_rctrl_pressed) flags |= KBD_FLAG_RCONTROL;
        if (is_ralt_pressed) flags |= KBD_FLAG_RALT;        
        kbd_signal_raw_key(prev_report.keycode[i], ch, chshift, flags, KEY_RELEASED);
      } 
    }
  } 
  for (uint8_t i=0; i < 6; i++) 
  {
    if (report->keycode[i]) 
    {
      bool found = is_key_held (&prev_report, report->keycode[i]);

      if (!found) 
      {
        int ch; 
        int chshift;
        if (klayout == KLAYOUT_UK) {
          ch = conv_table_uk[report->keycode[i]][0];
          chshift = conv_table_uk[report->keycode[i]][(is_shift_pressed?1:0)];
        }
        else if (klayout == KLAYOUT_BE) {
          ch = conv_table_be[report->keycode[i]][0];
          chshift = conv_table_be[report->keycode[i]][(is_shift_pressed?1:0)];
        }
        else if (klayout == KLAYOUT_ES) {
          ch = conv_table_es[report->keycode[i]][0];
          chshift = conv_table_es[report->keycode[i]][(is_shift_pressed?1:0)];
        }
        else { /* KLAYOUT_US */
          ch = conv_table_us[report->keycode[i]][0];
          chshift = conv_table_us[report->keycode[i]][(is_shift_pressed?1:0)];
        }

        int flags = 0;
        if (is_lshift_pressed) flags |= KBD_FLAG_LSHIFT;
        if (is_lctrl_pressed) flags |= KBD_FLAG_LCONTROL;
        if (is_lalt_pressed) flags |= KBD_FLAG_LALT;
        if (is_rshift_pressed) flags |= KBD_FLAG_RSHIFT;
        if (is_rctrl_pressed) flags |= KBD_FLAG_RCONTROL;
        if (is_ralt_pressed) flags |= KBD_FLAG_RALT;
        kbd_signal_raw_key(report->keycode[i], ch, chshift, flags, KEY_PRESSED);
        //printf("key=%d\n", report->keycode[i]);
        //printf("flags=%d\n", flags);
      }
    }
  }
  // Capture previous modifier before updating prev_report
  uint8_t prev_mod = prev_report.modifier;
  prev_report = *report;

  // Notify emulator when only modifier keys change (e.g., holding/releasing Shift alone)
  if (prev_mod != report->modifier) {
    int flags = 0;
    if (report->modifier & KEYBOARD_MODIFIER_LEFTSHIFT)  flags |= KBD_FLAG_LSHIFT;
    if (report->modifier & KEYBOARD_MODIFIER_LEFTCTRL)   flags |= KBD_FLAG_LCONTROL;
    if (report->modifier & KEYBOARD_MODIFIER_LEFTALT)    flags |= KBD_FLAG_LALT;
    if (report->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) flags |= KBD_FLAG_RSHIFT;
    if (report->modifier & KEYBOARD_MODIFIER_RIGHTCTRL)  flags |= KBD_FLAG_RCONTROL;
    if (report->modifier & KEYBOARD_MODIFIER_RIGHTALT)   flags |= KBD_FLAG_RALT;

    // When any modifier is non-zero, consider it a press; when none, a release
    int pressed = (report->modifier != 0) ? KEY_PRESSED : KEY_RELEASED;
    // keycode/code/codeshifted are 0 to indicate a pure modifier update
    kbd_signal_raw_key(0, 0, 0, flags, pressed);
  }
}

static uint32_t extract_bits(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t value = 0;
    if (bit_size > 32) bit_size = 32;

    for (uint8_t i = 0; i < bit_size; i++) {
        uint16_t bit = bit_offset + i;
        if (report[bit >> 3] & (1u << (bit & 7))) {
            value |= 1u << i;
        }
    }

    return value;
}

static int32_t extract_signed_bits(const uint8_t *report, uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t value = extract_bits(report, bit_offset, bit_size);
    if (bit_size > 0 && bit_size < 32 && (value & (1u << (bit_size - 1)))) {
        value |= ~((1u << bit_size) - 1u);
    }
    return (int32_t)value;
}

static int32_t hid_item_data(const uint8_t *p, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++) {
        value |= ((uint32_t)p[i]) << (8 * i);
    }

    if (size == 1 && (value & 0x80)) return (int32_t)(value | 0xffffff00u);
    if (size == 2 && (value & 0x8000)) return (int32_t)(value | 0xffff0000u);
    return (int32_t)value;
}

static gamepad_report_info_t *find_or_add_gamepad_report(uint8_t instance, uint8_t report_id)
{
    for (uint8_t i = 0; i < gamepad_info[instance].report_count; i++) {
        if (gamepad_info[instance].reports[i].report_id == report_id) {
            return &gamepad_info[instance].reports[i];
        }
    }

    if (gamepad_info[instance].report_count >= MAX_GAMEPAD_REPORTS) {
        return NULL;
    }

    gamepad_report_info_t *info = &gamepad_info[instance].reports[gamepad_info[instance].report_count++];
    memset(info, 0, sizeof(*info));
    info->report_id = report_id;
    return info;
}

static void parse_gamepad_report_descriptor(uint8_t instance, const uint8_t *desc_report, uint16_t desc_len)
{
    if (instance >= CFG_TUH_HID) return;
    memset(&gamepad_info[instance], 0, sizeof(gamepad_info[instance]));
    if (!desc_report || desc_len == 0) return;

    uint16_t bit_offset[16] = {0};
    uint32_t usages[16] = {0};
    uint8_t usage_count = 0;
    int32_t usage_min = -1;
    int32_t usage_max = -1;
    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    uint8_t report_size = 0;
    uint8_t report_count = 0;
    uint8_t report_id = 0;

    for (uint16_t pos = 0; pos < desc_len;) {
        uint8_t prefix = desc_report[pos++];
        if (prefix == 0xfe) {
            if (pos + 1 >= desc_len) break;
            uint8_t long_size = desc_report[pos++];
            pos++;
            pos += long_size;
            continue;
        }

        uint8_t size_code = prefix & 0x03;
        uint8_t size = (size_code == 3) ? 4 : size_code;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0f;
        if (pos + size > desc_len) break;

        int32_t data = hid_item_data(&desc_report[pos], size);
        pos += size;

        if (type == 1) {
            switch (tag) {
                case 0x0: usage_page = (uint16_t)data; break;
                case 0x1: logical_min = data; break;
                case 0x2: logical_max = data; break;
                case 0x7: report_size = (uint8_t)data; break;
                case 0x8:
                    report_id = (uint8_t)data;
                    if (report_id < 16) bit_offset[report_id] = 0;
                    break;
                case 0x9: report_count = (uint8_t)data; break;
            }
        } else if (type == 2) {
            switch (tag) {
                case 0x0:
                    if (usage_count < 16) usages[usage_count++] = ((uint32_t)usage_page << 16) | (uint16_t)data;
                    break;
                case 0x1: usage_min = data; break;
                case 0x2: usage_max = data; break;
            }
        } else if (type == 0 && tag == 0x8) {
            bool constant = (data & 0x01) != 0;
            uint16_t *offset = (report_id < 16) ? &bit_offset[report_id] : &bit_offset[0];
            gamepad_report_info_t *info = find_or_add_gamepad_report(instance, report_id);

            if (info && !constant) {
                for (uint8_t i = 0; i < report_count; i++) {
                    uint32_t usage = 0;
                    if (usage_min >= 0 && usage_max >= usage_min) {
                        usage = ((uint32_t)usage_page << 16) | (uint16_t)(usage_min + i);
                    } else if (i < usage_count) {
                        usage = usages[i];
                    }

                    uint16_t field_offset = *offset + (uint16_t)i * report_size;
                    uint16_t page = usage >> 16;
                    uint16_t id = usage & 0xffff;

                    if (page == HID_USAGE_PAGE_DESKTOP && id == 0x30) {
                        info->x_offset = field_offset;
                        info->x_size = report_size;
                        info->x_min = logical_min;
                        info->x_max = logical_max;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x31) {
                        info->y_offset = field_offset;
                        info->y_size = report_size;
                        info->y_min = logical_min;
                        info->y_max = logical_max;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x39) {
                        info->hat_offset = field_offset;
                        info->hat_size = report_size;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x90) {
                        info->dpad_up_offset = field_offset;
                        info->dpad_size = report_size;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x91) {
                        info->dpad_down_offset = field_offset;
                        info->dpad_size = report_size;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x92) {
                        info->dpad_right_offset = field_offset;
                        info->dpad_size = report_size;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_DESKTOP && id == 0x93) {
                        info->dpad_left_offset = field_offset;
                        info->dpad_size = report_size;
                        info->valid = true;
                    } else if (page == HID_USAGE_PAGE_BUTTON && info->button_count < 32) {
                        if (info->button_count == 0) {
                            info->button_offset = field_offset;
                            info->button_size = report_size;
                        }
                        info->button_count++;
                        info->valid = true;
                    }
                }
            }

            if (offset) *offset += (uint16_t)report_size * report_count;
            if (info) info->report_bits = *offset;
            usage_count = 0;
            usage_min = -1;
            usage_max = -1;
        } else if (type == 0 && (tag == 0x0a || tag == 0x0c)) {
            usage_count = 0;
            usage_min = -1;
            usage_max = -1;
        }
    }
}

static gamepad_report_info_t *find_gamepad_report_info(uint8_t instance, const uint8_t **report, uint16_t *len)
{
    if (instance >= CFG_TUH_HID || gamepad_info[instance].report_count == 0) return NULL;

    if (gamepad_info[instance].report_count == 1 && gamepad_info[instance].reports[0].report_id == 0) {
        return &gamepad_info[instance].reports[0];
    }

    if (*len == 0) return NULL;
    uint8_t report_id = (*report)[0];
    for (uint8_t i = 0; i < gamepad_info[instance].report_count; i++) {
        if (gamepad_info[instance].reports[i].report_id == report_id) {
            (*report)++;
            (*len)--;
            return &gamepad_info[instance].reports[i];
        }
    }
    return NULL;
}

static void apply_hat(uint8_t hat, uint16_t *decoded_report, uint8_t instance)
{
    uint16_t up    = (instance == 0) ? MASK_JOY2_UP    : MASK_JOY1_UP;
    uint16_t down  = (instance == 0) ? MASK_JOY2_DOWN  : MASK_JOY1_DOWN;
    uint16_t left  = (instance == 0) ? MASK_JOY2_LEFT  : MASK_JOY1_LEFT;
    uint16_t right = (instance == 0) ? MASK_JOY2_RIGHT : MASK_JOY1_RIGHT;

    switch (hat) {
        case GAMEPAD_HAT_UP:         *decoded_report |= up;           break;
        case GAMEPAD_HAT_UP_RIGHT:   *decoded_report |= up | left;    break;
        case GAMEPAD_HAT_RIGHT:      *decoded_report |= left;         break;
        case GAMEPAD_HAT_DOWN_RIGHT: *decoded_report |= down | left;  break;
        case GAMEPAD_HAT_DOWN:       *decoded_report |= down;         break;
        case GAMEPAD_HAT_DOWN_LEFT:  *decoded_report |= down | right; break;
        case GAMEPAD_HAT_LEFT:       *decoded_report |= right;        break;
        case GAMEPAD_HAT_UP_LEFT:    *decoded_report |= up | right;   break;
    }
}

static bool process_parsed_gamepad_report(uint8_t instance, const uint8_t *report, uint16_t len)
{
    gamepad_report_info_t *info = find_gamepad_report_info(instance, &report, &len);
    if (!info || !info->valid) return false;
    if (((uint32_t)len * 8u) < info->report_bits) return false;

    uint16_t decoded_report = 0;

    uint16_t mask_btn   = (instance == 0) ? MASK_JOY2_BTN   : MASK_JOY1_BTN;
    uint16_t mask_up    = (instance == 0) ? MASK_JOY2_UP    : MASK_JOY1_UP;
    uint16_t mask_down  = (instance == 0) ? MASK_JOY2_DOWN  : MASK_JOY1_DOWN;
    uint16_t mask_left  = (instance == 0) ? MASK_JOY2_LEFT  : MASK_JOY1_LEFT;
    uint16_t mask_right = (instance == 0) ? MASK_JOY2_RIGHT : MASK_JOY1_RIGHT;

    if (info->x_size) {
        int32_t x = (info->x_min < 0) ? extract_signed_bits(report, info->x_offset, info->x_size)
                                      : (int32_t)extract_bits(report, info->x_offset, info->x_size);
        int32_t center = (info->x_min + info->x_max) / 2;
        int32_t deadzone = (info->x_max - info->x_min) / 4;
        if (x < center - deadzone) decoded_report |= mask_right;
        if (x > center + deadzone) decoded_report |= mask_left;
    }

    if (info->y_size) {
        int32_t y = (info->y_min < 0) ? extract_signed_bits(report, info->y_offset, info->y_size)
                                      : (int32_t)extract_bits(report, info->y_offset, info->y_size);
        int32_t center = (info->y_min + info->y_max) / 2;
        int32_t deadzone = (info->y_max - info->y_min) / 4;
        if (y < center - deadzone) decoded_report |= mask_up;
        if (y > center + deadzone) decoded_report |= mask_down;
    }

    if (info->hat_size) {
        uint8_t hat = (uint8_t)extract_bits(report, info->hat_offset, info->hat_size);
        printf("[HAT] instance=%d value=%d\r\n", instance, hat);
        apply_hat(hat, &decoded_report, instance);
    }

    if (info->dpad_size) {
        if (extract_bits(report, info->dpad_up_offset,    info->dpad_size)) decoded_report |= mask_up;
        if (extract_bits(report, info->dpad_down_offset,  info->dpad_size)) decoded_report |= mask_down;
        if (extract_bits(report, info->dpad_left_offset,  info->dpad_size)) decoded_report |= mask_left;
        if (extract_bits(report, info->dpad_right_offset, info->dpad_size)) decoded_report |= mask_right;
    }

    for (uint8_t i = 0; i < info->button_count; i++) {
        uint16_t offset = info->button_offset + (uint16_t)i * info->button_size;
        if (extract_bits(report, offset, info->button_size)) {
            //printf("[BTN] instance=%d index=%d pressed\r\n", instance, i);
            if      (i == 15)             decoded_report |= mask_btn;        // A버튼
            else if (i == 14)          decoded_report |= MASK_KEY_USER3; //B버튼
            else if (i == 13)          decoded_report |= MASK_KEY_USER2;
            else if (i == 1)             decoded_report |= MASK_KEY_USER2;
            else if (i == 2)             decoded_report |= mask_btn;        // A버튼
            else if (i == 0)          decoded_report |= MASK_KEY_USER4; //B버튼
            else if (i == 8  || i == 10) decoded_report |= MASK_KEY_USER1;  // L2, L1
            else if (i == 9  || i == 11) decoded_report |= MASK_KEY_USER3;  // R2, R1
            else if (i == 4)             decoded_report |= mask_up;
            else if (i == 5)             decoded_report |= mask_left;
            else if (i == 6)             decoded_report |= mask_down;
            else if (i == 7)             decoded_report |= mask_right;
        }
    }

    kbd_signal_raw_gamepad(instance, decoded_report);
    return true;
}

static void process_gamepad_report(uint8_t instance, const uint8_t *report, uint16_t len) {
    if (process_parsed_gamepad_report(instance, report, len)) {
        return;
    }

    uint16_t decoded_report = 0;

    uint16_t mask_btn   = (instance == 0) ? MASK_JOY2_BTN   : MASK_JOY1_BTN;
    uint16_t mask_up    = (instance == 0) ? MASK_JOY2_UP    : MASK_JOY1_UP;
    uint16_t mask_down  = (instance == 0) ? MASK_JOY2_DOWN  : MASK_JOY1_DOWN;
    uint16_t mask_left  = (instance == 0) ? MASK_JOY2_LEFT  : MASK_JOY1_LEFT;
    uint16_t mask_right = (instance == 0) ? MASK_JOY2_RIGHT : MASK_JOY1_RIGHT;

    // Directional Controls
    if (len > 0 && report[0] < 0x40) decoded_report |= mask_right;
    if (len > 0 && report[0] > 0xB0) decoded_report |= mask_left;
    if (len > 1 && report[1] < 0x40) decoded_report |= mask_up;
    if (len > 1 && report[1] > 0xB0) decoded_report |= mask_down;

    // A Button
    if (len > 5 && ((report[5] & 0x20) || (report[5] & 0x10))) decoded_report |= MASK_KEY_USER3;
    // B Button
    if (len > 5 && ((report[5] & 0x40) || (report[5] & 0x80))) decoded_report |= mask_btn;
    // Select
    if (len > 6 && report[6] & 0x10) decoded_report |= MASK_KEY_USER1;
    // Start
    if (len > 6 && report[6] & 0x20) decoded_report |= MASK_KEY_USER2;

    kbd_signal_raw_gamepad(instance, decoded_report);
}

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

void hid_app_task(void)
{
  // nothing to do
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  uint16_t vid, pid;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  printf("HID device address = %d, instance = %d is mounted\r\n", dev_addr, instance);
  printf("VID = %04x, PID = %04x\r\n", vid, pid);

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
  printf("Interface protocol: %d\r\n", itf_protocol);
  if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
    parse_gamepad_report_descriptor(instance, desc_report, desc_len);
    printf("Gamepad HID reports parsed: %u, descriptor len=%u\r\n",
           instance < CFG_TUH_HID ? gamepad_info[instance].report_count : 0,
           desc_len);
  }

  if (vid == USB_VID_SONY && pid == USB_PID_DUALSHOCK3) {
    bool ps3_ok = tuh_hid_set_report(dev_addr, instance, 0xf4,
                                     HID_REPORT_TYPE_FEATURE,
                                     ps3_enable_report,
                                     sizeof(ps3_enable_report));
    printf("PS3 controller enable report = %d\r\n", ps3_ok);
  }

  // NOTE: We do NOT call tuh_hid_set_protocol() here because TinyUSB
  // already calls SET_PROTOCOL internally during hidh_set_config()
  // (before mount_cb fires). We control which protocol is set via
  // tuh_hid_set_default_protocol() called before tuh_init().

  bool report_ok = tuh_hid_receive_report(dev_addr, instance);
  printf("tuh_hid_receive_report() = %d\r\n", report_ok);

  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD)
  {
    printf("Keyboard found\n");
  } else {
    printf("not a keyboard found (but asking for reports anyway!)\n");
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
  printf("HID device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
  if (instance < CFG_TUH_HID) {
    memset(&gamepad_info[instance], 0, sizeof(gamepad_info[instance]));
  }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len)
{
  printf("HID set report complete: instance=%d id=0x%02x type=%d len=%u\r\n",
         instance, report_id, report_type, len);
  if (report_id == 0xf4 && report_type == HID_REPORT_TYPE_FEATURE && len > 0) {
    bool report_ok = tuh_hid_receive_report(dev_addr, instance);
    printf("PS3 tuh_hid_receive_report() = %d\r\n", report_ok);
  }
}

// Invoked when received report from device via interrupt endpoint
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  // In principle we don't need to test that this USB report came from
  //   a keyboard, since we are only asking for reports from keyboards.
  // But, for future expansion, we should be systematic
  int proto = tuh_hid_interface_protocol (dev_addr, instance);

  // Debug: print first report from each instance to see what we're getting
  static uint8_t first_report_printed[CFG_TUH_HID] = {0};
  if (instance < CFG_TUH_HID && !first_report_printed[instance] && len > 0) {
    printf("First report from instance %d (proto=%d, len=%d): ", instance, proto, len);
    for (int i = 0; i < (len < 16 ? len : 16); i++) {
      printf("%02X ", report[i]);
    }
    printf("\r\n");
    first_report_printed[instance] = 1;
  }

  // Periodic alive counter — every 100 reports, log that we're still receiving
  static uint32_t report_count[CFG_TUH_HID] = {0};
  if (instance < CFG_TUH_HID) report_count[instance]++;
  if (instance < CFG_TUH_HID && (report_count[instance] % 100) == 0) {
    printf("[USB] Report received: instance=%d, count=%u, len=%d, proto=%d\r\n",
           instance, report_count[instance], len, proto);
  }

  switch (proto)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      if (len >= 8) {
        process_kbd_report ((hid_keyboard_report_t const*) report);
      }
      break;
    case HID_ITF_PROTOCOL_NONE:
      if (len == 8) {
        // Standard 8-byte keyboard report (modifier, reserved, 6 keycodes)
        process_kbd_report ((hid_keyboard_report_t const*) report);
      } else if (len == 9 && report[0] == 1) {
        // 8-byte keyboard report with Report ID = 1 prefix
        process_kbd_report ((hid_keyboard_report_t const*) (report + 1));
      } else if (len > 0) {
        process_gamepad_report(instance, report, len);
      }
      break;
    default:
      if (len == 8) {
        process_kbd_report ((hid_keyboard_report_t const*) report);
      } else if (len == 9 && report[0] == 1) {
        process_kbd_report ((hid_keyboard_report_t const*) (report + 1));
      }
      break;
  }

  // Ask the device for the next report -- asking for a report is a
  //   one-off operation, and must be repeated by the application.
  tuh_hid_receive_report (dev_addr, instance);

  /*
  if ( is_sony_ds4(dev_addr) )
  {
    process_sony_ds4(report, len);
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    printf("Error: cannot request to receive report\r\n");
  }
  */
}

void kbd_set_locale(KLAYOUT layout)
{
  klayout = layout;
}
