#ifndef H_VARNUM
#define H_VARNUM

#include <stdint.h>

#define SEGMENT_BITS 0x7F
#define CONTINUE_BIT 0x80
#define VARNUM_ERROR 0xFFFFFFFF

int32_t readVarInt (int client_fd);
int sizeVarInt (uint32_t value);
int writeVarInt (int client_fd, uint32_t value);
int sizeVarLong (uint64_t value);
int writeVarLong (int client_fd, uint64_t value);

#endif
