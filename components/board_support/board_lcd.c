#include "board_lcd.h"
#include "esp_log.h"
#include "tca9554.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"

// #define LCD_TEST_PATTERN_EN

static const char *TAG = "BOARD_LCD";

// Korvo2 v3 LCD configuration from board_def.h
#define LCD_CLK_GPIO                GPIO_NUM_1
#define LCD_MOSI_GPIO               GPIO_NUM_0
#define LCD_DC_GPIO                 GPIO_NUM_2
#define LCD_CTRL_GPIO               BIT(1)  // TCA9554_GPIO_NUM_1
#define LCD_RST_GPIO                BIT(2)  // TCA9554_GPIO_NUM_2
#define LCD_CS_GPIO                 BIT(3)  // TCA9554_GPIO_NUM_3
#define LCD_H_RES                   320
#define LCD_V_RES                   240
#define LCD_SWAP_XY                 (false)
#define LCD_MIRROR_X                (true)
#define LCD_MIRROR_Y                (true)
#define LCD_COLOR_INV               (false)

esp_err_t board_lcd_init(esp_lcd_panel_handle_t *out_panel_handle, void *cb)
{
	esp_err_t ret;

	// init IO expander (tca9554_init will create/configure the new-driver I2C bus)
	esp_tca9554_config_t pca_cfg = {
		.i2c_port = 1,                  /* use I2C port 1 to share with camera */
		.i2c_scl = GPIO_NUM_18,
		.i2c_sda = GPIO_NUM_17,
		.interrupt_output = -1,
	};
	ret = tca9554_init(&pca_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "tca9554_init failed: %s", esp_err_to_name(ret));
		return ret;
	}

	// configure LCD control pins through expander
	tca9554_set_io_config(LCD_CTRL_GPIO, TCA9554_IO_OUTPUT);
	tca9554_set_io_config(LCD_RST_GPIO, TCA9554_IO_OUTPUT);
	tca9554_set_io_config(LCD_CS_GPIO, TCA9554_IO_OUTPUT);
	
	// Ensure all control pins start in a known state
	tca9554_set_output_state(LCD_CTRL_GPIO, TCA9554_IO_LOW);
	tca9554_set_output_state(LCD_RST_GPIO, TCA9554_IO_LOW);
	tca9554_set_output_state(LCD_CS_GPIO, TCA9554_IO_HIGH); // CS high = inactive
	vTaskDelay(10 / portTICK_PERIOD_MS);
	
	// Perform LCD reset sequence
	tca9554_set_output_state(LCD_RST_GPIO, TCA9554_IO_LOW);
	vTaskDelay(10 / portTICK_PERIOD_MS);
	tca9554_set_output_state(LCD_RST_GPIO, TCA9554_IO_HIGH);
	vTaskDelay(100 / portTICK_PERIOD_MS); // Wait for LCD to reset
	
	// Set control pin high and CS low to activate LCD
	tca9554_set_output_state(LCD_CTRL_GPIO, TCA9554_IO_HIGH);
	tca9554_set_output_state(LCD_CS_GPIO, TCA9554_IO_LOW); // CS low = active
	vTaskDelay(10 / portTICK_PERIOD_MS);
	
	// Enable LCD backlight (assume LCD_CTRL_GPIO controls backlight)
	// Keep LCD_CTRL_GPIO high for backlight ON
	tca9554_set_output_state(LCD_CTRL_GPIO, TCA9554_IO_HIGH);

	// initialize SPI bus for LCD
	spi_bus_config_t buscfg = {
		.sclk_io_num = LCD_CLK_GPIO,
		.mosi_io_num = LCD_MOSI_GPIO,
		.miso_io_num = -1,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = LCD_V_RES * LCD_H_RES * 2,
	};
	ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
		return ret;
	}

	esp_lcd_panel_io_spi_config_t io_config = {
		.dc_gpio_num = LCD_DC_GPIO,
		.cs_gpio_num = -1,
		.pclk_hz = 20 * 1000 * 1000,  // 降低到20MHz确保稳定性
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
		.spi_mode = 0,
		.trans_queue_depth = 10,
		.on_color_trans_done = cb,
		.user_ctx = NULL,
	};
	esp_lcd_panel_dev_config_t panel_config = {
		.reset_gpio_num = -1,
		.color_space = ESP_LCD_COLOR_SPACE_BGR,
		.bits_per_pixel = 16,
	};

	esp_lcd_panel_io_handle_t panel_io;
	ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
		return ret;
	}

	esp_lcd_panel_handle_t panel_handle;
	ret = esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel_handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_lcd_new_panel_ili9341 failed: %s", esp_err_to_name(ret));
		return ret;
	}

	// Initialize the panel
	ret = esp_lcd_panel_init(panel_handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(ret));
		return ret;
	}

	// Configure mirror and swap according to board definition
	ret = esp_lcd_panel_mirror(panel_handle, LCD_MIRROR_X, LCD_MIRROR_Y);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_lcd_panel_mirror failed: %s", esp_err_to_name(ret));
		return ret;
	}

	if (LCD_SWAP_XY) {
		ret = esp_lcd_panel_swap_xy(panel_handle, LCD_SWAP_XY);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "esp_lcd_panel_swap_xy failed: %s", esp_err_to_name(ret));
			return ret;
		}
	}

	// Invert colors if needed
	if (LCD_COLOR_INV) {
		ret = esp_lcd_panel_invert_color(panel_handle, LCD_COLOR_INV);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "esp_lcd_panel_invert_color failed: %s", esp_err_to_name(ret));
			return ret;
		}
	}

	// Turn on the display
	ret = esp_lcd_panel_disp_on_off(panel_handle, true);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ESP_LOGI(TAG, "LCD panel initialized and display turned ON (mirror_x=%d, mirror_y=%d, swap_xy=%d, color_inv=%d)",
			LCD_MIRROR_X, LCD_MIRROR_Y, LCD_SWAP_XY, LCD_COLOR_INV);

#if defined(LCD_TEST_PATTERN_EN)
	// Draw a color bar test pattern to verify LCD color channels
	uint16_t *test_buf = (uint16_t *)heap_caps_malloc(LCD_H_RES * 80 * sizeof(uint16_t), MALLOC_CAP_DMA);
	if (test_buf) {
		// 4 color bars: 根据实际测试结果调整
		// 当前显示：绿，蓝，红，白
		// 映射关系：
		// - 发送绿色(0x07E0) → 显示红色
		// - 发送蓝色(0x001F) → 显示绿色
		// - 发送红色(0xF800) → 显示蓝色
		// 所以要显示红、绿、蓝、白，应该发送：
		for (int bar = 0; bar < 4; bar++) {
			uint16_t color;
			switch (bar) {
				case 0: color = 0x07E0; break; // 发送绿色 → 显示红色
				case 1: color = 0x001F; break; // 发送蓝色 → 显示绿色
				case 2: color = 0xF800; break; // 发送红色 → 显示蓝色
				case 3: color = 0xFFFF; break; // White
			}
			for (int i = 0; i < LCD_H_RES * 20; i++) {
				test_buf[bar * LCD_H_RES * 20 + i] = color;
			}
		}
		esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_H_RES, 80, test_buf);
		heap_caps_free(test_buf);
		ESP_LOGI(TAG, "Color test pattern drawn (Red, Green, Blue, White bars)");
		vTaskDelay(500 / portTICK_PERIOD_MS); // Wait longer to see colors
	}
#endif

	*out_panel_handle = panel_handle;
	return ESP_OK;
}

