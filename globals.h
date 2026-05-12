#ifndef H_GLOBALS
#define H_GLOBALS

#include <stdint.h>

// 平台判断
// Arduino ESP32 用 ARDUINO 开兼容层
#ifdef ARDUINO
  #define ARDUINO_PLATFORM
#endif

#ifdef ARDUINO_PLATFORM
  // C 代码直接走 arduino_compat.h
  #include "arduino_compat.h"
#else
  #include <unistd.h>
#endif

#ifdef ARDUINO_PLATFORM
  // task_yield 在 arduino_compat.h 中定义为宏
#elif defined(ESP_PLATFORM)
  void task_yield ();
#else
  #define task_yield();
#endif

#define true 1
#define false 0

// TCP 端口
#define PORT 25565

// 玩家存档槽位数
// 离线玩家也占槽
#define MAX_PLAYERS 16

// Mob 槽位数
#define MAX_MOBS (MAX_PLAYERS)

// Mob 消失距离
#define MOB_DESPAWN_DISTANCE 256

// 游戏模式: 0 生存, 1 创造, 2 冒险, 3 旁观
#define GAMEMODE 0

// 最大发块视距
#define VIEW_DISTANCE 2

// Tick 间隔, 单位微秒
#define TIME_BETWEEN_TICKS 1000000

// 由 TIME_BETWEEN_TICKS 算出
#define TICKS_PER_SECOND ((float)1000000 / TIME_BETWEEN_TICKS)

// 初始世界种子
// 启动后还会再做一次 hash
#define INITIAL_WORLD_SEED 0xA103DE6C

// 初始随机种子
// 掉落和 Mob 行为共用这个种子
#define INITIAL_RNG_SEED 0xE2B9419

// Minichunk 大小
// 最好取 2 的幂
#define CHUNK_SIZE 8

// 地形基准低点
#define TERRAIN_BASE_HEIGHT 60

// 洞穴基准深度
#define CAVE_BASE_DEPTH 24

// Biome 尺寸
// 最好也取 2 的幂
#define BIOME_SIZE (CHUNK_SIZE * 8)

// 由 BIOME_SIZE 算出
#define BIOME_RADIUS (BIOME_SIZE / 2)

// 记住最近去过多少个区块
// 避免重复发区块
// 至少得是 1
#define VISITED_HISTORY 4

// 最多保留多少玩家改动
// 也决定这块固定内存大小
#define MAX_BLOCK_CHANGES 20000

// 开启后把世界数据同步到磁盘或 flash
// 这是同步操作, 慢盘上会拖性能
// 平时还是以内存为主
// 只在启动时读盘
// ESP-IDF / Arduino 下走 LittleFS
// Flash 写入较慢且稳定性一般
// 所以默认不开
#if !defined(ESP_PLATFORM) && !defined(ARDUINO_PLATFORM)
  #define SYNC_WORLD_TO_DISK
#endif

// 最小磁盘同步间隔
// 默认只管玩家数据
// 方块改动默认是即时小块写
// 开 DISK_SYNC_BLOCKS_ON_INTERVAL 后方块改动也按间隔写
#define DISK_SYNC_INTERVAL 15000000

// 方块改动改成按间隔同步
// 快盘一般不需要
// #define DISK_SYNC_BLOCKS_ON_INTERVAL

// 网络超时
// 默认 15 秒
#define NETWORK_TIMEOUT_TIME 15000000

// 收包字符串缓冲区大小
#define MAX_RECV_BUF_LEN 256

// 给客户端发送 server brand
// 会显示在 F3 左上角
// 字符串在 globals.c 里改
#define SEND_BRAND

// 收到移动包就立刻转发
// 低 Tickrate 下会更顺
// 但人多了可能更抖
#define BROADCAST_ALL_MOVEMENT

// 按玩家数降低移动广播频率
// 人多时省点带宽
// 但看起来会更卡顿
// 低 Tickrate 下别和上面的选项反着配
// 否则更新频率会过低
#define SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT

// 方块更新时顺带算流体
// 稍微吃算力, 也可能不稳
#define DO_FLUID_FLOW

// 开启 chest
// 每个 chest 占 15 个 block change 槽
// 还有额外检查和内存复用
// 某些平台可能会卡甚至崩
#define ALLOW_CHESTS

// 开启飞行
// 饥饿时也能疾跑
// #define ENABLE_PLAYER_FLIGHT

