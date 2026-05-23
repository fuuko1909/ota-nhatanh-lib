#include "OtaNhatAnh.h"
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
OtaNhatAnhConfig& OtaNhatAnhConfig::wifiCredentials(const String& s, const String& p) { _wifiSsid = s; _wifiPass = p; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::wifiMode(OtaWifiMode m) { _wifiMode = m; return *this; }
OtaNhatAnhConfig& OtaNhatAnhConfig::otaAutoCheck(bool e) { _otaAutoCheck = e; return *this; }

// ─────────── Core ───────────
OtaNhatAnh::OtaNhatAnh() : _mqtt(_tls) {
  _instance = this;
}

String OtaNhatAnh::_topic(const String& sub) const {
  return "thiet-bi/" + _cfg._deviceId + "/" + sub;
}

void OtaNhatAnh::_setState(OtaState s) {
  if (s == _state) return;
  OtaState old = _state;
  _state = s;
  _stateEnterMs = millis();
  if (_onState) _onState(old, s);
}

// ─────────── WiFi non-blocking ───────────
void OtaNhatAnh::_trySaveWifiPrefs(const String& ssid, const String& pass) {
#if defined(ESP32)
  if (_prefs.begin("ota_wifi", false)) {
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
  }
#endif
}

bool OtaNhatAnh::_loadWifiPrefs(String& ssid, String& pass) {
#if defined(ESP32)
  if (_prefs.begin("ota_wifi", true)) {
    ssid = _prefs.getString("ssid", "");
    pass = _prefs.getString("pass", "");
    _prefs.end();
    return ssid.length() > 0;
  }
#endif
  return false;
}

void OtaNhatAnh::setWifi(const String& ssid, const String& pass, bool luuPreferences) {
  if (luuPreferences) _trySaveWifiPrefs(ssid, pass);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  _setState(OtaState::WIFI_CONNECTING);
  _lastWifiTry = millis();
}

void OtaNhatAnh::_startWifi() {
  if (_cfg._wifiMode == OtaWifiMode::OFF) {
    // User tự lo wifi → skip ngay sang state online tracker
    _setState(WiFi.status() == WL_CONNECTED ? OtaState::WIFI_OK : OtaState::WIFI_CONNECTING);
    _lastWifiTry = millis();
    return;
  }

  String ssid = _cfg._wifiSsid;
  String pass = _cfg._wifiPass;
  if (ssid.length() == 0) {
    // Thử load từ Preferences (lib lưu)
    _loadWifiPrefs(ssid, pass);
  } else {
    // User config từ code → lưu vào prefs cho lần boot sau
    _trySaveWifiPrefs(ssid, pass);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (ssid.length() > 0) {
    Serial.print("[OTA] WiFi.begin ssid=");
    Serial.println(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    // Fallback: thu WiFi.begin() khong tham so — Arduino core ESP32/ESP8266
    // luu cred trong NVS namespace 'nvs.net80211' (khi WiFi.persistent=true tu lan truoc)
    // Cred nay co the do WiFiManager/Arduino core ghi tu lib version cu.
#if defined(ESP32)
    String coreSsid = WiFi.SSID();
    if (coreSsid.length() > 0) {
      Serial.print("[OTA] WiFi.begin() — dung cred Arduino core lan truoc, ssid=");
      Serial.println(coreSsid);
      WiFi.begin();
      _setState(OtaState::WIFI_CONNECTING);
      _lastWifiTry = millis();
      return;
    }
#elif defined(ESP8266)
    if (WiFi.SSID().length() > 0) {
      Serial.print("[OTA] WiFi.begin() — dung cred Arduino core lan truoc, ssid=");
      Serial.println(WiFi.SSID());
      WiFi.begin();
      _setState(OtaState::WIFI_CONNECTING);
      _lastWifiTry = millis();
      return;
    }
#endif
    Serial.println("[OTA] Khong co wifi cred — chay offline. Goi ota.setWifi(ssid, pass) hoac ota.batConfigPortal().");
  }
  _setState(OtaState::WIFI_CONNECTING);
  _lastWifiTry = millis();
}

void OtaNhatAnh::batConfigPortal(uint16_t timeoutSec) {
#if defined(ESP32)
  // AP mode đơn giản: SSID = <deviceId>-recovery, không pass
  // Chạy non-blocking trong loop bằng cách switch mode, user tự config qua web
  String apName = _cfg._deviceId + "-recovery";
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  Serial.print("[OTA] AP rescue: ");
  Serial.print(apName);
  Serial.print(" IP=");
  Serial.println(WiFi.softAPIP());
  Serial.print("[OTA] Goi ota.setWifi(ssid, pass) tu code khac, hoac dung mDNS/web portal rieng. Timeout=");
  Serial.print(timeoutSec);
  Serial.println("s");
  // Không tự cài web server — user implement riêng nếu cần
#endif
}

// ─────────── MQTT non-blocking ───────────
bool OtaNhatAnh::_connectMqttNonBlocking() {
  if (_cfg._insecure) _tls.setInsecure();
  _mqtt.setServer(_cfg._mqttHost.c_str(), _cfg._mqttPort);
  _mqtt.setBufferSize(2048);
  _mqtt.setKeepAlive(60);
  _mqtt.setSocketTimeout(2);   // 2s timeout thay vì 15s default
  _mqtt.setCallback(_onMqttRaw);

  String willTopic = _topic("trang-thai");
  bool ok = _mqtt.connect(
    _cfg._deviceId.c_str(),
    _cfg._mqttUser.c_str(),
    _cfg._mqttPass.c_str(),
    willTopic.c_str(),
    1, true, "offline"
  );
  if (ok) {
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
  if (!_mqtt.connected()) return;
  JsonDocument doc;
  // compile_time thay version cứng để backend không ghi đè version_dang_chay sai
  doc["compile_time"] = String(__DATE__) + " " + String(__TIME__);
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = (unsigned long)(millis() / 1000);
  String out;
  serializeJson(doc, out);
  publish("he-thong/thong-tin", out, false);
  _lastHeartbeat = millis();
}

// ─────────── Lifecycle ───────────
void OtaNhatAnh::begin() {
  // Non-blocking: chỉ init state machine + start wifi background
  _startWifi();
}

bool OtaNhatAnh::wifiOnline() const { return WiFi.status() == WL_CONNECTED; }
bool OtaNhatAnh::mqttOnline() const { return wifiOnline() && const_cast<PubSubClient&>(_mqtt).connected(); }
int OtaNhatAnh::rssi() const { return wifiOnline() ? WiFi.RSSI() : 0; }
String OtaNhatAnh::localIp() const { return wifiOnline() ? WiFi.localIP().toString() : String(""); }

void OtaNhatAnh::_tickWifi() {
  if (_cfg._wifiMode == OtaWifiMode::OFF) {
    if (WiFi.status() == WL_CONNECTED) _setState(OtaState::WIFI_OK);
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    _setState(OtaState::WIFI_OK);
    _wifiRetryCount = 0;
    return;
  }
  // Đợi 30s mỗi lần connect, sau đó retry với backoff
  unsigned long since = millis() - _lastWifiTry;
  unsigned long backoff = (_wifiRetryCount < 5) ? 30000UL : 60000UL;
  if (since > backoff) {
    _wifiRetryCount++;
    Serial.print("[OTA] WiFi retry #");
    Serial.println(_wifiRetryCount);
    WiFi.disconnect();
    _startWifi();
  }
}

void OtaNhatAnh::_tickMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    _setState(OtaState::WIFI_CONNECTING);
    return;
  }
  if (_mqtt.connected()) {
    _setState(OtaState::ONLINE);
    return;
  }
  // Retry MQTT mỗi 5s
  unsigned long since = millis() - _lastMqttTry;
  if (since > 5000) {
    _lastMqttTry = millis();
    if (_connectMqttNonBlocking()) {
      _setState(OtaState::ONLINE);
    }
  }
}

void OtaNhatAnh::_tickOnline() {
  if (WiFi.status() != WL_CONNECTED) {
    _setState(OtaState::WIFI_CONNECTING);
    return;
  }
  if (!_mqtt.connected()) {
    _setState(OtaState::MQTT_CONNECTING);
    return;
  }
  _mqtt.loop();
  if (millis() - _lastHeartbeat > (unsigned long)_cfg._heartbeatSec * 1000UL) {
    _sendHeartbeat();
  }
  if (_cfg._otaAutoCheck) {
    unsigned long iv = (unsigned long)_cfg._otaIntervalHours * 3600UL * 1000UL;
    if (millis() - _lastOtaCheck > iv) {
      checkOtaNow();
    }
  }
}

void OtaNhatAnh::loop() {
  switch (_state) {
    case OtaState::BOOT:
    case OtaState::WIFI_CONNECTING:
    case OtaState::WIFI_FAIL:
      _tickWifi();
      break;
    case OtaState::WIFI_OK:
    case OtaState::MQTT_CONNECTING:
    case OtaState::MQTT_FAIL:
      _tickMqtt();
      break;
    case OtaState::ONLINE:
      _tickOnline();
      break;
    case OtaState::OTA_RUNNING:
      // Không tick gì — chờ httpUpdate xong (sẽ reboot)
      break;
  }
}

bool OtaNhatAnh::publish(const String& sub, const String& payload, bool retain) {
  if (!mqttOnline()) {
    unsigned long now = millis();
    if (now - _lastOfflineWarn > 30000) {
      _lastOfflineWarn = now;
      Serial.println("[OTA] offline — skip pub");
    }
    return false;
  }
  return _mqtt.publish(_topic(sub).c_str(), (const uint8_t*)payload.c_str(), payload.length(), retain);
}

bool OtaNhatAnh::publishSensor(const String& tenSensor, float giaTri) {
  return publish("sensor/" + tenSensor + "/state", String(giaTri, 3));
}

bool OtaNhatAnh::publishSensor(const String& tenSensor, const String& giaTri) {
  return publish("sensor/" + tenSensor + "/state", giaTri);
}

bool OtaNhatAnh::subscribe(const String& sub) {
  if (!mqttOnline()) return false;
  return _mqtt.subscribe(_topic(sub).c_str());
}

void OtaNhatAnh::_onMqttRaw(char* topic, byte* payload, unsigned int len) {
  if (!_instance) return;
  String t(topic);
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
  if (t.indexOf("/lenh/") >= 0) {
    _instance->_handleCommand(t, p);
  }
  if (_instance->_onMsg) _instance->_onMsg(t, p);
}

// ─────────── Log API ───────────
void OtaNhatAnh::_publishLog(char level, const char* msg) {
  Serial.print("[");
  Serial.print(level);
  Serial.print("] ");
  Serial.println(msg);
  if (!mqttOnline()) return;

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
  if (!mqttOnline()) return;
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

void OtaNhatAnh::checkOtaNow() {
  _lastOtaCheck = millis();
  if (_cfg._otaManifest.length() == 0) return;
  if (!wifiOnline()) {
    Serial.println("[OTA] Skip checkOtaNow — wifi offline");
    return;
  }
  _setState(OtaState::OTA_RUNNING);
  // Báo backend đã bắt đầu (best-effort, không block nếu mqtt down)
  publish("ota/ket-qua", "{\"ket_qua\":\"bat_dau\"}");

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
      // ESP đã reboot — không tới đây
      break;
  }
  _setState(OtaState::ONLINE);
}
