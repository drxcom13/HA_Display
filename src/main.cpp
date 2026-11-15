/**
 * HA_Display - LilyGo T5 4.7" (EPD47, ESP32-S3) Home Assistant desk display
 *
 * Two screens (960x540, landscape), switched with the icon in the top-right corner:
 *
 * Screen 1 - info                                   [grid icon -> screen 2]
 *   [ 13:51 ]          Niedziela
 *                      15 listopada 2026
 *   6°C częściowe zachm.  |  jutro 8/2°C pochmurno
 *   ------------------------------------------------
 *   Najbliższe wydarzenia
 *   pon 23.11  Wywóz: Zmieszane
 *   wt 24.11  Wywóz: Bio
 *   czw 26.11  Wywóz: Wielkogabaryty
 *   Aktualizacja 13:50  ·  WiFi -58 dBm
 *
 * Screen 2 - control tiles (4x3, max 12)            [calendar icon -> screen 1]
 *   +-------------+ +-------------+ +-------------+ +-------------+
 *   | Światło     | |             | |             | |             |
 *   | garaż       | |             | |             | |             |
 *   | ON          | |             | |             | |             |
 *   +-------------+ +-------------+ +-------------+ +-------------+
 *   (3 rows)
 *   Tap a tile -> homeassistant.toggle. Tiles are configured in TILE_CFG.
 *   Returns to screen 1 after SCREEN2_TIMEOUT_MS without touch.
 *
 * Data source (Home Assistant, REST API):
 *   input_text.desk_calendar_line_1..3  - filled by automation "EPD: aktualizuj linie wyświetlacza"
 *   input_text.desk_weather_line        - same automation (Met.no)
 *   sensor.desk_clock_line              - fallback clock, used only until NTP time is available
 *   TILE_CFG entities (screen 2)        - polled every TILE_POLL_MS while screen 2 is shown
 *
 * Battery: voltage on BATT_PIN (GPIO14, 1:2 divider; POWER_EN must be on while measuring), averaged,
 *          shown top-right on both screens, reported to HA as sensor.ha_display_battery every 5 min.
 *
 * Touch (GT911): icon -> switch screen; screen 1 elsewhere -> fetch HA data now + full redraw;
 *                screen 2 tile -> toggle entity.
 *
 * Clock comes from NTP (Europe/Warsaw, incl. DST), so it ticks exactly on the minute
 * without polling HA. Fonts (plsans*.h, plclock.h) include Polish characters.
 *
 * Connection loss handling:
 *   - last good data is kept on screen and stored in flash (NVS), so it survives a reboot
 *   - data older than STALE_AFTER_S, or from a previous day, is drawn in gray and marked "nieaktualne"
 *   - status line shows how long the connection has been down and how old the data is
 *   - after the first failed request the rest is skipped; retries back off 1 -> 2 -> 5 min
 *   - WiFi reconnect does not block the clock; the board restarts after WIFI_RESTART_AFTER_MS without WiFi
 *   - task watchdog restarts the board if the main loop hangs for WDT_TIMEOUT_S
 */

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> Tools -> PSRAM -> OPI PSRAM"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <time.h>
#include <Wire.h>
#include <TouchDrvGT911.hpp>   // SensorLib (lib_deps)
#include "epd_driver.h"
#include "plsans.h"
#include "plsans_bold.h"
#include "plclock.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, HA_HOST_ADDR, HA_PORT_NUM, HA_TOKEN_VALUE (not in git)

// ---------- CONFIG ----------
static const char *ENTITY_LINE[3] = {
    "input_text.desk_calendar_line_1",
    "input_text.desk_calendar_line_2",
    "input_text.desk_calendar_line_3",
};
static const char *ENTITY_WEATHER = "input_text.desk_weather_line";
static const char *ENTITY_CLOCK   = "sensor.desk_clock_line";   // fallback only

// Screen 2 tiles: any entity that supports homeassistant.toggle (light, switch, fan, input_boolean, ...)
// Max 12 tiles (4 columns x 3 rows). Add a line to add a tile. Long labels wrap to 2 lines.
struct TileCfg {
    const char *entity;
    const char *label;
};
static const TileCfg TILE_CFG[] = {
    {"light.light_garage_entrance_sonoff_light", "Światło garaż"},
};

// Battery
static const char *ENTITY_BATTERY = "sensor.ha_display_battery";   // created/updated by this device via REST API
static const unsigned long BATT_READ_MS   = 60UL * 1000UL;        // measure every minute
static const unsigned long BATT_REPORT_MS = 5UL * 60UL * 1000UL;  // report to HA every 5 min
static const int BATT_LOW_PCT             = 15;                   // warning threshold

static const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";      // Europe/Warsaw
static const char *NTP_1   = "pool.ntp.org";
static const char *NTP_2   = "time.google.com";

