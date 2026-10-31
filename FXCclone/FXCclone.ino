/*
  FXCclone
  ========
  FX-C Cart Clone

  Copies the external serial flash (game cart) from one FX-C to another
  directly over the I2C link cable - no PC in the middle.

  Both units must run this same sketch. Choose SEND on the source unit and
  RECEIVE on the target unit. After the two flash IDs match, the target's
  external flash is erased, then the source streams only the used range
  (scanned from the end of the flash, stopping at the last non-0xFF 64 KB
  block) and the target programs and verifies every page.

  IMPORTANT
  ---------
  * This copies the EXTERNAL serial flash (cart/games), not the AVR
    bootloader/sketch. The target can always be recovered by reflashing the
    AVR side over USB.
  * The target cart is erased. Do not abort a transfer.
  * Both units must have the same flash type/capacity (FX-C: W25Q128 16 MB).
  * At 100 kHz a full 16 MB image takes roughly an hour; at 200 kHz roughly
    half that. A partially filled cart is much faster because only the used
    range is transferred.
  * Use freshly charged batteries (or stable external power). A power loss
    during the target erase/write leaves the target cart incomplete, but the
    AVR bootloader is not touched.

  Wiring:
    SDA/SCL via the FX-C USB-C SBU link cable
    Flash CS: PE2/HWB (FX-C/mini)
    SCK/MOSI/MISO: PB1/PB2/PB3 (shared with the OLED)

  Tested board: arduboy-homemade:avr:arduboy-fxc (Arduboy2 6.0.0,
  ArduboyI2C 3.0.0, I2C_BUFFER_CAPACITY 48).
*/

#include <Arduboy2.h>

#define I2C_BUFFER_CAPACITY 48
#define I2C_FREQUENCY 200000
#define I2C_IMPLEMENTATION
#include <ArduboyI2C.h>

Arduboy2 arduboy;

// ---------------------------------------------------------------------------
// Flash wiring and SPI access
// ---------------------------------------------------------------------------

#if defined(CART_CS_RX)
  #define FLASH_PORT PORTD
  #define FLASH_DDR DDRD
  #define FLASH_BIT PORTD2
#elif defined(CART_CS_HWB)
  #define FLASH_PORT PORTE
  #define FLASH_DDR DDRE
  #define FLASH_BIT PORTE2
#else
  #define FLASH_PORT PORTD
  #define FLASH_DDR DDRD
  #define FLASH_BIT PORTD1
#endif

constexpr uint8_t FLASH_CMD_WRITE_ENABLE = 0x06;
constexpr uint8_t FLASH_CMD_READ_STATUS1 = 0x05;
constexpr uint8_t FLASH_CMD_WRITE_STATUS1 = 0x01;
constexpr uint8_t FLASH_CMD_PAGE_PROGRAM = 0x02;
constexpr uint8_t FLASH_CMD_READ_DATA = 0x03;
constexpr uint8_t FLASH_CMD_CHIP_ERASE = 0xC7;
constexpr uint8_t FLASH_CMD_BLOCK_ERASE_64K = 0xD8;
constexpr uint8_t FLASH_CMD_JEDEC = 0x9F;
constexpr uint8_t FLASH_CMD_RELEASE_POWERDOWN = 0xAB;

uint32_t cloneFlashSize = 0;
uint8_t cloneJedec[3] = {0, 0, 0};

// Start an SPI access to the external flash. The OLED is deselected for the
// duration; flashEnd() re-selects it (Arduboy2 keeps the OLED selected).
void flashBegin() {
  FLASH_DDR |= _BV(FLASH_BIT);
  FLASH_PORT |= _BV(FLASH_BIT);
  CS_PORT |= _BV(CS_BIT);
  SPCR = _BV(SPE) | _BV(MSTR);
  SPSR = _BV(SPI2X);
  // Clear a stale SPIF left by the display's open-loop transfers, otherwise
  // the first spiTransfer() could return before the new byte is clocked.
  uint8_t stale = SPSR;
  stale = SPDR;
  (void)stale;
  DDRB |= _BV(PORTB1) | _BV(PORTB2);
  DDRB &= ~(_BV(PORTB3));
}

void flashEnd() {
  FLASH_PORT |= _BV(FLASH_BIT);
  CS_PORT &= ~(_BV(CS_BIT));
}

void flashSelect() {
  FLASH_PORT &= ~_BV(FLASH_BIT);
}

void flashDeselect() {
  FLASH_PORT |= _BV(FLASH_BIT);
}

uint8_t spiTransfer(uint8_t value) {
  SPDR = value;
  while (!(SPSR & _BV(SPIF))) {
  }
  return SPDR;
}

void flashReadStatusRaw(uint8_t *status) {
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_READ_STATUS1);
  *status = spiTransfer(0xFF);
  flashDeselect();
  flashEnd();
}

bool flashWaitReady(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (true) {
    uint8_t status = 0;
    flashReadStatusRaw(&status);
    if ((status & 0x01) == 0) {
      return true;
    }
    if ((uint32_t)(millis() - start) > timeoutMs) {
      return false;
    }
  }
}

void flashWriteEnable() {
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_WRITE_ENABLE);
  flashDeselect();
  flashEnd();
}

void flashClearProtection() {
  uint8_t status = 0;
  flashReadStatusRaw(&status);
  if (status == 0) {
    return;
  }
  flashWriteEnable();
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_WRITE_STATUS1);
  spiTransfer(0x00);
  flashDeselect();
  flashEnd();
  flashWaitReady(1000);
}

void flashReadData(uint32_t address, uint8_t *buffer, uint16_t length) {
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_READ_DATA);
  spiTransfer((uint8_t)(address >> 16));
  spiTransfer((uint8_t)(address >> 8));
  spiTransfer((uint8_t)address);
  while (length--) {
    *buffer++ = spiTransfer(0xFF);
  }
  flashDeselect();
  flashEnd();
}

void flashReadJedec(uint8_t *id) {
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_JEDEC);
  id[0] = spiTransfer(0xFF);
  id[1] = spiTransfer(0xFF);
  id[2] = spiTransfer(0xFF);
  flashDeselect();
  flashEnd();
}

void flashWakeup() {
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_RELEASE_POWERDOWN);
  flashDeselect();
  flashEnd();
  delay(1);
}

