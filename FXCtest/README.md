# FXCtest（FX-C 全面检测）

> English version: [README_EN.md](README_EN.md)

一个跑在 Arduboy FX-C 上的硬件诊断应用：上电后直接进入菜单，提供屏幕、按键、声音、RGB LED、
FX 串行 Flash、EEPROM 和双机 I2C 联机等测试。用于检查 FX-C（或 mini 类使用 HWB 片选的硬件）。

> 依赖：Arduboy2 6.0.0、ArduboyI2C 3.0.0；Flash 测试使用程序内置的小型 SPI 驱动，不需要 ArduboyFX 库。
> LINK 联机测试把 `I2C_BUFFER_CAPACITY` 提高到 48（ArduboyI2C 默认 32），**两台机器必须烧同一版本**。
> 板包 `arduboy-homemade` 1.4.1，开发板 **Arduboy FX-C**（FQBN `arduboy-homemade:avr:arduboy-fxc`）。

---

## 1. 文件

| 路径 | 说明 |
| --- | --- |
| `FXCtest.ino` | 本应用主文件（Arduino 工程目录 `FXCtest`，程序标题 `FXCtest`） |
| `README.md` | 中文说明 |
| `README_EN.md` | English documentation |
| `../linktest/` | 旧的独立“双机联机测试”应用（协议与新版 v2 不兼容，仅作历史参考），可单独打开 `../linktest/linktest.ino` 烧录 |
| `../FXCclone/` | FXCclone：双机直连复制外部 Flash 已用区域的独立固件（不经过 PC）；目标 cart 会在 Flash 一致性检查通过后整片擦除，见 `../FXCclone/README.md`（中文）/ `../FXCclone/README_EN.md`（English） |

---

## 2. 编译与烧录

1. Arduino IDE 打开本目录（或直接用 arduino-cli 指向本目录）；
2. 开发板选择 **Arduboy FX-C**；
3. 库管理器安装 **Arduboy2**、**ArduboyI2C**；
4. 编译烧录。

arduino-cli 等价命令：

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  compile --fqbn arduboy-homemade:avr:arduboy-fxc --warnings all D:\fxctest\fsctest\FXCtest
```

> 如果你用的是 Cathy3K 的 **menu** 版 bootloader：上电先进 bootloader 菜单，在 loader 界面按 **A 或 B**
> 启动本程序。`game` 版 bootloader 上电会自动运行 sketch（按住 DOWN 进菜单）。

---

## 3. 主菜单与操作

```
FXCtest
> DISPLAY
  BUTTONS
  SOUND
  RGB LED
  FX FLASH
  EEPROM
  LINK TEST
