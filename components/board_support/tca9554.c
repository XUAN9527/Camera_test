/* Minimal TCA9554 driver using IDF i2c_master APIs (moved into board_support)
 * This avoids depending on esp_peripherals/i2c_bus ADF helpers.
 */
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "include/tca9554.h"

/* Define error codes if not already defined (should be in esp_err.h) */
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif

#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG 0x102
#endif

#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif

#ifndef I2C_CLK_SRC_APB
#define I2C_CLK_SRC_APB 0  /* Use APB clock source */
#endif

static const char *TAG = "TCA9554";

static uint16_t tca9554_addr = 0;
static i2c_master_bus_handle_t tca_bus = NULL;
static i2c_master_dev_handle_t tca_dev = NULL;
static bool tca_owns_i2c_port = false;

#define TCA9554_INPUT_PORT              0x00
#define TCA9554_OUTPUT_PORT             0x01
#define TCA9554_POLARITY_INVERSION_PORT 0x02
#define TCA9554_CONFIGURATION_PORT      0x03

static esp_err_t tca9554_write_reg(uint8_t reg_addr, uint8_t data)
{
	uint8_t buf[2] = { reg_addr, data };
	if (!tca_dev) {
		ESP_LOGE(TAG, "Device handle is NULL");
		return ESP_FAIL;
	}
	esp_err_t ret = i2c_master_transmit(tca_dev, buf, sizeof(buf), 500);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Write reg 0x%02x failed: %s", reg_addr, esp_err_to_name(ret));
	}
	return ret;
}

static esp_err_t tca9554_read_reg(uint8_t reg_addr, uint8_t *out)
{
	if (!tca_dev || !out) {
		ESP_LOGE(TAG, "Invalid parameters: dev=%p, out=%p", tca_dev, out);
		return ESP_FAIL;
	}
	esp_err_t ret = i2c_master_transmit_receive(tca_dev, &reg_addr, 1, out, 1, 500);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Read reg 0x%02x failed: %s", reg_addr, esp_err_to_name(ret));
	}
	return ret;
}

esp_err_t tca9554_init(esp_tca9554_config_t *cfg)
{
	esp_err_t ret;
	ESP_LOGI(TAG, "init cfg port=%d scl=%d sda=%d", cfg->i2c_port, cfg->i2c_scl, cfg->i2c_sda);

	/* basic argument validation */
	if (!cfg) {
		ESP_LOGE(TAG, "null configuration pointer");
		return ESP_ERR_INVALID_ARG;
	}

	/* validate port number */
	if (cfg->i2c_port < 0 || cfg->i2c_port >= I2C_NUM_MAX) {
		ESP_LOGE(TAG, "invalid I2C port %d", cfg->i2c_port);
		return ESP_ERR_INVALID_ARG;
	}

	/* Clear previous state if any */
	if (tca_dev) {
		i2c_master_bus_rm_device(tca_dev);
		tca_dev = NULL;
	}
	if (tca_bus && tca_owns_i2c_port) {
		/* Only delete the bus if we created it */
		i2c_del_master_bus(tca_bus);
		tca_bus = NULL;
		tca_owns_i2c_port = false;
	} else if (tca_bus) {
		/* We don't delete the bus here as it might be shared */
		tca_bus = NULL;
	}
	tca9554_addr = 0;

	/* try to reuse an existing I2C bus handle if one has already been created
	   on the specified port (camera, display or another peripheral may have
	   opened it already). */
	ret = i2c_master_get_bus_handle(cfg->i2c_port, &tca_bus);
	if (ret != ESP_OK) {
		ESP_LOGI(TAG, "No existing I2C bus handle found on port %d, creating new bus", cfg->i2c_port);
		i2c_master_bus_config_t bus_cfg = {
			.i2c_port = cfg->i2c_port,
			.sda_io_num = cfg->i2c_sda,
			.scl_io_num = cfg->i2c_scl,
			.glitch_ignore_cnt = 7,
			.intr_priority = 1,
			.trans_queue_depth = 4,
			.clk_source = I2C_CLK_SRC_APB,
			.flags = {.enable_internal_pullup = 1},
		};
		ret = i2c_new_master_bus(&bus_cfg, &tca_bus);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
			return ret;
		}
		tca_owns_i2c_port = true;
		ESP_LOGI(TAG, "Created new I2C bus on port %d", cfg->i2c_port);
	} else {
		tca_owns_i2c_port = false;
		ESP_LOGI(TAG, "Reusing existing I2C bus handle on port %d", cfg->i2c_port);
	}

	/* probe known 7‑bit addresses for TCA9554
	   Common addresses: 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
	   based on A0,A1,A2 pins (all low = 0x20) */
	const uint16_t addrs[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x38, 0x39, 0x3A, 0x3B };
	ESP_LOGI(TAG, "probing TCA9554 addresses 0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x", 
		addrs[0], addrs[1], addrs[2], addrs[3], addrs[4], addrs[5], addrs[6], addrs[7], addrs[8], addrs[9], addrs[10], addrs[11]);
	
	/* Try probing multiple times with delay between attempts */
	for (int attempt = 0; attempt < 3; ++attempt) {
		if (attempt > 0) {
			ESP_LOGW(TAG, "Retry probing TCA9554, attempt %d/3", attempt + 1);
			vTaskDelay(100 / portTICK_PERIOD_MS);
		}
		
		for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); ++i) {
			if (i2c_master_probe(tca_bus, addrs[i], 100) == ESP_OK) {
				tca9554_addr = addrs[i];
				ESP_LOGI(TAG, "Detected IO expander at 0x%02x", tca9554_addr);
				i2c_device_config_t dev_cfg = {
					.dev_addr_length = I2C_ADDR_BIT_LEN_7,
					.device_address = tca9554_addr,
					.scl_speed_hz = 100000,
					.flags = {0},
				};
				ret = i2c_master_bus_add_device(tca_bus, &dev_cfg, &tca_dev);
				if (ret != ESP_OK) {
					ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
					i2c_del_master_bus(tca_bus);
					tca_bus = NULL;
					return ret;
				}
				ESP_LOGI(TAG, "TCA9554 initialized successfully");
				return ESP_OK;
			}
		}
	}
	
	ESP_LOGE(TAG, "TCA9554 not found after 3 attempts");
	/* Don't delete the bus here as it might be shared with camera */
	/* Just clear our local reference */
	tca_bus = NULL;
	return ESP_ERR_NOT_FOUND;
}