static const unsigned long DATA_INTERVAL_MS      = 5UL * 60UL * 1000UL;  // normal HA poll (HA refreshes every 15 min)
static const unsigned long RETRY_INTERVALS_MS[]  = {60UL * 1000UL, 2UL * 60UL * 1000UL, 5UL * 60UL * 1000UL}; // after failures
static const unsigned long WIFI_RETRY_MS         = 30UL * 1000UL;        // non-blocking WiFi.reconnect() interval
static const unsigned long WIFI_RESTART_AFTER_MS = 30UL * 60UL * 1000UL; // reboot after 30 min without WiFi
static const long STALE_AFTER_S                  = 60L * 60L;            // data older than 1 h is "nieaktualne"
static const int FULL_REFRESH_MINUTE             = 0;                    // full refresh once per hour (anti-ghosting)
static const uint32_t WDT_TIMEOUT_S              = 120;                  // loop must reset the watchdog within this time
static const unsigned long TILE_POLL_MS          = 5UL * 1000UL;         // tile state poll (only on screen 2)
static const unsigned long SCREEN2_TIMEOUT_MS    = 60UL * 1000UL;        // back to screen 1 after no touch
static const unsigned long TOUCH_POLL_MS         = 300;                  // GT911 poll interval (LilyGo issue #143)
static const uint8_t GRAY = 9;                                           // 0 = black, 15 = white
static const uint8_t TILE_ON_FILL = 12;                                  // light gray background of an "on" tile

// ---------- Layout ----------
struct Field {
    Rect_t area;
    const GFXfont *font;
    int32_t baseline;   // baseline offset from area.y
    String text;
    bool dirty;
    bool gray;
    bool center;        // center text horizontally in the area
};

enum {
    F_CLOCK, F_DAY, F_DATE, F_WEATHER, F_HEADER, F_EV1, F_EV2, F_EV3, F_STATUS, F_COUNT
};

static Field fields[F_COUNT] = {
    /* F_CLOCK   */ {{24, 8, 440, 150}, &PlClock, 140, "--:--", true, false, false},
    /* F_DAY     */ {{500, 50, 360, 50}, &PlSansBold, 40, "", true, false, false},
    /* F_DATE    */ {{500, 105, 360, 50}, &PlSans, 36, "", true, false, false},
    /* F_WEATHER */ {{24, 170, 912, 48}, &PlSans, 36, "", true, false, false},
    /* F_HEADER  */ {{24, 240, 912, 44}, &PlSansBold, 34, "Najbliższe wydarzenia", true, false, false},
    /* F_EV1     */ {{24, 292, 912, 52}, &PlSans, 38, "", true, false, false},
    /* F_EV2     */ {{24, 350, 912, 52}, &PlSans, 38, "", true, false, false},
    /* F_EV3     */ {{24, 408, 912, 52}, &PlSans, 38, "", true, false, false},
    /* F_STATUS  */ {{24, 500, 912, 36}, &PlSans, 28, "Uruchamianie...", true, false, false},
};

#define SEP_X 24
#define SEP_Y 230
#define SEP_W 912
#define SEP_H 2

// Battery indicator (left of the screen switch icon): text + battery symbol
static const Rect_t BATT_AREA = {680, 8, 196, 44};
#define BATT_ICON_X 828
#define BATT_ICON_Y 18
#define BATT_ICON_W 42
#define BATT_ICON_H 24

// Screen switch icon (top-right) - also the tap zone, with a margin
static const Rect_t NAV_ICON = {884, 12, 52, 52};
static const int TOUCH_MARGIN = 16;

// Tile grid (screen 2)
#define TILE_COLS 4
#define TILE_ROWS 3
#define TILE_MAX (TILE_COLS * TILE_ROWS)
#define TILE_X0 24
#define TILE_Y0 96
#define TILE_W 216
#define TILE_H 134
#define TILE_GAP 16
static const int TILE_COUNT = sizeof(TILE_CFG) / sizeof(TILE_CFG[0]);
static_assert(sizeof(TILE_CFG) / sizeof(TILE_CFG[0]) <= TILE_MAX, "Too many tiles (max 12)");

enum Screen { SCREEN_INFO, SCREEN_TILES };

static const char *DNI[7]    = {"Niedziela", "Poniedziałek", "Wtorek", "Środa", "Czwartek", "Piątek", "Sobota"};
static const char *MIES[12]  = {"stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
                                "lipca", "sierpnia", "września", "października", "listopada", "grudnia"};

// ---------- State ----------
static unsigned long lastFetchAttempt = 0;
static unsigned long nextFetchDelay   = 0;      // 0 = fetch as soon as possible
static uint8_t failCount              = 0;
static unsigned long failSinceMs      = 0;      // when the current outage started (0 = no outage)
static String lastError;

static time_t dataTs = 0;                       // epoch of the last successful fetch (0 = unknown)
static unsigned long dataMs = 0;                // millis() of the last successful fetch (0 = never in this boot)

static unsigned long wifiLostSinceMs = 0;
static unsigned long lastWifiRetryMs = 0;

static int lastMinute = -1;
static int lastFullRefreshHour = -1;
static String haHost;                           // resolved host (IP for .local names)

struct TileState {
    String state;       // "on" / "off" / "" (unknown)
    String note;        // temporary message instead of the state ("przełączam...", "błąd")
    bool stale;         // last poll failed
    bool dirty;
};
static TileState tiles[TILE_MAX];
static unsigned long lastTilePoll = 0;
static unsigned long tilePollDelay = 0;

static Screen screen = SCREEN_INFO;

