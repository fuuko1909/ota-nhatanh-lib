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
  if (t.indexOf("/lenh/") >= 0) {
    _instance->_handleCommand(t, p);
  } else if (t.indexOf("/entity/") >= 0 && t.endsWith("/set")) {
    // Tach key: thiet-bi/<id>/entity/<key>/set
    int s1 = t.indexOf("/entity/") + 8;
    int s2 = t.lastIndexOf("/set");
    if (s2 > s1) {
      String key = t.substring(s1, s2);
      _instance->_handleEntitySet(key, p);
    }
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
    subscribe("entity/+/set");
    _publishAllEntityConfigs();
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

// ─────────── Live Serial stream ───────────
void OtaNhatAnh::_publishRawLine(const char* msg) {
  if (!_mqtt.connected()) return;
  // Throttle 100 tokens, refill 1/10ms (100/s)
  unsigned long now = millis();
  unsigned long delta = now - _rawTokenLast;
  if (delta >= 10) {
    uint16_t add = delta / 10;
    if (_rawTokens + add > 100) _rawTokens = 100;
    else _rawTokens += add;
    _rawTokenLast = now;
  }
  if (_rawTokens == 0) return;
  _rawTokens--;
  String topic = _topic("log/raw");
  _mqtt.publish(topic.c_str(), (const uint8_t*)msg, strlen(msg));
}

void OtaNhatAnh::rawPrint(const char *s) {
  Serial.print(s);
  // Gom vao buffer cho den \n hoac day buffer
  for (; *s; s++) {
    if (*s == '\n' || _rawIdx >= 255) {
      _rawBuf[_rawIdx] = 0;
      if (_rawIdx > 0) _publishRawLine(_rawBuf);
      _rawIdx = 0;
      continue;
    }
    if (*s == '\r') continue;
    _rawBuf[_rawIdx++] = *s;
  }
}

void OtaNhatAnh::rawPrintln(const char *s) {
  Serial.println(s);
  // Flush ngay
  int n = 0;
  while (s[n] && _rawIdx < 255) {
    if (s[n] != '\r' && s[n] != '\n') _rawBuf[_rawIdx++] = s[n];
    n++;
  }
  _rawBuf[_rawIdx] = 0;
  if (_rawIdx > 0) _publishRawLine(_rawBuf);
  _rawIdx = 0;
}

void OtaNhatAnh::rawPrintf(const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  rawPrint(buf);
}

void OtaNhatAnh::_handleCommand(const String& topic, const String& payload) {
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

// ─────────── Entity API ───────────
OtaEntity* OtaNhatAnh::_findEntity(const String& key) {
  for (auto& e : _entities) if (e.key == key) return &e;
  return nullptr;
}

void OtaNhatAnh::_publishEntityConfig(OtaEntity& e) {
  if (!_mqtt.connected()) return;
  String payload = "{\"platform\":\"" + e.platform + "\"";
  if (e.name.length()) payload += ",\"name\":\"" + e.name + "\"";
  if (e.unit.length()) payload += ",\"unit\":\"" + e.unit + "\"";
  if (e.iconOrClass.length()) payload += ",\"device_class\":\"" + e.iconOrClass + "\"";
  if (e.platform == "number") {
    payload += ",\"min\":" + String(e.numMin, 3);
    payload += ",\"max\":" + String(e.numMax, 3);
    payload += ",\"step\":" + String(e.numStep, 3);
  }
  payload += "}";
  String topic = _topic("entity/" + e.key + "/config");
  _mqtt.publish(topic.c_str(), (const uint8_t*)payload.c_str(), payload.length(), true);
  e.configPublished = true;
}

void OtaNhatAnh::_publishEntityState(OtaEntity& e, const String& value) {
  e.lastState = value;
  if (!_mqtt.connected()) return;
  String topic = _topic("entity/" + e.key + "/state");
  _mqtt.publish(topic.c_str(), (const uint8_t*)value.c_str(), value.length(), true);
}

void OtaNhatAnh::_publishAllEntityConfigs() {
  for (auto& e : _entities) {
    _publishEntityConfig(e);
    if (e.lastState.length()) _publishEntityState(e, e.lastState);
  }
}

void OtaNhatAnh::_handleEntitySet(const String& key, const String& payload) {
  OtaEntity* e = _findEntity(key);
  if (!e) return;
  String v = payload;
  v.trim();
  if (e->platform == "switch" && e->cbSwitch) {
    bool on = (v == "ON" || v == "on" || v == "true" || v == "1");
    e->cbSwitch(on);
    updateSwitch(key, on);
  } else if (e->platform == "number" && e->cbNumber) {
    float val = v.toFloat();
    e->cbNumber(val);
    updateNumber(key, val);
  } else if (e->platform == "button" && e->cbButton) {
    e->cbButton();
  } else if (e->platform == "text" && e->cbText) {
    e->cbText(v);
    updateText(key, v);
  }
}

void OtaNhatAnh::addSensor(const String& key, const String& name, const String& unit, const String& dc) {
  OtaEntity e; e.key = key; e.platform = "sensor"; e.name = name; e.unit = unit; e.iconOrClass = dc;
  _entities.push_back(e);
}

void OtaNhatAnh::addBinarySensor(const String& key, const String& name, const String& dc) {
  OtaEntity e; e.key = key; e.platform = "binary_sensor"; e.name = name; e.iconOrClass = dc;
  _entities.push_back(e);
}

void OtaNhatAnh::addSwitch(const String& key, const String& name, OtaSwitchCallback cb) {
  OtaEntity e; e.key = key; e.platform = "switch"; e.name = name; e.cbSwitch = cb;
  _entities.push_back(e);
}

void OtaNhatAnh::addNumber(const String& key, const String& name, float mn, float mx, float st, OtaNumberCallback cb) {
  OtaEntity e; e.key = key; e.platform = "number"; e.name = name;
  e.numMin = mn; e.numMax = mx; e.numStep = st; e.cbNumber = cb;
  _entities.push_back(e);
}

void OtaNhatAnh::addButton(const String& key, const String& name, OtaButtonCallback cb) {
  OtaEntity e; e.key = key; e.platform = "button"; e.name = name; e.cbButton = cb;
  _entities.push_back(e);
}

void OtaNhatAnh::addText(const String& key, const String& name) {
  OtaEntity e; e.key = key; e.platform = "text"; e.name = name;
  _entities.push_back(e);
}

void OtaNhatAnh::updateSensor(const String& k, float v)         { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, String(v,3)); }
void OtaNhatAnh::updateSensor(const String& k, const String& v) { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, v); }
void OtaNhatAnh::updateBinarySensor(const String& k, bool on)   { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, on?"ON":"OFF"); }
void OtaNhatAnh::updateSwitch(const String& k, bool on)         { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, on?"ON":"OFF"); }
void OtaNhatAnh::updateNumber(const String& k, float v)         { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, String(v,3)); }
void OtaNhatAnh::updateText(const String& k, const String& v)   { OtaEntity* e=_findEntity(k); if(e) _publishEntityState(*e, v); }

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
