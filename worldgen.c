#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "worldgen.h"

uint32_t getChunkHash (short x, short z) {

  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);

  return splitmix64(*((uint64_t *)buf));

}

uint8_t getChunkBiome (short x, short z) {

  // 把 biome 原点对齐到 0,0
  x += BIOME_RADIUS;
  z += BIOME_RADIUS;

  // 计算到 biome 中心的距离
  int8_t dx = BIOME_RADIUS - mod_abs(x, BIOME_SIZE);
  int8_t dz = BIOME_RADIUS - mod_abs(z, BIOME_SIZE);
  // 每个 biome 都是圆岛, 中间留 beach
  if (dx * dx + dz * dz > BIOME_RADIUS * BIOME_RADIUS) return W_beach;

  // 计算 biome 坐标
  short biome_x = div_floor(x, BIOME_SIZE);
  short biome_z = div_floor(z, BIOME_SIZE);

  // biome 直接从世界种子里取
  // 32 位种子按 4x4 biome 表解释
  // 每格占 2 bit, 所以 beach 以外只有 4 种 biome
  // 这样能省掉额外的 hash 分层
  uint8_t index = abs((biome_x & 3) + ((biome_z * 4) & 15));
  static const uint8_t biome_lookup[4] = {
    W_plains,
    W_mangrove_swamp,
    W_desert,
    W_snowy_plains
  };
  return biome_lookup[(world_seed >> (index * 2)) & 3];

}

uint8_t getCornerHeight (uint32_t hash, uint8_t biome) {

  // 高度直接拆 hash 取随机量
  // 多段叠加让地形更稳, 也保留少量大起伏
  uint8_t height = TERRAIN_BASE_HEIGHT;

  switch (biome) {

    case W_mangrove_swamp: {
      height += (
        (hash % 3) +
        ((hash >> 4) % 3) +
        ((hash >> 8) % 3) +
        ((hash >> 12) % 3)
      );
      // 低于海平面时再往下压一点
      if (height < 64) height -= (hash >> 24) & 3;
      break;
    }

    case W_plains: {
      height += (
        (hash & 3) +
        (hash >> 4 & 3) +
        (hash >> 8 & 3) +
        (hash >> 12 & 3)
      );
      break;
    }

    case W_desert: {
      height += 4 + (
        (hash & 3) +
        (hash >> 4 & 3)
      );
      break;
    }

    case W_beach: {
      // 起点略低于海平面, 保证整片进水
      height = 62 - (
        (hash & 3) +
        (hash >> 4 & 3) +
        (hash >> 8 & 3)
      );
      break;
    }

    case W_snowy_plains: {
      // 少拆几段, 但单段范围更大, 用来做丘陵
      height += (
        (hash & 7) +
        (hash >> 4 & 7)
      );
      break;
    }

    default: break;
  }

  return height;

}

uint8_t interpolate (uint8_t a, uint8_t b, uint8_t c, uint8_t d, int x, int z) {
  uint16_t top    = a * (CHUNK_SIZE - x) + b * x;
  uint16_t bottom = c * (CHUNK_SIZE - x) + d * x;
  return (top * (CHUNK_SIZE - z) + bottom * z) / (CHUNK_SIZE * CHUNK_SIZE);
}

static uint8_t getPlainsDecoration (ChunkAnchor anchor, int rx, int rz, uint8_t height) {
  if (height < 64) return 0;

  // 按地形 hash 和局部坐标生成固定装饰
  uint8_t deco = (uint8_t)(
    ((anchor.hash >> ((rx + rz) & 15)) & 0xFF) ^
    ((rx & 15) << 4) ^
    (rz & 15) ^
    height
  );

  if ((deco & 31) == 0) return B_dandelion;
  if ((deco & 31) == 1) return B_poppy;
  if ((deco & 3) == 0) return B_short_grass;

  return 0;
}

// 用 anchor 缓存计算地形高度
// anchor_ptr 要指向目标 minichunk
// 且 +X 和 +Z 邻居都要可用
uint8_t getHeightAtFromAnchors (int rx, int rz, ChunkAnchor *anchor_ptr) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome),
    getCornerHeight(anchor_ptr[1].hash, anchor_ptr[1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 1].hash, anchor_ptr[16 / CHUNK_SIZE + 1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 2].hash, anchor_ptr[16 / CHUNK_SIZE + 2].biome),
    rx, rz
  );

}

uint8_t getHeightAtFromHash (int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(chunk_hash, biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(chunk_hash, biome),
    getCornerHeight(getChunkHash(_x + 1, _z), getChunkBiome(_x + 1, _z)),
    getCornerHeight(getChunkHash(_x, _z + 1), getChunkBiome(_x, _z + 1)),
    getCornerHeight(getChunkHash(_x + 1, _z + 1), getChunkBiome(_x + 1, _z + 1)),
    rx, rz
  );

}

