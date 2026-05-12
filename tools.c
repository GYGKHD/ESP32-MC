#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef ARDUINO_PLATFORM
  // socket 接口都走 arduino_compat.h
  #include "globals.h"
#elif defined(ESP_PLATFORM)
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_timer.h"
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
#endif

#ifndef ARDUINO_PLATFORM
  #include "globals.h"
#endif
#include "varnum.h"
#include "procedures.h"
#include "tools.h"

#ifndef htonll
  static uint64_t htonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32);
  #else
    return value;
  #endif
  }
#endif

// recv_all 累计收包字节数
// 用来发现包长不匹配
uint64_t total_bytes_received = 0;

ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  char *p = buf;
  size_t total = 0;

  // 上次真正收到数据的时间
  // 用来判断超时
  int64_t last_update_time = get_program_time();

  debug_last_client_fd = client_fd;
  debug_io_expected = (int)n;
  debug_io_progress = 0;
  debug_io_require_first = require_first;
  debug_io_last_errno = 0;
  debug_io_last_result = 0;
  debug_io_wait_us = 0;

  // 要求首字节时先探测一次
  if (require_first) {
    ssize_t r = recv(client_fd, p, 1, MSG_PEEK);
    if (r <= 0) {
      debug_io_last_result = (int)r;
      debug_io_last_errno = errno;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; // 首字节还没到
      }
      return -1; // 出错或断开
    }
  }

  // 一直等到收满 n 字节
  while (total < n) {
    ssize_t r = recv(client_fd, p + total, n - total, 0);
    if (r < 0) {
      debug_io_last_result = (int)r;
      debug_io_last_errno = errno;
      debug_io_progress = (int)total;

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 处理网络超时
        int64_t wait_us = get_program_time() - last_update_time;
        debug_io_wait_us = wait_us;
        if (wait_us > NETWORK_TIMEOUT_TIME) {
          #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
          printf(
            "ERROR: recv timeout fd=%d state=%d expected=%d progress=%d wait_us=%lld errno=%d packet_len=%d packet_id=%d payload_len=%d require_first=%d\n",
            client_fd,
            getClientState(client_fd),
            debug_io_expected,
            debug_io_progress,
            (long long)debug_io_wait_us,
            debug_io_last_errno,
            debug_last_packet_length,
            debug_last_packet_id,
            debug_last_payload_length,
            debug_io_require_first
          );
          #endif
          disconnectClient(&client_fd, -1);
          return -1;
        }
        task_yield();
        continue;
      } else {
        #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
        printf(
          "ERROR: recv failed fd=%d state=%d expected=%d progress=%d errno=%d packet_len=%d packet_id=%d payload_len=%d\n",
          client_fd,
          getClientState(client_fd),
          debug_io_expected,
          debug_io_progress,
          debug_io_last_errno,
          debug_last_packet_length,
          debug_last_packet_id,
          debug_last_payload_length
        );
        #endif
        total_bytes_received += total;
        return -1; // 真错误
      }
    } else if (r == 0) {
      // 还没读满就断开了
      debug_io_last_result = 0;
      debug_io_progress = (int)total;
      total_bytes_received += total;
      return total;
    }
    total += r;
    debug_io_progress = (int)total;
    last_update_time = get_program_time();
  }

  total_bytes_received += total;
  return total; // 正好读满
}

ssize_t send_all (int client_fd, const void *buf, ssize_t len) {
  if (client_fd < 0) {
    errno = EBADF;
    return -1;
  }
  #ifdef ARDUINO_PLATFORM
  if (!arduino_fd_in_use(client_fd)) {
    errno = EBADF;
    return -1;
  }
  #endif

  // 一律按 uint8_t* 处理
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;
  int client_state = getClientState(client_fd);

  // 上次真正发出数据的时间
  // 用来判断超时
  int64_t last_update_time = get_program_time();
  int64_t timeout_limit_us = NETWORK_TIMEOUT_TIME;
  #ifdef ARDUINO_PLATFORM
  // 单核 Arduino 卡太久会拖住主循环
  // 这里把发送超时收紧一点
  timeout_limit_us = (client_state == STATE_PLAY) ? 300000 : 2000000;
  #endif

  debug_last_client_fd = client_fd;
  debug_io_expected = (int)len;
  debug_io_progress = 0;
  debug_io_require_first = 0;
  debug_io_last_errno = 0;
  debug_io_last_result = 0;
  debug_io_wait_us = 0;

  // 一直发到发完
  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) { // 发出去一部分
      sent += n;
      debug_io_progress = (int)sent;
      last_update_time = get_program_time();
      // 大包发送中途也让一下
      // 避免 WiFi 和其他客户端饿死
      if ((sent & 4095) == 0) task_yield();
      continue;
    }
    if (n == 0) { // 连接关了, 按错误处理
      errno = ECONNRESET;
      debug_io_last_result = 0;
      debug_io_last_errno = errno;
      disconnectClient(&client_fd, -3);
      return -1;
    }

    debug_io_last_result = (int)n;

    // 还不能发, 继续等
    #ifdef _WIN32 // 处理 Windows socket 超时
      int err = WSAGetLastError();
      debug_io_last_errno = err;
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
      debug_io_last_errno = errno;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      // 处理网络超时
      int64_t wait_us = get_program_time() - last_update_time;
      debug_io_wait_us = wait_us;
      if (wait_us > timeout_limit_us) {
        #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
        printf(
          "ERROR: send timeout fd=%d state=%d expected=%d progress=%d wait_us=%lld errno=%d packet_len=%d packet_id=%d payload_len=%d last_out_stage=%s last_out_len=%d last_out_id=%d\n",
          client_fd,
          getClientState(client_fd),
          debug_io_expected,
          debug_io_progress,
          (long long)debug_io_wait_us,
          debug_io_last_errno,
          debug_last_packet_length,
          debug_last_packet_id,
          debug_last_payload_length,
          debug_last_out_stage,
          debug_last_out_packet_length,
          debug_last_out_packet_id
        );
        #endif
        disconnectClient(&client_fd, -2);
        return -1;
      }
      task_yield();
      continue;
    }

    #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
    printf(
      "ERROR: send failed fd=%d state=%d expected=%d progress=%d errno=%d packet_len=%d packet_id=%d payload_len=%d last_out_stage=%s last_out_len=%d last_out_id=%d\n",
      client_fd,
      getClientState(client_fd),
      debug_io_expected,
      debug_io_progress,
      debug_io_last_errno,
      debug_last_packet_length,
      debug_last_packet_id,
      debug_last_payload_length,
      debug_last_out_stage,
      debug_last_out_packet_length,
      debug_last_out_packet_id
    );
    #endif

    disconnectClient(&client_fd, -3);
    return -1; // 真错误
  }

  return sent;
}

