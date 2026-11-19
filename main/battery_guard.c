/*
 * battery_guard.c
 *
 *  Low-battery sleep. See battery_guard.h.
 */

#include "base.h"
#include "battery_guard.h"
#include "bw_settings.h"
#include "ota_update.h"
#include "storage.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "soc/soc_caps.h"

static const char *BG_TAG = "BATTERY";

#define BG_READINGS   4             /*  Averaged, to smooth ADC noise  */

/*  Pins held through deep sleep at the levels the unit leaves them between
 *  connections: modem PWRKEY and RESET released, mic off, level shifter off
 *  (as at power-on, before esp32_init enables it).                          */
static const gpio_num_t s_held_pins[] = { MODEM_PWRKEY_PIN, MODEM_RESET_PIN, MIC_EN, GSM_LVL_OE };
static const int s_held_levels[]      = { 0,                0,               MIC_OFF, 0 };
#define NUM_HELD (sizeof(s_held_pins) / sizeof(s_held_pins[0]))

/*  RTC memory: kept through deep sleep, lost when the power goes  */
static RTC_DATA_ATTR bool     s_dormant = false;     /*  In a spell: wakes need resume_battery_mv  */
static RTC_DATA_ATTR uint32_t s_spells = 0;          /*  Spells since the last restart  */
static RTC_DATA_ATTR int64_t  s_start = 0;           /*  Current or last spell, 0 if the clock was not set  */
static RTC_DATA_ATTR int64_t  s_end = 0;
static RTC_DATA_ATTR uint32_t s_wakes = 0;           /*  Wakes that found the battery still low  */
static RTC_DATA_ATTR int      s_start_mv = 0;
static RTC_DATA_ATTR int      s_lowest_mv = 0;
static RTC_DATA_ATTR int      s_end_mv = 0;

static int64_t clock_now(void)
{
    time_t now = time(NULL);
    return now < 1700000000 ? 0 : (int64_t) now;     /*  0: the clock has not been set  */
}

int battery_guard_read_mv(void)
{
    int total = 0, count = 0;
    for (int i = 0; i < BG_READINGS; i++) {
        int pin_mv = read_adc_voltage_gpio8();
        if (pin_mv > 0) {
            total += pin_mv;
            count++;
        }
    }
    if (count == 0) {
        return -1;
    }
    int battery_mv = (total / count) * 2;            /*  50/50 divider  */
    return battery_mv < BW_BATTERY_PRESENT_MV ? -1 : battery_mv;
}

bool battery_guard_is_low(int battery_mv)
{
    return battery_mv > 0 && battery_mv < (int) g_settings.low_battery_mv;
}

static void release_pins(void)
{
    for (size_t i = 0; i < NUM_HELD; i++) {
        gpio_hold_dis(s_held_pins[i]);
    }
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
}

