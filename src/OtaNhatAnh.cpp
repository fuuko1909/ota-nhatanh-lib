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

  // Neu AP rescue dang chay, GIU mode AP_STA de khong tat AP
  WiFi.mode(_apActive ? WIFI_AP_STA : WIFI_STA);
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
    if (!_apActive) {
      Serial.println("[OTA] Khong co wifi cred — tu mo AP rescue de cau hinh.");
      batConfigPortal(600);    // 10 phut
    }
    _setState(OtaState::WIFI_CONNECTING);
    _lastWifiTry = millis();
    return;
  }
  _setState(OtaState::WIFI_CONNECTING);
  _lastWifiTry = millis();
}

void OtaNhatAnh::batConfigPortal(uint16_t timeoutSec) {
  if (_apActive) return;
  String apName = _cfg._deviceId + "-recovery";
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  delay(100);
  IPAddress apIp = WiFi.softAPIP();
  Serial.print("[OTA] AP rescue: SSID=");
  Serial.print(apName);
  Serial.print("  IP=");
  Serial.println(apIp);
  Serial.print("[OTA] Mo trinh duyet vao http://");
  Serial.print(apIp);
  Serial.println(" de cau hinh wifi. Captive portal se tu redirect.");

#if defined(ESP32)
  _apWeb = new WebServer(80);
  _apDns = new DNSServer();
#elif defined(ESP8266)
  _apWeb = new ESP8266WebServer(80);
  _apDns = new DNSServer();
#endif
  _apDns->setErrorReplyCode(DNSReplyCode::NoError);
  _apDns->start(53, "*", apIp);  // capture all → portal trigger

  _apWeb->on("/", HTTP_GET, [this]() { _apHandleRoot(); });
  _apWeb->on("/save", HTTP_POST, [this]() { _apHandleSave(); });
  _apWeb->on("/scan", HTTP_GET, [this]() { _apHandleScan(); });
  // Captive portal probes (Android/iOS/Windows tự gọi)
  _apWeb->onNotFound([this]() { _apHandleRoot(); });
  _apWeb->begin();

  _apActive = true;
  _apStartMs = millis();
  _apTimeoutSec = timeoutSec;
}

void OtaNhatAnh::_apStop() {
  if (!_apActive) return;
  if (_apWeb) { _apWeb->stop(); delete _apWeb; _apWeb = nullptr; }
  if (_apDns) { _apDns->stop(); delete _apDns; _apDns = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _apActive = false;
  Serial.println("[OTA] AP rescue stopped");
}

void OtaNhatAnh::_apTick() {
  if (!_apActive) return;
  _apDns->processNextRequest();
  _apWeb->handleClient();
  // Auto-stop khi WiFi STA connect duoc
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OTA] AP rescue: STA da connect → stop AP");
    _apStop();
    return;
  }
  // Timeout
  if (_apTimeoutSec > 0 && (millis() - _apStartMs) / 1000UL > _apTimeoutSec) {
    Serial.println("[OTA] AP rescue: timeout");
    _apStop();
  }
}

String OtaNhatAnh::_apFormHtml(const String& thongBao) const {
  String h;
  h.reserve(2048);
  h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>OTA NhatAnh — Cau hinh WiFi</title>");
  h += F("<style>");
  h += F("body{font-family:-apple-system,sans-serif;background:#0a1020;color:#e6edf7;padding:20px;max-width:480px;margin:auto}");
  h += F("h1{color:#2dd4bf;font-size:1.4em;margin-bottom:6px}");
  h += F(".sub{color:#94a3b8;font-size:0.9em;margin-bottom:18px}");
  h += F("label{display:block;margin:14px 0 6px;font-size:0.85em;color:#94a3b8;text-transform:uppercase}");
  h += F("input,select{width:100%;padding:10px;background:#0f1730;border:1px solid #1f2a4a;color:#e6edf7;border-radius:6px;font-size:1em;box-sizing:border-box}");
  h += F("button{margin-top:18px;width:100%;padding:12px;background:#2dd4bf;color:#0a1020;border:0;border-radius:6px;font-size:1em;font-weight:600;cursor:pointer}");
  h += F("button.scan{background:#1f2a4a;color:#2dd4bf;margin-top:6px}");
  h += F(".msg{padding:10px;background:#2dd4bf22;border:1px solid #2dd4bf;border-radius:6px;margin-bottom:14px;color:#2dd4bf}");
  h += F(".dev{margin-top:24px;font-size:0.75em;color:#64748b;text-align:center}");
  h += F("</style></head><body>");
  h += F("<h1>OTA NhatAnh</h1>");
  h += F("<p class='sub'>Cau hinh WiFi cho thiet bi <b>");
  h += _cfg._deviceId;
  h += F("</b></p>");
  if (thongBao.length() > 0) {
    h += F("<div class='msg'>");
    h += thongBao;
    h += F("</div>");
  }
  h += F("<form method='POST' action='/save'>");
  h += F("<label>SSID</label>");
  h += F("<input name='ssid' required maxlength='32' autofocus>");
  h += F("<label>Mat khau</label>");
  h += F("<input name='pass' type='password' maxlength='64'>");
  h += F("<button type='submit'>Luu &amp; Ket noi</button>");
  h += F("</form>");
  h += F("<button class='scan' onclick=\"location='/scan'\">Quet lai mang xung quanh</button>");
  h += F("<p class='dev'>OtaNhatAnh lib v0.7.2</p>");
  h += F("</body></html>");
  return h;
}