void discard_all (int client_fd, size_t remaining, uint8_t require_first) {
  while (remaining > 0) {
    size_t recv_n = remaining > MAX_RECV_BUF_LEN ? MAX_RECV_BUF_LEN : remaining;
    ssize_t received = recv_all(client_fd, recv_buffer, recv_n, require_first);
    if (received < 0) return;
    if (received > remaining) return;
    remaining -= received;
    require_first = false;
  }
}

ssize_t writeByte (int client_fd, uint8_t byte) {
  return send_all(client_fd, &byte, 1);
}
ssize_t writeUint16 (int client_fd, uint16_t num) {
  uint16_t be = htons(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint32 (int client_fd, uint32_t num) {
  uint32_t be = htonl(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint64 (int client_fd, uint64_t num) {
  uint64_t be = htonll(num);
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeFloat (int client_fd, float num) {
  uint32_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonl(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}
ssize_t writeDouble (int client_fd, double num) {
  uint64_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonll(bits);
  return send_all(client_fd, &bits, sizeof(bits));
}

uint8_t readByte (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 1, false);
  return recv_buffer[0];
}
uint16_t readUint16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((uint16_t)recv_buffer[0] << 8) | recv_buffer[1];
}
int16_t readInt16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((int16_t)recv_buffer[0] << 8) | (int16_t)recv_buffer[1];
}
uint32_t readUint32 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 4, false);
  return ((uint32_t)recv_buffer[0] << 24) |
         ((uint32_t)recv_buffer[1] << 16) |
         ((uint32_t)recv_buffer[2] << 8) |
         ((uint32_t)recv_buffer[3]);
}
uint64_t readUint64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((uint64_t)recv_buffer[0] << 56) |
         ((uint64_t)recv_buffer[1] << 48) |
         ((uint64_t)recv_buffer[2] << 40) |
         ((uint64_t)recv_buffer[3] << 32) |
         ((uint64_t)recv_buffer[4] << 24) |
         ((uint64_t)recv_buffer[5] << 16) |
         ((uint64_t)recv_buffer[6] << 8) |
         ((uint64_t)recv_buffer[7]);
}
int64_t readInt64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((int64_t)recv_buffer[0] << 56) |
         ((int64_t)recv_buffer[1] << 48) |
         ((int64_t)recv_buffer[2] << 40) |
         ((int64_t)recv_buffer[3] << 32) |
         ((int64_t)recv_buffer[4] << 24) |
         ((int64_t)recv_buffer[5] << 16) |
         ((int64_t)recv_buffer[6] << 8) |
         ((int64_t)recv_buffer[7]);
}
float readFloat (int client_fd) {
  uint32_t bytes = readUint32(client_fd);
  float output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}
double readDouble (int client_fd) {
  uint64_t bytes = readUint64(client_fd);
  double output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}

// 读带长度前缀的数据
ssize_t readLengthPrefixedData (int client_fd) {
  uint32_t length = readVarInt(client_fd);
  if (length >= MAX_RECV_BUF_LEN) {
    printf("ERROR: Received length (%lu) exceeds maximum (%u)\n", length, MAX_RECV_BUF_LEN);
    disconnectClient(&client_fd, -1);
    recv_count = 0;
    return 0;
  }
  return recv_all(client_fd, recv_buffer, length, false);
}

// 读字符串到 recv_buffer
void readString (int client_fd) {
  recv_count = readLengthPrefixedData(client_fd);
  recv_buffer[recv_count] = '\0';
}
// 最多读 N 字节字符串
void readStringN (int client_fd, uint32_t max_length) {
  // 上限不合法时退回 readString
  if (max_length >= MAX_RECV_BUF_LEN) {
    readString(client_fd);
    return;
  }
  // 没超上限就全部读入
  uint32_t length = readVarInt(client_fd);
  if (max_length > length) {
    recv_count = recv_all(client_fd, recv_buffer, length, false);
    recv_buffer[recv_count] = '\0';
    return;
  }
  // 超上限就截断, 剩余部分丢弃
  recv_count = recv_all(client_fd, recv_buffer, max_length, false);
  recv_buffer[recv_count] = '\0';
  uint8_t dummy;
  for (uint32_t i = max_length; i < length; i ++) {
    recv_all(client_fd, &dummy, 1, false);
  }
}

uint32_t fast_rand () {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64 (uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

#ifndef ESP_PLATFORM
// 返回程序运行时间, 单位微秒
// 这里只用来计算时间差
int64_t get_program_time () {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif


