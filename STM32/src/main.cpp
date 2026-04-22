#include <Arduino.h>
#include <cstdio>
#include <cstring>

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 57600
#endif

#ifndef ESP_UART_BAUD
#define ESP_UART_BAUD 57600
#endif

#ifndef RELAY_ACTIVE_HIGH
#define RELAY_ACTIVE_HIGH 1
#endif

#ifndef LIGHT_RELAY_PIN
#define LIGHT_RELAY_PIN PA2
#endif

#ifndef PUMP_RELAY_PIN
#define PUMP_RELAY_PIN PA3
#endif

#ifndef MASTER_SWITCH_PIN
#define MASTER_SWITCH_PIN PA0
#endif

#ifndef LIGHT_BUTTON_PIN
#define LIGHT_BUTTON_PIN PA1
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN PC13
#endif

#ifndef INPUT_ACTIVE_LEVEL
#define INPUT_ACTIVE_LEVEL LOW
#endif

#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 1
#endif

namespace {

constexpr uint32_t INPUT_DEBOUNCE_MS = 35;
constexpr uint32_t DISPLAY_REFRESH_MS = 250;
constexpr uint32_t STATE_HEARTBEAT_MS = 2000;
constexpr uint32_t ESP_LINK_TIMEOUT_MS = 8000;
constexpr size_t RX_BUFFER_SIZE = 128;

constexpr uint8_t OLED_SCL_PIN = PB10;
constexpr uint8_t OLED_SDA_PIN = PB11;
constexpr uint8_t OLED_ADDR_8BIT_3C = 0x78;  // 0x3C << 1
constexpr uint8_t OLED_ADDR_8BIT_3D = 0x7A;  // 0x3D << 1
constexpr uint8_t OLED_PAGES = 8;
constexpr uint8_t OLED_WIDTH = 128;

uint8_t oledGram[OLED_PAGES][OLED_WIDTH];

char rxBuffer[RX_BUFFER_SIZE];
size_t rxLength = 0;

bool lightOn = false;
bool pumpOn = false;
bool masterSwitchOn = false;

uint32_t lastDisplayMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastEspSeenMs = 0;

char lastSource[20] = "BOOT";

struct DebouncedInput {
  bool raw;
  bool stable;
  uint32_t changedAtMs;
};

DebouncedInput masterSwitchInput{};
DebouncedInput lightButtonInput{};

void oledSclSet(bool level) {
  digitalWrite(OLED_SCL_PIN, level ? HIGH : LOW);
}

void oledSdaSet(bool level) {
  digitalWrite(OLED_SDA_PIN, level ? HIGH : LOW);
}

void oledI2CDelay() {
  delayMicroseconds(3);
}

void oledI2CStart() {
  oledSdaSet(true);
  oledSclSet(true);
  oledI2CDelay();
  oledSdaSet(false);
  oledI2CDelay();
  oledSclSet(false);
}

void oledI2CStop() {
  oledSdaSet(false);
  oledSclSet(true);
  oledI2CDelay();
  oledSdaSet(true);
  oledI2CDelay();
}

void oledI2CSendByte(uint8_t byteValue) {
  for (uint8_t i = 0; i < 8; i++) {
    oledSdaSet((byteValue & 0x80U) != 0U);
    oledSclSet(true);
    oledI2CDelay();
    oledSclSet(false);
    oledI2CDelay();
    byteValue <<= 1;
  }

  // Ignore ACK for robustness with different module variants.
  oledSdaSet(true);
  oledSclSet(true);
  oledI2CDelay();
  oledSclSet(false);
}

void oledWriteCommandToAddr(uint8_t addr8bit, uint8_t cmd) {
  oledI2CStart();
  oledI2CSendByte(addr8bit);
  oledI2CSendByte(0x00);
  oledI2CSendByte(cmd);
  oledI2CStop();
}

void oledWriteDataToAddr(uint8_t addr8bit, uint8_t data) {
  oledI2CStart();
  oledI2CSendByte(addr8bit);
  oledI2CSendByte(0x40);
  oledI2CSendByte(data);
  oledI2CStop();
}

void oledWriteCommandAll(uint8_t cmd) {
  oledWriteCommandToAddr(OLED_ADDR_8BIT_3C, cmd);
  oledWriteCommandToAddr(OLED_ADDR_8BIT_3D, cmd);
}

void oledWriteDataAll(uint8_t data) {
  oledWriteDataToAddr(OLED_ADDR_8BIT_3C, data);
  oledWriteDataToAddr(OLED_ADDR_8BIT_3D, data);
}

void oledClearBuffer() {
  memset(oledGram, 0, sizeof(oledGram));
}

void oledRefresh() {
  for (uint8_t page = 0; page < OLED_PAGES; page++) {
    oledWriteCommandAll(static_cast<uint8_t>(0xB0U + page));
    oledWriteCommandAll(0x10);
    oledWriteCommandAll(0x00);
    for (uint8_t col = 0; col < OLED_WIDTH; col++) {
      oledWriteDataAll(oledGram[page][col]);
    }
  }
}

void oledDrawPixel(uint8_t x, uint8_t y) {
  if (x >= OLED_WIDTH || y >= 64) {
    return;
  }
  oledGram[y / 8U][x] |= static_cast<uint8_t>(1U << (y % 8U));
}

const uint8_t *glyphForChar(char c) {
  static const uint8_t SPACE[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t COLON[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
  static const uint8_t DASH[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
  static const uint8_t SLASH[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
  static const uint8_t UNDERSCORE[5] = {0x40, 0x40, 0x40, 0x40, 0x40};

  static const uint8_t D0[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
  static const uint8_t D1[5] = {0x00, 0x42, 0x7F, 0x40, 0x00};
  static const uint8_t D2[5] = {0x42, 0x61, 0x51, 0x49, 0x46};
  static const uint8_t D3[5] = {0x21, 0x41, 0x45, 0x4B, 0x31};
  static const uint8_t D4[5] = {0x18, 0x14, 0x12, 0x7F, 0x10};
  static const uint8_t D5[5] = {0x27, 0x45, 0x45, 0x45, 0x39};
  static const uint8_t D6[5] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
  static const uint8_t D7[5] = {0x01, 0x71, 0x09, 0x05, 0x03};
  static const uint8_t D8[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t D9[5] = {0x06, 0x49, 0x49, 0x29, 0x1E};

  static const uint8_t A[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
  static const uint8_t B[5] = {0x7F, 0x49, 0x49, 0x49, 0x36};
  static const uint8_t C[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
  static const uint8_t D[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
  static const uint8_t E[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
  static const uint8_t F[5] = {0x7F, 0x09, 0x09, 0x09, 0x01};
  static const uint8_t G[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
  static const uint8_t H[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
  static const uint8_t I[5] = {0x00, 0x41, 0x7F, 0x41, 0x00};
  static const uint8_t J[5] = {0x20, 0x40, 0x41, 0x3F, 0x01};
  static const uint8_t K[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
  static const uint8_t L[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
  static const uint8_t M[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
  static const uint8_t N[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
  static const uint8_t O[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
  static const uint8_t P[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
  static const uint8_t Q[5] = {0x3E, 0x41, 0x51, 0x21, 0x5E};
  static const uint8_t R[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
  static const uint8_t S[5] = {0x46, 0x49, 0x49, 0x49, 0x31};
  static const uint8_t T[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
  static const uint8_t U[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
  static const uint8_t V[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F};
  static const uint8_t W[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
  static const uint8_t X[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
  static const uint8_t Y[5] = {0x07, 0x08, 0x70, 0x08, 0x07};
  static const uint8_t Z[5] = {0x61, 0x51, 0x49, 0x45, 0x43};

  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - 'a' + 'A');
  }

  switch (c) {
    case ' ': return SPACE;
    case ':': return COLON;
    case '-': return DASH;
    case '/': return SLASH;
    case '_': return UNDERSCORE;
    case '0': return D0;
    case '1': return D1;
    case '2': return D2;
    case '3': return D3;
    case '4': return D4;
    case '5': return D5;
    case '6': return D6;
    case '7': return D7;
    case '8': return D8;
    case '9': return D9;
    case 'A': return A;
    case 'B': return B;
    case 'C': return C;
    case 'D': return D;
    case 'E': return E;
    case 'F': return F;
    case 'G': return G;
    case 'H': return H;
    case 'I': return I;
    case 'J': return J;
    case 'K': return K;
    case 'L': return L;
    case 'M': return M;
    case 'N': return N;
    case 'O': return O;
    case 'P': return P;
    case 'Q': return Q;
    case 'R': return R;
    case 'S': return S;
    case 'T': return T;
    case 'U': return U;
    case 'V': return V;
    case 'W': return W;
    case 'X': return X;
    case 'Y': return Y;
    case 'Z': return Z;
    default: return SPACE;
  }
}

void oledDrawChar5x7(uint8_t x, uint8_t y, char c) {
  const uint8_t *glyph = glyphForChar(c);
  for (uint8_t col = 0; col < 5; col++) {
    for (uint8_t row = 0; row < 7; row++) {
      if ((glyph[col] & static_cast<uint8_t>(1U << row)) != 0U) {
        oledDrawPixel(static_cast<uint8_t>(x + col), static_cast<uint8_t>(y + row));
      }
    }
  }
}

void oledDrawText5x7(uint8_t x, uint8_t y, const char *text) {
  while (*text != '\0' && x < 122U) {
    oledDrawChar5x7(x, y, *text);
    x = static_cast<uint8_t>(x + 6U);
    text++;
  }
}

void oledInit() {
  pinMode(OLED_SCL_PIN, OUTPUT);
  pinMode(OLED_SDA_PIN, OUTPUT);
  oledSclSet(true);
  oledSdaSet(true);
  delay(60);

  const uint8_t initCmds[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
      0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1,
      0xDB, 0x30, 0xA4, 0xA6, 0x8D, 0x14, 0xAF};

  for (uint8_t cmd : initCmds) {
    oledWriteCommandAll(cmd);
  }

  // Power-on diagnostic: entire display ON, then back to RAM.
  oledWriteCommandAll(0xA5);
  delay(300);
  oledWriteCommandAll(0xA4);

  oledClearBuffer();
  oledRefresh();
}

bool readInputActive(uint8_t pin) {
  return digitalRead(pin) == INPUT_ACTIVE_LEVEL;
}

void writeRelay(uint8_t pin, bool on) {
#if RELAY_ACTIVE_HIGH
  digitalWrite(pin, on ? HIGH : LOW);
#else
  digitalWrite(pin, on ? LOW : HIGH);
#endif
}

void writeStatusLed(bool on) {
#if STATUS_LED_ACTIVE_LOW
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

void applyOutputs() {
  writeRelay(LIGHT_RELAY_PIN, lightOn);
  writeRelay(PUMP_RELAY_PIN, pumpOn);
  writeStatusLed(lightOn || pumpOn);
}

void setLastSource(const char *source) {
  if (source == nullptr || source[0] == '\0') {
    return;
  }
  strncpy(lastSource, source, sizeof(lastSource) - 1);
  lastSource[sizeof(lastSource) - 1] = '\0';
}

bool overallOn() {
  return lightOn || pumpOn;
}

bool espOnline() {
  return (millis() - lastEspSeenMs) <= ESP_LINK_TIMEOUT_MS;
}

void sendState(const char *source, bool forceSourceUpdate) {
  if (forceSourceUpdate) {
    setLastSource(source);
  }

  char line[128];
  snprintf(line,
           sizeof(line),
           "STATE,LIGHT=%u,PUMP=%u,MASTER_SW=%u,OVERALL=%u,SRC=%s",
           lightOn ? 1 : 0,
           pumpOn ? 1 : 0,
           masterSwitchOn ? 1 : 0,
           overallOn() ? 1 : 0,
           lastSource);
  Serial1.println(line);
  lastHeartbeatMs = millis();
}

void normalizeSource(char *dst, size_t dstSize, const char *src) {
  if (dstSize == 0) {
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && i < (dstSize - 1U)) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/')) {
      c = '-';
    }
    dst[i] = c;
    i++;
  }
  dst[i] = '\0';
}

void drawDisplay() {
  char line1[22];
  char line2[22];
  char line3[22];
  char line4[22];
  char line5[22];
  char line6[22];
  char srcShort[11];

  normalizeSource(srcShort, sizeof(srcShort), lastSource);

  snprintf(line1, sizeof(line1), "XIAOHEI AQUA");
  snprintf(line2, sizeof(line2), "LIGHT : %s", lightOn ? "ON" : "OFF");
  snprintf(line3, sizeof(line3), "PUMP  : %s", pumpOn ? "ON" : "OFF");
  snprintf(line4, sizeof(line4), "MASTER: %s", masterSwitchOn ? "ON" : "OFF");
  snprintf(line5, sizeof(line5), "NET   : %s", espOnline() ? "ONLINE" : "WAIT");
  snprintf(line6, sizeof(line6), "SRC   : %s", srcShort);

  oledClearBuffer();
  oledDrawText5x7(0, 0, line1);
  oledDrawText5x7(0, 10, line2);
  oledDrawText5x7(0, 20, line3);
  oledDrawText5x7(0, 30, line4);
  oledDrawText5x7(0, 40, line5);
  oledDrawText5x7(0, 50, line6);
  oledRefresh();
}

bool updateDebounced(DebouncedInput &input,
                     bool sample,
                     uint32_t nowMs,
                     uint32_t debounceMs) {
  if (sample != input.raw) {
    input.raw = sample;
    input.changedAtMs = nowMs;
  }

  if ((nowMs - input.changedAtMs) >= debounceMs && input.stable != input.raw) {
    input.stable = input.raw;
    return true;
  }
  return false;
}

void applyHardwareMasterSwitch(bool nextStateOn) {
  if (masterSwitchOn == nextStateOn && lightOn == nextStateOn && pumpOn == nextStateOn) {
    return;
  }
  masterSwitchOn = nextStateOn;
  lightOn = nextStateOn;
  pumpOn = nextStateOn;
  applyOutputs();
  sendState("MASTER_SWITCH", true);
}

void applyLightButtonClick() {
  if (masterSwitchOn) {
    sendState("MASTER_LOCK", true);
    return;
  }
  lightOn = !lightOn;
  applyOutputs();
  sendState("LIGHT_BUTTON", true);
}

void toUpperInline(char *text) {
  if (text == nullptr) {
    return;
  }
  while (*text != '\0') {
    if (*text >= 'a' && *text <= 'z') {
      *text = static_cast<char>(*text - 'a' + 'A');
    }
    ++text;
  }
}

void applyGroupAction(const char *action, const char *sourceTag) {
  if (masterSwitchOn && strcmp(action, "ON") != 0) {
    sendState("MASTER_LOCK", true);
    return;
  }

  if (strcmp(action, "ON") == 0) {
    lightOn = true;
    pumpOn = true;
  } else if (strcmp(action, "OFF") == 0) {
    lightOn = false;
    pumpOn = false;
  } else if (strcmp(action, "TOGGLE") == 0) {
    bool next = !(lightOn && pumpOn);
    lightOn = next;
    pumpOn = next;
  } else {
    return;
  }

  applyOutputs();
  sendState(sourceTag, true);
}

void applySingleAction(bool &target, const char *action, const char *sourceTag) {
  if (masterSwitchOn && strcmp(action, "ON") != 0) {
    sendState("MASTER_LOCK", true);
    return;
  }

  if (strcmp(action, "ON") == 0) {
    target = true;
  } else if (strcmp(action, "OFF") == 0) {
    target = false;
  } else if (strcmp(action, "TOGGLE") == 0) {
    target = !target;
  } else {
    return;
  }

  applyOutputs();
  sendState(sourceTag, true);
}

void handleCommand(char *line) {
  lastEspSeenMs = millis();

  char working[RX_BUFFER_SIZE];
  strncpy(working, line, sizeof(working) - 1);
  working[sizeof(working) - 1] = '\0';
  toUpperInline(working);

  char *ctx = nullptr;
  char *head = strtok_r(working, ",", &ctx);
  if (head == nullptr) {
    return;
  }

  if (strcmp(head, "PING") == 0) {
    Serial1.println("PONG");
    return;
  }

  if (strcmp(head, "CMD") != 0) {
    return;
  }

  char *target = strtok_r(nullptr, ",", &ctx);
  char *action = strtok_r(nullptr, ",", &ctx);
  if (target == nullptr) {
    return;
  }

  if (strcmp(target, "QUERY") == 0 || strcmp(target, "STATUS") == 0) {
    sendState("QUERY", false);
    return;
  }

  if (action == nullptr) {
    return;
  }

  if (strcmp(target, "LIGHT") == 0) {
    applySingleAction(lightOn, action, "APP_LIGHT");
    return;
  }

  if (strcmp(target, "PUMP") == 0) {
    applySingleAction(pumpOn, action, "APP_PUMP");
    return;
  }

  if (strcmp(target, "MASTER") == 0 || strcmp(target, "ALL") == 0) {
    applyGroupAction(action, "APP_MASTER");
  }
}

void pollEspSerial() {
  while (Serial1.available() > 0) {
    char ch = static_cast<char>(Serial1.read());
    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      rxBuffer[rxLength] = '\0';
      if (rxLength > 0) {
        handleCommand(rxBuffer);
      }
      rxLength = 0;
      continue;
    }

    if (rxLength < (RX_BUFFER_SIZE - 1)) {
      rxBuffer[rxLength++] = ch;
    } else {
      rxLength = 0;
    }
  }
}

}  // namespace

void setup() {
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(MASTER_SWITCH_PIN, INPUT_PULLUP);
  pinMode(LIGHT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);

  Serial.begin(SERIAL_BAUD);
  Serial1.begin(ESP_UART_BAUD);

  oledInit();

  uint32_t nowMs = millis();
  masterSwitchInput.raw = readInputActive(MASTER_SWITCH_PIN);
  masterSwitchInput.stable = masterSwitchInput.raw;
  masterSwitchInput.changedAtMs = nowMs;

  lightButtonInput.raw = readInputActive(LIGHT_BUTTON_PIN);
  lightButtonInput.stable = lightButtonInput.raw;
  lightButtonInput.changedAtMs = nowMs;

  masterSwitchOn = masterSwitchInput.stable;
  lightOn = masterSwitchOn;
  pumpOn = masterSwitchOn;
  applyOutputs();

  lastEspSeenMs = nowMs;
  sendState("BOOT", true);
  drawDisplay();
}

void loop() {
  pollEspSerial();

  uint32_t nowMs = millis();

  if (updateDebounced(masterSwitchInput, readInputActive(MASTER_SWITCH_PIN), nowMs, INPUT_DEBOUNCE_MS)) {
    applyHardwareMasterSwitch(masterSwitchInput.stable);
  }

  if (updateDebounced(lightButtonInput, readInputActive(LIGHT_BUTTON_PIN), nowMs, INPUT_DEBOUNCE_MS)) {
    if (lightButtonInput.stable) {
      applyLightButtonClick();
    }
  }

  if ((nowMs - lastHeartbeatMs) >= STATE_HEARTBEAT_MS) {
    sendState("HEARTBEAT", false);
  }

  if ((nowMs - lastDisplayMs) >= DISPLAY_REFRESH_MS) {
    lastDisplayMs = nowMs;
    drawDisplay();
  }
}
