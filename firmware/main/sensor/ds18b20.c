/*
 * DS18B20 1-Wire bit-bang driver for ESP32
 *
 * Implements the full 1-Wire reset / read / write timing on a single GPIO.
 * Supports up to DS18B20_MAX_SENSORS devices via ROM enumeration.
 */

#include "ds18b20.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* Spinlock for disabling interrupts during timing-critical 1-Wire slots */
static portMUX_TYPE ow_mux = portMUX_INITIALIZER_UNLOCKED;

/* Retry count for CRC failures (noise on breadboard power rails, etc.) */
#define DS18B20_READ_RETRIES  3

/* Bus enumeration attempts per call to enumerate() */
#define DS18B20_SEARCH_ATTEMPTS  3

/* After this many consecutive reads where *no* sensor answers, forget the
   ROM list and re-enumerate (probe replugged, sensor wedged, ...). At the
   5 s sensor_task period this is ~30 s. */
#define DS18B20_REENUM_AFTER_FAILS  6

static int fail_streak = 0;
static int rescan_count = 0;

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
    portENTER_CRITICAL(&ow_mux);
    ow_low();
    ets_delay_us(480);
    ow_release();
    ets_delay_us(70);
    bool presence = (ow_read_level() == 0);
    ets_delay_us(410);
    portEXIT_CRITICAL(&ow_mux);
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
    portENTER_CRITICAL(&ow_mux);
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 1);
        byte >>= 1;
    }
    portEXIT_CRITICAL(&ow_mux);
}

/** Read a full byte (LSB first). */
static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    portENTER_CRITICAL(&ow_mux);
    for (int i = 0; i < 8; i++) {
        byte >>= 1;
        if (ow_read_bit()) {
            byte |= 0x80;
        }
    }
    portEXIT_CRITICAL(&ow_mux);
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

            /* The read-read-write triplet for each bit is timing-critical:
               Wi-Fi interrupts (busy at boot) can stretch a slot and corrupt
               the search, so keep it in one critical section (~210 us). */
            portENTER_CRITICAL(&ow_mux);
            int bit_a = ow_read_bit();
            int bit_b = ow_read_bit();

            if (bit_a && bit_b) {
                portEXIT_CRITICAL(&ow_mux);
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
            portEXIT_CRITICAL(&ow_mux);
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

        /* Yield to let the IDLE task feed the watchdog */
        vTaskDelay(pdMS_TO_TICKS(1));

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

/* Search the bus (with retries) and configure every sensor found.
   Sets rom_count; returns ESP_OK if at least one sensor was found. */
static esp_err_t enumerate(void)
{
    rom_count = 0;
    for (int attempt = 0; attempt < DS18B20_SEARCH_ATTEMPTS; attempt++) {
        if (ow_reset()) {
            rom_count = search_roms();
            if (rom_count > 0) break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (rom_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Set all sensors to 12-bit resolution (default, but be explicit) */
    for (int i = 0; i < rom_count; i++) {
        if (!ow_reset()) continue;
        ow_write_byte(0x55);  /* Match ROM */
        for (int b = 0; b < 8; b++) ow_write_byte(rom_codes[i][b]);
        ow_write_byte(0x4E);  /* Write Scratchpad */
        ow_write_byte(0x00);  /* TH — unused */
        ow_write_byte(0x00);  /* TL — unused */
        ow_write_byte(0x7F);  /* Config: 12-bit resolution */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

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

    if (enumerate() != ESP_OK) {
        /* Not fatal: ds18b20_read_temps() keeps re-scanning, so a probe
           that missed the boot-time search still comes back on its own. */
        ESP_LOGE(TAG, "No DS18B20 found on GPIO %d — will keep re-scanning",
                 DS18B20_GPIO);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Initialised — %d DS18B20 sensor(s) on GPIO %d",
             rom_count, DS18B20_GPIO);
    return ESP_OK;
}

esp_err_t ds18b20_read_temps(float *temps, int *count)
{
    if (rom_count == 0) {
        /* Missed at boot or dropped after repeated failures: re-scan.
           Cheap (~1 ms) when nothing answers the reset pulse. */
        if (enumerate() != ESP_OK) {
            if (++rescan_count % 12 == 1) {   /* log ~once a minute */
                ESP_LOGW(TAG, "Still no DS18B20 on GPIO %d (re-scanning)",
                         DS18B20_GPIO);
            }
            *count = 0;
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "Re-discovered %d DS18B20 sensor(s) after %d re-scan(s)",
                 rom_count, rescan_count);
        rescan_count = 0;
        fail_streak = 0;
    }

    int read_count = 0;

    /* Issue Convert T to all devices simultaneously */
    if (!ow_reset()) {
        goto finish;          /* nothing on the bus — counts as a failure */
    }
    ow_write_byte(0xCC);  /* Skip ROM — all devices convert */
    ow_write_byte(0x44);  /* Convert T */

    /* Wait for conversion (750 ms max at 12-bit resolution) */
    vTaskDelay(pdMS_TO_TICKS(800));

    for (int i = 0; i < rom_count; i++) {
        bool ok = false;

        for (int retry = 0; retry < DS18B20_READ_RETRIES; retry++) {
            if (!ow_reset()) continue;

            ow_write_byte(0x55);  /* Match ROM */
            for (int b = 0; b < 8; b++) ow_write_byte(rom_codes[i][b]);
            ow_write_byte(0xBE);  /* Read Scratchpad */

            uint8_t scratch[9];
            for (int b = 0; b < 9; b++) {
                scratch[b] = ow_read_byte();
            }

            if (crc8(scratch, 8) != scratch[8]) {
                ESP_LOGW(TAG, "CRC mismatch on sensor %d (attempt %d/%d)",
                         i, retry + 1, DS18B20_READ_RETRIES);
                continue;
            }

            /* A bus stuck low reads all zeros, and crc8(0...0) == 0 passes
               the check above — which would look like a valid 0.00 C. */
            bool all_zero = true;
            for (int b = 0; b < 9; b++) {
                if (scratch[b] != 0) { all_zero = false; break; }
            }
            if (all_zero) {
                ESP_LOGW(TAG, "Sensor %d: all-zero scratchpad (bus stuck low?) "
                         "(attempt %d/%d)", i, retry + 1, DS18B20_READ_RETRIES);
                continue;
            }

            int16_t raw = (int16_t)((scratch[1] << 8) | scratch[0]);

            /* 0x0550 = 85.0 C is the power-on reset value: the sensor
               browned out or never ran the conversion. Not a real reading. */
            if (raw == 0x0550) {
                ESP_LOGW(TAG, "Sensor %d: 85.0 C power-on value, ignoring "
                         "(attempt %d/%d)", i, retry + 1, DS18B20_READ_RETRIES);
                continue;
            }

            temps[read_count] = (float)raw / 16.0f;
            read_count++;
            ok = true;
            break;
        }

        if (!ok) {
            ESP_LOGW(TAG, "Sensor %d: all %d read attempts failed",
                     i, DS18B20_READ_RETRIES);
        }
    }

finish:
    *count = read_count;

    if (read_count > 0) {
        fail_streak = 0;
        return ESP_OK;
    }
    if (++fail_streak >= DS18B20_REENUM_AFTER_FAILS) {
        ESP_LOGW(TAG, "No sensor answered for %d reads — re-enumerating bus",
                 fail_streak);
        rom_count = 0;      /* next call re-scans */
        fail_streak = 0;
    }
    return ESP_FAIL;
}
