/*
 * BME280 I2C driver for ESP-IDF v5.x
 *
 * Implements forced-mode reads with full compensation formulas from
 * the Bosch BME280 datasheet (BST-BME280-DS002, rev 1.9).
 */

#include "bme280_i2c.h"

#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme280";

/* ------------------------------------------------------------------ */
/*  BME280 register addresses                                         */
/* ------------------------------------------------------------------ */
#define BME280_REG_CHIP_ID      0xD0
#define BME280_REG_RESET        0xE0
#define BME280_REG_CTRL_HUM     0xF2
#define BME280_REG_STATUS       0xF3
#define BME280_REG_CTRL_MEAS    0xF4
#define BME280_REG_CONFIG       0xF5
#define BME280_REG_DATA_START   0xF7   /* press_msb */
#define BME280_REG_CALIB_T1     0x88   /* T/P calibration 0x88-0x9F */
#define BME280_REG_CALIB_H1     0xA1
#define BME280_REG_CALIB_H2     0xE1   /* Humidity calibration 0xE1-0xE7 */

#define BME280_CHIP_ID_VAL      0x60

/* ------------------------------------------------------------------ */
/*  Compensation parameters (from datasheet Table 16)                 */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Temperature */
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    /* Pressure */
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    /* Humidity */
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

static bme280_calib_t cal;
static bool i2c_installed = false;

/* ------------------------------------------------------------------ */
/*  I2C helpers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t bme280_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(BME280_I2C_PORT, BME280_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t bme280_read_regs(uint8_t start_reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(BME280_I2C_PORT, BME280_I2C_ADDR,
                                        &start_reg, 1,
                                        data, len,
                                        pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------------ */
/*  Read calibration data                                              */
/* ------------------------------------------------------------------ */
static esp_err_t bme280_read_calibration(void)
{
    uint8_t buf[26];
    esp_err_t err;

    /* T1..P9 are in 0x88..0xA1 (26 bytes) */
    err = bme280_read_regs(BME280_REG_CALIB_T1, buf, 26);
    if (err != ESP_OK) return err;

    cal.dig_T1 = (uint16_t)(buf[1] << 8  | buf[0]);
    cal.dig_T2 = (int16_t) (buf[3] << 8  | buf[2]);
    cal.dig_T3 = (int16_t) (buf[5] << 8  | buf[4]);
    cal.dig_P1 = (uint16_t)(buf[7] << 8  | buf[6]);
    cal.dig_P2 = (int16_t) (buf[9] << 8  | buf[8]);
    cal.dig_P3 = (int16_t) (buf[11] << 8 | buf[10]);
    cal.dig_P4 = (int16_t) (buf[13] << 8 | buf[12]);
    cal.dig_P5 = (int16_t) (buf[15] << 8 | buf[14]);
    cal.dig_P6 = (int16_t) (buf[17] << 8 | buf[16]);
    cal.dig_P7 = (int16_t) (buf[19] << 8 | buf[18]);
    cal.dig_P8 = (int16_t) (buf[21] << 8 | buf[20]);
    cal.dig_P9 = (int16_t) (buf[23] << 8 | buf[22]);

    /* H1 at 0xA1 */
    err = bme280_read_regs(BME280_REG_CALIB_H1, &cal.dig_H1, 1);
    if (err != ESP_OK) return err;

    /* H2..H6 at 0xE1..0xE7 (7 bytes) */
    uint8_t hbuf[7];
    err = bme280_read_regs(BME280_REG_CALIB_H2, hbuf, 7);
    if (err != ESP_OK) return err;

    cal.dig_H2 = (int16_t)(hbuf[1] << 8 | hbuf[0]);
    cal.dig_H3 = hbuf[2];
    cal.dig_H4 = (int16_t)((hbuf[3] << 4) | (hbuf[4] & 0x0F));
    cal.dig_H5 = (int16_t)((hbuf[5] << 4) | (hbuf[4] >> 4));
    cal.dig_H6 = (int8_t)hbuf[6];

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Compensation formulas (integer, from BME280 datasheet)            */
/* ------------------------------------------------------------------ */

/* t_fine is shared between temperature and pressure/humidity comp */
static int32_t t_fine;

static float compensate_temperature(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1))) *
                    ((int32_t)cal.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)cal.dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12) *
                    ((int32_t)cal.dig_T3)) >> 14;
    t_fine = var1 + var2;
    return (float)((t_fine * 5 + 128) >> 8) / 100.0f;
}

