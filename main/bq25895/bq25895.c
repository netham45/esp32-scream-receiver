/**
 * @file bq25895.c
 * @brief Driver for BQ25895 battery charger IC
 */

#include "bq25895.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bq25895";

// I2C handle
static i2c_port_t i2c_port = I2C_NUM_0;
static bool is_initialized = false;

// Default configuration
static bq25895_config_t config = {
    .i2c_port = I2C_NUM_0,
    .i2c_freq = 400000,
    .sda_gpio = -1,
    .scl_gpio = -1,
    .int_gpio = -1,
    .stat_gpio = -1,
};

/**
 * @brief Read a register from the BQ25895
 */
esp_err_t bq25895_read_reg(bq25895_reg_t reg, uint8_t *value)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25895_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25895_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, value, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register 0x%02x, err = %d (%s)", reg, ret, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Write a register to the BQ25895
 */
esp_err_t bq25895_write_reg(bq25895_reg_t reg, uint8_t value)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25895_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02x with value 0x%02x, err = %d (%s)", reg, value, ret, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Scan I2C bus for devices
 */
static esp_err_t bq25895_scan_i2c_bus(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus for devices...");
    uint8_t devices_found = 0;
    
    for (uint8_t i = 1; i < 128; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", i);
            devices_found++;
        }
    }
    
    if (devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices found on the bus");
        return ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGI(TAG, "Found %d I2C devices on the bus", devices_found);
        return ESP_OK;
    }
}

/**
 * @brief Initialize the BQ25895 driver
 */
esp_err_t bq25895_init(const bq25895_config_t *cfg)
{
    if (is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->sda_gpio < 0 || cfg->scl_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Save configuration
    config = *cfg;
    i2c_port = cfg->i2c_port;

    ESP_LOGI(TAG, "Initializing BQ25895 on I2C port %d (SDA: %d, SCL: %d, freq: %d Hz)",
             cfg->i2c_port, cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_freq);

    // Configure I2C
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = cfg->i2c_freq,
    };

    esp_err_t ret = i2c_param_config(i2c_port, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters, err = %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    // Check if I2C driver is already installed
    ret = i2c_driver_install(i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C driver already installed, trying to delete and reinstall");
        i2c_driver_delete(i2c_port);
        ret = i2c_driver_install(i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2C driver, err = %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    // Scan I2C bus for devices
    ret = bq25895_scan_i2c_bus();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No I2C devices found on the bus, but continuing anyway");
    }

    // Mark as initialized so we can use read/write functions
    is_initialized = true;

    // Check if BQ25895 is present
    uint8_t value;
    ret = bq25895_read_reg(BQ25895_REG_14, &value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID, err = %d (%s)", ret, esp_err_to_name(ret));
        is_initialized = false;
        i2c_driver_delete(i2c_port);
        return ret;
    }

    // Check device ID (bits 5:3 should be 111 for BQ25895)
    uint8_t device_id = (value >> 3) & 0x07;
    if (device_id != 0x07) {
        ESP_LOGE(TAG, "Invalid device ID: 0x%02x (expected 0x07), register value: 0x%02x", device_id, value);
        is_initialized = false;
        i2c_driver_delete(i2c_port);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "BQ25895 initialized successfully, device ID: 0x%02x, register value: 0x%02x", device_id, value);

    // Reset watchdog timer
    ret = bq25895_reset_watchdog();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset watchdog timer, err = %d (%s)", ret, esp_err_to_name(ret));
    }

    return ESP_OK;
}

/**
 * @brief Deinitialize the BQ25895 driver
 */
esp_err_t bq25895_deinit(void)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_driver_delete(i2c_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete I2C driver, err = %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    is_initialized = false;
    return ESP_OK;
}

/**
 * @brief Reset the BQ25895 to default settings
 */
esp_err_t bq25895_reset(void)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Set bit 7 of REG14 to reset the device
    uint8_t value = 0x80;
    esp_err_t ret = bq25895_write_reg(BQ25895_REG_14, value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset device, err = %d (%s)", ret, esp_err_to_name(ret));
        return ret;
    }

    // Wait for reset to complete
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

/**
 * @brief Reset the watchdog timer
 */
esp_err_t bq25895_reset_watchdog(void)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Read current value of REG03
    uint8_t value;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_03, &value);
    if (ret != ESP_OK) {
        return ret;
    }

    // Set bit 6 (WD_RST) to reset the watchdog timer
    value |= (1 << 6);
    ret = bq25895_write_reg(BQ25895_REG_03, value);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Get the current status of the BQ25895
 */
