# FXCtest (FX-C Hardware Diagnostic)

FXCtest is a hardware diagnostic application for the Arduboy FX-C (and mini-style
hardware using the HWB flash chip select). It boots directly into a menu and
provides display, button, sound, RGB LED, FX serial-flash, EEPROM and two-unit
I2C link tests. It is intended for checking FX-C units.

> Dependencies: Arduboy2 6.0.0 and ArduboyI2C 3.0.0. The flash test uses a small
> built-in SPI driver and does not require the ArduboyFX library.
> The LINK TEST raises `I2C_BUFFER_CAPACITY` to 48 (ArduboyI2C defaults to 32).
> **Both units must run the same build.**
> Board package: `arduboy-homemade` 1.4.1, board **Arduboy FX-C**
> (FQBN `arduboy-homemade:avr:arduboy-fxc`).

Build result: **Flash 21960 / 29696 B (73%), RAM 1665 / 2560 B (65%)**
(895 B free stack space).

---

## 1. Files

| Path | Description |
| --- | --- |
| `FXCtest.ino` | Main application file (Arduino project directory `FXCtest`) |
| `README.md` | Chinese documentation |
| `README_EN.md` | This English documentation |
| `../linktest/` | Old standalone two-unit link test app (incompatible with the new v2 protocol; historical reference only) |
| `../FXCclone/` | FXCclone: standalone cart-clone firmware; copies the used external-flash area over I2C and erases the target cart only after a flash-identity check |

---

## 2. Build and Flash

1. Open this directory in the Arduino IDE (or point arduino-cli at it).
2. Select board **Arduboy FX-C**.
3. Install **Arduboy2** and **ArduboyI2C** from the Library Manager.
4. Compile and upload.

