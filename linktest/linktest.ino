/*
  Arduboy FX-C - I2C Link Tester
  ==============================

  A two-machine communication test for the Arduboy FX-C, built on the
  ArduboyI2C library (https://github.com/sub1inear/ArduboyI2C).

  Flash this same sketch onto BOTH FX-C units, connect them with the FX-C
  link cable and power them on. The library handshake assigns one unit as
  the controller (shown as HOST) and the other as the target (shown as
  GUEST). The host then continuously sends 8-byte ping packets and checks
  the 8-byte pong replies (sequence, cookie, checksum, status), measuring
  round-trip time and showing counters and a 32-packet success trace.

  Notes
  -----
  * ArduboyI2C requires I2C_IMPLEMENTATION to be defined in exactly one
    source file, before including ArduboyI2C.h.
  * I2C::begin() must be called after arduboy.begin().
  * The FX-C only has software pull-ups, so ArduboyI2C only compiles with
    I2C_FREQUENCY <= 200 kHz (default 100 kHz). The cable-flip detection
    timings are tuned for the default 100 kHz.
  * I2C::checkCableFlipped() is called before the handshake. The link cable
    of the FX-C can be plugged in either way; the device that sees the
    flipped cable asks the user to flip it back (see the library docs).
  * Callbacks run inside the I2C interrupt handler: they must not use
    delay(), millis(), Serial, etc. All packet parsing/reply building is
    done with plain variables, and buffered counters are drained by the
    main loop.

  Wiring
  ------
    FX-C #1 [LINK connector] <==== FX-C link cable ====> [LINK connector] FX-C #2
    (ArduboyI2C uses the TWI pins PD0/PD1 - Arduino D3 = SCL, D2 = SDA -
     with the internal pull-ups enabled by I2C::begin().)

  Controls
  --------
    HOST : A = stop and search for the peer again, B = reset statistics
    GUEST: B = reset statistics

  The host re-links automatically when the link has been down for 2 s. The
  guest periodically checks the cable orientation while the link is lost
  and prompts to flip the cable if it is inserted backwards.

  Tested with Arduboy2 6.0.0 and ArduboyI2C 3.0.0.
*/

#include <Arduboy2.h>

// Must be defined in exactly one file before including ArduboyI2C.h.
#define I2C_IMPLEMENTATION
#include <ArduboyI2C.h>

// ---------------------------------------------------------------------------
// Protocol and timing constants
// ---------------------------------------------------------------------------

constexpr uint8_t PROTOCOL_VERSION = 1;

constexpr uint8_t PING_MAGIC = 0xA5;
constexpr uint8_t PONG_MAGIC = 0x5A;

constexpr uint8_t PONG_STATUS_OK = 0;
constexpr uint8_t PONG_STATUS_NO_PING = 1;

// Single byte written by I2C::handshake() while it is looking for the peer.
constexpr uint8_t PROBE_BYTE = 0x00;

// Attempts for one ping/pong transaction (write ping + read pong).
constexpr uint8_t CONTROLLER_MAX_ATTEMPTS = 3;

// The host treats the link as lost when no valid pong arrived for this long.
constexpr uint16_t LINK_LOST_MS = 750;

// The host starts searching (handshake) when the link stays down this long.
constexpr uint16_t AUTO_RELINK_MS = 2000;

// The guest checks the cable orientation this often while the link is lost.
constexpr uint16_t TARGET_FLIP_CHECK_MS = 1500;

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------
//
// Both packets are exactly 8 bytes so the default I2C_BUFFER_CAPACITY (32)
// is more than enough and the transaction time stays short.
// The last two bytes are a Fletcher-16 checksum over the preceding bytes.

struct PingPacket {
  uint8_t magic;
  uint8_t version;
  uint8_t seq;
  uint8_t flags;
  uint16_t cookie;
  uint16_t checksum;
};

struct PongPacket {
  uint8_t magic;
  uint8_t version;
  uint8_t seq;      // echoed ping sequence
  uint8_t status;   // PONG_STATUS_*
  uint16_t cookie;  // echoed ping cookie
  uint16_t checksum;
};

static_assert(sizeof(PingPacket) == 8, "PingPacket must be 8 bytes");
static_assert(sizeof(PongPacket) == 8, "PongPacket must be 8 bytes");

// ---------------------------------------------------------------------------
// Failure reasons (shown on the host screen)
// ---------------------------------------------------------------------------

