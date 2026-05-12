#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifdef ARDUINO_PLATFORM
  // 网络接口都走 arduino_compat.h
  #include "globals.h"
#elif defined(ESP_PLATFORM)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "nvs_flash.h"
  #include "esp_wifi.h"
  #include "esp_event.h"
  #include "esp_timer.h"
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
#else
  #include <sys/types.h>
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
#endif

#ifndef ARDUINO_PLATFORM
  #include "globals.h"
#endif
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"

#define ACTIVE_VIEW_DISTANCE VIEW_DISTANCE

// 按状态把包分发给处理函数
void handlePacket (int client_fd, int length, int packet_id, int state) {

  // 记下起始收包字节数
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        uint8_t uuid[16];
        char name[16];
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        if (sc_loginSuccess(client_fd, uuid, name)) break;
      } else if (state == STATE_CONFIGURATION) {
        if (cs_clientInformation(client_fd)) break;
      }
      break;

    case 0x01:
      // 处理状态查询 ping
      if (state == STATE_STATUS) {
        // 原样回包
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, readUint64(client_fd));
        // 回完后断开
        recv_count = 0;
        return;
      } else if (state == STATE_PLAY) {
        cs_attack(client_fd);
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        printf("Client Acknowledged Login\n\n");
        setClientState(client_fd, STATE_CONFIGURATION);

        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
        if (sc_updateEnabledFeatures(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;
      } else if (state == STATE_CONFIGURATION) {
        printf("Client Acknowledged Configuration\n\n");

        // 切到 play 状态
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        // 发送完整出生流程
        spawnPlayer(player);

        // 把现有玩家同步给他
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          // 跳过仍在加载的玩家
          if (player_data[i].flags & 0x20) continue;
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        // 再把现有 Mob 发给他
        // UUID 前半段写随机数
        uint8_t uuid[16];
        uint32_t r = fast_rand();
        memcpy(uuid, &r, 4);
        // 活着的 Mob 全部补发, 后半段写 Mob ID
        for (int i = 0; i < MAX_MOBS; i ++) {
          if (mob_data[i].type == 0) continue;
          if ((mob_data[i].data & 31) == 0) continue;
          memcpy(uuid + 4, &i, 4);
          // 参数顺序见 spawnMob
          sc_spawnEntity(
            client_fd, -2 - i, uuid,
            mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
            0, 0
          );
          broadcastMobMetadata(client_fd, -2 - i);
        }

      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        if (cs_selectKnownPacks(client_fd)) break;
        printf("  Synchronizing registries and tags\n\n");
        if (sc_registries(client_fd)) break;
        printf("  Finishing configuration\n\n");
        sc_finishConfiguration(client_fd);
      }
      break;

    case 0x09:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) {
        // 区块 batch, 忽略
        discard_all(client_fd, length, false);
      }
      break;

    case 0x0C:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0D: // Client Tick, 忽略
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x13:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1C:
      if (state == STATE_PLAY) {
        // 客户端 keep-alive, 忽略
        discard_all(client_fd, length, false);
      }
      break;

    case 0x1A:
      if (state == STATE_PLAY) cs_interact(client_fd);
      break;

    case 0x20:
    case 0x21:
    case 0x1E:
    case 0x1F:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        // 读取玩家坐标和朝向
        if (packet_id == 0x1E) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x21) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        // 处理摔落伤害
        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
        }

        // 只有标志位时到此结束
        if (packet_id == 0x21) break;

        // 更新玩家朝向
        if (packet_id != 0x1E) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        // 判断是否广播给其他人
        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          // 不开全量广播时
          // 移动更新改绑到 Tick 节奏
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
          // 按玩家数降频广播
          if (++player->packets_since_update < client_count) {
            should_broadcast = false;
          } else {
            // 这里故意不强行改回 true
            player->packets_since_update = 0;
          }
        #endif

        if (should_broadcast) {
          // 这包没带朝向时从玩家数据补
          if (packet_id == 0x1E) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          // 广播当前位置
          for (int i = 0; i < MAX_PLAYERS; i ++) {
            if (player_data[i].client_fd == -1) continue;
            if (player_data[i].flags & 0x20) continue;
            if (player_data[i].client_fd == client_fd) continue;
            if (packet_id == 0x20) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              sc_teleportEntity(player_data[i].client_fd, client_fd, x, y, z, yaw, pitch);
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        // 只有朝向时到此结束
        if (packet_id == 0x20) break;

        // 用移动包频率近似模拟饱和度消耗
        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        int player_index = (int)(player - player_data);

        // 转成整数坐标
        short cx = x, cy = y, cz = z;
        short prev_chunk_x = div_floor(player->x, 16);
        short prev_chunk_z = div_floor(player->z, 16);
        // 计算玩家当前区块
        short _x = div_floor(cx, 16), _z = div_floor(cz, 16);
        // 计算区块位移
        short dx = _x - prev_chunk_x;
        short dz = _z - prev_chunk_z;

        // 限制玩家高度范围
        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        // 同区块内移动时直接写坐标
        if (dx == 0 && dz == 0) {
          player->x = cx;
          player->y = cy;
          player->z = cz;
          break;
        }

        // 写回玩家坐标
        player->x = cx;
        player->y = cy;
        player->z = cz;

        // 检查最近是否来过这个区块
        int found = false;
        for (int i = 0; i < VISITED_HISTORY; i ++) {
          if (player->visited_x[i] == _x && player->visited_z[i] == _z) {
            found = true;
            break;
          }
        }
        if (found) break;

        // 更新最近访问区块
        for (int i = 0; i < VISITED_HISTORY - 1; i ++) {
          player->visited_x[i] = player->visited_x[i + 1];
          player->visited_z[i] = player->visited_z[i + 1];
        }
        player->visited_x[VISITED_HISTORY - 1] = _x;
        player->visited_z[VISITED_HISTORY - 1] = _z;

        uint32_t r = fast_rand();
        // 大约每 4 个新区块刷 1 次 Mob
        if ((r & 3) == 0) {
          short edge_dx = dx == 0 ? 0 : (dx > 0 ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE);
          short edge_dz = dz == 0 ? 0 : (dz > 0 ? ACTIVE_VIEW_DISTANCE : -ACTIVE_VIEW_DISTANCE);
          // Mob 刷在新区块边缘一排
          // 区块内坐标随机
          short mob_x = (_x + edge_dx) * 16 + ((r >> 4) & 15);
          short mob_z = (_z + edge_dz) * 16 + ((r >> 8) & 15);
          // 从玩家附近高度开始往上找落点
          uint8_t mob_y = cy - 8;
          uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
          uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
          uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
          while (mob_y < 255) {
            if ( // 脚下实心, 脚和头顶可生成
              !isPassableBlock(b_low) &&
              isPassableSpawnBlock(b_mid) &&
              isPassableSpawnBlock(b_top)
            ) break;
            b_low = b_mid;
            b_mid = b_top;
            b_top = getBlockAt(mob_x, mob_y + 2, mob_z);
            mob_y ++;
          }
          if (mob_y != 255) {
            // 白天地上刷被动
            // 地下或夜里刷敌对
            if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
              uint32_t mob_choice = (r >> 12) & 3;
              if (mob_choice == 0) spawnMob(26, mob_x, mob_y, mob_z, 4); // Chicken
              else if (mob_choice == 1) spawnMob(30, mob_x, mob_y, mob_z, 10); // Cow
              else if (mob_choice == 2) spawnMob(100, mob_x, mob_y, mob_z, 10); // Pig
              else if (mob_choice == 3) spawnMob(111, mob_x, mob_y, mob_z, 8); // Sheep
            } else {
              spawnMob(150, mob_x, mob_y, mob_z, 20); // Zombie
            }
          }
        }

        int count = 0;
        #ifdef DEV_LOG_CHUNK_GENERATION
          printf("Sending new chunks (%d, %d)\n", _x, _z);
          clock_t start, end;
          start = clock();
        #endif

        sc_startWaitingForChunks(client_fd);
        sc_setCenterChunk(client_fd, _x, _z);
        queueDeferredMovementChunks(player_index, _x, _z, dx, dz);
        count = (abs(dx) + abs(dz)) * (ACTIVE_VIEW_DISTANCE * 2 + 1);

        #ifdef DEV_LOG_CHUNK_GENERATION
          end = clock();
          double total_ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
          printf("Generated %d chunks in %.0f ms (%.2f ms per chunk)\n", count, total_ms, total_ms / (double)count);
        #endif

      }
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2C:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x35:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;
	
    case 0x3F:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x29:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x42:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x43:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      #ifdef DEV_LOG_UNKNOWN_PACKETS
        printf("Unknown packet: 0x");
        if (packet_id < 16) printf("0");
        printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
      #endif
      discard_all(client_fd, length, false);
      break;

  }

  // 包长没对齐时补救
  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }

  #ifdef DEV_LOG_LENGTH_DISCREPANCY
  if (processed_length != 0) {
    printf("WARNING: Packet 0x");
    if (packet_id < 16) printf("0");
    printf("%X parsed incorrectly!\n  Expected: %d, parsed: %d\n\n", packet_id, length, processed_length);
  }
  #endif
  #ifdef DEV_LOG_UNKNOWN_PACKETS
  if (processed_length == 0) {
    printf("Unknown packet: 0x");
    if (packet_id < 16) printf("0");
    printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
  }
  #endif

}

