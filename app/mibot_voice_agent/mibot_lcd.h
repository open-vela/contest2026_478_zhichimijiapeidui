/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LCD integration points for the Mibot SF32 application.
 *
 * The default renderer only paints a state color and a small ASCII label.
 * A future animation module can register a callback without changing the
 * UART protocol or state machine.
 */

#ifndef __APPS_EXAMPLES_MIBOT_AGENT_MIBOT_LCD_H
#define __APPS_EXAMPLES_MIBOT_AGENT_MIBOT_LCD_H

#include <stdint.h>

typedef void (*mibot_lcd_animation_cb_t)(void *fbmem,
                                          uint16_t width,
                                          uint16_t height,
                                          uint16_t stride,
                                          uint8_t bpp,
                                          uint32_t tick_ms,
                                          uint8_t state);

int mibot_lcd_set_animation_callback(mibot_lcd_animation_cb_t callback);
int mibot_lcd_set_text(const char *text);

#endif
