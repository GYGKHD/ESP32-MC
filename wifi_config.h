#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void wifiCfgInit(uint8_t io0_pin, unsigned long hold_ms, unsigned long mode_timeout_ms);
void wifiCfgPoll(void);
void wifiCfgLoadBootCredentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len);
bool wifiCfgIsWebModeActive(void);

#endif
