# FXCclone (FX-C Cart Clone)

FXCclone copies the **external serial flash (game cart image)** of one FX-C to
another FX-C over the I2C link cable, with no PC in between. Only the **used
range** is transferred (the source scans backwards from the end of the flash
and stops at the last non-0xFF 64 KB block). After both flash IDs/capacities
match, the target erases its whole cart and the source streams/writes the used
pages with per-page verification.

> **Only the external cart flash is copied; the AVR bootloader/sketch is not.**
> The target's AVR bootloader remains intact. If the copy is interrupted, the
> target can still enter its bootloader and be re-flashed/re-cloned over USB.
> **The target cart is fully erased after the flash check passes**, so the
> target's existing games/menu will be overwritten.

## Quick Start

1. Flash `FXCclone` onto both FX-C units once over USB;
2. On the **target**: select `RECEIVE (TARGET)` → `A` → confirm with `A`;
   - the target reads its own flash ID and **does not erase yet**;
3. On the **source**: select `SEND (SOURCE)` → `A`;
   - if both flash IDs/capacities match, the target starts a 64 KB block erase;
4. When the target finishes erasing and the source shows `TARGET READY`,
   press **`A:START COPY`**;
5. Both units show `COPY OK` when finished; press `B` to go back.

> The source cannot install the AVR application into a target that has never
> been flashed with FXCclone; use USB/Arduino IDE for the first installation.

## 1. Build and Flash

- Folder: `FXCclone/`, compile for the **Arduboy FX-C** board;
- Dependencies: Arduboy2 and ArduboyI2C (same as the main diagnostic app);
- **Both units must run the same `FXCclone.ino`**;
- Default I2C speed: **200 kHz** (`I2C_FREQUENCY 200000`). If the link is
  unreliable, change it to `100000` and reflash both units (about 2× slower).

Build size: **Flash 17114 / 29696 B (57%), RAM 1941 / 2560 B (75%)**.

## 2. Usage

1. Flash FXCclone onto both FX-C units and connect the I2C link cable.
2. **Operate the target first (RECEIVE)**:
   - menu: `RECEIVE (TARGET)` → `A`;
   - on the confirmation screen press `A`: the target reads its own Flash ID
     and **does not erase yet**;
   - it shows `CHECKING FLASH / CART NOT ERASED` and waits for the source's
     flash information.
3. **Operate the source next (SEND)**:
   - menu: `SEND (SOURCE)` → `A`;
   - the source reads its own Flash ID and keeps sending it to the target;
   - it shows `WAIT TARGET...` and **does not scan or send yet**;
   - if the two flash IDs/capacities match, the target starts the whole-chip
     erase; when it finishes, the source shows `TARGET READY`;
   - press `A:START COPY` to scan the used range and begin the copy;
   - if they do not match, the target shows `FLASH MISMATCH` and the source
     shows `FLASH MISMATCH / COPY ABORTED`; **the target cart is not erased**.
4. Both units show progress during the transfer; **do not disconnect or power
   off**.
5. Both units show `COPY OK` when finished.
6. Press `B` to return; power-cycle/reset both units to use the new cart.

### 2.1 Flash Identity Check

- The source reads its JEDEC ID after entering SEND (e.g. `EF 40 18` = Winbond
  16 MB).
- The source sends this ID + capacity to the target via I2C.
- The target reads its own JEDEC ID and compares:
  - manufacturer / memory type / capacity bytes must match exactly;
  - the capacity must match.
- **Match**: the target starts the whole-chip erase, then transfer is allowed.
- **Mismatch**: target shows `FLASH MISMATCH`, source shows `COPY ABORTED`;
  **the target cart is not erased**.
- If the target cannot read its own Flash ID, it shows `COPY FAILED`; it does
  not erase either.

### Controls

