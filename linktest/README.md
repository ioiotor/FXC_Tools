# Arduboy FX-C 双机通信检测（I2C Link Tester）

> 本目录是独立保留的“双机联机测试”应用；全面检测应用在上一级目录（`..\fsctest.ino`）。

用 **ArduboyI2C** 库检测两台 **Arduboy FX-C** 之间的 I2C 联机通信是否正常。
同一份 `linktest.ino` 烧录到两台机器，接上 FX-C 联机线后开机即可：程序会自动完成握手、分配主机/从机，
然后持续进行带校验的双向 ping-pong 测试，并在两块 OLED 上实时显示通信状态、往返时延和丢包统计。

> 适用硬件：Arduboy FX-C（ATmega32u4，128×64 OLED，带可反插的 I2C 联机线）。
> 依赖库：Arduboy2 6.0.0、ArduboyI2C 3.0.0（sub1inear）。
> 本工程已在 `arduboy-homemade:avr:arduboy-fxc`（Arduboy FX-C 板型）下编译通过：
> Flash 12018 / 29696 字节（40%），RAM 1341 / 2560 字节（52%）。尚未在真机上验证。

---

## 1. 它能检测什么

| 检测项 | 说明 |
| --- | --- |
| 握手 / 角色分配 | 两台机器谁做主机（HOST / Controller）、谁做从机（GUEST / Target） |
| 线缆反插 | FX-C 联机线可以反插；被插反的一端会提示 `CABLE FLIPPED!` 并要求翻转 |
| 双向数据通路 | 主机发 8 字节 ping，从机回 8 字节 pong，校验 magic、版本、序号、随机 cookie、Fletcher-16 校验和 |
| 通信质量 | 每笔事务的失败原因（地址 NACK、数据 NACK、读 NACK、总线错误、校验错、序号错等） |
| 往返时延 | 最近 / 平均 / 最小 / 最大 RTT（微秒），由主机用 `micros()` 测量 |
| 丢包与重试 | LOST（3 次尝试全失败）、RTY（事务内额外重试次数） |
| 最近 32 包曲线 | 屏幕底部 32 格：实心 = 成功，空心 = 失败，最右为最新 |
| 断线恢复 | 主机 2 秒无有效数据自动重新握手；从机失联时周期检测线缆方向 |

---

## 2. 文件与依赖

| 文件 | 说明 |
| --- | --- |
| `linktest.ino` | 完整程序（Arduino 工程主文件，直接打开 `D:\fxctest\fsctest\linktest` 即可） |
| `README.md` | 本说明 |

在 Arduino IDE 的库管理器中安装：

- **Arduboy2** ≥ 6.0.0（作者 Scott Allen 等）
- **ArduboyI2C** 3.0.0（作者 sub1inear）

开发板包：安装提供 **Arduboy FX-C** 板型的 `arduboy-homemade` AVR 板包，
然后在 *工具 → 开发板* 中选择 **Arduboy FX-C**（FQBN：`arduboy-homemade:avr:arduboy-fxc`）。
如果你的板包只有 “Arduboy FX”，也可以先用 FX 板型试编译（I2C 引脚相同），但联机线反插检测与 FX-C 相关。

---

## 3. 硬件连接与开机

```
   FX-C #1                                  FX-C #2
  ┌─────────┐        FX-C 联机线           ┌─────────┐
  │  LINK   │◄════════════════════════════►│  LINK   │
  └─────────┘                              └─────────┘
```

1. 用 FX-C 原装联机线（I2C link cable）连接两台机器的联机接口。
2. **两台都烧录同一份 `linktest.ino`**。
3. 推荐先开一台，等它显示 `SEARCHING FOR ANOTHER UNIT`，再开另一台；
   两台几乎同时开机也可以，但先开一台时线缆反插检测最可靠。
4. 如果某台显示 `CABLE FLIPPED!`，按屏幕提示把**这台机器上的**联机线插头翻转 180° 重新插入即可，
   程序会自动继续。
5. 如果你刷的是 Cathy3K 的 **menu** 版 bootloader（上电先进 bootloader 菜单），需要在**两台**机器上
   分别从菜单启动本 sketch；`game` 版 bootloader 则上电自动运行 sketch（按住 DOWN 才进菜单）。
   详见第 9 节。

