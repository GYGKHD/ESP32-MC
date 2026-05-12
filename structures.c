
#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "worldgen.h"
#include "procedures.h"
#include "structures.h"

void setBlockIfReplaceable (short x, uint8_t y, short z, uint8_t block) {
  uint8_t target = getBlockAt(x, y, z);
  if (!isReplaceableBlock(target) && target != B_oak_leaves) return;
  makeBlockChange(x, y, z, block);
}

// 在输入坐标中心放一棵树
void placeTreeStructure (short x, uint8_t y, short z) {

  // 取随机数控制树高和叶角
  uint32_t r = fast_rand();
  uint8_t birch_style = ((r >> 3) & 3) == 0;
  uint8_t log_block = B_oak_log;
  uint8_t height = (birch_style ? 5 : 4) + (r % 3);

  // 树根改成原木, 下方补泥土
  makeBlockChange(x, y - 1, z, B_dirt);
  makeBlockChange(x, y, z, log_block);
  // 生成树干
  for (int i = 1; i < height; i ++) {
    setBlockIfReplaceable(x, y + i, z, log_block);
  }
  // 记录叶角偏移, 用来取随机位
  uint8_t t = 2;
  // 第一层叶子
  for (int i = -2; i <= 2; i ++) {
    for (int j = -2; j <= 2; j ++) {
      setBlockIfReplaceable(x + i, y + height - 3, z + j, B_oak_leaves);
      // 随机裁掉角块, 模拟原版树形
      if ((i == 2 || i == -2) && (j == 2 || j == -2)) {
        t ++;
        if ((r >> t) & 1) continue;
      }
      setBlockIfReplaceable(x + i, y + height - 2, z + j, B_oak_leaves);
    }
  }
  // 第二层叶子
  for (int i = -1; i <= 1; i ++) {
    for (int j = -1; j <= 1; j ++) {
      setBlockIfReplaceable(x + i, y + height - 1, z + j, B_oak_leaves);
      if ((i == 1 || i == -1) && (j == 1 || j == -1)) {
        t ++;
        if ((r >> t) & 1) continue;
      }
      setBlockIfReplaceable(x + i, y + height, z + j, B_oak_leaves);
    }
  }

}