static float battVolt = -1;          // smoothed battery voltage, -1 = not measured yet
static int battPct = -1;             // percent shown on screen, -1 = unknown
static bool battDirty = false;
static unsigned long lastBattRead = 0;
static unsigned long lastBattReport = 0;
static unsigned long lastInteractionMs = 0;

static TouchDrvGT911 touch;
static bool touchOk = false;
static bool touchWasDown = false;
static unsigned long lastTouchPoll = 0;
static Preferences prefs;

// ---------- Helpers ----------
static void setField(int i, const String &text)
{
    if (fields[i].text != text) {
        fields[i].text = text;
        fields[i].dirty = true;
    }
}

static void setGray(int i, bool gray)
{
    if (fields[i].gray != gray) {
        fields[i].gray = gray;
        fields[i].dirty = true;
    }
}

static int32_t textWidth(const GFXfont *font, const String &s)
{
    if (s.length() == 0) return 0;
    int32_t x = 0, y = 100, x1, y1, w, h;
    get_text_bounds(font, s.c_str(), &x, &y, &x1, &y1, &w, &h, NULL);
    return w;
}

// Remove the last UTF-8 code point
static void popCodepoint(String &s)
{
    int n = s.length();
    if (n == 0) return;
    n--;
    while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) n--;
    s.remove(n);
}

// Shorten text with "..." so it fits into maxWidth pixels
static String fitText(const GFXfont *font, const String &in, int32_t maxWidth)
{
    if (textWidth(font, in) <= maxWidth) return in;
    String s = in;
    while (s.length() > 0) {
        popCodepoint(s);
        s.trim();
        if (textWidth(font, s + "...") <= maxWidth) return s + "...";
    }
    return "";
}

static void drawText(const GFXfont *font, const String &text, int32_t x, int32_t y, bool gray)
{
    if (text.length() == 0) return;
    FontProperties props = {};
    props.fg_color = gray ? GRAY : 0;
    props.bg_color = 15;
    props.fallback_glyph = 0;
    props.flags = 0;
    write_mode(font, text.c_str(), &x, &y, NULL, BLACK_ON_WHITE, &props);
}

static void drawField(Field &f, bool clearFirst = true)
{
    if (clearFirst) epd_clear_area(f.area);
    String t = fitText(f.font, f.text, f.area.width);
    int32_t x = f.center ? f.area.x + (f.area.width - textWidth(f.font, t)) / 2 : f.area.x;
    drawText(f.font, t, x, f.area.y + f.baseline, f.gray);
    f.dirty = false;
}

// Filled rectangle in a gray level 0 (black) .. 15 (white). No framebuffer needed.
static void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t level)
{
    if (w <= 0 || h <= 0) return;
    int32_t bw = (w + 1) / 2;
    size_t bytes = (size_t)bw * h;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return;
    memset(buf, (level << 4) | level, bytes);
    Rect_t area = {x, y, bw * 2, h};
    epd_draw_grayscale_image(area, buf);
    free(buf);
}

static void frameRect(const Rect_t &r, int32_t t)
{
    fillRect(r.x, r.y, r.width, t, 0);                       // top
    fillRect(r.x, r.y + r.height - t, r.width, t, 0);        // bottom
    fillRect(r.x, r.y, t, r.height, 0);                      // left
    fillRect(r.x + r.width - t, r.y, t, r.height, 0);        // right
}

// Screen 1 icon: 2x2 tiles (-> control screen). Screen 2 icon: calendar (-> info screen).
static void drawNavIcon()
{
    if (!touchOk) return;
    const Rect_t &b = NAV_ICON;
    if (screen == SCREEN_INFO) {
        int32_t s = (b.width - 8) / 2;
        fillRect(b.x, b.y, s, s, 0);
        fillRect(b.x + s + 8, b.y, s, s, 0);
        fillRect(b.x, b.y + s + 8, s, s, 0);
        fillRect(b.x + s + 8, b.y + s + 8, s, s, 0);
    } else {
        frameRect(b, 3);
        fillRect(b.x, b.y, b.width, 14, 0);                  // calendar header
        fillRect(b.x + 12, b.y - 6, 4, 10, 0);               // rings
        fillRect(b.x + b.width - 16, b.y - 6, 4, 10, 0);
        for (int r = 0; r < 2; r++)                          // day dots
            for (int c = 0; c < 3; c++)
                fillRect(b.x + 8 + c * 14, b.y + 22 + r * 12, 6, 6, 0);
    }
}

static Rect_t tileRect(int i)
{
    int c = i % TILE_COLS, r = i / TILE_COLS;
    Rect_t rect = {TILE_X0 + c * (TILE_W + TILE_GAP), TILE_Y0 + r * (TILE_H + TILE_GAP), TILE_W, TILE_H};
    return rect;
}

// Split text into max 2 lines that fit maxWidth (wraps at a space, ellipsizes the rest)
static void wrapTwoLines(const GFXfont *font, const String &text, int32_t maxWidth, String &l1, String &l2)
{
    l1 = text;
    l2 = "";
    if (textWidth(font, text) <= maxWidth) return;
    int cut = -1;
    for (int i = text.indexOf(' '); i >= 0; i = text.indexOf(' ', i + 1)) {
        if (textWidth(font, text.substring(0, i)) <= maxWidth) cut = i;
        else break;
    }
    if (cut < 0) { l1 = fitText(font, text, maxWidth); return; }
    l1 = text.substring(0, cut);
    l2 = fitText(font, text.substring(cut + 1), maxWidth);
}

