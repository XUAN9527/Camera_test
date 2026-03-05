// Board LCD helper (IDF-only)
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

/**
 * Initialize the LCD panel and return a panel handle.
 * @param out_panel_handle: pointer to receive created `esp_lcd_panel_handle_t`.
 * @param cb: optional callback passed to esp_lcd panel io config (may be NULL).
 */
esp_err_t board_lcd_init(esp_lcd_panel_handle_t *out_panel_handle, void *cb);