bool flashReadSize() {
  // Cathy3K powers the cart down just before starting the sketch
  // (SFC_POWERDOWN in its StartSketch path), so release it first.
  flashWakeup();
  uint8_t id[3] = {0, 0, 0};
  flashReadJedec(id);
  if (id[0] != 0xEF || id[2] < 0x10 || id[2] > 0x18) {
    cloneFlashSize = 0;
    return false;
  }
  cloneJedec[0] = id[0];
  cloneJedec[1] = id[1];
  cloneJedec[2] = id[2];
  cloneFlashSize = 1UL << id[2];
  return true;
}

void flashChipEraseStart() {
  flashWriteEnable();
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_CHIP_ERASE);
  flashDeselect();
  flashEnd();
}

void flashBlockEraseStart(uint32_t address) {
  flashWriteEnable();
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_BLOCK_ERASE_64K);
  spiTransfer((uint8_t)(address >> 16));
  spiTransfer((uint8_t)(address >> 8));
  spiTransfer((uint8_t)address);
  flashDeselect();
  flashEnd();
}

bool flashPageProgram(uint32_t address, const uint8_t *data) {
  flashWriteEnable();
  flashBegin();
  flashSelect();
  spiTransfer(FLASH_CMD_PAGE_PROGRAM);
  spiTransfer((uint8_t)(address >> 16));
  spiTransfer((uint8_t)(address >> 8));
  spiTransfer((uint8_t)address);
  for (uint8_t i = 0; i < 128; i++) {
    spiTransfer(data[i * 2]);
    spiTransfer(data[i * 2 + 1]);
  }
  flashDeselect();
  flashEnd();
  return flashWaitReady(10000);
}

// ---------------------------------------------------------------------------
// Fletcher-16 helpers
// ---------------------------------------------------------------------------

uint16_t cloneFletcher16(const uint8_t *data, uint16_t length) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  while (length--) {
    sum1 += *data++;
    if (sum1 >= 255) {
      sum1 -= 255;
    }
    sum2 += sum1;
    if (sum2 >= 255) {
      sum2 -= 255;
    }
  }
  return (uint16_t)((sum2 << 8) | sum1);
}

void cloneFletcherAdd(uint16_t &sum1, uint16_t &sum2,
                      const uint8_t *data, uint16_t length) {
  while (length--) {
    sum1 += *data++;
    if (sum1 >= 255) {
      sum1 -= 255;
    }
    sum2 += sum1;
    if (sum2 >= 255) {
      sum2 -= 255;
    }
  }
}

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------

constexpr uint8_t CLONE_MAGIC = 0xA6;
constexpr uint8_t CLONE_REPLY_MAGIC = 0x5A;
constexpr uint8_t CLONE_VERSION = 1;
constexpr uint8_t CLONE_FRAME = 48;
constexpr uint8_t CLONE_HEADER = 6;
constexpr uint8_t CLONE_PAYLOAD = 40;
constexpr uint8_t CLONE_CHUNKS = 7;
constexpr uint8_t CLONE_LAST_CHUNK_BYTES = 16;

enum CloneType : uint8_t {
  CLONE_PING = 1,
  CLONE_HELLO = 2,
  CLONE_START = 3,
  CLONE_DATA = 4,
  CLONE_END = 5,
  CLONE_DONE = 6,
  CLONE_CHECK = 7
};