// 取指定坐标的原始地形高度
// 不叠加 block change
uint8_t getHeightAt (int x, int z) {

  int _x = div_floor(x, CHUNK_SIZE);
  int _z = div_floor(z, CHUNK_SIZE);
  int rx = mod_abs(x, CHUNK_SIZE);
  int rz = mod_abs(z, CHUNK_SIZE);
  uint32_t chunk_hash = getChunkHash(_x, _z);
  uint8_t biome = getChunkBiome(_x, _z);

  return getHeightAtFromHash(rx, rz, _x, _z, chunk_hash, biome);

}

uint8_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {

  if (y >= 64 && y >= height && feature.y != 255) switch (anchor.biome) {
    case W_plains: { // 平原树

      // 水下不生成树
      if (feature.y < 64) break;
      uint8_t birch_style = (((anchor.hash >> ((feature.x + feature.z) & 7)) & 3) == 0);
      // 少量地表矿树:
      // 树干和树根是矿石
      // 树叶是石头
      uint8_t ore_tree_roll = (uint8_t)(
        ((anchor.hash >> ((feature.x ^ feature.z) & 15)) & 255) ^
        ((feature.x * 17 + feature.z * 31) & 255)
      );
      uint8_t ore_tree_type = 0; // 0 普通, 1 铁矿, 2 钻石
      // 矿树和普通树总概率对半
      if (ore_tree_roll & 1) {
        ore_tree_type = (ore_tree_roll & 2) ? 2 : 1;
      }

      uint8_t tree_log = B_oak_log;
      uint8_t tree_leaf = B_oak_leaves;
      uint8_t tree_root = B_dirt;
      if (ore_tree_type == 1) {
        tree_log = B_iron_ore;
        tree_leaf = B_stone;
        tree_root = B_iron_ore;
      } else if (ore_tree_type == 2) {
        tree_log = B_diamond_ore;
        tree_leaf = B_stone;
        tree_root = B_diamond_ore;
      }
      int tree_top = feature.y - feature.variant + 6 + (birch_style ? 1 : 0);

      // 处理树干和树根下的地块
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return tree_root;
        if (y >= feature.y && y < tree_top) return tree_log;
      }

      // 计算到树中心的 X/Z 距离
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;

      // 生成树叶团
      if (dx < 3 && dz < 3 && y > tree_top - 4 && y < tree_top - 1) {
        if (y == tree_top - 2 && dx == 2 && dz == 2) break;
        return tree_leaf;
      }
      if (dx < 2 && dz < 2 && y >= tree_top - 1 && y <= tree_top) {
        if (y == tree_top && dx == 1 && dz == 1) break;
        return tree_leaf;
      }

      // 这里已经确定在平原海平面以上
      // 不用再往下判断周围地块
      if (y == height) return B_grass_block;
      if (y == height + 1) {
        uint8_t decoration = getPlainsDecoration(anchor, rx, rz, height);
        if (decoration) return decoration;
      }
      return B_air;
    }

    case W_desert: { // 沙漠枯木和仙人掌

      if (x != feature.x || z != feature.z) break;

      if (feature.variant == 0) {
        if (y == height + 1) return B_dead_bush;
      } else if (y > height) {
        // 仙人掌高度按目标地形奇偶决定
        if (height & 1 && y <= height + 3) return B_cactus;
        if (y <= height + 2) return B_cactus;
      }

      break;

    }

    case W_mangrove_swamp: { // 沼泽睡莲和苔藓地毯

      if (x == feature.x && z == feature.z && y == 64 && height < 63) {
        return B_lily_pad;
      }

      if (y == height + 1) {
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        if (dx + dz < 4) return B_moss_carpet;
      }

      break;
    }

    case W_snowy_plains: { // 雪原草丛

      if (x == feature.x && z == feature.z && y == height + 1 && height >= 64) {
        return B_short_grass;
      }

      break;
    }

    default: break;
  }

  // 处理地表层
  if (height >= 63) {
    if (y == height) {
      if (anchor.biome == W_mangrove_swamp) return B_mud;
      if (anchor.biome == W_snowy_plains) return B_snowy_grass_block;
      if (anchor.biome == W_desert) return B_sand;
      if (anchor.biome == W_beach) return B_sand;
      return B_grass_block;
    }
    if (anchor.biome == W_plains && y == height + 1) {
      uint8_t decoration = getPlainsDecoration(anchor, rx, rz, height);
      if (decoration) return decoration;
    }
    if (anchor.biome == W_snowy_plains && y == height + 1) {
      return B_snow;
    }
  }
  // 从地表下 4 格开始生成矿和洞穴
  if (y <= height - 4) {
    // 洞穴直接镜像地表起伏
    int8_t gap = height - TERRAIN_BASE_HEIGHT;
    if (y < CAVE_BASE_DEPTH + gap && y > CAVE_BASE_DEPTH - gap) return B_air;

    // 用区块内 X/Z 过一遍 xorshift
    // 给这一列固定出一个矿点 Y
    uint8_t ore_y = ((rx & 15) << 4) + (rz & 15);
    ore_y ^= ore_y << 4;
    ore_y ^= ore_y >> 5;
    ore_y ^= ore_y << 1;
    ore_y &= 63;

    if (y == ore_y) {
      // ore_y 本身近似 [0,64) 随机数
      // 再和 anchor hash 组合出矿物稀有度
      uint8_t ore_probability = (anchor.hash >> (ore_y % 24)) & 255;
      // 矿物按高度和概率决定
      if (y < 15) {
        if (ore_probability < 10) return B_diamond_ore;
        if (ore_probability < 12) return B_gold_ore;
        if (ore_probability < 15) return B_redstone_ore;
      }
      if (y < 30) {
        if (ore_probability < 3) return B_gold_ore;
        if (ore_probability < 8) return B_redstone_ore;
      }
      if (y < 54) {
        if (ore_probability < 30) return B_iron_ore;
        if (ore_probability < 40) return B_copper_ore;
      }
      if (ore_probability < 60) return B_coal_ore;
      if (y < 5) return B_lava;
      return B_cobblestone;
    }

    // 其余情况回退成石头
    return B_stone;
  }
  // 处理中间土层
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_mangrove_swamp) return B_mud;
    if (anchor.biome == W_beach && height > 64) return B_sandstone;
    return B_dirt;
  }
  // 还没命中且低于海平面时补水或冰
  if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
  if (y < 64) return B_water;

  // 其余情况回退成空气
  return B_air;

}