esp_err_t bq25895_get_status(bq25895_status_t *status)
{
    if (!is_initialized || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_0b, reg_0c, reg_0e, reg_0f, reg_10, reg_11, reg_12, reg_13;
    esp_err_t ret;

    // Read status registers
    ret = bq25895_read_reg(BQ25895_REG_0B, &reg_0b);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_0C, &reg_0c);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_0E, &reg_0e);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_0F, &reg_0f);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_10, &reg_10);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_11, &reg_11);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_12, &reg_12);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_13, &reg_13);
    if (ret != ESP_OK) return ret;

    // Parse status registers
    status->vbus_stat = (reg_0b >> 5) & 0x07;
    status->chg_stat = (reg_0b >> 3) & 0x03;
    status->pg_stat = (reg_0b >> 2) & 0x01;
    status->sdp_stat = (reg_0b >> 1) & 0x01;
    status->vsys_stat = reg_0b & 0x01;

    status->watchdog_fault = (reg_0c >> 7) & 0x01;
    status->boost_fault = (reg_0c >> 6) & 0x01;
    status->chg_fault = (reg_0c >> 4) & 0x03;
    status->bat_fault = (reg_0c >> 3) & 0x01;
    status->ntc_fault = reg_0c & 0x07;

    status->therm_stat = (reg_0e >> 7) & 0x01;

    // Calculate voltages and currents
    status->bat_voltage = 2.304f + ((reg_0e & 0x7F) * 0.02f);
    status->sys_voltage = 2.304f + ((reg_0f & 0x7F) * 0.02f);
    status->ts_voltage = 0.21f + ((reg_10 & 0x7F) * 0.00465f);
    status->vbus_voltage = 2.6f + ((reg_11 & 0x7F) * 0.1f);
    status->charge_current = (reg_12 & 0x7F) * 0.05f;

    return ESP_OK;
}

/**
 * @brief Get the current charge parameters of the BQ25895
 */
esp_err_t bq25895_get_charge_params(bq25895_charge_params_t *params)
{
    if (!is_initialized || params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_00, reg_01, reg_02, reg_03, reg_04, reg_05, reg_06, reg_07, reg_08, reg_09, reg_0a, reg_0d;
    esp_err_t ret;

    // Read parameter registers
    ret = bq25895_read_reg(BQ25895_REG_00, &reg_00);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_01, &reg_01);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_02, &reg_02);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_03, &reg_03);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_04, &reg_04);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_05, &reg_05);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_06, &reg_06);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_07, &reg_07);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_08, &reg_08);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_09, &reg_09);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_0A, &reg_0a);
    if (ret != ESP_OK) return ret;

    ret = bq25895_read_reg(BQ25895_REG_0D, &reg_0d);
    if (ret != ESP_OK) return ret;

    // Parse parameter registers
    params->enable_hi_impedance = (reg_00 >> 7) & 0x01;
    params->input_current_limit_ma = 100 + ((reg_00 & 0x3F) * 50);

    params->input_voltage_limit_mv = 3900;
    if ((reg_0d >> 7) & 0x01) {
        // Absolute VINDPM
        params->input_voltage_limit_mv = 2600 + ((reg_0d & 0x7F) * 100);
    } else {
        // Relative VINDPM
        uint8_t vindpm_offset = reg_01 & 0x1F;
        params->input_voltage_limit_mv = 3900 + (vindpm_offset * 100);
    }

    params->enable_charging = (reg_03 >> 4) & 0x01;
    params->enable_otg = (reg_03 >> 5) & 0x01;

    params->charge_current_ma = (reg_04 & 0x7F) * 64;
    params->precharge_current_ma = ((reg_05 >> 4) & 0x0F) * 64;
    params->termination_current_ma = (reg_05 & 0x0F) * 64;

    params->charge_voltage_mv = 3840 + (((reg_06 >> 2) & 0x3F) * 16);
    
    params->enable_termination = (reg_07 >> 7) & 0x01;
    params->enable_safety_timer = (reg_07 >> 3) & 0x01;
    
    switch ((reg_07 >> 1) & 0x03) {
        case 0: params->fast_charge_timer_hours = 5; break;
        case 1: params->fast_charge_timer_hours = 8; break;
        case 2: params->fast_charge_timer_hours = 12; break;
        case 3: params->fast_charge_timer_hours = 20; break;
    }

    params->enable_ir_compensation = (reg_08 >> 5) & 0x01;
    params->ir_compensation_mohm = ((reg_08 >> 5) & 0x07) * 20;
    params->ir_compensation_voltage_mv = (reg_08 & 0x07) * 32;

    switch (reg_08 & 0x03) {
        case 0: params->thermal_regulation_threshold = 60; break;
        case 1: params->thermal_regulation_threshold = 80; break;
        case 2: params->thermal_regulation_threshold = 100; break;
        case 3: params->thermal_regulation_threshold = 120; break;
    }

    params->boost_voltage_mv = 4550 + (((reg_0a >> 4) & 0x0F) * 64);

    return ESP_OK;
}

