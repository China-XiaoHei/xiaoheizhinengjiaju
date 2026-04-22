#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <WiFiManager.h>

#include <cstring>

#ifndef STM32_UART_BAUD
#define STM32_UART_BAUD 57600
#endif

#ifndef STM32_RX_PIN
#define STM32_RX_PIN 4
#endif

#ifndef STM32_TX_PIN
#define STM32_TX_PIN 5
#endif

#ifndef STM32_USE_HW_UART
#define STM32_USE_HW_UART 1
#endif

#ifndef STM32_USE_SW_UART
#define STM32_USE_SW_UART 1
#endif

#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2
#endif

#ifndef MQTT_HOST
#define MQTT_HOST "broker-cn.emqx.io"
#endif

#ifndef MQTT_HOST_BACKUP
#define MQTT_HOST_BACKUP "broker.emqx.io"
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif

#ifndef MQTT_TOPIC_ROOT
#define MQTT_TOPIC_ROOT "xiaohei/aquarium/khome-20260422"
#endif

#ifndef AP_SSID
#define AP_SSID u8"ESP小黑"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "sunjunjie@1"
#endif

#ifndef HOME_WIFI_SSID
#define HOME_WIFI_SSID u8"春风直达拉萨"
#endif

#ifndef HOME_WIFI_PASSWORD
#define HOME_WIFI_PASSWORD "sunjunjie@1"
#endif

namespace {

constexpr size_t STM32_BUFFER_SIZE = 160;
constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
constexpr uint32_t STATE_QUERY_INTERVAL_IDLE_MS = 3000;
constexpr uint32_t STATE_QUERY_INTERVAL_KEEPALIVE_MS = 12000;
constexpr uint32_t STATUS_LED_BLINK_MS = 500;
constexpr uint32_t HOME_WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr char DEVICE_NAME[] = u8"鱼缸照明";

enum class Stm32Link : uint8_t {
  NONE = 0,
  HW_UART = 1,
  SW_UART = 2
};

SoftwareSerial stm32SwSerial(STM32_RX_PIN, STM32_TX_PIN);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
ESP8266WebServer httpServer(80);
WiFiManager wifiManager;

char stm32BufferHw[STM32_BUFFER_SIZE];
char stm32BufferSw[STM32_BUFFER_SIZE];
size_t stm32LengthHw = 0;
size_t stm32LengthSw = 0;

String topicState;
String topicAvailability;
String topicCmdWildcard;
String topicCmdLight;
String topicCmdPump;
String topicCmdMaster;
String topicCmdQuery;

unsigned long lastMqttReconnectAttemptMs = 0;
unsigned long lastStateQueryMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastStm32RxMs = 0;

bool mqttOnline = false;
bool ledPhase = false;
Stm32Link activeLink = Stm32Link::NONE;
String mqttConnectedHost;

struct DeviceState {
  bool known = false;
  bool light = false;
  bool pump = false;
  bool masterSwitch = false;
  bool overall = false;
  String source = "BOOT";
  unsigned long updatedAtMs = 0;
};

DeviceState state;

bool isStm32LinkEnabled(Stm32Link link) {
  switch (link) {
    case Stm32Link::HW_UART:
#if STM32_USE_HW_UART
      return true;
#else
      return false;
#endif
    case Stm32Link::SW_UART:
#if STM32_USE_SW_UART
      return true;
#else
      return false;
#endif
    case Stm32Link::NONE:
    default:
      return false;
  }
}

String stm32LinkName(Stm32Link link) {
  switch (link) {
    case Stm32Link::HW_UART:
      return "HW_UART";
    case Stm32Link::SW_UART:
      return "SW_UART";
    case Stm32Link::NONE:
    default:
      return "NONE";
  }
}

void setStatusLed(bool on) {
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
}

void updateLed() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if ((now - lastBlinkMs) >= STATUS_LED_BLINK_MS) {
      lastBlinkMs = now;
      ledPhase = !ledPhase;
      setStatusLed(ledPhase);
    }
    return;
  }

  if (mqttOnline) {
    setStatusLed(true);
  } else {
    unsigned long now = millis();
    if ((now - lastBlinkMs) >= STATUS_LED_BLINK_MS) {
      lastBlinkMs = now;
      ledPhase = !ledPhase;
      setStatusLed(ledPhase);
    }
  }
}

