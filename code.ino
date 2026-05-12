/**
 * Arduino 版 ESP32MC
 * 协议版本 26.1.2 / 775
 */

#define ARDUINO_PLATFORM

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include "globals.h"
#include "arduino_compat.h"
#include "wifi_config.h"

extern "C" void esp32mc_start();

static bool wifi_ready = false;
static bool server_started = false;

static const uint8_t kWebCfgButtonPin = 0;      // IO0 / BOOT 键
static const uint8_t kServerActivityLedPin = 9; // IO9 活动灯

static const char *resetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unmapped";
  }
}

static void startServerTask() {
  if (server_started) return;

  BaseType_t task_ok = xTaskCreatePinnedToCore(
    [](void*) { esp32mc_start(); vTaskDelete(NULL); },
    "esp32mc",
    16384,
    NULL,
    5,
    NULL,
    1
  );

  if (task_ok == pdPASS) server_started = true;

  Serial.printf(
    "esp32mc task create: %s, free heap after create: %u bytes\n",
    task_ok == pdPASS ? "ok" : "failed",
    ESP.getFreeHeap()
  );
}

void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      wifi_ready = true;
      arduino_activity_led_set_enabled(1);
      startServerTask();
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi disconnected, reconnecting...");
      wifi_ready = false;
      arduino_activity_led_set_enabled(0);
      WiFi.reconnect();
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.println("\nESP32MC starting...");
  Serial.printf("Reset reason: %d (%s)\n", (int)reset_reason, resetReasonString(reset_reason));
  Serial.printf("Free heap on boot: %u bytes\n", ESP.getFreeHeap());

  arduino_activity_led_begin(kServerActivityLedPin, LOW);
  arduino_activity_led_set_enabled(0);

  wifiCfgInit(kWebCfgButtonPin, 3000, 180000);

  char wifi_ssid[33];
  char wifi_pass[65];
  wifiCfgLoadBootCredentials(wifi_ssid, sizeof(wifi_ssid), wifi_pass, sizeof(wifi_pass));

  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);

  if (wifi_ssid[0]) {
    Serial.printf("Connecting to saved WiFi: %s\n", wifi_ssid);
    WiFi.begin(wifi_ssid, wifi_pass);
  } else {
    Serial.println("WiFi not configured. Hold BOOT to enter config mode.");
  }
}

void loop() {
  wifiCfgPoll();
  vTaskDelay(pdMS_TO_TICKS(20));
}
