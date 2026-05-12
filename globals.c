#include <stdio.h>
#include <stdint.h>

#ifndef ARDUINO_PLATFORM
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
#endif

#include "globals.h"

#if defined(ESP_PLATFORM) && !defined(ARDUINO_PLATFORM)
  #include "esp_task_wdt.h"
  #include "esp_timer.h"

  #define TASK_YIELD_INTERVAL 1000 * 1000
  #define TASK_YIELD_TICKS 1

  int64_t last_yield = 0;
  void task_yield () {
    int64_t time_now = esp_timer_get_time();
    if (time_now - last_yield < TASK_YIELD_INTERVAL) return;
    vTaskDelay(TASK_YIELD_TICKS);
    last_yield = time_now;
  }
#endif

ssize_t recv_count;
uint8_t recv_buffer[MAX_RECV_BUF_LEN] = {0};

uint32_t world_seed = INITIAL_WORLD_SEED;
uint32_t rng_seed = INITIAL_RNG_SEED;

uint16_t world_time = 0;
uint32_t server_ticks = 0;

char motd[] = { "An ESP32MC server" };
uint8_t motd_len = sizeof(motd) - 1;

#ifdef SEND_BRAND
  char brand[] = { "ESP32MC" };
  uint8_t brand_len = sizeof(brand) - 1;
#endif

uint16_t client_count;

int debug_last_client_fd = -1;
int debug_last_state = -1;
int debug_last_packet_length = -1;
int debug_last_packet_id = -1;
int debug_last_payload_length = -1;
int debug_last_out_packet_length = -1;
int debug_last_out_packet_id = -1;
const char *debug_last_out_stage = "none";

int debug_io_expected = 0;
int debug_io_progress = 0;
int debug_io_require_first = 0;
int debug_io_last_errno = 0;
int debug_io_last_result = 0;
int64_t debug_io_wait_us = 0;

BlockChange block_changes[MAX_BLOCK_CHANGES];
int block_changes_count = 0;

PlayerData player_data[MAX_PLAYERS];
int player_data_count = 0;

MobData mob_data[MAX_MOBS];