static void hold_pins(void)
{
    for (size_t i = 0; i < NUM_HELD; i++) {
        gpio_set_direction(s_held_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_held_pins[i], s_held_levels[i]);
        gpio_hold_en(s_held_pins[i]);
    }
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

static void sleep_now(void)
{
    uint32_t sleep_s = g_settings.low_battery_sleep_s;
    ESP_LOGW(BG_TAG, "Sleeping for %lu s; recording and reporting resume above %lu mV",
             (unsigned long) sleep_s, (unsigned long) g_settings.resume_battery_mv);
    vTaskDelay(pdMS_TO_TICKS(200));                  /*  Let the log drain  */
    hold_pins();
    esp_sleep_enable_timer_wakeup((uint64_t) sleep_s * 1000000ULL);
    esp_deep_sleep_start();
}

static void start_spell(int battery_mv)
{
    s_dormant = true;
    s_spells++;
    s_start = clock_now();
    s_end = 0;
    s_wakes = 0;
    s_start_mv = battery_mv;
    s_lowest_mv = battery_mv;
    s_end_mv = 0;
}

void battery_guard_sleep(int battery_mv)
{
    if (ota_on_trial()) {
        ESP_LOGW(BG_TAG, "Battery %d mV is low, but the firmware is on trial and a restart would "
                 "roll it back; carrying on", battery_mv);
        return;
    }
    ESP_LOGW(BG_TAG, "Battery %d mV is below %lu mV; stopping until it recovers",
             battery_mv, (unsigned long) g_settings.low_battery_mv);
    start_spell(battery_mv);
    sleep_now();
}

void battery_guard_boot(void)
{
    release_pins();

    esp_reset_reason_t reason = esp_reset_reason();
    bool waking = (reason == ESP_RST_DEEPSLEEP) && s_dormant;
    if (!waking) {
        s_dormant = false;
    }

    int battery_mv = battery_guard_read_mv();
    if (battery_mv < 0) {
        ESP_LOGW(BG_TAG, "No battery reading (none fitted, or the ADC failed); starting anyway");
        if (waking) {
            s_dormant = false;
            s_end = clock_now();
        }
        return;
    }

    /*  After a spell or a brownout, start only once the battery has properly recovered:
     *  otherwise the modem would pull it down again and the unit reset over and over  */
    bool need_resume = waking || reason == ESP_RST_BROWNOUT;
    uint32_t need_mv = need_resume ? g_settings.resume_battery_mv : g_settings.low_battery_mv;

    if (battery_mv >= (int) need_mv) {
        if (waking) {
            s_dormant = false;
            s_end = clock_now();
            s_end_mv = battery_mv;
            ESP_LOGI(BG_TAG, "Battery %d mV has recovered after %lu low wake(s); starting",
                     battery_mv, (unsigned long) s_wakes);
        } else {
            ESP_LOGI(BG_TAG, "Battery %d mV", battery_mv);
        }
        return;
    }

    if (ota_on_trial()) {
        ESP_LOGW(BG_TAG, "Battery %d mV is below %lu mV, but the firmware is on trial; starting anyway",
                 battery_mv, (unsigned long) need_mv);
        s_dormant = false;
        return;
    }

    if (waking) {
        s_wakes++;
        if (battery_mv < s_lowest_mv) {
            s_lowest_mv = battery_mv;
        }
        ESP_LOGW(BG_TAG, "Battery %d mV is still below %lu mV", battery_mv, (unsigned long) need_mv);
    } else {
        ESP_LOGW(BG_TAG, "Battery %d mV is below %lu mV at %s; not starting",
                 battery_mv, (unsigned long) need_mv, reason == ESP_RST_BROWNOUT ? "a brownout restart" : "start-up");
        start_spell(battery_mv);
    }
    sleep_now();
}

static void format_time(int64_t t, char *out, size_t size)
{
    if (t == 0) {
        strlcpy(out, "unknown", size);
        return;
    }
    time_t tt = (time_t) t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, size, "%Y-%m-%d %H:%M:%S GMT", &tm);
}

size_t battery_guard_format(char *out, size_t size)
{
    if (size == 0) {
        return 0;
    }
    int n;
    if (s_spells == 0) {
        n = snprintf(out, size, "low_battery_spells=0\n");
    } else {
        char start[32], end[32];
        format_time(s_start, start, sizeof(start));
        format_time(s_end, end, sizeof(end));
        n = snprintf(out, size,
            "low_battery_spells=%lu\n"
            "low_battery_last_start=%s\n"
            "low_battery_last_end=%s\n"
            "low_battery_last_wakes=%lu\n"
            "low_battery_last_start_mv=%d\n"
            "low_battery_last_lowest_mv=%d\n"
            "low_battery_last_end_mv=%d\n",
            (unsigned long) s_spells, start, end, (unsigned long) s_wakes,
            s_start_mv, s_lowest_mv, s_end_mv);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t) n < size ? (size_t) n : size - 1;
}
