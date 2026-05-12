#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "globals.h"
#include "wifi_config.h"

namespace {

static const char *kEntryCommand = "#WIFI_UART_CFG#";
static const char *kPrefsNamespace = "wifi_cfg";
static const char *kPrefsSsidKey = "ssid";
static const char *kPrefsPassKey = "pass";

static const size_t kMaxSsidLen = 32;
static const size_t kMaxPassLen = 64;
static const size_t kMaxLineLen = 256;

enum class ConfigMode : uint8_t {
  None = 0,
  Uart = 1,
  Web = 2
};

Preferences g_prefs;
bool g_prefs_open = false;

ConfigMode g_mode = ConfigMode::None;
unsigned long g_mode_timeout_ms = 180000;
unsigned long g_last_activity_ms = 0;

uint8_t g_io0_pin = 0;
unsigned long g_hold_ms = 3000;
bool g_button_down = false;
bool g_long_press_handled = false;
unsigned long g_button_down_ms = 0;

char g_saved_ssid[kMaxSsidLen + 1] = {0};
char g_saved_pass[kMaxPassLen + 1] = {0};
char g_pending_ssid[kMaxSsidLen + 1] = {0};
char g_pending_pass[kMaxPassLen + 1] = {0};
bool g_has_pending = false;

String g_line_buffer;

void setString(char *dest, size_t dest_len, const String &value) {
  if (dest_len == 0) return;
  value.toCharArray(dest, dest_len);
  dest[dest_len - 1] = '\0';
}

void setCString(char *dest, size_t dest_len, const char *value) {
  if (dest_len == 0) return;
  strlcpy(dest, value, dest_len);
}

void touchActivity() {
  g_last_activity_ms = millis();
}

bool openPrefs() {
  if (g_prefs_open) return true;
  g_prefs_open = g_prefs.begin(kPrefsNamespace, false);
  return g_prefs_open;
}

void closePrefs() {
  if (!g_prefs_open) return;
  g_prefs.end();
  g_prefs_open = false;
}

void printOk() {
  Serial.println("OK");
}

void printError(int code, const char *msg) {
  Serial.printf("ERROR,%d,%s\r\n", code, msg);
}

String escapeField(const String &src) {
  String out;
  out.reserve(src.length() + 8);
  for (size_t i = 0; i < src.length(); i++) {
    char c = src.charAt(i);
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

const char *authToText(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
#if defined(WIFI_AUTH_WPA2_ENTERPRISE)
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
#endif
#if defined(WIFI_AUTH_WPA3_PSK)
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
#endif
#if defined(WIFI_AUTH_WPA2_WPA3_PSK)
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
#endif
    default: return "UNKNOWN";
  }
}

const char *statusToText(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_CONNECT_FAILED: return "CONNECT_FAIL";
    case WL_CONNECTION_LOST: return "CONN_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_IDLE_STATUS: return "IDLE";
    default: return "UNKNOWN";
  }
}

bool parseQuoted(const String &line, size_t *pos, String *value) {
  if (*pos >= line.length() || line.charAt(*pos) != '"') return false;
  (*pos)++;

  String out;
  bool escaped = false;

  for (; *pos < line.length(); (*pos)++) {
    char c = line.charAt(*pos);
    if (escaped) {
      out += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      (*pos)++;
      *value = out;
      return true;
    }
    out += c;
  }
  return false;
}

void loadSavedCredentialsInternal() {
  if (!openPrefs()) return;

  String ssid = g_prefs.getString(kPrefsSsidKey, "");
  String pass = g_prefs.getString(kPrefsPassKey, "");

  if (ssid.length() > 0 && ssid.length() <= kMaxSsidLen) {
    setString(g_saved_ssid, sizeof(g_saved_ssid), ssid);
    if (pass.length() <= kMaxPassLen) {
      setString(g_saved_pass, sizeof(g_saved_pass), pass);
    }
  }

  closePrefs();
}

bool savePendingCredentialsInternal() {
  if (!g_has_pending) return false;
  if (!openPrefs()) return false;

  g_prefs.putString(kPrefsSsidKey, g_pending_ssid);
  g_prefs.putString(kPrefsPassKey, g_pending_pass);

  String verify_ssid = g_prefs.getString(kPrefsSsidKey, "");
  String verify_pass = g_prefs.getString(kPrefsPassKey, "");
  bool ok = (verify_ssid == String(g_pending_ssid)) && (verify_pass == String(g_pending_pass));

  closePrefs();
  if (!ok) return false;

  setCString(g_saved_ssid, sizeof(g_saved_ssid), g_pending_ssid);
  setCString(g_saved_pass, sizeof(g_saved_pass), g_pending_pass);
  return true;
}

void exitCurrentMode(bool announce) {
  if (g_mode == ConfigMode::Uart && announce) {
    Serial.println("+UARTCFG:EXIT");
  } else if (g_mode == ConfigMode::Web && announce) {
    Serial.println("+WEBCFG:EXIT");
  }
  g_mode = ConfigMode::None;
}

void enterUartMode() {
  if (g_mode == ConfigMode::Web) {
    printError(3, "BUSY_WEBCFG");
    return;
  }

  g_mode = ConfigMode::Uart;
  touchActivity();
  Serial.printf("+UARTCFG:ENTER,VER=1.1,TIMEOUT=%lu\r\n", g_mode_timeout_ms / 1000UL);
  printOk();
}

void enterWebMode() {
  if (g_mode == ConfigMode::Uart) {
    printError(3, "BUSY_UARTCFG");
    return;
  }

  g_mode = ConfigMode::Web;
  touchActivity();
  Serial.println("+WEBCFG:ENTER");
}

void commandHelp() {
  Serial.println("+HELP:#WIFI_UART_CFG#");
  Serial.println("+HELP:AT");
  Serial.println("+HELP:AT+HELP");
  Serial.println("+HELP:AT+WSCAN");
  Serial.println("+HELP:AT+WSET=\"<ssid>\",\"<password>\"");
  Serial.println("+HELP:AT+WSAVE");
  Serial.println("+HELP:AT+WINFO?");
  Serial.println("+HELP:AT+WEXIT");
  printOk();
}

void commandScan() {
  Serial.println("+WSCAN:START");

  int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    printError(4, "SCAN_FAILED");
    return;
  }