> 引脚参考：ArduboyI2C 使用 ATmega32u4 的 TWI（`PD0` = Arduino D3 = SCL，`PD1` = Arduino D2 = SDA），
> 并在 `I2C::begin()` 中打开内部上拉。FX-C 只有软件上拉，因此库把总线频率限制在 200 kHz 以内。
> 若自制联机线：至少需要连接 GND、SDA、SCL 三根线（库使用内部上拉），不要接 5V；
> FX-C 联机接口的具体针脚定义请以官方资料为准。

---

## 4. 编译与烧录

### Arduino IDE

1. 打开 `linktest.ino`；
2. 开发板选择 **Arduboy FX-C**；
3. 依次给两台机器插 USB 烧录（烧录时可先不接联机线）；
4. 两台都烧录完成后按第 3 节连接并开机。

### arduino-cli（可选，本工程就是用这条命令验证的）

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  compile --fqbn arduboy-homemade:avr:arduboy-fxc --warnings all D:\fxctest\fsctest\linktest
```

---

## 5. 屏幕说明

### 主机（HOST / Controller）

```
HOST        LINK:OK █
TX 12345      OK 12344
LOST 1        RTY 2
RTT 2100      AVG 2088
MIN 1990      MAX 2600
ERR NONE
■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■   ← 最近 32 笔事务
A:RELINK B:RESET
```

| 字段 | 含义 |
| --- | --- |
| `LINK:OK` / `LINK:LOST` | 750 ms 内是否收到过有效 pong；LOST 时右侧方块闪烁 |
| `TX` | 已发起的事务数（每帧 1 笔，约 60 笔/秒） |
| `OK` | 收到并通过全部校验的 pong 数，正常时应与 TX 基本相等 |
| `LOST` | 3 次尝试全部失败的事务数 |
| `RTY` | 事务内额外重试次数（偶发正常，持续增长说明链路不稳） |
| `RTT` / `AVG` / `MIN` / `MAX` | 往返时延，单位微秒（100 kHz 下 8+8 字节正常约 1.7–3 ms，即 1700–3000 µs） |
| `ERR` | 最近一次失败原因；正常为 `NONE` |
| 底部 32 格 | 最近 32 笔事务：实心=成功，空心=失败，最右为最新 |
| 控制 | `A:RELINK` 重新握手；`B:RESET` 清零统计 |

### 从机（GUEST / Target）

```
GUEST       LINK:OK █
RX 12344      BAD 0
PRB 0         SEQ 1F
QUAL 100%
HOST PINGS OK
ADDR 08  LIB 3.0
■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
B:RESET
```

| 字段 | 含义 |
| --- | --- |
| `LINK:OK` / `LINK:LOST` | 750 ms 内是否收到过有效 ping |
| `RX` | 收到的有效 ping 数 |
| `BAD` | 无效 ping / 意外写入数（校验错、长度错等） |
| `PRB` | 收到的握手探测字节（`0x00`）数；启动阶段和主机重新握手时会短暂增加，正常联机后不再增长 |
| `SEQ` | 最近一次有效 ping 的序号（十六进制） |
| `QUAL` | 最近 32 个包的成功率；无数据时显示 `--` |
| `HOST PINGS OK` / `HOST SILENT` / `WAITING FOR HOST` | 当前链路状态 |
| `ADDR 08  LIB 3.0` | 从机地址（I2C 7 位地址 0x08）与 ArduboyI2C 库版本 |
| 控制 | `B:RESET` 清零统计 |

> 计数类字段在屏幕上最多显示 5 位数（`99999`），超过后显示饱和值，但内部计数仍在累加。

---

## 6. 检测原理

### 6.1 握手：谁是主机

两台机器启动后都调用 `I2C::handshake()`：

- 若检测到总线忙（另一台已经在发握手信号），本机把地址设为 `I2C::targetAddress`（0x08），成为 **从机**；
- 若总线空闲，本机成为 **主机**，并持续向 0x08 写单字节 `0x00`，直到收到 ACK 为止；
- 先开机的那台通常会先成为主机，后开机的成为从机；之后双方进入数据阶段。

### 6.2 线缆反插检测

FX-C 联机线两端可以反向插入。`I2C::checkCableFlipped()` 会在握手期间采样 SDA/SCL 的边沿数量：
正常方向时 SCL 的边沿明显多于 SDA，反插时两者对调。检测到反插的一端会显示 `CABLE FLIPPED!` 并阻塞等待用户翻转，
翻转后自动继续握手。

本程序还做了一个额外保险：**从机在失联期间每 1.5 秒重新检查一次线缆方向**。
这样即使两台机器几乎同时开机、初始检测窗口刚好错过，也能在几秒内发现反插并提示，而不会一直卡在等待。

### 6.3 数据协议（每包 8 字节）

主机 → 从机 PingPacket：

| 偏移 | 字段 | 内容 |
| --- | --- | --- |
| 0 | magic | `0xA5` |
| 1 | version | 协议版本 `0x01` |
| 2 | seq | 递增序号（8 位回绕） |
| 3 | flags | 保留，`0` |
| 4–5 | cookie | 由 seq 派生的 16 位随机值（小端） |
| 6–7 | checksum | 前 6 字节的 Fletcher-16（小端） |

从机 → 主机 PongPacket：

| 偏移 | 字段 | 内容 |
| --- | --- | --- |
| 0 | magic | `0x5A` |
| 1 | version | 协议版本 `0x01` |
| 2 | seq | 回显 ping 的序号 |
| 3 | status | `0`=OK，`1`=尚未收到有效 ping |
| 4–5 | cookie | 回显 ping 的 cookie |
| 6–7 | checksum | 前 6 字节的 Fletcher-16 |

主机校验顺序：I2C 写错误 → I2C 读错误 → magic/version → checksum → status → seq → cookie。
任何一步失败，本笔事务最多重试 3 次；3 次都失败记入 `LOST`，并把最后一次原因显示在 `ERR`。

主机先写 ping 再立刻读 pong。从机在 `onReceive` 中解析并保存 ping，在 `onRequest` 中回 pong。
如果主机读得太早（从机中断还没处理完），这次读会 NACK，主机立即重试整笔事务；
因此偶发的 `RTY` 是正常现象，不属于故障。

### 6.4 断线恢复

- 主机 750 ms 未收到有效 pong → `LINK:LOST`；持续 2 s → 自动重新 `handshake()`，屏幕显示 `SEARCHING FOR ANOTHER UNIT`。
- 从机只负责监听/响应，不主动重新握手（避免两台机器同时变成主机而“撞车”）。
- 恢复连接后，主机把对端 ACK 的握手信号视为重新上线，继续 ping-pong；
  从机的 `PRB` 会记录这些握手探测字节。
- 也可随时按主机上的 `A`（RELINK）手动触发重新握手。

### 6.5 ISR 约束

`onReceive` / `onRequest` 在 I2C 中断里执行（中断关闭），因此回调内不能调用 `millis()`、`delay()`、`Serial`。
本程序在回调里只做校验、保存和 `I2C::reply()`，计数通过 `volatile uint8_t` 增量交给主循环合并，
主循环合并时用 `cli()/SREG` 保护。

---

## 7. 结果判读与常见问题

### 正常现象

- 两台分别显示 `HOST` 和 `GUEST`，双方均为 `LINK:OK`；
- 主机 `OK ≈ TX`，`LOST` 基本为 0，`RTY` 偶发；
- `RTT` 约 1700–3000 µs（100 kHz 总线，8+8 字节，含从机中断响应）；
- 底部 32 格基本全为实心；
- 从机 `RX` 与主机 `TX` 接近，`BAD 0`，`QUAL 100%`，`PRB` 为 0 或很小
  （`TX` 统计事务数，`RX` 只统计不同序号的有效 ping，同一次重试不会重复计数，因此两者不必严格相等）。

### 故障排查

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 两台都停在 `SEARCHING FOR ANOTHER UNIT` | 未接联机线 / 对方未开机 / 对方没运行本程序；或两台几乎同时上电、都成了 Controller；或线插反且双方都没检测到 | 检查联机线；**复位其中一台**（另一台保持在 SEARCHING），让先启动的一台重新搜索；若怀疑线插反，先翻转其中一端的插头再复位一台 |
| 一台 `HOST`、一台 `GUEST`，但 HOST `LINK:LOST`、`ERR WRITE_ADDR_NACK` | 从机没有在监听（程序未运行、已复位或地址未设好） | 复位从机；或按主机 `A` 重新握手 |
| 主机 `ERR READ_ADDR_NACK` 持续出现 | 写成功读失败：接触不良、从机被反复中断、线太长 | 重新插紧联机线；缩短/更换线缆 |
| `BAD_CHECKSUM` / `BAD_COOKIE` / `BAD_SEQ` 偶发 | 电气干扰、接触电阻、线缆过长、频率过高 | 重试通常可恢复；持续出现则检查线材与插头 |
| `RTY` 持续增长、`RTT` 抖动大 | 联机线接触不良或干扰 | 清洁/更换联机线，确保插头到位 |
| 短 USB 线正常、长 USB 线失败 | FX-C 只有约 20–50 kΩ 内部上拉；长线电容让 SDA/SCL 上升沿变慢、超出 100 kHz 的时序余量 | 换短的全功能被动 USB-C 线；自制板加 2.2–4.7 kΩ 上拉到 3.3 V；或两端一致地降低 I2C 频率 |
| 显示 `CABLE FLIPPED!` | 联机线反插 | 按提示翻转**本机**的插头 |
| 从机 `PRB` 增加但 `RX` 不增加 | 主机正在重新握手 | 等 1–2 秒；或按主机 `A` |
| Arduino IDE 找不到 Arduboy FX-C 板型 | 未安装 `arduboy-homemade` 板包 | 安装板包后重新选择开发板 |
| 编译报错找不到 ArduboyI2C.h | 未安装库 | 库管理器搜索安装 ArduboyI2C（sub1inear） |
| 只开了一台机器 | 程序会停在 `SEARCHING`（`handshake()` 没有超时） | 这是预期行为；接上第二台或断电重启 |

### 已知限制

- 本程序只能和**同样运行本程序**的另一台 FX-C 通信（协议是自定义的，不兼容其它联机游戏）。
- `I2C::handshake()` 本身没有超时：没有对端时会一直停在 SEARCHING，需要断电/复位退出。
  两台同时上电并都进入“等待”时，复位其中一台即可恢复（另一台保持 SEARCHING，等它重新启动后完成握手）。
- I2C 事务是阻塞的、库本身也没有总线超时。正常拔掉联机线时主机会快速 NACK 并自动重连；
  但如果拔插过程中恰好把 SDA/SCL 卡在低电平，极少数情况下可能卡死，此时断电重启即可。
- `RTT` 是整笔事务（写 ping + 读 pong）的耗时，包含事务内重试的时间，并按 65535 µs 上限截断；
  因此偶发一次重试会让该次 RTT 偏高，`MIN/AVG` 也包含这些重试样本，不是“纯单次传输延迟”。
- 屏幕计数最多显示 `99999`；`RTT` 显示上限 65535 µs（超出按上限显示）。
- 本次交付只做了编译验证（见文件头），未在真机运行；如真机行为与第 6 节推断不符，
  请优先检查库版本（需 3.0.0）与接线。
- 不建议修改 `I2C_FREQUENCY`：FX-C 只有内部软件上拉，库要求 ≤ 200 kHz，且线缆反插检测的时序按默认
  100 kHz 调整。确需尝试时，在 `#include <ArduboyI2C.h>` 之前定义，例如 `#define I2C_FREQUENCY 200000`。
