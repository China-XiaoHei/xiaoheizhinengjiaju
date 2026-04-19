#include <Arduino.h>
#include <cstring>

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

#ifndef RELAY_ACTIVE_HIGH
#define RELAY_ACTIVE_HIGH 1
#endif

#ifndef RELAY_PIN
#define RELAY_PIN PB12
#endif

#ifndef BUTTON_PIN
#define BUTTON_PIN PA0
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN PC13
#endif

#ifndef BUTTON_ACTIVE_LEVEL
#define BUTTON_ACTIVE_LEVEL LOW
#endif

#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 1
#endif

namespace {

constexpr size_t SERIAL_BUFFER_SIZE = 64;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

char serialBuffer[SERIAL_BUFFER_SIZE];
size_t serialLength = 0;
bool relayOn = false;
bool rawButtonState = false;
bool stableButtonState = false;
uint32_t lastButtonChangeMs = 0;

bool readButtonPressed() {
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE_LEVEL;
}

void writeRelay(bool on) {
#if RELAY_ACTIVE_HIGH
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
#else
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
#endif
}

void writeStatusLed(bool on) {
#if STATUS_LED_ACTIVE_LOW
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

void publishState(const char *source) {
  Serial.print("STATE:");
  Serial.print(relayOn ? "ON" : "OFF");
  Serial.print(':');
  Serial.println(source);
}

void applyRelayState(bool on, const char *source, bool forcePublish) {
  const bool changed = relayOn != on;
  relayOn = on;
  writeRelay(relayOn);
  writeStatusLed(relayOn);

  if (changed || forcePublish) {
    publishState(source);
  }
}

void handleProtocolLine(char *line) {
  if (strcmp(line, "CMD:SET:ON") == 0) {
    applyRelayState(true, "cloud", true);
    return;
  }

  if (strcmp(line, "CMD:SET:OFF") == 0) {
    applyRelayState(false, "cloud", true);
    return;
  }

  if (strcmp(line, "CMD:TOGGLE") == 0) {
    applyRelayState(!relayOn, "cloud", true);
    return;
  }

  if (strcmp(line, "CMD:GET") == 0) {
    publishState("sync");
    return;
  }

  Serial.print("ERR:UNKNOWN:");
  Serial.println(line);
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      serialBuffer[serialLength] = '\0';
      if (serialLength > 0) {
        handleProtocolLine(serialBuffer);
      }
      serialLength = 0;
      continue;
    }

    if (serialLength < SERIAL_BUFFER_SIZE - 1) {
      serialBuffer[serialLength++] = ch;
    } else {
      serialLength = 0;
    }
  }
}

void pollButton() {
  const bool sample = readButtonPressed();

  if (sample != rawButtonState) {
    rawButtonState = sample;
    lastButtonChangeMs = millis();
  }

  if ((millis() - lastButtonChangeMs) >= BUTTON_DEBOUNCE_MS &&
      stableButtonState != rawButtonState) {
    stableButtonState = rawButtonState;

    if (stableButtonState) {
      applyRelayState(!relayOn, "button", true);
    }
  }
}

}  // namespace

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);

  Serial.begin(SERIAL_BAUD);

  rawButtonState = readButtonPressed();
  stableButtonState = rawButtonState;
  lastButtonChangeMs = millis();

  applyRelayState(false, "boot", true);
  Serial.println("BOOT:READY");
}

void loop() {
  pollSerial();
  pollButton();
}