int main () {
  #ifdef _WIN32 // 初始化 Windows Socket
    WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
      }
  #endif

  // 启动时先把种子再做一次 hash
  world_seed = splitmix64(world_seed);
  printf("World seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((world_seed >> (8 * i)) & 255));

  rng_seed = splitmix64(rng_seed);
  printf("\nRNG seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((rng_seed >> (8 * i)) & 255));
  printf("\n\n");

  // 先把方块改动表标成未分配
  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_changes[i].block = 0xFF;
  }

  // 初始化磁盘 / Flash 同步
  if (initSerializer()) exit(EXIT_FAILURE);

  // fd 和玩家引用先清成 -1
  int clients[MAX_PLAYERS], client_index = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i * 2] = -1;
    player_data[i].client_fd = -1;
  }

#ifdef ARDUINO_PLATFORM
  // Arduino 下走 WiFiServer
  arduino_server_begin();
  printf("Server listening on port %d...\n", PORT);
#else
  // 创建服务器 TCP socket
  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
#ifdef _WIN32
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
      (const char*)&opt, sizeof(opt)) < 0) {
#else
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif    
    perror("socket options failed");
    exit(EXIT_FAILURE);
  }

  // 绑定 IP 和端口
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  // 开始监听
  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  printf("Server listening on port %d...\n", PORT);

  // 设成非阻塞
  #ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(server_fd, FIONBIO, &mode) != 0) {
      fprintf(stderr, "Failed to set non-blocking mode\n");
      exit(EXIT_FAILURE);
    }
  #else
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  #endif
#endif // ARDUINO_PLATFORM

  // 上次 Tick 时间
  int64_t last_tick_time = get_program_time();

  // 主循环
  // 每轮处理一个客户端, 并尝试接收新连接
  while (true) {
    // 先让出执行权
    task_yield();

    // 尝试接收新连接
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] != -1) continue;
      #ifdef ARDUINO_PLATFORM
        clients[i] = arduino_accept();
      #else
        clients[i] = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      #endif
      // 新连接也设成非阻塞
      if (clients[i] != -1) {
        printf("New client, fd: %d\n", clients[i]);
        // Arduino 的小 fd 会复用, 清掉旧重复项
        for (int j = 0; j < MAX_PLAYERS; j ++) {
          if (j == i) continue;
          if (clients[j] != clients[i]) continue;
          clients[j] = -1;
        }
      #if !defined(ARDUINO_PLATFORM) && defined(_WIN32)
        u_long mode = 1;
        ioctlsocket(clients[i], FIONBIO, &mode);
      #elif !defined(ARDUINO_PLATFORM)
        int flags = fcntl(clients[i], F_GETFL, 0);
        fcntl(clients[i], F_SETFL, flags | O_NONBLOCK);
      #endif
        // Arduino 下 fd 会复用, 状态必须重置
        setClientState(clients[i], STATE_NONE);
        client_count ++;
      }
      break;
    }

    // 找下一个有效客户端
    client_index ++;
    if (client_index == MAX_PLAYERS) client_index = 0;
    if (clients[client_index] == -1) continue;

    // 跑周期事件
    int64_t time_since_last_tick = get_program_time() - last_tick_time;
    if (time_since_last_tick > TIME_BETWEEN_TICKS) {
      handleServerTick(time_since_last_tick);
      last_tick_time = get_program_time();
    }

    // 处理当前客户端
    int client_fd = clients[client_index];
    PlayerData *queued_player;
    int queued_player_index = -1;
    if (getPlayerData(client_fd, &queued_player) == 0) {
      queued_player_index = (int)(queued_player - player_data);
    }

    // 先看有没有数据可读
    #ifdef ARDUINO_PLATFORM
    {
      if (!arduino_is_connected(client_fd)) {
        if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
          clients[client_index] = -1;
        } else {
          disconnectClient(&clients[client_index], 1);
        }
        continue;
      }
      recv_count = arduino_available(client_fd);
      if (recv_count < 1) {
        if (!arduino_is_connected(client_fd)) {
          if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
            clients[client_index] = -1;
          } else {
            disconnectClient(&clients[client_index], 1);
          }
        } else if (queued_player_index != -1) {
          processDeferredChunkStreaming(queued_player, queued_player_index);
          if (queued_player->client_fd == -1) {
            clients[client_index] = -1;
          }
        }
        continue;
      }
    }
    #elif defined(_WIN32)
    recv_count = recv(client_fd, recv_buffer, 2, MSG_PEEK);
    if (recv_count == 0) {
      if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
        clients[client_index] = -1;
      } else {
        disconnectClient(&clients[client_index], 1);
      }
      continue;
    }
    if (recv_count == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        if (queued_player_index != -1) {
          processDeferredChunkStreaming(queued_player, queued_player_index);
          if (queued_player->client_fd == -1) {
            clients[client_index] = -1;
          }
        }
        continue; // 还没数据, 保持连接
      } else {
        if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
          clients[client_index] = -1;
        } else {
          disconnectClient(&clients[client_index], 1);
        }
        continue;
      }
    }
    #else
    recv_count = recv(client_fd, &recv_buffer, 2, MSG_PEEK);
    if (recv_count < 2) {
      if (recv_count == 0 || (recv_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
          clients[client_index] = -1;
        } else {
          disconnectClient(&clients[client_index], 1);
        }
      } else if (queued_player_index != -1) {
        processDeferredChunkStreaming(queued_player, queued_player_index);
        if (queued_player->client_fd == -1) {
          clients[client_index] = -1;
        }
      }
      continue;
    }
    #endif
    // 处理 0xBEEF / 0xFEED 导入导出
    #ifdef DEV_ENABLE_BEEF_DUMPS
    // BEEF: 导出世界后断开
    if (recv_buffer[0] == 0xBE && recv_buffer[1] == 0xEF && getClientState(client_fd) == STATE_NONE) {
      // 方块改动和玩家数据直接连续发送
      send_all(client_fd, block_changes, sizeof(block_changes));
      send_all(client_fd, player_data, sizeof(player_data));
      // 把连接里的剩余数据读空
      shutdown(client_fd, SHUT_WR);
      recv_all(client_fd, recv_buffer, sizeof(recv_buffer), false);
      // 断开客户端
      disconnectClient(&clients[client_index], 6);
      continue;
    }
    // FEED: 导入世界后断开
    if (recv_buffer[0] == 0xFE && recv_buffer[1] == 0xED && getClientState(client_fd) == STATE_NONE) {
      // 先读掉 0xFEED 两字节
      recv_all(client_fd, recv_buffer, 2, false);
      // 整块读入内存
      recv_all(client_fd, block_changes, sizeof(block_changes), false);
      recv_all(client_fd, player_data, sizeof(player_data), false);
      // 恢复 block_changes_count
      for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].block == B_chest) i += 14;
        if (i >= block_changes_count) block_changes_count = i + 1;
      }
      // 同步到磁盘
      writeBlockChangesToDisk(0, block_changes_count);
      writePlayerDataToDisk();
      // 断开客户端
      disconnectClient(&clients[client_index], 7);
      continue;
    }
    #endif

    // 更新调试上下文
    debug_last_client_fd = client_fd;
    debug_last_state = getClientState(client_fd);
    debug_last_packet_length = -1;
    debug_last_packet_id = -1;
    debug_last_payload_length = -1;

    // 读取包长度
    int length = readVarInt(client_fd);
    if (length == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 2);
      continue;
    }
    debug_last_packet_length = length;

    // 读取包 ID
    int packet_id = readVarInt(client_fd);
    if (packet_id == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 3);
      continue;
    }
    debug_last_packet_id = packet_id;

    // 读取客户端状态
    int state = getClientState(client_fd);
    debug_last_state = state;

    // 旧版 server list ping 直接断开
    if (state == STATE_NONE && length == 254 && packet_id == 122) {
      disconnectClient(&clients[client_index], 5);
      continue;
    }

    // 处理包内容
    debug_last_payload_length = length - sizeVarInt(packet_id);
    handlePacket(client_fd, debug_last_payload_length, packet_id, state);
    if (recv_count == 0) {
      if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
        clients[client_index] = -1;
      } else {
        if (state == STATE_STATUS && packet_id == 0x01) {
          disconnectClient(&clients[client_index], 8);
        } else {
          disconnectClient(&clients[client_index], 4);
        }
      }
      continue;
    }
    if (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
      if (queued_player_index == -1 && getClientState(client_fd) == STATE_NONE) {
        clients[client_index] = -1;
      } else {
        disconnectClient(&clients[client_index], 4);
      }
      continue;
    }
  }

  #ifndef ARDUINO_PLATFORM
  close(server_fd);
  #endif
 
  #ifdef _WIN32 // 清理 Windows Socket
    WSACleanup();
  #endif

  printf("Server closed.\n");

}

#ifdef ARDUINO_PLATFORM

// code.ino 里连上 WiFi 后会调这里
// 用 extern "C" 暴露给 C++ 链接器
#ifdef __cplusplus
extern "C" {
#endif
void esp32mc_start() {
  main();
}
#ifdef __cplusplus
}
#endif

#elif defined(ESP_PLATFORM)

void esp32mc_main (void *pvParameters) {
  main();
  vTaskDelete(NULL);
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    printf("Got IP, starting server...\n\n");
    xTaskCreate(esp32mc_main, "esp32mc", 4096, NULL, 5, NULL);
  }
}

void wifi_init () {
  nvs_flash_init();
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
  printf("WiFi credentials are no longer hardcoded. Configure WiFi before starting ESP-IDF build.\n");
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main () {
  esp_timer_early_init();
  wifi_init();
}

#endif
