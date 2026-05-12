#include <stdio.h>
#include <string.h>

#ifdef ARDUINO_PLATFORM
  // 网络接口走 arduino_compat.h
#elif defined(ESP_PLATFORM)
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_task_wdt.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "registries.h"
#include "worldgen.h"
#include "crafting.h"
#include "procedures.h"
#include "packets.h"

#define ACTIVE_VIEW_DISTANCE VIEW_DISTANCE
#define CHUNK_PACKET_BUFFER_SIZE 2048
#define TOTAL_CHUNK_SECTION_COUNT 24
#define EMPTY_SECTIONS_BELOW_ZERO 4
#define GENERATED_CHUNK_SECTION_COUNT 6
#define MAX_CHUNK_SECTION_ENCODED_SIZE 5000
#define SKY_LIGHT_SECTION_COUNT 26
#define DARK_SKY_LIGHT_SECTION_COUNT 8
#define MC_VERSION_NAME "26.1.2"
#define MC_PROTOCOL_VERSION 775
#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

#ifndef htonll
static uint64_t chunkPacketHtonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)(value & 0xFFFFFFFFu));
    return ((uint64_t)low << 32) | high;
  #else
    return value;
  #endif
}
#else
  #define chunkPacketHtonll htonll
#endif

typedef struct {
  int client_fd;
  size_t used;
  uint8_t failed;
  uint8_t data[CHUNK_PACKET_BUFFER_SIZE];
} ChunkPacketWriter;

static uint8_t chunk_section_encoded[GENERATED_CHUNK_SECTION_COUNT][MAX_CHUNK_SECTION_ENCODED_SIZE];
static uint16_t chunk_section_encoded_len[GENERATED_CHUNK_SECTION_COUNT];
static uint64_t chunk_section_words[64 * 8];

static int chunkPacketFlush (ChunkPacketWriter *writer) {
  if (writer->failed) return -1;
  if (writer->used == 0) return 0;
  if (send_all(writer->client_fd, writer->data, writer->used) == -1) {
    writer->failed = 1;
    return -1;
  }
  writer->used = 0;
  return 0;
}

static int chunkPacketWrite (ChunkPacketWriter *writer, const void *data, size_t len) {
  if (writer->failed) return -1;
  if (len > CHUNK_PACKET_BUFFER_SIZE) {
    if (chunkPacketFlush(writer) == -1) return -1;
    if (send_all(writer->client_fd, data, len) == -1) {
      writer->failed = 1;
      return -1;
    }
    return 0;
  }
  if (writer->used + len > CHUNK_PACKET_BUFFER_SIZE) {
    if (chunkPacketFlush(writer) == -1) return -1;
  }
  memcpy(writer->data + writer->used, data, len);
  writer->used += len;
  return 0;
}

static int chunkPacketWriteByte (ChunkPacketWriter *writer, uint8_t value) {
  return chunkPacketWrite(writer, &value, sizeof(value));
}

static int chunkPacketWriteUint16 (ChunkPacketWriter *writer, uint16_t value) {
  uint16_t be = htons(value);
  return chunkPacketWrite(writer, &be, sizeof(be));
}

static int chunkPacketWriteUint32 (ChunkPacketWriter *writer, uint32_t value) {
  uint32_t be = htonl(value);
  return chunkPacketWrite(writer, &be, sizeof(be));
}

static int chunkPacketWriteUint64 (ChunkPacketWriter *writer, uint64_t value) {
  uint64_t be = chunkPacketHtonll(value);
  return chunkPacketWrite(writer, &be, sizeof(be));
}

static int chunkPacketWriteVarInt (ChunkPacketWriter *writer, uint32_t value) {
  uint8_t encoded[5];
  size_t used = 0;
  while (true) {
    if ((value & ~SEGMENT_BITS) == 0) {
      encoded[used++] = (uint8_t)value;
      break;
    }
    encoded[used++] = (uint8_t)((value & SEGMENT_BITS) | CONTINUE_BIT);
    value >>= 7;
  }
  return chunkPacketWrite(writer, encoded, used);
}

static size_t chunkPacketEncodeVarInt (uint8_t *dst, uint32_t value) {
  size_t used = 0;
  while (true) {
    if ((value & ~SEGMENT_BITS) == 0) {
      dst[used++] = (uint8_t)value;
      return used;
    }
    dst[used++] = (uint8_t)((value & SEGMENT_BITS) | CONTINUE_BIT);
    value >>= 7;
  }
}

static uint8_t chunkSectionBitsPerEntry (uint16_t palette_len) {
  if (palette_len <= 1) return 0;

  uint8_t bits = 0;
  uint16_t max_index = palette_len - 1;
  while (max_index > 0) {
    bits ++;
    max_index >>= 1;
  }
  if (bits < 4) bits = 4;
  return bits;
}

static uint8_t chunkSectionHasFluid (uint8_t block) {
  return block >= B_water && block <= B_lava_6;
}

static size_t encodeChunkSection (uint8_t *dst, const uint8_t *blocks, uint8_t biome) {
  uint16_t palette_index[256];
  uint8_t palette_blocks[256];
  uint16_t palette_len = 0;
  uint16_t non_air_count = 0;
  uint16_t fluid_count = 0;
  size_t used = 0;

  memset(palette_index, 0xFF, sizeof(palette_index));

  for (int i = 0; i < 4096; i ++) {
    uint8_t block = blocks[i];
    if (block != B_air) non_air_count ++;
    if (chunkSectionHasFluid(block)) fluid_count ++;
    if (palette_index[block] != 0xFFFF) continue;
    palette_index[block] = palette_len;
    palette_blocks[palette_len] = block;
    palette_len ++;
  }

  uint16_t block_count = htons(non_air_count);
  memcpy(dst + used, &block_count, sizeof(block_count));
  used += sizeof(block_count);

  uint16_t fluid_be = htons(fluid_count);
  memcpy(dst + used, &fluid_be, sizeof(fluid_be));
  used += sizeof(fluid_be);

  uint8_t bits = chunkSectionBitsPerEntry(palette_len);
  dst[used++] = bits;

  if (bits == 0) {
    used += chunkPacketEncodeVarInt(dst + used, block_palette[palette_blocks[0]]);
  } else {
    used += chunkPacketEncodeVarInt(dst + used, palette_len);
    for (uint16_t i = 0; i < palette_len; i ++) {
      used += chunkPacketEncodeVarInt(dst + used, block_palette[palette_blocks[i]]);
    }

    size_t long_count = (size_t)bits * 64;
    memset(chunk_section_words, 0, long_count * sizeof(uint64_t));

    for (int i = 0; i < 4096; i ++) {
      uint64_t value = palette_index[blocks[i]];
      uint32_t bit_index = (uint32_t)i * bits;
      size_t word_index = bit_index >> 6;
      uint8_t bit_offset = bit_index & 63;

      chunk_section_words[word_index] |= value << bit_offset;
      if (bit_offset + bits > 64) {
        chunk_section_words[word_index + 1] |= value >> (64 - bit_offset);
      }
    }

    for (size_t i = 0; i < long_count; i ++) {
      uint64_t be = chunkPacketHtonll(chunk_section_words[i]);
      memcpy(dst + used, &be, sizeof(be));
      used += sizeof(be);
    }
  }

  dst[used++] = 0;     // biome 位数
  dst[used++] = biome; // biome 调色板

  return used;
}

