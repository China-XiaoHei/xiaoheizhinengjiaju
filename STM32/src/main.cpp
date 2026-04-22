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
constexpr uint32_t BUTTON_SHORT_PRESS_MIN_MS = 20;
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
bool masterSwitchPressed = false;
uint32_t masterSwitchPressStartMs = 0;
bool lightButtonPressed = false;
uint32_t lightButtonPressStartMs = 0;

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

static const uint8_t HZ_YU_16X16[32] = {
    0x03, 0xF0, 0x04, 0x20, 0x08, 0x40, 0x1F, 0xF8, 0x28, 0x88, 0x08, 0x88, 0x0F, 0xF8, 0x08, 0x88,
    0x08, 0x88, 0x0F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_GANG_16X16[32] = {
    0x08, 0x00, 0x0F, 0x7C, 0x14, 0x10, 0x24, 0x10, 0x04, 0x10, 0x3F, 0x90, 0x04, 0x10, 0x04, 0x10,
    0x15, 0x10, 0x15, 0x10, 0x15, 0x10, 0x17, 0x10, 0x19, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_ZHAO_16X16[32] = {
    0x12, 0x44, 0x12, 0x44, 0x12, 0x44, 0x1E, 0x98, 0x12, 0x00, 0x12, 0xFC, 0x12, 0x84, 0x12, 0x84,
    0x1E, 0xFC, 0x00, 0x00, 0x12, 0x44, 0x11, 0x22, 0x21, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_MING_16X16[32] = {
    0x3E, 0x42, 0x22, 0x42, 0x22, 0x42, 0x22, 0x7E, 0x3E, 0x42, 0x22, 0x42, 0x22, 0x42, 0x22, 0x7E,
    0x3E, 0x42, 0x20, 0x82, 0x00, 0x82, 0x01, 0x02, 0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_BENG_16X16[32] = {
    0x3F, 0xFE, 0x02, 0x00, 0x07, 0xF8, 0x0C, 0x08, 0x14, 0x08, 0x27, 0xF8, 0x00, 0x00, 0x00, 0x84,
    0x1E, 0xD8, 0x02, 0xA0, 0x04, 0x90, 0x08, 0x88, 0x33, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_ZONG_16X16[32] = {
    0x04, 0x10, 0x02, 0x20, 0x0F, 0xF8, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0F, 0xF8, 0x00, 0x00,
    0x00, 0x80, 0x04, 0x44, 0x14, 0x42, 0x14, 0x12, 0x23, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_KAI_16X16[32] = {
    0x04, 0x10, 0x04, 0x10, 0x04, 0x10, 0x04, 0x10, 0x04, 0x10, 0x3F, 0xFE, 0x04, 0x10, 0x04, 0x10,
    0x04, 0x10, 0x08, 0x10, 0x08, 0x10, 0x10, 0x10, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t HZ_GUAN_16X16[32] = {
    0x02, 0x10, 0x02, 0x20, 0x1F, 0xFC, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x3F, 0xFE, 0x01, 0x40,
    0x01, 0x40, 0x02, 0x20, 0x04, 0x10, 0x08, 0x08, 0x30, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void oledDrawHz16(uint8_t x, uint8_t y, const uint8_t *glyph) {
  for (uint8_t row = 0; row < 16; row++) {
    uint8_t b1 = glyph[row * 2];
    uint8_t b2 = glyph[row * 2 + 1];

    for (uint8_t col = 0; col < 8; col++) {
      if ((b1 & static_cast<uint8_t>(0x80U >> col)) != 0U) {
        oledDrawPixel(static_cast<uint8_t>(x + col), static_cast<uint8_t>(y + row));
      }
    }
    for (uint8_t col = 0; col < 8; col++) {
      if ((b2 & static_cast<uint8_t>(0x80U >> col)) != 0U) {
        oledDrawPixel(static_cast<uint8_t>(x + 8U + col), static_cast<uint8_t>(y + row));
      }
    }
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

void drawDisplay() {
  oledClearBuffer();

  // 第一行: 鱼缸照明 + 开关状态
  oledDrawHz16(0, 0, HZ_YU_16X16);
  oledDrawHz16(16, 0, HZ_GANG_16X16);
  oledDrawHz16(32, 0, HZ_ZHAO_16X16);
  oledDrawHz16(48, 0, HZ_MING_16X16);
  oledDrawHz16(96, 0, lightOn ? HZ_KAI_16X16 : HZ_GUAN_16X16);

  // 第二行: 鱼泵 + 开关状态
  oledDrawHz16(0, 24, HZ_YU_16X16);
  oledDrawHz16(16, 24, HZ_BENG_16X16);
  oledDrawHz16(96, 24, pumpOn ? HZ_KAI_16X16 : HZ_GUAN_16X16);

  // 第三行: 总开关 + 开关状态
  oledDrawHz16(0, 48, HZ_ZONG_16X16);
  oledDrawHz16(16, 48, HZ_KAI_16X16);
  oledDrawHz16(32, 48, HZ_GUAN_16X16);
  oledDrawHz16(96, 48, masterSwitchOn ? HZ_KAI_16X16 : HZ_GUAN_16X16);

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

void applyMasterSwitchClick() {
  masterSwitchOn = !masterSwitchOn;
  lightOn = masterSwitchOn;
  pumpOn = masterSwitchOn;
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

  masterSwitchOn = false;
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
    if (masterSwitchInput.stable) {
      masterSwitchPressed = true;
      masterSwitchPressStartMs = nowMs;
    } else if (masterSwitchPressed) {
      uint32_t pressDurationMs = nowMs - masterSwitchPressStartMs;
      if (pressDurationMs >= BUTTON_SHORT_PRESS_MIN_MS) {
        applyMasterSwitchClick();
      }
      masterSwitchPressed = false;
    }
  }

  if (updateDebounced(lightButtonInput, readInputActive(LIGHT_BUTTON_PIN), nowMs, INPUT_DEBOUNCE_MS)) {
    if (lightButtonInput.stable) {
      lightButtonPressed = true;
      lightButtonPressStartMs = nowMs;
    } else if (lightButtonPressed) {
      uint32_t pressDurationMs = nowMs - lightButtonPressStartMs;
      if (pressDurationMs >= BUTTON_SHORT_PRESS_MIN_MS) {
        applyLightButtonClick();
      }
      lightButtonPressed = false;
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
