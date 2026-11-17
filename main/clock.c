// The clock. The system time comes from the network (SNTP) and is kept across power cycles by
// the board's RX8025T real-time clock chip when its backup cell is fitted. Until one of the two
// has supplied a time, the taskbar shows the uptime instead.
//
// A board with no clock chip (BOARD_HAS_RTC = 0) is the same code with the chip part left out: the
// zone list and the network path are common, and the time simply never survives a power cycle.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "board.h"
#include "os.h"

static const char *TAG = "clock";

#if BOARD_HAS_RTC
#include "driver/i2c_master.h"

static i2c_master_dev_handle_t s_rtc;
static bool s_rtc_ok;
#endif

// A few common zones, as POSIX TZ strings (daylight saving included where it applies).
static const struct { const char *name, *tz; } s_zones[] = {
    {"UTC", "UTC0"},
    {"London, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Istanbul, Moscow", "<+03>-3"},
    {"Dubai", "<+04>-4"},
    {"India", "IST-5:30"},
    {"Bangkok, Jakarta", "<+07>-7"},
    {"China, Singapore", "CST-8"},
    {"Japan, Korea", "JST-9"},
    {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Hawaii", "HST10"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"Sao Paulo, Buenos Aires", "<-03>3"},
};
#define NZONES ((int)(sizeof(s_zones) / sizeof(s_zones[0])))
static int s_zone;

int clock_zone_count(void) { return NZONES; }
const char *clock_zone_name(int i) { return i >= 0 && i < NZONES ? s_zones[i].name : ""; }
int clock_zone(void) { return s_zone; }

void clock_set_zone(int i)
{
    if (i < 0 || i >= NZONES) i = 0;
    s_zone = i;
    setenv("TZ", s_zones[i].tz, 1);
    tzset();
}

bool clock_valid(void) { return time(NULL) > 1704067200; }   // after 2024-01-01: the time has been set

// ---------------------------------------------------------------- RX8025T

#if BOARD_HAS_RTC

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

static bool rtc_read(struct tm *tm)
{
    uint8_t reg = 0, d[7], flag_reg = 0x0E, flags = 0;
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, d, sizeof(d), 50) != ESP_OK) return false;
    if (i2c_master_transmit_receive(s_rtc, &flag_reg, 1, &flags, 1, 50) != ESP_OK) return false;
    if (flags & 0x02) return false;     // VLF: the chip lost power, its time means nothing
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec = unbcd(d[0] & 0x7F); tm->tm_min = unbcd(d[1] & 0x7F); tm->tm_hour = unbcd(d[2] & 0x3F);
    tm->tm_mday = unbcd(d[4] & 0x3F); tm->tm_mon = unbcd(d[5] & 0x1F) - 1; tm->tm_year = unbcd(d[6]) + 100;
    return tm->tm_year >= 124 && tm->tm_mon >= 0 && tm->tm_mon < 12 && tm->tm_mday >= 1 && tm->tm_mday <= 31 &&
           tm->tm_hour < 24 && tm->tm_min < 60 && tm->tm_sec < 60;
}

static bool rtc_write(const struct tm *tm)
{
    uint8_t d[8] = {0, bcd(tm->tm_sec), bcd(tm->tm_min), bcd(tm->tm_hour), (uint8_t)(1 << tm->tm_wday),
                    bcd(tm->tm_mday), bcd(tm->tm_mon + 1), bcd(tm->tm_year % 100)};
    if (i2c_master_transmit(s_rtc, d, sizeof(d), 50) != ESP_OK) return false;
    uint8_t clear_flags[2] = {0x0E, 0x00};
    return i2c_master_transmit(s_rtc, clear_flags, 2, 50) == ESP_OK;
}

// The chip keeps UTC.
void clock_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c();
    if (!bus || i2c_master_probe(bus, BOARD_RTC_ADDR, 50) != ESP_OK) { ESP_LOGI(TAG, "no RX8025T clock chip found"); return; }
    const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = BOARD_RTC_ADDR, .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(bus, &dev, &s_rtc) != ESP_OK) return;
    s_rtc_ok = true;
    struct tm tm;
    if (rtc_read(&tm)) {
        const char *tz = getenv("TZ");
        char saved[48] = "";
        if (tz) strlcpy(saved, tz, sizeof(saved));
        setenv("TZ", "UTC0", 1); tzset();
        struct timeval tv = {.tv_sec = mktime(&tm)};
        if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
        tzset();
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "time from the clock chip: %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        ESP_LOGI(TAG, "RX8025T found, but it holds no valid time (no backup cell?)");
    }
}

#else  // BOARD_HAS_RTC == 0: no chip on the bus, so the time never survives a power cycle

void clock_init(void) { }

#endif

void clock_net_synced(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    ESP_LOGI(TAG, "time from the network: %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
#if BOARD_HAS_RTC
    if (s_rtc_ok && !rtc_write(&tm)) ESP_LOGW(TAG, "could not set the clock chip");
#endif
}

const char *clock_source(void)
{
    if (!clock_valid()) return "not set";
#if BOARD_HAS_RTC
    return s_rtc_ok ? "network + RX8025T chip" : "network";
#else
    return "network";     // SNTP is the only thing this board can ask for the time
#endif
}