// 服务端到客户端 状态响应 (server list ping)
int sc_statusResponse (int client_fd) {

  int online = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (player_data[i].client_fd != -1) online++;
  }

  char max_players[8];
  char online_players[8];
  int max_len = snprintf(max_players, sizeof(max_players), "%u", MAX_PLAYERS);
  int online_len = snprintf(online_players, sizeof(online_players), "%d", online);

  if (max_len < 0) max_len = 1;
  if (online_len < 0) online_len = 1;

  char header[] = "{"
    "\"version\":{\"name\":\"" MC_VERSION_NAME "\",\"protocol\":" STRINGIFY(MC_PROTOCOL_VERSION) "},"
    "\"players\":{\"max\":";
  char middle[] = ",\"online\":";
  char description[] =
    "},\"description\":{\"extra\":["
      "{\"text\":\"T\",\"color\":\"red\"},"
      "{\"text\":\"h\",\"color\":\"gold\"},"
      "{\"text\":\"i\",\"color\":\"yellow\"},"
      "{\"text\":\"s\",\"color\":\"green\"},"
      "{\"text\":\" \",\"color\":\"dark_gray\"},"
      "{\"text\":\"i\",\"color\":\"aqua\"},"
      "{\"text\":\"s\",\"color\":\"blue\"},"
      "{\"text\":\" \",\"color\":\"dark_gray\"},"
      "{\"text\":\"A\",\"color\":\"light_purple\"},"
      "{\"text\":\" \",\"color\":\"dark_gray\"},"
      "{\"text\":\"E\",\"color\":\"red\"},"
      "{\"text\":\"S\",\"color\":\"gold\"},"
      "{\"text\":\"P\",\"color\":\"yellow\"},"
      "{\"text\":\"3\",\"color\":\"green\"},"
      "{\"text\":\"2\",\"color\":\"aqua\"},"
      "{\"text\":\" \",\"color\":\"dark_gray\"},"
      "{\"text\":\"S\",\"color\":\"blue\"},"
      "{\"text\":\"E\",\"color\":\"light_purple\"},"
      "{\"text\":\"R\",\"color\":\"red\"},"
      "{\"text\":\"V\",\"color\":\"gold\"},"
      "{\"text\":\"E\",\"color\":\"yellow\"},"
      "{\"text\":\"R\",\"color\":\"green\"}"
    "],\"text\":\"\"}";
  char footer[] = "}";

  uint16_t string_len = (uint16_t)(
    (sizeof(header) - 1) +
    max_len +
    (sizeof(middle) - 1) +
    online_len +
    (sizeof(description) - 1) +
    (sizeof(footer) - 1)
  );

  writeVarInt(client_fd, 1 + string_len + sizeVarInt(string_len));
  writeByte(client_fd, 0x00);

  writeVarInt(client_fd, string_len);
  send_all(client_fd, header, sizeof(header) - 1);
  send_all(client_fd, max_players, max_len);
  send_all(client_fd, middle, sizeof(middle) - 1);
  send_all(client_fd, online_players, online_len);
  send_all(client_fd, description, sizeof(description) - 1);
  send_all(client_fd, footer, sizeof(footer) - 1);

  return 0;
}

// 客户端到服务端 Handshake
int cs_handshake (int client_fd) {
  printf("Received Handshake:\n");

  printf("  Protocol version: %d\n", (int)readVarInt(client_fd));
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Server address: %s\n", recv_buffer);
  printf("  Server port: %u\n", readUint16(client_fd));
  int intent = readVarInt(client_fd);
  if (intent == VARNUM_ERROR) return 1;
  printf("  Intent: %d\n\n", intent);
  setClientState(client_fd, intent);

  return 0;
}

// 客户端到服务端 登录 Start
int cs_loginStart (int client_fd, uint8_t *uuid, char *name) {
  printf("Received Login Start:\n");

  readString(client_fd);
  if (recv_count == -1) return 1;
  strncpy(name, (char *)recv_buffer, 16 - 1);
  name[16 - 1] = '\0';
  printf("  Player name: %s\n", name);
  recv_count = recv_all(client_fd, recv_buffer, 16, false);
  if (recv_count == -1) return 1;
  memcpy(uuid, recv_buffer, 16);
  printf("  Player UUID: ");
  for (int i = 0; i < 16; i ++) printf("%x", uuid[i]);
  printf("\n\n");

  return 0;
}

// 服务端到客户端 登录 Success
int sc_loginSuccess (int client_fd, uint8_t *uuid, char *name) {
  printf("Sending Login Success...\n\n");

  uint8_t name_length = strlen(name);
  writeVarInt(client_fd, 1 + 16 + sizeVarInt(name_length) + name_length + 1);
  writeVarInt(client_fd, 0x02);
  send_all(client_fd, uuid, 16);
  writeVarInt(client_fd, name_length);
  send_all(client_fd, name, name_length);
  writeVarInt(client_fd, 0);

  return 0;
}

int cs_clientInformation (int client_fd) {
  int tmp;
  printf("Received Client Information:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Locale: %s\n", recv_buffer);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  printf("  View distance: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Chat mode: %d\n", tmp);
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Chat colors: on\n");
  else printf("  Chat colors: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  printf("  Skin parts: %d\n", tmp);
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Main hand: right\n");
  else printf("  Main hand: left\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Text filtering: on\n");
  else printf("  Text filtering: off\n");
  tmp = readByte(client_fd);
  if (recv_count == -1) return 1;
  if (tmp) printf("  Allow listing: on\n");
  else printf("  Allow listing: off\n");
  tmp = readVarInt(client_fd);
  if (recv_count == -1) return 1;
  printf("  Particles: %d\n\n", tmp);
  return 0;
}

// 客户端到服务端 Select Known Packs
int cs_selectKnownPacks (int client_fd) {
  printf("Received Client's Known Packs\n");

  int pack_count = readVarInt(client_fd);
  if (pack_count == VARNUM_ERROR) return 1;
  printf("  Count: %d\n", pack_count);
  uint8_t selection_matches = pack_count == 1;

  char namespace_name[64];
  char pack_id[64];
  char pack_version[64];

  for (int i = 0; i < pack_count; i ++) {
    readString(client_fd);
    if (recv_count == -1) return 1;
    strncpy(namespace_name, (char *)recv_buffer, sizeof(namespace_name) - 1);
    namespace_name[sizeof(namespace_name) - 1] = '\0';

    readString(client_fd);
    if (recv_count == -1) return 1;
    strncpy(pack_id, (char *)recv_buffer, sizeof(pack_id) - 1);
    pack_id[sizeof(pack_id) - 1] = '\0';

    readString(client_fd);
    if (recv_count == -1) return 1;
    strncpy(pack_version, (char *)recv_buffer, sizeof(pack_version) - 1);
    pack_version[sizeof(pack_version) - 1] = '\0';

    printf("  Pack %d: %s:%s@%s\n", i + 1, namespace_name, pack_id, pack_version);

    if (
      strcmp(namespace_name, "minecraft") != 0 ||
      strcmp(pack_id, "core") != 0 ||
      strcmp(pack_version, MC_VERSION_NAME) != 0
    ) selection_matches = 0;
  }

  if (selection_matches) {
    printf("  Selection matches server known packs\n");
  } else {
    printf("  WARNING: client pack selection differs from server request\n");
    printf("  WARNING: ESP32MC currently sends registry entries without inline fallback data\n");
  }

  printf("\n");
  return 0;
}

// 服务端到客户端 Known Packs
int sc_knownPacks (int client_fd) {
  printf("Sending Server's Known Packs\n\n");
  static const char namespace_name[] = "minecraft";
  static const char pack_name[] = "core";
  static const char pack_version[] = MC_VERSION_NAME;
  const int namespace_len = (int)strlen(namespace_name);
  const int pack_name_len = (int)strlen(pack_name);
  const int pack_version_len = (int)strlen(pack_version);

  writeVarInt(
    client_fd,
    1 +
    1 +
    sizeVarInt(namespace_len) + namespace_len +
    sizeVarInt(pack_name_len) + pack_name_len +
    sizeVarInt(pack_version_len) + pack_version_len
  );
  writeVarInt(client_fd, 0x0E);
  writeVarInt(client_fd, 1);

  writeVarInt(client_fd, namespace_len);
  send_all(client_fd, namespace_name, namespace_len);

  writeVarInt(client_fd, pack_name_len);
  send_all(client_fd, pack_name, pack_name_len);

  writeVarInt(client_fd, pack_version_len);
  send_all(client_fd, pack_version, pack_version_len);

  return 0;
}

// 服务端到客户端 更新 Enabled Features
int sc_updateEnabledFeatures (int client_fd) {
  printf("Sending Enabled Features\n\n");
  static const char feature_name[] = "minecraft:vanilla";
  const int feature_len = (int)strlen(feature_name);

  writeVarInt(
    client_fd,
    1 +
    1 +
    sizeVarInt(feature_len) + feature_len
  );
  writeVarInt(client_fd, 0x0C);
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, feature_len);
  send_all(client_fd, feature_name, feature_len);

  return 0;
}

