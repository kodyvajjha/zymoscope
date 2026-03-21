/*
 * DS18B20 1-Wire bit-bang driver for ESP32
 *
 * Implements the full 1-Wire reset / read / write timing on a single GPIO.
 * Supports up to DS18B20_MAX_SENSORS devices via ROM enumeration.
 */

#include "ds18b20.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"          /* ets_delay_us */

static const char *TAG = "ds18b20";

/* ------------------------------------------------------------------ */
/*  ROM storage                                                       */
/* ------------------------------------------------------------------ */
static uint8_t  rom_codes[DS18B20_MAX_SENSORS][8];
static int      rom_count = 0;

/* ------------------------------------------------------------------ */
/*  Low-level 1-Wire timing helpers                                   */
/* ------------------------------------------------------------------ */

/* Drive the bus low */
static inline void ow_low(void)
{
    gpio_set_direction(DS18B20_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DS18B20_GPIO, 0);
}

/* Release the bus (external pull-up takes it high) */
static inline void ow_release(void)
{
    gpio_set_direction(DS18B20_GPIO, GPIO_MODE_INPUT);
}

/* Read the current bus level */
static inline int ow_read_level(void)
{
    return gpio_get_level(DS18B20_GPIO);
}

/**
 * 1-Wire reset pulse.  Returns true if at least one device pulled the
 * bus low during the presence window.
 */
static bool ow_reset(void)
{
    ow_low();
    ets_delay_us(480);
    ow_release();
    ets_delay_us(70);
    bool presence = (ow_read_level() == 0);
    ets_delay_us(410);
    return presence;
}

/** Write a single bit (LSB first). */
static void ow_write_bit(int bit)
{
    if (bit & 1) {
        /* Write-1 slot: pull low 6 us, release, wait 64 us */
        ow_low();
        ets_delay_us(6);
        ow_release();
        ets_delay_us(64);
    } else {
        /* Write-0 slot: pull low 60 us, release, wait 10 us */
        ow_low();
        ets_delay_us(60);
        ow_release();
        ets_delay_us(10);
    }
}

/** Read a single bit. */
static int ow_read_bit(void)
{
    ow_low();
    ets_delay_us(3);
    ow_release();
    ets_delay_us(10);
    int val = ow_read_level();
    ets_delay_us(53);
    return val;
}

/** Write a full byte (LSB first). */
static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 1);
        byte >>= 1;
    }
}

/** Read a full byte (LSB first). */
static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte >>= 1;
        if (ow_read_bit()) {
            byte |= 0x80;
        }
    }
    return byte;
}

/* ------------------------------------------------------------------ */
/*  CRC-8 (Dallas/Maxim polynomial x^8+x^5+x^4+1)                   */
/* ------------------------------------------------------------------ */
static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/*  ROM Search (simplified — walks the full tree)                     */
/* ------------------------------------------------------------------ */
static int search_roms(void)
{
    /*
     * Implements the 1-Wire search algorithm from Maxim AN187.
     * Each pass discovers one ROM code by resolving bit conflicts.
     */
    int last_discrepancy = -1;
    int found = 0;
    uint8_t rom[8];
    memset(rom, 0, sizeof(rom));

    while (found < DS18B20_MAX_SENSORS) {
        if (!ow_reset()) {
            break;
        }
        ow_write_byte(0xF0);  /* Search ROM */

        int next_discrepancy = -1;

        for (int bit_idx = 0; bit_idx < 64; bit_idx++) {
            int byte_idx = bit_idx / 8;
            int bit_mask = 1 << (bit_idx % 8);

            int bit_a = ow_read_bit();
            int bit_b = ow_read_bit();

            if (bit_a && bit_b) {
                /* No devices on bus — done */
                goto done;
            }

            int dir;
            if (bit_a != bit_b) {
                /* All devices agree */
                dir = bit_a;
            } else {
                /* Conflict — both 0 and 1 present */
                if (bit_idx == last_discrepancy) {
                    dir = 1;
                } else if (bit_idx > last_discrepancy) {
                    dir = 0;
                    next_discrepancy = bit_idx;
                } else {
                    dir = (rom[byte_idx] & bit_mask) ? 1 : 0;
                    if (dir == 0) {
                        next_discrepancy = bit_idx;
                    }
                }
            }

            if (dir) {
                rom[byte_idx] |= bit_mask;
            } else {
                rom[byte_idx] &= ~bit_mask;
            }
            ow_write_bit(dir);
        }

        /* Validate CRC */
        if (crc8(rom, 7) == rom[7]) {
            memcpy(rom_codes[found], rom, 8);
            found++;
            ESP_LOGI(TAG, "Found sensor %d: %02X%02X%02X%02X%02X%02X%02X%02X",
                     found,
                     rom[0], rom[1], rom[2], rom[3],
                     rom[4], rom[5], rom[6], rom[7]);
        }

        last_discrepancy = next_discrepancy;
        if (last_discrepancy < 0) {
            break;  /* No more branches */
        }
    }

done:
    return found;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t ds18b20_init(void)
{
    /* Configure GPIO with internal pull-up (external 4.7k is still
       recommended for reliable operation) */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << DS18B20_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    if (!ow_reset()) {
        ESP_LOGE(TAG, "No 1-Wire devices detected on GPIO %d", DS18B20_GPIO);
        return ESP_ERR_NOT_FOUND;
    }

    rom_count = search_roms();
    if (rom_count == 0) {
        ESP_LOGE(TAG, "Search found no DS18B20 sensors");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Initialised — %d DS18B20 sensor(s) on GPIO %d",
             rom_count, DS18B20_GPIO);

    /* Set all sensors to 12-bit resolution (default, but be explicit) */
    for (int i = 0; i < rom_count; i++) {
        if (!ow_reset()) continue;
        ow_write_byte(0x55);  /* Match ROM */
        for (int b = 0; b < 8; b++) ow_write_byte(rom_codes[i][b]);
        ow_write_byte(0x4E);  /* Write Scratchpad */
        ow_write_byte(0x00);  /* TH — unused */
        ow_write_byte(0x00);  /* TL — unused */
        ow_write_byte(0x7F);  /* Config: 12-bit resolution */
    }

    return ESP_OK;
}

esp_err_t ds18b20_read_temps(float *temps, int *count)
{
    if (rom_count == 0) {
        *count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    /* Issue Convert T to all devices simultaneously */
    if (!ow_reset()) {
        *count = 0;
        return ESP_FAIL;
    }
    ow_write_byte(0xCC);  /* Skip ROM — all devices convert */
    ow_write_byte(0x44);  /* Convert T */

    /* Wait for conversion (750 ms max at 12-bit resolution) */
    vTaskDelay(pdMS_TO_TICKS(800));

    int read_count = 0;
    for (int i = 0; i < rom_count; i++) {
        if (!ow_reset()) continue;

        ow_write_byte(0x55);  /* Match ROM */
        for (int b = 0; b < 8; b++) ow_write_byte(rom_codes[i][b]);
        ow_write_byte(0xBE);  /* Read Scratchpad */

        uint8_t scratch[9];
        for (int b = 0; b < 9; b++) {
            scratch[b] = ow_read_byte();
        }

        if (crc8(scratch, 8) != scratch[8]) {
            ESP_LOGW(TAG, "CRC mismatch on sensor %d, skipping", i);
            continue;
        }

        int16_t raw = (int16_t)((scratch[1] << 8) | scratch[0]);
        temps[read_count] = (float)raw / 16.0f;
        read_count++;
    }

    *count = read_count;
    return (read_count > 0) ? ESP_OK : ESP_FAIL;
}