// 开启拾取动画
// 只补动画, 不改实际拾取逻辑
// 掉落物仍直接进入背包
// 会额外多发几个包
#define ENABLE_PICKUP_ANIMATION

// 开启仙人掌伤害
#define ENABLE_CACTUS_DAMAGE

// 记录未知包 ID
// #define DEV_LOG_UNKNOWN_PACKETS

// 断开时打印详细网络日志
#define DEV_LOG_NETWORK_DIAGNOSTICS

// 记录包长和解析长度不一致
#define DEV_LOG_LENGTH_DISCREPANCY

// 记录区块生成日志
// #define DEV_LOG_CHUNK_GENERATION

// 登录后只发最小 play 包集合
// 用来排查到底哪个客户端包触发掉线
#define DEV_MINIMAL_PLAY_BOOTSTRAP

// 开启 0xBEEF / 0xFEED 导出导入世界
// 没鉴权, 默认不开
// #define DEV_ENABLE_BEEF_DUMPS

#define STATE_NONE 0
#define STATE_STATUS 1
#define STATE_LOGIN 2
#define STATE_TRANSFER 3
#define STATE_CONFIGURATION 4
#define STATE_PLAY 5

extern ssize_t recv_count;
extern uint8_t recv_buffer[MAX_RECV_BUF_LEN];

extern uint32_t world_seed;
extern uint32_t rng_seed;

extern uint16_t world_time;
extern uint32_t server_ticks;

extern char motd[];
extern uint8_t motd_len;

#ifdef SEND_BRAND
  extern char brand[];
  extern uint8_t brand_len;
#endif

extern uint16_t client_count;

extern int debug_last_client_fd;
extern int debug_last_state;
extern int debug_last_packet_length;
extern int debug_last_packet_id;
extern int debug_last_payload_length;
extern int debug_last_out_packet_length;
extern int debug_last_out_packet_id;
extern const char *debug_last_out_stage;

extern int debug_io_expected;
extern int debug_io_progress;
extern int debug_io_require_first;
extern int debug_io_last_errno;
extern int debug_io_last_result;
extern int64_t debug_io_wait_us;

typedef struct {
  short x;
  short z;
  uint8_t y;
  uint8_t block;
} BlockChange;

#pragma pack(push, 1)

typedef struct {
  uint8_t uuid[16];
  char name[16];
  int client_fd;
  short x;
  uint8_t y;
  short z;
  short visited_x[VISITED_HISTORY];
  short visited_z[VISITED_HISTORY];
  #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
    uint16_t packets_since_update;
  #endif
  int8_t yaw;
  int8_t pitch;
  uint8_t grounded_y;
  uint8_t health;
  uint8_t hunger;
  uint16_t saturation;
  uint8_t hotbar;
  uint16_t inventory_items[41];
  uint16_t craft_items[9];
  uint8_t inventory_count[41];
  uint8_t craft_count[9];
  // 具体含义见下方 flags
  // 无 flag 时存鼠标物品 ID
  uint16_t flagval_16;
  // 具体含义见下方 flags
  // 无 flag 时存鼠标物品数量
  uint8_t flagval_8;
  // 0x01 - 攻击冷却, flagval_8 作计时器
  // 0x02 - 新玩家待生成
  // 0x04 - 潜行
  // 0x08 - 疾跑
  // 0x10 - 吃东西, flagval_16 作计时器
  // 0x20 - 客户端加载中, flagval_16 作超时计时器
  // 0x40 - 移动更新冷却
  // 0x80 - craft_items 已锁, 当前存的是指针
  uint8_t flags;
} PlayerData;

typedef struct {
  uint8_t type;
  short x;
  // Mob 死后
  // Y 坐标改作回收计时器
  uint8_t y;
  short z;
  // 低 5 位: 血量
  // 中间 1 位: 羊是否剃毛
  // 高 2 位: 惊慌计时器
  uint8_t data;
} MobData;

#pragma pack(pop)

union EntityDataValue {
  uint8_t byte;
  int pose;
};

typedef struct {
  uint8_t index;
  // 0 - Byte
  // 21 - Pose
  int type;
  union EntityDataValue value;
} EntityData;

extern BlockChange block_changes[MAX_BLOCK_CHANGES];
extern int block_changes_count;

extern PlayerData player_data[MAX_PLAYERS];
extern int player_data_count;

extern MobData mob_data[MAX_MOBS];

#endif




