#include <Arduino.h>
#include <U8g2lib.h>

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
constexpr uint32_t DISPLAY_REFRESH_MS = 200;
constexpr uint32_t STATE_HEARTBEAT_MS = 2000;
constexpr uint32_t ESP_LINK_TIMEOUT_MS = 8000;
constexpr size_t RX_BUFFER_SIZE = 128;

U8G2_SSD1306_128X64_NONAME_F_SW_I2C oled(U8G2_R0, PB10, PB11, U8X8_PIN_NONE);

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
  char line1[32];
  char line2[32];
  char line3[32];
  char line4[32];
  char line5[32];
  char line6[32];

  snprintf(line1, sizeof(line1), "XiaoHei Aquarium");
  snprintf(line2, sizeof(line2), "LIGHT : %s", lightOn ? "ON" : "OFF");
  snprintf(line3, sizeof(line3), "PUMP  : %s", pumpOn ? "ON" : "OFF");
  snprintf(line4, sizeof(line4), "MASTER: %s", masterSwitchOn ? "ON" : "OFF");
  snprintf(line5, sizeof(line5), "NET   : %s", espOnline() ? "ONLINE" : "WAIT");
  snprintf(line6, sizeof(line6), "SRC   : %s", lastSource);

  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(0, 8, line1);
  oled.drawStr(0, 18, line2);
  oled.drawStr(0, 28, line3);
  oled.drawStr(0, 38, line4);
  oled.drawStr(0, 48, line5);
  oled.drawStr(0, 58, line6);
  oled.sendBuffer();
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

  oled.begin();

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