enum FailReason : uint8_t {
  FAIL_NONE = 0,
  FAIL_WRITE_ADDR_NACK,
  FAIL_WRITE_DATA_NACK,
  FAIL_READ_ADDR_NACK,
  FAIL_BUS_ERROR,
  FAIL_BAD_MAGIC,
  FAIL_BAD_CHECKSUM,
  FAIL_BAD_SEQ,
  FAIL_BAD_COOKIE,
  FAIL_PEER_NOT_READY
};

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------

Arduboy2 arduboy;
I2C::Role role = I2C::Role::Controller;

struct ControllerState {
  uint8_t seq;
  uint32_t tx;      // completed transactions
  uint32_t ok;      // transactions with a valid pong
  uint32_t lost;    // transactions that failed all attempts
  uint32_t retries; // extra attempts inside transactions
  uint32_t trace;   // last 32 transaction results, newest in bit 0
  uint16_t rttLast;
  uint16_t rttMin;
  uint16_t rttMax;
  uint32_t rttSum;      // sum of accepted RTT samples
  uint32_t rttSamples;  // number of accepted RTT samples
  uint32_t lastOkMs;    // millis() of the last valid pong
  uint8_t failReason;
} hostState;

struct TargetState {
  uint32_t valid;   // valid pings received
  uint32_t bad;     // invalid pings / unexpected writes
  uint32_t probes;  // handshake probe writes (0x00) seen
  uint32_t trace;   // last 32 received pings, newest in bit 0
  uint8_t lastSeq;
  uint32_t lastValidMs;
  uint32_t lastFlipCheckMs;
} guestState;

// --- target (ISR-side) state -----------------------------------------------
//
// The I2C callbacks run with interrupts disabled. They only touch the
// variables below; the main loop drains the volatile counters.