enum CloneStatus : uint8_t {
  CLONE_READY = 0,
  CLONE_OK = 1,
  CLONE_FAIL = 2,
  CLONE_BUSY = 3,
  CLONE_CHECK_OK = 4,
  CLONE_MISMATCH = 5
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum Screen : uint8_t {
  SCR_MENU = 0,
  SCR_CONFIRM,
  SCR_SOURCE_READY,
  SCR_SOURCE_RESULT,
  SCR_TARGET
};

enum SourcePhase : uint8_t {
  SRC_SCAN = 0,
  SRC_WAIT_TARGET,
  SRC_COPY,
  SRC_DONE,
  SRC_FAIL
};

enum TargetPhase : uint8_t {
  TGT_CHECK = 0,
  TGT_MISMATCH,
  TGT_ERASE,
  TGT_WAIT,
  TGT_COPY,
  TGT_DONE,
  TGT_FAIL
};

Screen screen = SCR_MENU;
bool chooseSend = true;
bool sourcePeerReady = false;
uint32_t sourceLastPingMs = 0;
bool sourceFlashOk = false;
bool sourceTargetErasing = false;
bool sourceMismatch = false;
uint16_t sourceTargetEraseBlock = 0;

uint8_t cloneFrame[CLONE_FRAME];
uint8_t clonePage[256];
uint8_t cloneVerify[256];
uint8_t cloneAck[8];

uint32_t cloneUsedEnd = 0;
uint32_t cloneFlashPages = 0;
uint16_t cloneSrcSum1 = 0;
uint16_t cloneSrcSum2 = 0;
uint32_t cloneBytesDone = 0;
uint32_t cloneStartMs = 0;
uint8_t sourcePhase = SRC_SCAN;
uint32_t progressStartMs = 0;
uint32_t progressLastDone = 0;
uint32_t sourceCopyStartMs = 0;
uint32_t sourceCopyLastPage = 0;

// Target state (written from the I2C ISR).
volatile uint8_t targetPhase = TGT_ERASE;
volatile bool targetReady = false;
volatile bool targetEraseStarted = false;
volatile bool targetPageReady = false;
volatile bool targetDoneRequested = false;
volatile uint8_t targetChunkMask = 0;
volatile uint16_t targetPage = 0;
volatile uint16_t targetPageChecksum = 0;
volatile uint32_t targetUsedEnd = 0;
volatile uint16_t targetSum1 = 0;
volatile uint16_t targetSum2 = 0;
volatile uint32_t targetPagesWritten = 0;
volatile uint32_t targetNextPage = 0;
volatile uint8_t targetResult = CLONE_BUSY;
volatile uint8_t targetAck[8];
volatile uint32_t targetEraseMs = 0;
volatile bool targetCheckPassed = false;
uint16_t targetEraseBlock = 0;
uint16_t targetEraseBlocks = 0;
bool targetEraseInProgress = false;
uint32_t targetEraseBlockStartMs = 0;
uint8_t targetSourceJedec[3] = {0, 0, 0};
uint32_t targetSourceSize = 0;
uint32_t targetTransferStartMs = 0;
bool targetTimerStarted = false;

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------


void printDuration(uint32_t seconds);

void drawProgress(const __FlashStringHelper *title, uint32_t done,
                  uint32_t total) {
  if (progressStartMs == 0 || done < progressLastDone || done == 0) {
    progressStartMs = millis();
  }
  progressLastDone = done;
  const uint32_t elapsedMs = (uint32_t)(millis() - progressStartMs);
  uint32_t etaSec = 0;
  bool etaValid = false;
  if (done >= 2 && total > done) {
    const uint32_t msPerUnit = elapsedMs / done;
    const uint32_t remain = total - done;
    etaSec = (uint32_t)(((uint64_t)msPerUnit * remain) / 1000ULL);
    etaValid = true;
  }

  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(title);
  uint8_t percent = 0;
  if (total) {
    percent = (uint8_t)((done * 100UL) / total);
    if (percent > 100) {
      percent = 100;
    }
  }
  arduboy.setCursor(0, 16);
  arduboy.print(F("PROGRESS "));
  arduboy.print(percent);
  arduboy.print('%');
  arduboy.setCursor(0, 28);
  arduboy.print(F("DONE  "));
  arduboy.print(done);
  arduboy.setCursor(0, 40);
  arduboy.print(F("TOTAL "));
  arduboy.print(total);
  arduboy.setCursor(0, 48);
  arduboy.print(F("EL "));
  printDuration(elapsedMs / 1000UL);
  arduboy.print(F(" LEFT "));
  if (etaValid) {
    printDuration(etaSec == 0 ? 1 : etaSec);
  } else {
    arduboy.print(F("--"));
  }
  arduboy.setCursor(0, 56);
  arduboy.print(F("PLEASE WAIT"));
  arduboy.display();
}

void drawSourceCopy(uint32_t page, uint32_t totalPages) {
  if (sourceCopyStartMs == 0 || page <= sourceCopyLastPage) {
    sourceCopyStartMs = millis();
  }
  sourceCopyLastPage = page;

  const uint32_t elapsedMs =
      (uint32_t)(millis() - sourceCopyStartMs);
  uint32_t etaSec = 0;
  bool etaValid = false;
  if (page >= 2 && totalPages > page) {
    const uint32_t msPerPage = elapsedMs / page;
    const uint32_t remain = totalPages - page;
    etaSec = (uint32_t)(((uint64_t)msPerPage * remain) / 1000ULL);
    etaValid = true;
  }
  uint8_t percent = 0;
  if (totalPages) {
    percent = (uint8_t)((page * 100UL) / totalPages);
    if (percent > 100) {
      percent = 100;
    }
  }

  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("SENDING CART"));
  arduboy.setCursor(0, 12);
  arduboy.print(F("PROGRESS "));
  arduboy.print(percent);
  arduboy.print('%');
  arduboy.setCursor(0, 24);
  arduboy.print(F("PAGE "));
  arduboy.print(page);
  arduboy.print('/');
  arduboy.print(totalPages);
  arduboy.setCursor(0, 36);
  arduboy.print(F("BYTES "));
  arduboy.print(cloneBytesDone / 1024UL);
  arduboy.print('/');
  arduboy.print(cloneUsedEnd / 1024UL);
  arduboy.print(F(" KB"));
  arduboy.setCursor(0, 48);
  arduboy.print(F("EL "));
  printDuration(elapsedMs / 1000UL);
  arduboy.print(F(" LEFT "));
  if (etaValid) {
    printDuration(etaSec == 0 ? 1 : etaSec);
  } else {
    arduboy.print(F("--"));
  }
  arduboy.setCursor(0, 56);
  arduboy.print(F("DO NOT UNPLUG"));
  arduboy.display();
}

void drawMenu() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FXCclone"));
  arduboy.setCursor(0, 10);
  arduboy.print(F("UP/DN SELECT"));
  arduboy.setCursor(0, 22);
  arduboy.print(chooseSend ? F(">") : F(" "));
  arduboy.print(F(" SEND (SOURCE)"));
  arduboy.setCursor(0, 30);
  arduboy.print(chooseSend ? F(" ") : F(">"));
  arduboy.print(F(" RECEIVE (TARGET)"));
  arduboy.setCursor(0, 44);
  arduboy.print(F("TARGET CART ERASED"));
  arduboy.setCursor(0, 52);
  arduboy.print(F("A:START B:EXIT"));
  arduboy.display();
}

void drawSourceResult() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FXCclone"));
  arduboy.setCursor(0, 10);
  arduboy.print((sourcePhase == SRC_DONE) ? F("COPY OK") : F("COPY FAILED"));
  arduboy.setCursor(0, 22);
  arduboy.print(F("USED "));
  arduboy.print(cloneUsedEnd);
  arduboy.print(F("B"));
  arduboy.setCursor(0, 34);
  arduboy.print(F("PAGES "));
  arduboy.print(cloneFlashPages);
  arduboy.setCursor(0, 46);
  arduboy.print(F("CHK "));
  uint16_t sum = (uint16_t)((cloneSrcSum2 << 8) | cloneSrcSum1);
  arduboy.print(sum, HEX);
  arduboy.setCursor(64, 46);
  arduboy.print(F("EL "));
  printDuration((uint32_t)(millis() - cloneStartMs) / 1000UL);
  arduboy.setCursor(0, 56);
  arduboy.print(F("B:BACK"));
  arduboy.display();
}

void printDuration(uint32_t seconds) {
  if (seconds >= 3600UL) {
    arduboy.print(seconds / 3600UL);
    arduboy.print('h');
    arduboy.print((seconds % 3600UL) / 60UL);
    arduboy.print('m');
  } else if (seconds >= 60UL) {
    arduboy.print(seconds / 60UL);
    arduboy.print('m');
    arduboy.print(seconds % 60UL);
    arduboy.print('s');
  } else {
    arduboy.print(seconds);
    arduboy.print('s');
  }
}

void printHexByte(uint8_t value) {
  if (value < 16) {
    arduboy.print('0');
  }
  arduboy.print(value, HEX);
}

void drawTargetScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(targetPhase == TGT_MISMATCH ? F("FLASH MISMATCH")
                                            : F("FXCclone TARGET"));

  if (targetPhase == TGT_CHECK) {
    arduboy.setCursor(0, 12);
    arduboy.print(F("TGT "));
    printHexByte(cloneJedec[0]);
    arduboy.print(' ');
    printHexByte(cloneJedec[1]);
    arduboy.print(' ');
    printHexByte(cloneJedec[2]);
    arduboy.setCursor(0, 24);
    arduboy.print(F("WAITING SOURCE"));
    arduboy.setCursor(0, 36);
    arduboy.print(F("CHECKING FLASH"));
    arduboy.setCursor(0, 48);
    arduboy.print(F("CART NOT ERASED"));
    arduboy.setCursor(0, 56);
    arduboy.print(F("B:BACK"));
  } else if (targetPhase == TGT_MISMATCH) {
    arduboy.setCursor(0, 12);
    arduboy.print(F("SRC "));
    printHexByte(targetSourceJedec[0]);
    arduboy.print(' ');
    printHexByte(targetSourceJedec[1]);
    arduboy.print(' ');
    printHexByte(targetSourceJedec[2]);
    arduboy.setCursor(0, 24);
    arduboy.print(F("TGT "));
    printHexByte(cloneJedec[0]);
    arduboy.print(' ');
    printHexByte(cloneJedec[1]);
    arduboy.print(' ');
    printHexByte(cloneJedec[2]);
    arduboy.setCursor(0, 36);
    arduboy.print(F("COPY ABORTED"));
    arduboy.setCursor(0, 48);
    arduboy.print(F("CART NOT ERASED"));
    arduboy.setCursor(0, 56);
    arduboy.print(F("B:BACK"));
  } else if (targetPhase == TGT_ERASE) {
    const uint32_t totalBlocks = targetEraseBlocks;
    const uint32_t doneBlocks = targetEraseBlock;
    const uint32_t currentBlock =
        doneBlocks + (targetEraseInProgress ? 1UL : 0UL);
    const uint32_t elapsedMs = (uint32_t)(millis() - targetEraseMs);
    uint32_t etaSec = 0;
    bool etaValid = false;
    if (doneBlocks >= 2 && totalBlocks > doneBlocks) {
      const uint32_t msPerBlock = elapsedMs / doneBlocks;
      const uint32_t remainBlocks = totalBlocks - doneBlocks;
      etaSec = (uint32_t)(((uint64_t)msPerBlock * remainBlocks) / 1000ULL);
      etaValid = true;
    }

    arduboy.setCursor(0, 12);
    arduboy.print(F("ERASING FLASH"));
    arduboy.setCursor(0, 24);
    arduboy.print(F("BLOCK "));
    arduboy.print(currentBlock);
    arduboy.print('/');
    arduboy.print(totalBlocks);
    arduboy.setCursor(0, 36);
    arduboy.print(F("PROGRESS "));
    if (totalBlocks) {
      uint8_t percent = (uint8_t)((doneBlocks * 100UL) / totalBlocks);
      if (percent > 100) {
        percent = 100;
      }
      arduboy.print(percent);
    } else {
      arduboy.print(0);
    }
    arduboy.print('%');
    arduboy.setCursor(0, 48);
    arduboy.print(F("EL "));
    printDuration(elapsedMs / 1000UL);
    arduboy.print(F(" LEFT "));
    if (etaValid) {
      printDuration(etaSec == 0 ? 1 : etaSec);
    } else {
      arduboy.print(F("--"));
    }
    arduboy.setCursor(0, 56);
    arduboy.print(F("DO NOT POWER OFF"));
  } else if (targetPhase == TGT_WAIT) {
    arduboy.setCursor(0, 16);
    arduboy.print(F("WAITING SOURCE"));
    arduboy.setCursor(0, 28);
    arduboy.print(F("CONNECT LINK CABLE"));
    arduboy.setCursor(0, 44);
    arduboy.print(F("CART IS ERASED"));
    arduboy.setCursor(0, 56);
    arduboy.print(F("B:BACK"));
  } else if (targetPhase == TGT_COPY) {
    const uint32_t usedEnd = targetUsedEnd;
    const uint32_t totalPages = (usedEnd + 255UL) / 256UL;
    const uint32_t donePages = targetPagesWritten;
    const uint32_t elapsedMs = targetTimerStarted
                                   ? (uint32_t)(millis() - targetTransferStartMs)
                                   : 0;
    uint32_t etaSec = 0;
    bool etaValid = false;
    if (targetTimerStarted && donePages >= 2 && totalPages > donePages) {
      const uint32_t msPerPage = elapsedMs / donePages;
      const uint32_t remainPages = totalPages - donePages;
      etaSec = (uint32_t)(((uint64_t)msPerPage * remainPages) / 1000ULL);
      etaValid = true;
    }

    arduboy.setCursor(0, 12);
    arduboy.print(F("RECEIVING"));
    arduboy.setCursor(0, 24);
    arduboy.print(F("PAGE "));
    arduboy.print(donePages);
    arduboy.print('/');
    arduboy.print(totalPages);
    arduboy.setCursor(0, 36);
    arduboy.print(F("EL "));
    printDuration(elapsedMs / 1000UL);
    arduboy.print(F(" LEFT "));
    if (etaValid) {
      printDuration(etaSec == 0 ? 1 : etaSec);
    } else {
      arduboy.print(F("--"));
    }
    arduboy.setCursor(0, 48);
    arduboy.print(F("ACK "));
    arduboy.print(targetResult == CLONE_OK ? F("OK") : F("BUSY"));
    arduboy.setCursor(0, 56);
    arduboy.print(F("DO NOT UNPLUG"));
  } else if (targetPhase == TGT_DONE) {
    arduboy.setCursor(0, 16);
    arduboy.print(F("COPY OK"));
    arduboy.setCursor(0, 28);
    arduboy.print(F("BYTES "));
    arduboy.print(targetUsedEnd);
    arduboy.setCursor(0, 40);
    arduboy.print(F("PAGES "));
    arduboy.print(targetPagesWritten);
    arduboy.setCursor(0, 56);
    arduboy.print(F("B:BACK"));
  } else {
    arduboy.setCursor(0, 16);
    arduboy.print(F("COPY FAILED"));
    arduboy.setCursor(0, 28);
    arduboy.print(F("CHECK LOG/ERR"));
    arduboy.setCursor(0, 56);
    arduboy.print(F("B:BACK"));
  }
  arduboy.display();
}

void drawConfirm() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("ERASE TARGET CART?"));
  arduboy.setCursor(0, 16);
  arduboy.print(F("ALL GAMES ON TARGET"));
  arduboy.setCursor(0, 26);
  arduboy.print(F("WILL BE ERASED"));
  arduboy.setCursor(0, 40);
  arduboy.print(F("MAKE SURE SOURCE"));
  arduboy.setCursor(0, 48);
  arduboy.print(F("IS READY"));
  arduboy.setCursor(0, 56);
  arduboy.print(F("A:YES B:NO"));
  arduboy.display();
}