static void drawTile(int i, bool clearFirst)
{
    Rect_t r = tileRect(i);
    if (clearFirst) epd_clear_area(r);

    TileState &t = tiles[i];
    bool on = t.state == "on";
    bool known = on || t.state == "off";
    bool gray = t.stale || !known;

    if (on) fillRect(r.x, r.y, r.width, r.height, TILE_ON_FILL);
    frameRect(r, on ? 6 : 2);

    const int32_t pad = 14, inner = r.width - 2 * pad;
    String l1, l2;
    wrapTwoLines(&PlSans, TILE_CFG[i].label, inner, l1, l2);
    int32_t y1 = r.y + pad + PlSans.ascender;
    drawText(&PlSans, l1, r.x + pad, y1, gray);
    drawText(&PlSans, l2, r.x + pad, y1 + PlSans.advance_y, gray);

    String st;
    const GFXfont *f = &PlSans;
    if (t.note.length()) st = t.note;
    else if (on) { st = "ON"; f = &PlSansBold; }
    else if (known) st = "OFF";
    else st = "niedostępne";
    drawText(f, fitText(f, st, inner), r.x + pad, r.y + r.height - 16, gray && t.note.length() == 0);

    t.dirty = false;
}

static void markTilesDirty()
{
    for (int i = 0; i < TILE_COUNT; i++) tiles[i].dirty = true;
}

// ---------- Battery ----------
// Li-ion discharge curve (open circuit, approx.): voltage -> percent
static int voltageToPercent(float v)
{
    static const float V[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
    static const int   P[] = {0,     5,     12,    28,    45,    62,    78,    90,    100};
    if (v <= V[0]) return 0;
    for (int i = 1; i < 9; i++) {
        if (v <= V[i]) return P[i - 1] + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * (P[i] - P[i - 1]) + 0.5f);
    }
    return 100;
}

// Returns the battery voltage in V, or -1 when the reading is invalid
static float readBatteryVoltage()
{
    epd_poweron();                       // POWER_EN must be on for the battery divider
    delay(10);
    uint32_t sum = 0;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t mv = analogReadMilliVolts(BATT_PIN);
        if (mv > 500) { sum += mv; n++; }   // ADC2 may return 0 while WiFi is busy - skip those
        delay(2);
    }
    epd_poweroff();
    if (!n) return -1;
    return (float)sum / n * 2.0f / 1000.0f;   // 1:2 divider
}

static void updateBattery()
{
    lastBattRead = millis();
    float v = readBatteryVoltage();
    if (v < 0) return;
    battVolt = (battVolt < 0) ? v : battVolt * 0.7f + v * 0.3f;   // smooth WiFi load dips
    int pct = voltageToPercent(battVolt);
    // redraw only on a real change (avoids e-paper flicker on +-1% noise)
    if (battPct < 0 || abs(pct - battPct) >= 2 || (pct <= BATT_LOW_PCT) != (battPct <= BATT_LOW_PCT)) {
        battPct = pct;
        battDirty = true;
    }
    Serial.printf("[Batt] %.2f V (avg %.2f V) -> %d%%\n", v, battVolt, pct);
}

static void drawBattery(bool clearFirst)
{
    if (clearFirst) epd_clear_area(BATT_AREA);
    battDirty = false;
    if (battPct < 0) return;

    bool low = battPct <= BATT_LOW_PCT;
    // symbol: outline + tip + fill proportional to the charge
    Rect_t body = {BATT_ICON_X, BATT_ICON_Y, BATT_ICON_W, BATT_ICON_H};
    frameRect(body, 2);
    fillRect(BATT_ICON_X + BATT_ICON_W, BATT_ICON_Y + 7, 4, BATT_ICON_H - 14, 0);
    int32_t innerW = BATT_ICON_W - 8;
    int32_t fillW = (innerW * battPct / 100) & ~1;               // even width (2 px per byte)
    if (battPct > 0 && fillW < 2) fillW = 2;
    if (fillW > 0) fillRect(BATT_ICON_X + 4, BATT_ICON_Y + 4, fillW, BATT_ICON_H - 8, low ? 0 : 4);

    String t = String(battPct) + "%";
    const GFXfont *f = low ? &PlSansBold : &PlSans;
    drawText(f, t, BATT_ICON_X - 10 - textWidth(f, t), BATT_ICON_Y + 20, false);
}

// Draw only what changed on the current screen (partial refresh)
static void drawDirty()
{
    bool any = false;
    if (screen == SCREEN_INFO) for (int i = 0; i < F_COUNT; i++) any |= fields[i].dirty;
    else for (int i = 0; i < TILE_COUNT; i++) any |= tiles[i].dirty;
    any |= battDirty;
    if (!any) return;

    epd_poweron();
    if (battDirty) drawBattery(true);
    if (screen == SCREEN_INFO) {
        for (int i = 0; i < F_COUNT; i++) if (fields[i].dirty) drawField(fields[i]);
    } else {
        for (int i = 0; i < TILE_COUNT; i++) if (tiles[i].dirty) drawTile(i, true);
    }
    epd_poweroff();
}

