#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#ifdef ARDUINO_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "registries.h"
#include "worldgen.h"
#include "structures.h"
#include "serialize.h"
#include "procedures.h"

int client_states[MAX_PLAYERS * 2];

#ifdef ARDUINO_PLATFORM
static void logRuntimeDiag (const char *stage) {
  UBaseType_t watermark_words = uxTaskGetStackHighWaterMark(NULL);
  printf(
    "Runtime diag: stage=%s heap=%u min_heap=%u stack_min_free=%u\n",
    stage,
    (unsigned int)esp_get_free_heap_size(),
    (unsigned int)esp_get_minimum_free_heap_size(),
    (unsigned int)(watermark_words * sizeof(StackType_t))
  );
}
#else
static void logRuntimeDiag (const char *stage) {
  (void)stage;
}
#endif

#define ACTIVE_VIEW_DISTANCE VIEW_DISTANCE
#define DEFERRED_MOVEMENT_QUEUE_CAPACITY ((ACTIVE_VIEW_DISTANCE * 2 + 1) * (ACTIVE_VIEW_DISTANCE * 2 + 1) * 2)
#define DEFERRED_MOVEMENT_SEND_INTERVAL_US_SINGLE 80000
#define DEFERRED_MOVEMENT_SEND_INTERVAL_US_MULTI 260000
static uint8_t deferred_movement_active[MAX_PLAYERS];
static uint8_t deferred_movement_head[MAX_PLAYERS];
static uint8_t deferred_movement_tail[MAX_PLAYERS];
static uint8_t deferred_movement_count[MAX_PLAYERS];
static uint16_t deferred_movement_total[MAX_PLAYERS];
static uint16_t deferred_movement_sent[MAX_PLAYERS];
static short deferred_movement_x[MAX_PLAYERS][DEFERRED_MOVEMENT_QUEUE_CAPACITY];
static short deferred_movement_z[MAX_PLAYERS][DEFERRED_MOVEMENT_QUEUE_CAPACITY];
static int64_t deferred_movement_next_send_us[MAX_PLAYERS];
static int64_t deferred_chunk_stream_global_next_send_us;

#ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
#define DEFERRED_INITIAL_SEND_INTERVAL_US_SINGLE 120000
#define DEFERRED_INITIAL_SEND_INTERVAL_US_MULTI 380000
static uint8_t deferred_chunk_active[MAX_PLAYERS];
static uint8_t deferred_chunk_index[MAX_PLAYERS];
static uint16_t deferred_chunk_total[MAX_PLAYERS];
static uint16_t deferred_chunk_sent[MAX_PLAYERS];
static int64_t deferred_chunk_next_send_us[MAX_PLAYERS];

static int64_t deferredInitialSendIntervalUs (void) {
  return client_count > 1 ?
    DEFERRED_INITIAL_SEND_INTERVAL_US_MULTI :
    DEFERRED_INITIAL_SEND_INTERVAL_US_SINGLE;
}
#endif

static int64_t deferredMovementSendIntervalUs (void) {
  return client_count > 1 ?
    DEFERRED_MOVEMENT_SEND_INTERVAL_US_MULTI :
    DEFERRED_MOVEMENT_SEND_INTERVAL_US_SINGLE;
}

static int64_t deferredChunkSendIntervalWithBackoff (int64_t base_interval_us, int64_t send_elapsed_us) {
  int64_t interval = base_interval_us;
  if (send_elapsed_us > interval) interval = send_elapsed_us;
  // 多人连接下大区块写入后额外留一点缓冲
  if (client_count > 1 && send_elapsed_us > 0) interval += send_elapsed_us / 2;
  if (interval < base_interval_us) interval = base_interval_us;
  if (interval > 1000000) interval = 1000000;
  return interval;
}

#ifdef DEV_MINIMAL_PLAY_BOOTSTRAP

static void beginDeferredInitialChunks (int player_index) {
  deferred_chunk_active[player_index] = true;
  deferred_chunk_index[player_index] = 0;
  deferred_chunk_total[player_index] = (uint16_t)((ACTIVE_VIEW_DISTANCE * 2 + 1) * (ACTIVE_VIEW_DISTANCE * 2 + 1) - 1);
  deferred_chunk_sent[player_index] = 0;
  deferred_chunk_next_send_us[player_index] = 0;
  printf(
    "Chunk init queue: need=%u sent=0 queued=%u sending=0\n",
    deferred_chunk_total[player_index],
    deferred_chunk_total[player_index]
  );
}

static void stopDeferredInitialChunks (int player_index) {
  deferred_chunk_active[player_index] = false;
  deferred_chunk_index[player_index] = 0;
  deferred_chunk_total[player_index] = 0;
  deferred_chunk_sent[player_index] = 0;
  deferred_chunk_next_send_us[player_index] = 0;
}

static void resetDeferredMovementChunks (int player_index) {
  deferred_movement_active[player_index] = false;
  deferred_movement_head[player_index] = 0;
  deferred_movement_tail[player_index] = 0;
  deferred_movement_count[player_index] = 0;
  deferred_movement_total[player_index] = 0;
  deferred_movement_sent[player_index] = 0;
  deferred_movement_next_send_us[player_index] = 0;
}

static uint8_t hasDeferredMovementChunk (int player_index, short x, short z) {
  int offset = deferred_movement_head[player_index];
  for (int i = 0; i < deferred_movement_count[player_index]; i ++) {
    if (
      deferred_movement_x[player_index][offset] == x &&
      deferred_movement_z[player_index][offset] == z
    ) return true;
    offset ++;
    if (offset == DEFERRED_MOVEMENT_QUEUE_CAPACITY) offset = 0;
  }
  return false;
}

static void enqueueDeferredMovementChunk (int player_index, short x, short z) {
  if (hasDeferredMovementChunk(player_index, x, z)) return;
  if (deferred_movement_count[player_index] == DEFERRED_MOVEMENT_QUEUE_CAPACITY) {
    deferred_movement_head[player_index] ++;
    if (deferred_movement_head[player_index] == DEFERRED_MOVEMENT_QUEUE_CAPACITY) {
      deferred_movement_head[player_index] = 0;
    }
    deferred_movement_count[player_index] --;
  }
  int tail = deferred_movement_tail[player_index];
  deferred_movement_x[player_index][tail] = x;
  deferred_movement_z[player_index][tail] = z;
  tail ++;
  if (tail == DEFERRED_MOVEMENT_QUEUE_CAPACITY) tail = 0;
  deferred_movement_tail[player_index] = tail;
  deferred_movement_count[player_index] ++;
  deferred_movement_active[player_index] = true;
}

void queueDeferredMovementChunks (int player_index, short center_x, short center_z, short dx, short dz) {
  short dx_input = dx;
  short dz_input = dz;
  #ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
  if (deferred_chunk_active[player_index]) {
    // 初始方形区块流保持原进度
    // 移动时反复重置会饿死队列并留下空洞
    printf(
      "Chunk init queue keep: center=(%d,%d) need=%u sent=%u queued=%u sending=0\n",
      center_x,
      center_z,
      deferred_chunk_total[player_index],
      deferred_chunk_sent[player_index],
      (uint16_t)(deferred_chunk_total[player_index] - deferred_chunk_sent[player_index])
    );
    return;
  }
  #endif

  uint8_t queue_was_empty = deferred_movement_count[player_index] == 0;
  uint8_t queued_before = deferred_movement_count[player_index];
  if (queue_was_empty) {
    deferred_movement_total[player_index] = 0;
    deferred_movement_sent[player_index] = 0;
  }

  short stream_x = center_x - dx;
  short stream_z = center_z - dz;

  while (dx != 0) {
    short step = dx > 0 ? 1 : -1;
    stream_x += step;
    short edge_x = stream_x + step * ACTIVE_VIEW_DISTANCE;
    enqueueDeferredMovementChunk(player_index, edge_x, center_z);
    for (int i = 1; i <= ACTIVE_VIEW_DISTANCE; i ++) {
      enqueueDeferredMovementChunk(player_index, edge_x, center_z - i);
      enqueueDeferredMovementChunk(player_index, edge_x, center_z + i);
    }
    dx -= step;
  }

  while (dz != 0) {
    short step = dz > 0 ? 1 : -1;
    stream_z += step;
    short edge_z = stream_z + step * ACTIVE_VIEW_DISTANCE;
    enqueueDeferredMovementChunk(player_index, center_x, edge_z);
    for (int i = 1; i <= ACTIVE_VIEW_DISTANCE; i ++) {
      enqueueDeferredMovementChunk(player_index, center_x - i, edge_z);
      enqueueDeferredMovementChunk(player_index, center_x + i, edge_z);
    }
    dz -= step;
  }

  uint8_t queued_after = deferred_movement_count[player_index];
  uint8_t queued_added = queued_after >= queued_before ? (queued_after - queued_before) : queued_after;
  deferred_movement_total[player_index] += queued_added;
  printf(
    "Chunk move queue: center=(%d,%d) need=%u sent=%u queued=%u sending=0 add=%u (dx=%d dz=%d)\n",
    center_x,
    center_z,
    deferred_movement_total[player_index],
    deferred_movement_sent[player_index],
    deferred_movement_count[player_index],
    queued_added,
    dx_input,
    dz_input
  );

  deferred_movement_next_send_us[player_index] = 0;
}