/**
 * @brief Set the charge parameters of the BQ25895
 */
esp_err_t bq25895_set_charge_params(const bq25895_charge_params_t *params)
{
    if (!is_initialized || params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // Set input current limit
    ret = bq25895_set_input_current_limit(params->input_current_limit_ma);
    if (ret != ESP_OK) return ret;

    // Set input voltage limit
    ret = bq25895_set_input_voltage_limit(params->input_voltage_limit_mv);
    if (ret != ESP_OK) return ret;

    // Set charge voltage
    ret = bq25895_set_charge_voltage(params->charge_voltage_mv);
    if (ret != ESP_OK) return ret;

    // Set charge current
    ret = bq25895_set_charge_current(params->charge_current_ma);
    if (ret != ESP_OK) return ret;

    // Set precharge and termination current
    uint8_t reg_05 = ((params->precharge_current_ma / 64) << 4) | (params->termination_current_ma / 64);
    ret = bq25895_write_reg(BQ25895_REG_05, reg_05);
    if (ret != ESP_OK) return ret;

    // Set termination, safety timer, and charge timer
    uint8_t timer_val;
    if (params->fast_charge_timer_hours <= 5) {
        timer_val = 0;
    } else if (params->fast_charge_timer_hours <= 8) {
        timer_val = 1;
    } else if (params->fast_charge_timer_hours <= 12) {
        timer_val = 2;
    } else {
        timer_val = 3;
    }

    uint8_t reg_07;
    ret = bq25895_read_reg(BQ25895_REG_07, &reg_07);
    if (ret != ESP_OK) return ret;

    reg_07 &= ~(0x8A); // Clear EN_TERM, EN_TIMER, and CHG_TIMER bits
    reg_07 |= (params->enable_termination ? (1 << 7) : 0);
    reg_07 |= (1<<4); //watchdog
    reg_07 |= (1<<5); //watchdog
    reg_07 |= (params->enable_safety_timer ? (1 << 3) : 0);
    reg_07 |= (timer_val << 1);

    ret = bq25895_write_reg(BQ25895_REG_07, reg_07);
    if (ret != ESP_OK) return ret;

    // Set IR compensation and thermal regulation
    uint8_t treg_val;
    if (params->thermal_regulation_threshold <= 60) {
        treg_val = 0;
    } else if (params->thermal_regulation_threshold <= 80) {
        treg_val = 1;
    } else if (params->thermal_regulation_threshold <= 100) {
        treg_val = 2;
    } else {
        treg_val = 3;
    }

    uint8_t bat_comp = params->ir_compensation_mohm / 20;
    uint8_t vclamp = params->ir_compensation_voltage_mv / 32;

    uint8_t reg_08 = (bat_comp << 5) | (vclamp << 2) | treg_val;
    ret = bq25895_write_reg(BQ25895_REG_08, reg_08);
    if (ret != ESP_OK) return ret;

    // Set boost voltage
    ret = bq25895_set_boost_voltage(params->boost_voltage_mv);
    if (ret != ESP_OK) return ret;

    // Set charging and OTG mode
    ret = bq25895_enable_charging(params->enable_charging);
    if (ret != ESP_OK) return ret;

    ret = bq25895_enable_otg(params->enable_otg);
    if (ret != ESP_OK) return ret;

    // Set Hi-Z mode
    uint8_t reg_00;
    ret = bq25895_read_reg(BQ25895_REG_00, &reg_00);
    if (ret != ESP_OK) return ret;

    reg_00 &= ~(1 << 7); // Clear EN_HIZ bit
    reg_00 |= (params->enable_hi_impedance ? (1 << 7) : 0);

    ret = bq25895_write_reg(BQ25895_REG_00, reg_00);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Enable or disable charging
 */
esp_err_t bq25895_enable_charging(bool enable)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_03;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_03, &reg_03);
    if (ret != ESP_OK) return ret;

    reg_03 &= ~(1 << 4); // Clear CHG_CONFIG bit
    reg_03 |= (enable ? (1 << 4) : 0);

    ret = bq25895_write_reg(BQ25895_REG_03, reg_03);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Enable or disable OTG mode
 */
esp_err_t bq25895_enable_otg(bool enable)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg_03;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_03, &reg_03);
    if (ret != ESP_OK) return ret;

    reg_03 &= ~(1 << 5); // Clear OTG_CONFIG bit
    reg_03 |= (enable ? (1 << 5) : 0);

    ret = bq25895_write_reg(BQ25895_REG_03, reg_03);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Set the charge voltage limit
 */
