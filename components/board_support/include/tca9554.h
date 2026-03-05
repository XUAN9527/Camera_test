/*
 * MIT Licensed small TCA9554 header (moved into board_support)
 *
 * Note: this component provides a minimal driver that uses the new
 * `i2c_master_*` APIs.  There are several other copies of a TCA9554
 * implementation in the workspace (esp_peripherals/offline and in
 * esp-adf).  Only one of them should be linked to the final binary
 * or you will get duplicate-symbol linker errors.  If you ever import
 * the old driver, rename its header or undefine the conflicting symbols.
 */
#ifndef _BOARD_SUPPORT_TCA9554_H_
#define _BOARD_SUPPORT_TCA9554_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gpio.h"
#include "driver/i2c.h"   /* for i2c_port_t */
#include "esp_err.h"

typedef enum {
    TCA9554_GPIO_NUM_0 = BIT(0),
    TCA9554_GPIO_NUM_1 = BIT(1),
    TCA9554_GPIO_NUM_2 = BIT(2),
    TCA9554_GPIO_NUM_3 = BIT(3),
    TCA9554_GPIO_NUM_4 = BIT(4),
    TCA9554_GPIO_NUM_5 = BIT(5),
    TCA9554_GPIO_NUM_6 = BIT(6),
    TCA9554_GPIO_NUM_7 = BIT(7),
    TCA9554_GPIO_NUM_MAX
} esp_tca9554_gpio_num_t;

typedef enum {
    TCA9554_IO_LOW,
    TCA9554_IO_HIGH,
    TCA9554_LEVEL_ERROR
} esp_tca9554_io_level_t;

typedef enum {
    TCA9554_IO_RETAINED,
    TCA9554_IO_INVERTED
} esp_tca9554_io_polarity_t;

typedef enum {
    TCA9554_IO_OUTPUT,
    TCA9554_IO_INPUT
} esp_tca9554_io_config_t;

typedef struct {
    i2c_port_t  i2c_port;           /* which I2C peripheral to use */
    gpio_num_t  i2c_sda;
    gpio_num_t  i2c_scl;
    gpio_num_t  interrupt_output;
} esp_tca9554_config_t;

esp_err_t tca9554_init(esp_tca9554_config_t *cfg);
/* convenience wrapper used by board_lcd later */

esp_err_t tca9554_deinit(void);
esp_tca9554_io_level_t tca9554_get_input_state(esp_tca9554_gpio_num_t gpio_num);
esp_tca9554_io_level_t tca9554_get_output_state(esp_tca9554_gpio_num_t gpio_num);
esp_err_t tca9554_set_output_state(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_level_t level);
esp_err_t tca9554_set_polarity_inversion(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_polarity_t polarity);
esp_tca9554_io_config_t tca9554_get_io_config(esp_tca9554_gpio_num_t gpio_num);
esp_err_t tca9554_set_io_config(esp_tca9554_gpio_num_t gpio_num, esp_tca9554_io_config_t io_config);
void tca9554_read_all(void);

#ifdef __cplusplus
}
#endif

#endif