static void processDeferredInitialChunks (PlayerData *player, int player_index) {
  if (!deferred_chunk_active[player_index]) return;
  int64_t now = get_program_time();
  if (now < deferred_chunk_next_send_us[player_index]) return;
  if (now < deferred_chunk_stream_global_next_send_us) return;

  short center_x = div_floor(player->x, 16);
  short center_z = div_floor(player->z, 16);
  int total_slots = (ACTIVE_VIEW_DISTANCE * 2 + 1) * (ACTIVE_VIEW_DISTANCE * 2 + 1);

  while (true) {
    if (deferred_chunk_index[player_index] >= total_slots) {
      printf("Spawn stage: deferred final sync\n");
      printf(
        "Chunk init queue done: need=%u sent=%u queued=0 sending=0\n",
        deferred_chunk_total[player_index],
        deferred_chunk_sent[player_index]
      );
      logRuntimeDiag("deferred_final_sync");
      sc_synchronizePlayerPosition(
        player->client_fd,
        (float)player->x + 0.5f,
        player->y,
        (float)player->z + 0.5f,
        player->yaw * 180 / 127,
        player->pitch * 90 / 127
      );
      // 初始区块没发完前继续隔离加载流量
      if (player->flags & 0x20) {
        sc_setHeldItem(player->client_fd, player->hotbar);
        sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
        sc_updateTime(player->client_fd, world_time);
        player->flags &= ~0x20;
        player->flagval_16 = 0;
      }
      stopDeferredInitialChunks(player_index);
      return;
    }

    int raw = deferred_chunk_index[player_index]++;
    int side = ACTIVE_VIEW_DISTANCE * 2 + 1;
    int ox = raw / side - ACTIVE_VIEW_DISTANCE;
    int oz = raw % side - ACTIVE_VIEW_DISTANCE;
    if (ox == 0 && oz == 0) continue;

    printf("Spawn stage: deferred chunk stream\n");
    logRuntimeDiag("deferred_chunk_stream");
    int64_t send_started_us = get_program_time();
    if (sc_chunkDataAndUpdateLight(player->client_fd, center_x + ox, center_z + oz) == -1) {
      printf(
        "Chunk init queue aborted: need=%u sent=%u queued=%u sending=0 cause=send_failed\n",
        deferred_chunk_total[player_index],
        deferred_chunk_sent[player_index],
        (uint16_t)(deferred_chunk_total[player_index] - deferred_chunk_sent[player_index])
      );
      stopDeferredInitialChunks(player_index);
      resetDeferredMovementChunks(player_index);
      disconnectClient(&player->client_fd, -3);
      return;
    }
    int64_t send_elapsed_us = get_program_time() - send_started_us;
    deferred_chunk_sent[player_index] ++;
    printf(
      "Chunk init queue progress: need=%u sent=%u queued=%u sending=1 chunk=(%d,%d)\n",
      deferred_chunk_total[player_index],
      deferred_chunk_sent[player_index],
      (uint16_t)(deferred_chunk_total[player_index] - deferred_chunk_sent[player_index]),
      center_x + ox,
      center_z + oz
    );
    int64_t send_interval_us = deferredChunkSendIntervalWithBackoff(
      deferredInitialSendIntervalUs(), send_elapsed_us
    );
    int64_t next_send_us = get_program_time() + send_interval_us;
    deferred_chunk_next_send_us[player_index] = next_send_us;
    deferred_chunk_stream_global_next_send_us = next_send_us;
    return;
  }
}
#else
static void beginDeferredInitialChunks (int player_index) {
  (void)player_index;
}

static void stopDeferredInitialChunks (int player_index) {
  (void)player_index;
}

static void processDeferredInitialChunks (PlayerData *player, int player_index) {
  (void)player;
  (void)player_index;
}
#endif

void processDeferredMovementChunks (PlayerData *player, int player_index) {
  if (!deferred_movement_active[player_index]) return;
  if (player->client_fd == -1) {
    resetDeferredMovementChunks(player_index);
    return;
  }
  if (deferred_movement_count[player_index] == 0) {
    resetDeferredMovementChunks(player_index);
    return;
  }
  int64_t now = get_program_time();
  if (now < deferred_movement_next_send_us[player_index]) return;
  if (now < deferred_chunk_stream_global_next_send_us) return;

  int head = deferred_movement_head[player_index];
  short x = deferred_movement_x[player_index][head];
  short z = deferred_movement_z[player_index][head];
  head ++;
  if (head == DEFERRED_MOVEMENT_QUEUE_CAPACITY) head = 0;
  deferred_movement_head[player_index] = head;
  deferred_movement_count[player_index] --;
  if (deferred_movement_count[player_index] == 0) deferred_movement_active[player_index] = false;

  int64_t send_started_us = get_program_time();
  if (sc_chunkDataAndUpdateLight(player->client_fd, x, z) == -1) {
    printf(
      "Chunk move queue aborted: need=%u sent=%u queued=%u sending=0 cause=send_failed\n",
      deferred_movement_total[player_index],
      deferred_movement_sent[player_index],
      deferred_movement_count[player_index]
    );
    resetDeferredMovementChunks(player_index);
    disconnectClient(&player->client_fd, -3);
    return;
  }
  int64_t send_elapsed_us = get_program_time() - send_started_us;
  deferred_movement_sent[player_index] ++;
  printf(
    "Chunk move queue progress: need=%u sent=%u queued=%u sending=1 chunk=(%d,%d)\n",
    deferred_movement_total[player_index],
    deferred_movement_sent[player_index],
    deferred_movement_count[player_index],
    x,
    z
  );
  if (deferred_movement_count[player_index] == 0) {
    printf(
      "Chunk move queue done: need=%u sent=%u queued=0 sending=0\n",
      deferred_movement_total[player_index],
      deferred_movement_sent[player_index]
    );
  }
  int64_t send_interval_us = deferredChunkSendIntervalWithBackoff(
    deferredMovementSendIntervalUs(), send_elapsed_us
  );
  int64_t next_send_us = get_program_time() + send_interval_us;
  deferred_movement_next_send_us[player_index] = next_send_us;
  deferred_chunk_stream_global_next_send_us = next_send_us;
}

void processDeferredChunkStreaming (PlayerData *player, int player_index) {
  #ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
  if (deferred_chunk_active[player_index]) {
    processDeferredInitialChunks(player, player_index);
    return;
  }
  #endif

  processDeferredMovementChunks(player, player_index);
}

uint8_t isChunkStreamingActive (int player_index) {
  if (deferred_movement_active[player_index]) return true;
  #ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
  if (deferred_chunk_active[player_index]) return true;
  #endif
  return false;
}

void setClientState (int client_fd, int new_state) {
  int first_match = -1;
  // 更新第一条匹配记录, 并清掉 fd 复用留下的旧项
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    if (first_match == -1) {
      first_match = i;
      client_states[i + 1] = new_state;
    } else {
      client_states[i] = -1;
      client_states[i + 1] = STATE_NONE;
    }
  }
  if (first_match != -1) return;

  // 没有匹配项时占用空槽
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != -1) continue;
    client_states[i] = client_fd;
    client_states[i + 1] = new_state;
    return;
  }
}

int getClientState (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    return client_states[i + 1];
  }
  return STATE_NONE;
}

int getClientIndex (int client_fd) {
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    return i;
  }
  return -1;
}

// 把玩家数据重置到初始状态
void resetPlayerData (PlayerData *player) {
  player->health = 20;
  player->hunger = 20;
  player->saturation = 2500;
  player->x = 8;
  player->z = 8;
  player->y = 80;
  player->flags |= 0x02;
  player->grounded_y = 0;
  for (int i = 0; i < 41; i ++) {
    player->inventory_items[i] = 0;
    player->inventory_count[i] = 0;
  }
  for (int i = 0; i < 9; i ++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;
}

// 给 player_data 分配一条记录
int reservePlayerData (int client_fd, uint8_t *uuid, char *name) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    // 找到已有玩家记录
    if (memcmp(player_data[i].uuid, uuid, 16) == 0) {
      // 写入 fd 和用户名
      player_data[i].client_fd = client_fd;
      memcpy(player_data[i].name, name, 16);
      // 标记为加载中
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      // 重置最近访问区块
      for (int j = 0; j < VISITED_HISTORY; j ++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      resetDeferredMovementChunks(i);
      return 0;
    }
    // 查找未分配的玩家槽
    uint8_t empty = true;
    for (uint8_t j = 0; j < 16; j ++) {
      if (player_data[i].uuid[j] != 0) {
        empty = false;
        break;
      }
    }
    // 找到空槽后初始化默认数据
    if (empty) {
      if (player_data_count >= MAX_PLAYERS) return 1;
      player_data[i].client_fd = client_fd;
      player_data[i].flags |= 0x20;
      player_data[i].flagval_16 = 0;
      memcpy(player_data[i].uuid, uuid, 16);
      memcpy(player_data[i].name, name, 16);
      resetPlayerData(&player_data[i]);
      for (int j = 0; j < VISITED_HISTORY; j ++) {
        player_data[i].visited_x[j] = 32767;
        player_data[i].visited_z[j] = 32767;
      }
      resetDeferredMovementChunks(i);
      player_data_count ++;
      return 0;
    }
  }

  return 1;

}

int getPlayerData (int client_fd, PlayerData **output) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == client_fd) {
      *output = &player_data[i];
      return 0;
    }
  }
  return 1;
}

// 按名字查找玩家, 找不到返回 NULL
PlayerData *getPlayerByName (int start_offset, int end_offset, uint8_t *buffer) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    int j;
    for (j = start_offset; j < end_offset && j < 256 && buffer[j] != ' '; j++) {
      if (player_data[i].name[j - start_offset] != buffer[j]) break;
    }
    if ((j == end_offset || buffer[j] == ' ') && j < 256) {
      return &player_data[i];
    }
  }
  return NULL;
}


// 标记客户端断开并清理玩家数据
void handlePlayerDisconnect (int client_fd) {
  // 在玩家表里找对应记录
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd != client_fd) continue;
    stopDeferredInitialChunks(i);
    resetDeferredMovementChunks(i);
    // 标记玩家离线
    player_data[i].client_fd = -1;
    // 组装离开提示
    uint8_t player_name_len = strlen(player_data[i].name);
    strcpy((char *)recv_buffer, player_data[i].name);
    strcpy((char *)recv_buffer + player_name_len, " left the game");
    // 向其他在线客户端广播离开消息
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      int target_fd = player_data[j].client_fd;
      if (target_fd == -1) continue;
      if (target_fd == client_fd) continue;
      if (player_data[j].flags & 0x20) continue;
      // 发送系统消息
      sc_systemChat(target_fd, (char *)recv_buffer, 14 + player_name_len);
      if (player_data[j].client_fd != target_fd) continue;
      // 移除离开的玩家实体
      sc_removeEntity(target_fd, client_fd);
    }
    break;
  }
  // 清掉这个 fd 对应的所有状态项
  for (int i = 0; i < MAX_PLAYERS * 2; i += 2) {
    if (client_states[i] != client_fd) continue;
    client_states[i] = -1;
    client_states[i + 1] = STATE_NONE;
  }
}

// 标记客户端已加入并向其他玩家广播
void handlePlayerJoin (PlayerData* player) {
#ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (&player_data[i] != player) continue;
    beginDeferredInitialChunks(i);
    break;
  }
#endif

  // 组装加入提示
  uint8_t player_name_len = strlen(player->name);
  strcpy((char *)recv_buffer, player->name);
  strcpy((char *)recv_buffer + player_name_len, " joined the game");

  // 向其他客户端和自己同步玩家信息与实体
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int target_fd = player_data[i].client_fd;
    if (target_fd == -1) continue;

    sc_systemChat(target_fd, (char *)recv_buffer, 16 + player_name_len);
    if (player_data[i].client_fd != target_fd) continue;
    sc_playerInfoUpdateAddPlayer(target_fd, *player);
    if (player_data[i].client_fd != target_fd) continue;
    if (target_fd != player->client_fd) {
      if (player->client_fd == -1) break;
      sc_spawnEntityPlayer(target_fd, *player);
    }
  }

  // 清掉客户端加载标记和超时计时器
  #ifndef DEV_MINIMAL_PLAY_BOOTSTRAP
  player->flags &= ~0x20;
  #endif
  player->flagval_16 = 0;

}

