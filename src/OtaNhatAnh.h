#pragma once

#include <Arduino.h>
#include <functional>

#if defined(ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <HTTPUpdate.h>
  #include <Preferences.h>
  #include <WebServer.h>
  #include <DNSServer.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecureBearSSL.h>
  #include <ESP8266httpUpdate.h>
  #include <EEPROM.h>
  #include <ESP8266WebServer.h>
  #include <DNSServer.h>
#endif

#include <PubSubClient.h>

// Macro live stream — user dùng OTA_PRINTLN("...") thay Serial.println("...")
#define OTA_PRINT(x)    ota.rawPrint((const char*)(x))
#define OTA_PRINTLN(x)  ota.rawPrintln((const char*)(x))
#define OTA_PRINTF(...) ota.rawPrintf(__VA_ARGS__)

typedef std::function<void(String, String)> OtaMqttCallback;

enum class OtaWifiMode {
  AUTO,        // STA tự retry, có hàm batConfigPortal() cho user trigger AP rescue
  STA_ONLY,    // chỉ STA, không AP rescue
  OFF,         // lib không quản WiFi, user tự kết nối
};

enum class OtaState {
  BOOT,
  WIFI_CONNECTING,
  WIFI_FAIL,
  WIFI_OK,
  MQTT_CONNECTING,
  MQTT_FAIL,
  ONLINE,
  OTA_RUNNING,
};

typedef std::function<void(OtaState, OtaState)> OtaStateCallback;

class OtaNhatAnhConfig {
  friend class OtaNhatAnh;
public:
  OtaNhatAnhConfig& deviceId(const String& id);
  OtaNhatAnhConfig& mqttHost(const String& host, uint16_t port);
  OtaNhatAnhConfig& mqttCredentials(const String& user, const String& password);
  OtaNhatAnhConfig& otaManifest(const String& url);
  OtaNhatAnhConfig& checkOtaEveryHours(uint8_t hours);
  OtaNhatAnhConfig& insecureTls(bool insecure = true);
  OtaNhatAnhConfig& heartbeatSeconds(uint16_t s);
  OtaNhatAnhConfig& wifiCredentials(const String& ssid, const String& pass);
  OtaNhatAnhConfig& wifiMode(OtaWifiMode mode);
  OtaNhatAnhConfig& otaAutoCheck(bool enable);

private:
  String _deviceId;
  String _mqttHost;
  uint16_t _mqttPort = 8883;
  String _mqttUser;
  String _mqttPass;
  String _otaManifest;
  uint8_t _otaIntervalHours = 6;
  bool _insecure = false;
  uint16_t _heartbeatSec = 60;
  String _wifiSsid;
  String _wifiPass;
  OtaWifiMode _wifiMode = OtaWifiMode::AUTO;
  bool _otaAutoCheck = false;     // default OFF, chỉ chạy on-demand
};

class OtaNhatAnh {
public:
  OtaNhatAnh();

  OtaNhatAnhConfig& config() { return _cfg; }
  void onMqttMessage(OtaMqttCallback cb) { _onMsg = cb; }
  void onStateChange(OtaStateCallback cb) { _onState = cb; }

  // Non-blocking begin: chỉ init state, return < 100ms
  void begin();
  // Non-blocking loop: return < 1ms khi không có việc
  void loop();

  // Publish (return false ngay nếu offline, không block)
  bool publish(const String& subTopic, const String& payload, bool retain = false);
  bool publishSensor(const String& tenSensor, float giaTri);
  bool publishSensor(const String& tenSensor, const String& giaTri);
  bool subscribe(const String& subTopic);

  void logDebug(const char* fmt, ...);
  void logInfo(const char* fmt, ...);
  void logWarn(const char* fmt, ...);
  void logError(const char* fmt, ...);

  // Live stream Serial qua MQTT topic log/raw
  void rawPrint(const char *s);
  void rawPrintln(const char *s);
  void rawPrintf(const char *fmt, ...);

  // State queries
  OtaState state() const { return _state; }
  bool wifiOnline() const;
  bool mqttOnline() const;
  bool isConnected() const { return mqttOnline(); }  // backward compat
  int rssi() const;
  String localIp() const;

  // Hành động
  void setWifi(const String& ssid, const String& pass, bool luuPreferences = true);
  void batConfigPortal(uint16_t timeoutSec = 300);
  void checkOtaNow();  // user trigger thủ công

private:
  OtaNhatAnhConfig _cfg;
  WiFiClientSecure _tls;
  PubSubClient _mqtt;
  OtaMqttCallback _onMsg;
  OtaStateCallback _onState;

  OtaState _state = OtaState::BOOT;
  unsigned long _stateEnterMs = 0;
  unsigned long _lastWifiTry = 0;
  unsigned long _lastMqttTry = 0;
  unsigned long _lastHeartbeat = 0;
  unsigned long _lastOtaCheck = 0;
  uint8_t _wifiRetryCount = 0;

  // Throttle log
  unsigned long _logTokenLast = 0;
  uint16_t _logTokens = 20;
  unsigned long _rawTokenLast = 0;
  uint16_t _rawTokens = 100;
  char _rawBuf[256];
  int _rawIdx = 0;
  unsigned long _lastOfflineWarn = 0;

#if defined(ESP32)
  Preferences _prefs;
  WebServer* _apWeb = nullptr;
  DNSServer* _apDns = nullptr;
#elif defined(ESP8266)
  ESP8266WebServer* _apWeb = nullptr;
  DNSServer* _apDns = nullptr;
#endif
  bool _apActive = false;
  unsigned long _apStartMs = 0;
  uint16_t _apTimeoutSec = 0;

  String _topic(const String& sub) const;
  void _setState(OtaState s);
  void _tickBoot();
  void _tickWifi();
  void _tickMqtt();
  void _tickOnline();
  void _startWifi();
  void _trySaveWifiPrefs(const String& ssid, const String& pass);
  bool _loadWifiPrefs(String& ssid, String& pass);
  bool _connectMqttNonBlocking();
  void _sendBirth();
  void _sendHeartbeat();
  void _publishLog(char level, const char* msg);
  void _publishRawLine(const char* msg);
  void _handleCommand(const String& topic, const String& payload);
  static void _onMqttRaw(char* topic, byte* payload, unsigned int len);

  // Captive portal handlers
  void _apTick();
  void _apStop();
  void _apHandleRoot();
  void _apHandleSave();
  void _apHandleScan();
  String _apFormHtml(const String& thongBao = "") const;

  static OtaNhatAnh* _instance;
};