void OtaNhatAnh::_apHandleRoot() {
  _apWeb->send(200, "text/html", _apFormHtml());
}

void OtaNhatAnh::_apHandleScan() {
  int n = WiFi.scanNetworks();
  String html;
  html.reserve(2048);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>Scan</title>");
  html += F("<style>body{font-family:sans-serif;background:#0a1020;color:#e6edf7;padding:20px;max-width:480px;margin:auto}");
  html += F("a{display:block;padding:10px;background:#0f1730;color:#2dd4bf;text-decoration:none;border-radius:6px;margin:6px 0}");
  html += F("a:hover{background:#1f2a4a}small{color:#94a3b8}</style></head><body>");
  html += F("<h2>Tim thay ");
  html += n;
  html += F(" mang</h2>");
  for (int i = 0; i < n; i++) {
    html += F("<a href='/?ssid=");
    html += WiFi.SSID(i);
    html += F("'>");
    html += WiFi.SSID(i);
    html += F(" <small>(");
    html += WiFi.RSSI(i);
    html += F(" dBm)</small></a>");
  }
  html += F("<p><a href='/'>← Quay lai</a></p></body></html>");
  _apWeb->send(200, "text/html", html);
}

void OtaNhatAnh::_apHandleSave() {
  String ssid = _apWeb->arg("ssid");
  String pass = _apWeb->arg("pass");
  if (ssid.length() == 0) {
    _apWeb->send(400, "text/html", _apFormHtml("SSID khong duoc rong"));
    return;
  }
  Serial.print("[OTA] AP rescue: nhan SSID=");
  Serial.println(ssid);
  _trySaveWifiPrefs(ssid, pass);

  String okHtml = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='5'><title>Da luu</title>"
    "<style>body{font-family:sans-serif;background:#0a1020;color:#e6edf7;text-align:center;padding:40px}"
    "h1{color:#2dd4bf}</style></head><body>"
    "<h1>Da luu</h1><p>Thiet bi se thu ket noi WiFi. Neu thanh cong, AP nay se tat.</p></body></html>");
  _apWeb->send(200, "text/html", okHtml);
  delay(500);

  // Thuc hien connect — WiFi.mode AP_STA -> begin STA
  WiFi.begin(ssid.c_str(), pass.c_str());
  // _apTick() se tu phat hien WL_CONNECTED → stop AP
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
  if (_apActive) _apTick();
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
  unsigned long backoff;
  if (_wifiRetryCount < 5) backoff = 30000UL;
  else if (_wifiRetryCount < 10) backoff = 60000UL;
  else backoff = 300000UL;   // 5 phut
  if (since > backoff) {
    _wifiRetryCount++;
    Serial.print("[OTA] WiFi retry #");
    Serial.println(_wifiRetryCount);
    // STA_ONLY khong tu mo AP. AUTO -> sau 3 fail tu mo AP rescue
    if (_cfg._wifiMode == OtaWifiMode::AUTO && _wifiRetryCount >= 3 && !_apActive) {
      Serial.println("[OTA] WiFi STA fail 3 lan → tu mo AP rescue");
      batConfigPortal(600);
    }
    // Khong WiFi.disconnect() neu AP dang chay — se phut mode AP_STA
    if (!_apActive) WiFi.disconnect();
    _startWifi();
  }
}

// AP tick cung phai chay khi state ONLINE/MQTT/... neu user goi batConfigPortal tay
// (kep them vao tickOnline va tickMqtt)

void OtaNhatAnh::_tickMqtt() {
  if (_apActive) _apTick();
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
  if (_apActive) _apTick();
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