void sourcePollPeer() {
  if ((uint32_t)(millis() - sourceLastPingMs) < 200) {
    return;
  }
  sourceLastPingMs = millis();
  if (!sourceFlashOk) {
    sourcePeerReady = false;
    sourceTargetErasing = false;
    return;
  }

  // Send the source flash identity so the target can refuse a mismatch
  // BEFORE it erases its own cart.
  cloneClearPayload();
  cloneFrame[6] = cloneJedec[0];
  cloneFrame[7] = cloneJedec[1];
  cloneFrame[8] = cloneJedec[2];
  cloneFrame[9] = (uint8_t)cloneFlashSize;
  cloneFrame[10] = (uint8_t)(cloneFlashSize >> 8);
  cloneFrame[11] = (uint8_t)(cloneFlashSize >> 16);
  cloneFrame[12] = (uint8_t)(cloneFlashSize >> 24);
  if (!cloneSendFrame(CLONE_CHECK, 0)) {
    sourcePeerReady = false;
    sourceTargetErasing = false;
    return;
  }

  uint8_t status;
  uint16_t page;
  uint16_t checksum;
  if (!cloneReadAck(status, page, checksum)) {
    sourcePeerReady = false;
    sourceTargetErasing = false;
    return;
  }
  if (status == CLONE_READY) {
    sourcePeerReady = true;
    sourceTargetErasing = false;
    sourceMismatch = false;
  } else if (status == CLONE_CHECK_OK) {
    sourcePeerReady = false;
    sourceTargetErasing = true;
    sourceMismatch = false;
    sourceTargetEraseBlock = page;
  } else if (status == CLONE_MISMATCH) {
    sourcePeerReady = false;
    sourceTargetErasing = false;
    sourceMismatch = true;
  } else if (status == CLONE_FAIL) {
    sourcePeerReady = false;
    sourceTargetErasing = false;
    sourceMismatch = true;
  } else {
    sourcePeerReady = false;
    sourceTargetErasing = false;
  }
}

void drawSourceReady() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FXCclone SOURCE"));

  arduboy.setCursor(0, 12);
  arduboy.print(F("SRC "));
  printHexByte(cloneJedec[0]);
  arduboy.print(' ');
  printHexByte(cloneJedec[1]);
  arduboy.print(' ');
  printHexByte(cloneJedec[2]);

  arduboy.setCursor(0, 24);
  if (!sourceFlashOk) {
    arduboy.print(F("SOURCE FLASH ERR"));
  } else if (sourceMismatch) {
    arduboy.print(F("FLASH MISMATCH"));
  } else if (sourcePeerReady) {
    arduboy.print(F("TARGET READY"));
  } else if (sourceTargetErasing) {
    arduboy.print(F("TARGET ERASING"));
  } else {
    arduboy.print(F("WAIT TARGET..."));
  }

  arduboy.setCursor(0, 36);
  if (!sourceFlashOk) {
    arduboy.print(F("CHECK CART"));
  } else if (sourceMismatch) {
    arduboy.print(F("COPY ABORTED"));
  } else if (sourcePeerReady) {
    arduboy.print(F("A:START COPY"));
  } else if (sourceTargetErasing) {
    arduboy.print(F("WAIT TARGET..."));
  } else {
    arduboy.print(F("TARGET NOT READY"));
  }

  arduboy.setCursor(0, 48);
  if (sourceTargetErasing) {
    arduboy.print(F("ERASE "));
    arduboy.print(sourceTargetEraseBlock);
    arduboy.print('/');
    arduboy.print(cloneFlashSize / 65536UL);
  } else {
    arduboy.print(F("TARGET MUST MATCH"));
  }
  arduboy.setCursor(0, 56);
  arduboy.print(F("B:EXIT"));
  arduboy.display();
}

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------

void cloneBuildFrame(uint8_t type, uint8_t flags) {
  cloneFrame[0] = CLONE_MAGIC;
  cloneFrame[1] = CLONE_VERSION;
  cloneFrame[2] = type;
  cloneFrame[3] = 0;
  cloneFrame[4] = CLONE_FRAME;
  cloneFrame[5] = flags;
  const uint16_t checksum = cloneFletcher16(cloneFrame, 46);
  cloneFrame[46] = (uint8_t)checksum;
  cloneFrame[47] = (uint8_t)(checksum >> 8);
}

void cloneClearPayload() {
  for (uint8_t i = 0; i < CLONE_PAYLOAD; i++) {
    cloneFrame[CLONE_HEADER + i] = 0xFF;
  }
}

bool cloneSendFrame(uint8_t type, uint8_t flags) {
  cloneBuildFrame(type, flags);
  I2C::write(I2C::targetAddress, cloneFrame, CLONE_FRAME, I2C::Mode::Sync);
  return I2C::getError() == I2C::Error::None;
}

bool cloneReadAck(uint8_t &status, uint16_t &page, uint16_t &checksum) {
  I2C::read(I2C::targetAddress, cloneAck, 8);
  if (I2C::getError() != I2C::Error::None) {
    return false;
  }
  if (cloneAck[0] != CLONE_REPLY_MAGIC) {
    return false;
  }
  status = cloneAck[1];
  page = (uint16_t)cloneAck[2] | ((uint16_t)cloneAck[3] << 8);
  checksum = (uint16_t)cloneAck[4] | ((uint16_t)cloneAck[5] << 8);
  return true;
}

// ---------------------------------------------------------------------------
// Source side
// ---------------------------------------------------------------------------

bool sourceScanUsedEnd() {
  if (!flashReadSize()) {
    return false;
  }
  const uint32_t blocks = cloneFlashSize / 65536UL;
  for (int32_t block = (int32_t)blocks - 1; block >= 0; block--) {
    bool used = false;
    for (uint32_t offset = 0; offset < 65536UL; offset += 256) {
      flashReadData(((uint32_t)block << 16) + offset, cloneVerify, 256);
      for (uint16_t i = 0; i < 256; i++) {
        if (cloneVerify[i] != 0xFF) {
          used = true;
          break;
        }
      }
      if (used) {
        break;
      }
    }
    drawProgress(F("SCANNING FLASH"), (uint32_t)(blocks - 1 - block), blocks);
    if (used) {
      cloneUsedEnd = ((uint32_t)block + 1UL) << 16;
      return true;
    }
  }
  cloneUsedEnd = 0;
  return false;
}

