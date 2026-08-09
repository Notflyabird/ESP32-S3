#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Width of every ASCII glyph in pixels */
#define LCD_FONT_ASC_W  8
/** Width of every Chinese glyph in pixels */
#define LCD_FONT_CN_W   16
/** Height of every glyph in pixels */
#define LCD_FONT_H      16

/**
 * @brief Look up the glyph bitmap for an ASCII character (0x20–0x7E).
 *
 * @param ch          ASCII character.
 * @param out_width   Set to 8 if valid, 0 if not.
 * @return Pointer to 16 bytes of glyph data (top row first, MSB=left),
 *         or NULL if character not available.
 */
const uint8_t *lcd_font_get_glyph(char ch, uint8_t *out_width);

/**
 * @brief Look up the 16x16 bitmap for a Chinese character (Unicode codepoint).
 *        Uses binary search on a sorted codepoint table.
 *
 * @param unicode     Unicode codepoint of the character.
 * @param out_width   Set to 16 if found, 0 if not.
 * @return Pointer to 32 bytes of glyph data (16 rows x 2 bytes/row, MSB=left),
 *         or NULL if character not in the font.
 */
const uint8_t *lcd_font_get_glyph_cn(uint32_t unicode, uint8_t *out_width);

#ifdef __cplusplus
}
#endif
