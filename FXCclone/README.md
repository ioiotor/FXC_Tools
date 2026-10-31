# FXCclone（FX-C Cart Clone）

> English version: [README_EN.md](README_EN.md)

一台 FX-C 把**自己的外部串行 Flash（游戏 cart 镜像）**通过 **USB-C to USB-C 联机线**复制到另一台 FX-C，
不经过 PC。只复制**已使用区域**（从 Flash 末尾向前扫描，跳过全 0xFF 的 64 KB 块），
两边 Flash 型号/容量检查通过后，目标机才整片擦除，再逐页写入并校验。

> 联机线使用 **USB-C to USB-C** 线材，但实际是 USB-C **SBU1/SBU2** 上的 I2C，
> 不是普通 USB 数据/充电协议。请使用带 SBU 连接的全功能线；充电-only 线可能不通。

> **只复制外部 cart Flash，不复制 AVR 内部 bootloader/sketch。**
> 目标机的 AVR 引导程序不受影响，最坏情况可以重新烧 AVR 或重新 clone。
> 目标机的 cart 会被**整片擦除**，目标机原有的游戏/菜单会被覆盖。

## 快速操作（最短流程）

1. 两台 FX-C 用 USB 各烧一次 FXCclone；
2. **目标机**选 RECEIVE (TARGET) → A → 确认页 A（先检查 Flash，不会立刻擦除）；
3. **源机**选 SEND (SOURCE) → A；两边 Flash 一致后目标机自动按 64 KB 块擦除；
4. 目标擦除完成、源机显示 TARGET READY 后，按 **A:START COPY**；
5. 看到两边 COPY OK 即完成；按 B 返回。

> 完整说明和按键表见下文；复制前后**不要断电/拔线**。

## 1. 编译与烧录

- 目录：`FXCclone/`，用 **Arduboy FX-C** 板型编译；
- 依赖：Arduboy2、ArduboyI2C（与主诊断程序相同）；
- **两台机器必须烧同一份 `FXCclone.ino`**；
- 默认 I2C 200 kHz（`I2C_FREQUENCY 200000`）；如果链路不稳，把该宏改成
  `100000` 后两台一起重烧（速度约慢一倍）。

编译结果：**Flash 17116 / 29696 B（57%），RAM 1941 / 2560 B（75%）**。

## 2. 使用步骤

1. 两台 FX-C 烧好 FXCclone，连接 USB-C to USB-C 联机线；
2. **先操作目标机（RECEIVE）**：
   - 菜单选 `RECEIVE (TARGET)` → `A`；
   - 确认页按 `A`：目标先读自己的 Flash ID，**暂时不擦除**；
   - 显示 `CHECKING FLASH / CART NOT ERASED`，等待源机发来 Flash 信息；
3. **再操作源机（SEND）**：
   - 菜单选 `SEND (SOURCE)` → `A`；
   - 源机先读自己的 Flash ID，并持续发给目标机做比较；
   - 显示 `WAIT TARGET...`，**不会立即扫描/发送**；
   - 若两边 Flash 型号/容量一致：目标机自动开始整片擦除，完成后源机显示
     `TARGET READY`，此时按 `A:START COPY` 才开始扫描并复制；
   - 若不一致：目标机显示 `FLASH MISMATCH`，源机显示 `FLASH MISMATCH / COPY ABORTED`，
     **目标 cart 不会被擦除**；
4. 传输过程中两台都显示进度，**不要断电、不要拔线**；
5. 源机显示 `COPY OK`、目标机显示 `COPY OK` 即完成；
6. 按 `B` 返回菜单；两台重新上电/复位即可使用新 cart。

### 2.1 Flash 一致性检查

- 源机在进入 SEND 后读取自己的 JEDEC ID（如 `EF 40 18` = Winbond 16 MB）；
- 源机通过 I2C 把这个 ID + 容量发给目标机；
- 目标机读取自己的 JEDEC ID，逐项比较：
  - manufacturer / memory type / capacity 字节必须完全相同；
  - 容量必须相同；
- **一致**：目标机才开始整片擦除，之后才允许传输；
- **不一致**：目标机进入 `FLASH MISMATCH`，源机进入 `COPY ABORTED`，**不会擦除目标 cart**；
- 如果目标机连自己的 Flash ID 都读不到，会显示 `COPY FAILED`，同样不会擦除。

### 按键