static const char *disconnectCauseString (int cause) {
  switch (cause) {
    case -3: return "network send failed";
    case -2: return "network send timeout";
    case -1: return "network recv timeout or read failure";
    case 1: return "peer closed connection or peek/read failed";
    case 2: return "invalid packet length (VarInt parse failed)";
    case 3: return "invalid packet id (VarInt parse failed)";
    case 4: return "packet handler read failed";
    case 5: return "legacy status ping";
    case 6: return "BEEF dump complete";
    case 7: return "FEED upload complete";
    case 8: return "status ping complete";
    default: return "unknown";
  }
}
static const char *stateString (int state) {
  switch (state) {
    case STATE_NONE: return "none";
    case STATE_STATUS: return "status";
    case STATE_LOGIN: return "login";
    case STATE_TRANSFER: return "transfer";
    case STATE_CONFIGURATION: return "configuration";
    case STATE_PLAY: return "play";
    default: return "unknown";
  }
}

void disconnectClient (int *client_fd, int cause) {
  if (*client_fd == -1) return;

  int fd = *client_fd;
  int state = getClientState(fd);
  int close_result = 0;
  uint8_t duplicate_close = false;
  int os_errno = 0;

  #ifdef ARDUINO_PLATFORM
  // fd 已关闭且没有协议状态时
  // 说明这是 fd 复用后的重复断开
  if (!arduino_fd_in_use(fd) && state == STATE_NONE) {
    *client_fd = -1;
    return;
  }
  close(fd);
  #elif defined(_WIN32)
  close_result = closesocket(fd);
  os_errno = close_result == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (close_result == SOCKET_ERROR && os_errno == WSAENOTSOCK) duplicate_close = true;
  #else
  close_result = close(fd);
  os_errno = close_result < 0 ? errno : 0;
  if (close_result < 0 && os_errno == EBADF) duplicate_close = true;
  #endif

  if (duplicate_close) {
    *client_fd = -1;
    return;
  }

  if (client_count > 0) client_count --;
  setClientState(fd, STATE_NONE);
  handlePlayerDisconnect(fd);

  printf(
    "Disconnected client %d, cause: %d (%s), os_errno: %d\n"
    "  state: %d (%s), last_packet: len=%d id=%d payload=%d\n"
    "  last_out: stage=%s len=%d id=%d\n"
    "  io: expected=%d progress=%d wait_us=%lld require_first=%d last_result=%d last_errno=%d\n\n",
    fd,
    cause,
    disconnectCauseString(cause),
    os_errno,
    state,
    stateString(state),
    debug_last_packet_length,
    debug_last_packet_id,
    debug_last_payload_length,
    debug_last_out_stage,
    debug_last_out_packet_length,
    debug_last_out_packet_id,
    debug_io_expected,
    debug_io_progress,
    (long long)debug_io_wait_us,
    debug_io_require_first,
    debug_io_last_result,
    debug_io_last_errno
  );

  *client_fd = -1;
}

uint8_t serverSlotToClientSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // 玩家背包

    if (slot < 9) return slot + 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 40) return 45;
    if (slot >= 36 && slot <= 39) return 44 - slot;
    if (slot >= 41 && slot <= 44) return slot - 40;

  } else if (window_id == 12) { // 合成台

    if (slot >= 41 && slot <= 49) return slot - 40;
    return serverSlotToClientSlot(0, slot - 1);

  } else if (window_id == 14) { // 熔炉

    if (slot >= 41 && slot <= 43) return slot - 41;
    return serverSlotToClientSlot(0, slot + 6);

  }

  return 255;
}

uint8_t clientSlotToServerSlot (int window_id, uint8_t slot) {

  if (window_id == 0) { // 玩家背包

    if (slot >= 36 && slot <= 44) return slot - 36;
    if (slot >= 9 && slot <= 35) return slot;
    if (slot == 45) return 40;
    if (slot >= 5 && slot <= 8) return 44 - slot;

    // 把背包合成槽映射到 player data 的合成格
    // 这里利用了两段缓冲在内存里相邻
    if (slot == 1) return 41;
    if (slot == 2) return 42;
    if (slot == 3) return 44;
    if (slot == 4) return 45;

  } else if (window_id == 12) { // 合成台

    // 同样沿用上面的合成偏移映射
    if (slot >= 1 && slot <= 9) return 40 + slot;
    // 其余槽位仅整体偏移一格
    if (slot >= 10 && slot <= 45) return clientSlotToServerSlot(0, slot - 1);

  } else if (window_id == 14) { // 熔炉

    // 把熔炉槽临时映射到玩家合成格
    // 这样窗口关闭后能再退回背包
    if (slot <= 2) return 41 + slot;
    // 其余槽位仅整体偏移 6 格
    if (slot >= 3 && slot <= 38) return clientSlotToServerSlot(0, slot + 6);

  }
  #ifdef ALLOW_CHESTS
  else if (window_id == 2) { // chest

    // 把 chest 槽位溢出映射到合成格
    // 这不是标准做法, 依赖上层单独处理
    if (slot <= 26) return 41 + slot;
    // 其余槽位仅整体偏移 18 格
    if (slot >= 27 && slot <= 62) return clientSlotToServerSlot(0, slot - 18);

  }
  #endif

  return 255;
}

int givePlayerItem (PlayerData *player, uint16_t item, uint8_t count) {

  if (item == 0 || count == 0) return 0;

  uint8_t slot = 255;
  uint8_t stack_size = getItemStackSize(item);

  for (int i = 0; i < 41; i ++) {
    if (player->inventory_items[i] == item && player->inventory_count[i] <= stack_size - count) {
      slot = i;
      break;
    }
  }

  if (slot == 255) {
    for (int i = 0; i < 41; i ++) {
      if (player->inventory_count[i] == 0) {
        slot = i;
        break;
      }
    }
  }

  // 槽位落在主背包外时分配失败
  if (slot >= 36) return 1;

  player->inventory_items[slot] = item;
  player->inventory_count[slot] += count;
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, slot), player->inventory_count[slot], item);

  return 0;

}

// 向客户端发送完整的玩家出生流程
void spawnPlayer (PlayerData *player) {
  printf("Spawn stage: begin\n");
  logRuntimeDiag("begin");

  // 玩家出生坐标先用占位值
  float spawn_x = 8.5f, spawn_y = 80.0f, spawn_z = 8.5f;
  float spawn_yaw = 0.0f, spawn_pitch = 0.0f;

  if (player->flags & 0x02) { // 新玩家
    // 按地形高度计算出生 Y
    spawn_y = getHeightAt(8, 8) + 1;
    player->y = spawn_y;
    player->flags &= ~0x02;
  } else { // 老玩家
    // 从玩家数据恢复出生坐标
    spawn_x = (float)player->x + 0.5;
    spawn_y = player->y;
    spawn_z = (float)player->z + 0.5;
    spawn_yaw = player->yaw * 180 / 127;
    spawn_pitch = player->pitch * 90 / 127;
  }

  // 第一次同步出生坐标
  printf("Spawn stage: first position sync\n");
  logRuntimeDiag("first_position_sync");
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

  task_yield(); // 包之间检查任务时间片

  // 清空合成残留并解锁 craft_items
  for (int i = 0; i < 9; i++) {
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
  }
  player->flags &= ~0x80;

  #ifndef DEV_MINIMAL_PLAY_BOOTSTRAP
  // 同步客户端背包和快捷栏
  printf("Spawn stage: inventory sync\n");
  logRuntimeDiag("inventory_sync");
  for (uint8_t i = 0; i < 41; i ++) {
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, i), player->inventory_count[i], player->inventory_items[i]);
  }
  sc_setHeldItem(player->client_fd, player->hotbar);
  // 同步生命和饥饿
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  // 同步时间
  sc_updateTime(player->client_fd, world_time);

  #ifdef ENABLE_PLAYER_FLIGHT
  if (GAMEMODE != 1 && GAMEMODE != 3) {
    // 给玩家开启飞行
    sc_playerAbilities(player->client_fd, 0x04);
  }
  #endif
  #else
  printf("Spawn stage: minimal bootstrap active\n");
  logRuntimeDiag("minimal_bootstrap");
  #endif

  // 计算玩家所在区块
  short _x = div_floor(player->x, 16), _z = div_floor(player->z, 16);

  // 先发区块流前导包
  printf("Spawn stage: chunk preamble\n");
  logRuntimeDiag("chunk_preamble");
  sc_setDefaultSpawnPosition(player->client_fd, 8, 80, 8, spawn_yaw, spawn_pitch);
  sc_startWaitingForChunks(player->client_fd);
  sc_setCenterChunk(player->client_fd, _x, _z);

  task_yield(); // 包之间检查任务时间片

  // 先发送出生区块
  printf("Spawn stage: chunk stream\n");
  logRuntimeDiag("chunk_stream");
  if (sc_chunkDataAndUpdateLight(player->client_fd, _x, _z) == -1) {
    printf("Spawn stage: chunk stream failed\n");
    logRuntimeDiag("chunk_stream_failed");
    disconnectClient(&player->client_fd, -3);
    return;
  }
#ifndef DEV_MINIMAL_PLAY_BOOTSTRAP
  for (int i = -ACTIVE_VIEW_DISTANCE; i <= ACTIVE_VIEW_DISTANCE; i ++) {
    for (int j = -ACTIVE_VIEW_DISTANCE; j <= ACTIVE_VIEW_DISTANCE; j ++) {
      if (i == 0 && j == 0) continue;
      if (sc_chunkDataAndUpdateLight(player->client_fd, _x + i, _z + j) == -1) {
        printf("Spawn stage: chunk stream failed\n");
        logRuntimeDiag("chunk_stream_failed");
        disconnectClient(&player->client_fd, -3);
        return;
      }
    }
  }
#endif
  // 区块发完后再同步一次坐标
  printf("Spawn stage: final position sync\n");
  logRuntimeDiag("final_position_sync");
  sc_synchronizePlayerPosition(player->client_fd, spawn_x, spawn_y, spawn_z, spawn_yaw, spawn_pitch);

  task_yield(); // 包之间检查任务时间片

  printf("Spawn stage: complete\n");
  logRuntimeDiag("complete");
}