String jsonEscape(const String &input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < static_cast<size_t>(input.length()); ++i) {
    char ch = input[static_cast<unsigned int>(i)];
    if (ch == '\\' || ch == '"') {
      output += '\\';
      output += ch;
      continue;
    }
    if (ch == '\n') {
      output += "\\n";
      continue;
    }
    if (ch == '\r') {
      output += "\\r";
      continue;
    }
    if (ch == '\t') {
      output += "\\t";
      continue;
    }
    output += ch;
  }
  return output;
}

String buildStateJson() {
  String json = "{";
  json += "\"deviceName\":\"";
  json += jsonEscape(DEVICE_NAME);
  json += "\",\"light\":";
  json += state.light ? "true" : "false";
  json += ",\"pump\":";
  json += state.pump ? "true" : "false";
  json += ",\"masterSwitch\":";
  json += state.masterSwitch ? "true" : "false";
  json += ",\"overall\":";
  json += state.overall ? "true" : "false";
  json += ",\"source\":\"";
  json += jsonEscape(state.source);
  json += "\",\"known\":";
  json += state.known ? "true" : "false";
  json += ",\"ip\":\"";
  json += jsonEscape(WiFi.localIP().toString());
  json += "\",\"wifiSsid\":\"";
  json += jsonEscape(WiFi.SSID());
  json += "\",\"mqttHost\":\"";
  json += jsonEscape(mqttConnectedHost);
  json += "\",\"mqttOnline\":";
  json += mqttOnline ? "true" : "false";
  json += ",\"serialLink\":\"";
  json += stm32LinkName(activeLink);
  json += "\",\"lastStm32RxMs\":";
  json += String(lastStm32RxMs);
  json += "}";
  return json;
}

void publishStateToCloud() {
  if (!mqttClient.connected() || !state.known) {
    return;
  }
  String payload = buildStateJson();
  mqttClient.publish(topicState.c_str(), payload.c_str(), true);
}

void writeLineToLink(Stm32Link link, const String &line) {
  if (!isStm32LinkEnabled(link)) {
    return;
  }

  if (link == Stm32Link::HW_UART) {
#if STM32_USE_HW_UART
    Serial.print(line);
    Serial.print('\n');
#endif
    return;
  }

  if (link == Stm32Link::SW_UART) {
#if STM32_USE_SW_UART
    stm32SwSerial.print(line);
    stm32SwSerial.print('\n');
#endif
  }
}

void sendToStm32(const String &line) {
  if (activeLink != Stm32Link::NONE && isStm32LinkEnabled(activeLink)) {
    writeLineToLink(activeLink, line);
    return;
  }

#if STM32_USE_HW_UART
  writeLineToLink(Stm32Link::HW_UART, line);
#endif
#if STM32_USE_SW_UART
  writeLineToLink(Stm32Link::SW_UART, line);
#endif
}

void sendQueryToStm32() {
  sendToStm32("CMD,QUERY");
}

void applyStateToken(const String &token) {
  if (token.startsWith("LIGHT=")) {
    state.light = token.substring(6).toInt() == 1;
    return;
  }
  if (token.startsWith("PUMP=")) {
    state.pump = token.substring(5).toInt() == 1;
    return;
  }
  if (token.startsWith("MASTER_SW=")) {
    state.masterSwitch = token.substring(10).toInt() == 1;
    return;
  }
  if (token.startsWith("OVERALL=")) {
    state.overall = token.substring(8).toInt() == 1;
    return;
  }
  if (token.startsWith("SRC=")) {
    state.source = token.substring(4);
  }
}

void markStm32LinkAlive(Stm32Link source) {
  lastStm32RxMs = millis();
  if (isStm32LinkEnabled(source)) {
    activeLink = source;
  }
}