Equivalent arduino-cli command:

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  compile --fqbn arduboy-homemade:avr:arduboy-fxc --warnings all D:\fxctest\fsctest\FXCtest
```

> With a Cathy3K **menu** bootloader: power on into the bootloader menu, then
> press **A or B** in the loader screen to start this application.
> With a `game` bootloader, the sketch runs automatically at power-on
> (hold DOWN to enter the menu).

---

## 3. Menu and Controls

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

- **UP / DOWN**: move the cursor; **A**: enter the selected test; **B**: return
  from a test to the menu.
- The menu shows 7 items and scrolls; `^`/`v` on the right indicates more items.

Per-test controls:

| Test | Controls |
| --- | --- |
| DISPLAY | Patterns advance automatically (~1.5 s each); A = next; pressing A on the last pattern marks the test passed and returns; B = return |
| BUTTONS | The indicator for the pressed button fills in real time; a released but already-seen button keeps a dot; `LAST` shows the most recent button name. After all six are seen it shows `ALL PASS` but **does not auto-return**; hold **A+B for 2 s** to exit |
| SOUND | Plays three beeps on entry; A = heard (pass), B = not heard (fail), then returns |
| RGB LED | Cycles red/green/blue/white/off; A = visible (pass), B = not visible (fail), then returns |
| FX FLASH | Read-only test; shows `STEP 1/4 ... 4/4` progress (bit-banged SPI has no unbounded waits); A = rescan, B = return |
| EEPROM | Shows the checksum; **A = write/read/restore test** (writes the last byte temporarily and restores it); B = return |
| LINK TEST | See section 5; HOST menu: A = run all, UP/DN = select soak, B = exit; during a run B = abort; result pages: UP/DN = page, A = rerun, B = exit |

---

## 4. Test Details

### DISPLAY

Eight full-screen patterns: all-on, all-off, checkerboard, vertical stripes,
horizontal stripes, border/diagonal, dither gradient and inverted. Useful for:

- dead pixels, bad rows/columns, uneven brightness and ghosting;
- SSD1306 driver, OLED reset/chip select and SPI wiring;
- inverted-display command handling (the final `INVERT` pattern).

The bottom status bar is part of the UI and is not part of the pattern test.

### BUTTONS

Live display of UP/DOWN/LEFT/RIGHT/A/B:

- **Currently held button**: its 6×6 indicator is filled solid.
- **Seen and released**: the outline plus a small center dot remains.
- **Not seen yet**: empty outline only.
- `LAST` below the grid shows the most recently pressed button; simultaneous
  multiple buttons show `MULTI`.
- After all six buttons have been seen it shows `ALL PASS`, but it does not
  auto-exit; you can keep pressing any button, including already-tested ones.
- Hold **A+B for 2 seconds** to exit.

A faulty button will not light its indicator; press the same button several
times to confirm.

### SOUND

`BeepPin1` plays three beeps (440 / 880 / 1760 Hz) on the speaker pin
(Arduino D5/PC6 on standard Arduboy hardware) and temporarily enables audio
(`arduboy.audio.on()`); EEPROM is not written. If nothing is heard, check the
speaker, its soldering and the audio-mute setting.

### RGB LED

Cycles red, green, blue, white and off. Some FX-C batches may not have an RGB
LED fitted; in that case pressing B to mark it as not visible is acceptable and
does not indicate a core failure.

### FX FLASH

Read-only test; it does not modify games or saves. SPI is **bit-banged**
(PB1/PB2/PB3) and does not use the ArduboyFX library, so a faulty flash or
wrong chip select produces an error rather than a hang:

- The OLED chip select is raised during the test and restored afterwards
  (Arduboy2 leaves the display selected after boot; an older version missed
  this step and appeared to freeze at `STEP 1/4`).
- On entry it shows `STEP 1/4 WAKEUP`, `STEP 2/4 JEDEC`, `STEP 3/4 IMAGE` and
  `STEP 4/4 STATUS`.
- JEDEC ID and capacity (capacity comes from the JEDEC size byte; a W25Q128JV
  normally reports `EF 40 18` = 16 MB).
- Status registers SR1/SR2/SR3 (read-only; write protection is not changed).
- Menu image signature `ARDUBOY`.
- The board's flash chip-select configuration (FX-C/mini uses `PE2/HWB`);
  the bottom line shows `SPI: BITBANG`.

`NO CHIP` means JEDEC read all zeros/all 0xFF: usually the flash is not fitted,
not powered, has the wrong chip select, or the SPI pins/mode are wrong.
`IMAGE NONE` only means there is no bootable menu image in flash; the hardware
may still be fine.

### EEPROM

- Shows a Fletcher-16 checksum of the complete 1 KB EEPROM (read-only).
- Press A to run the write test: read the last byte (address 1023), write its
  inverse, read it back, restore the original value, read it back again. Both
  directions must succeed for a PASS.
- **Note**: the test briefly modifies the last EEPROM byte and restores it. If
  power is lost during the test that byte may remain modified. The system area
  (addresses 0–15, including bootloader/audio/unit-name settings) is not
  touched, but the last byte may belong to a game save; decide whether to run
  the write test on real hardware.

---

## 5. LINK TEST (two-unit I2C)

Based on **ArduboyI2C**. Both units must run the **same firmware**. The first
unit to enter LINK TEST becomes the **HOST (controller)**; the other becomes
the **TARGET (passive responder)**.

### 5.1 Procedure

1. Put both FX-C units into **LINK TEST**. The app first checks for a flipped
   cable (follow the `CABLE FLIPPED!` prompt), then performs the handshake.
2. HOST menu:
   - `A`: start the full test sequence;
   - `UP/DOWN`: select SOAK duration (`NONE` / `1` / `5` / `10` minutes);
   - `B`: exit (notify TARGET, then both return to the main menu).
3. After A, the HOST runs:
   - **QUALITY**: 3 seconds of 8-byte ping/pong, collecting success rate,
     retries, error categories and RTT;
   - **MAX FRAME**: 16 attempts each at 8/16/24/32/48-byte frames; the host
     verifies the echo and Fletcher-16;
   - **THROUGHPUT**: continuous 48-byte frames carrying 1 KB of payload,
     measuring effective throughput;
   - **SOAK**: the selected duration of continuous quality testing (`NONE`
     skips it);
   - **PEER INFO**: after soak, read the peer's version, app CRC, uptime,
     RX/BAD counts, buffer capacity and related values;
   - **SPEED**: 64 attempts each at 50/100/200 kHz (200 kHz is experimental and
     may fail; it runs last so earlier statistics are not affected).
4. The TARGET displays the host's current speed/frame/command, RX/BAD counters
   and link status; press **B** to exit.
5. After completion, the HOST shows 5 result pages
   (**UP/DOWN** = page, **A** = rerun with the same settings, **B** = exit):
   - page 1 overview: PASS/FAIL, overall rate, TX/OK/LOST, RTT
     last/avg/min/max, jitter, longest consecutive-failure run;
   - page 2 quality/errors: WR/RD/CRC/MAG/STA/SEQ/PAY counters, retries, last
     error, BUS/HSD;
   - page 3 frame sizes/throughput: per-size successes, effective KB/s,
     1 KB time;
   - page 4 speeds/peer: 50/100/200 kHz successes and peer CRC/uptime/RX/BAD/
     capacity;
   - page 5 RTT histogram.
6. Press **B** during a run to abort; measured data is retained and the result
   pages are shown.

### 5.2 PASS Criteria (HOST)

- QUALITY success rate ≥95% and ≥100 transactions;
- MAX FRAME: all five sizes (8/16/24/32/48) achieve 16/16;
- 1 KB throughput test completes;
- 50 kHz and 100 kHz each achieve ≥58/64;
- PEER INFO succeeds;
- if SOAK was selected: soak success rate ≥95% and ≥100 transactions;
- 200 kHz is recorded but does not affect PASS/FAIL.

The result pages show PASS/FAIL and the rate/frame/speed summary in the LINK
result pages.

### 5.3 Data Notes

- `RATE`: success rate over all phases; `QUALITY` only covers the quality phase.
  `SPD` is the **configured** I2C clock (50/100/200 kHz), not a scope-measured
  SCL frequency; each speed's success rate and RTT reflect real link quality.
- `RTT`: one write+read round trip including retries, in µs; `JIT` is the mean
  absolute difference between adjacent successful RTTs.
- `BUS`: idle SDA/SCL levels after the handshake (bit1=SDA, bit0=SCL; normal
  `03`). `HSD` is wall-clock time from entering LINK TEST to handshake
  completion, including waiting for the other unit/cable, so a few seconds is
  normal. `FLIP` indicates a flipped-cable prompt.
- `MAX FRAME`: this app sets `I2C_BUFFER_CAPACITY` to **48**, so the maximum
  frame is 48 bytes (40 bytes of payload); ArduboyI2C defaults to 32. **Both
  units must use the same build.** 64-byte frames fail to compile under the
  current Arduino AVR toolchain with LTO, so they are disabled; changing the
  library/build options may allow 64.
- `THROUGHPUT` uses continuous bursts (not one frame per 60 Hz refresh), so
  GOODPUT reflects bus capability rather than UI refresh. Only payload bytes
  count (48−8=40 per frame), excluding protocol headers/checksums.
- `PEER`: peer firmware version, app CRC, uptime, valid/bad frame counts,
  buffer capacity, last speed/command.
- Each new HOST run sends RESET, clearing TARGET RX/BAD counts, so PEER RX/BAD
  refers to **this run**; `UP` is TARGET uptime in seconds and is not cleared.
  PEER INFO is read after SOAK, so RX/BAD includes this run's soak frames.

### 5.4 Cautions

- **Without a peer it stays at `SEARCHING FOR ANOTHER UNIT`**:
  `I2C::handshake()` has no timeout; reset to exit.
- Keep the link cable short (FX-C only has 32U4 internal pull-ups); 50 kHz is
  more tolerant, 100 kHz recommended, and 200 kHz failing on long/poor cables is
  expected.
- With a flipped cable, only the unit that enters LINK second may detect it; if
  both are stuck, reset one unit.
- The new protocol (v2: variable frame/command/peer info) is **not compatible**
  with the old `linktest/` app; do not mix them.

---

## 6. Known Limitations

- This app does **not** use `arduboy.begin()`; it uses `arduboy.boot()` plus its
  own startup screen. After reset it immediately shows
  `FXCtest / STARTING...`, then enters the menu. Startup only initializes the
  display/buttons/audio and does **not** access external flash, so a faulty
  flash cannot prevent the menu from starting; stuck buttons cannot block it
  waiting for release.
  - If the screen shows only Arduboy graphics/blocks/game menu, this sketch is
    not running (usually still in the bootloader menu); start it from the
    bootloader. A menu bootloader starts the sketch with A or B.
  - If it stays at `STARTING...`, the app has started but is stuck during
    startup. Startup is not expected to block; please report the symptom.
  - If **FX FLASH** shows `NO CHIP` (JEDEC all 0/all 0xFF), the problem is in
    the flash/SPI bus, not the menu/system; the other tests remain usable.
- **Battery voltage, USB data/charging and the power button are not tested**:
  FX-C does not route battery voltage to a readable ADC; USB functionality must
  be checked on a PC.
- **RGB LED** may not be fitted on some batches; the test relies on user
  confirmation.
- The **EEPROM write test** briefly modifies the last byte; if not needed, just
  read the checksum.
- The FX FLASH test is **read-only**; it does not test writes/erases, avoiding
  damage to games and saves.
- With a menu bootloader, the sketch must be started from the bootloader menu;
  the app cannot control this.
- **LINK TEST** needs another unit running the same firmware; without a peer,
  `I2C::handshake()` has no timeout and stays at `SEARCHING`.
- **MAX FRAME is 48 B** (`I2C_BUFFER_CAPACITY`=48); 64 B frames trigger an
  "operand out of range" assembler error under AVR GCC 7.3 LTO, so they are not
  enabled. Testing 64 B requires library/toolchain changes on both units.
- **200 kHz** is only recorded as experimental; FX-C has internal pull-ups only,
  so failures on long cables are normal.
- **SOAK 10 minutes** makes the link test very long; use `NONE` or 1 minute for
  a quick check.
- LINK TEST temporarily changes `TWBR`; it restores 100 kHz afterwards. If
  power is lost, the next boot starts from 100 kHz.
- This app targets FX-C/mini (flash CS on PE2/HWB). A regular Arduboy FX puts
  flash CS on PD1/SDA, which conflicts with I2C, so LINK TEST is not applicable.

---

## 7. References

- ArduboyI2C library/docs: <https://github.com/sub1inear/ArduboyI2C>,
  <https://sub1inear.github.io/ArduboyI2C/>
- Arduboy2 library: <https://github.com/MLXXXp/Arduboy2>
- Board package (Arduboy FX-C): `arduboy-homemade` (install from Arduino Board
  Manager)

## 8. License

This example code may be used, modified and distributed freely (MIT license).
The Arduboy2 and ArduboyI2C libraries follow their own licenses.

---

Chinese version: [README.md](README.md)