// 向其他玩家广播玩家实体元数据
void broadcastPlayerMetadata (PlayerData *player) {
  uint8_t sneaking = (player->flags & 0x04) != 0;
  uint8_t sprinting = (player->flags & 0x08) != 0;

  uint8_t entity_bit_mask = 0;
  if (sneaking) entity_bit_mask |= 0x02;
  if (sprinting) entity_bit_mask |= 0x08;

  EntityData metadata[] = {
    {
      0,                   // 索引: 实体位掩码
      0,                   // 类型: Byte
      { entity_bit_mask }, // 值
    }
  };

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];
    int client_fd = other_player->client_fd;

    if (client_fd == -1) continue;
    if (client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;

    sc_setEntityMetadata(client_fd, player->client_fd, metadata, 1);
  }
}

// 给指定玩家发送 Mob 实体元数据
// client_fd 为 -1 时广播给所有玩家
void broadcastMobMetadata (int client_fd, int entity_id) {

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  EntityData *metadata;
  size_t length;

  switch (mob->type) {
    case 111: // 羊
      if (!((mob->data >> 5) & 1)) // 没剪毛就不发元数据
        return;

      metadata = malloc(sizeof *metadata);
      metadata[0] = (EntityData){
        17,                // 索引: 羊位掩码
        0,                 // 类型: Byte
        { (uint8_t)0x10 }, // 值
      };
      length = 1;

      break;

    default: return;
  }

  if (client_fd == -1) {
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      PlayerData* player = &player_data[i];
      client_fd = player->client_fd;

      if (client_fd == -1) continue;
      if (player->flags & 0x20) continue;

      sc_setEntityMetadata(client_fd, entity_id, metadata, length);
    }
  } else {
    sc_setEntityMetadata(client_fd, entity_id, metadata, length);
  }

  free(metadata);
}

uint8_t getBlockChange (short x, uint8_t y, short z) {
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) continue;
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z
    ) return block_changes[i].block;
    #ifdef ALLOW_CHESTS
      // 跳过 chest 内容区
      if (block_changes[i].block == B_chest) i += 14;
    #endif
  }
  return 0xFF;
}

// 处理 block change 空间耗尽
void failBlockChange (short x, uint8_t y, short z, uint8_t block) {

  // 取这个位置原来的方块
  uint8_t before = getBlockAt(x, y, z);

  // 给所有玩家回滚这个方块
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int target_fd = player_data[i].client_fd;
    if (target_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    // 把客户端方块重置回去
    sc_blockUpdate(target_fd, x, y, z, before);
    if (player_data[i].client_fd != target_fd) continue;
    // 发送容量超限提示
    sc_systemChat(target_fd, "Block changes limit exceeded. Restore original terrain to continue.", 67);
  }

}

uint8_t makeBlockChange (short x, uint8_t y, short z, uint8_t block) {

  // 向所有在线客户端发送方块更新
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    sc_blockUpdate(player_data[i].client_fd, x, y, z, block);
  }

  // 计算这里的原始地形方块
  // 与原地形一致的改动不需要存入 block change
  ChunkAnchor anchor = {
    x / CHUNK_SIZE,
    z / CHUNK_SIZE
  };
  if (x % CHUNK_SIZE < 0) anchor.x --;
  if (z % CHUNK_SIZE < 0) anchor.z --;
  anchor.hash = getChunkHash(anchor.x, anchor.z);
  anchor.biome = getChunkBiome(anchor.x, anchor.z);

  uint8_t is_base_block = block == getTerrainAt(x, y, z, anchor);

  // block_changes 里 0xFF 表示空洞或已恢复项
  // 先记住第一处空洞, 供后续新增改动使用
  int first_gap = block_changes_count;

  // 优先覆盖同坐标旧项
  // 避免同一坐标出现冲突记录
  for (int i = 0; i < block_changes_count; i ++) {
    if (block_changes[i].block == 0xFF) {
      if (first_gap == block_changes_count) first_gap = i;
      continue;
    }
    if (
      block_changes[i].x == x &&
      block_changes[i].y == y &&
      block_changes[i].z == z
    ) {
      #ifdef ALLOW_CHESTS
      // 覆盖 chest 时连后面 14 条物品数据一起清掉
      if (block_changes[i].block == B_chest) {
        for (int j = 1; j < 15; j ++) block_changes[i + j].block = 0xFF;
      }
      #endif
      if (is_base_block) block_changes[i].block = 0xFF;
      else {
        #ifdef ALLOW_CHESTS
        // 放 chest 时先把当前目标项腾空
        // 再走下面的 chest 专用分配流程
        if (block == B_chest) {
          block_changes[i].block = 0xFF;
          if (first_gap > i) first_gap = i;
          #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
          writeBlockChangesToDisk(i, i);
          #endif
          break;
        }
        #endif
        block_changes[i].block = block;
      }
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(i, i);
      #endif
      return 0;
    }
  }

  // 与原地形一致时不新建改动项
  if (is_base_block) return 0;

  #ifdef ALLOW_CHESTS
  if (block == B_chest) {
    // chest 一共要占 15 条 entry
    // 所以必须找一段连续空洞
    // 找不到时就自然追加到尾部
    int last_real_entry = first_gap - 1;
    for (int i = first_gap; i <= block_changes_count + 15; i ++) {
      if (i >= MAX_BLOCK_CHANGES) break; // 空间耗尽, 走 failBlockChange

      if (block_changes[i].block != 0xFF) {
        last_real_entry = i;
        continue;
      }
      if (i - last_real_entry != 15) continue;
      // 找到足够宽的空洞后写入 chest
      block_changes[last_real_entry + 1].x = x;
      block_changes[last_real_entry + 1].y = y;
      block_changes[last_real_entry + 1].z = z;
      block_changes[last_real_entry + 1].block = block;
      // 后面 14 条清零, 留给物品数据
      for (int i = 2; i <= 15; i ++) {
        block_changes[last_real_entry + i].x = 0;
        block_changes[last_real_entry + i].y = 0;
        block_changes[last_real_entry + i].z = 0;
        block_changes[last_real_entry + i].block = 0;
      }
      // 必要时扩展有效搜索范围
      if (i >= block_changes_count) {
        block_changes_count = i + 1;
      }
      // 需要时写盘
      #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
      writeBlockChangesToDisk(last_real_entry + 1, last_real_entry + 15);
      #endif
      return 0;
    }
    // 走到这里说明 chest 没写进去
    failBlockChange(x, y, z, block);
    return 1;
  }
  #endif

  // 处理普通改动项空间耗尽
  if (first_gap == MAX_BLOCK_CHANGES) {
    failBlockChange(x, y, z, block);
    return 1;
  }

  // 否则写进第一处可用空洞
  block_changes[first_gap].x = x;
  block_changes[first_gap].y = y;
  block_changes[first_gap].z = z;
  block_changes[first_gap].block = block;
  // 需要时写盘
  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeBlockChangesToDisk(first_gap, first_gap);
  #endif
  // 如果追加到尾部就扩展有效范围
  if (first_gap == block_changes_count) {
    block_changes_count ++;
  }

  return 0;
}

// 根据方块和工具计算挖掘掉落
// 概率值按 N = floor(P * (2 ^ 32)) 预先换算
uint16_t getMiningResult (uint16_t held_item, uint8_t block) {

  switch (block) {

    case B_dandelion: return I_dandelion;
    case B_poppy: return I_poppy;

    case B_oak_leaves:
      if (held_item == I_shears) return I_oak_leaves;
      uint32_t r = fast_rand();
      if (r < 21474836) return I_apple; // 0.5%
      if (r < 85899345) return I_stick; // 2%
      if (r < 214748364) return I_oak_sapling; // 5%
      return 0;
      break;

    case B_stone:
    case B_cobblestone:
    case B_stone_slab:
    case B_cobblestone_slab:
    case B_sandstone:
    case B_furnace:
    case B_coal_ore:
    case B_iron_ore:
    case B_iron_block:
    case B_gold_block:
    case B_diamond_block:
    case B_redstone_block:
    case B_coal_block:
      // 检查玩家是否拿着任意镐子
      if (
        held_item != I_wooden_pickaxe &&
        held_item != I_stone_pickaxe &&
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_gold_ore:
    case B_redstone_ore:
    case B_diamond_ore:
      // 检查玩家是否拿着铁镐或更高
      if (
        held_item != I_iron_pickaxe &&
        held_item != I_golden_pickaxe &&
        held_item != I_diamond_pickaxe &&
        held_item != I_netherite_pickaxe
      ) return 0;
      break;

    case B_snow:
      // 检查玩家是否拿着任意铲子
      if (
        held_item != I_wooden_shovel &&
        held_item != I_stone_shovel &&
        held_item != I_iron_shovel &&
        held_item != I_golden_shovel &&
        held_item != I_diamond_shovel &&
        held_item != I_netherite_shovel
      ) return 0;
      break;

    default: break;
  }

  return B_to_I[block];

}

// 掷一次随机数判断工具是否损坏
void bumpToolDurability (PlayerData *player) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  // 这里不存耐久值
  // 改用按原版耐久加权的随机损坏
  uint32_t r = fast_rand();
  if (
    ((held_item == I_wooden_pickaxe || held_item == I_wooden_axe || held_item == I_wooden_shovel) && r < 72796055) ||
    ((held_item == I_stone_pickaxe || held_item == I_stone_axe || held_item == I_stone_shovel) && r < 32786009) ||
    ((held_item == I_iron_pickaxe || held_item == I_iron_axe || held_item == I_iron_shovel) && r < 17179869) ||
    ((held_item == I_golden_pickaxe || held_item == I_golden_axe || held_item == I_golden_shovel) && r < 134217728) ||
    ((held_item == I_diamond_pickaxe || held_item == I_diamond_axe || held_item == I_diamond_shovel) && r < 2751420) ||
    ((held_item == I_netherite_pickaxe || held_item == I_netherite_axe || held_item == I_netherite_shovel) && r < 2114705) ||
    (held_item == I_shears && r < 18046081)
  ) {
    player->inventory_items[player->hotbar] = 0;
    player->inventory_count[player->hotbar] = 0;
    sc_entityEvent(player->client_fd, player->client_fd, 47);
    sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), 0, 0);
  }

}