  for (int i = 0; i < count; i++) {
    String ssid = escapeField(WiFi.SSID(i));
    int32_t rssi = WiFi.RSSI(i);
    const char *auth = authToText((wifi_auth_mode_t)WiFi.encryptionType(i));
    int32_t channel = WiFi.channel(i);
    Serial.printf("+WSCAN:ITEM,%d,\"%s\",%ld,%s,%ld\r\n", i, ssid.c_str(), (long)rssi, auth, (long)channel);
  }
  WiFi.scanDelete();
  Serial.printf("+WSCAN:END,%d\r\n", count);
  printOk();
}

void commandSet(const String &line) {
  const String prefix = "AT+WSET=";
  if (!line.startsWith(prefix)) {
    printError(2, "BAD_PARAMS");
    return;
  }

  size_t pos = prefix.length();
  String ssid;
  String pass;

  if (!parseQuoted(line, &pos, &ssid)) {
    printError(2, "BAD_PARAMS");
    return;
  }
  if (pos >= line.length() || line.charAt(pos) != ',') {
    printError(2, "BAD_PARAMS");
    return;
  }
  pos++;
  if (!parseQuoted(line, &pos, &pass)) {
    printError(2, "BAD_PARAMS");
    return;
  }
  if (pos != line.length()) {
    printError(2, "BAD_PARAMS");
    return;
  }

  if (ssid.length() == 0 || ssid.length() > kMaxSsidLen || pass.length() > kMaxPassLen) {
    printError(2, "BAD_PARAMS");
    return;
  }

  setString(g_pending_ssid, sizeof(g_pending_ssid), ssid);
  setString(g_pending_pass, sizeof(g_pending_pass), pass);
  g_has_pending = true;

  String escaped = escapeField(ssid);
  Serial.printf("+WSET:OK,SSID=\"%s\",PWD_LEN=%u\r\n", escaped.c_str(), (unsigned)pass.length());
  printOk();
}

void commandSave() {
  if (!g_has_pending) {
    printError(2, "NO_PENDING_CREDENTIALS");
    return;
  }

  if (!savePendingCredentialsInternal()) {
    printError(5, "FLASH_SAVE_FAIL");
    return;
  }

  Serial.println("+WSAVE:OK");
  printOk();
  String escaped = escapeField(String(g_saved_ssid));
  Serial.printf("+WIFI:CONNECTING,\"%s\"\r\n", escaped.c_str());

  WiFi.disconnect(false, true);
  WiFi.begin(g_saved_ssid, g_saved_pass);
}