- 联机线长度没有保证：官方 FX-C 只靠 32U4 内部上拉（约 20–50 kΩ）和短线，长线/劣质线可能失败。
  自制板建议预留 2.2–4.7 kΩ 的 SDA/SCL 上拉焊盘；长线场景不要依赖内部上拉。

---

## 8. 可调参数

都在 `linktest.ino` 顶部，按需修改：

| 宏/常量 | 默认 | 说明 |
| --- | --- | --- |
| `CONTROLLER_MAX_ATTEMPTS` | 3 | 每笔事务的最大尝试次数 |
| `LINK_LOST_MS` | 750 | 多久没有有效数据判定 `LINK:LOST` |
| `AUTO_RELINK_MS` | 2000 | 主机失联多久后自动重新握手 |
| `TARGET_FLIP_CHECK_MS` | 1500 | 从机失联时多久检查一次线缆方向 |
| `PROTOCOL_VERSION` | 1 | 协议版本号（两台必须一致） |
| `I2C_FREQUENCY` | 100000 | 库频率宏，需在 include 前定义（不建议改） |

---

## 9. 关于 bootloader（常见问题）

**ArduboyI2C 不检测、也不依赖 bootloader。** 库只使用 ATmega32u4 的 TWI 硬件和定时器，不读取 bootloader
版本/保留区/`MCUSR`，不调用 `exitToBootloader()`，也不使用 flash 菜单或 `ArduboyFX` 库。本程序同样不读
bootloader 的 0x7FFC 版本向量。bootloader 只影响两件事：**能不能把 sketch 启动起来**，以及**能不能上传**。

