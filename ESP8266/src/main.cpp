#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <WiFiManager.h>

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

#ifndef STM32_UART_BAUD
#define STM32_UART_BAUD 115200
#endif

#ifndef STM32_RX_PIN
#define STM32_RX_PIN 4
#endif

#ifndef STM32_TX_PIN
#define STM32_TX_PIN 5
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2
#endif

#ifndef DEVICE_NAME
#define DEVICE_NAME "鱼缸照明"
#endif

#ifndef MQTT_HOST
#define MQTT_HOST "broker.emqx.io"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

#ifndef MQTT_TOPIC_ROOT
#define MQTT_TOPIC_ROOT "xiaohei/fishlight/khome-20260419-a7c2"
#endif

#ifndef AP_SSID
#define AP_SSID "XiaoHei-FishLight"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "12345678"
#endif

namespace {

constexpr size_t STM32_BUFFER_SIZE = 96;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t STATE_QUERY_INTERVAL_MS = 4000;

SoftwareSerial stm32Serial(STM32_RX_PIN, STM32_TX_PIN);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WiFiManager wifiManager;

char stm32Buffer[STM32_BUFFER_SIZE];
size_t stm32Length = 0;

bool currentPower = false;
bool stateKnown = false;
String lastSource = "boot";
unsigned long lastReconnectAttemptMs = 0;
unsigned long lastStateQueryMs = 0;

String commandTopic;
String stateTopic;
String availabilityTopic;
String clientId;

void setStatusLed(bool online) {
  digitalWrite(STATUS_LED_PIN, online ? LOW : HIGH);
}

void sendToStm32(const char *line) {
  stm32Serial.print(line);
  stm32Serial.print('\n');
  Serial.print("STM32 <= ");
  Serial.println(line);
}

void publishState(const char *source) {
  if (!mqttClient.connected() || !stateKnown) {
    return;
  }

  char payload[192];
  snprintf(
      payload,
      sizeof(payload),
      "{\"deviceName\":\"%s\",\"power\":%s,\"source\":\"%s\"}",
      DEVICE_NAME,
      currentPower ? "true" : "false",
      source);

  mqttClient.publish(stateTopic.c_str(), payload, true);
}

void handleStateLine(const String &line) {
  if (line == "BOOT:READY") {
    sendToStm32("CMD:GET");
    return;
  }

  if (!line.startsWith("STATE:")) {
    Serial.print("Ignored line: ");
    Serial.println(line);
    return;
  }

  const int first = line.indexOf(':');
  const int second = line.indexOf(':', first + 1);
  if (second < 0) {
    return;
  }

  const String powerToken = line.substring(first + 1, second);
  const String sourceToken = line.substring(second + 1);

  currentPower = powerToken == "ON";
  stateKnown = true;
  lastSource = sourceToken;
  Serial.print("State updated from STM32: ");
  Serial.println(line);

  publishState(lastSource.c_str());
}

void readStm32Serial() {
  while (stm32Serial.available() > 0) {
    const char ch = static_cast<char>(stm32Serial.read());

    if (ch == '\r') {
      continue;
    }

    if (ch == '\n') {
      stm32Buffer[stm32Length] = '\0';
      if (stm32Length > 0) {
        handleStateLine(String(stm32Buffer));
      }
      stm32Length = 0;
      continue;
    }

    if (stm32Length < STM32_BUFFER_SIZE - 1) {
      stm32Buffer[stm32Length++] = ch;
    } else {
      stm32Length = 0;
    }
  }
}

void handleMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    message += static_cast<char>(payload[i]);
  }
  message.trim();
  message.toUpperCase();

  Serial.print("MQTT <= ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(message);

  if (message == "ON") {
    sendToStm32("CMD:SET:ON");
  } else if (message == "OFF") {
    sendToStm32("CMD:SET:OFF");
  } else if (message == "TOGGLE") {
    sendToStm32("CMD:TOGGLE");
  } else if (message == "QUERY") {
    sendToStm32("CMD:GET");
  }
}

bool connectMqtt() {
  if (mqttClient.connected()) {
    return true;
  }

  const bool ok = mqttClient.connect(
      clientId.c_str(),
      availabilityTopic.c_str(),
      1,
      true,
      "offline");

  if (!ok) {
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }

  mqttClient.subscribe(commandTopic.c_str(), 1);
  mqttClient.publish(availabilityTopic.c_str(), "online", true);
  if (stateKnown) {
    publishState(lastSource.c_str());
  }
  sendToStm32("CMD:GET");
  setStatusLed(true);
  Serial.println("MQTT connected.");
  return true;
}

void ensureMqttConnection() {
  if (mqttClient.connected()) {
    return;
  }

  setStatusLed(false);
  const unsigned long now = millis();
  if ((now - lastReconnectAttemptMs) < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastReconnectAttemptMs = now;
  connectMqtt();
}

void ensureStateQuery() {
  const unsigned long now = millis();
  if ((now - lastStateQueryMs) < STATE_QUERY_INTERVAL_MS) {
    return;
  }

  lastStateQueryMs = now;
  sendToStm32("CMD:GET");
}

}  // namespace

void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);

  Serial.begin(SERIAL_BAUD);
  stm32Serial.begin(STM32_UART_BAUD);

  commandTopic = String(MQTT_TOPIC_ROOT) + "/command";
  stateTopic = String(MQTT_TOPIC_ROOT) + "/state";
  availabilityTopic = String(MQTT_TOPIC_ROOT) + "/availability";
  clientId = "FishLightESP-" + String(ESP.getChipId(), HEX);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(handleMqttMessage);

  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalTimeout(180);

  const bool connected = wifiManager.autoConnect(AP_SSID, AP_PASSWORD);
  if (!connected) {
    Serial.println("WiFi config timeout, rebooting.");
    ESP.restart();
  }

  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());

  sendToStm32("CMD:GET");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setStatusLed(false);
    delay(50);
    return;
  }

  ensureMqttConnection();
  mqttClient.loop();
  readStm32Serial();

  if (!stateKnown) {
    ensureStateQuery();
  }
}