// 检查当前工具是否能瞬间挖掉这个方块
uint8_t isInstantlyMined (PlayerData *player, uint8_t block) {

  uint16_t held_item = player->inventory_items[player->hotbar];

  if (
    block == B_snow ||
    block == B_snow_block
  ) return (
    held_item == I_stone_shovel ||
    held_item == I_iron_shovel ||
    held_item == I_diamond_shovel ||
    held_item == I_netherite_shovel ||
    held_item == I_golden_shovel
  );

  if (block == B_oak_leaves)
    return held_item == I_shears;

  return (
    block == B_dead_bush ||
    block == B_short_grass ||
    block == B_dandelion ||
    block == B_poppy ||
    block == B_torchflower ||
    block == B_golden_dandelion ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_fern ||
    block == B_bush ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_torch ||
    block == B_lily_pad ||
    block == B_oak_sapling
  );

}

// 检查这个方块是否必须有下方支撑
uint8_t isColumnBlock (uint8_t block) {
  return (
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_cactus ||
    block == B_short_grass ||
    block == B_dandelion ||
    block == B_poppy ||
    block == B_torchflower ||
    block == B_golden_dandelion ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_fern ||
    block == B_bush ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_dead_bush ||
    block == B_sand ||
    block == B_torch ||
    block == B_oak_sapling
  );
}

// 检查这个方块是否可穿过
uint8_t isPassableBlock (uint8_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_snow ||
    block == B_moss_carpet ||
    block == B_short_grass ||
    block == B_dandelion ||
    block == B_poppy ||
    block == B_torchflower ||
    block == B_golden_dandelion ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_fern ||
    block == B_bush ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_dead_bush ||
    block == B_oak_sapling ||
    block == B_torch
  );
}
// 检查这个方块是否可生成实体
uint8_t isPassableSpawnBlock (uint8_t block) {
    if ((block >= B_water && block < B_water + 8) ||
        (block >= B_lava && block < B_lava + 4))
    {
        return 0;
    }
    return isPassableBlock(block);
}

// 检查这个方块是否可被替换
uint8_t isReplaceableBlock (uint8_t block) {
  return (
    block == B_air ||
    (block >= B_water && block < B_water + 8) ||
    (block >= B_lava && block < B_lava + 4) ||
    block == B_short_grass ||
    block == B_dandelion ||
    block == B_poppy ||
    block == B_torchflower ||
    block == B_golden_dandelion ||
    block == B_blue_orchid ||
    block == B_allium ||
    block == B_azure_bluet ||
    block == B_red_tulip ||
    block == B_orange_tulip ||
    block == B_white_tulip ||
    block == B_pink_tulip ||
    block == B_oxeye_daisy ||
    block == B_cornflower ||
    block == B_wither_rose ||
    block == B_lily_of_the_valley ||
    block == B_fern ||
    block == B_bush ||
    block == B_short_dry_grass ||
    block == B_tall_dry_grass ||
    block == B_dead_bush ||
    block == B_oak_sapling ||
    block == B_snow
  );
}

uint8_t isReplaceableFluid (uint8_t block, uint8_t level, uint8_t fluid) {
  if (block >= fluid && block - fluid < 8) {
    return block - fluid > level;
  }
  return isReplaceableBlock(block);
}

// 检查这个物品能否放进 composter
// 返回产出骨粉的 2^32 分之一概率值
uint32_t isCompostItem (uint16_t item) {

  // 输出值按下式预计算:
  // P = 2^32 / (7 / compost_chance)

  if ( // 堆肥概率: 30%
    item == I_oak_leaves ||
    item == I_short_grass ||
    item == I_wheat_seeds ||
    item == I_oak_sapling ||
    item == I_moss_carpet
  ) return 184070026;

  if ( // 堆肥概率: 50%
    item == I_cactus ||
    item == I_sugar_cane
  ) return 306783378;

  if ( // 堆肥概率: 65%
    item == I_apple ||
    item == I_lily_pad
  ) return 398818392;

  return 0;
}

// 返回物品最大堆叠数
uint8_t getItemStackSize (uint16_t item) {

  if (
    // 镐子
    item == I_wooden_pickaxe ||
    item == I_stone_pickaxe ||
    item == I_iron_pickaxe ||
    item == I_golden_pickaxe ||
    item == I_diamond_pickaxe ||
    item == I_netherite_pickaxe ||
    // 斧子
    item == I_wooden_axe ||
    item == I_stone_axe ||
    item == I_iron_axe ||
    item == I_golden_axe ||
    item == I_diamond_axe ||
    item == I_netherite_axe ||
    // 铲子
    item == I_wooden_shovel ||
    item == I_stone_shovel ||
    item == I_iron_shovel ||
    item == I_golden_shovel ||
    item == I_diamond_shovel ||
    item == I_netherite_shovel ||
    // 剑
    item == I_wooden_sword ||
    item == I_stone_sword ||
    item == I_iron_sword ||
    item == I_golden_sword ||
    item == I_diamond_sword ||
    item == I_netherite_sword ||
    // 锄头
    item == I_wooden_hoe ||
    item == I_stone_hoe ||
    item == I_iron_hoe ||
    item == I_golden_hoe ||
    item == I_diamond_hoe ||
    item == I_netherite_hoe ||
    // 剪刀
    item == I_shears
  ) return 1;

  if (
    item == I_snowball
  ) return 16;

  return 64;
}

// 返回护甲片的防御点
// 不是护甲时返回 0
uint8_t getItemDefensePoints (uint16_t item) {

  switch (item) {
    case I_leather_helmet: return 1;
    case I_golden_helmet: return 2;
    case I_iron_helmet: return 2;
    case I_diamond_helmet: // 与下界合金相同
    case I_netherite_helmet: return 3;
    case I_leather_chestplate: return 3;
    case I_golden_chestplate: return 5;
    case I_iron_chestplate: return 6;
    case I_diamond_chestplate: // 与下界合金相同
    case I_netherite_chestplate: return 8;
    case I_leather_leggings: return 2;
    case I_golden_leggings: return 3;
    case I_iron_leggings: return 5;
    case I_diamond_leggings: // 与下界合金相同
    case I_netherite_leggings: return 6;
    case I_leather_boots: return 1;
    case I_golden_boots: return 1;
    case I_iron_boots: return 2;
    case I_diamond_boots: // 与下界合金相同
    case I_netherite_boots: return 3;
    default: break;
  }

  return 0;
}

// 计算玩家当前护甲总防御点
uint8_t getPlayerDefensePoints (PlayerData *player) {
  return (
    // 头盔
    getItemDefensePoints(player->inventory_items[39]) +
    // 胸甲
    getItemDefensePoints(player->inventory_items[38]) +
    // 护腿
    getItemDefensePoints(player->inventory_items[37]) +
    // 靴子
    getItemDefensePoints(player->inventory_items[36])
  );
}

// 返回护甲对应的服务器槽位
// 不是护甲时返回 255
uint8_t getArmorItemSlot (uint16_t item) {

    switch (item) {
    case I_leather_helmet:
    case I_golden_helmet:
    case I_iron_helmet:
    case I_diamond_helmet:
    case I_netherite_helmet:
      return 39;
    case I_leather_chestplate:
    case I_golden_chestplate:
    case I_iron_chestplate:
    case I_diamond_chestplate:
    case I_netherite_chestplate:
      return 38;
    case I_leather_leggings:
    case I_golden_leggings:
    case I_iron_leggings:
    case I_diamond_leggings:
    case I_netherite_leggings:
      return 37;
    case I_leather_boots:
    case I_golden_boots:
    case I_iron_boots:
    case I_diamond_boots:
    case I_netherite_boots:
      return 36;
    default: break;
  }

  return 255;
}

// 处理玩家食用当前手持物品
// 返回这次是否成功吃掉物品
// just_check 为 true 时只检查不消耗
uint8_t handlePlayerEating (PlayerData *player, uint8_t just_check) {

  // 不能吃时直接返回
  if (player->hunger >= 20) return false;

  uint16_t *held_item = &player->inventory_items[player->hotbar];
  uint8_t *held_count = &player->inventory_count[player->hotbar];

  // 手上没东西时直接返回
  if (*held_item == 0 || *held_count == 0) return false;

  uint8_t food = 0;
  uint16_t saturation = 0;

  // 这里的饱和度与原版大约按 1:500 映射
  switch (*held_item) {
    case I_chicken: food = 2; saturation = 600; break;
    case I_beef: food = 3; saturation = 900; break;
    case I_porkchop: food = 3; saturation = 300; break;
    case I_mutton: food = 2; saturation = 600; break;
    case I_cooked_chicken: food = 6; saturation = 3600; break;
    case I_cooked_beef: food = 8; saturation = 6400; break;
    case I_cooked_porkchop: food = 8; saturation = 6400; break;
    case I_cooked_mutton: food = 6; saturation = 4800; break;
    case I_rotten_flesh: food = 4; saturation = 0; break;
    case I_apple: food = 4; saturation = 1200; break;
    default: break;
  }

  // 只检查时不改状态
  if (just_check) return food != 0;

  // 增加饱和度和饥饿值
  player->saturation += saturation;
  player->hunger += food;
  if (player->hunger > 20) player->hunger = 20;

  // 消耗手持物品
  *held_count -= 1;
  if (*held_count == 0) *held_item = 0;

  // 把变化同步给客户端
  sc_entityEvent(player->client_fd, player->client_fd, 9);
  sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  sc_setContainerSlot(
    player->client_fd, 0,
    serverSlotToClientSlot(0, player->hotbar),
    *held_count, *held_item
  );

  return true;
}

void handleFluidMovement (short x, uint8_t y, short z, uint8_t fluid, uint8_t block) {

  // 取流体等级 0-7
  // 这里等级越高表示流得越远
  uint8_t level = block - fluid;

  // 读取四周相邻流体块
  uint8_t adjacent[4] = {
    getBlockAt(x + 1, y, z),
    getBlockAt(x - 1, y, z),
    getBlockAt(x, y, z + 1),
    getBlockAt(x, y, z - 1)
  };

  // 维持与流体源头的连通
  if (level != 0) {
    // 检查是否连到低一级的同类流体
    uint8_t connected = false;
    for (int i = 0; i < 4; i ++) {
      if (adjacent[i] == block - 1) {
        connected = true;
        break;
      }
    }
    // 断开后清掉当前方块并重算周围流动
    if (!connected) {
      makeBlockChange(x, y, z, B_air);
      checkFluidUpdate(x + 1, y, z, adjacent[0]);
      checkFluidUpdate(x - 1, y, z, adjacent[1]);
      checkFluidUpdate(x, y, z + 1, adjacent[2]);
      checkFluidUpdate(x, y, z - 1, adjacent[3]);
      return;
    }
  }

  // 先判断是否向下流, 优先级高于侧向扩散
  uint8_t block_below = getBlockAt(x, y - 1, z);
  if (isReplaceableBlock(block_below)) {
    makeBlockChange(x, y - 1, z, fluid);
    return handleFluidMovement(x, y - 1, z, fluid, fluid);
  }

  // 到最大等级后停止侧向扩散
  if (level == 3 && fluid == B_lava) return;
  if (level == 7) return;

  // 处理侧向流动, 等级加 1
  if (isReplaceableFluid(adjacent[0], level, fluid)) {
    makeBlockChange(x + 1, y, z, block + 1);
    handleFluidMovement(x + 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[1], level, fluid)) {
    makeBlockChange(x - 1, y, z, block + 1);
    handleFluidMovement(x - 1, y, z, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[2], level, fluid)) {
    makeBlockChange(x, y, z + 1, block + 1);
    handleFluidMovement(x, y, z + 1, fluid, block + 1);
  }
  if (isReplaceableFluid(adjacent[3], level, fluid)) {
    makeBlockChange(x, y, z - 1, block + 1);
    handleFluidMovement(x, y, z - 1, fluid, block + 1);
  }

}

