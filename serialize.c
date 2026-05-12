#include "globals.h"

#ifdef SYNC_WORLD_TO_DISK

#if defined(ARDUINO_PLATFORM)
  // Arduino ESP32 走 LittleFS
  #include <LittleFS.h>
  #define FILE_PATH "/world.bin"
  // 挂载 LittleFS
  static inline int arduino_mount_littlefs() {
    if (!LittleFS.begin(true)) { // 失败时格式化
      Serial.println("LittleFS mount failed");
      return 1;
    }
    return 0;
  }
#elif defined(ESP_PLATFORM)
  #include "esp_littlefs.h"
  #define FILE_PATH "/littlefs/world.bin"
#else
  #include <stdio.h>
  #define FILE_PATH "world.bin"
#endif

#include "tools.h"
#include "registries.h"
#include "serialize.h"

int64_t last_disk_sync_time = 0;

// 读取世界数据, 没有就新建
int initSerializer () {

  last_disk_sync_time = get_program_time();

  #if defined(ARDUINO_PLATFORM)
    if (arduino_mount_littlefs()) return 1;
  #elif defined(ESP_PLATFORM)
    esp_vfs_littlefs_conf_t conf = {
      .base_path = "/littlefs",
      .partition_label = "littlefs",
      .format_if_mount_failed = true,
      .dont_mount = false
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
      printf("LittleFS error %d\n", ret);
      perror("Failed to mount LittleFS. Aborting.");
      return 1;
    }
  #endif

  // 先尝试打开世界文件
  FILE *file = fopen(FILE_PATH, "rb");
  if (file) {

    // 先读方块改动
    size_t read = fread(block_changes, 1, sizeof(block_changes), file);
    if (read != sizeof(block_changes)) {
      printf("Read %u bytes from \"world.bin\", expected %u (block changes). Aborting.\n", read, sizeof(block_changes));
      fclose(file);
      return 1;
    }
    // 恢复 block_changes_count
    for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
      if (block_changes[i].block == 0xFF) continue;
      if (block_changes[i].block == B_chest) i += 14;
      if (i >= block_changes_count) block_changes_count = i + 1;
    }
    // 跳到玩家数据区
    if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
      perror("Failed to seek to player data in \"world.bin\". Aborting.");
      fclose(file);
      return 1;
    }
    // 直接读玩家数据
    read = fread(player_data, 1, sizeof(player_data), file);
    fclose(file);
    if (read != sizeof(player_data)) {
      printf("Read %u bytes from \"world.bin\", expected %u (player data). Aborting.\n", read, sizeof(player_data));
      return 1;
    }

  } else { // 世界文件不存在时新建
    printf("No \"world.bin\" file found, creating one...\n\n");

    // 用二进制写模式新建
    file = fopen(FILE_PATH, "wb");
    if (!file) {
      perror(
        "Failed to open \"world.bin\" for writing.\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }
    // 先写初始方块改动表
    // 前提是所有 entry 的 block 都已是 0xFF
    size_t written = fwrite(block_changes, 1, sizeof(block_changes), file);
    if (written != sizeof(block_changes)) {
      perror(
        "Failed to write initial block data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // 跳到玩家数据区
    if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
      perror(
        "Failed to seek past block changes in \"world.bin\"."
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      fclose(file);
      return 1;
    }
    // 再写初始玩家数据
    written = fwrite(player_data, 1, sizeof(player_data), file);
    fclose(file);
    if (written != sizeof(player_data)) {
      perror(
        "Failed to write initial player data to \"world.bin\".\n"
        "Consider checking permissions or disabling SYNC_WORLD_TO_DISK in \"globals.h\"."
      );
      return 1;
    }

  }

  return 0;
}

// 写一段方块改动到磁盘
void writeBlockChangesToDisk (int from, int to) {

  // 用读写模式打开
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Block updates have been dropped.");
    return;
  }

  for (int i = from; i <= to; i ++) {
    // 跳到对应偏移
    if (fseek(file, i * sizeof(BlockChange), SEEK_SET) != 0) {
      fclose(file);
      perror("Failed to seek in \"world.bin\". Block updates have been dropped.");
      return;
    }
    // 写这条方块改动
    if (fwrite(&block_changes[i], 1, sizeof(BlockChange), file) != sizeof(BlockChange)) {
      fclose(file);
      perror("Failed to write to \"world.bin\". Block updates have been dropped.");
      return;
    }
  }

  fclose(file);
}

// 全量写玩家数据
void writePlayerDataToDisk () {

  // 用读写模式打开
  FILE *file = fopen(FILE_PATH, "r+b");
  if (!file) {
    perror("Failed to open \"world.bin\". Player updates have been dropped.");
    return;
  }
  // 跳过方块改动区
  if (fseek(file, sizeof(block_changes), SEEK_SET) != 0) {
    fclose(file);
    perror("Failed to seek in \"world.bin\". Player updates have been dropped.");
    return;
  }
  // 整块写玩家数据
  // 这部分写入较大, 不要太频繁
  if (fwrite(&player_data, 1, sizeof(player_data), file) != sizeof(player_data)) {
    fclose(file);
    perror("Failed to write to \"world.bin\". Player updates have been dropped.");
    return;
  }

  fclose(file);
}

// 到时间后再写队列里的数据
void writeDataToDiskOnInterval () {

  // 没到间隔就先不写
  if (get_program_time() - last_disk_sync_time < DISK_SYNC_INTERVAL) return;
  last_disk_sync_time = get_program_time();

  // 写玩家数据和方块改动
  writePlayerDataToDisk();
  #ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
  writeBlockChangesToDisk(0, block_changes_count);
  #endif

}

#ifdef ALLOW_CHESTS
// 把 chest 槽位改动写到磁盘
void writeChestChangesToDisk (uint8_t *storage_ptr, uint8_t slot) {
  /**
   * 这里仍沿用 chest 内存复用方案
   * chest 数据直接塞在 block_changes 里
   * storage_ptr 指向 chest 后面的第一条数据
   * 先减去 block_changes 起始地址
   * 再除以 BlockChange 大小得到 entry 下标
   * 最后加上 slot / 2, 因为一条 entry 编两个槽位
   */
  int index = (int)(storage_ptr - (uint8_t *)block_changes) / sizeof(BlockChange) + slot / 2;
  writeBlockChangesToDisk(index, index);
}
#endif

#endif