PingPacket isrLastPing;
bool isrHavePing = false;
volatile uint8_t isrValidCount = 0;
volatile uint8_t isrBadCount = 0;
volatile uint8_t isrProbeCount = 0;
volatile uint8_t isrLastSeq = 0xFF;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Fletcher-16 over the given bytes. Cheap enough to run inside the ISR and
// catches corrupted packets (the I2C hardware itself only reports ACK/NACK).
uint16_t checksum16(const uint8_t *data, uint8_t length) {
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

// Sequence dependent payload; a corrupt or stale packet is much more likely
// to be caught than by the sequence number alone.
uint16_t makeCookie(uint8_t seq) {
  uint32_t value = (uint32_t)seq * 2654435761UL + 0x9E3779B9UL;
  value ^= value >> 13;
  value ^= value << 7;
  value ^= value >> 17;
  return (uint16_t)value;
}

uint8_t failReasonFromError(I2C::Error error) {
  switch (error) {
    case I2C::Error::WriteAddrNack: return FAIL_WRITE_ADDR_NACK;
    case I2C::Error::WriteDataNack: return FAIL_WRITE_DATA_NACK;
    case I2C::Error::ReadAddrNack:  return FAIL_READ_ADDR_NACK;
    case I2C::Error::Bus:           return FAIL_BUS_ERROR;
    default:                        return FAIL_BUS_ERROR;
  }
}

const __FlashStringHelper *failReasonText(uint8_t reason) {
  switch (reason) {
    case FAIL_WRITE_ADDR_NACK: return F("WRITE_ADDR_NACK");
    case FAIL_WRITE_DATA_NACK: return F("WRITE_DATA_NACK");
    case FAIL_READ_ADDR_NACK:  return F("READ_ADDR_NACK");
    case FAIL_BUS_ERROR:       return F("BUS_ERROR");
    case FAIL_BAD_MAGIC:       return F("BAD_MAGIC");
    case FAIL_BAD_CHECKSUM:    return F("BAD_CHECKSUM");
    case FAIL_BAD_SEQ:         return F("BAD_SEQ");
    case FAIL_BAD_COOKIE:      return F("BAD_COOKIE");
    case FAIL_PEER_NOT_READY:  return F("PEER_NOT_READY");
    default:                   return F("NONE");
  }
}

// Counters are shown with at most five digits; very long runs saturate the
// display instead of drawing past the right edge of the screen.
void printAmount(uint32_t value) {
  if (value > 99999UL) {
    value = 99999UL;
  }
  arduboy.print(value);
}

uint8_t popcount32(uint32_t value) {
  uint8_t count = 0;
  while (value) {
    count += value & 1UL;
    value >>= 1;
  }
  return count;
}

void printHexByte(uint8_t value) {
  if (value < 0x10) {
    arduboy.print('0');
  }
  arduboy.print(value, HEX);
}

// ---------------------------------------------------------------------------
// Target (I2C target) callbacks - keep these short, no millis()/delay()
// ---------------------------------------------------------------------------

void onTargetReceive() {
  const uint8_t *buffer = I2C::getBuffer();
  const uint8_t size = I2C::getBufferSize();

  if (size == sizeof(PingPacket) &&
      buffer[0] == PING_MAGIC &&
      buffer[1] == PROTOCOL_VERSION) {
    const PingPacket *ping = reinterpret_cast<const PingPacket *>(buffer);
    const uint16_t expected =
        checksum16(buffer, sizeof(PingPacket) - sizeof(ping->checksum));
    if (ping->checksum == expected) {
      // A retried ping has the same sequence number; do not count it twice.
      const bool isNewSequence = !isrHavePing || ping->seq != isrLastPing.seq;
      isrLastPing = *ping;
      isrHavePing = true;
      isrLastSeq = ping->seq;
      if (isNewSequence && isrValidCount != 255) {
        isrValidCount++;
      }
      return;
    }
  } else if (size == 1 && buffer[0] == PROBE_BYTE) {
    // Single zero byte from I2C::handshake() while the host looks for us.
    // Forget the last ping of the previous session so a pong from it can
    // never be echoed back after a re-link.
    isrHavePing = false;
    if (isrProbeCount != 255) {
      isrProbeCount++;
    }
    return;
  }

  if (isrBadCount != 255) {
    isrBadCount++;
  }
}

void onTargetRequest() {
  PongPacket pong;
  pong.magic = PONG_MAGIC;
  pong.version = PROTOCOL_VERSION;

  if (isrHavePing) {
    pong.status = PONG_STATUS_OK;
    pong.seq = isrLastPing.seq;
    pong.cookie = isrLastPing.cookie;
  } else {
    pong.status = PONG_STATUS_NO_PING;
    pong.seq = 0xFF;
    pong.cookie = 0;
  }

  pong.checksum = checksum16(reinterpret_cast<const uint8_t *>(&pong),
                             sizeof(PongPacket) - sizeof(pong.checksum));
  I2C::reply(pong);
}

// ---------------------------------------------------------------------------
// Statistics / session handling
// ---------------------------------------------------------------------------

void resetControllerStatistics() {
  // The sequence number is deliberately NOT reset here: keeping it monotonic
  // across re-links and statistic resets means a pong left over from an older
  // session can never match the next ping's sequence/cookie by accident.
  hostState.tx = 0;
  hostState.ok = 0;
  hostState.lost = 0;
  hostState.retries = 0;
  hostState.trace = 0;
  hostState.rttLast = 0;
  hostState.rttMin = 0xFFFF;
  hostState.rttMax = 0;
  hostState.rttSum = 0;
  hostState.rttSamples = 0;
  hostState.failReason = FAIL_NONE;
}

void resetTargetStatistics() {
  guestState.valid = 0;
  guestState.bad = 0;
  guestState.probes = 0;
  guestState.trace = 0;
  guestState.lastSeq = 0xFF;
}

void resetIsrCounters() {
  const uint8_t savedSreg = SREG;
  cli();
  isrValidCount = 0;
  isrBadCount = 0;
  isrProbeCount = 0;
  SREG = savedSreg;
}

// Only used when a new session starts: forget the last ping so a pong from a
// previous session can never be echoed back.
void resetIsrPingState() {
  const uint8_t savedSreg = SREG;
  cli();
  isrHavePing = false;
  isrLastSeq = 0xFF;
  SREG = savedSreg;
}

// Called after every handshake. The library has already set our address to
// I2C::targetAddress when we became the target; callbacks must be installed
// before the address is enabled so no incoming packet is missed.
void startSession(I2C::Role newRole) {
  role = newRole;

  if (role == I2C::Role::Target) {
    resetIsrPingState();
    resetIsrCounters();
    I2C::onReceive(onTargetReceive);
    I2C::onRequest(onTargetRequest);
    I2C::setAddress(I2C::targetAddress);

    resetTargetStatistics();
    guestState.lastValidMs = millis();
    guestState.lastFlipCheckMs = millis();
  } else {
    I2C::onReceive(nullptr);
    I2C::onRequest(nullptr);
    I2C::setAddress(I2C::nullAddress);

    resetControllerStatistics();
    hostState.lastOkMs = millis();
  }
}

bool hostLinkUp() {
  return (uint32_t)(millis() - hostState.lastOkMs) < LINK_LOST_MS;
}

bool guestLinkUp() {
  return (uint32_t)(millis() - guestState.lastValidMs) < LINK_LOST_MS;
}

uint16_t averageRtt() {
  if (hostState.rttSamples == 0) {
    return 0;
  }
  return (uint16_t)(hostState.rttSum / hostState.rttSamples);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void drawSplashScreen() {
  arduboy.clear();
  arduboy.setCursor(28, 8);
  arduboy.print(F("ARDUBOY FX-C"));
  arduboy.setCursor(19, 20);
  arduboy.print(F("I2C LINK TESTER"));
  arduboy.setCursor(19, 34);
  arduboy.print(F("v1.0  LIB "));
  arduboy.print(I2C_VERSION_MAJOR);
  arduboy.print('.');
  arduboy.print(I2C_VERSION_MINOR);
  arduboy.print('.');
  arduboy.print(I2C_VERSION_PATCH);
  arduboy.setCursor(10, 48);
  arduboy.print(F("SAME SKETCH ON BOTH"));
  arduboy.display();
}

void drawCableFlippedScreen() {
  arduboy.clear();
  arduboy.setCursor(22, 16);
  arduboy.print(F("CABLE FLIPPED!"));
  arduboy.setCursor(7, 32);
  arduboy.print(F("FLIP THE LINK CABLE"));
  arduboy.setCursor(22, 44);
  arduboy.print(F("ON THIS DEVICE"));
  arduboy.display();
}

void drawSearchingScreen() {
  arduboy.clear();
  arduboy.setCursor(25, 16);
  arduboy.print(F("SEARCHING FOR"));
  arduboy.setCursor(28, 28);
  arduboy.print(F("ANOTHER UNIT"));
  arduboy.setCursor(10, 44);
  arduboy.print(F("CONNECT LINK CABLE"));
  // The handshake has no timeout. If both units were started at almost the
  // same instant, both can become controller and wait forever; restarting one
  // unit while the other keeps searching recovers the pair.
  arduboy.setCursor(10, 56);
  arduboy.print(F("STUCK? RESTART ONE"));
  arduboy.display();
}

void drawHeader(const __FlashStringHelper *roleName, bool linkUp) {
  arduboy.setCursor(0, 0);
  arduboy.print(roleName);
  arduboy.setCursor(42, 0);
  if (linkUp) {
    arduboy.print(F("LINK:OK"));
    arduboy.fillRect(118, 1, 7, 7, WHITE);
  } else {
    arduboy.print(F("LINK:LOST"));
    if ((millis() / 250) & 1) {
      arduboy.fillRect(118, 1, 7, 7, WHITE);
    }
  }
}

// 32 blocks, 4 pixels each: filled = good packet, outline = bad/lost.
void drawTraceBar(uint8_t y, uint32_t trace) {
  for (uint8_t i = 0; i < 32; i++) {
    const bool good = (trace >> (31 - i)) & 1UL;
    if (good) {
      arduboy.fillRect(i * 4, y, 3, 7, WHITE);
    } else {
      arduboy.drawRect(i * 4, y, 3, 7, WHITE);
    }
  }
}

void drawHostScreen() {
  const bool linkUp = hostLinkUp();
  arduboy.clear();
  drawHeader(F("HOST"), linkUp);

  arduboy.setCursor(0, 8);
  arduboy.print(F("TX "));
  printAmount(hostState.tx);
  arduboy.setCursor(66, 8);
  arduboy.print(F("OK "));
  printAmount(hostState.ok);

  arduboy.setCursor(0, 16);
  arduboy.print(F("LOST "));
  printAmount(hostState.lost);
  arduboy.setCursor(66, 16);
  arduboy.print(F("RTY "));
  printAmount(hostState.retries);

  arduboy.setCursor(0, 24);
  arduboy.print(F("RTT "));
  arduboy.print(hostState.rttLast);
  arduboy.setCursor(66, 24);
  arduboy.print(F("AVG "));
  arduboy.print(averageRtt());

  arduboy.setCursor(0, 32);
  arduboy.print(F("MIN "));
  arduboy.print(hostState.rttMin == 0xFFFF ? 0 : hostState.rttMin);
  arduboy.setCursor(66, 32);
  arduboy.print(F("MAX "));
  arduboy.print(hostState.rttMax);

  arduboy.setCursor(0, 40);
  arduboy.print(F("ERR "));
  arduboy.print(failReasonText(hostState.failReason));

  drawTraceBar(48, hostState.trace);

  arduboy.setCursor(0, 56);
  arduboy.print(F("A:RELINK B:RESET"));
  arduboy.display();
}

void drawGuestScreen() {
  const bool linkUp = guestLinkUp();
  arduboy.clear();
  drawHeader(F("GUEST"), linkUp);

  arduboy.setCursor(0, 8);
  arduboy.print(F("RX "));
  printAmount(guestState.valid);
  arduboy.setCursor(66, 8);
  arduboy.print(F("BAD "));
  printAmount(guestState.bad);

  arduboy.setCursor(0, 16);
  arduboy.print(F("PRB "));
  printAmount(guestState.probes);
  arduboy.setCursor(66, 16);
  arduboy.print(F("SEQ "));
  printHexByte(guestState.lastSeq);

  arduboy.setCursor(0, 24);
  arduboy.print(F("QUAL "));
  const uint32_t samples = guestState.valid + guestState.bad;
  if (samples == 0) {
    arduboy.print(F("--"));
  } else {
    const uint32_t window = samples < 32 ? samples : 32;
    const uint8_t good = popcount32(guestState.trace);
    arduboy.print((uint32_t)good * 100UL / window);
    arduboy.print('%');
  }

  arduboy.setCursor(0, 32);
  if (linkUp) {
    arduboy.print(F("HOST PINGS OK"));
  } else if (guestState.valid == 0) {
    arduboy.print(F("WAITING FOR HOST"));
  } else {
    arduboy.print(F("HOST SILENT"));
  }

  arduboy.setCursor(0, 40);
  arduboy.print(F("ADDR "));
  printHexByte(I2C::getAddress());
  arduboy.print(F("  LIB "));
  arduboy.print(I2C_VERSION_MAJOR);
  arduboy.print('.');
  arduboy.print(I2C_VERSION_MINOR);

  drawTraceBar(48, guestState.trace);

  arduboy.setCursor(0, 56);
  arduboy.print(F("B:RESET"));
  arduboy.display();
}

// ---------------------------------------------------------------------------
// Host (I2C controller) transaction
// ---------------------------------------------------------------------------

bool runHostTransaction() {
  PingPacket ping;
  ping.magic = PING_MAGIC;
  ping.version = PROTOCOL_VERSION;
  ping.seq = ++hostState.seq;
  ping.flags = 0;
  ping.cookie = makeCookie(ping.seq);
  ping.checksum = checksum16(reinterpret_cast<const uint8_t *>(&ping),
                             sizeof(PingPacket) - sizeof(ping.checksum));

  PongPacket pong;
  bool success = false;
  uint8_t failReason = FAIL_NONE;
  const uint32_t startUs = micros();

  for (uint8_t attempt = 0; attempt < CONTROLLER_MAX_ATTEMPTS && !success;
       attempt++) {
    if (attempt > 0) {
      hostState.retries++;
    }

    I2C::write(I2C::targetAddress, ping, I2C::Mode::Sync);
    I2C::Error error = I2C::getError();
    if (error != I2C::Error::None) {
      failReason = failReasonFromError(error);
      continue;
    }

    I2C::read(I2C::targetAddress, pong);
    error = I2C::getError();
    if (error != I2C::Error::None) {
      failReason = failReasonFromError(error);
      continue;
    }

    if (pong.magic != PONG_MAGIC || pong.version != PROTOCOL_VERSION) {
      failReason = FAIL_BAD_MAGIC;
      continue;
    }
    if (pong.checksum !=
        checksum16(reinterpret_cast<const uint8_t *>(&pong),
                   sizeof(PongPacket) - sizeof(pong.checksum))) {
      failReason = FAIL_BAD_CHECKSUM;
      continue;
    }
    if (pong.status != PONG_STATUS_OK) {
      failReason = FAIL_PEER_NOT_READY;
      continue;
    }
    if (pong.seq != ping.seq) {
      failReason = FAIL_BAD_SEQ;
      continue;
    }
    if (pong.cookie != ping.cookie) {
      failReason = FAIL_BAD_COOKIE;
      continue;
    }

    success = true;
  }

  const uint32_t elapsedUs = micros() - startUs;

  hostState.tx++;
  if (success) {
    hostState.ok++;
    hostState.rttLast =
        elapsedUs > 0xFFFFUL ? 0xFFFF : (uint16_t)elapsedUs;
    if (hostState.rttLast < hostState.rttMin) {
      hostState.rttMin = hostState.rttLast;
    }
    if (hostState.rttLast > hostState.rttMax) {
      hostState.rttMax = hostState.rttLast;
    }
    hostState.rttSum += hostState.rttLast;
    hostState.rttSamples++;
    if (hostState.rttSum > 0xF0000000UL) {
      hostState.rttSum >>= 1;
      hostState.rttSamples >>= 1;
    }
    hostState.lastOkMs = millis();
    hostState.failReason = FAIL_NONE;
  } else {
    hostState.lost++;
    hostState.failReason = failReason;
  }

  hostState.trace = (hostState.trace << 1) | (success ? 1UL : 0UL);
  return success;
}

// ---------------------------------------------------------------------------
// Guest event draining and cable-orientation self check
// ---------------------------------------------------------------------------

void pollGuestEvents() {
  uint8_t valid;
  uint8_t bad;
  uint8_t probes;
  uint8_t lastSeq;

  const uint8_t savedSreg = SREG;
  cli();
  valid = isrValidCount;
  isrValidCount = 0;
  bad = isrBadCount;
  isrBadCount = 0;
  probes = isrProbeCount;
  isrProbeCount = 0;
  lastSeq = isrLastSeq;
  SREG = savedSreg;

  if (valid != 0) {
    guestState.valid += valid;
    guestState.lastSeq = lastSeq;
    guestState.lastValidMs = millis();
    for (uint8_t i = 0; i < valid; i++) {
      guestState.trace = (guestState.trace << 1) | 1UL;
    }
  }
  if (bad != 0) {
    guestState.bad += bad;
    for (uint8_t i = 0; i < bad; i++) {
      guestState.trace <<= 1;
    }
  }
  guestState.probes += probes;
}

// While the link is lost the guest periodically checks whether the cable is
// plugged in backwards (the FX-C cable is reversible). This also recovers the
// case where both units booted at nearly the same instant with a flipped
// cable and therefore missed the initial check in setup().
void checkCableOrientationIfLost() {
  if (guestLinkUp()) {
    return;
  }
  if ((uint32_t)(millis() - guestState.lastFlipCheckMs) <
      TARGET_FLIP_CHECK_MS) {
    return;
  }
  guestState.lastFlipCheckMs = millis();
  I2C::checkCableFlipped(drawCableFlippedScreen);
}

// ---------------------------------------------------------------------------
// Re-link / reset actions
// ---------------------------------------------------------------------------

void relinkWithPeer() {
  drawSearchingScreen();
  I2C::Role newRole = I2C::handshake(drawSearchingScreen);
  startSession(newRole);
  // Give the other side a moment to install its callbacks.
  delay(20);
}

void resetStatistics() {
  if (role == I2C::Role::Controller) {
    resetControllerStatistics();
  } else {
    resetIsrCounters();
    resetTargetStatistics();
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  arduboy.begin();
  arduboy.setFrameRate(60);

  drawSplashScreen();
  delay(1200);

  // I2C::begin() must come after arduboy.begin(): boot() disables the TWI.
  I2C::begin();

  // On the FX-C the link cable can be inserted either way. This blocks with a
  // hint if the cable is flipped, and returns when it is oriented correctly.
  I2C::checkCableFlipped(drawCableFlippedScreen);

  // Waits for the other unit and decides which side is the controller.
  I2C::Role newRole = I2C::handshake(drawSearchingScreen);
  startSession(newRole);
  delay(20);
}

void loop() {
  if (!arduboy.nextFrame()) {
    return;
  }
  arduboy.pollButtons();

  if (arduboy.justPressed(B_BUTTON)) {
    resetStatistics();
  }

  if (role == I2C::Role::Controller) {
    // Manual re-link, and automatic recovery when the link stays down.
    if (arduboy.justPressed(A_BUTTON)) {
      relinkWithPeer();
      return;
    }
    if (!hostLinkUp() &&
        (uint32_t)(millis() - hostState.lastOkMs) >= AUTO_RELINK_MS) {
      relinkWithPeer();
      return;
    }

    runHostTransaction();
    drawHostScreen();
  } else {
    pollGuestEvents();
    checkCableOrientationIfLost();
    drawGuestScreen();
  }
}
