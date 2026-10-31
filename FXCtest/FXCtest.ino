/*
  FXCtest
  =========================

  A comprehensive hardware diagnostic for the Arduboy FX-C (also works on
  hardware that behaves like an FX-C / Arduboy Mini with HWB flash chip select).

  Tools:
    1. DISPLAY    - full screen patterns (all on/off, checker, stripes, border,
                    dither, inverted) for dead pixels / driver faults.
    2. BUTTONS    - live test of UP/DOWN/LEFT/RIGHT/A/B; all six must be seen.
    3. SOUND      - plays three beeps and asks the user to confirm.
    4. RGB LED    - cycles red/green/blue/white/off; user confirms.
    5. FX FLASH   - read-only FX serial flash test: JEDEC ID, capacity, status
                    registers, menu image signature (ARDUBOY). SPI is
                    bit-banged (not the ArduboyFX library), shows STEP 1/4
                    ... 4/4 progress and has no unbounded hardware waits.
    6. EEPROM     - reads and checksums the 1 KB EEPROM; an optional write test
                    writes one byte (last address), verifies, then restores it.
    7. LINK TEST  - two-machine I2C link test using the ArduboyI2C library.
                    Handshake/cable-flip detection, 3 s quality phase
                    (success rate, retries, error breakdown, RTT
                    last/avg/min/max/jitter, max consecutive failures),
                    max-frame sweep 8/16/24/32/48 bytes, 1 KB throughput
                    test, 50/100/200 kHz speed sweep, peer info exchange,
                    bus-idle check and optional 1/5/10 minute soak test.

  Notes
  -----
  * I2C_IMPLEMENTATION must be defined once before including ArduboyI2C.h.
  * I2C_BUFFER_CAPACITY is raised to 48 so the link test can try 48-byte
    frames (the library default is 32). Both units must run this same build.
  * I2C::begin() must be called after arduboy.boot() / arduboy.begin().
  * The FX-C only has internal (software) pull-ups, so ArduboyI2C only
    compiles with I2C_FREQUENCY <= 200 kHz (default 100 kHz). Short link
    cables are strongly recommended; the 200 kHz phase is experimental and
    may fail on long cables.
  * The LINK TEST needs another unit running the same app and selecting
    LINK TEST at the same time. Without a peer I2C::handshake() has no
    timeout and the app will wait on the SEARCHING screen until reset.
  * In LINK TEST, the controller ("HOST") chooses the soak time and runs
    the whole sequence; the target just responds and shows live counters.
  * The EEPROM write test writes one byte at the last EEPROM address and
    restores its original value. Do not power off during that test.
  * The flash tests are read-only. Existing games/save data are not modified.
  * Start-up deliberately does not access the external flash: the app goes
    straight to the menu and runs FX FLASH on demand.

  Tested with Arduboy2 6.0.0 and ArduboyI2C 3.0.0
*/

#include <Arduboy2.h>
#include <EEPROM.h>
#include <avr/pgmspace.h>
#include <string.h>

#define I2C_BUFFER_CAPACITY 48
#define I2C_IMPLEMENTATION
#include <ArduboyI2C.h>

Arduboy2 arduboy;
BeepPin1 beep;

// ---------------------------------------------------------------------------
// Screens and shared state
// ---------------------------------------------------------------------------

enum Screen : uint8_t {
  SCR_MENU = 0,
  SCR_DISPLAY,
  SCR_BUTTONS,
  SCR_SOUND,
  SCR_RGB,
  SCR_FLASH,
  SCR_EEPROM,
  SCR_LINK
};

Screen screen = SCR_MENU;
uint8_t menuIndex = 0;
uint8_t menuTop = 0;

constexpr uint8_t MENU_ITEMS = 7;
constexpr uint8_t MENU_VISIBLE = 6;

uint16_t appCrc = 0;
bool appCrcDone = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    arduboy.print('0');
  }
  arduboy.print(value, HEX);
}

void printHexWord(uint16_t value) {
  printHexByte((uint8_t)(value >> 8));
  printHexByte((uint8_t)value);
}

// Fletcher-16 over RAM data.
uint16_t fletcher16(const uint8_t *data, uint16_t length) {
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
  return (sum2 << 8) | sum1;
}

// Fletcher-16 over the application flash area.
uint16_t fletcherFlash(uint16_t length) {
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  for (uint16_t i = 0; i < length; i++) {
    sum1 += pgm_read_byte(i);
    if (sum1 >= 255) {
      sum1 -= 255;
    }
    sum2 += sum1;
    if (sum2 >= 255) {
      sum2 -= 255;
    }
  }
  return (sum2 << 8) | sum1;
}

// Compute the application flash fingerprint only once.
void ensureAppCrc() {
  if (!appCrcDone) {
    appCrc = fletcherFlash(0x7400);
    appCrcDone = true;
  }
}

// Fletcher-16 over the internal EEPROM.
uint16_t fletcherEeprom(uint16_t length) {  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  for (uint16_t i = 0; i < length; i++) {
    sum1 += EEPROM.read(i);
    if (sum1 >= 255) {
      sum1 -= 255;
    }
    sum2 += sum1;
    if (sum2 >= 255) {
      sum2 -= 255;
    }
  }
  return (sum2 << 8) | sum1;
}

uint32_t capacityBytes(uint8_t jedecSize) {
  if (jedecSize >= 26) {
    return 0;
  }
  return 1UL << jedecSize;
}

const __FlashStringHelper *menuLabel(uint8_t index) {
  switch (index) {
    case 0: return F("DISPLAY");
    case 1: return F("BUTTONS");
    case 2: return F("SOUND");
    case 3: return F("RGB LED");
    case 4: return F("FX FLASH");
    case 5: return F("EEPROM");
    case 6: return F("LINK TEST");
    default: return F("--");
  }
}

// FX flash discovery results used by FX FLASH.
struct FxFlashInfo {
  uint8_t jedec[3];
  bool present;
  bool tested;
  bool image;
} fxInfo;

// ---------------------------------------------------------------------------
// FX serial flash access (read-only, bit-banged SPI)
// ---------------------------------------------------------------------------
//
// This neither uses the ArduboyFX library nor the hardware SPI peripheral:
// SPI is bit-banged on the display's SPI pins (PB1/PB2/PB3) while the flash
// chip select is PE2/HWB on the FX-C/mini. Bit-banging has no hardware wait
// loops that can block forever if the flash bus is faulty, which keeps the
// FX FLASH test safe and lets it print step-by-step progress.

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

constexpr uint8_t FLASH_CMD_WAKEUP = 0xAB;
constexpr uint8_t FLASH_CMD_JEDEC = 0x9F;
constexpr uint8_t FLASH_CMD_READ = 0x03;
constexpr uint8_t FLASH_CMD_STATUS1 = 0x05;
constexpr uint8_t FLASH_CMD_STATUS2 = 0x35;
constexpr uint8_t FLASH_CMD_STATUS3 = 0x15;

void flashDelay() {
  __builtin_avr_delay_cycles(8);
}

uint8_t flashTransfer(uint8_t out) {
  uint8_t in = 0;
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (out & 0x80) {
      PORTB |= _BV(PORTB2);  // MOSI
    } else {
      PORTB &= ~_BV(PORTB2);
    }
    out <<= 1;
    flashDelay();
    PORTB |= _BV(PORTB1);  // SCK rising edge (sample here)
    flashDelay();
    in <<= 1;
    if (PINB & _BV(PORTB3)) {
      in |= 1;
    }
    flashDelay();
    PORTB &= ~_BV(PORTB1);  // SCK falling edge
    flashDelay();
  }
  return in;
}

void flashSelect() {
  FLASH_PORT &= ~_BV(FLASH_BIT);
}

void flashDeselect() {
  FLASH_PORT |= _BV(FLASH_BIT);
}

void flashBegin() {
  SPCR &= ~_BV(SPE);  // release SCK/MOSI for bit-banging
  FLASH_DDR |= _BV(FLASH_BIT);
  flashDeselect();
  DDRB |= _BV(PORTB1) | _BV(PORTB2);
  DDRB &= ~(_BV(PORTB3));
  PORTB &= ~(_BV(PORTB1));
  CS_PORT |= _BV(CS_BIT);  // deselect the OLED while bit-banging
}

void flashEnd() {
  flashDeselect();
  SPCR = _BV(SPE) | _BV(MSTR);
  SPSR = _BV(SPI2X);
  // Arduboy2 selects the OLED permanently in bootOLED() and never re-selects
  // it in display(). If we leave the display deselected here, all later
  // display() calls are silently ignored (the screen looks frozen).
  CS_PORT &= ~_BV(CS_BIT);
}

void flashPrepare() {
  // Make sure the flash chip select is an output and deselected.
  FLASH_DDR |= _BV(FLASH_BIT);
  flashDeselect();
}

void flashWakeup() {
  flashSelect();
  flashTransfer(FLASH_CMD_WAKEUP);
  flashDeselect();
}

void flashReadJedec(uint8_t *id) {
  flashSelect();
  flashTransfer(FLASH_CMD_JEDEC);
  id[0] = flashTransfer(0xFF);
  id[1] = flashTransfer(0xFF);
  id[2] = flashTransfer(0xFF);
  flashDeselect();
}

uint8_t flashReadStatus(uint8_t command) {
  flashSelect();
  flashTransfer(command);
  const uint8_t value = flashTransfer(0xFF);
  flashDeselect();
  return value;
}