esp_err_t bq25895_set_charge_voltage(uint16_t voltage_mv)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (voltage_mv < 3840 || voltage_mv > 4608) {
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate VREG value (3840mV + VREG*16mV)
    uint8_t vreg = (voltage_mv - 3840) / 16;
    if (vreg > 0x3F) {
        vreg = 0x3F; // Max value is 63 (4.608V)
    }

    uint8_t reg_06;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_06, &reg_06);
    if (ret != ESP_OK) return ret;

    reg_06 &= ~(0xFC); // Clear VREG bits (bits 7-2)
    reg_06 |= (vreg << 2);

    ret = bq25895_write_reg(BQ25895_REG_06, reg_06);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Set the charge current limit
 */
esp_err_t bq25895_set_charge_current(uint16_t current_ma)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (current_ma > 5056) {
        current_ma = 5056; // Max value is 5056mA
    }

    // Calculate ICHG value (ICHG*64mA)
    uint8_t ichg = current_ma / 64;
    if (ichg > 0x7F) {
        ichg = 0x7F; // Max value is 127 (5056mA)
    }

    esp_err_t ret = bq25895_write_reg(BQ25895_REG_04, ichg);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Set the input current limit
 */
esp_err_t bq25895_set_input_current_limit(uint16_t current_ma)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (current_ma < 100) {
        current_ma = 100; // Min value is 100mA
    } else if (current_ma > 3250) {
        current_ma = 3250; // Max value is 3250mA
    }

    // Calculate IINLIM value (100mA + IINLIM*50mA)
    uint8_t iinlim = (current_ma - 100) / 50;
    if (iinlim > 0x3F) {
        iinlim = 0x3F; // Max value is 63 (3250mA)
    }

    uint8_t reg_00;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_00, &reg_00);
    if (ret != ESP_OK) return ret;

    reg_00 &= ~(0x3F); // Clear IINLIM bits (bits 5-0)
    reg_00 |= iinlim;

    ret = bq25895_write_reg(BQ25895_REG_00, reg_00);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Set the input voltage limit
 */
esp_err_t bq25895_set_input_voltage_limit(uint16_t voltage_mv)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (voltage_mv < 3900) {
        voltage_mv = 3900; // Min value is 3900mV
    } else if (voltage_mv > 14000) {
        voltage_mv = 14000; // Max value is 14000mV
    }

    // Use absolute VINDPM mode
    uint8_t reg_0d;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_0D, &reg_0d);
    if (ret != ESP_OK) return ret;

    reg_0d |= (1 << 7); // Set FORCE_VINDPM bit

    // Calculate VINDPM value (2600mV + VINDPM*100mV)
    uint8_t vindpm = (voltage_mv - 2600) / 100;
    if (vindpm > 0x7F) {
        vindpm = 0x7F; // Max value is 127 (15300mV)
    }

    reg_0d &= ~(0x7F); // Clear VINDPM bits (bits 6-0)
    reg_0d |= vindpm;

    ret = bq25895_write_reg(BQ25895_REG_0D, reg_0d);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

/**
 * @brief Set the boost mode voltage
 */
esp_err_t bq25895_set_boost_voltage(uint16_t voltage_mv)
{
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (voltage_mv < 4550) {
        voltage_mv = 4550; // Min value is 4550mV
    } else if (voltage_mv > 5510) {
        voltage_mv = 5510; // Max value is 5510mV
    }

    // Calculate BOOSTV value (4550mV + BOOSTV*64mV)
    uint8_t boostv = (voltage_mv - 4550) / 64;
    if (boostv > 0x0F) {
        boostv = 0x0F; // Max value is 15 (5510mV)
    }

    uint8_t reg_0a;
    esp_err_t ret = bq25895_read_reg(BQ25895_REG_0A, &reg_0a);
    if (ret != ESP_OK) return ret;

    reg_0a &= ~(0xF0); // Clear BOOSTV bits (bits 7-4)
    reg_0a |= (boostv << 4);

    ret = bq25895_write_reg(BQ25895_REG_0A, reg_0a);
    if (ret != ESP_OK) return ret;

    return ESP_OK;
}

