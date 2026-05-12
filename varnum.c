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

#include <errno.h>

#include "varnum.h"
#include "globals.h"
#include "tools.h"
#include "procedures.h"

int32_t readVarInt (int client_fd) {
  int32_t value = 0;
  int position = 0;
  uint8_t byte;

  while (true) {
    byte = readByte(client_fd);
    if (recv_count != 1) {
      #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
      printf(
        "ERROR: readVarInt failed fd=%d recv_count=%d errno=%d state=%d packet_len=%d packet_id=%d payload=%d\n",
        client_fd,
        (int)recv_count,
        errno,
        getClientState(client_fd),
        debug_last_packet_length,
        debug_last_packet_id,
        debug_last_payload_length
      );
      #endif
      return VARNUM_ERROR;
    }

    value |= (byte & SEGMENT_BITS) << position;

    if ((byte & CONTINUE_BIT) == 0) break;

    position += 7;
    if (position >= 32) {
      #ifdef DEV_LOG_NETWORK_DIAGNOSTICS
      printf(
        "ERROR: readVarInt overflow fd=%d state=%d partial_value=%d packet_len=%d packet_id=%d payload=%d\n",
        client_fd,
        getClientState(client_fd),
        value,
        debug_last_packet_length,
        debug_last_packet_id,
        debug_last_payload_length
      );
      #endif
      return VARNUM_ERROR;
    }
  }

  return value;
}

int sizeVarInt (uint32_t value) {
  int size = 1;
  while ((value & ~SEGMENT_BITS) != 0) {
    value >>= 7;
    size ++;
  }
  return size;
}

int writeVarInt (int client_fd, uint32_t value) {
  while (true) {
    if ((value & ~SEGMENT_BITS) == 0) {
      if (writeByte(client_fd, value) == -1) return -1;
      return 0;
    }

    if (writeByte(client_fd, (value & SEGMENT_BITS) | CONTINUE_BIT) == -1) return -1;

    value >>= 7;
  }
}

int sizeVarLong (uint64_t value) {
  int size = 1;
  while ((value & ~((uint64_t)SEGMENT_BITS)) != 0) {
    value >>= 7;
    size ++;
  }
  return size;
}

int writeVarLong (int client_fd, uint64_t value) {
  while (true) {
    if ((value & ~((uint64_t)SEGMENT_BITS)) == 0) {
      if (writeByte(client_fd, (uint8_t)value) == -1) return -1;
      return 0;
    }

    if (writeByte(client_fd, (uint8_t)((value & SEGMENT_BITS) | CONTINUE_BIT)) == -1) return -1;
    value >>= 7;
  }
}