// 客户端到服务端 Plugin 消息
int cs_pluginMessage (int client_fd) {
  printf("Received Plugin Message:\n");
  readString(client_fd);
  if (recv_count == -1) return 1;
  printf("  Channel: \"%s\"\n", recv_buffer);
  if (strcmp((char *)recv_buffer, "minecraft:brand") == 0) {
    readString(client_fd);
    if (recv_count == -1) return 1;
    printf("  Brand: \"%s\"\n", recv_buffer);
  }
  printf("\n");
  return 0;
}

// 服务端到客户端 Plugin 消息
int sc_sendPluginMessage (int client_fd, const char *channel, const uint8_t *data, size_t data_len) {
  printf("Sending Plugin Message\n\n");
  int channel_len = (int)strlen(channel);

  writeVarInt(client_fd, 1 + sizeVarInt(channel_len) + channel_len + sizeVarInt(data_len) + data_len);
  writeByte(client_fd, 0x01);

  writeVarInt(client_fd, channel_len);
  send_all(client_fd, channel, channel_len);

  writeVarInt(client_fd, data_len);
  send_all(client_fd, data, data_len);

  return 0;
}

// 服务端到客户端 Finish 配置阶段
int sc_finishConfiguration (int client_fd) {
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, 0x03);
  return 0;
}

// 服务端到客户端 登录 (play)
int sc_loginPlay (int client_fd) {
  debug_last_out_packet_id = 0x31;
  debug_last_out_packet_length = 47 + sizeVarInt(MAX_PLAYERS) + sizeVarInt(ACTIVE_VIEW_DISTANCE) * 2;
  debug_last_out_stage = "login_play";

  writeVarInt(client_fd, 47 + sizeVarInt(MAX_PLAYERS) + sizeVarInt(ACTIVE_VIEW_DISTANCE) * 2);
  writeByte(client_fd, 0x31);
  // 实体 ID
  writeUint32(client_fd, client_fd);
  // hardcore
  writeByte(client_fd, false);
  // 维度列表
  writeVarInt(client_fd, 1);
  writeVarInt(client_fd, 9);
  const char *dimension = "overworld";
  send_all(client_fd, dimension, 9);
  // 最大玩家数
  writeVarInt(client_fd, MAX_PLAYERS);
  // 视距
  writeVarInt(client_fd, ACTIVE_VIEW_DISTANCE);
  // 模拟距离
  writeVarInt(client_fd, ACTIVE_VIEW_DISTANCE);
  // 精简调试信息
  writeByte(client_fd, 0);
  // 重生界面
  writeByte(client_fd, true);
  // 限制合成
  writeByte(client_fd, false);
  // 维度 ID
  // 这里只发 overworld
  writeVarInt(client_fd, 0);
  // 维度名
  writeVarInt(client_fd, 9);
  send_all(client_fd, dimension, 9);
  // 哈希种子
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // 游戏模式
  writeByte(client_fd, GAMEMODE);
  // 上个游戏模式
  writeByte(client_fd, 0xFF);
  // debug 世界
  writeByte(client_fd, 0);
  // flat 世界
  writeByte(client_fd, 0);
  // 死亡位置
  writeByte(client_fd, 0);
  // 传送门冷却
  writeVarInt(client_fd, 0);
  // 海平面
  writeVarInt(client_fd, 63);
  // secure chat
  writeByte(client_fd, 0);

  return 0;

}

// 服务端到客户端 Synchronize 玩家 坐标
int sc_synchronizePlayerPosition (int client_fd, double x, double y, double z, float yaw, float pitch) {
  debug_last_out_packet_id = 0x48;
  debug_last_out_packet_length = 61 + sizeVarInt(-1);
  debug_last_out_stage = "sync_position";

  writeVarInt(client_fd, 61 + sizeVarInt(-1));
  writeByte(client_fd, 0x48);

  // 传送 ID
  writeVarInt(client_fd, -1);

  // 坐标
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // 速度
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);
  writeDouble(client_fd, 0);

  // 朝向
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);

  // 标志位
  writeUint32(client_fd, 0);

  return 0;

}

// 服务端到客户端 Set 默认 生成 坐标
int sc_setDefaultSpawnPosition (int client_fd, int64_t x, int64_t y, int64_t z, float yaw, float pitch) {
  const char *dimension = "minecraft:overworld";
  size_t dimension_len = strlen(dimension);
  int payload_length = sizeVarInt((int)dimension_len) + (int)dimension_len + 8 + 4 + 4;
  uint64_t packed_position =
    ((((uint64_t)x) & 0x3FFFFFFULL) << 38) |
    ((((uint64_t)z) & 0x3FFFFFFULL) << 12) |
    (((uint64_t)y) & 0xFFFULL);

  debug_last_out_packet_id = 0x61;
  debug_last_out_packet_length = sizeVarInt(0x61) + payload_length;
  debug_last_out_stage = "default_spawn";

  writeVarInt(client_fd, sizeVarInt(0x61) + payload_length);
  writeVarInt(client_fd, 0x61);

  writeVarInt(client_fd, (int)dimension_len);
  send_all(client_fd, dimension, dimension_len);
  writeUint64(client_fd, packed_position);
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);

  return 0;
}

// 服务端到客户端 玩家 Abilities
int sc_playerAbilities (int client_fd, uint8_t flags) {

  writeVarInt(client_fd, 10);
  writeByte(client_fd, 0x40);

  writeByte(client_fd, flags);
  writeFloat(client_fd, 0.05f);
  writeFloat(client_fd, 0.1f);

  return 0;
}

// 服务端到客户端 更新 时间
int sc_updateTime (int client_fd, uint64_t ticks) {
  #define PACKET_WRITE(call) do { if ((call) == -1) return -1; } while (0)
  const uint32_t overworld_clock_id = 0;
  const uint32_t the_end_clock_id = 1;
  const int clock_update_count = 2;
  const int clock_state_size = sizeVarLong(ticks) + 4 + 4;
  const int payload_length =
    8 +
    sizeVarInt(clock_update_count) +
    sizeVarInt(overworld_clock_id) + clock_state_size +
    sizeVarInt(the_end_clock_id) + clock_state_size;

  debug_last_out_packet_id = 0x71;
  debug_last_out_packet_length = sizeVarInt(0x71) + payload_length;
  debug_last_out_stage = "update_time";

  PACKET_WRITE(writeVarInt(client_fd, debug_last_out_packet_length));
  PACKET_WRITE(writeVarInt(client_fd, 0x71));

  PACKET_WRITE(writeUint64(client_fd, ticks));

  // 26.1.2 这里会顺带同步 world_clock
  PACKET_WRITE(writeVarInt(client_fd, clock_update_count));

  PACKET_WRITE(writeVarInt(client_fd, overworld_clock_id));
  PACKET_WRITE(writeVarLong(client_fd, ticks));
  PACKET_WRITE(writeFloat(client_fd, 0.0f));
  PACKET_WRITE(writeFloat(client_fd, 1.0f));

  PACKET_WRITE(writeVarInt(client_fd, the_end_clock_id));
  PACKET_WRITE(writeVarLong(client_fd, ticks));
  PACKET_WRITE(writeFloat(client_fd, 0.0f));
  PACKET_WRITE(writeFloat(client_fd, 1.0f));

  #undef PACKET_WRITE
  return 0;
}

// 服务端到客户端 Game Event 13 (Start waiting for level 区块)
int sc_startWaitingForChunks (int client_fd) {
  debug_last_out_packet_id = 0x26;
  debug_last_out_packet_length = 6;
  debug_last_out_stage = "start_waiting_chunks";
  writeVarInt(client_fd, 6);
  writeByte(client_fd, 0x26);
  writeByte(client_fd, 13);
  writeUint32(client_fd, 0);
  return 0;
}

// 服务端到客户端 Set 中心 区块
int sc_setCenterChunk (int client_fd, int x, int y) {
  debug_last_out_packet_id = 0x5E;
  debug_last_out_packet_length = 1 + sizeVarInt(x) + sizeVarInt(y);
  debug_last_out_stage = "set_center_chunk";
  writeVarInt(client_fd, 1 + sizeVarInt(x) + sizeVarInt(y));
  writeByte(client_fd, 0x5E);
  writeVarInt(client_fd, x);
  writeVarInt(client_fd, y);
  return 0;
}

// 服务端到客户端 区块 数据 and 更新 光照
int sc_chunkDataAndUpdateLight (int client_fd, int _x, int _z) {

  const int bedrock_section_size = 4 + 1 + sizeVarInt(block_palette[B_bedrock]) + 1 + sizeVarInt(0);
  const int air_section_size = 4 + 1 + sizeVarInt(block_palette[B_air]) + 1 + sizeVarInt(0);
  const int generated_section_count = GENERATED_CHUNK_SECTION_COUNT; // y = 0..95
  const int empty_section_count_above = TOTAL_CHUNK_SECTION_COUNT - EMPTY_SECTIONS_BELOW_ZERO - generated_section_count;
  const uint64_t sky_light_mask = ((1ULL << SKY_LIGHT_SECTION_COUNT) - 1) & ~((1ULL << DARK_SKY_LIGHT_SECTION_COUNT) - 1);
  const uint64_t empty_sky_light_mask = (1ULL << DARK_SKY_LIGHT_SECTION_COUNT) - 1;
  const int bright_sky_light_section_count = SKY_LIGHT_SECTION_COUNT - DARK_SKY_LIGHT_SECTION_COUNT;
  const int light_mask_size = sizeVarInt(1) + sizeof(uint64_t);
  const int light_data_size =
    light_mask_size + // sky light mask
    sizeVarInt(0) +   // block light mask
    light_mask_size + // 空 sky light mask
    sizeVarInt(0) +   // 空 block light mask
    sizeVarInt(bright_sky_light_section_count) +
    (sizeVarInt(2048) + 2048) * bright_sky_light_section_count +
    sizeVarInt(0);    // block light 数组数
  ChunkPacketWriter writer = { .client_fd = client_fd, .used = 0, .failed = 0 };
  int chunk_data_size = bedrock_section_size * EMPTY_SECTIONS_BELOW_ZERO + air_section_size * empty_section_count_above;
  int x = _x * 16, z = _z * 16;

  #define CHUNK_WRITE(call) do { if ((call) == -1) return -1; } while (0)

  for (int i = 0; i < generated_section_count; i ++) {
    uint8_t biome = buildChunkSection(x, i * 16, z);
    chunk_section_encoded_len[i] = (uint16_t)encodeChunkSection(
      chunk_section_encoded[i], chunk_section, biome
    );
    chunk_data_size += chunk_section_encoded_len[i];
    task_yield();
  }

  debug_last_out_packet_id = 0x2D;
  debug_last_out_packet_length = 11 + sizeVarInt(chunk_data_size) + chunk_data_size + light_data_size;
  debug_last_out_stage = "chunk_data";

  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 11 + sizeVarInt(chunk_data_size) + chunk_data_size + light_data_size));
  CHUNK_WRITE(chunkPacketWriteByte(&writer, 0x2D));

  CHUNK_WRITE(chunkPacketWriteUint32(&writer, _x));
  CHUNK_WRITE(chunkPacketWriteUint32(&writer, _z));

  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0)); // 不发 heightmaps

  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, chunk_data_size));

  // Y=0 以下全发 bedrock
  for (int i = 0; i < EMPTY_SECTIONS_BELOW_ZERO; i ++) {
    CHUNK_WRITE(chunkPacketWriteUint16(&writer, 4096)); // 方块数
    CHUNK_WRITE(chunkPacketWriteUint16(&writer, 0)); // 流体数
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // 方块位数
    CHUNK_WRITE(chunkPacketWriteVarInt(&writer, block_palette[B_bedrock])); // 方块调色板 bedrock
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // biome 位数
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // biome 调色板
  }
  // 让出给 idle task
  task_yield();

  // 发可生成地形的 section
  for (int i = 0; i < generated_section_count; i ++) {
    CHUNK_WRITE(chunkPacketWrite(&writer, chunk_section_encoded[i], chunk_section_encoded_len[i]));
    // 让出给 idle task
    task_yield();
  }

  // y=80 以上基础地形只有空气
  for (int i = 0; i < empty_section_count_above; i ++) {
    CHUNK_WRITE(chunkPacketWriteUint16(&writer, 0)); // 方块数
    CHUNK_WRITE(chunkPacketWriteUint16(&writer, 0)); // 流体数
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // 方块位数
    CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0)); // 方块调色板 air
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // biome 位数
    CHUNK_WRITE(chunkPacketWriteByte(&writer, 0)); // biome 调色板
  }
  task_yield();

  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0)); // 不发方块实体

  // 光照数据
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 1));
  CHUNK_WRITE(chunkPacketWriteUint64(&writer, sky_light_mask));
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0));
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 1));
  CHUNK_WRITE(chunkPacketWriteUint64(&writer, empty_sky_light_mask));
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0));

  // sky light 数组
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, bright_sky_light_section_count));
  for (int i = 0; i < 2048; i ++) chunk_section[i] = 0xFF;
  for (int i = 2048; i < 4096; i ++) chunk_section[i] = 0;
  for (int i = 0; i < bright_sky_light_section_count; i ++) {
    CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 2048));
    CHUNK_WRITE(chunkPacketWrite(&writer, chunk_section, 2048));
  }
  // 不发 block light
  CHUNK_WRITE(chunkPacketWriteVarInt(&writer, 0));

  CHUNK_WRITE(chunkPacketFlush(&writer));

  // 基础区块发完后再盖玩家改动
  // 这样区块生成保持固定，也不用每个 section 重扫改动表
  for (int i = 0; i < block_changes_count; i ++) {
    uint8_t block = block_changes[i].block;
    if (block == 0xFF) continue;
    if (
      block_changes[i].x >= x && block_changes[i].x < x + 16 &&
      block_changes[i].z >= z && block_changes[i].z < z + 16
    ) {
      if (sc_blockUpdate(client_fd, block_changes[i].x, block_changes[i].y, block_changes[i].z, block) == -1) return -1;
    }
    #ifdef ALLOW_CHESTS
      if (block == B_chest) i += 14;
    #endif
  }

  #undef CHUNK_WRITE
  return 0;

}