void flashReadData(uint32_t address, uint8_t *buffer, uint16_t length) {
  flashSelect();
  flashTransfer(FLASH_CMD_READ);
  flashTransfer((uint8_t)(address >> 16));
  flashTransfer((uint8_t)(address >> 8));
  flashTransfer((uint8_t)address);
  while (length--) {
    *buffer++ = flashTransfer(0xFF);
  }
  flashDeselect();
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------

void moveMenu(int8_t delta) {
  menuIndex = (uint8_t)((menuIndex + MENU_ITEMS + delta) % MENU_ITEMS);
  if (menuIndex < menuTop) {
    menuTop = menuIndex;
  }
  if (menuIndex >= menuTop + MENU_VISIBLE) {
    menuTop = menuIndex - MENU_VISIBLE + 1;
  }
}

void drawMenu() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FXCtest"));

  for (uint8_t row = 0; row < MENU_VISIBLE; row++) {
    const uint8_t item = menuTop + row;
    if (item >= MENU_ITEMS) {
      break;
    }
    arduboy.setCursor(0, 8 + row * 8);
    arduboy.print(item == menuIndex ? '>' : ' ');
    arduboy.setCursor(6, 8 + row * 8);
    arduboy.print(menuLabel(item));
  }

  // Scroll indicators
  if (menuTop > 0) {
    arduboy.setCursor(120, 8);
    arduboy.print('^');
  }
  if (menuTop + MENU_VISIBLE < MENU_ITEMS) {
    arduboy.setCursor(120, 48);
    arduboy.print('v');
  }

  arduboy.setCursor(0, 56);
  arduboy.print(F("UP/DN A:RUN"));
  arduboy.display();
}

// ---------------------------------------------------------------------------
// Display test
// ---------------------------------------------------------------------------

constexpr uint8_t DISPLAY_PATTERNS = 8;
uint8_t displayPattern = 0;
uint32_t displayTimer = 0;

const __FlashStringHelper *displayPatternName(uint8_t index) {
  switch (index) {
    case 0: return F("ALL ON");
    case 1: return F("ALL OFF");
    case 2: return F("CHECKER");
    case 3: return F("VSTRIPES");
    case 4: return F("HSTRIPES");
    case 5: return F("BORDER");
    case 6: return F("DITHER");
    default: return F("INVERT");
  }
}

void displayEnter() {
  displayPattern = 0;
  displayTimer = millis();
  arduboy.invert(false);
}

void drawDisplayPattern() {
  arduboy.clear();
  switch (displayPattern) {
    case 0:
      arduboy.fillScreen(WHITE);
      break;
    case 1:
      break;
    case 2:
      for (uint8_t y = 0; y < 56; y += 8) {
        for (uint8_t x = 0; x < 128; x += 8) {
          if (((x / 8) + (y / 8)) & 1) {
            arduboy.fillRect(x, y, 4, 4, WHITE);
          }
        }
      }
      break;
    case 3:
      for (uint8_t x = 0; x < 128; x += 2) {
        arduboy.fillRect(x, 0, 1, 56, WHITE);
      }
      break;
    case 4:
      for (uint8_t y = 0; y < 56; y += 2) {
        arduboy.fillRect(0, y, 128, 1, WHITE);
      }
      break;
    case 5:
      arduboy.drawRect(0, 0, 128, 56, WHITE);
      arduboy.drawRect(2, 2, 124, 52, WHITE);
      arduboy.drawLine(0, 0, 127, 55, WHITE);
      arduboy.drawLine(127, 0, 0, 55, WHITE);
      break;
    case 6: {
      uint8_t *buffer = arduboy.getBuffer();
      for (uint8_t page = 0; page < 7; page++) {
        memset(buffer + page * 128, (uint8_t)(page * 37), 128);
      }
      break;
    }
    default:
      for (uint8_t y = 0; y < 56; y += 8) {
        for (uint8_t x = 0; x < 128; x += 8) {
          if (((x / 8) + (y / 8)) & 1) {
            arduboy.fillRect(x, y, 6, 6, WHITE);
          }
        }
      }
      break;
  }

  // Status bar is not part of the pattern.
  arduboy.fillRect(0, 56, 128, 8, BLACK);
  arduboy.setCursor(0, 56);
  arduboy.print(F("PAT "));
  arduboy.print(displayPattern + 1);
  arduboy.print('/');
  arduboy.print(DISPLAY_PATTERNS);
  arduboy.print(' ');
  arduboy.print(displayPatternName(displayPattern));

  arduboy.invert(displayPattern == DISPLAY_PATTERNS - 1);
  arduboy.display();
}

void displayLoop() {
  if (arduboy.justPressed(A_BUTTON)) {
    if (displayPattern + 1 >= DISPLAY_PATTERNS) {
          arduboy.invert(false);
      screen = SCR_MENU;
      return;
    }
    displayPattern++;
    displayTimer = millis();
  } else if (arduboy.justPressed(B_BUTTON)) {
    arduboy.invert(false);
    screen = SCR_MENU;
    return;
  }

  if (displayPattern + 1 < DISPLAY_PATTERNS &&
      (uint32_t)(millis() - displayTimer) > 1500) {
    displayPattern++;
    displayTimer = millis();
  }

  drawDisplayPattern();
}

// ---------------------------------------------------------------------------
// Button test
// ---------------------------------------------------------------------------

constexpr uint8_t ALL_BUTTONS =
    UP_BUTTON | DOWN_BUTTON | LEFT_BUTTON | RIGHT_BUTTON | A_BUTTON | B_BUTTON;
uint8_t buttonsSeen = 0;
uint8_t buttonsNow = 0;
uint8_t buttonsLast = 0;
uint32_t buttonsAbortMs = 0;

void buttonsEnter() {
  buttonsSeen = 0;
  buttonsNow = 0;
  buttonsLast = 0;
  buttonsAbortMs = 0;
}

const __FlashStringHelper *buttonMaskName(uint8_t mask) {
  switch (mask) {
    case UP_BUTTON: return F("UP");
    case DOWN_BUTTON: return F("DOWN");
    case LEFT_BUTTON: return F("LEFT");
    case RIGHT_BUTTON: return F("RIGHT");
    case A_BUTTON: return F("A");
    case B_BUTTON: return F("B");
    default: return mask ? F("MULTI") : F("--");
  }
}

void drawButtonBox(uint8_t x, uint8_t y, const __FlashStringHelper *name,
                   uint8_t mask) {
  arduboy.setCursor(x, y);
  arduboy.print(name);

  const bool seen = (buttonsSeen & mask) != 0;
  const bool down = (buttonsNow & mask) != 0;
  if (down) {
    // Currently held: fill the whole indicator.
    arduboy.fillRect(x + 30, y + 1, 6, 6, WHITE);
  } else {
    // Already seen but released: outline + small center dot.
    arduboy.drawRect(x + 30, y + 1, 6, 6, WHITE);
    if (seen) {
      arduboy.fillRect(x + 32, y + 3, 2, 2, WHITE);
    }
  }
}

void drawButtonsScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("BUTTON TEST"));
  drawButtonBox(0, 8, F("UP"), UP_BUTTON);
  drawButtonBox(64, 8, F("DOWN"), DOWN_BUTTON);
  drawButtonBox(0, 16, F("LEFT"), LEFT_BUTTON);
  drawButtonBox(64, 16, F("RIGHT"), RIGHT_BUTTON);
  drawButtonBox(0, 24, F("A"), A_BUTTON);
  drawButtonBox(64, 24, F("B"), B_BUTTON);

  arduboy.setCursor(0, 36);
  arduboy.print(F("SEEN "));
  printHexByte(buttonsSeen);
  arduboy.print(F(" LAST "));
  arduboy.print(buttonMaskName(buttonsLast));

  if (buttonsSeen == ALL_BUTTONS) {
    arduboy.setCursor(40, 46);
    arduboy.print(F("ALL PASS"));
  } else {
    arduboy.setCursor(0, 46);
    arduboy.print(F("PRESS ALL 6 BUTTONS"));
  }

  arduboy.setCursor(0, 56);
  arduboy.print(F("A+B 2s: EXIT"));
  arduboy.display();
}

void buttonsLoop() {
  const uint8_t state = arduboy.buttonsState();
  buttonsNow = state;
  if (state != 0) {
    buttonsLast = state;
    buttonsSeen |= state;
  }

  if ((state & (A_BUTTON | B_BUTTON)) == (A_BUTTON | B_BUTTON)) {
    if (buttonsAbortMs == 0) {
      buttonsAbortMs = millis();
    } else if ((uint32_t)(millis() - buttonsAbortMs) > 2000) {
      screen = SCR_MENU;
      return;
    }
  } else {
    buttonsAbortMs = 0;
  }

  drawButtonsScreen();
}

// ---------------------------------------------------------------------------
// Sound test
// ---------------------------------------------------------------------------

void playTestBeeps() {
  arduboy.audio.on();
  beep.tone(beep.freq(440.0));
  arduboy.delayShort(150);
  beep.noTone();
  beep.tone(beep.freq(880.0));
  arduboy.delayShort(150);
  beep.noTone();
  beep.tone(beep.freq(1760.0));
  arduboy.delayShort(150);
  beep.noTone();
}

void soundEnter() {
  playTestBeeps();
}

void drawSoundScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("SOUND TEST"));
  arduboy.setCursor(0, 16);
  arduboy.print(F("3 BEEPS PLAYED"));
  arduboy.setCursor(0, 28);
  arduboy.print(F("440 / 880 / 1760 Hz"));
  arduboy.setCursor(0, 44);
  arduboy.print(F("A:HEARD  B:NOT HEARD"));
  arduboy.setCursor(0, 56);
  arduboy.print(F("B:MENU"));
  arduboy.display();
}

void soundLoop() {
  if (arduboy.justPressed(A_BUTTON)) {
    beep.noTone();
    screen = SCR_MENU;
    return;
  }
  if (arduboy.justPressed(B_BUTTON)) {
    beep.noTone();
    screen = SCR_MENU;
    return;
  }
  drawSoundScreen();
}