esp_err_t tca9554_deinit()
{
	if (tca_dev) {
		i2c_master_bus_rm_device(tca_dev);
		tca_dev = NULL;
	}
	if (tca_bus && tca_owns_i2c_port) {
		i2c_del_master_bus(tca_bus);
		tca_bus = NULL;
		tca_owns_i2c_port = false;
	} else if (tca_bus) {
		/* We don't delete the bus here as it might be shared */
		tca_bus = NULL;
	}
	/* clear address too, avoids stale value if driver is re‑inited */
	tca9554_addr = 0;
	return ESP_OK;
}

esp_tca9554_io_level_t tca9554_get_input_state(esp_tca9554_gpio_num_t gpio_num)
{
	uint8_t data = 0;
	if (gpio_num < TCA9554_GPIO_NUM_MAX) {
		if (tca9554_read_reg(TCA9554_INPUT_PORT, &data) != ESP_OK) return TCA9554_LEVEL_ERROR;
	} else {
		ESP_LOGE(TAG, "gpio num error");
		return TCA9554_LEVEL_ERROR;
	}
	return (data & gpio_num) ? TCA9554_IO_HIGH : TCA9554_IO_LOW;
}

esp_tca9554_io_level_t tca9554_get_output_state(esp_tca9554_gpio_num_t gpio_num)
{
	uint8_t data = 0;
	if (gpio_num < TCA9554_GPIO_NUM_MAX) {
		if (tca9554_read_reg(TCA9554_OUTPUT_PORT, &data) != ESP_OK) return TCA9554_LEVEL_ERROR;
	} else {
		ESP_LOGE(TAG, "gpio num error");
		return TCA9554_LEVEL_ERROR;
	}
	return (data & gpio_num) ? TCA9554_IO_HIGH : TCA9554_IO_LOW;
}

esp_err_t tca9554_set_output_state(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_level_t level)
{
	if (gpio_num >= TCA9554_GPIO_NUM_MAX) {
		ESP_LOGE(TAG, "gpio num error");
		return ESP_ERR_INVALID_ARG;
	}
	uint8_t data = 0;
	if (tca9554_read_reg(TCA9554_OUTPUT_PORT, &data) != ESP_OK) return ESP_FAIL;
	if (level == TCA9554_IO_HIGH) data |= gpio_num; else data &= ~gpio_num;
	return tca9554_write_reg(TCA9554_OUTPUT_PORT, data);
}

esp_err_t tca9554_set_polarity_inversion(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_polarity_t polarity)
{
	if (gpio_num >= TCA9554_GPIO_NUM_MAX) {
		ESP_LOGE(TAG, "gpio num error");
		return ESP_ERR_INVALID_ARG;
	}
	uint8_t data = 0;
	if (tca9554_read_reg(TCA9554_POLARITY_INVERSION_PORT, &data) != ESP_OK) return ESP_FAIL;
	if (polarity == TCA9554_IO_INVERTED) data |= gpio_num; else data &= ~gpio_num;
	return tca9554_write_reg(TCA9554_POLARITY_INVERSION_PORT, data);
}

esp_tca9554_io_config_t tca9554_get_io_config(esp_tca9554_gpio_num_t gpio_num)
{
	if (gpio_num >= TCA9554_GPIO_NUM_MAX) {
		ESP_LOGE(TAG, "gpio num error");
		return TCA9554_IO_RETAINED;
	}
	uint8_t data = 0;
	if (tca9554_read_reg(TCA9554_CONFIGURATION_PORT, &data) != ESP_OK) return TCA9554_IO_RETAINED;
	return (data & gpio_num) ? TCA9554_IO_INPUT : TCA9554_IO_OUTPUT;
}

esp_err_t tca9554_set_io_config(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_config_t io_config)
{
	if (gpio_num >= TCA9554_GPIO_NUM_MAX) {
		ESP_LOGE(TAG, "gpio num error");
		return ESP_ERR_INVALID_ARG;
	}
	uint8_t data = 0;
	if (tca9554_read_reg(TCA9554_CONFIGURATION_PORT, &data) != ESP_OK) return ESP_FAIL;
	if (io_config == TCA9554_IO_INPUT) data |= gpio_num; else data &= ~gpio_num;
	return tca9554_write_reg(TCA9554_CONFIGURATION_PORT, data);
}

void tca9554_read_all()
{
	for (int i = 0; i < 4; i++) {
		uint8_t reg_val = 0;
		if (tca9554_read_reg(i, &reg_val) == ESP_OK) {
			ESP_LOGI(TAG, "REG:%02x, %02x", i, reg_val);
		} else {
			ESP_LOGW(TAG, "REG:%02x read failed", i);
		}
	}
}

