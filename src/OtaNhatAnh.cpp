#include "OtaNhatAnh.h"
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <stdarg.h>

OtaNhatAnh* OtaNhatAnh::_instance = nullptr;

// ─────────── Config builder ───────────
OtaNhatAnhConfig& OtaNhatAnhConfig::deviceId(const String& id) { _deviceId = id; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::mqttHost(const String& h, uint16_t p) { _mqttHost = h; _mqttPort = p; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::mqttCredentials(const String& u, const String& p) { _mqttUser = u; _mqttPass = p; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::otaManifest(const String& url) { _otaManifest = url; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::checkOtaEveryHours(uint8_t h) { _otaIntervalHours = h; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::insecureTls(bool i) { _insecure = i; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::heartbeatSeconds(uint16_t s) { _heartbeatSec = s; return *this; }

// ─────────── Core ───────────
OtaNhatAnh::OtaNhatAnh() : _mqtt(_tls) {
  _instance = this;
}

String OtaNhatAnh::_topic(const String& sub) const {
  return "thiet-bi/" + _cfg._deviceId + "/" + sub;
}

void OtaNhatAnh::_setupWifi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  String apName = _cfg._deviceId + "-recovery";
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println("[OTA] WiFi config timeout, restart");
    delay(2000);
    ESP.restart();
  }
  Serial.print("[OTA] WiFi OK, IP=");
  Serial.println(WiFi.localIP());
}

void OtaNhatAnh::_setupMqtt() {
  if (_cfg._insecure) {
    _tls.setInsecure();
  }
  _mqtt.setServer(_cfg._mqttHost.c_str(), _cfg._mqttPort);
  _mqtt.setBufferSize(2048);
  _mqtt.setKeepAlive(60);
  _mqtt.setCallback(_onMqttRaw);
}

void OtaNhatAnh::_onMqttRaw(char* topic, byte* payload, unsigned int len) {
  if (!_instance) return;
  String t(topic);
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
  // Handle lệnh hệ thống trước (lenh/*)
  if (t.indexOf("/lenh/") >= 0) {
    _instance->_handleCommand(t, p);
  }
  if (_instance->_onMsg) _instance->_onMsg(t, p);
}

bool OtaNhatAnh::_connectMqtt() {
  String willTopic = _topic("trang-thai");
  if (_mqtt.connect(
        _cfg._deviceId.c_str(),
        _cfg._mqttUser.c_str(),
        _cfg._mqttPass.c_str(),
        willTopic.c_str(),
        1,        // willQos
        true,     // willRetain
        "offline" // willMessage
      )) {
    Serial.println("[OTA] MQTT connected");
    _sendBirth();
    subscribe("lenh/#");
    return true;
  }
  Serial.print("[OTA] MQTT fail rc=");
  Serial.println(_mqtt.state());
  return false;
}

void OtaNhatAnh::_sendBirth() {
  publish("trang-thai", "online", true);
  _sendHeartbeat();
}

void OtaNhatAnh::_sendHeartbeat() {
  JsonDocument doc;
  doc["version"] = "0.1.0";
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = (unsigned long)(millis() / 1000);
  String out;
  serializeJson(doc, out);
  publish("he-thong/thong-tin", out, false);
  _lastHeartbeat = millis();
}

void OtaNhatAnh::begin() {
  _setupWifi();
  _setupMqtt();
  _connectMqtt();
  checkOtaNow();  // check ngay khi boot
}

bool OtaNhatAnh::isConnected() const {
  return WiFi.status() == WL_CONNECTED && const_cast<PubSubClient&>(_mqtt).connected();
}

void OtaNhatAnh::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(500);
    return;
  }
  if (!_mqtt.connected()) {
    if (millis() - _lastReconnect > 5000) {
      _lastReconnect = millis();
      _connectMqtt();
    }
  } else {
    _mqtt.loop();
    if (millis() - _lastHeartbeat > (unsigned long)_cfg._heartbeatSec * 1000UL) {
      _sendHeartbeat();
    }
  }
  unsigned long ivMs = (unsigned long)_cfg._otaIntervalHours * 3600UL * 1000UL;
  if (millis() - _lastOtaCheck > ivMs) {
    checkOtaNow();
  }
}

bool OtaNhatAnh::publish(const String& sub, const String& payload, bool retain) {
  if (!_mqtt.connected()) return false;
  return _mqtt.publish(_topic(sub).c_str(), (const uint8_t*)payload.c_str(), payload.length(), retain);
}

bool OtaNhatAnh::publishSensor(const String& tenSensor, float giaTri) {
  return publish("sensor/" + tenSensor + "/state", String(giaTri, 3));
}

bool OtaNhatAnh::publishSensor(const String& tenSensor, const String& giaTri) {
  return publish("sensor/" + tenSensor + "/state", giaTri);
}

bool OtaNhatAnh::subscribe(const String& sub) {
  return _mqtt.subscribe(_topic(sub).c_str());
}

// ─────────── Log API ───────────
void OtaNhatAnh::_publishLog(char level, const char* msg) {
  Serial.print("[");
  Serial.print(level);
  Serial.print("] ");
  Serial.println(msg);
  if (!_mqtt.connected()) return;

  // Throttle: 20 tokens, refill 1/50ms
  unsigned long now = millis();
  unsigned long delta = now - _logTokenLast;
  if (delta >= 50) {
    uint16_t add = delta / 50;
    if (_logTokens + add > 20) _logTokens = 20;
    else _logTokens += add;
    _logTokenLast = now;
  }
  if (_logTokens == 0) return;
  _logTokens--;

  // Payload JSON: {"l":"I","m":"..."}
  String payload = "{\"l\":\"";
  payload += level;
  payload += "\",\"m\":\"";
  for (const char* p = msg; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { payload += '\\'; payload += c; }
    else if (c == '\n') payload += ' ';
    else if (c >= 32) payload += c;
  }
  payload += "\"}";
  _mqtt.publish(_topic("log").c_str(), payload.c_str());
}

#define _OTA_LOG_IMPL(LVL) \
  char buf[256]; \
  va_list ap; va_start(ap, fmt); \
  vsnprintf(buf, sizeof(buf), fmt, ap); \
  va_end(ap); \
  _publishLog(LVL, buf);

void OtaNhatAnh::logDebug(const char* fmt, ...) { _OTA_LOG_IMPL('D') }
void OtaNhatAnh::logInfo(const char* fmt, ...)  { _OTA_LOG_IMPL('I') }
void OtaNhatAnh::logWarn(const char* fmt, ...)  { _OTA_LOG_IMPL('W') }
void OtaNhatAnh::logError(const char* fmt, ...) { _OTA_LOG_IMPL('E') }

void OtaNhatAnh::_handleCommand(const String& topic, const String& payload) {
  // topic dạng thiet-bi/<id>/lenh/<ten>
  int p = topic.lastIndexOf('/');
  if (p < 0) return;
  String lenh = topic.substring(p + 1);
  if (lenh == "ota") {
    logInfo("Lenh OTA - check ngay");
    checkOtaNow();
  } else if (lenh == "reboot") {
    logInfo("Lenh reboot");
    delay(500);
    ESP.restart();
  }
}

void OtaNhatAnh::checkOtaNow() {
  _lastOtaCheck = millis();
  if (_cfg._otaManifest.length() == 0) return;
  Serial.print("[OTA] Check manifest: ");
  Serial.println(_cfg._otaManifest);

  WiFiClientSecure client;
  if (_cfg._insecure) client.setInsecure();

#if defined(ESP32)
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(client, _cfg._otaManifest);
#elif defined(ESP8266)
  ESPhttpUpdate.rebootOnUpdate(true);
  HTTPUpdateResult r = ESPhttpUpdate.update(client, _cfg._otaManifest);
#endif
  switch (r) {
    case HTTP_UPDATE_FAILED:
      Serial.println("[OTA] FAILED");
      publish("ota/ket-qua", "{\"ket_qua\":\"that_bai\"}");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] no update");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] OK — restarting");
      break;
  }
}