// ---------------------------------------------------------------------------
// RGB LED test
// ---------------------------------------------------------------------------

uint8_t rgbIndex = 0;
uint32_t rgbTimer = 0;

const __FlashStringHelper *rgbName(uint8_t index) {
  switch (index) {
    case 0: return F("RED");
    case 1: return F("GREEN");
    case 2: return F("BLUE");
    case 3: return F("WHITE");
    default: return F("OFF");
  }
}

void applyRgb() {
  switch (rgbIndex) {
    case 0:
      arduboy.digitalWriteRGB(RGB_ON, RGB_OFF, RGB_OFF);
      break;
    case 1:
      arduboy.digitalWriteRGB(RGB_OFF, RGB_ON, RGB_OFF);
      break;
    case 2:
      arduboy.digitalWriteRGB(RGB_OFF, RGB_OFF, RGB_ON);
      break;
    case 3:
      arduboy.digitalWriteRGB(RGB_ON, RGB_ON, RGB_ON);
      break;
    default:
      arduboy.digitalWriteRGB(RGB_OFF, RGB_OFF, RGB_OFF);
      break;
  }
}

void rgbEnter() {
  rgbIndex = 0;
  rgbTimer = millis();
  applyRgb();
}

void drawRgbScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("RGB LED TEST"));
  arduboy.setCursor(0, 16);
  arduboy.print(F("COLOR: "));
  arduboy.print(rgbName(rgbIndex));
  arduboy.setCursor(0, 32);
  arduboy.print(F("CHECK THE LED"));
  arduboy.setCursor(0, 44);
  arduboy.print(F("A:VISIBLE B:NOT"));
  arduboy.setCursor(0, 56);
  arduboy.print(F("CYCLES AUTOMATICALLY"));
  arduboy.display();
}

void rgbLoop() {
  if (arduboy.justPressed(A_BUTTON)) {
    arduboy.digitalWriteRGB(RGB_OFF, RGB_OFF, RGB_OFF);
    screen = SCR_MENU;
    return;
  }
  if (arduboy.justPressed(B_BUTTON)) {
    arduboy.digitalWriteRGB(RGB_OFF, RGB_OFF, RGB_OFF);
    screen = SCR_MENU;
    return;
  }

  if ((uint32_t)(millis() - rgbTimer) > 700) {
    rgbIndex = (rgbIndex + 1) % 5;
    rgbTimer = millis();
    applyRgb();
  }

  drawRgbScreen();
}

// ---------------------------------------------------------------------------
// FX flash test screen
// ---------------------------------------------------------------------------

uint8_t fxStatus1 = 0;
uint8_t fxStatus2 = 0;
uint8_t fxStatus3 = 0;

void fxReadStatus() {
  fxStatus1 = 0;
  fxStatus2 = 0;
  fxStatus3 = 0;
  if (!fxInfo.present) {
    return;
  }
  flashBegin();
  fxStatus1 = flashReadStatus(FLASH_CMD_STATUS1);
  fxStatus2 = flashReadStatus(FLASH_CMD_STATUS2);
  if (fxInfo.jedec[2] == 0x18) {
    fxStatus3 = flashReadStatus(FLASH_CMD_STATUS3);
  }
  flashEnd();
}

void flashStep(const __FlashStringHelper *text) {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FX FLASH TEST"));
  arduboy.setCursor(0, 16);
  arduboy.print(text);
  arduboy.display();
}

void fxEnter() {
  fxInfo.tested = true;
  fxInfo.present = false;
  fxInfo.image = false;
  fxInfo.jedec[0] = 0;
  fxInfo.jedec[1] = 0;
  fxInfo.jedec[2] = 0;

  uint8_t id[3] = {0, 0, 0};

  flashStep(F("STEP 1/4 WAKEUP"));
  flashBegin();
  flashWakeup();
  flashEnd();

  flashStep(F("STEP 2/4 JEDEC"));
  flashBegin();
  flashReadJedec(id);
  flashEnd();
  fxInfo.jedec[0] = id[0];
  fxInfo.jedec[1] = id[1];
  fxInfo.jedec[2] = id[2];
  const bool blank = (id[0] == 0x00 && id[1] == 0x00) ||
                     (id[0] == 0xFF && id[1] == 0xFF);
  fxInfo.present = !blank;

  if (fxInfo.present) {
    flashStep(F("STEP 3/4 IMAGE"));
    uint8_t signature[7];
    flashBegin();
    flashReadData(0, signature, 7);
    flashEnd();
    fxInfo.image = memcmp(signature, "ARDUBOY", 7) == 0;
  }

  flashStep(F("STEP 4/4 STATUS"));
  fxReadStatus();
}

void drawFlashScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("FX FLASH TEST"));

  arduboy.setCursor(0, 8);
  arduboy.print(F("JEDEC "));
  if (fxInfo.present) {
    printHexByte(fxInfo.jedec[0]);
    arduboy.print(' ');
    printHexByte(fxInfo.jedec[1]);
    arduboy.print(' ');
    printHexByte(fxInfo.jedec[2]);
  } else {
    arduboy.print(F("NO CHIP"));
  }

  arduboy.setCursor(0, 16);
  arduboy.print(F("CAP   "));
  if (fxInfo.present && capacityBytes(fxInfo.jedec[2])) {
    arduboy.print(capacityBytes(fxInfo.jedec[2]) >> 20);
    arduboy.print(F(" MB"));
  } else {
    arduboy.print('?');
  }

  arduboy.setCursor(0, 24);
  arduboy.print(F("SR1 "));
  printHexByte(fxStatus1);
  arduboy.print(F(" SR2 "));
  printHexByte(fxStatus2);
  arduboy.print(F(" SR3 "));
  printHexByte(fxStatus3);

  arduboy.setCursor(0, 32);
  arduboy.print(F("IMAGE "));
  arduboy.print(fxInfo.image ? F("ARDUBOY") : F("NONE"));

  arduboy.setCursor(0, 40);
#if defined(CART_CS_HWB)
  arduboy.print(F("CS: PE2/HWB"));
#elif defined(CART_CS_RX)
  arduboy.print(F("CS: PD2/RX"));
#else
  arduboy.print(F("CS: PD1/SDA"));
#endif

  arduboy.setCursor(0, 48);
  arduboy.print(F("SPI: BITBANG"));

  arduboy.setCursor(0, 56);
  arduboy.print(F("A:RESCAN B:BACK"));
  arduboy.display();
}

void flashLoop() {
  if (arduboy.justPressed(A_BUTTON)) {
    fxEnter();
  } else if (arduboy.justPressed(B_BUTTON)) {
    screen = SCR_MENU;
    return;
  }
  drawFlashScreen();
}

// ---------------------------------------------------------------------------
// EEPROM test screen
// ---------------------------------------------------------------------------

uint16_t eepromCrc = 0;
uint16_t eeTestAddress = 0;
uint8_t eeOriginal = 0;
uint8_t eeTestValue = 0;
uint8_t eeWriteOk1 = 0;
uint8_t eeWriteOk2 = 0;
uint8_t eeRestoreOk = 0;
uint8_t eeTestRun = 0;

void eepromRead() {
  eepromCrc = fletcherEeprom(EEPROM.length());
}

void eepromEnter() {
  eepromRead();
  eeTestRun = 0;
  eeWriteOk1 = 0;
  eeWriteOk2 = 0;
  eeRestoreOk = 0;
  eeTestAddress = (uint16_t)(EEPROM.length() - 1);
}

void eepromRunTest() {
  eeTestRun = 1;
  eeOriginal = EEPROM.read(eeTestAddress);
  eeTestValue = (uint8_t)~eeOriginal;
  EEPROM.update(eeTestAddress, eeTestValue);
  eeWriteOk1 = (EEPROM.read(eeTestAddress) == eeTestValue) ? 1 : 0;
  EEPROM.update(eeTestAddress, eeOriginal);
  eeWriteOk2 = (EEPROM.read(eeTestAddress) == eeOriginal) ? 1 : 0;
  eeRestoreOk = eeWriteOk1 && eeWriteOk2;
  eepromRead();
}

void drawEepromScreen() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("EEPROM TEST"));

  arduboy.setCursor(0, 8);
  arduboy.print(F("SIZE "));
  arduboy.print(EEPROM.length());
  arduboy.print(F("  CRC "));
  printHexWord(eepromCrc);

  arduboy.setCursor(0, 16);
  arduboy.print(F("LAST "));
  printHexByte(EEPROM.read(eeTestAddress));
  arduboy.print(F("  ADDR "));
  arduboy.print(eeTestAddress);

  arduboy.setCursor(0, 24);
  arduboy.print(F("A:WRITE/RESTORE TEST"));

  arduboy.setCursor(0, 32);
  if (eeTestRun) {
    arduboy.print(F("W1:"));
    arduboy.print(eeWriteOk1 ? F("OK") : F("NG"));
    arduboy.print(F(" W2:"));
    arduboy.print(eeWriteOk2 ? F("OK") : F("NG"));
  } else {
    arduboy.print(F("NOT RUN YET"));
  }

  arduboy.setCursor(0, 40);
  arduboy.print(F("WRITES 1 BYTE, THEN"));
  arduboy.setCursor(0, 48);
  arduboy.print(F("RESTORES IT"));

  arduboy.setCursor(0, 56);
  arduboy.print(F("B:BACK"));
  arduboy.display();
}

void eepromLoop() {
  if (arduboy.justPressed(A_BUTTON)) {
    eepromRunTest();
  } else if (arduboy.justPressed(B_BUTTON)) {
    screen = SCR_MENU;
    return;
  }
  drawEepromScreen();
}

// ---------------------------------------------------------------------------
// I2C link test (quality / max frame / throughput / speed / soak)
// ---------------------------------------------------------------------------

#define LINK_NOINLINE __attribute__((noinline))

constexpr uint8_t LINK_MAGIC_HOST = 0xA5;
constexpr uint8_t LINK_MAGIC_TARGET = 0x5A;
constexpr uint8_t LINK_VERSION = 2;
constexpr uint8_t LINK_MAX_FRAME = 48;
constexpr uint8_t LINK_HEADER_LEN = 6;
constexpr uint8_t LINK_MIN_FRAME = 8;
constexpr uint8_t LINK_ATTEMPTS = 3;
constexpr uint16_t LINK_QUALITY_MS = 3000;
constexpr uint16_t LINK_THROUGHPUT_BYTES = 1024;
constexpr uint8_t LINK_FRAME_TX_COUNT = 16;
constexpr uint8_t LINK_SPEED_TX_COUNT = 64;
constexpr uint8_t LINK_FRAME_COUNT = 5;
constexpr uint8_t LINK_SPEED_COUNT = 3;
constexpr uint8_t LINK_PAGE_COUNT = 5;

enum LinkCmd : uint8_t {
  LINK_CMD_PING = 0,
  LINK_CMD_FRAME = 1,
  LINK_CMD_INFO = 2,
  LINK_CMD_EXIT = 3,
  LINK_CMD_RESET = 4
};

enum LinkStatus : uint8_t {
  LINK_STATUS_OK = 0,
  LINK_STATUS_NO_FRAME = 1
};

enum LinkError : uint8_t {
  LINK_ERR_NONE = 0,
  LINK_ERR_WRITE,
  LINK_ERR_READ,
  LINK_ERR_MAGIC,
  LINK_ERR_SIZE,
  LINK_ERR_STATUS,
  LINK_ERR_SEQ,
  LINK_ERR_CHECKSUM,
  LINK_ERR_PAYLOAD,
  LINK_ERR_COUNT
};

enum LinkState : uint8_t {
  LINK_STATE_MENU = 0,
  LINK_STATE_RUN,
  LINK_STATE_DONE,
  LINK_STATE_TARGET
};

enum LinkPhase : uint8_t {
  LINK_PHASE_QUALITY = 0,
  LINK_PHASE_FRAMES,
  LINK_PHASE_THROUGHPUT,
  LINK_PHASE_SPEED,
  LINK_PHASE_INFO,
  LINK_PHASE_SOAK,
  LINK_PHASE_DONE
};

const uint8_t LINK_FRAME_SIZES[LINK_FRAME_COUNT] = {8, 16, 24, 32, 48};

I2C::Role linkRole = I2C::Role::Controller;
uint8_t linkState = LINK_STATE_MENU;
uint8_t linkPhase = LINK_PHASE_QUALITY;
uint8_t linkPage = 0;
uint8_t linkSeq = 0;
uint8_t linkSpeedCode = 1;
uint8_t linkSoakCode = 0;
uint8_t linkResult = 0;
bool linkAborted = false;
uint8_t linkBusBits = 0;
uint16_t linkHandshakeMs = 0;
bool linkFlipSeen = false;

uint32_t linkPhaseStart = 0;
uint32_t linkSoakEnd = 0;
uint8_t linkFrameIndex = 0;
uint8_t linkFrameTx = 0;
uint8_t linkSpeedIndex = 0;
uint8_t linkSpeedTx = 0;
uint32_t linkThroughputBytes = 0;
uint32_t linkThroughputStart = 0;
uint32_t linkThroughputMs = 0;
uint16_t linkThroughputTx = 0;
uint16_t linkQualityTx = 0;
uint16_t linkQualityOk = 0;
uint16_t linkSoakTx = 0;
uint16_t linkSoakOk = 0;

uint8_t linkTxFrame[LINK_MAX_FRAME];
uint8_t linkRxFrame[LINK_MAX_FRAME];

// Target-side (ISR) state.
uint8_t linkRxFrameSize = 0;
uint8_t linkRxCmd = 0;
uint8_t linkRxSeq = 0;
uint8_t linkRxFlags = 0;
bool linkHaveFrame = false;
volatile bool linkExitRequested = false;
volatile uint8_t linkValidDelta = 0;
volatile uint8_t linkBadDelta = 0;
volatile uint8_t linkLastCmd = 0;
volatile uint8_t linkLastSpeed = 1;
volatile uint8_t linkLastSize = 0;
volatile uint16_t linkValidReport = 0;
volatile uint16_t linkBadReport = 0;
volatile uint16_t linkUptimeReport = 0;
uint16_t linkValidTotal = 0;
uint16_t linkBadTotal = 0;
uint32_t linkLastValidMs = 0;

struct LinkStats {
  uint32_t tx;
  uint32_t ok;
  uint32_t lost;
  uint32_t retries;
  uint16_t errors[LINK_ERR_COUNT];
  uint8_t maxConsecFails;
  uint8_t consecFails;
  uint16_t rttLast;
  uint16_t rttMin;
  uint16_t rttMax;
  uint32_t rttSum;
  uint32_t rttCount;
  uint32_t jitterSum;
  uint16_t rttPrev;
  uint8_t rttHavePrev;
  uint16_t hist[8];
  uint8_t errLog[6];
  uint8_t errLogCount;
  uint8_t errLogPos;
} linkStats;

struct LinkPeerInfo {
  uint8_t version;
  uint8_t role;
  uint16_t appCrc;
  uint16_t uptimeSec;
  uint16_t rxValid;
  uint16_t rxBad;
  uint8_t capacity;
  uint8_t maxFrame;
  uint8_t speed;
  uint8_t lastCmd;
  uint8_t lastSize;
} linkPeer;
bool linkPeerOk = false;

struct LinkFrameResult {
  uint8_t ok;
  uint8_t fail;
} linkFrameResults[LINK_FRAME_COUNT];

struct LinkSpeedResult {
  uint8_t ok;
  uint8_t fail;
} linkSpeedResults[LINK_SPEED_COUNT];

// ---------------------------------------------------------------------------
// Link helpers
// ---------------------------------------------------------------------------

LINK_NOINLINE uint8_t linkSoakMinutes() {
  switch (linkSoakCode) {
    case 1: return 1;
    case 2: return 5;
    case 3: return 10;
    default: return 0;
  }
}

const __FlashStringHelper *linkSoakText() {
  switch (linkSoakCode) {
    case 1: return F("1 MIN");
    case 2: return F("5 MIN");
    case 3: return F("10 MIN");
    default: return F("NONE");
  }
}

const __FlashStringHelper *linkSpeedName(uint8_t code) {
  switch (code) {
    case 0: return F("50K");
    case 2: return F("200K");
    case 3: return F("--");
    default: return F("100K");
  }
}

const __FlashStringHelper *linkCmdName(uint8_t cmd) {
  switch (cmd) {
    case LINK_CMD_FRAME: return F("FRAME");
    case LINK_CMD_INFO: return F("INFO");
    case LINK_CMD_EXIT: return F("EXIT");
    case LINK_CMD_RESET: return F("RESET");
    default: return F("PING");
  }
}

const __FlashStringHelper *linkPhaseName() {
  switch (linkPhase) {
    case LINK_PHASE_QUALITY: return F("QUALITY");
    case LINK_PHASE_FRAMES: return F("MAX FRAME");
    case LINK_PHASE_THROUGHPUT: return F("THROUGHPUT");
    case LINK_PHASE_SPEED: return F("SPEED");
    case LINK_PHASE_INFO: return F("PEER INFO");
    case LINK_PHASE_SOAK: return F("SOAK");
    default: return F("DONE");
  }
}

LINK_NOINLINE uint8_t linkFlags() {
  return (uint8_t)((linkSpeedCode & 3) | ((linkSoakCode & 3) << 2));
}

LINK_NOINLINE uint8_t linkPercent(uint32_t part, uint32_t total) {
  if (total == 0) {
    return 0;
  }
  return (uint8_t)((part * 100UL) / total);
}

LINK_NOINLINE void linkResetStats() {
  memset(&linkStats, 0, sizeof(linkStats));
  linkStats.rttMin = 0xFFFF;
  memset(linkFrameResults, 0, sizeof(linkFrameResults));
  memset(linkSpeedResults, 0, sizeof(linkSpeedResults));
  linkPeerOk = false;
  linkAborted = false;
  linkQualityTx = 0;
  linkQualityOk = 0;
  linkSoakTx = 0;
  linkSoakOk = 0;
  linkThroughputBytes = 0;
  linkThroughputMs = 0;
  linkThroughputTx = 0;
  linkFrameIndex = 0;
  linkFrameTx = 0;
  linkSpeedIndex = 0;
  linkSpeedTx = 0;
}

LINK_NOINLINE void linkRecordError(uint8_t error) {
  if (error < LINK_ERR_COUNT) {
    linkStats.errors[error]++;
  }
  linkStats.errLog[linkStats.errLogPos] = error;
  linkStats.errLogPos = (uint8_t)((linkStats.errLogPos + 1) % 6);
  if (linkStats.errLogCount < 6) {
    linkStats.errLogCount++;
  }
}

LINK_NOINLINE void linkSetSpeed(uint8_t code) {
  uint32_t freq;
  if (code == 0) {
    freq = 50000;
  } else if (code == 2) {
    freq = 200000;
  } else {
    freq = 100000;
  }
  TWBR = (uint8_t)((F_CPU / freq - 16) / 2);
  TWSR &= (uint8_t)~0x03;
  linkSpeedCode = code;
}