// 服务端到客户端 保活 (play)
int sc_keepAlive (int client_fd) {
  #define PACKET_WRITE(call) do { if ((call) == -1) return -1; } while (0)

  PACKET_WRITE(writeVarInt(client_fd, 9));
  PACKET_WRITE(writeByte(client_fd, 0x2C));

  PACKET_WRITE(writeUint64(client_fd, 0));

  #undef PACKET_WRITE
  return 0;
}

// 服务端到客户端 Set Container Slot
int sc_setContainerSlot (int client_fd, int window_id, uint16_t slot, uint8_t count, uint16_t item) {
  debug_last_out_packet_id = 0x14;
  debug_last_out_packet_length =
    1 +
    sizeVarInt(window_id) +
    1 + 2 +
    sizeVarInt(count) +
    (count > 0 ? sizeVarInt(item) + 2 : 0);
  debug_last_out_stage = "set_container_slot";

  writeVarInt(client_fd,
    1 +
    sizeVarInt(window_id) +
    1 + 2 +
    sizeVarInt(count) +
    (count > 0 ? sizeVarInt(item) + 2 : 0)
  );
  writeByte(client_fd, 0x14);

  writeVarInt(client_fd, window_id);
  writeVarInt(client_fd, 0);
  writeUint16(client_fd, slot);

  writeVarInt(client_fd, count);
  if (count > 0) {
    writeVarInt(client_fd, item);
    writeVarInt(client_fd, 0);
    writeVarInt(client_fd, 0);
  }

  return 0;

}

// 服务端到客户端 方块 更新
int sc_blockUpdate (int client_fd, int64_t x, int64_t y, int64_t z, uint8_t block) {
  writeVarInt(client_fd, 9 + sizeVarInt(block_palette[block]));
  writeByte(client_fd, 0x08);
  writeUint64(client_fd, ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF));
  writeVarInt(client_fd, block_palette[block]);
  return 0;
}

// 服务端到客户端 Acknowledge 方块 Change
int sc_acknowledgeBlockChange (int client_fd, int sequence) {
  writeVarInt(client_fd, 1 + sizeVarInt(sequence));
  writeByte(client_fd, 0x04);
  writeVarInt(client_fd, sequence);
  return 0;
}

// 客户端到服务端 玩家 Action
int cs_playerAction (int client_fd) {

  uint8_t action = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  readByte(client_fd); // 忽略 face

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerAction(player, action, x, y, z);

  return 0;

}

// 服务端到客户端 打开 Screen
int sc_openScreen (int client_fd, uint8_t window, const char *title, uint16_t length) {

  writeVarInt(client_fd, 1 + 2 * sizeVarInt(window) + 1 + 2 + length);
  writeByte(client_fd, 0x3B);

  writeVarInt(client_fd, window);
  writeVarInt(client_fd, window);

  writeByte(client_fd, 8); // string nbt tag
  writeUint16(client_fd, length); // string 长度
  send_all(client_fd, title, length);

  return 0;
}

// 客户端到服务端 Use Item
int cs_useItem (int client_fd) {

  uint8_t hand = readByte(client_fd);
  int sequence = readVarInt(client_fd);

  // 忽略 yaw/pitch
  recv_all(client_fd, recv_buffer, 8, false);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerUseItem(player, 0, 0, 0, 255);

  return 0;
}

// 客户端到服务端 Use Item On
int cs_useItemOn (int client_fd) {

  uint8_t hand = readByte(client_fd);

  int64_t pos = readInt64(client_fd);
  int x = pos >> 38;
  int y = pos << 52 >> 52;
  int z = pos << 26 >> 38;

  uint8_t face = readByte(client_fd);

  // 忽略光标坐标
  readUint32(client_fd);
  readUint32(client_fd);
  readUint32(client_fd);

  // 忽略 inside block 和 world border hit
  readByte(client_fd);
  readByte(client_fd);

  int sequence = readVarInt(client_fd);
  sc_acknowledgeBlockChange(client_fd, sequence);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  handlePlayerUseItem(player, x, y, z, face);

  return 0;
}