| 状态 | 操作 |
| --- | --- |
| 菜单 | UP/DOWN 选择 SEND/RECEIVE，A 确认，B 无操作 |
| 目标确认页 | A = 进入 Flash 检查，B = 返回（此时还没擦除） |
| 目标 `CHECKING FLASH` | B = 退出（此时还没擦除） |
| 目标 `FLASH MISMATCH` | B = 返回（目标 cart 未被擦除） |
| 目标 `WAITING SOURCE` | B = 退出（此时 cart 已被擦除） |
| 目标 `DONE/FAIL` | B = 返回菜单 |
| 源机 `WAIT TARGET` | B = 中止 |
| 源机 `TARGET ERASING` | B = 中止 |
| 源机 `TARGET READY` | A = 开始扫描并复制，B = 中止 |
| 源机 `FLASH MISMATCH` | B = 返回（目标 cart 未被擦除） |
| 源机 `COPY OK/FAILED` | B = 返回菜单 |
| 传输中 | 不响应 B（避免中断破坏目标 cart） |

## 3. 实现方式

- 源机用**硬件 SPI（8 MHz）**读取自己的 Flash；
- 目标机用**硬件 SPI** 擦除/编程/回读校验；
- I2C 帧固定 48 字节（`I2C_BUFFER_CAPACITY=48`），每帧 40 字节有效数据；
- 每个 256 字节 Flash 页由 7 个 DATA 帧 + START/END 帧组成，目标逐页：
  - 校验帧/页 Fletcher-16；
  - 擦除后页编程；
  - 回读 256 字节比较；
  - 成功才 ACK，源机才继续；
- 目标先按 64 KB 块循环擦除（`0xD8`）整片 Flash（屏幕显示 `BLOCK n/总块、PROGRESS、EL/LEFT`），再写已用页；目标未使用区域保持 0xFF；
- 最后用 Fletcher-16 比较整段已用数据；
- 目标机 COPY 页显示 `PAGE 当前/总数`、`EL 已用时间`、`LEFT 预计剩余`
  （按已完成页平均耗时估算，页数太少时显示 `--`）；
- 源机扫描页显示 `PROGRESS/DONE/TOTAL/EL/LEFT`；目标擦除时源机显示 `TARGET ERASING` + `ERASE 当前块/总块数`；
- 源机发送页显示 `PROGRESS/PAGE/BYTES/EL/LEFT` 和 `DO NOT UNPLUG`；
- 源机结果页显示 `USED/PAGES/CHK/EL`。
- 目标机显示页数/字节进度。

## 4. 时间参考

按 **200 kHz**、48 字节帧、目标 60 Hz 主循环估算（实际受 Flash/线材/重试影响）：

| 已用数据 | 传输时间 |
| --- | --- |
| 1 MB | 约 2–3 分钟 |
| 4 MB | 约 8–12 分钟 |
| 8 MB | 约 18–25 分钟 |
| 16 MB | 约 35–50 分钟 |

另外：

- 源机扫描已用范围：最多约 15–25 秒（扫到 16 MB 时）；
- 目标整片擦除：约 40–90 秒（16 MB = 256 个 64 KB 块，逐块擦除并显示进度）；
- 100 kHz 时传输时间约翻倍。

## 5. 限制与风险

- **只复制外部 cart Flash**，不能复制 AVR bootloader/sketch；那需要 USB/ISP。
- **目标 cart 被整片擦除**；源机只读，不受影响。
- 两台 Flash 必须同型号/同容量（FX-C 通常是 W25Q128 16 MB）。目标会校验
  源机发来的已用长度是否超过自己的容量。
- **中断 = 目标 cart 不完整**：传输中拔线/断电/复位，目标卡在未完成状态；
  重新进入 RECEIVE 再完整拷贝一次即可（会再次整片擦除）。AVR 引导程序不受影响。
- 源机/目标机都必须有足够电量；建议满电。整片 16 MB 拷贝可能持续 30–50 分钟。
- 线反插/接触不良会表现为一直 `WAIT`；翻转线缆或换短线后重试。
- 目标机原 EEPROM 存档不会被覆盖（只有外部 Flash 被复制）。
- 请只用于自己拥有的机器/数据。

## 6. 失败后怎么恢复

- 目标 cart 损坏：重新跑一次 clone（目标 RECEIVE → 源 SEND）即可；
- 目标 AVR 程序仍在，可以正常进入 bootloader；
- 如果连 bootloader 都需要恢复，用 USB 按正常方式烧 AVR（与 cart 无关）。

## 7. 与 LINK TEST 的关系

- 主诊断程序的 `LINK TEST` 只测试 I2C 通信质量/帧长/速度，不写 Flash；
- `FXCclone` 是独立固件，专门做 cart 拷贝；
- 不要和主诊断程序或旧 `linktest` 混用协议。