// Full clear + redraw of the current screen (removes ghosting)
static void drawFull()
{
    epd_poweron();
    epd_clear();
    if (screen == SCREEN_INFO) {
        fillRect(SEP_X, SEP_Y, SEP_W, SEP_H, 0);
        for (int i = 0; i < F_COUNT; i++) drawField(fields[i], false);   // screen already cleared
    } else {
        drawText(&PlSansBold, "Sterowanie", 24, 20 + PlSansBold.ascender, false);
        fillRect(24, 78, 840, 2, 0);
        for (int i = 0; i < TILE_COUNT; i++) drawTile(i, false);
    }
    drawNavIcon();
    drawBattery(false);
    epd_poweroff();
}

// "12 min", "2 h 5 min", "3 dni"
static String formatAge(unsigned long seconds)
{
    char buf[24];
    if (seconds < 60) return "<1 min";
    if (seconds < 3600) { snprintf(buf, sizeof(buf), "%lu min", seconds / 60); return buf; }
    if (seconds < 86400) {
        unsigned long m = (seconds % 3600) / 60;
        if (m) snprintf(buf, sizeof(buf), "%lu h %lu min", seconds / 3600, m);
        else snprintf(buf, sizeof(buf), "%lu h", seconds / 3600);
        return buf;
    }
    unsigned long d = seconds / 86400;
    snprintf(buf, sizeof(buf), "%lu %s", d, d == 1 ? "dzień" : "dni");
    return buf;
}

// ---------- WiFi / time ----------
static bool secretsFilled()
{
    return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 && strcmp(HA_TOKEN_VALUE, "YOUR_LONG_LIVED_TOKEN") != 0;
}

static bool timeValid(struct tm &t)
{
    return getLocalTime(&t, 0) && t.tm_year > (2020 - 1900);
}

// Blocking connect - used only in setup()
static void connectWiFiBlocking(uint32_t timeoutMs)
{
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
        esp_task_wdt_reset();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected, IP: %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("WiFi connect failed");
    }
}

// Non-blocking WiFi supervision - called from loop()
static void superviseWiFi()
{
    unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (wifiLostSinceMs) Serial.println("WiFi back");
        wifiLostSinceMs = 0;
        return;
    }
    if (!wifiLostSinceMs) {
        wifiLostSinceMs = now;
        Serial.println("WiFi lost");
    }
    if (now - lastWifiRetryMs >= WIFI_RETRY_MS) {
        lastWifiRetryMs = now;
        WiFi.reconnect();
    }
    if (now - wifiLostSinceMs >= WIFI_RESTART_AFTER_MS) {
        Serial.println("No WiFi for too long - restarting");
        delay(100);
        ESP.restart();
    }
}

// Resolve "xxx.local" through mDNS (plain HTTPClient on ESP32 cannot always do it)
static String resolveHaHost()
{
    String host = HA_HOST_ADDR;
    if (!host.endsWith(".local")) return host;

    static bool mdnsStarted = false;
    if (!mdnsStarted) mdnsStarted = MDNS.begin("lilygo-epd47");

    String name = host.substring(0, host.length() - 6);
    IPAddress ip = MDNS.queryHost(name, 3000);
    if ((uint32_t)ip == 0) {
        Serial.printf("mDNS: %s not found, using hostname\n", host.c_str());
        return host;
    }
    Serial.printf("mDNS: %s -> %s\n", host.c_str(), ip.toString().c_str());
    return ip.toString();
}

// ---------- Persistent cache (NVS) ----------
static void loadCache()
{
    prefs.begin("epd", true);
    for (int i = 0; i < 3; i++) {
        String key = String("l") + (i + 1);
        fields[F_EV1 + i].text = prefs.getString(key.c_str(), "");
    }
    fields[F_WEATHER].text = prefs.getString("w", "");
    dataTs = (time_t)prefs.getLong64("ts", 0);
    prefs.end();
    Serial.printf("Cache loaded (ts=%ld)\n", (long)dataTs);
}

static void saveCache()
{
    prefs.begin("epd", false);
    for (int i = 0; i < 3; i++) {
        String key = String("l") + (i + 1);
        if (prefs.getString(key.c_str(), "") != fields[F_EV1 + i].text) prefs.putString(key.c_str(), fields[F_EV1 + i].text);
    }
    if (prefs.getString("w", "") != fields[F_WEATHER].text) prefs.putString("w", fields[F_WEATHER].text);
    prefs.putLong64("ts", (int64_t)dataTs);
    prefs.end();
}

