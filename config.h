// config.h
#pragma once

// ---- Display (TFT_eSPI uses User_Setup.h already) ---------------------------
#define TFT_BL_PIN          21

// ---- Touch (bit-banged XPT2046) --------------------------------------------
#define TOUCH_CS_PIN        33
#define TOUCH_CLK_PIN       25
#define TOUCH_MOSI_PIN      32
#define TOUCH_MISO_PIN      39

// ---- SD on VSPI -------------------------------------------------------------
#define SD_CS_PIN            5
#define SD_SCK_PIN          18
#define SD_MISO_PIN         19
#define SD_MOSI_PIN         23
#define SD_FREQ_HZ          25000000

// ---- Screen geometry --------------------------------------------------------
#define SCR_W              320
#define SCR_H              240
#define TAB_H               28
#define STATUS_H            18
#define FOOTER_H            32
#define LIST_TOP          (TAB_H + STATUS_H)
#define LIST_BOT          (SCR_H - FOOTER_H)
#define ROW_H               28
#define LIST_ROWS         ((LIST_BOT - LIST_TOP) / ROW_H)

// ---- Radio schedule (M1: no promiscuous yet) --------------------------------
#define SURVEY_PERIOD_MS   8000    // WiFi API scan window
#define BLE_SCAN_MS       12000    // continuous-ish BLE window before restart

// ---- Baseline ---------------------------------------------------------------
#define LEARN_WINDOW_MS  (10 * 60 * 1000)   // 10 min auto-learn
#define MAX_BASELINE_WIFI  128
#define MAX_BASELINE_BLE   128
#define ALERT_AUTO_ACK_DWELL_MS (2 * 60 * 1000) // auto-ack after 2 min

// ---- Live view caps ---------------------------------------------------------
#define MAX_WIFI            32
#define MAX_BLE             48

// ---- Queue sizes ------------------------------------------------------------
#define SD_QUEUE_LEN        24
#define BLE_QUEUE_LEN       32
#define ESPNOW_QUEUE_LEN    24

// ---- ESP-NOW ----------------------------------------------------------------
#define ESPNOW_CHANNEL       1
#define MAX_ESPNOW_FRAMES   16    // in-memory ring of sniffed frames
#define CHAT_LOG_MAX         8    // in-memory chat log
#define CHAT_MSG_MAX       120    // chars per message (ESP-NOW caps at 250)

// ---- SD rotation ------------------------------------------------------------
#define LOG_ROTATE_BYTES   (1 * 1024 * 1024)
#define LOG_ROTATE_KEEP     5

// ---- Debug ------------------------------------------------------------------
#define HEARTBEAT_MS      1000
#define TOUCH_DEBOUNCE_MS  220

// ---- Built-in ambient light sensor (LDR on many CYD boards) ---------------
// CYD variants commonly wire an LDR divider to GPIO34 (ADC1_CH6).
// If your board uses a different pin, override here.
#define LIGHT_SENSOR_PIN      34
#define LIGHT_SENSOR_MIN      0
#define LIGHT_SENSOR_MAX      4095
// Set to 1 when raw ADC goes down as scene gets brighter.
#define LIGHT_SENSOR_INVERT   0

// ---- Alert handling ---------------------------------------------------------
// If an unacknowledged alert signature (type+MAC) persists for this long, it
// is auto-acknowledged and suppressed from the active alert view.
#define ALERT_AUTO_ACK_DWELL_MS (2 * 60 * 1000UL)