UP/DN A:RUN
```

- **UP / DOWN**：移动光标；**A**：进入所选测试；**B**：从测试返回菜单。
- 菜单会滚动显示 7 项，右侧 `^`/`v` 表示还有项目。

各测试内部操作：

| 测试 | 操作 |
| --- | --- |
| DISPLAY | 自动切换图案（约 1.5 s/个）；A 下一个；最后一个图案按 A 记为通过并返回；B 直接返回 |
| BUTTONS | 按哪个键，哪个键的指示块实时填满，松开后保留已测标记；下方显示 `LAST` 键名。六个都检测到显示 `ALL PASS`，但**不自动返回**；按住 **A+B 2 秒** 退出 |
| SOUND | 进入时播放 3 声蜂鸣；A = 听到（通过），B = 没听到（失败），随后返回 |
| RGB LED | 自动循环 红/绿/蓝/白/灭；A = 正常（通过），B = 不可见（失败），随后返回 |
| FX FLASH | 只读测试；显示 `STEP 1/4 ... 4/4` 进度（SPI 为软件模拟，不会无限等待）；A = 重新读取，B = 返回 |
| EEPROM | 显示校验和；**A = 写/读/恢复测试**（会写最后一个字节并立即恢复），B = 返回 |
| LINK TEST | 见第 5 节；HOST 菜单 A = 运行全部、UP/DN 选 soak、B 退出；运行中 B = 中止；结果页 UP/DN 翻页、A 重测、B 退出 |

---

## 4. 各测试说明

### DISPLAY

8 个全屏图案：全亮、全灭、棋盘格、竖条纹、横条纹、边框/对角线、抖动渐变、反色。用于检查：

- 死像素、坏行/坏列、亮度不均、残影；
- SSD1306 驱动、OLED 复位/片选、SPI 链路；
- 反色命令是否正常（最后一张 `INVERT`）。

底部状态条属于界面，不参与图案判断。

### BUTTONS

实时显示六个按键（UP/DOWN/LEFT/RIGHT/A/B）：

- **当前按住**的按键：对应 6×6 指示块整块填满；
- **已经按过、当前松开**：保留边框和中心小点；
- **还没按过**：只有空心框；
- 下方 `LAST` 显示最近按键名；多键同时按下时显示 `MULTI`；
- 六个键都按到后显示 `ALL PASS`；不会自动退出，可以继续重复按任意键观察；
- **按住 A+B 2 秒** 退出测试。

有按键接触不良时，对应指示块不会响应；可多次按同一键确认。

### SOUND

用 `BeepPin1` 在扬声器引脚（Arduboy 标准为 Arduino D5/PC6）上播放 440 / 880 / 1760 Hz 三声蜂鸣，
并临时打开声音（`arduboy.audio.on()`），不写 EEPROM。听不到时检查扬声器、焊接和音频静音设置。

### RGB LED

循环红、绿、蓝、白、灭。部分 FX-C 批次可能没有装配 RGB LED，此时按 B 记为失败即可（不是主功能故障）。

### FX FLASH

只读测试，不会修改游戏/存档。SPI 用**软件模拟**（bit-bang，PB1/PB2/PB3），不依赖 ArduboyFX 库，
也没有会无限等待的硬件 SPI 等待，所以坏 Flash/错片选只会得到错误结果，不会把程序卡死：

- 测试期间会临时拉高 OLED 片选，结束后重新选中显示（Arduboy2 启动后一直保持显示选中，
  `display()` 不会重新选中；旧版本漏了恢复这一步，会表现为画面冻结在 `STEP 1/4`）；
- 进入时依次显示 `STEP 1/4 WAKEUP`、`STEP 2/4 JEDEC`、`STEP 3/4 IMAGE`、`STEP 4/4 STATUS`；
- JEDEC ID 与容量（容量由 JEDEC 的 size 字节计算；W25Q128JV 正常为 `EF 40 18` = 16 MB）；
- 状态寄存器 SR1/SR2/SR3（只读，不改写保护）；
- 菜单镜像签名 `ARDUBOY`；
- 当前板包的 Flash 片选配置（FX-C/mini 为 `PE2/HWB`），底部显示 `SPI: BITBANG`。

如果显示 `NO CHIP`：JEDEC 读到全 0/全 FF，通常是 Flash 未焊接、未供电、CS 不对，或 SPI 引脚/模式不对。
`IMAGE NONE` 只表示 Flash 里没有可启动的菜单镜像，硬件本身可能是好的。

### EEPROM

- 显示 1 KB EEPROM 的 Fletcher-16 校验和（只读）；
- 按 A 执行写测试：读取最后一个字节（地址 1023）→ 写入其反码 → 读回校验 → 写回原值 → 读回校验；
  两个方向都成功才算 PASS。
- **注意**：测试会短暂修改最后一个 EEPROM 字节并立即恢复；如果测试过程中断电，那个字节可能保持被改写的值。
  系统区（地址 0–15，含 bootloader/音频/单位名）不会被触碰；但最后一个字节可能被某个游戏的存档使用，
  请自行判断是否要在真机上运行写测试。

---

## 5. LINK TEST（双机 I2C 联机）

基于 **ArduboyI2C**，两台机器必须烧**同一份固件**。先进入 LINK TEST 的一台成为 **HOST（控制端）**，
另一台成为 **TARGET（从端，被动响应）**。

### 5.1 操作流程

1. 两台 FX-C 都进入 **LINK TEST**；程序先做线缆反插检测（`CABLE FLIPPED!` 时按提示翻转），然后握手。
2. HOST 菜单：
   - `A`：开始完整测试序列；
   - `UP/DOWN`：选 SOAK 时长（`NONE` / `1` / `5` / `10` 分钟）；
   - `B`：退出（通知 TARGET 后双方回主菜单）。
3. HOST 按 A 后自动依次执行：
   - **QUALITY**：3 秒 8 字节 ping/pong，统计成功率、重试、错误分类、RTT；
   - **MAX FRAME**：8/16/24/32/48 字节帧各 16 次，主机校验对端回显和 Fletcher-16；
   - **THROUGHPUT**：用 48 字节帧**连续突发**传 1 KB 有效载荷，算有效吞吐；
   - **SOAK**：按所选时长连续跑质量测试（`NONE` 跳过）；
   - **PEER INFO**：soak 结束后读对端版本、应用 CRC、运行时间、RX/BAD 计数、缓冲容量等；
   - **SPEED**：50/100/200 kHz 各 64 次（200 kHz 为实验档，可能失败；放最后，避免影响前面统计）。
4. TARGET 屏幕实时显示主机当前速度/帧长/命令、RX/BAD、链路状态；按 **B** 退出。
5. HOST 完成后进入 5 页结果（**UP/DOWN** 翻页，**A** 用相同设置重测，**B** 退出）：
   - 第 1 页 总览：PASS/FAIL、总成功率、TX/OK/LOST、RTT 最近/平均/最小/最大、抖动、最大连续失败；
   - 第 2 页 质量/错误：WR/RD/CRC/MAG/STA/SEQ/PAY 分项计数、重试、最后错误码、BUS/HSD；
   - 第 3 页 帧长/吞吐：各帧长成功数、有效吞吐 KB/s、1 KB 用时；
   - 第 4 页 速度/对端：50/100/200 kHz 成功数、对端 CRC/运行时间/RX/BAD/容量；
   - 第 5 页 RTT 柱状图。
6. 运行中按 **B** 中止，保留已测数据并进入结果页。

### 5.2 PASS 判定（HOST）

- QUALITY 成功率 ≥95% 且 ≥100 笔；
- MAX FRAME：8/16/24/32/48 五个长度全部 16/16；
- 1 KB 吞吐测试完成；
- 50 kHz 与 100 kHz 各 ≥58/64；
- PEER INFO 成功；
- 若选了 SOAK：soak 成功率 ≥95% 且 ≥100 笔；
- 200 kHz 只记录，不参与 PASS/FAIL。

结果页会显示 PASS/FAIL，成功率/帧长/速度等摘要也显示在 LINK 结果页内。

### 5.3 数据说明

- `RATE`：所有阶段合计成功率；`QUALITY` 只统计质量阶段。`SPD` 是**配置**的 I2C 时钟（50/100/200 kHz），
  不是用示波器量出来的 SCL 实际频率；每档成功率和 RTT 能反映真实通信质量。
- `RTT`：单笔 write+read 往返时间（含重试），单位 µs；`JIT` 是相邻成功 RTT 的平均绝对差。
- `BUS`：握手后 SDA/SCL 空闲电平（bit1=SDA，bit0=SCL，正常 `03`）；`HSD` 是从进入 LINK TEST 到握手完成的墙钟时间，
  包含等待另一台机器/插线的时间，所以几秒很正常；`FLIP` 是否出现反插提示。
- `MAX FRAME`：本程序把 `I2C_BUFFER_CAPACITY` 设为 **48**，所以单帧上限 48 字节（有效载荷 40 字节）；
  ArduboyI2C 默认只有 32。**两台机器必须同版本**。64 字节帧在当前 Arduino AVR 工具链的 LTO 下会编译失败，
  所以未启用；如果你自己改库/编译参数，可以再试 64。
- `THROUGHPUT` 用**连续突发**方式（不再是一帧/60Hz），所以 GOODPUT 反映总线能力而不是界面刷新率；
  只算有效载荷（每帧 48−8=40 字节），不含协议头/校验。
- `PEER`：对端上报的固件版本、应用 CRC、运行秒数、有效/坏帧数、缓冲容量、最后速度/命令。
- 每次 HOST 按 A 开始新测试时会发 RESET，TARGET 的 RX/BAD 计数清零，所以 PEER RX/BAD 对应**本轮**；
  `UP` 是 TARGET 开机秒数，不清零。PEER INFO 在 SOAK 结束后读取，所以 RX/BAD 包含本轮 soak 的帧。

### 5.4 注意事项

- **没有对端时会一直停在 `SEARCHING FOR ANOTHER UNIT`**：`I2C::handshake()` 本身没有超时，需要复位退出。
- 联机线要短（FX-C 只有 32U4 内部上拉）；50 kHz 更稳，100 kHz 推荐，200 kHz 在长线/劣质线上大量失败属正常。
- 线反插时只有后进入 LINK 的一台能检测到；若两台都卡住，复位其中一台。
- 新协议（v2：变长帧/命令/对端信息）与 `linktest/` 旧应用**不兼容**，不要混测。

---

## 6. 已知限制

- 本应用**不使用** `arduboy.begin()`，而是用 `arduboy.boot()` + 自己的启动画面：
  复位后立即显示 `FXCtest / STARTING...`，随后进入菜单；启动阶段只初始化屏幕/按键/音频，
  **不访问外部 Flash**，所以坏 Flash 不会阻止菜单启动；卡键也不会卡在“等待按键松开”。
  - 如果屏幕上**只有 Arduboy 图形/方块/游戏菜单**，说明当前运行的不是本 sketch（通常还在 bootloader 菜单里），
    需要先从 bootloader 启动本程序；menu 版 bootloader 在 loader 界面按 A 或 B 启动 sketch。
  - 如果一直停在 `STARTING...`，说明 app 已启动但卡在启动阶段。现在启动阶段理论上不会阻塞，请把现象反馈。
  - 如果 **FX FLASH** 显示 `NO CHIP`（JEDEC 全 0/全 FF），说明问题在 Flash/SPI 总线，而不是菜单/系统；
    其它测试仍可正常使用。
- **不测试电池、USB 数据/充电、电源按键**：FX-C 没有把电池电压接到可读 ADC；USB 功能需要在 PC 上确认。
- **RGB LED** 在部分批次可能没有装配，测试按“用户确认”判定。
- **EEPROM 写测试**会短暂改写最后一个字节；不需要时只读校验和即可。
- FX FLASH 测试**只读**；不测试写入/擦除，避免破坏游戏和存档。
- 与 menu 版 bootloader 搭配时，需要先从 bootloader 菜单启动本 sketch；这不是程序能控制的。
- **LINK TEST** 需要另一台运行同版本固件的机器；没有对端时 `I2C::handshake()` 无超时，会停在 `SEARCHING`。
- LINK TEST 的 **MAX FRAME 上限是 48 B**（`I2C_BUFFER_CAPACITY`=48）；64 B 帧在 AVR GCC 7.3 的 LTO 下会触发
  “operand out of range” 汇编错误，所以当前未启用。若要测 64 B，需要改库/工具链参数，且两端一致。
- **200 kHz** 只作为实验档记录；FX-C 只有内部上拉，长线时该档失败是正常现象。
- **SOAK 10 分钟**会让联机测试持续很久；只想快速验证时选 `NONE` 或 1 分钟。
- 联机测试期间会临时修改 `TWBR`；结束后会恢复 100 kHz。若中途断电，重新上电后从 100 kHz 开始。
- 本应用面向 FX-C/mini（Flash CS 在 PE2/HWB）。普通 Arduboy FX 把 Flash CS 放在 PD1/SDA 上，
  会与 I2C 冲突，联机测试不适用。

---

## 7. 参考资料

- ArduboyI2C 库与文档：<https://github.com/sub1inear/ArduboyI2C>，<https://sub1inear.github.io/ArduboyI2C/>
- Arduboy2 库：<https://github.com/MLXXXp/Arduboy2>
- 板包（Arduboy FX-C）：`arduboy-homemade`（Arduino 开发板管理器安装）

## 8. 许可

本示例代码可自由使用、修改和分发（MIT 许可）。依赖的 Arduboy2、ArduboyI2C 库遵循各自的许可证。