ChunkFeature getFeatureFromAnchor (ChunkAnchor anchor) {

  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);

  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  uint8_t skip_feature = false;

  // 这一步既防止树跨 chunk
  // 也顺便压低地表特征密度
  if (anchor.biome != W_mangrove_swamp) {
    if (feature.x < 3 || feature.x > CHUNK_SIZE - 3) skip_feature = true;
    else if (feature.z < 3 || feature.z > CHUNK_SIZE - 3) skip_feature = true;
  }

  if (skip_feature) {
    // 跳过的特征统一记成 Y = 0xFF
    feature.y = 0xFF;
  } else {
    feature.x += anchor.x * CHUNK_SIZE;
    feature.z += anchor.z * CHUNK_SIZE;
    feature.y = getHeightAtFromHash(
      mod_abs(feature.x, CHUNK_SIZE), mod_abs(feature.z, CHUNK_SIZE),
      anchor.x, anchor.z, anchor.hash, anchor.biome
    ) + 1;
    feature.variant = (anchor.hash >> (feature.x + feature.z)) & 1;
  }

  return feature;

}

uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor) {

  if (y > 80) return B_air;

  int rx = x % CHUNK_SIZE;
  int rz = z % CHUNK_SIZE;
  if (rx < 0) rx += CHUNK_SIZE;
  if (rz < 0) rz += CHUNK_SIZE;

  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAtFromHash(rx, rz, anchor.x, anchor.z, anchor.hash, anchor.biome);

  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);

}

uint8_t getBlockAt (int x, int y, int z) {

  if (y < 0) return B_bedrock;

  uint8_t block_change = getBlockChange(x, y, z);
  if (block_change != 0xFF) return block_change;

  short anchor_x = div_floor(x, CHUNK_SIZE);
  short anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = {
    .x = anchor_x,
    .z = anchor_z,
    .hash = getChunkHash(anchor_x, anchor_z),
    .biome = getChunkBiome(anchor_x, anchor_z)
  };

  return getTerrainAt(x, y, z, anchor);

}

uint8_t chunk_section[4096];

// 构建 16x16x16 区块数据到输出缓冲
// 返回区块原点角落的 biome
uint8_t buildChunkSectionInto (uint8_t *section_out, int cx, int cy, int cz) {
  ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
  ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
  uint8_t chunk_section_height[16][16];

  // 预计算相关 minichunk 的 hash、anchor 和 feature
  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {

      ChunkAnchor *anchor = chunk_anchors + anchor_index;

      anchor->x = j / CHUNK_SIZE;
      anchor->z = i / CHUNK_SIZE;
      anchor->hash = getChunkHash(anchor->x, anchor->z);
      anchor->biome = getChunkBiome(anchor->x, anchor->z);

      // 计算本 section 内各 minichunk 的 feature
      if (i != cz + 16 && j != cx + 16) {
        chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
        feature_index ++;
      }

      anchor_index ++;
    }
  }

  // 预计算整段区块高度
  for (int i = 0; i < 16; i ++) {
    for (int j = 0; j < 16; j ++) {
      anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
      ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
      chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
    }
  }

  // 按逻辑顺序生成 4096 个方块
  for (int j = 0; j < 4096; j += 8) {
    // 这几项在当前 8 格批次里不变
    int y = j / 256 + cy;
    int rz = j / 16 % 16;
    int rz_mod = rz % CHUNK_SIZE;
    feature_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE);
    anchor_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);
    for (int offset = 0; offset < 8; offset++) {
      int k = j + offset;
      int rx = k % 16;
      // 合并缓存结果取出方块
      section_out[k] = getTerrainAtFromCache(
        rx + cx, y, rz + cz,
        rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index],
        chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
  }

  return chunk_anchors[0].biome;

}

// 兼容旧调用点的包装
uint8_t buildChunkSection (int cx, int cy, int cz) {
  return buildChunkSectionInto(chunk_section, cx, cy, cz);
}