bool sourceWaitTargetReady() {
  uint32_t start = millis();
  while (true) {
    cloneClearPayload();
    if (!cloneSendFrame(CLONE_PING, 0)) {
      drawProgress(F("WAIT TARGET"), 0, 0);
      delay(100);
      continue;
    }
    uint8_t status;
    uint16_t page;
    uint16_t checksum;
    if (cloneReadAck(status, page, checksum) && status == CLONE_READY) {
      return true;
    }
    if ((uint32_t)(millis() - start) > 400000UL) {
      return false;
    }
    arduboy.pollButtons();
    if (arduboy.justPressed(B_BUTTON)) {
      return false;
    }
    drawProgress(F("TARGET ERASING"), 0, 0);
    delay(100);
  }
}

bool sourceSendHello() {
  for (uint8_t attempt = 0; attempt < 10; attempt++) {
    cloneClearPayload();
    cloneFrame[6] = (uint8_t)cloneUsedEnd;
    cloneFrame[7] = (uint8_t)(cloneUsedEnd >> 8);
    cloneFrame[8] = (uint8_t)(cloneUsedEnd >> 16);
    cloneFrame[9] = (uint8_t)(cloneUsedEnd >> 24);
    if (cloneSendFrame(CLONE_HELLO, 0)) {
      uint8_t status;
      uint16_t page;
      uint16_t checksum;
      if (cloneReadAck(status, page, checksum) && status == CLONE_OK) {
        return true;
      }
    }
    delay(10);
  }
  return false;
}

bool sourceSendPage(uint32_t page, uint16_t pageChecksum) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    cloneClearPayload();
    cloneFrame[6] = (uint8_t)page;
    cloneFrame[7] = (uint8_t)(page >> 8);
    bool sent = cloneSendFrame(CLONE_START, 0);
    for (uint8_t chunk = 0; chunk < CLONE_CHUNKS && sent; chunk++) {
      cloneClearPayload();
      const uint8_t length = (chunk == CLONE_CHUNKS - 1)
                                 ? CLONE_LAST_CHUNK_BYTES
                                 : CLONE_PAYLOAD;
      for (uint8_t i = 0; i < length; i++) {
        cloneFrame[CLONE_HEADER + i] = clonePage[chunk * CLONE_PAYLOAD + i];
      }
      sent = cloneSendFrame(CLONE_DATA, chunk);
    }
    if (sent) {
      cloneClearPayload();
      cloneFrame[6] = (uint8_t)page;
      cloneFrame[7] = (uint8_t)(page >> 8);
      cloneFrame[8] = (uint8_t)pageChecksum;
      cloneFrame[9] = (uint8_t)(pageChecksum >> 8);
      sent = cloneSendFrame(CLONE_END, 0);
    }
    if (!sent) {
      delay(20);
      continue;
    }
    uint8_t status;
    uint16_t ackPage;
    uint16_t ackChecksum;
    for (uint16_t wait = 0; wait < 500; wait++) {
      if (cloneReadAck(status, ackPage, ackChecksum)) {
        if (status == CLONE_OK) {
          return true;
        }
        if (status == CLONE_FAIL) {
          return false;
        }
      }
      delay(1);
    }
  }
  return false;
}

bool sourceSendDone() {
  const uint16_t checksum =
      (uint16_t)((cloneSrcSum2 << 8) | cloneSrcSum1);
  for (uint8_t attempt = 0; attempt < 10; attempt++) {
    cloneClearPayload();
    cloneFrame[6] = (uint8_t)cloneUsedEnd;
    cloneFrame[7] = (uint8_t)(cloneUsedEnd >> 8);
    cloneFrame[8] = (uint8_t)(cloneUsedEnd >> 16);
    cloneFrame[9] = (uint8_t)(cloneUsedEnd >> 24);
    cloneFrame[10] = (uint8_t)checksum;
    cloneFrame[11] = (uint8_t)(checksum >> 8);
    if (cloneSendFrame(CLONE_DONE, 0)) {
      uint8_t status;
      uint16_t page;
      uint16_t ackChecksum;
      if (cloneReadAck(status, page, ackChecksum) && status == CLONE_OK) {
        return true;
      }
    }
    delay(10);
  }
  return false;
}

void sourceRun() {
  sourcePhase = SRC_SCAN;
  cloneBytesDone = 0;
  cloneStartMs = millis();

  if (!sourceScanUsedEnd() || cloneUsedEnd == 0) {
    sourcePhase = SRC_FAIL;
    return;
  }

  cloneFlashPages = (cloneUsedEnd + 255UL) / 256UL;
  sourcePhase = SRC_WAIT_TARGET;
  if (!sourceWaitTargetReady()) {
    sourcePhase = SRC_FAIL;
    return;
  }
  if (!sourceSendHello()) {
    sourcePhase = SRC_FAIL;
    return;
  }

  sourcePhase = SRC_COPY;
  cloneSrcSum1 = 0;
  cloneSrcSum2 = 0;
  for (uint32_t page = 0; page < cloneFlashPages; page++) {
    const uint32_t address = page * 256UL;
    flashReadData(address, clonePage, 256);
    uint16_t valid = 256;
    if (address + 256UL > cloneUsedEnd) {
      valid = (uint16_t)(cloneUsedEnd - address);
      for (uint16_t i = valid; i < 256; i++) {
        clonePage[i] = 0xFF;
      }
    }
    const uint16_t checksum = cloneFletcher16(clonePage, 256);
    if (!sourceSendPage(page, checksum)) {
      sourcePhase = SRC_FAIL;
      return;
    }
    cloneFletcherAdd(cloneSrcSum1, cloneSrcSum2, clonePage, valid);
    cloneBytesDone += valid;
    drawSourceCopy(page + 1, cloneFlashPages);
  }

  if (!sourceSendDone()) {
    sourcePhase = SRC_FAIL;
    return;
  }
  sourcePhase = SRC_DONE;
}

// ---------------------------------------------------------------------------
// Target side
// ---------------------------------------------------------------------------

void targetSetAck(uint8_t status, uint16_t page) {
  const uint8_t saved = SREG;
  cli();
  targetAck[0] = CLONE_REPLY_MAGIC;
  targetAck[1] = status;
  targetAck[2] = (uint8_t)page;
  targetAck[3] = (uint8_t)(page >> 8);
  targetAck[4] = (uint8_t)targetSum1;
  targetAck[5] = (uint8_t)targetSum2;
  targetAck[6] = 0;
  targetAck[7] = 0;
  SREG = saved;
}

void targetOnRequest() {
  uint8_t reply[8];
  const uint8_t saved = SREG;
  cli();
  for (uint8_t i = 0; i < 8; i++) {
    reply[i] = targetAck[i];
  }
  SREG = saved;
  I2C::reply(reply, 8);
}