LINK_NOINLINE void linkBuildHostFrame(uint8_t cmd, uint8_t size) {
  linkSeq++;
  linkTxFrame[0] = LINK_MAGIC_HOST;
  linkTxFrame[1] = LINK_VERSION;
  linkTxFrame[2] = cmd;
  linkTxFrame[3] = linkSeq;
  linkTxFrame[4] = size;
  linkTxFrame[5] = linkFlags();
  for (uint8_t i = LINK_HEADER_LEN; i < (uint8_t)(size - 2); i++) {
    linkTxFrame[i] = (uint8_t)(linkSeq + i * 13U);
  }
  const uint16_t checksum = fletcher16(linkTxFrame, (uint16_t)(size - 2));
  linkTxFrame[size - 2] = (uint8_t)checksum;
  linkTxFrame[size - 1] = (uint8_t)(checksum >> 8);
}

LINK_NOINLINE bool linkTransaction(uint8_t cmd, uint8_t size) {
  linkBuildHostFrame(cmd, size);
  const uint8_t seq = linkTxFrame[3];
  bool success = false;

  const uint32_t startUs = micros();
  for (uint8_t attempt = 0; attempt < LINK_ATTEMPTS && !success; attempt++) {
    if (attempt > 0) {
      linkStats.retries++;
    }

    I2C::write(I2C::targetAddress, linkTxFrame, size, I2C::Mode::Sync);
    if (I2C::getError() != I2C::Error::None) {
      linkRecordError(LINK_ERR_WRITE);
      continue;
    }

    I2C::read(I2C::targetAddress, linkRxFrame, size);
    if (I2C::getError() != I2C::Error::None) {
      linkRecordError(LINK_ERR_READ);
      continue;
    }

    if (linkRxFrame[0] != LINK_MAGIC_TARGET ||
        linkRxFrame[1] != LINK_VERSION) {
      linkRecordError(LINK_ERR_MAGIC);
      continue;
    }
    if (linkRxFrame[4] != size) {
      linkRecordError(LINK_ERR_SIZE);
      continue;
    }
    if (linkRxFrame[2] != LINK_STATUS_OK) {
      linkRecordError(LINK_ERR_STATUS);
      continue;
    }
    if (linkRxFrame[3] != seq) {
      linkRecordError(LINK_ERR_SEQ);
      continue;
    }
    const uint16_t expected = fletcher16(linkRxFrame, (uint16_t)(size - 2));
    const uint16_t received =
        (uint16_t)linkRxFrame[size - 2] |
        ((uint16_t)linkRxFrame[size - 1] << 8);
    if (expected != received) {
      linkRecordError(LINK_ERR_CHECKSUM);
      continue;
    }
    if (cmd == LINK_CMD_FRAME && size > LINK_MIN_FRAME) {
      if (memcmp(linkRxFrame + LINK_HEADER_LEN,
                 linkTxFrame + LINK_HEADER_LEN,
                 (size_t)(size - LINK_MIN_FRAME)) != 0) {
        linkRecordError(LINK_ERR_PAYLOAD);
        continue;
      }
    }
    success = true;
  }

  const uint32_t elapsedUs = micros() - startUs;
  linkStats.tx++;
  if (success) {
    linkStats.ok++;
    linkStats.consecFails = 0;
    const uint16_t rtt =
        elapsedUs > 0xFFFFUL ? 0xFFFF : (uint16_t)elapsedUs;
    linkStats.rttLast = rtt;
    if (rtt < linkStats.rttMin) {
      linkStats.rttMin = rtt;
    }
    if (rtt > linkStats.rttMax) {
      linkStats.rttMax = rtt;
    }
    linkStats.rttSum += rtt;
    linkStats.rttCount++;
    if (linkStats.rttHavePrev) {
      linkStats.jitterSum +=
          (rtt > linkStats.rttPrev) ? (rtt - linkStats.rttPrev)
                                    : (linkStats.rttPrev - rtt);
    }
    linkStats.rttPrev = rtt;
    linkStats.rttHavePrev = 1;
    uint8_t bucket = (uint8_t)(rtt / 250U);
    if (bucket > 7) {
      bucket = 7;
    }
    linkStats.hist[bucket]++;
  } else {
    linkStats.lost++;
    if (linkStats.consecFails < 255) {
      linkStats.consecFails++;
    }
    if (linkStats.consecFails > linkStats.maxConsecFails) {
      linkStats.maxConsecFails = linkStats.consecFails;
    }
  }
  return success;
}

LINK_NOINLINE void linkReadPeerInfo() {
  linkPeer.version = linkRxFrame[6];
  linkPeer.role = linkRxFrame[7];
  linkPeer.appCrc =
      (uint16_t)linkRxFrame[8] | ((uint16_t)linkRxFrame[9] << 8);
  linkPeer.uptimeSec =
      (uint16_t)linkRxFrame[10] | ((uint16_t)linkRxFrame[11] << 8);
  linkPeer.rxValid =
      (uint16_t)linkRxFrame[12] | ((uint16_t)linkRxFrame[13] << 8);
  linkPeer.rxBad =
      (uint16_t)linkRxFrame[14] | ((uint16_t)linkRxFrame[15] << 8);
  linkPeer.capacity = linkRxFrame[16];
  linkPeer.maxFrame = linkRxFrame[17];
  linkPeer.speed = linkRxFrame[18];
  linkPeer.lastCmd = linkRxFrame[19];
  linkPeer.lastSize = linkRxFrame[20];
  linkPeerOk = true;
}

// ---------------------------------------------------------------------------
// Target callbacks (ISR context)
// ---------------------------------------------------------------------------

LINK_NOINLINE void linkTargetBuildInfo() {
  linkTxFrame[6] = LINK_VERSION;
  linkTxFrame[7] = 0;  // role: target
  linkTxFrame[8] = (uint8_t)appCrc;
  linkTxFrame[9] = (uint8_t)(appCrc >> 8);
  linkTxFrame[10] = (uint8_t)linkUptimeReport;
  linkTxFrame[11] = (uint8_t)(linkUptimeReport >> 8);
  linkTxFrame[12] = (uint8_t)linkValidReport;
  linkTxFrame[13] = (uint8_t)(linkValidReport >> 8);
  linkTxFrame[14] = (uint8_t)linkBadReport;
  linkTxFrame[15] = (uint8_t)(linkBadReport >> 8);
  linkTxFrame[16] = (uint8_t)I2C_BUFFER_CAPACITY;
  linkTxFrame[17] = LINK_MAX_FRAME;
  linkTxFrame[18] = linkLastSpeed;
  linkTxFrame[19] = linkLastCmd;
  linkTxFrame[20] = linkLastSize;
  for (uint8_t i = 21; i < 30; i++) {
    linkTxFrame[i] = 0;
  }
}

LINK_NOINLINE void linkOnReceive() {
  const uint8_t *buffer = I2C::getBuffer();
  const uint8_t size = I2C::getBufferSize();

  if (size < LINK_MIN_FRAME || size > LINK_MAX_FRAME ||
      buffer[0] != LINK_MAGIC_HOST || buffer[1] != LINK_VERSION ||
      buffer[4] != size) {
    if (linkBadDelta < 255) {
      linkBadDelta++;
    }
    return;
  }

  const uint16_t expected = fletcher16(buffer, (uint16_t)(size - 2));
  const uint16_t received =
      (uint16_t)buffer[size - 2] | ((uint16_t)buffer[size - 1] << 8);
  if (expected != received) {
    if (linkBadDelta < 255) {
      linkBadDelta++;
    }
    return;
  }

  const uint8_t cmd = buffer[2];
  if (cmd == LINK_CMD_EXIT) {
    linkExitRequested = true;
    return;
  }
  if (cmd == LINK_CMD_RESET) {
    linkHaveFrame = false;
    linkRxFrameSize = 0;
    linkValidDelta = 0;
    linkBadDelta = 0;
    linkValidTotal = 0;
    linkBadTotal = 0;
    linkValidReport = 0;
    linkBadReport = 0;
    linkLastCmd = cmd;
    return;
  }

  memcpy(linkRxFrame, buffer, size);
  linkRxFrameSize = size;
  linkRxCmd = cmd;
  linkRxSeq = buffer[3];
  linkRxFlags = buffer[5];
  linkHaveFrame = true;
  linkLastCmd = cmd;
  linkLastSpeed = buffer[5] & 3;
  linkLastSize = size;
  if (linkValidDelta < 250) {
    linkValidDelta++;
  }
}

LINK_NOINLINE void linkOnRequest() {
  uint8_t size = linkHaveFrame ? linkRxFrameSize : LINK_MIN_FRAME;
  if (size < LINK_MIN_FRAME || size > LINK_MAX_FRAME) {
    size = LINK_MIN_FRAME;
  }

  if (!linkHaveFrame) {
    memset(linkTxFrame, 0, LINK_MIN_FRAME);
    linkTxFrame[0] = LINK_MAGIC_TARGET;
    linkTxFrame[1] = LINK_VERSION;
    linkTxFrame[2] = LINK_STATUS_NO_FRAME;
    linkTxFrame[4] = LINK_MIN_FRAME;
  } else {
    memcpy(linkTxFrame, linkRxFrame, size);
    linkTxFrame[0] = LINK_MAGIC_TARGET;
    linkTxFrame[1] = LINK_VERSION;
    linkTxFrame[2] = LINK_STATUS_OK;
    if (linkRxCmd == LINK_CMD_INFO && size >= 32) {
      linkTargetBuildInfo();
    }
  }

  const uint16_t checksum = fletcher16(linkTxFrame, (uint16_t)(size - 2));
  linkTxFrame[size - 2] = (uint8_t)checksum;
  linkTxFrame[size - 1] = (uint8_t)(checksum >> 8);
  I2C::reply(linkTxFrame, size);
}

// ---------------------------------------------------------------------------
// Link entry / cleanup
// ---------------------------------------------------------------------------