static float compensate_pressure(int32_t adc_P)
{
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)cal.dig_P6;
    var2 = var2 + ((var1 * (int64_t)cal.dig_P5) << 17);
    var2 = var2 + (((int64_t)cal.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)cal.dig_P3) >> 8) +
           ((var1 * (int64_t)cal.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)cal.dig_P1) >> 33;
    if (var1 == 0) return 0.0f;

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)cal.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)cal.dig_P7) << 4);

    return (float)((uint32_t)p) / 256.0f / 100.0f;  /* hPa */
}

static float compensate_humidity(int32_t adc_H)
{
    int32_t v = t_fine - 76800;
    v = (((((adc_H << 14) - (((int32_t)cal.dig_H4) << 20) -
            (((int32_t)cal.dig_H5) * v)) + 16384) >> 15) *
         (((((((v * ((int32_t)cal.dig_H6)) >> 10) *
              (((v * ((int32_t)cal.dig_H3)) >> 11) + 32768)) >> 10) +
            2097152) * ((int32_t)cal.dig_H2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)cal.dig_H1)) >> 4);
    v = (v < 0) ? 0 : v;
    v = (v > 419430400) ? 419430400 : v;
    return (float)(v >> 12) / 1024.0f;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t bme280_init(void)
{
    esp_err_t err;

    /* Install I2C master driver if not already done */
    if (!i2c_installed) {
        i2c_config_t conf = {
            .mode             = I2C_MODE_MASTER,
            .sda_io_num       = BME280_SDA_GPIO,
            .scl_io_num       = BME280_SCL_GPIO,
            .sda_pullup_en    = GPIO_PULLUP_ENABLE,
            .scl_pullup_en    = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,  /* 100 kHz */
        };
        err = i2c_param_config(BME280_I2C_PORT, &conf);
        if (err != ESP_OK) return err;

        err = i2c_driver_install(BME280_I2C_PORT, conf.mode, 0, 0, 0);
        if (err != ESP_OK) return err;

        i2c_installed = true;
    }

    /* Check chip ID */
    uint8_t chip_id;
    err = bme280_read_regs(BME280_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID");
        return err;
    }
    if (chip_id != BME280_CHIP_ID_VAL) {
        ESP_LOGE(TAG, "Unexpected chip ID: 0x%02X (expected 0x%02X)",
                 chip_id, BME280_CHIP_ID_VAL);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Soft reset */
    bme280_write_reg(BME280_REG_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Wait for NVM copy to complete */
    uint8_t status;
    do {
        bme280_read_regs(BME280_REG_STATUS, &status, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (status & 0x01);

    /* Read calibration data */
    err = bme280_read_calibration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read calibration data");
        return err;
    }

    /* Configure:
     * ctrl_hum  = osrs_h x1
     * ctrl_meas = osrs_t x1, osrs_p x1, mode=sleep (forced each read)
     * config    = standby 1000ms, filter off
     */
    bme280_write_reg(BME280_REG_CTRL_HUM, 0x01);
    bme280_write_reg(BME280_REG_CONFIG, 0xA0);
    bme280_write_reg(BME280_REG_CTRL_MEAS, 0x24);  /* sleep mode */

    ESP_LOGI(TAG, "BME280 initialised (addr=0x%02X, I2C port %d)",
             BME280_I2C_ADDR, BME280_I2C_PORT);
    return ESP_OK;
}

esp_err_t bme280_read(float *temp_c, float *humidity, float *pressure_hpa)
{
    esp_err_t err;

    /* Trigger a forced measurement */
    err = bme280_write_reg(BME280_REG_CTRL_MEAS, 0x25);  /* forced mode */
    if (err != ESP_OK) return err;

    /* Wait for measurement to complete */
    uint8_t status;
    int retries = 50;
    do {
        vTaskDelay(pdMS_TO_TICKS(5));
        err = bme280_read_regs(BME280_REG_STATUS, &status, 1);
        if (err != ESP_OK) return err;
    } while ((status & 0x08) && --retries > 0);

    /* Read raw data (press[2:0], temp[2:0], hum[1:0] = 8 bytes) */
    uint8_t data[8];
    err = bme280_read_regs(BME280_REG_DATA_START, data, 8);
    if (err != ESP_OK) return err;

    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8)  | (int32_t)data[7];

    /* Compensate — temperature first (sets t_fine) */
    *temp_c       = compensate_temperature(adc_T);
    *pressure_hpa = compensate_pressure(adc_P);
    *humidity     = compensate_humidity(adc_H);

    return ESP_OK;
}