void targetOnReceive() {
  const uint8_t *buffer = I2C::getBuffer();
  const uint8_t size = I2C::getBufferSize();
  if (size != CLONE_FRAME || buffer[0] != CLONE_MAGIC ||
      buffer[1] != CLONE_VERSION || buffer[4] != CLONE_FRAME) {
    return;
  }
  const uint16_t checksum = cloneFletcher16(buffer, 46);
  const uint16_t received =
      (uint16_t)buffer[46] | ((uint16_t)buffer[47] << 8);
  if (checksum != received) {
    return;
  }

  const uint8_t type = buffer[2];
  const uint8_t flags = buffer[5];
  const uint8_t *payload = buffer + CLONE_HEADER;

  switch (type) {
    case CLONE_CHECK: {
      if (targetPhase == TGT_FAIL) {
        targetSetAck(CLONE_FAIL, 0);
        break;
      }
      if (targetPhase == TGT_MISMATCH) {
        targetSetAck(CLONE_MISMATCH, 0);
        break;
      }
      if (targetCheckPassed || targetPhase == TGT_ERASE ||
          targetPhase == TGT_WAIT || targetPhase == TGT_COPY ||
          targetPhase == TGT_DONE) {
        targetSetAck(targetReady ? CLONE_READY : CLONE_CHECK_OK,
                      targetReady ? 0 : targetEraseBlock);
        break;
      }

      const uint32_t size = (uint32_t)payload[3] |
                            ((uint32_t)payload[4] << 8) |
                            ((uint32_t)payload[5] << 16) |
                            ((uint32_t)payload[6] << 24);
      targetSourceJedec[0] = payload[0];
      targetSourceJedec[1] = payload[1];
      targetSourceJedec[2] = payload[2];
      targetSourceSize = size;
      const bool match = (payload[0] == cloneJedec[0] &&
                          payload[1] == cloneJedec[1] &&
                          payload[2] == cloneJedec[2] &&
                          size == cloneFlashSize);
      if (!match) {
        targetPhase = TGT_MISMATCH;
        targetReady = false;
        targetSetAck(CLONE_MISMATCH, 0);
      } else {
        targetCheckPassed = true;
        targetSetAck(CLONE_CHECK_OK, 0);
      }
      break;
    }

    case CLONE_PING:
      if (targetReady) {
        targetSetAck(CLONE_READY, 0);
      } else {
        targetSetAck(CLONE_BUSY, 0);
      }
      break;

    case CLONE_HELLO: {
      const uint32_t end = (uint32_t)payload[0] |
                           ((uint32_t)payload[1] << 8) |
                           ((uint32_t)payload[2] << 16) |
                           ((uint32_t)payload[3] << 24);
      if (!targetReady || end == 0 || end > cloneFlashSize) {
        targetSetAck(CLONE_FAIL, 0);
        break;
      }
      targetUsedEnd = end;
      targetSum1 = 0;
      targetSum2 = 0;
      targetPagesWritten = 0;
      targetNextPage = 0;
      targetPhase = TGT_COPY;
      targetSetAck(CLONE_OK, 0);
      break;
    }

    case CLONE_START: {
      const uint16_t page = (uint16_t)payload[0] |
                            ((uint16_t)payload[1] << 8);
      targetPage = page;
      targetChunkMask = 0;
      targetPageChecksum = 0;
      targetPageReady = false;
      for (uint16_t i = 0; i < 256; i++) {
        clonePage[i] = 0xFF;
      }
      targetSetAck(CLONE_BUSY, page);
      break;
    }

    case CLONE_DATA: {
      const uint8_t chunk = flags & 0x07;
      if (chunk >= CLONE_CHUNKS) {
        targetSetAck(CLONE_FAIL, targetPage);
        break;
      }
      const uint8_t length = (chunk == CLONE_CHUNKS - 1)
                                 ? CLONE_LAST_CHUNK_BYTES
                                 : CLONE_PAYLOAD;
      for (uint8_t i = 0; i < length; i++) {
        clonePage[chunk * CLONE_PAYLOAD + i] = payload[i];
      }
      targetChunkMask |= (uint8_t)(1 << chunk);
      targetSetAck(CLONE_BUSY, targetPage);
      break;
    }

    case CLONE_END: {
      const uint16_t page = (uint16_t)payload[0] |
                            ((uint16_t)payload[1] << 8);
      const uint16_t pageChecksum =
          (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
      if (page != targetPage || targetChunkMask != 0x7F) {
        targetSetAck(CLONE_FAIL, targetPage);
        break;
      }
      targetPageChecksum = pageChecksum;
      targetPageReady = true;
      targetPhase = TGT_COPY;
      targetSetAck(CLONE_BUSY, targetPage);
      break;
    }

    case CLONE_DONE: {
      const uint32_t end = (uint32_t)payload[0] |
                           ((uint32_t)payload[1] << 8) |
                           ((uint32_t)payload[2] << 16) |
                           ((uint32_t)payload[3] << 24);
      const uint16_t checksum =
          (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
      const uint16_t expected =
          (uint16_t)((targetSum2 << 8) | targetSum1);
      if (end == targetUsedEnd && checksum == expected &&
          targetPagesWritten == (targetUsedEnd + 255UL) / 256UL) {
        targetPhase = TGT_DONE;
        targetReady = false;
        targetSetAck(CLONE_OK, 0);
      } else {
        targetPhase = TGT_FAIL;
        targetReady = false;
        targetSetAck(CLONE_FAIL, 0);
      }
      break;
    }

    default:
      break;
  }
}

void targetBegin() {
  I2C::begin();
  I2C::onReceive(targetOnReceive);
  I2C::onRequest(targetOnRequest);
  I2C::setAddress(I2C::targetAddress);

  targetPhase = TGT_CHECK;
  targetReady = false;
  targetCheckPassed = false;
  targetEraseInProgress = false;
  targetEraseBlock = 0;
  targetEraseBlocks = 0;
  targetPageReady = false;
  targetDoneRequested = false;
  targetChunkMask = 0;
  targetPage = 0;
  targetPageChecksum = 0;
  targetUsedEnd = 0;
  targetSum1 = 0;
  targetSum2 = 0;
  targetPagesWritten = 0;
  targetNextPage = 0;
  targetResult = CLONE_BUSY;
  targetTransferStartMs = 0;
  targetTimerStarted = false;
  targetSourceJedec[0] = 0;
  targetSourceJedec[1] = 0;
  targetSourceJedec[2] = 0;
  targetSourceSize = 0;
  targetSetAck(CLONE_BUSY, 0);

  // Read the local flash ID now. Do NOT erase until the source has sent its
  // flash ID and the target has confirmed that both flashes match.
  if (!flashReadSize()) {
    targetPhase = TGT_FAIL;
    return;
  }
}

void targetLoop() {
  if (targetPhase == TGT_COPY && !targetTimerStarted) {
    targetTimerStarted = true;
    targetTransferStartMs = millis();
  }

  if (targetPhase == TGT_CHECK) {
    if (targetCheckPassed && cloneFlashSize >= 65536UL) {
      flashClearProtection();
      targetEraseBlock = 0;
      targetEraseBlocks = (uint16_t)(cloneFlashSize / 65536UL);
      targetEraseInProgress = false;
      targetEraseMs = millis();
      targetPhase = TGT_ERASE;
    }
    drawTargetScreen();
    return;
  }

  if (targetPhase == TGT_ERASE) {
    if (!targetEraseInProgress) {
      if (targetEraseBlock >= targetEraseBlocks) {
        targetReady = true;
        targetPhase = TGT_WAIT;
        targetSetAck(CLONE_READY, 0);
        drawTargetScreen();
        return;
      }
      flashBlockEraseStart((uint32_t)targetEraseBlock * 65536UL);
      targetEraseInProgress = true;
      targetEraseBlockStartMs = millis();
    } else {
      uint8_t status = 0;
      flashReadStatusRaw(&status);
      if ((status & 0x01) == 0) {
        targetEraseBlock++;
        targetEraseInProgress = false;
      } else if ((uint32_t)(millis() - targetEraseBlockStartMs) > 10000UL) {
        targetPhase = TGT_FAIL;
        targetSetAck(CLONE_FAIL, 0);
      }
    }
    drawTargetScreen();
    return;
  }

  if (targetPhase == TGT_WAIT || targetPhase == TGT_COPY) {
    if (targetPageReady) {
      const uint16_t page = targetPage;
      const uint16_t expected = targetPageChecksum;
      const uint16_t actual = cloneFletcher16(clonePage, 256);
      const uint32_t address = (uint32_t)page * 256UL;

      bool ok = (actual == expected);
      if (ok) {
        ok = flashPageProgram(address, clonePage);
      }
      if (ok) {
        flashReadData(address, cloneVerify, 256);
        ok = (memcmp(cloneVerify, clonePage, 256) == 0);
      }

      if (ok && (uint32_t)page > targetNextPage) {
        ok = false;
      }
      if (ok) {
        uint16_t valid = 256;
        if (address + 256UL > targetUsedEnd) {
          valid = (uint16_t)(targetUsedEnd - address);
        }
        if ((uint32_t)page == targetNextPage) {
          uint16_t sum1 = targetSum1;
          uint16_t sum2 = targetSum2;
          cloneFletcherAdd(sum1, sum2, clonePage, valid);
          targetSum1 = sum1;
          targetSum2 = sum2;
          targetPagesWritten++;
          targetNextPage = (uint32_t)page + 1UL;
        }
        targetResult = CLONE_OK;
        targetSetAck(CLONE_OK, page);
      } else {
        targetResult = CLONE_FAIL;
        targetSetAck(CLONE_FAIL, page);
        targetPhase = TGT_FAIL;
        targetReady = false;
      }
      targetPageReady = false;
    }
    drawTargetScreen();
    return;
  }

  drawTargetScreen();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  arduboy.boot();
  arduboy.setFrameRate(60);

  FLASH_DDR |= _BV(FLASH_BIT);
  FLASH_PORT |= _BV(FLASH_BIT);

  arduboy.clear();
  arduboy.setCursor(40, 20);
  arduboy.print(F("FXCclone"));
  arduboy.setCursor(16, 36);
  arduboy.print(F("SOURCE -> TARGET"));
  arduboy.setCursor(22, 44);
  arduboy.print(F("I2C LINK CABLE"));
  arduboy.setCursor(0, 56);
  arduboy.print(F("USE CHARGED POWER"));
  arduboy.display();
  arduboy.delayShort(500);

  screen = SCR_MENU;
}

void loop() {
  if (!arduboy.nextFrame()) {
    return;
  }
  arduboy.pollButtons();

  if (screen == SCR_MENU) {
    if (arduboy.justPressed(UP_BUTTON) || arduboy.justPressed(DOWN_BUTTON)) {
      chooseSend = !chooseSend;
    }
    if (arduboy.justPressed(A_BUTTON)) {
      if (chooseSend) {
        I2C::begin();
        sourcePeerReady = false;
        sourceTargetErasing = false;
        sourceMismatch = false;
        sourceTargetEraseBlock = 0;
        sourceLastPingMs = 0;
        sourceFlashOk = flashReadSize();
        screen = SCR_SOURCE_READY;
      } else {
        screen = SCR_CONFIRM;
      }
    }
    drawMenu();
    return;
  }

  if (screen == SCR_SOURCE_READY) {
    sourcePollPeer();
    if (arduboy.justPressed(B_BUTTON)) {
      I2C::end();
      screen = SCR_MENU;
      return;
    }
    if (arduboy.justPressed(A_BUTTON) && sourcePeerReady) {
      sourceRun();
      screen = SCR_SOURCE_RESULT;
      I2C::end();
      return;
    }
    drawSourceReady();
    return;
  }

  if (screen == SCR_CONFIRM) {
    if (arduboy.justPressed(A_BUTTON)) {
      targetBegin();
      screen = SCR_TARGET;
      return;
    }
    if (arduboy.justPressed(B_BUTTON)) {
      screen = SCR_MENU;
      return;
    }
    drawConfirm();
    return;
  }

  if (screen == SCR_SOURCE_RESULT) {
    if (arduboy.justPressed(B_BUTTON)) {
      screen = SCR_MENU;
      return;
    }
    drawSourceResult();
    return;
  }

  if (screen == SCR_TARGET) {
    if ((targetPhase == TGT_CHECK || targetPhase == TGT_MISMATCH ||
         targetPhase == TGT_WAIT || targetPhase == TGT_DONE ||
         targetPhase == TGT_FAIL) &&
        arduboy.justPressed(B_BUTTON)) {
      I2C::end();
      screen = SCR_MENU;
      return;
    }
    targetLoop();
    return;
  }
}