LINK_NOINLINE void drawLinkSearching() {
  arduboy.clear();
  arduboy.setCursor(25, 16);
  arduboy.print(F("SEARCHING FOR"));
  arduboy.setCursor(28, 28);
  arduboy.print(F("ANOTHER UNIT"));
  arduboy.setCursor(10, 44);
  arduboy.print(F("CONNECT LINK CABLE"));
  arduboy.setCursor(10, 56);
  arduboy.print(F("STUCK? RESTART ONE"));
  arduboy.display();
}

LINK_NOINLINE void drawLinkCableFlipped() {
  linkFlipSeen = true;
  arduboy.clear();
  arduboy.setCursor(22, 16);
  arduboy.print(F("CABLE FLIPPED!"));
  arduboy.setCursor(7, 32);
  arduboy.print(F("FLIP THE LINK CABLE"));
  arduboy.setCursor(22, 44);
  arduboy.print(F("ON THIS DEVICE"));
  arduboy.display();
}

LINK_NOINLINE void linkCleanup() {
  I2C::onReceive(nullptr);
  I2C::onRequest(nullptr);
  I2C::end();
}

LINK_NOINLINE void linkSendExit() {
  linkBuildHostFrame(LINK_CMD_EXIT, LINK_MIN_FRAME);
  I2C::write(I2C::targetAddress, linkTxFrame, LINK_MIN_FRAME,
             I2C::Mode::Sync);
}

LINK_NOINLINE void linkSendReset() {
  linkBuildHostFrame(LINK_CMD_RESET, LINK_MIN_FRAME);
  I2C::write(I2C::targetAddress, linkTxFrame, LINK_MIN_FRAME,
             I2C::Mode::Sync);
}

LINK_NOINLINE void linkExitToMenu() {
  linkSetSpeed(1);
  if (linkRole == I2C::Role::Controller) {
    linkSendExit();
  }
  linkCleanup();
  screen = SCR_MENU;
}

LINK_NOINLINE void linkEnter() {
  ensureAppCrc();
  I2C::begin();
  linkBusBits = (uint8_t)(PIND & 0x03);
  linkFlipSeen = false;
  const uint32_t handshakeStart = millis();
  I2C::checkCableFlipped(drawLinkCableFlipped);
  linkRole = I2C::handshake(drawLinkSearching);
  linkHandshakeMs = (uint16_t)(millis() - handshakeStart);

  if (linkRole == I2C::Role::Target) {
    const uint8_t savedSreg = SREG;
    cli();
    linkHaveFrame = false;
    linkExitRequested = false;
    linkValidDelta = 0;
    linkBadDelta = 0;
    linkValidTotal = 0;
    linkBadTotal = 0;
    linkValidReport = 0;
    linkBadReport = 0;
    linkLastCmd = 0;
    linkLastSpeed = 1;
    linkLastSize = 0;
    linkUptimeReport = 0;
    SREG = savedSreg;
    I2C::onReceive(linkOnReceive);
    I2C::onRequest(linkOnRequest);
    I2C::setAddress(I2C::targetAddress);
    linkLastValidMs = millis();
    linkState = LINK_STATE_TARGET;
  } else {
    I2C::onReceive(nullptr);
    I2C::onRequest(nullptr);
    I2C::setAddress(I2C::nullAddress);
    linkResetStats();
    linkSeq = 0;
    linkSpeedCode = 1;
    linkSoakCode = 0;
    linkPage = 0;
    linkState = LINK_STATE_MENU;
  }
  delay(10);
}

// ---------------------------------------------------------------------------
// Controller test sequence
// ---------------------------------------------------------------------------

LINK_NOINLINE void linkStartSuite() {
  linkResetStats();
  linkSetSpeed(1);
  linkSendReset();
  linkSeq = 0;
  linkSpeedCode = 1;
  linkPhase = LINK_PHASE_QUALITY;
  linkPhaseStart = millis();
  linkFrameIndex = 0;
  linkFrameTx = 0;
  linkSpeedIndex = 0;
  linkSpeedTx = 0;
  linkThroughputBytes = 0;
  linkThroughputMs = 0;
  linkThroughputTx = 0;
  linkSpeedCode = 1;
  linkState = LINK_STATE_RUN;
}

LINK_NOINLINE uint8_t linkPhasePercent() {
  switch (linkPhase) {
    case LINK_PHASE_QUALITY: {
      uint32_t elapsed = millis() - linkPhaseStart;
      return elapsed >= LINK_QUALITY_MS
                 ? 100
                 : (uint8_t)((elapsed * 100UL) / LINK_QUALITY_MS);
    }
    case LINK_PHASE_FRAMES:
      return (uint8_t)(((uint16_t)linkFrameIndex * LINK_FRAME_TX_COUNT +
                        linkFrameTx) *
                       100UL / (LINK_FRAME_COUNT * LINK_FRAME_TX_COUNT));
    case LINK_PHASE_THROUGHPUT: {
      const uint8_t percent =
          (uint8_t)((linkThroughputBytes * 100UL) / LINK_THROUGHPUT_BYTES);
      return percent > 100 ? 100 : percent;
    }
    case LINK_PHASE_SPEED:
      return (uint8_t)(((uint16_t)linkSpeedIndex * LINK_SPEED_TX_COUNT +
                        linkSpeedTx) *
                       100UL / (LINK_SPEED_COUNT * LINK_SPEED_TX_COUNT));
    case LINK_PHASE_SOAK: {
      const uint32_t total = (uint32_t)linkSoakMinutes() * 60000UL;
      if (total == 0) {
        return 100;
      }
      const uint32_t elapsed = millis() - linkPhaseStart;
      return elapsed >= total ? 100 : (uint8_t)((elapsed * 100UL) / total);
    }
    default:
      return 100;
  }
}

LINK_NOINLINE void linkFinish(bool aborted) {
  linkSetSpeed(1);
  linkAborted = aborted;
  const uint8_t qualityRate = linkPercent(linkQualityOk, linkQualityTx);
  const uint8_t soakRate = linkPercent(linkSoakOk, linkSoakTx);

  bool framesPass = true;
  for (uint8_t i = 0; i < LINK_FRAME_COUNT; i++) {
    if (linkFrameResults[i].ok != LINK_FRAME_TX_COUNT ||
        linkFrameResults[i].fail != 0) {
      framesPass = false;
    }
  }

  const bool throughputPass =
      linkThroughputBytes >= LINK_THROUGHPUT_BYTES;
  const bool speed50 = linkSpeedResults[0].ok >= 58;
  const bool speed100 = linkSpeedResults[1].ok >= 58;
  bool soakPass = true;
  if (linkSoakMinutes() > 0) {
    soakPass = soakRate >= 95 && linkSoakTx >= 100;
  }

  const bool pass = !aborted && qualityRate >= 95 && linkQualityTx >= 100 &&
                    framesPass && throughputPass && speed50 && speed100 &&
                    soakPass && linkPeerOk;

  linkResult = pass ? 1 : 2;
  linkState = LINK_STATE_DONE;
  linkPage = 0;
}