void checkFluidUpdate (short x, uint8_t y, short z, uint8_t block) {

  uint8_t fluid;
  if (block >= B_water && block < B_water + 8) fluid = B_water;
  else if (block >= B_lava && block < B_lava + 4) fluid = B_lava;
  else return;

  handleFluidMovement(x, y, z, fluid, block);

}

#ifdef ENABLE_PICKUP_ANIMATION
// 在指定坐标播放物品拾取动画
void playPickupAnimation (PlayerData *player, uint16_t item, double x, double y, double z) {
  // 26.1.2 下为稳定性禁用
  // 物品仍直接通过 givePlayerItem() 发放
  (void)player;
  (void)item;
  (void)x;
  (void)y;
  (void)z;

}
#endif

void handlePlayerAction (PlayerData *player, int action, short x, short y, short z) {

  // 玩家丢物品时重同步槽位
  if (action == 3 || action == 4) {
    sc_setContainerSlot(
      player->client_fd, 0,
      serverSlotToClientSlot(0, player->hotbar),
      player->inventory_count[player->hotbar],
      player->inventory_items[player->hotbar]
    );
    return;
  }

  // 停止吃东西动作
  if (action == 5) {
    // 重置进食计时并清掉进食标记
    player->flagval_16 = 0;
    player->flags &= ~0x10;
  }

  // 不是挖方块相关动作就直接忽略
  if (action != 0 && action != 2) return;

  // 创造模式只会发 start mining
  // 这里不再额外校验, 直接移除方块
  if (action == 0 && GAMEMODE == 1) {
    makeBlockChange(x, y, z, 0);
    return;
  }

  uint8_t block = getBlockAt(x, y, z);

  // start mining 包只允许瞬挖方块
  if (action == 0 && !isInstantlyMined(player, block)) return;

  // 改方块失败就直接返回
  if (makeBlockChange(x, y, z, 0)) return;

  uint16_t held_item = player->inventory_items[player->hotbar];
  uint16_t item = getMiningResult(held_item, block);
  bumpToolDurability(player);

  if (item) {
    #ifdef ENABLE_PICKUP_ANIMATION
    playPickupAnimation(player, item, x, y, z);
    #endif
    givePlayerItem(player, item, 1);
  }

  // 更新周围流体
  uint8_t block_above = getBlockAt(x, y + 1, z);
  #ifdef DO_FLUID_FLOW
    checkFluidUpdate(x, y + 1, z, block_above);
    checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
    checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
    checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
    checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
  #endif

  // 检查上方是否有依附方块要一起断
  // 有的话沿整列向上处理
  uint8_t y_offset = 1;
  while (isColumnBlock(block_above)) {
    // 破坏下一格
    makeBlockChange(x, y + y_offset, z, 0);
    // 这里按空手掉落判定
    uint16_t item = getMiningResult(0, block_above);
    if (item) givePlayerItem(player, item, 1);
    // 取下一格继续处理
    y_offset ++;
    block_above = getBlockAt(x, y + y_offset, z);
  }
}

void handlePlayerUseItem (PlayerData *player, short x, short y, short z, uint8_t face) {

  // 有坐标时先取目标方块
  uint8_t target = face == 255 ? 0 : getBlockAt(x, y, z);
  // 取手持物品信息
  uint8_t *count = &player->inventory_count[player->hotbar];
  uint16_t *item = &player->inventory_items[player->hotbar];

  // 非潜行时先处理容器交互
  if (!(player->flags & 0x04) && face != 255) {
    if (target == B_crafting_table) {
      sc_openScreen(player->client_fd, 12, "Crafting", 8);
      return;
    } else if (target == B_furnace) {
      sc_openScreen(player->client_fd, 14, "Furnace", 7);
      return;
    } else if (target == B_composter) {
      // 先看手上有没有物品
      if (*count == 0) return;
      // 检查是否是可堆肥物品
      uint32_t compost_chance = isCompostItem(*item);
      if (compost_chance != 0) {
        // 扣掉参与堆肥的物品
        if ((*count -= 1) == 0) *item = 0;
        sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
        // 按概率给骨粉
        if (fast_rand() < compost_chance) {
          givePlayerItem(player, I_bone_meal, 1);
        }
        return;
      }
    }
    #ifdef ALLOW_CHESTS
    else if (target == B_chest) {
      // 取 chest 后面那段 block_changes 数据指针
      uint8_t *storage_ptr = NULL;
      for (int i = 0; i < block_changes_count; i ++) {
        if (block_changes[i].block != B_chest) continue;
        if (block_changes[i].x != x || block_changes[i].y != y || block_changes[i].z != z) continue;
        storage_ptr = (uint8_t *)(&block_changes[i + 1]);
        break;
      }
      if (storage_ptr == NULL) return;
      // 这里直接做内存复用
      // 把指针塞进玩家的 craft_items
      // 这样能省内存, 但实现非常硬
      memcpy(player->craft_items, &storage_ptr, sizeof(storage_ptr));
      // craft_items 现在存的是指针, 先锁住
      player->flags |= 0x80;
      // 打开 chest 界面
      sc_openScreen(player->client_fd, 2, "Chest", 5);
      // 从 block_changes 里装载 chest 槽位
      // 这里同样靠 memcpy 直接抄内存
      for (int i = 0; i < 27; i ++) {
        uint16_t item;
        uint8_t count;
        memcpy(&item, storage_ptr + i * 3, 2);
        memcpy(&count, storage_ptr + i * 3 + 2, 1);
        sc_setContainerSlot(player->client_fd, 2, i, count, item);
      }
      return;
    }
    #endif
  }

  // 当前槽没物品就直接返回
  if (*count == 0) return;

  // 先处理特殊物品逻辑
  if (*item == I_bone_meal) {
    uint8_t target_below = getBlockAt(x, y - 1, z);
    if (target == B_oak_sapling) {
      // 先消耗骨粉
      // 种错树苗也会白白浪费, 这是原版行为
      if ((*count -= 1) == 0) *item = 0;
      sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);
      if ( // 树苗只能种在这些方块上
        target_below == B_dirt ||
        target_below == B_grass_block ||
        target_below == B_snowy_grass_block ||
        target_below == B_mud
      ) {
        // 骨粉有 25% 概率催成树
        if ((fast_rand() & 3) == 0) placeTreeStructure(x, y, z);
      }
    }
  } else if (handlePlayerEating(player, true)) {
    // 重置进食计时并标记进食中
    player->flagval_16 = 0;
    player->flags |= 0x10;
  } else if (getItemDefensePoints(*item) != 0) {
    // 对着方块时这个动作会发两次
    // 带坐标的那次直接忽略
    if (face != 255) return;
    // 和对应护甲槽交换
    uint8_t slot = getArmorItemSlot(*item);
    uint16_t prev_item = player->inventory_items[slot];
    player->inventory_items[slot] = *item;
    player->inventory_count[slot] = 1;
    player->inventory_items[player->hotbar] = prev_item;
    player->inventory_count[player->hotbar] = 1;
    // 更新客户端背包
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, slot), 1, *item);
    sc_setContainerSlot(player->client_fd, -2, serverSlotToClientSlot(0, player->hotbar), 1, prev_item);
    return;
  }

  // 没坐标就不继续放方块
  if (face == 255) return;

  // 手上物品不是方块就退出
  uint8_t block = I_to_B(*item);
  if (block == 0) return;

  switch (face) {
    case 0: y -= 1; break;
    case 1: y += 1; break;
    case 2: z -= 1; break;
    case 3: z += 1; break;
    case 4: x -= 1; break;
    case 5: x += 1; break;
    default: break;
  }

  // 检查是否满足放置条件
  if (
    !( // 玩家是否挡住位置
      !isPassableBlock(block) &&
      x == player->x &&
      (y == player->y || y == player->y + 1) &&
      z == player->z
    ) &&
    isReplaceableBlock(getBlockAt(x, y, z)) &&
    (!isColumnBlock(block) || getBlockAt(x, y - 1, z) != B_air)
  ) {
    // 应用服务端方块改动
    if (makeBlockChange(x, y, z, block)) return;
    // 扣掉当前槽数量
    *count -= 1;
    // 数量清零后清掉物品 ID
    if (*count == 0) *item = 0;
    // 计算流体更新
    #ifdef DO_FLUID_FLOW
      checkFluidUpdate(x, y + 1, z, getBlockAt(x, y + 1, z));
      checkFluidUpdate(x - 1, y, z, getBlockAt(x - 1, y, z));
      checkFluidUpdate(x + 1, y, z, getBlockAt(x + 1, y, z));
      checkFluidUpdate(x, y, z - 1, getBlockAt(x, y, z - 1));
      checkFluidUpdate(x, y, z + 1, getBlockAt(x, y, z + 1));
    #endif
  }

  // 同步快捷栏内容给玩家
  sc_setContainerSlot(player->client_fd, 0, serverSlotToClientSlot(0, player->hotbar), *count, *item);

}