// ---------- Home Assistant ----------
// Generic HA REST call. `connErr` is set when HA/WiFi is unreachable or the token is rejected
// (no point trying more requests in this round).
static bool haRequest(const char *method, const String &path, const String &body,
                      String &payload, String &err, bool &connErr)
{
    connErr = false;
    if (WiFi.status() != WL_CONNECTED) { err = "brak WiFi"; connErr = true; return false; }
    if (haHost.length() == 0) haHost = resolveHaHost();

    HTTPClient http;
    String url = String("http://") + haHost + ":" + HA_PORT_NUM + path;
    http.setConnectTimeout(4000);
    http.setTimeout(5000);
    if (!http.begin(url)) { err = "URL"; return false; }
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN_VALUE);
    http.addHeader("Content-Type", "application/json");

    int code = (strcmp(method, "POST") == 0) ? http.POST(body) : http.GET();
    esp_task_wdt_reset();
    if (code < 200 || code > 299) {
        http.end();
        if (code < 0) {
            err = "brak połączenia";
            connErr = true;
            haHost = "";                     // re-resolve next time (IP may have changed)
        } else {
            err = String("HTTP ") + code;
            connErr = (code == 401 || code == 403);   // token problem: every request will fail the same way
        }
        return false;
    }
    payload = http.getString();
    http.end();
    return true;
}

// Read the state of one entity
static bool fetchState(const char *entity, String &out, String &err, bool &connErr)
{
    String payload;
    if (!haRequest("GET", String("/api/states/") + entity, "", payload, err, connErr)) return false;

    JsonDocument filter;
    filter["state"] = true;
    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (jerr) { err = "JSON"; return false; }

    const char *state = doc["state"];
    if (!state) { err = "brak stanu"; return false; }

    out = state;
    if (out == "unknown" || out == "unavailable") out = "";
    return true;
}

// ---------- Tiles (screen 2) ----------
static void setTileState(int i, const String &state, bool stale)
{
    TileState &t = tiles[i];
    if (t.state != state || t.stale != stale || t.note.length()) {
        t.state = state;
        t.stale = stale;
        t.note = "";
        t.dirty = true;
    }
}

static void pollTiles()
{
    lastTilePoll = millis();
    tilePollDelay = TILE_POLL_MS;
    bool connErr = false;
    for (int i = 0; i < TILE_COUNT; i++) {
        String v, err;
        if (!connErr && fetchState(TILE_CFG[i].entity, v, err, connErr)) {
            if (v != tiles[i].state) Serial.printf("[Tile %d] %s = %s\n", i, TILE_CFG[i].entity, v.c_str());
            setTileState(i, v, false);
        } else {
            setTileState(i, tiles[i].state, true);   // keep last known state, grayed
        }
    }
}

static void toggleTile(int i)
{
    Serial.printf("[Tile %d] toggle %s\n", i, TILE_CFG[i].entity);
    TileState &t = tiles[i];
    if (WiFi.status() != WL_CONNECTED) {
        t.note = "brak WiFi";
        t.dirty = true;
        lastTilePoll = millis();
        tilePollDelay = 3000;                 // show the real state again shortly
        return;
    }

    String payload, err;
    bool connErr;
    String body = String("{\"entity_id\":\"") + TILE_CFG[i].entity + "\"}";
    if (!haRequest("POST", "/api/services/homeassistant/toggle", body, payload, err, connErr)) {
        Serial.printf("[Tile %d] toggle failed: %s\n", i, err.c_str());
        t.note = "błąd";
        t.dirty = true;
        lastTilePoll = millis();
        tilePollDelay = 3000;
        return;
    }

    // Optimistic update for instant feedback, then confirm with a poll
    if (t.state == "on") t.state = "off";
    else if (t.state == "off") t.state = "on";
    t.note = "";
    t.stale = false;
    t.dirty = true;
    lastTilePoll = millis();
    tilePollDelay = 1500;
}

// Publish the battery state to HA (creates/updates sensor.ha_display_battery)
static void reportBattery()
{
    lastBattReport = millis();
    if (battPct < 0 || WiFi.status() != WL_CONNECTED || failSinceMs) return;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"state\":%d,\"attributes\":{\"unit_of_measurement\":\"%%\",\"device_class\":\"battery\","
             "\"state_class\":\"measurement\",\"voltage\":%.2f,\"friendly_name\":\"HA Display bateria\"}}",
             battPct, battVolt);
    String payload, err;
    bool connErr;
    if (!haRequest("POST", String("/api/states/") + ENTITY_BATTERY, body, payload, err, connErr))
        Serial.printf("[Batt] report failed: %s\n", err.c_str());
}

static int tileAt(int16_t x, int16_t y)
{
    for (int i = 0; i < TILE_COUNT; i++) {
        Rect_t r = tileRect(i);
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return i;
    }
    return -1;
}

// ---------- Touch (GT911) ----------
static bool initTouch()
{
    Wire.begin(BOARD_SDA, BOARD_SCL);
    pinMode(TOUCH_INT, OUTPUT);              // wake the controller (may be in sleep state)
    digitalWrite(TOUCH_INT, HIGH);
    delay(20);

    // The reset pin is pulled up in hardware, so the address must be found by scanning
    uint8_t addr = 0;
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) addr = 0x14;
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) addr = 0x5D;
    if (!addr) {
        Serial.println("Touch: GT911 not found - touch disabled");
        return false;
    }

    touch.setPins(-1, TOUCH_INT);
    if (!touch.begin(Wire, addr, BOARD_SDA, BOARD_SCL)) {
        Serial.println("Touch: GT911 init failed - touch disabled");
        return false;
    }
    touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(false, true);
    Serial.printf("Touch: GT911 at 0x%02X\n", addr);
    return true;
}