// 客户端到服务端 Click Container
int cs_clickContainer (int client_fd) {

  int window_id = readVarInt(client_fd);

  readVarInt(client_fd); // 忽略 state ID

  int16_t clicked_slot = readInt16(client_fd);
  uint8_t button = readByte(client_fd);
  uint8_t mode = readVarInt(client_fd);

  int changes_count = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t apply_changes = true;
  // 不让丢物品
  if (mode == 4 && clicked_slot != -999) {
    // 点丢弃键就把槽位同步回去
    uint8_t slot = clientSlotToServerSlot(window_id, clicked_slot);
    sc_setContainerSlot(client_fd, window_id, clicked_slot, player->inventory_count[slot], player->inventory_items[slot]);
    apply_changes = false;
  } else if (mode == 0 && clicked_slot == -999) {
    // 点背包外面就把物品退回玩家
    if (button == 0) {
      givePlayerItem(player, player->flagval_16, player->flagval_8);
      player->flagval_16 = 0;
      player->flagval_8 = 0;
    } else {
      givePlayerItem(player, player->flagval_16, 1);
      player->flagval_8 -= 1;
      if (player->flagval_8 == 0) player->flagval_16 = 0;
    }
    apply_changes = false;
  }

  uint8_t slot, count, craft = false;
  uint16_t item;
  int tmp;

  uint16_t *p_item;
  uint8_t *p_count;

  #ifdef ALLOW_CHESTS
  // 这个 hack 见 handlePlayerUseItem
  uint8_t *storage_ptr;
  memcpy(&storage_ptr, player->craft_items, sizeof(storage_ptr));
  #endif

  for (int i = 0; i < changes_count; i ++) {

    slot = clientSlotToServerSlot(window_id, readUint16(client_fd));
    // 背包外的槽位溢出到 crafting buffer
    if (slot > 40 && apply_changes) craft = true;

    #ifdef ALLOW_CHESTS
    if (window_id == 2 && slot > 40) {
      // 从玩家的 storage 指针取槽位
      // 这个 hack 见 handlePlayerUseItem
      p_item = (uint16_t *)(storage_ptr + (slot - 41) * 3);
      p_count = storage_ptr + (slot - 41) * 3 + 2;
    } else
    #endif
    {
      // craft_items 锁住时不许碰这些槽位
      if (slot > 40 && player->flags & 0x80) return 1;
      p_item = &player->inventory_items[slot];
      p_count = &player->inventory_count[slot];
    }

    if (!readByte(client_fd)) { // 空物品
      if (slot != 255 && apply_changes) {
        *p_item = 0;
        *p_count = 0;
        #ifdef ALLOW_CHESTS
        if (window_id == 2 && slot > 40) {
          broadcastChestUpdate(client_fd, storage_ptr, 0, 0, slot - 41);
        }
        #endif
      }
      continue;
    }

    item = readVarInt(client_fd);
    count = (uint8_t)readVarInt(client_fd);

    // 忽略 components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);

    if (count > 0 && apply_changes) {
      *p_item = item;
      *p_count = count;
      #ifdef ALLOW_CHESTS
      if (window_id == 2 && slot > 40) {
        broadcastChestUpdate(client_fd, storage_ptr, item, count, slot - 41);
      }
      #endif
    }

  }

  // window 0 是玩家背包, 12 是 crafting table
  if (craft && (window_id == 0 || window_id == 12)) {
    getCraftingOutput(player, &count, &item);
    sc_setContainerSlot(client_fd, window_id, 0, count, item);
  } else if (window_id == 14) { // furnace
    getSmeltingOutput(player);
    for (int i = 0; i < 3; i ++) {
      sc_setContainerSlot(client_fd, window_id, i, player->craft_count[i], player->craft_items[i]);
    }
  }

  // 记录鼠标拿着的物品
  if (readByte(client_fd)) {
    player->flagval_16 = readVarInt(client_fd);
    player->flagval_8 = readVarInt(client_fd);
    // 忽略 components
    readLengthPrefixedData(client_fd);
    readLengthPrefixedData(client_fd);
  } else {
    player->flagval_16 = 0;
    player->flagval_8 = 0;
  }

  return 0;

}

// 服务端到客户端 Set Cursor Item
int sc_setCursorItem (int client_fd, uint16_t item, uint8_t count) {

  writeVarInt(client_fd, 1 + sizeVarInt(count) + (count != 0 ? sizeVarInt(item) + 2 : 0));
  writeByte(client_fd, 0x60);

  writeVarInt(client_fd, count);
  if (count == 0) return 0;

  writeVarInt(client_fd, item);

  // 不发 components
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);

  return 0;
}