- **运行**：只要 bootloader 能把 app 正常启动，`arduboy.begin()` / `I2C::begin()` 会重新初始化显示、
  按键、TWI 和引脚，bootloader 里的改动（USB 描述符字符串、开机 logo、编程器 ID 等）对 I2C 联机测试没有影响。
- **上传**：Arduino IDE 靠 bootloader 的 USB VID/PID（FX-C 是 `0x2341/0x0036`，与板包一致）识别端口并
  用 AVR109/1200bps touch 上传；只改字符串/logo 不影响上传，改 VID/PID 才会让 IDE 找不到端口
  （运行本身仍不受影响，只是要手动指定端口/avrdude）。
- **menu / game 变体**：Cathy3K 的 `menu` 版上电总是进 bootloader 菜单（初始是 loader 界面），
  在 loader 界面按 **A 或 B** 会运行 MCU 里已经烧录的 sketch；`game` 版上电直接运行 sketch
  （按住 DOWN 才进菜单）。联机测试建议用 **game** 版，省去两台机器分别启动的步骤；
  用 menu 版时两台都要进入本 sketch 后才会握手。
- **boot 区大小 / fuse**：FX-C 板包使用 3K boot 区布局（`0x7400–0x7FFF`，app 上限 29696 字节），
  fuse 为 LFUSE `0xFF` / HFUSE `0xD2` / EFUSE `0xCB`。只要 bootloader 仍 ≤ 3K 且 fuse 不变，
  本 sketch（12018 字节）远小于 app 上限，不需要改板包的 `upload.maximum_size`。
  如果自行增大了 bootloader 或改了 BOOTSZ，必须同步修改板包的 app 上限和 fuse，否则会覆盖 boot 区或无法启动。
- **变体匹配**：FX-C 板包用的是 `arduboymini-bootloader-*.hex`（Arduboy Mini 变体）。自制 bootloader 时
  要用同样的 ARDUBOYMINI/menu 或 game 变体；`sda`、`ssd132x`、`st7565` 等是给其它接线/显示屏用的，
  烧到 FX-C 上虽然一般不影响 app 运行，但 bootloader 菜单可能黑屏/花屏。
- **修改 bootloader 的一个实际好处**：menu 版由人工先后启动两台机器，天然错开了握手时机，反而能避免
  “两台几乎同时上电、都成为 Controller”的极端情况（本程序在 SEARCHING 画面也提示 `STUCK? RESTART ONE`）。

---

## 10. 参考资料

- ArduboyI2C 库与文档：<https://github.com/sub1inear/ArduboyI2C>，<https://sub1inear.github.io/ArduboyI2C/>
- Arduboy2 库：<https://github.com/MLXXXp/Arduboy2>
- 板包（Arduboy FX-C 板型）：`arduboy-homemade`（Arduino 开发板管理器安装）

## 11. 许可

本示例代码可自由使用、修改和分发（MIT 许可）。依赖的 Arduboy2、ArduboyI2C 库遵循各自的许可证。