void spawnMob (uint8_t type, short x, uint8_t y, short z, uint8_t health) {

  for (int i = 0; i < MAX_MOBS; i ++) {
    // 查找 type 为 0 的空槽
    if (mob_data[i].type != 0) continue;

    // 写入输入参数
    mob_data[i].type = type;
    mob_data[i].x = x;
    mob_data[i].y = y;
    mob_data[i].z = z;
    mob_data[i].data = health & 31;

    // 用随机数和 Mob 索引拼一个 UUID
    uint8_t uuid[16];
    uint32_t r = fast_rand();
    memcpy(uuid, &r, 4);
    memcpy(uuid + 4, &i, 4);

    // 向所有玩家广播实体生成
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      sc_spawnEntity(
        player_data[j].client_fd,
        -2 - i, // 负 ID 避免和玩家 ID 冲突
        uuid, // 使用上面生成的 UUID
        type, (double)x + 0.5f, y, (double)z + 0.5f,
        // 朝向玩家反方向, 看起来像正对玩家生成
        (player_data[j].yaw + 127) & 255, 0
      );
    }

    // 新生成的 Mob 目前不需要额外元数据
    // 以后如果需要, 再打开这一行
    // broadcastMobMetadata(-1, i);

    break;
  }

}

void interactEntity (int entity_id, int interactor_id) {

  PlayerData *player;
  if (getPlayerData(interactor_id, &player)) return;

  int mob_index = -entity_id - 2;
  if (mob_index < 0 || mob_index >= MAX_MOBS) return;
  MobData *mob = &mob_data[mob_index];

  switch (mob->type) {
    case 111: // 羊
      if (player->inventory_items[player->hotbar] != I_shears)
        return;

      if ((mob->data >> 5) & 1) // 已经剪过毛就返回
        return;

      mob->data |= 1 << 5; // 标记已剪毛

      bumpToolDurability(player);

      #ifdef ENABLE_PICKUP_ANIMATION
      playPickupAnimation(player, I_white_wool, mob->x, mob->y, mob->z);
      #endif

      uint8_t item_count = 1 + (fast_rand() & 1); // 1 到 2 个
      givePlayerItem(player, I_white_wool, item_count);

      for (int i = 0; i < MAX_PLAYERS; i ++) {
        PlayerData* player = &player_data[i];
        int client_fd = player->client_fd;

        if (client_fd == -1) continue;
        if (player->flags & 0x20) continue;

        sc_entityAnimation(client_fd, interactor_id, 0);
      }

      broadcastMobMetadata(-1, entity_id);

      break;
  }
}

void hurtEntity (int entity_id, int attacker_id, uint8_t damage_type, uint8_t damage) {

  if (attacker_id >= 0) { // Attacker is a player

    PlayerData *player;
    if (getPlayerData(attacker_id, &player)) return;

    // Check if attack cooldown flag is set
    if (player->flags & 0x01) return;

    // Scale damage based on held item
    uint16_t held_item = player->inventory_items[player->hotbar];
    if (held_item == I_wooden_sword) damage *= 4;
    else if (held_item == I_golden_sword) damage *= 4;
    else if (held_item == I_stone_sword) damage *= 5;
    else if (held_item == I_iron_sword) damage *= 6;
    else if (held_item == I_diamond_sword) damage *= 7;
    else if (held_item == I_netherite_sword) damage *= 8;

    // Enable attack cooldown
    player->flags |= 0x01;
    player->flagval_8 = 0;

  }

  // Whether this attack caused the target entity to die
  uint8_t entity_died = false;

  if (entity_id >= 0) { // The attacked entity is a player

    PlayerData *player;
    if (getPlayerData(entity_id, &player)) return;

    // Don't continue if the player is already dead
    if (player->health == 0) return;

    // Calculate damage reduction from player's armor
    uint8_t defense = getPlayerDefensePoints(player);
    // This uses the old (pre-1.9) protection calculation. Factors are
    // scaled up 256 times to avoid floating point math. Due to lost
    // precision, the 4% reduction factor drops to ~3.9%, although the
    // the resulting effective damage is then also rounded down.
    uint8_t effective_damage = damage * (256 - defense * 10) / 256;

    // Process health change on the server
    if (player->health <= effective_damage) {

      player->health = 0;
      entity_died = true;

      // Prepare death message in recv_buffer
      uint8_t player_name_len = strlen(player->name);
      strcpy((char *)recv_buffer, player->name);

      if (damage_type == D_fall && damage > 8) {
        // Killed by a greater than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " fell from a high place");
        recv_buffer[player_name_len + 23] = '\0';
      } else if (damage_type == D_fall) {
        // Killed by a less than 5 block fall
        strcpy((char *)recv_buffer + player_name_len, " hit the ground too hard");
        recv_buffer[player_name_len + 24] = '\0';
      } else if (damage_type == D_lava) {
        // Killed by being in lava
        strcpy((char *)recv_buffer + player_name_len, " tried to swim in lava");
        recv_buffer[player_name_len + 22] = '\0';
      } else if (attacker_id < -1) {
        // Killed by a mob
        strcpy((char *)recv_buffer + player_name_len, " was slain by a mob");
        recv_buffer[player_name_len + 19] = '\0';
      } else if (attacker_id >= 0) {
        // Killed by a player
        PlayerData *attacker;
        if (getPlayerData(attacker_id, &attacker)) return;
        strcpy((char *)recv_buffer + player_name_len, " was slain by ");
        strcpy((char *)recv_buffer + player_name_len + 14, attacker->name);
        recv_buffer[player_name_len + 14 + strlen(attacker->name)] = '\0';
      } else if (damage_type == D_cactus) {
        // Killed by being near a cactus
        strcpy((char *)recv_buffer + player_name_len, " was pricked to death");
        recv_buffer[player_name_len + 21] = '\0';
      } else {
        // Unknown death reason
        strcpy((char *)recv_buffer + player_name_len, " died");
        recv_buffer[player_name_len + 5] = '\0';
      }

    } else player->health -= effective_damage;

    // Update health on the client
    sc_setHealth(entity_id, player->health, player->hunger, player->saturation);

  } else { // The attacked entity is a mob

    int mob_index = -entity_id - 2;
    if (mob_index < 0 || mob_index >= MAX_MOBS) return;
    MobData *mob = &mob_data[mob_index];

    uint8_t mob_health = mob->data & 31;

    // Don't continue if the mob is already dead
    if (mob_health == 0) return;

    // Set the mob's panic timer
    mob->data |= (3 << 6);

    // Process health change on the server
    if (mob_health <= damage) {

      mob->data -= mob_health;
      mob->y = 0;
      entity_died = true;

      // Handle mob drops
      if (attacker_id >= 0) {
        PlayerData *player;
        if (getPlayerData(attacker_id, &player)) return;
        switch (mob->type) {
          case 26: givePlayerItem(player, I_chicken, 1); break;
          case 30: givePlayerItem(player, I_beef, 1 + (fast_rand() % 3)); break;
          case 100: givePlayerItem(player, I_porkchop, 1 + (fast_rand() % 3)); break;
          case 111: givePlayerItem(player, I_mutton, 1 + (fast_rand() & 1)); break;
          case 150: givePlayerItem(player, I_rotten_flesh, (fast_rand() % 3)); break;
          default: break;
        }
      }

    } else mob->data -= damage;

  }

  // Broadcast damage event to all players
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int client_fd = player_data[i].client_fd;
    if (client_fd == -1) continue;
    sc_damageEvent(client_fd, entity_id, damage_type);
    if (player_data[i].client_fd != client_fd) continue;
    // Below this, handle death events
    if (!entity_died) continue;
    sc_entityEvent(client_fd, entity_id, 3);
    if (player_data[i].client_fd != client_fd) continue;
    if (entity_id >= 0) {
      // If a player died, broadcast their death message
      sc_systemChat(client_fd, (char *)recv_buffer, strlen((char *)recv_buffer));
    }
  }

}