// 客户端到服务端 Set 玩家 坐标 And 朝向
int cs_setPlayerPositionAndRotation (int client_fd, double *x, double *y, double *z, float *yaw, float *pitch, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// 客户端到服务端 Set 玩家 坐标
int cs_setPlayerPosition (int client_fd, double *x, double *y, double *z, uint8_t *on_ground) {

  *x = readDouble(client_fd);
  *y = readDouble(client_fd);
  *z = readDouble(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

// 客户端到服务端 Set 玩家 朝向
int cs_setPlayerRotation (int client_fd, float *yaw, float *pitch, uint8_t *on_ground) {

  *yaw = readFloat(client_fd);
  *pitch = readFloat(client_fd);

  *on_ground = readByte(client_fd) & 0x01;

  return 0;
}

int cs_setPlayerMovementFlags (int client_fd, uint8_t *on_ground) {

  *on_ground = readByte(client_fd) & 0x01;

  PlayerData *player;
  if (!getPlayerData(client_fd, &player))
    broadcastPlayerMetadata(player);

  return 0;
}

// 客户端到服务端 Swing Arm
int cs_swingArm (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t hand = readVarInt(client_fd);

  uint8_t animation = 255;
  switch (hand) {
    case 0: {
      animation = 0;
      break;
    }
    case 1: {
      animation = 2;
      break;
    }
  }

  if (animation == 255)
    return 1;

  // 转发动画给其他在线玩家
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    PlayerData* other_player = &player_data[i];

    if (other_player->client_fd == -1) continue;
    if (other_player->client_fd == player->client_fd) continue;
    if (other_player->flags & 0x20) continue;

    sc_entityAnimation(other_player->client_fd, player->client_fd, animation);
  }

  return 0;
}

// 客户端到服务端 Set Held Item
int cs_setHeldItem (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  uint8_t slot = readUint16(client_fd);
  if (slot >= 9) return 1;

  player->hotbar = slot;

  return 0;
}

// 服务端到客户端 Set Held Item
int sc_setHeldItem (int client_fd, uint8_t slot) {
  debug_last_out_packet_id = 0x69;
  debug_last_out_packet_length = sizeVarInt(0x69) + 1;
  debug_last_out_stage = "set_held_item";

  writeVarInt(client_fd, sizeVarInt(0x69) + 1);
  writeVarInt(client_fd, 0x69);

  writeByte(client_fd, slot);

  return 0;
}

// 客户端到服务端 关闭 Container
int cs_closeContainer (int client_fd) {

  uint8_t window_id = readVarInt(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // 合成格物品退回玩家
  // chest 的话只清 storage 指针
  for (uint8_t i = 0; i < 9; i ++) {
    if (window_id != 2) {
      givePlayerItem(player, player->craft_items[i], player->craft_count[i]);
      uint8_t client_slot = serverSlotToClientSlot(window_id, 41 + i);
      if (client_slot != 255) sc_setContainerSlot(player->client_fd, window_id, client_slot, 0, 0);
    }
    player->craft_items[i] = 0;
    player->craft_count[i] = 0;
    // 解锁 craft_items
    player->flags &= ~0x80;
  }

  givePlayerItem(player, player->flagval_16, player->flagval_8);
  sc_setCursorItem(client_fd, 0, 0);
  player->flagval_16 = 0;
  player->flagval_8 = 0;

  return 0;
}

// 服务端到客户端 玩家 Info 更新, "Add 玩家" action
int sc_playerInfoUpdateAddPlayer (int client_fd, PlayerData player) {

  writeVarInt(client_fd, 21 + strlen(player.name)); // 包长度
  writeByte(client_fd, 0x46); // 包 ID

  writeByte(client_fd, 0x01); // EnumSet: Add Player
  writeByte(client_fd, 1); // 玩家数

  // 玩家 UUID
  send_all(client_fd, player.uuid, 16);
  // 玩家名
  writeByte(client_fd, strlen(player.name));
  send_all(client_fd, player.name, strlen(player.name));
  // properties
  writeByte(client_fd, 0);

  return 0;
}

// 服务端到客户端 生成 实体
int sc_spawnEntity (
  int client_fd,
  int id, uint8_t *uuid, int type,
  double x, double y, double z,
  uint8_t yaw, uint8_t pitch
) {

  const int data = 0;
  const int packet_length = 45 + sizeVarInt(id) + sizeVarInt(type) + sizeVarInt(data);

  debug_last_out_packet_id = 0x01;
  debug_last_out_packet_length = packet_length;
  debug_last_out_stage = "add_entity";

  writeVarInt(client_fd, packet_length);
  writeByte(client_fd, 0x01);

  writeVarInt(client_fd, id); // 实体 ID
  send_all(client_fd, uuid, 16); // 实体 UUID
  writeVarInt(client_fd, type); // 实体类型

  // 坐标
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);

  // 26.1.2 里零速度就是 1 个零字节
  writeByte(client_fd, 0);

  // 朝向: xRot, yRot, yHeadRot
  writeByte(client_fd, pitch);
  writeByte(client_fd, yaw);
  writeByte(client_fd, yaw);

  // data
  writeVarInt(client_fd, data);

  return 0;
}

// 服务端到客户端 Set 实体元数据
int sc_setEntityMetadata (int client_fd, int id, EntityData *metadata, size_t length) {
  int entity_metadata_size = sizeEntityMetadata(metadata, length);
  if (entity_metadata_size == -1) return 1;

  writeVarInt(client_fd, 2 + sizeVarInt(id) + entity_metadata_size);
  writeByte(client_fd, 0x63);

  writeVarInt(client_fd, id); // 实体 ID

  for (size_t i = 0; i < length; i ++) {
    EntityData *data = &metadata[i];
    writeEntityData(client_fd, data);
  }

  writeByte(client_fd, 0xFF); // 结束

  return 0;
}

// 服务端到客户端 生成 实体 (from PlayerData)
int sc_spawnEntityPlayer (int client_fd, PlayerData player) {
  return sc_spawnEntity(
    client_fd,
    // 26.1.2 里 minecraft:player = 155
    player.client_fd, player.uuid, 155,
    player.x > 0 ? (double)player.x + 0.5 : (double)player.x - 0.5,
    player.y,
    player.z > 0 ? (double)player.z + 0.5 : (float)player.z - 0.5,
    player.yaw, player.pitch
  );
}

// 服务端到客户端 实体 Animation
int sc_entityAnimation (int client_fd, int id, uint8_t animation) {
  writeVarInt(client_fd, 2 + sizeVarInt(id));
  writeByte(client_fd, 0x02);

  writeVarInt(client_fd, id); // 实体 ID
  writeByte(client_fd, animation); // 动画

  return 0;
}

// 服务端到客户端 传送实体
int sc_teleportEntity (
  int client_fd, int id,
  double x, double y, double z,
  float yaw, float pitch
) {

  // 包长度和 ID
  writeVarInt(client_fd, 62 + sizeVarInt(id));
  writeByte(client_fd, 0x7D);

  // 实体 ID
  writeVarInt(client_fd, id);
  // 坐标
  writeDouble(client_fd, x);
  writeDouble(client_fd, y);
  writeDouble(client_fd, z);
  // 速度
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  writeUint64(client_fd, 0);
  // 朝向
  writeFloat(client_fd, yaw);
  writeFloat(client_fd, pitch);
  // 落地标记
  writeByte(client_fd, 1);
  // 26.1.2 的 teleport timing 字段
  writeUint32(client_fd, 0);

  return 0;
}

// 服务端到客户端 Set Head 朝向
int sc_setHeadRotation (int client_fd, int id, uint8_t yaw) {

  // 包长度和 ID
  writeByte(client_fd, 2 + sizeVarInt(id));
  writeByte(client_fd, 0x53);
  // 实体 ID
  writeVarInt(client_fd, id);
  // 头部 yaw
  writeByte(client_fd, yaw);

  return 0;
}

// 服务端到客户端 Set Head 朝向
int sc_updateEntityRotation (int client_fd, int id, uint8_t yaw, uint8_t pitch) {

  // 包长度和 ID
  writeByte(client_fd, 4 + sizeVarInt(id));
  writeByte(client_fd, 0x38);
  // 实体 ID
  writeVarInt(client_fd, id);
  // 朝向
  writeByte(client_fd, yaw);
  writeByte(client_fd, pitch);
  // 落地标记
  writeByte(client_fd, 1);

  return 0;
}

// 服务端到客户端 伤害 Event
int sc_damageEvent (int client_fd, int entity_id, int type) {

  writeVarInt(client_fd, 4 + sizeVarInt(entity_id) + sizeVarInt(type));
  writeByte(client_fd, 0x19);

  writeVarInt(client_fd, entity_id);
  writeVarInt(client_fd, type);
  writeByte(client_fd, 0);
  writeByte(client_fd, 0);
  writeByte(client_fd, false);

  return 0;
}

// 服务端到客户端 Set 生命
int sc_setHealth (int client_fd, uint8_t health, uint8_t food, uint16_t saturation) {
  debug_last_out_packet_id = 0x68;
  debug_last_out_packet_length = 9 + sizeVarInt(food);
  debug_last_out_stage = "set_health";

  writeVarInt(client_fd, 9 + sizeVarInt(food));
  writeByte(client_fd, 0x68);

  writeFloat(client_fd, (float)health);
  writeVarInt(client_fd, food);
  writeFloat(client_fd, (float)(saturation - 200) / 500.0f);

  return 0;
}

// 服务端到客户端 重生
int sc_respawn (int client_fd) {

  writeVarInt(client_fd, 28);
  writeByte(client_fd, 0x52);

  // 维度 ID
  writeVarInt(client_fd, 0);
  // 维度名
  const char *dimension = "overworld";
  writeVarInt(client_fd, 9);
  send_all(client_fd, dimension, 9);
  // 哈希种子
  writeUint64(client_fd, 0x0123456789ABCDEF);
  // 游戏模式
  writeByte(client_fd, GAMEMODE);
  // 上个游戏模式
  writeByte(client_fd, 0xFF);
  // debug 世界
  writeByte(client_fd, 0);
  // flat 世界
  writeByte(client_fd, 0);
  // 死亡位置
  writeByte(client_fd, 0);
  // 传送门冷却
  writeVarInt(client_fd, 0);
  // 海平面
  writeVarInt(client_fd, 63);
  // 保留数据
  writeByte(client_fd, 0);

  return 0;
}

// 客户端到服务端 Client Status
int cs_clientStatus (int client_fd) {

  uint8_t id = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  if (id == 0) {
    sc_respawn(client_fd);
    resetPlayerData(player);
    spawnPlayer(player);
  }

  return 0;
}

// 服务端到客户端 System 聊天
int sc_systemChat (int client_fd, char* message, uint16_t len) {

  writeVarInt(client_fd, 5 + len);
  writeByte(client_fd, 0x79);

  // String NBT tag
  writeByte(client_fd, 8);
  writeUint16(client_fd, len);
  send_all(client_fd, message, len);

  // 是不是 action bar
  writeByte(client_fd, false);

  return 0;
}

// 客户端到服务端 聊天 消息
int cs_chat (int client_fd) {

  // 先截一下，别把缓冲区顶满
  readStringN(client_fd, 224);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  size_t message_len = strlen((char *)recv_buffer);
  uint8_t name_len = strlen(player->name);

  if (recv_buffer[0] != '!') { // 普通聊天

    // 消息后挪，给玩家名前缀腾位置
    memmove(recv_buffer + name_len + 3, recv_buffer, message_len + 1);
    // 玩家名写到索引 1
    memcpy(recv_buffer + 1, player->name, name_len);
    // 补上尖括号和空格
    recv_buffer[0] = '<';
    recv_buffer[name_len + 1] = '>';
    recv_buffer[name_len + 2] = ' ';

    // 转发给所有在线玩家
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (player_data[i].client_fd == -1) continue;
      if (player_data[i].flags & 0x20) continue;
      sc_systemChat(player_data[i].client_fd, (char *)recv_buffer, message_len + name_len + 3);
    }

    goto cleanup;
  }

  // 处理聊天命令

  if (!strncmp((char *)recv_buffer, "!msg", 4)) {

    int target_offset = 5;
    int target_end_offset = 0;
    int text_offset = 0;

    // 跳过 !msg 后面的空格
    while (recv_buffer[target_offset] == ' ') target_offset++;
    target_end_offset = target_offset;
    // 取目标名字
    while (recv_buffer[target_end_offset] != ' ' && recv_buffer[target_end_offset] != '\0' && target_end_offset < 21) target_end_offset++;
    text_offset = target_end_offset;
    // 跳过消息前空格
    while (recv_buffer[text_offset] == ' ') text_offset++;

    // 缺参数就回用法
    if (target_offset == target_end_offset || target_end_offset == text_offset) {
      sc_systemChat(client_fd, "§7Usage: !msg <player> <message>", 33);
      goto cleanup;
    }

    // 找目标玩家
    PlayerData *target = getPlayerByName(target_offset, target_end_offset, recv_buffer);
    if (target == NULL) {
      sc_systemChat(client_fd, "Player not found", 16);
      goto cleanup;
    }

    // 拼成原版私聊格式
    int name_len = strlen(player->name);
    int text_len = message_len - text_offset;
    memmove(recv_buffer + name_len + 24, recv_buffer + text_offset, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§o%s whispers to you:", player->name);
    recv_buffer[name_len + 23] = ' ';
    // 发给目标玩家
    sc_systemChat(target->client_fd, (char *)recv_buffer, (uint16_t)(name_len + 24 + text_len));

    // 拼回给发送者的提示
    int target_len = target_end_offset - target_offset;
    memmove(recv_buffer + target_len + 23, recv_buffer + name_len + 24, text_len);
    snprintf((char *)recv_buffer, sizeof(recv_buffer), "§7§oYou whisper to %s:", target->name);
    recv_buffer[target_len + 22] = ' ';
    // 回给发送者
    sc_systemChat(client_fd, (char *)recv_buffer, (uint16_t)(target_len + 23 + text_len));

    goto cleanup;
  }

  if (!strncmp((char *)recv_buffer, "!help", 5)) {
    // 发命令帮助
    const char help_msg[] = "§7Commands:\n"
    "  !msg <player> <message> - Send a private message\n"
    "  !help - Show this help message";
    sc_systemChat(client_fd, (char *)help_msg, (uint16_t)sizeof(help_msg) - 1);
    goto cleanup;
  }

  // 兜底
  sc_systemChat(client_fd, "§7Unknown command", 18);

cleanup:
  readUint64(client_fd); // 忽略 timestamp
  readUint64(client_fd); // 忽略 salt
  // 忽略 signature
  uint8_t has_signature = readByte(client_fd);
  if (has_signature) recv_all(client_fd, recv_buffer, 256, false);
  readVarInt(client_fd); // 忽略消息计数
  // 忽略 ack 位图和校验
  recv_all(client_fd, recv_buffer, 4, false);

  return 0;
}

// 客户端到服务端 Interact
int cs_attack (int client_fd) {

  int entity_id = readVarInt(client_fd);
  hurtEntity(entity_id, client_fd, D_generic, 1);

  return 0;
}

// 客户端到服务端 Interact
int cs_interact (int client_fd) {

  int entity_id = readVarInt(client_fd);
  uint8_t type = readByte(client_fd);

  if (type == 2) {
    // 忽略目标坐标
    recv_all(client_fd, recv_buffer, 12, false);
  }
  if (type != 1) {
    // 忽略 hand
    recv_all(client_fd, recv_buffer, 1, false);
  }

  // 忽略潜行标记
  recv_all(client_fd, recv_buffer, 1, false);

  if (type == 0) { // Interact
    interactEntity(entity_id, client_fd);
  } else if (type == 1) { // Attack
    hurtEntity(entity_id, client_fd, D_generic, 1);
  }

  return 0;
}

// 服务端到客户端 实体 Event
int sc_entityEvent (int client_fd, int entity_id, uint8_t status) {

  writeVarInt(client_fd, 6);
  writeByte(client_fd, 0x22);

  writeUint32(client_fd, entity_id);
  writeByte(client_fd, status);

  return 0;
}

// 服务端到客户端 Remove 实体
int sc_removeEntity (int client_fd, int entity_id) {

  writeVarInt(client_fd, 2 + sizeVarInt(entity_id));
  writeByte(client_fd, 0x4D);

  writeByte(client_fd, 1);
  writeVarInt(client_fd, entity_id);

  return 0;
}

// 客户端到服务端 玩家 Input
int cs_playerInput (int client_fd) {

  uint8_t flags = readByte(client_fd);

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // 开关潜行标记
  if (flags & 0x20) player->flags |= 0x04;
  else player->flags &= ~0x04;

  broadcastPlayerMetadata(player);

  return 0;
}

// 客户端到服务端 玩家 Command
int cs_playerCommand (int client_fd) {

  readVarInt(client_fd); // 忽略实体 ID
  uint8_t action = readByte(client_fd);
  readVarInt(client_fd); // 忽略 Jump Boost 值

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // 处理疾跑
  if (action == 1) player->flags |= 0x08;
  else if (action == 2) player->flags &= ~0x08;

  broadcastPlayerMetadata(player);

  return 0;
}

// 服务端到客户端 Pickup Item (take_item_entity)
int sc_pickupItem (int client_fd, int collected, int collector, uint8_t count) {

  writeVarInt(client_fd, 1 + sizeVarInt(collected) + sizeVarInt(collector) + sizeVarInt(count));
  writeByte(client_fd, 0x7C);

  writeVarInt(client_fd, collected);
  writeVarInt(client_fd, collector);
  writeVarInt(client_fd, count);

  return 0;
}

// 客户端到服务端 玩家 Loaded
int cs_playerLoaded (int client_fd) {

  PlayerData *player;
  if (getPlayerData(client_fd, &player)) return 1;

  // 转给玩家 join 流程
  handlePlayerJoin(player);

  return 0;
}

// 服务端到客户端 Registry 数据和更新 Tags
int sc_registries (int client_fd) {

  printf("Sending Registries\n\n");
  send_all(client_fd, registries_bin, sizeof(registries_bin));

  printf("Sending Tags\n\n");
  send_all(client_fd, tags_bin, sizeof(tags_bin));

  return 0;

}