static bool inRect(int16_t x, int16_t y, const Rect_t &r, int margin)
{
    return x >= r.x - margin && x < r.x + r.width + margin && y >= r.y - margin && y < r.y + r.height + margin;
}

// Returns true when a new tap (finger down) was detected; x/y are screen coordinates
static bool pollTouch(int16_t &x, int16_t &y)
{
    if (!touchOk) return false;
    if (millis() - lastTouchPoll < TOUCH_POLL_MS) return false;
    lastTouchPoll = millis();

    bool down = touch.getPoint(&x, &y, 1) > 0;
    bool tap = down && !touchWasDown;        // react once per touch, not while the finger stays
    touchWasDown = down;
    return tap;
}

// One fetch round. Returns true when all entities were read.
static bool fetchHaData()
{
    String err, v;
    bool connErr = false;
    String newLines[3], newWeather;
    int ok = 0;
    const int total = 4;

    // Collect everything first; apply only when the whole set is consistent
    for (int i = 0; i < 3 && !connErr; i++) {
        if (fetchState(ENTITY_LINE[i], v, err, connErr)) { newLines[i] = v; ok++; }
    }
    if (!connErr && fetchState(ENTITY_WEATHER, v, err, connErr)) { newWeather = v; ok++; }

    struct tm t;
    bool haveTime = timeValid(t);

    // Clock fallback until NTP is synchronised. Format: "13:51  Niedziela, 15 listopada"
    if (!haveTime && !connErr && fetchState(ENTITY_CLOCK, v, err, connErr)) {
        int sp = v.indexOf("  ");
        int comma = v.indexOf(',');
        if (sp > 0) {
            setField(F_CLOCK, v.substring(0, sp));
            if (comma > sp) {
                setField(F_DAY, v.substring(sp + 2, comma));
                String d = v.substring(comma + 1);
                d.trim();
                setField(F_DATE, d);
            }
        }
    }

    if (ok == total) {
        for (int i = 0; i < 3; i++) setField(F_EV1 + i, newLines[i]);
        setField(F_WEATHER, newWeather);
        dataMs = millis();
        dataTs = haveTime ? time(nullptr) : 0;
        saveCache();
        lastError = "";
        Serial.printf("[HA] ok. W: %s | 1: %s | 2: %s | 3: %s\n", newWeather.c_str(),
                      newLines[0].c_str(), newLines[1].c_str(), newLines[2].c_str());
        return true;
    }

    lastError = err;
    Serial.printf("[HA] failed (%d/%d): %s\n", ok, total, err.c_str());
    return false;
}

// Age of the displayed data in seconds; -1 = unknown
static long dataAgeSeconds()
{
    if (dataMs) return (long)((millis() - dataMs) / 1000UL);
    struct tm t;
    if (dataTs && timeValid(t)) return (long)(time(nullptr) - dataTs);
    return -1;
}

static bool dataFromAnotherDay()
{
    struct tm now, then;
    if (!dataTs || !timeValid(now)) return false;
    localtime_r(&dataTs, &then);
    return then.tm_yday != now.tm_yday || then.tm_year != now.tm_year;
}

// "13:51" today, "14.11 13:51" otherwise
static String formatDataTime()
{
    if (!dataTs) return "";
    struct tm then, now;
    localtime_r(&dataTs, &then);
    char buf[24];
    if (timeValid(now) && then.tm_yday == now.tm_yday && then.tm_year == now.tm_year)
        snprintf(buf, sizeof(buf), "%02d:%02d", then.tm_hour, then.tm_min);
    else
        snprintf(buf, sizeof(buf), "%02d.%02d %02d:%02d", then.tm_mday, then.tm_mon + 1, then.tm_hour, then.tm_min);
    return buf;
}

// Status line + gray "stale" marking
static void updateStatusAndStaleness()
{
    long age = dataAgeSeconds();
    bool hasData = fields[F_EV1].text.length() || fields[F_WEATHER].text.length();
    bool stale = hasData && (age < 0 || age > STALE_AFTER_S || dataFromAnotherDay());

    for (int i = F_WEATHER; i <= F_EV3; i++) {
        if (i == F_HEADER) continue;
        setGray(i, stale);
    }
    setField(F_HEADER, stale ? "Najbliższe wydarzenia (nieaktualne)" : "Najbliższe wydarzenia");

    String status;
    String dataTime = formatDataTime();
    String dataPart = dataTime.length() ? String("dane ") + dataTime : String("brak danych");

    if (WiFi.status() != WL_CONNECTED) {
        status = String("Brak WiFi");
        if (wifiLostSinceMs) status += String(" od ") + formatAge((millis() - wifiLostSinceMs) / 1000UL);
        status += String("  ·  ") + dataPart;
    } else if (failSinceMs) {
        status = (lastError == "brak połączenia" ? String("Brak HA") : String("HA: ") + lastError) + " od " + formatAge((millis() - failSinceMs) / 1000UL) + "  ·  " + dataPart;
    } else {
        status = String("Aktualizacja ") + (dataTime.length() ? dataTime : String("OK")) + "  ·  WiFi " + WiFi.RSSI() + " dBm";
    }
    if (battPct >= 0 && battPct <= BATT_LOW_PCT) status = String("Słaba bateria!  ·  ") + status;
    setField(F_STATUS, status);
}