LINK_NOINLINE void linkControllerStep() {
  linkUptimeReport = (uint16_t)(millis() / 1000);

  switch (linkPhase) {
    case LINK_PHASE_QUALITY: {
      if ((uint32_t)(millis() - linkPhaseStart) >= LINK_QUALITY_MS) {
        linkPhase = LINK_PHASE_FRAMES;
        linkFrameIndex = 0;
        linkFrameTx = 0;
        linkPhaseStart = millis();
      } else {
        const bool ok = linkTransaction(LINK_CMD_PING, LINK_MIN_FRAME);
        linkQualityTx++;
        if (ok) {
          linkQualityOk++;
        }
      }
      break;
    }

    case LINK_PHASE_FRAMES: {
      if (linkFrameIndex >= LINK_FRAME_COUNT) {
        linkPhase = LINK_PHASE_THROUGHPUT;
        linkThroughputBytes = 0;
        linkThroughputStart = millis();
        linkPhaseStart = millis();
        break;
      }
      const uint8_t size = LINK_FRAME_SIZES[linkFrameIndex];
      const bool ok = linkTransaction(LINK_CMD_FRAME, size);
      if (ok) {
        linkFrameResults[linkFrameIndex].ok++;
      } else {
        linkFrameResults[linkFrameIndex].fail++;
      }
      linkFrameTx++;
      if (linkFrameTx >= LINK_FRAME_TX_COUNT) {
        linkFrameIndex++;
        linkFrameTx = 0;
      }
      break;
    }

    case LINK_PHASE_THROUGHPUT: {
      // Burst several frames back-to-back so the result reflects the bus,
      // not the 60 Hz UI loop (one transaction per frame would cap it at
      // roughly 1 KB/s). Bounded to keep the UI responsive.
      linkThroughputStart = millis();
      while (linkThroughputBytes < LINK_THROUGHPUT_BYTES &&
             linkThroughputTx < 96 &&
             (uint32_t)(millis() - linkThroughputStart) < 3000) {
        if (linkTransaction(LINK_CMD_FRAME, LINK_MAX_FRAME)) {
          linkThroughputBytes +=
              (uint32_t)(LINK_MAX_FRAME - LINK_MIN_FRAME);
        }
        linkThroughputTx++;
      }
      linkThroughputMs = millis() - linkThroughputStart;
      linkPhase = LINK_PHASE_SOAK;
      linkPhaseStart = millis();
      linkSoakEnd = millis() + (uint32_t)linkSoakMinutes() * 60000UL;
      break;
    }

    case LINK_PHASE_SPEED: {
      if (linkSpeedIndex >= LINK_SPEED_COUNT) {
        linkSetSpeed(1);
        linkFinish(false);
        break;
      }
      const bool ok = linkTransaction(LINK_CMD_PING, LINK_MIN_FRAME);
      if (ok) {
        linkSpeedResults[linkSpeedIndex].ok++;
      } else {
        linkSpeedResults[linkSpeedIndex].fail++;
      }
      linkSpeedTx++;
      if (linkSpeedTx >= LINK_SPEED_TX_COUNT) {
        linkSpeedIndex++;
        if (linkSpeedIndex < LINK_SPEED_COUNT) {
          linkSetSpeed(linkSpeedIndex);
        }
        linkSpeedTx = 0;
      }
      break;
    }

    case LINK_PHASE_INFO: {
      if (linkTransaction(LINK_CMD_INFO, 32)) {
        linkReadPeerInfo();
      }
      linkPhase = LINK_PHASE_SPEED;
      linkPhaseStart = millis();
      linkSpeedIndex = 0;
      linkSpeedTx = 0;
      linkSetSpeed(0);
      break;
    }

    case LINK_PHASE_SOAK: {
      if (linkSoakMinutes() == 0 ||
          (int32_t)(millis() - linkSoakEnd) >= 0) {
        linkPhase = LINK_PHASE_INFO;
        linkPhaseStart = millis();
        break;
      }
      const bool ok = linkTransaction(LINK_CMD_PING, LINK_MIN_FRAME);
      linkSoakTx++;
      if (ok) {
        linkSoakOk++;
      }
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Link screens
// ---------------------------------------------------------------------------

LINK_NOINLINE void drawLinkMenu() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("LINK TEST HOST"));
  arduboy.setCursor(0, 8);
  arduboy.print(F("BUS "));
  printHexByte(linkBusBits);
  arduboy.print(F(" HSD "));
  arduboy.print(linkHandshakeMs);
  arduboy.print(F("ms"));

  arduboy.setCursor(0, 16);
  arduboy.print(F("SPD "));
  arduboy.print(linkSpeedName(1));
  arduboy.print(F("  FLIP "));
  arduboy.print(linkFlipSeen ? F("Y") : F("N"));

  arduboy.setCursor(0, 24);
  arduboy.print(F("A:RUN ALL TESTS"));

  arduboy.setCursor(0, 32);
  arduboy.print(F("SOAK "));
  arduboy.print(linkSoakText());
  arduboy.setCursor(0, 40);
  arduboy.print(F("UP/DN: CHANGE SOAK"));

  arduboy.setCursor(0, 48);
  arduboy.print(F("B:EXIT"));

  arduboy.setCursor(0, 56);
  arduboy.print(F("A=START  B=EXIT"));
  arduboy.display();
}

LINK_NOINLINE void drawLinkRunning() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("LINK HOST"));
  arduboy.setCursor(0, 8);
  arduboy.print(linkPhaseName());

  arduboy.setCursor(0, 16);
  arduboy.print(F("SPD "));
  arduboy.print(linkSpeedName(linkSpeedCode));
  if (linkPhase == LINK_PHASE_FRAMES) {
    arduboy.print(F(" FRM "));
    arduboy.print(LINK_FRAME_SIZES[linkFrameIndex < LINK_FRAME_COUNT
                                     ? linkFrameIndex
                                     : LINK_FRAME_COUNT - 1]);
    arduboy.print('B');
  } else if (linkPhase == LINK_PHASE_THROUGHPUT) {
    arduboy.print(F(" FRM 48B"));
  }

  arduboy.setCursor(0, 24);
  arduboy.print(F("TX "));
  arduboy.print(linkStats.tx);
  arduboy.setCursor(64, 24);
  arduboy.print(F("OK "));
  arduboy.print(linkStats.ok);

  arduboy.setCursor(0, 32);
  arduboy.print(F("LOST "));
  arduboy.print(linkStats.lost);
  arduboy.setCursor(64, 32);
  arduboy.print(F("RTY "));
  arduboy.print(linkStats.retries);

  arduboy.setCursor(0, 40);
  arduboy.print(F("PROG "));
  arduboy.print(linkPhasePercent());
  arduboy.print(F("%  EL "));
  arduboy.print((uint32_t)(millis() - linkPhaseStart) / 1000);
  arduboy.print('s');

  arduboy.setCursor(0, 48);
  arduboy.print(F("RTT "));
  arduboy.print(linkStats.rttLast);
  arduboy.print(F("us  LOST "));
  arduboy.print(linkStats.lost);

  arduboy.setCursor(0, 56);
  arduboy.print(F("B:ABORT"));
  arduboy.display();
}

LINK_NOINLINE void drawLinkHistogram() {
  uint16_t maxValue = 1;
  for (uint8_t i = 0; i < 8; i++) {
    if (linkStats.hist[i] > maxValue) {
      maxValue = linkStats.hist[i];
    }
  }
  for (uint8_t i = 0; i < 8; i++) {
    const uint8_t x = (uint8_t)(8 + i * 15);
    const uint8_t height =
        (uint8_t)((uint32_t)linkStats.hist[i] * 23UL / maxValue);
    if (height) {
      arduboy.fillRect(x, (uint8_t)(39 - height), 11, height, WHITE);
    }
    arduboy.drawRect(x, 39, 11, 1, WHITE);
  }
}

LINK_NOINLINE void drawLinkDonePage() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("LINK DONE "));
  arduboy.print(linkResult == 1 ? F("PASS") : F("FAIL"));
  if (linkAborted) {
    arduboy.print(F(" ABRT"));
  }

  if (linkPage == 0) {
    arduboy.setCursor(0, 8);
    arduboy.print(F("RATE "));
    arduboy.print(linkPercent(linkStats.ok, linkStats.tx));
    arduboy.print(F("% TX "));
    arduboy.print(linkStats.tx);

    arduboy.setCursor(0, 16);
    arduboy.print(F("OK "));
    arduboy.print(linkStats.ok);
    arduboy.print(F(" LOST "));
    arduboy.print(linkStats.lost);

    arduboy.setCursor(0, 24);
    arduboy.print(F("RTT "));
    arduboy.print(linkStats.rttLast);
    arduboy.print(F(" AVG "));
    arduboy.print(linkStats.rttCount ? (uint16_t)(linkStats.rttSum / linkStats.rttCount) : 0);

    arduboy.setCursor(0, 32);
    arduboy.print(F("MIN "));
    arduboy.print(linkStats.rttMin == 0xFFFF ? 0 : linkStats.rttMin);
    arduboy.print(F(" MAX "));
    arduboy.print(linkStats.rttMax);

    arduboy.setCursor(0, 40);
    arduboy.print(F("JIT "));
    arduboy.print(linkStats.rttCount > 1 ? (uint16_t)(linkStats.jitterSum / (linkStats.rttCount - 1)) : 0);
    arduboy.print(F("us MAXF "));
    arduboy.print(linkStats.maxConsecFails);

    arduboy.setCursor(0, 48);
    arduboy.print(F("SPD "));
    arduboy.print(linkSpeedName(1));
    arduboy.print(F(" BUS "));
    printHexByte(linkBusBits);
  } else if (linkPage == 1) {
    arduboy.setCursor(0, 8);
    arduboy.print(F("QUALITY "));
    arduboy.print(linkPercent(linkQualityOk, linkQualityTx));
    arduboy.print(F("%  TX "));
    arduboy.print(linkQualityTx);

    arduboy.setCursor(0, 16);
    arduboy.print(F("WR "));
    arduboy.print(linkStats.errors[LINK_ERR_WRITE]);
    arduboy.print(F(" RD "));
    arduboy.print(linkStats.errors[LINK_ERR_READ]);
    arduboy.print(F(" CRC "));
    arduboy.print(linkStats.errors[LINK_ERR_CHECKSUM]);

    arduboy.setCursor(0, 24);
    arduboy.print(F("MAG "));
    arduboy.print(linkStats.errors[LINK_ERR_MAGIC]);
    arduboy.print(F(" STA "));
    arduboy.print(linkStats.errors[LINK_ERR_STATUS]);
    arduboy.print(F(" SEQ "));
    arduboy.print(linkStats.errors[LINK_ERR_SEQ]);

    arduboy.setCursor(0, 32);
    arduboy.print(F("PAY "));
    arduboy.print(linkStats.errors[LINK_ERR_PAYLOAD]);
    arduboy.print(F(" RTY "));
    arduboy.print(linkStats.retries);

    arduboy.setCursor(0, 40);
    arduboy.print(F("LOG "));
    for (uint8_t i = 0; i < linkStats.errLogCount && i < 4; i++) {
      const uint8_t idx = (uint8_t)((linkStats.errLogPos + 6 - linkStats.errLogCount + i) % 6);
      printHexByte(linkStats.errLog[idx]);
      arduboy.print(' ');
    }

    arduboy.setCursor(0, 48);
    arduboy.print(F("BUS "));
    printHexByte(linkBusBits);
    arduboy.print(F(" HSD "));
    arduboy.print(linkHandshakeMs);
    arduboy.print(F("ms"));
  } else if (linkPage == 2) {
    arduboy.setCursor(0, 8);
    arduboy.print(F("MAX FRAME"));
    for (uint8_t i = 0; i < LINK_FRAME_COUNT; i++) {
      arduboy.setCursor((i % 2) * 64, (uint8_t)(16 + (i / 2) * 8));
      arduboy.print(LINK_FRAME_SIZES[i]);
      arduboy.print('B');
      arduboy.print(' ');
      arduboy.print(linkFrameResults[i].ok);
      arduboy.print('/');
      arduboy.print(LINK_FRAME_TX_COUNT);
    }

    arduboy.setCursor(0, 40);
    arduboy.print(F("GOODPUT "));
    if (linkThroughputMs) {
      const uint32_t kb10 =
          (linkThroughputBytes * 10000UL) / (linkThroughputMs * 1024UL);
      arduboy.print(kb10 / 10);
      arduboy.print('.');
      arduboy.print(kb10 % 10);
    } else {
      arduboy.print(F("--"));
    }
    arduboy.print(F(" KB/s"));

    arduboy.setCursor(0, 48);
    arduboy.print(F("PAYLOAD "));
    arduboy.print(linkThroughputBytes);
    arduboy.print(F("B  "));
    arduboy.print(linkThroughputMs);
    arduboy.print(F("ms"));
  } else if (linkPage == 3) {
    arduboy.setCursor(0, 8);
    arduboy.print(F("SPEED SWEEP"));
    for (uint8_t i = 0; i < LINK_SPEED_COUNT; i++) {
      arduboy.setCursor((i % 2) * 64, (uint8_t)(16 + (i / 2) * 8));
      arduboy.print(linkSpeedName(i));
      arduboy.print(' ');
      arduboy.print(linkSpeedResults[i].ok);
      arduboy.print('/');
      arduboy.print(LINK_SPEED_TX_COUNT);
    }

    if (linkPeerOk) {
      arduboy.setCursor(0, 32);
      arduboy.print(F("PEER "));
      printHexWord(linkPeer.appCrc);
      arduboy.print(F(" V"));
      arduboy.print(linkPeer.version);
      arduboy.print(F(" CAP "));
      arduboy.print(linkPeer.capacity);
      arduboy.setCursor(0, 40);
      arduboy.print(F("UP "));
      arduboy.print(linkPeer.uptimeSec);
      arduboy.print(F("s RX "));
      arduboy.print(linkPeer.rxValid);
      arduboy.setCursor(0, 48);
      arduboy.print(F("BAD "));
      arduboy.print(linkPeer.rxBad);
      arduboy.setCursor(64, 48);
      arduboy.print(F("SPD "));
      arduboy.print(linkSpeedName(linkPeer.speed));
    } else {
      arduboy.setCursor(0, 32);
      arduboy.print(F("NO PEER INFO"));
    }
  } else {
    arduboy.setCursor(0, 8);
    arduboy.print(F("RTT HISTOGRAM"));
    drawLinkHistogram();
    arduboy.setCursor(0, 40);
    arduboy.print(F("MIN "));
    arduboy.print(linkStats.rttMin == 0xFFFF ? 0 : linkStats.rttMin);
    arduboy.print(F(" AVG "));
    arduboy.print(linkStats.rttCount ? (uint16_t)(linkStats.rttSum / linkStats.rttCount) : 0);
    arduboy.setCursor(0, 48);
    arduboy.print(F("MAX "));
    arduboy.print(linkStats.rttMax);
    arduboy.print(F(" JIT "));
    arduboy.print(linkStats.rttCount > 1 ? (uint16_t)(linkStats.jitterSum / (linkStats.rttCount - 1)) : 0);
  }

  arduboy.setCursor(0, 56);
  arduboy.print(F("UP/DN B:EXIT"));
  arduboy.display();
}