| State | Action |
| --- | --- |
| Menu | UP/DOWN select SEND/RECEIVE, A confirm, B no-op |
| Target confirmation | A = enter flash check, B = back (nothing erased yet) |
| Target `CHECKING FLASH` | B = exit (nothing erased yet) |
| Target `FLASH MISMATCH` | B = back (target cart not erased) |
| Target `WAITING SOURCE` | B = exit (cart has already been erased) |
| Target `DONE/FAIL` | B = back to menu |
| Source `WAIT TARGET` | B = abort |
| Source `TARGET ERASING` | B = abort |
| Source `TARGET READY` | A = start scan/copy, B = abort |
| Source `FLASH MISMATCH` | B = back (target cart not erased) |
| Source `COPY OK/FAILED` | B = back to menu |
| During transfer | B is ignored (avoid corrupting the target cart) |

## 3. Implementation

- The source reads its own flash over **hardware SPI at 8 MHz**.
- The target erases/programs/reads back over **hardware SPI**.
- I2C frames are fixed at 48 bytes (`I2C_BUFFER_CAPACITY=48`); each frame has
  40 bytes of payload.
- A 256-byte flash page is sent as 7 DATA frames plus START/END; for each page
  the target:
  - checks the frame/page Fletcher-16;
  - programs the page after erase;
  - reads back 256 bytes and compares;
  - only ACKs on success, then the source continues.
- The target erases the whole flash in **64 KB blocks (`0xD8`)** with on-screen
  progress (`BLOCK n/total`, `PROGRESS`, `EL/LEFT`), then writes the used pages;
  unused target area remains 0xFF.
- Finally, a Fletcher-16 over the whole used range is compared.
- Target COPY screen shows `PAGE current/total`, `EL`, `LEFT` (estimated from
  the average completed-page time; `--` until enough pages).
- Source scan screen shows `PROGRESS/DONE/TOTAL/EL/LEFT`; while the target is
  erasing, the source shows `TARGET ERASING` + `ERASE block/total`.
- Source send screen shows `PROGRESS/PAGE/BYTES/EL/LEFT` and
  `DO NOT UNPLUG`.
- Source result screen shows `USED/PAGES/CHK/EL`.

## 4. Timing Reference

Estimates for **200 kHz**, 48-byte frames, target 60 Hz main loop (actual time
depends on flash, cable and retries):

| Used data | Transfer time |
| --- | --- |
| 1 MB | about 2–3 minutes |
| 4 MB | about 8–12 minutes |
| 8 MB | about 18–25 minutes |
| 16 MB | about 35–50 minutes |

Additional overhead:

- Source used-range scan: up to about 15–25 seconds (when scanning 16 MB);
- Target whole-chip erase: about 40–90 seconds (16 MB = 256 × 64 KB blocks,
  erased one block at a time with progress);
- At 100 kHz the transfer time roughly doubles.

## 5. Limitations and Risks

- **Only the external cart flash is copied**, not the AVR bootloader/sketch;
  that requires USB/ISP.
- **The target cart is fully erased**; the source is read-only and unaffected.
- Both flashes must be the same model/capacity (FX-C normally has a W25Q128
  16 MB). The target rejects a source used length larger than its own capacity.
- **Interruption = incomplete target cart**: disconnecting/power loss/reset
  during transfer leaves it unfinished; run RECEIVE → SEND again to copy from
  scratch (the target is erased again). The AVR bootloader is unaffected.
- Both units need enough battery power; a full 16 MB copy can take 30–50
  minutes. Fully charged units are recommended.
- A reversed/poor cable usually appears as a permanent `WAIT`; flip the cable
  or use a shorter one and retry.
- The target's EEPROM saves are not overwritten (only external flash is
  copied).
- Use this only on hardware/data you own.

## 6. Recovery

- Target cart corrupted/incomplete: run the clone again (target RECEIVE →
  source SEND).
- The target AVR application is still present and can enter the bootloader.
- If even the bootloader needs recovery, flash the AVR over USB as usual
  (independent of the cart).

## 7. Relationship to LINK TEST

- The main diagnostic app's `LINK TEST` only tests I2C quality/frame
  size/speed; it does not write flash.
- `FXCclone` is a separate firmware dedicated to cart cloning.
- Do not mix it with the diagnostic app or the old `linktest` protocol.

---

Chinese version: [README.md](README.md)