void handleStateLine(const String &line, Stm32Link source) {
  if (line == "PONG") {
    markStm32LinkAlive(source);
    return;
  }

  if (!line.startsWith("STATE,")) {
    return;
  }

  markStm32LinkAlive(source);
  state.known = true;
  state.updatedAtMs = millis();
  state.overall = false;

  size_t begin = 6;
  while (begin < static_cast<size_t>(line.length())) {
    int comma = line.indexOf(',', static_cast<int>(begin));
    if (comma < 0) {
      comma = line.length();
    }

    String token = line.substring(static_cast<unsigned int>(begin), static_cast<unsigned int>(comma));
    token.trim();
    applyStateToken(token);
    begin = static_cast<size_t>(comma + 1);
  }

  if (!state.overall) {
    state.overall = state.light || state.pump;
  }

  publishStateToCloud();
}

void processStm32Byte(char ch, char *buffer, size_t &length, Stm32Link source) {
  if (ch == '\r') {
    return;
  }

  if (ch == '\n') {
    buffer[length] = '\0';
    if (length > 0) {
      handleStateLine(String(buffer), source);
    }
    length = 0;
    return;
  }

  if (length < (STM32_BUFFER_SIZE - 1)) {
    buffer[length++] = ch;
  } else {
    length = 0;
  }
}

void readStm32Serial() {
#if STM32_USE_HW_UART
  while (Serial.available() > 0) {
    char ch = static_cast<char>(Serial.read());
    processStm32Byte(ch, stm32BufferHw, stm32LengthHw, Stm32Link::HW_UART);
  }
#endif

#if STM32_USE_SW_UART
  while (stm32SwSerial.available() > 0) {
    char ch = static_cast<char>(stm32SwSerial.read());
    processStm32Byte(ch, stm32BufferSw, stm32LengthSw, Stm32Link::SW_UART);
  }
#endif
}

String parseActionPayload(const uint8_t *payload, unsigned int length) {
  String action;
  action.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    action += static_cast<char>(payload[i]);
  }
  action.trim();
  action.toUpperCase();
  return action;
}

bool normalizeAction(String &action) {
  return action == "ON" || action == "OFF" || action == "TOGGLE";
}

void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  String topicText = topic;
  String action = parseActionPayload(payload, length);

  if (topicText == topicCmdQuery) {
    sendQueryToStm32();
    return;
  }

  if (!normalizeAction(action)) {
    return;
  }

  if (topicText == topicCmdLight) {
    sendToStm32("CMD,LIGHT," + action);
    return;
  }

  if (topicText == topicCmdPump) {
    sendToStm32("CMD,PUMP," + action);
    return;
  }

  if (topicText == topicCmdMaster) {
    sendToStm32("CMD,MASTER," + action);
  }
}

bool connectMqttOnHost(const char *host) {
  mqttClient.setServer(host, MQTT_PORT);
  String clientId = "XiaoHeiESP-" + String(ESP.getChipId(), HEX);
  bool ok = mqttClient.connect(clientId.c_str(), topicAvailability.c_str(), 1, true, "offline");
  if (!ok) {
    return false;
  }

  mqttClient.subscribe(topicCmdWildcard.c_str(), 1);
  mqttClient.publish(topicAvailability.c_str(), "online", true);
  mqttConnectedHost = host;
  mqttOnline = true;
  publishStateToCloud();
  sendQueryToStm32();
  return true;
}

bool connectMqtt() {
  if (mqttClient.connected()) {
    mqttOnline = true;
    return true;
  }

  mqttOnline = false;
  if (connectMqttOnHost(MQTT_HOST)) {
    return true;
  }

  if (strcmp(MQTT_HOST, MQTT_HOST_BACKUP) != 0 && connectMqttOnHost(MQTT_HOST_BACKUP)) {
    return true;
  }

  return false;
}

void ensureMqttConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    mqttOnline = false;
    mqttConnectedHost = "";
    return;
  }

  if (mqttClient.connected()) {
    mqttOnline = true;
    return;
  }

  unsigned long now = millis();
  if ((now - lastMqttReconnectAttemptMs) < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastMqttReconnectAttemptMs = now;
  connectMqtt();
}

void sendJson(int statusCode, const String &payload) {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  httpServer.send(statusCode, "application/json; charset=utf-8", payload);
}

void handleApiState() {
  sendJson(200, buildStateJson());
}

void handleApiControl() {
  String target = httpServer.arg("target");
  String action = httpServer.arg("action");

  target.trim();
  action.trim();
  target.toUpperCase();
  action.toUpperCase();

  if (target.length() == 0) {
    sendJson(400, "{\"ok\":false,\"error\":\"target required\"}");
    return;
  }

  if (target == "QUERY") {
    sendQueryToStm32();
    sendJson(200, "{\"ok\":true,\"message\":\"query sent\"}");
    return;
  }

  if (!normalizeAction(action)) {
    sendJson(400, "{\"ok\":false,\"error\":\"action must be ON/OFF/TOGGLE\"}");
    return;
  }

  if (target == "LIGHT") {
    sendToStm32("CMD,LIGHT," + action);
  } else if (target == "PUMP") {
    sendToStm32("CMD,PUMP," + action);
  } else if (target == "MASTER" || target == "ALL") {
    sendToStm32("CMD,MASTER," + action);
  } else {
    sendJson(400, "{\"ok\":false,\"error\":\"target must be LIGHT/PUMP/MASTER/QUERY\"}");
    return;
  }

  String payload = String("{\"ok\":true,\"target\":\"") + target + "\",\"action\":\"" + action + "\"}";
  sendJson(200, payload);
}

void handleApiOptions() {
  sendJson(204, "");
}

void setupHttpServer() {
  httpServer.on("/api/state", HTTP_GET, handleApiState);
  httpServer.on("/api/control", HTTP_GET, handleApiControl);
  httpServer.on("/api/control", HTTP_POST, handleApiControl);
  httpServer.on("/api/state", HTTP_OPTIONS, handleApiOptions);
  httpServer.on("/api/control", HTTP_OPTIONS, handleApiOptions);
  httpServer.on("/", HTTP_GET, []() {
    String html = "<html><body><h3>XiaoHei Aquarium ESP</h3>"
                  "<p>GET /api/state</p>"
                  "<p>GET /api/control?target=LIGHT&action=ON</p></body></html>";
    httpServer.send(200, "text/html; charset=utf-8", html);
  });
  httpServer.begin();
}

void ensureStateSync() {
  unsigned long now = millis();
  uint32_t interval = state.known ? STATE_QUERY_INTERVAL_KEEPALIVE_MS : STATE_QUERY_INTERVAL_IDLE_MS;
  if ((now - lastStateQueryMs) >= interval) {
    lastStateQueryMs = now;
    sendQueryToStm32();
  }
}

bool connectHomeWifiFirst() {
  if (strlen(HOME_WIFI_SSID) == 0) {
    return false;
  }

  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < HOME_WIFI_CONNECT_TIMEOUT_MS) {
    delay(300);
  }
  return WiFi.status() == WL_CONNECTED;
}

}  // namespace

void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);

#if STM32_USE_HW_UART
  Serial.begin(STM32_UART_BAUD);
#endif

#if STM32_USE_SW_UART
  stm32SwSerial.begin(STM32_UART_BAUD);
#endif

  topicState = String(MQTT_TOPIC_ROOT) + "/state";
  topicAvailability = String(MQTT_TOPIC_ROOT) + "/availability";
  topicCmdWildcard = String(MQTT_TOPIC_ROOT) + "/cmd/#";
  topicCmdLight = String(MQTT_TOPIC_ROOT) + "/cmd/light";
  topicCmdPump = String(MQTT_TOPIC_ROOT) + "/cmd/pump";
  topicCmdMaster = String(MQTT_TOPIC_ROOT) + "/cmd/master";
  topicCmdQuery = String(MQTT_TOPIC_ROOT) + "/cmd/query";

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(384);

  WiFi.mode(WIFI_STA);
  bool homeWifiOk = connectHomeWifiFirst();

  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConfigPortalBlocking(true);
  if (!homeWifiOk) {
    bool wifiOk = wifiManager.autoConnect(AP_SSID, AP_PASSWORD);
    if (!wifiOk) {
      delay(1000);
      ESP.restart();
    }
  }

  setupHttpServer();
  sendQueryToStm32();
}

void loop() {
  readStm32Serial();
  httpServer.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    ensureMqttConnection();
    mqttClient.loop();
    ensureStateSync();
  } else {
    mqttOnline = false;
    mqttConnectedHost = "";
  }

  updateLed();
}