LINK_NOINLINE void drawLinkTarget() {
  arduboy.clear();
  arduboy.setCursor(0, 0);
  arduboy.print(F("LINK TARGET"));

  if (!linkHaveFrame) {
    arduboy.setCursor(0, 24);
    arduboy.print(F("WAITING FOR HOST"));
    arduboy.setCursor(0, 40);
    arduboy.print(F("B:EXIT"));
    arduboy.display();
    return;
  }

  arduboy.setCursor(0, 8);
  arduboy.print(F("SPD "));
  arduboy.print(linkSpeedName(linkLastSpeed));
  arduboy.print(F("  FRM "));
  arduboy.print(linkLastSize);
  arduboy.print('B');

  arduboy.setCursor(0, 16);
  arduboy.print(F("CMD "));
  arduboy.print(linkCmdName(linkLastCmd));

  arduboy.setCursor(0, 24);
  arduboy.print(F("RX "));
  arduboy.print(linkValidTotal);
  arduboy.setCursor(64, 24);
  arduboy.print(F("BAD "));
  arduboy.print(linkBadTotal);

  arduboy.setCursor(0, 32);
  arduboy.print((uint32_t)(millis() - linkLastValidMs) < 750
                    ? F("LINK:OK")
                    : F("LINK:LOST"));

  arduboy.setCursor(0, 40);
  arduboy.print(F("UP "));
  arduboy.print((uint16_t)(millis() / 1000));
  arduboy.print('s');

  arduboy.setCursor(0, 48);
  arduboy.print(F("B:EXIT"));
  arduboy.display();
}

LINK_NOINLINE void linkPollTarget() {
  uint8_t valid;
  uint8_t bad;
  const uint8_t savedSreg = SREG;
  cli();
  valid = linkValidDelta;
  linkValidDelta = 0;
  bad = linkBadDelta;
  linkBadDelta = 0;
  SREG = savedSreg;

  if (valid) {
    if (linkValidTotal > 65000) {
      linkValidTotal = 65535;
    } else {
      linkValidTotal += valid;
    }
    linkLastValidMs = millis();
  }
  if (linkBadTotal > 65000) {
    linkBadTotal = 65535;
  } else {
    linkBadTotal += bad;
  }
  linkValidReport = linkValidTotal;
  linkBadReport = linkBadTotal;
  linkUptimeReport = (uint16_t)(millis() / 1000);
}

LINK_NOINLINE void linkLoop() {
  if (linkRole == I2C::Role::Target) {
    if (linkExitRequested) {
      linkCleanup();
      screen = SCR_MENU;
      return;
    }
    linkPollTarget();
    if (arduboy.justPressed(B_BUTTON)) {
      linkCleanup();
      screen = SCR_MENU;
      return;
    }
    drawLinkTarget();
    return;
  }

  switch (linkState) {
    case LINK_STATE_MENU:
      if (arduboy.justPressed(UP_BUTTON)) {
        linkSoakCode = (uint8_t)((linkSoakCode + 3) % 4);
      }
      if (arduboy.justPressed(DOWN_BUTTON)) {
        linkSoakCode = (uint8_t)((linkSoakCode + 1) % 4);
      }
      if (arduboy.justPressed(A_BUTTON)) {
        linkStartSuite();
        return;
      }
      if (arduboy.justPressed(B_BUTTON)) {
        linkExitToMenu();
        return;
      }
      drawLinkMenu();
      break;

    case LINK_STATE_RUN:
      if (arduboy.justPressed(B_BUTTON)) {
        linkFinish(true);
        return;
      }
      linkControllerStep();
      drawLinkRunning();
      break;

    case LINK_STATE_DONE:
      if (arduboy.justPressed(UP_BUTTON)) {
        linkPage = (uint8_t)((linkPage + LINK_PAGE_COUNT - 1) % LINK_PAGE_COUNT);
      }
      if (arduboy.justPressed(DOWN_BUTTON)) {
        linkPage = (uint8_t)((linkPage + 1) % LINK_PAGE_COUNT);
      }
      if (arduboy.justPressed(A_BUTTON)) {
        linkStartSuite();
        return;
      }
      if (arduboy.justPressed(B_BUTTON)) {
        linkExitToMenu();
        return;
      }
      drawLinkDonePage();
      break;

    default:
      linkState = LINK_STATE_MENU;
      break;
  }
}

// ---------------------------------------------------------------------------
// Screen entry and main loop
// ---------------------------------------------------------------------------

void screenEnter(Screen next) {
  screen = next;
  switch (next) {
    case SCR_DISPLAY:
      displayEnter();
      break;
    case SCR_BUTTONS:
      buttonsEnter();
      break;
    case SCR_SOUND:
      soundEnter();
      break;
    case SCR_RGB:
      rgbEnter();
      break;
    case SCR_FLASH:
      fxEnter();
      break;
    case SCR_EEPROM:
      eepromEnter();
      break;
    case SCR_LINK:
      linkEnter();
      break;
    default:
      break;
  }
}

void setup() {
  // Raw hardware initialisation only. Deliberately skip Arduboy2's boot logo
  // and its "wait until all buttons are released" loop: a diagnostic tool
  // must still start when a button is stuck, and immediate visual feedback
  // makes it obvious whether the application is actually running.
  arduboy.boot();
  arduboy.setFrameRate(60);
  arduboy.audio.begin();

  arduboy.clear();
  arduboy.setCursor(43, 20);
  arduboy.print(F("FXCtest"));
  arduboy.setCursor(31, 32);
  arduboy.print(F("STARTING..."));
  arduboy.display();

  beep.begin();

  // Keep the external flash chip select deselected during start-up. The SPI
  // pins and flash protocol are only touched inside FX FLASH, so the start-up
  // path cannot be affected by a broken flash bus.
  flashPrepare();

  // Do NOT touch the external flash here. If the FX flash bus is faulty, the
  // app must still reach the menu so the remaining diagnostics stay usable.
  // FX FLASH runs on demand and shows a progress message before accessing it.
  arduboy.delayShort(200);
  screen = SCR_MENU;
}

void loop() {
  if (!arduboy.nextFrame()) {
    return;
  }
  arduboy.pollButtons();

  switch (screen) {
    case SCR_MENU:
      if (arduboy.justPressed(UP_BUTTON)) {
        moveMenu(-1);
      }
      if (arduboy.justPressed(DOWN_BUTTON)) {
        moveMenu(1);
      }
      if (arduboy.justPressed(A_BUTTON)) {
        screenEnter((Screen)(SCR_DISPLAY + menuIndex));
      }
      drawMenu();
      break;

    case SCR_DISPLAY:
      displayLoop();
      break;

    case SCR_BUTTONS:
      buttonsLoop();
      break;

    case SCR_SOUND:
      soundLoop();
      break;

    case SCR_RGB:
      rgbLoop();
      break;

    case SCR_FLASH:
      flashLoop();
      break;

    case SCR_EEPROM:
      eepromLoop();
      break;

    case SCR_LINK:
      linkLoop();
      break;

    default:
      screen = SCR_MENU;
      break;
  }
}