void commandInfo() {
  wl_status_t status = WiFi.status();
  Serial.printf(
    "+WINFO:MODE=%s,CONNECTED=%d,SAVED=%d,PENDING=%d\r\n",
    (g_mode == ConfigMode::Uart) ? "UART" : (g_mode == ConfigMode::Web ? "WEB" : "NONE"),
    status == WL_CONNECTED ? 1 : 0,
    g_saved_ssid[0] ? 1 : 0,
    g_has_pending ? 1 : 0
  );

  if (g_saved_ssid[0]) {
    Serial.printf("+WINFO:SAVED_SSID=\"%s\"\r\n", escapeField(String(g_saved_ssid)).c_str());
  }

  if (status == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("+WINFO:IP=\"%s\",STATE=%s\r\n", ip.toString().c_str(), statusToText(status));
  } else {
    Serial.printf("+WINFO:STATE=%s\r\n", statusToText(status));
  }
  printOk();
}

void handleUartCommand(const String &line) {
  if (line == "AT") {
    printOk();
    return;
  }
  if (line == "AT+HELP") {
    commandHelp();
    return;
  }
  if (line == "AT+WSCAN") {
    commandScan();
    return;
  }
  if (line.startsWith("AT+WSET=")) {
    commandSet(line);
    return;
  }
  if (line == "AT+WSAVE") {
    commandSave();
    return;
  }
  if (line == "AT+WINFO?") {
    commandInfo();
    return;
  }
  if (line == "AT+WEXIT") {
    exitCurrentMode(true);
    printOk();
    return;
  }
  printError(1, "UNKNOWN_CMD");
}

void processLine(const String &line) {
  if (line.length() == 0) return;
  if (line == kEntryCommand) {
    enterUartMode();
    return;
  }

  if (g_mode == ConfigMode::Web) {
    if (line == "AT" || line.startsWith("AT+")) {
      printError(3, "BUSY_WEBCFG");
    }
    return;
  }

  if (g_mode != ConfigMode::Uart) {
    if (line == "AT" || line.startsWith("AT+")) {
      printError(3, "NOT_IN_UARTCFG");
    }
    return;
  }

  touchActivity();
  handleUartCommand(line);
}

void processSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_line_buffer;
      g_line_buffer = "";
      line.trim();
      processLine(line);
      continue;
    }
    if (g_line_buffer.length() < kMaxLineLen) g_line_buffer += c;
  }
}

void processButton() {
  bool is_down = digitalRead(g_io0_pin) == LOW;

  if (is_down && !g_button_down) {
    g_button_down = true;
    g_long_press_handled = false;
    g_button_down_ms = millis();
    return;
  }

  if (!is_down && g_button_down) {
    g_button_down = false;
    g_long_press_handled = false;
    return;
  }

  if (!is_down || !g_button_down || g_long_press_handled) return;

  if ((millis() - g_button_down_ms) >= g_hold_ms) {
    g_long_press_handled = true;
    enterWebMode();
  }
}

void processTimeout() {
  if (g_mode == ConfigMode::None || g_mode_timeout_ms == 0) return;
  if ((millis() - g_last_activity_ms) < g_mode_timeout_ms) return;

  if (g_mode == ConfigMode::Uart) {
    Serial.println("+UARTCFG:TIMEOUT");
  } else if (g_mode == ConfigMode::Web) {
    Serial.println("+WEBCFG:TIMEOUT");
  }
  exitCurrentMode(false);
}

} // 匿名命名空间结束

void wifiCfgInit(uint8_t io0_pin, unsigned long hold_ms, unsigned long mode_timeout_ms) {
  g_io0_pin = io0_pin;
  g_hold_ms = hold_ms;
  g_mode_timeout_ms = mode_timeout_ms;
  g_mode = ConfigMode::None;
  g_has_pending = false;
  g_line_buffer = "";

  pinMode(g_io0_pin, INPUT_PULLUP);
  loadSavedCredentialsInternal();
}

void wifiCfgPoll(void) {
  processButton();
  processSerial();
  processTimeout();
}

void wifiCfgLoadBootCredentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len) {
  if (ssid_out && ssid_len > 0) ssid_out[0] = '\0';
  if (pass_out && pass_len > 0) pass_out[0] = '\0';

  if (g_saved_ssid[0]) {
    if (ssid_out) setCString(ssid_out, ssid_len, g_saved_ssid);
    if (pass_out) setCString(pass_out, pass_len, g_saved_pass);
  }
}

bool wifiCfgIsWebModeActive(void) {
  return g_mode == ConfigMode::Web;
}