static void updateClockFields(const struct tm &t)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    setField(F_CLOCK, buf);
    setField(F_DAY, DNI[t.tm_wday]);
    snprintf(buf, sizeof(buf), "%d %s %d", t.tm_mday, MIES[t.tm_mon], t.tm_year + 1900);
    setField(F_DATE, buf);
}

static void doFetch()
{
    lastFetchAttempt = millis();
    if (fetchHaData()) {
        failCount = 0;
        failSinceMs = 0;
        nextFetchDelay = DATA_INTERVAL_MS;
    } else {
        if (!failSinceMs) failSinceMs = millis();
        if (failCount < 255) failCount++;
        int idx = min((int)failCount - 1, 2);
        nextFetchDelay = RETRY_INTERVALS_MS[idx];
    }
    updateStatusAndStaleness();
}

static void setupWatchdog()
{
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);   // re-configures the TWDT if already running
#endif
    esp_task_wdt_add(NULL);                   // watch the loop task
}

// ---------- Arduino lifecycle ----------
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\nEPD47 Home Assistant desk display");

    setupWatchdog();
    epd_init();

    if (!secretsFilled()) {
        setField(F_STATUS, "Uzupełnij secrets.h (WiFi i token HA) i wgraj ponownie");
        drawFull();
        return;
    }

    // Show last known data straight away (grayed out until confirmed fresh)
    loadCache();

    connectWiFiBlocking(20000);
    configTzTime(TZ_INFO, NTP_1, NTP_2);

    struct tm t;
    for (int i = 0; i < 20 && WiFi.status() == WL_CONNECTED && !timeValid(t); i++) delay(250);   // NTP up to ~5 s
    if (timeValid(t)) {
        updateClockFields(t);
        lastMinute = t.tm_min;
        lastFullRefreshHour = t.tm_hour;
    }

    touchOk = initTouch();

    if (WiFi.status() != WL_CONNECTED) wifiLostSinceMs = millis();
    updateBattery();
    doFetch();
    reportBattery();
    drawFull();
}

void loop()
{
    esp_task_wdt_reset();
    if (!secretsFilled()) { delay(10000); return; }

    superviseWiFi();

    unsigned long now = millis();
    bool full = false;

    struct tm t;
    if (timeValid(t) && t.tm_min != lastMinute) {
        lastMinute = t.tm_min;
        updateClockFields(t);
        updateStatusAndStaleness();          // keeps "od X min" and stale marking current
        if (t.tm_min == FULL_REFRESH_MINUTE && t.tm_hour != lastFullRefreshHour) {
            lastFullRefreshHour = t.tm_hour;
            full = true;
        }
    }

    // Touch
    int16_t tx, ty;
    if (pollTouch(tx, ty)) {
        Serial.printf("[Touch] %d,%d\n", tx, ty);
        lastInteractionMs = now;
        if (inRect(tx, ty, NAV_ICON, TOUCH_MARGIN)) {
            screen = (screen == SCREEN_INFO) ? SCREEN_TILES : SCREEN_INFO;
            Serial.printf("[Screen] %s\n", screen == SCREEN_INFO ? "info" : "tiles");
            if (screen == SCREEN_TILES) {
                if (WiFi.status() == WL_CONNECTED) pollTiles();   // fresh states before drawing
                markTilesDirty();
            }
            full = true;
        } else if (screen == SCREEN_INFO) {
            nextFetchDelay = 0;              // fetch HA data in this loop pass
            full = true;                     // and redraw the whole screen (clears ghosting)
        } else {
            int i = tileAt(tx, ty);
            if (i >= 0) toggleTile(i);
            else tilePollDelay = 0;          // tap outside tiles: refresh states
        }
    }

    // Back to the info screen after a while without touch
    if (screen == SCREEN_TILES && now - lastInteractionMs >= SCREEN2_TIMEOUT_MS) {
        screen = SCREEN_INFO;
        Serial.println("[Screen] timeout -> info");
        full = true;
    }

    // Tile states - only while screen 2 is shown; skipped while HA is down
    if (screen == SCREEN_TILES && now - lastTilePoll >= tilePollDelay) {
        if (WiFi.status() == WL_CONNECTED && !failSinceMs) pollTiles();
        else {
            lastTilePoll = now;
            tilePollDelay = TILE_POLL_MS;
            for (int i = 0; i < TILE_COUNT; i++) setTileState(i, tiles[i].state, true);
        }
    }

    // Battery
    if (now - lastBattRead >= BATT_READ_MS) {
        int before = battPct;
        updateBattery();
        if ((before <= BATT_LOW_PCT) != (battPct <= BATT_LOW_PCT)) updateStatusAndStaleness();
    }
    if (now - lastBattReport >= BATT_REPORT_MS) reportBattery();

    if (now - lastFetchAttempt >= nextFetchDelay) {
        if (WiFi.status() == WL_CONNECTED) {
            doFetch();
        } else {
            lastFetchAttempt = now;          // no WiFi: don't block, just refresh the status
            nextFetchDelay = RETRY_INTERVALS_MS[0];
            updateStatusAndStaleness();
        }
    }

    if (full) drawFull();
    else drawDirty();

    delay(20);   // short tick (touch is polled every TOUCH_POLL_MS)
}