// Simulates events scheduled for regular intervals
// Takes the time since the last tick in microseconds as the only arguemnt
void handleServerTick (int64_t time_since_last_tick) {

  // Update world time
  world_time = (world_time + time_since_last_tick / 50000) % 24000;
  // Increment server tick counter
  server_ticks ++;

  // Update player events
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData *player = &player_data[i];
    if (player->client_fd == -1) continue; // Skip offline players
    if (player->flags & 0x20) { // Check "client loading" flag
      #ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
      if (deferred_chunk_active[i]) {
        processDeferredInitialChunks(player, i);
        continue;
      }
      #endif
      // If 3 seconds (60 vanilla ticks) have passed, assume player has loaded
      player->flagval_16 ++;
      if (player->flagval_16 > (uint16_t)(3 * TICKS_PER_SECOND)) {
        handlePlayerJoin(player);
      } else continue;
    }
    // Reset player attack cooldown
    if (player->flags & 0x01) {
      if (player->flagval_8 >= (uint8_t)(0.6f * TICKS_PER_SECOND)) {
        player->flags &= ~0x01;
        player->flagval_8 = 0;
      } else player->flagval_8 ++;
    }
    // Handle eating animation
    if (player->flags & 0x10) {
      if (player->flagval_16 >= (uint16_t)(1.6f * TICKS_PER_SECOND)) {
        handlePlayerEating(&player_data[i], false);
        player->flags &= ~0x10;
        player->flagval_16 = 0;
      } else player->flagval_16 ++;
    }
    // Reset movement update cooldown if not broadcasting every update
    // Effectively ties player movement updates to the tickrate
    #ifndef BROADCAST_ALL_MOVEMENT
      player->flags &= ~0x40;
    #endif
    processDeferredInitialChunks(player, i);
    if (deferred_movement_active[i]) continue;
    #ifdef DEV_MINIMAL_PLAY_BOOTSTRAP
    if (deferred_chunk_active[i]) continue;
    #endif
    // Below this, process events that happen once per second
    if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
    // Send Keep Alive and Update Time packets
    if (sc_keepAlive(player->client_fd) == -1) {
      disconnectClient(&player->client_fd, -3);
      continue;
    }
    if (sc_updateTime(player->client_fd, world_time) == -1) {
      disconnectClient(&player->client_fd, -3);
      continue;
    }
    // Tick damage from lava
    uint8_t block = getBlockAt(player->x, player->y, player->z);
    if (block >= B_lava && block < B_lava + 4) {
      hurtEntity(player->client_fd, -1, D_lava, 8);
    }
    #ifdef ENABLE_CACTUS_DAMAGE
    // Tick damage from a cactus block if one is under/inside or around the player.
    if (block == B_cactus ||
      getBlockAt(player->x + 1, player->y, player->z) == B_cactus ||
      getBlockAt(player->x - 1, player->y, player->z) == B_cactus ||
      getBlockAt(player->x, player->y, player->z + 1) == B_cactus ||
      getBlockAt(player->x, player->y, player->z - 1) == B_cactus
    ) hurtEntity(player->client_fd, -1, D_cactus, 4);
    #endif
    // Heal from saturation if player is able and has enough food
    if (player->health >= 20 || player->health == 0) continue;
    if (player->hunger < 18) continue;
    if (player->saturation >= 600) {
      player->saturation -= 600;
      player->health ++;
    } else {
      player->hunger --;
      player->health ++;
    }
    sc_setHealth(player->client_fd, player->health, player->hunger, player->saturation);
  }

  // Perform regular checks for if it's time to write to disk
  writeDataToDiskOnInterval();

  /**
   * If the RNG seed ever hits 0, it'll never generate anything
   * else. This is because the fast_rand function uses a simple
   * XORshift. This isn't a common concern, so we only check for
   * this periodically. If it does become zero, we reset it to
   * the world seed as a good-enough fallback.
   */
  if (rng_seed == 0) rng_seed = world_seed;

  // Tick mob behavior
  for (int i = 0; i < MAX_MOBS; i ++) {
    if (mob_data[i].type == 0) continue;
    int entity_id = -2 - i;

    // Handle deallocation on mob death
    if ((mob_data[i].data & 31) == 0) {
      if (mob_data[i].y < (unsigned int)TICKS_PER_SECOND) {
        mob_data[i].y ++;
        continue;
      }
      mob_data[i].type = 0;
      for (int j = 0; j < MAX_PLAYERS; j ++) {
        int target_fd = player_data[j].client_fd;
        if (target_fd == -1) continue;
        // Spawn death smoke particles
        sc_entityEvent(target_fd, entity_id, 60);
        if (player_data[j].client_fd != target_fd) continue;
        // Remove the entity from the client
        sc_removeEntity(target_fd, entity_id);
      }
      continue;
    }

    uint8_t passive = (
      mob_data[i].type == 26 || // Chicken
      mob_data[i].type == 30 || // Cow
      mob_data[i].type == 100 || // Pig
      mob_data[i].type == 111 // Sheep
    );
    // Mob "panic" timer, set to 3 after being hit
    // Currently has no effect on hostile mobs
    uint8_t panic = (mob_data[i].data >> 6) & 3;

    // Burn hostile mobs if above ground during sunlight
    if (!passive && (world_time < 13000 || world_time > 23460) && mob_data[i].y > 48) {
      hurtEntity(entity_id, -1, D_on_fire, 2);
    }

    uint32_t r = fast_rand();

    if (passive) {
      if (panic) {
        // If panicking, move randomly at up to 4 times per second
        if (TICKS_PER_SECOND >= 4) {
          uint32_t ticks_per_panic = (uint32_t)(TICKS_PER_SECOND / 4);
          if (server_ticks % ticks_per_panic != 0) continue;
        }
        // Reset panic state after timer runs out
        // Each panic timer tick takes one second
        if (server_ticks % (uint32_t)TICKS_PER_SECOND == 0) {
          mob_data[i].data -= (1 << 6);
        }
      } else {
        // When not panicking, move idly once per 4 seconds on average
        if (r % (4 * (unsigned int)TICKS_PER_SECOND) != 0) continue;
      }
    } else {
      // Update hostile mobs once per second
      if (server_ticks % (uint32_t)TICKS_PER_SECOND != 0) continue;
    }

    // Find the player closest to this mob
    PlayerData* closest_player = &player_data[0];
    uint32_t closest_dist = 2147483647;
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      uint16_t curr_dist = (
        abs(mob_data[i].x - player_data[j].x) +
        abs(mob_data[i].z - player_data[j].z)
      );
      if (curr_dist < closest_dist) {
        closest_dist = curr_dist;
        closest_player = &player_data[j];
      }
    }

    // Despawn mobs past a certain distance from nearest player
    if (closest_dist > MOB_DESPAWN_DISTANCE) {
      mob_data[i].type = 0;
      continue;
    }

    short old_x = mob_data[i].x, old_z = mob_data[i].z;
    uint8_t old_y = mob_data[i].y;

    short new_x = old_x, new_z = old_z;
    uint8_t new_y = old_y, yaw = 0;

    if (passive) { // Passive mob movement handling

      // Move by one block on the X or Z axis
      // Yaw is set to face in the direction of motion
      if ((r >> 2) & 1) {
        if ((r >> 1) & 1) { new_x += 1; yaw = 192; }
        else { new_x -= 1; yaw = 64; }
      } else {
        if ((r >> 1) & 1) { new_z += 1; yaw = 0; }
        else { new_z -= 1; yaw = 128; }
      }

    } else { // Hostile mob movement handling

      // If we're already next to the player, hurt them and skip movement
      if (closest_dist < 3 && abs(old_y - closest_player->y) < 2) {
        hurtEntity(closest_player->client_fd, entity_id, D_generic, 6);
        continue;
      }

      // Move towards the closest player on 8 axis
      // The condition nesting ensures a correct yaw at 45 degree turns
      if (closest_player->x < old_x) {
        new_x -= 1; yaw = 64;
        if (closest_player->z < old_z) { new_z -= 1; yaw += 32; }
        else if (closest_player->z > old_z) { new_z += 1; yaw -= 32; }
      }
      else if (closest_player->x > old_x) {
        new_x += 1; yaw = 192;
        if (closest_player->z < old_z) { new_z -= 1; yaw -= 32; }
        else if (closest_player->z > old_z) { new_z += 1; yaw += 32; }
      } else {
        if (closest_player->z < old_z) { new_z -= 1; yaw = 128; }
        else if (closest_player->z > old_z) { new_z += 1; yaw = 0; }
      }

    }

    // Holds the block that the mob is moving into
    uint8_t block = getBlockAt(new_x, new_y, new_z);
    // Holds the block above the target block, i.e. the "head" block
    uint8_t block_above = getBlockAt(new_x, new_y + 1, new_z);

    // Validate movement on X axis
    if (new_x != old_x && (
      !isPassableBlock(getBlockAt(new_x, new_y + 1, old_z)) ||
      (
        !isPassableBlock(getBlockAt(new_x, new_y, old_z)) &&
        !isPassableBlock(getBlockAt(new_x, new_y + 2, old_z))
      )
    )) {
      new_x = old_x;
      block = getBlockAt(old_x, new_y, new_z);
      block_above = getBlockAt(old_x, new_y + 1, new_z);
    }
    // Validate movement on Z axis
    if (new_z != old_z && (
      !isPassableBlock(getBlockAt(old_x, new_y + 1, new_z)) ||
      (
        !isPassableBlock(getBlockAt(old_x, new_y, new_z)) &&
        !isPassableBlock(getBlockAt(old_x, new_y + 2, new_z))
      )
    )) {
      new_z = old_z;
      block = getBlockAt(new_x, new_y, old_z);
      block_above = getBlockAt(new_x, new_y + 1, old_z);
    }
    // Validate diagonal movement
    if (new_x != old_x && new_z != old_z && (
      !isPassableBlock(block_above) ||
      (
        !isPassableBlock(block) &&
        !isPassableBlock(getBlockAt(new_x, new_y + 2, new_z))
      )
    )) {
      // We know that movement along just one axis is fine thanks to the
      // checks above, pick one based on proximity.
      int dist_x = abs(old_x - closest_player->x);
      int dist_z = abs(old_z - closest_player->z);
      if (dist_x < dist_z) new_z = old_z;
      else new_x = old_x;
      block = getBlockAt(new_x, new_y, new_z);
    }

    // Check if we're supposed to climb/drop one block
    // The checks above already ensure that there's enough space to climb
    if (!isPassableBlock(block)) new_y += 1;
    else if (isPassableBlock(getBlockAt(new_x, new_y - 1, new_z))) new_y -= 1;

    // Exit early if all movement was cancelled
    if (new_x == mob_data[i].x && new_z == old_z && new_y == old_y) continue;

    // Prevent collisions with other mobs
    uint8_t colliding = false;
    for (int j = 0; j < MAX_MOBS; j ++) {
      if (j == i) continue;
      if (mob_data[j].type == 0) continue;
      if (
        mob_data[j].x == new_x &&
        mob_data[j].z == new_z &&
        abs((int)mob_data[j].y - (int)new_y) < 2
      ) {
        colliding = true;
        break;
      }
    }
    if (colliding) continue;

    if ( // Hurt mobs that stumble into lava
      (block >= B_lava && block < B_lava + 4) ||
      (block_above >= B_lava && block_above < B_lava + 4)
    ) hurtEntity(entity_id, -1, D_lava, 8);

    // Store new mob position
    mob_data[i].x = new_x;
    mob_data[i].y = new_y;
    mob_data[i].z = new_z;

    // Vary the yaw angle to look just a little less robotic
    yaw += ((r >> 7) & 31) - 16;

    // Broadcast relevant entity movement packets
    for (int j = 0; j < MAX_PLAYERS; j ++) {
      if (player_data[j].client_fd == -1) continue;
      sc_teleportEntity (
        player_data[j].client_fd, entity_id,
        (double)new_x + 0.5, new_y, (double)new_z + 0.5,
        yaw * 360 / 256, 0
      );
      sc_setHeadRotation(player_data[j].client_fd, entity_id, yaw);
    }

  }

}

#ifdef ALLOW_CHESTS
// Broadcasts a chest slot update to all clients who have that chest open,
// except for the client who initiated the update.
void broadcastChestUpdate (int origin_fd, uint8_t *storage_ptr, uint16_t item, uint8_t count, uint8_t slot) {

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (player_data[i].flags & 0x20) continue;
    // Filter for players that have this chest open
    if (memcmp(player_data[i].craft_items, &storage_ptr, sizeof(storage_ptr)) != 0) continue;
    // Send slot update packet
    sc_setContainerSlot(player_data[i].client_fd, 2, slot, count, item);
  }

  #ifndef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeChestChangesToDisk(storage_ptr, slot);
  #endif

}
#endif

ssize_t writeEntityData (int client_fd, EntityData *data) {
  writeByte(client_fd, data->index);
  writeVarInt(client_fd, data->type);

  switch (data->type) {
    case 0: // Byte
      return writeByte(client_fd, data->value.byte);
    case 21: // Pose
      writeVarInt(client_fd, data->value.pose);
      return 0;

    default: return -1;
  }
}

// Returns the networked size of an EntityData entry
int sizeEntityData (EntityData *data) {
  int value_size;

  switch (data->type) {
    case 0: // Byte
      value_size = 1;
      break;
    case 21: // Pose
      value_size = sizeVarInt(data->value.pose);
      break;

    default: return -1;
  }

  return 1 + sizeVarInt(data->type) + value_size;
}

// Returns the networked size of an array of EntityData entries
int sizeEntityMetadata (EntityData *metadata, size_t length) {
  int total_size = 0;
  for (size_t i = 0; i < length; i ++) {
    int size = sizeEntityData(&metadata[i]);
    if (size == -1) return -1;
    total_size += size;
  }
  return total_size;
}



