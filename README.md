# ESP32MC Server

一个跑在 ESP32S3 上的极简 Minecraft Java 服务器。

项目现在主要面向 Arduino ESP32S3，协议版本是 `26.1.2 / 775`。核心逻辑基本都还是 C，网络层用一层兼容代码接到 `WiFiServer / WiFiClient` 上。

代码部分参考 https://github.com/p2r3/bareiron
## 现在有的东西

- 玩家登录、出生、移动、聊天
- 区块生成、基础地形、生物群系
- 方块放置、破坏、简单流体
- 背包、合成、熔炉
- 基础 Mob 刷新和行为
- 可选世界数据落盘
- 串口 / Web 配网

这不是完整原版服，目标也不是兼容插件生态。它更像一个能在 ESP32 上自己跑起来的实验性生存服。

## 运行方式

默认入口是 [code.ino](code.ino)。

大致流程：

1. 用 Arduino IDE 或兼容的 ESP32S3 构建环境打开这个目录
2. 选择 ESP32S3 开发板
3. 编译并烧录
4. 设备启动后先连 WiFi
5. 连上以后服务器监听 `25565`

如果没配好 WiFi，可以走 [wifi_config.cpp](wifi_config.cpp) 里的配网流程。

## 主要文件

- [code.ino](code.ino): Arduino 入口，连 WiFi，拉起主任务
- [main.c](main.c): 主循环、连接管理、收包分发
- [packets.c](packets.c): 协议收发
- [procedures.c](procedures.c): 玩家、方块、Mob、Tick 逻辑
- [worldgen.c](worldgen.c): 地形和区块生成
- [crafting.c](crafting.c): 合成和熔炉
- [serialize.c](serialize.c): 世界数据读写
- [arduino_compat.cpp](arduino_compat.cpp): 把 Arduino 网络接口接到现有 C 代码
- [globals.h](globals.h): 主要开关和常量

## 一些说明

- 很多功能都靠编译开关控制，先看 [globals.h](globals.h)
- `registries.c / registries.h` 体积很大，是协议注册表数据
- 仓库里有构建产物和调试文件，不全是源码

## 当前定位

这个项目优先考虑：

- 在 ESP32S3 上能跑
- 代码结构尽量直接
- 出问题时方便查

不优先考虑：

- 完整原版特性
- 高并发
- 插件兼容
- 漂亮的工程包装
