#pragma once

#include <Arduino.h>
#include <functional>

#if defined(ESP32)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <HTTPUpdate.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecureBearSSL.h>
  #include <ESP8266httpUpdate.h>
#endif

#include <PubSubClient.h>

typedef std::function<void(String, String)> OtaMqttCallback;

class OtaNhatAnhConfig {
  friend class OtaNhatAnh;
public:
  OtaNhatAnhConfig& deviceId(const String& id);
  OtaNhatAnhConfig& mqttHost(const String& host, uint16_t port);
  OtaNhatAnhConfig& mqttCredentials(const String& user, const String& password);
  OtaNhatAnhConfig& otaManifest(const String& url);
  OtaNhatAnhConfig& checkOtaEveryHours(uint8_t hours);
  OtaNhatAnhConfig& insecureTls(bool insecure = true);  // bỏ verify cert (TEST)
  OtaNhatAnhConfig& heartbeatSeconds(uint16_t s);

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
};

class OtaNhatAnh {
public:
  OtaNhatAnh();

  OtaNhatAnhConfig& config() { return _cfg; }
  void onMqttMessage(OtaMqttCallback cb) { _onMsg = cb; }

  void begin();
  void loop();

  bool publish(const String& subTopic, const String& payload, bool retain = false);
  bool publishSensor(const String& tenSensor, float giaTri);
  bool publishSensor(const String& tenSensor, const String& giaTri);
  bool subscribe(const String& subTopic);

  bool isConnected() const;
  void checkOtaNow();

private:
  OtaNhatAnhConfig _cfg;
  WiFiClientSecure _tls;
  PubSubClient _mqtt;
  OtaMqttCallback _onMsg;
  unsigned long _lastReconnect = 0;
  unsigned long _lastHeartbeat = 0;
  unsigned long _lastOtaCheck = 0;

  String _topic(const String& sub) const;
  void _setupWifi();
  void _setupMqtt();
  bool _connectMqtt();
  void _sendBirth();
  void _sendHeartbeat();
  static void _onMqttRaw(char* topic, byte* payload, unsigned int len);
  static OtaNhatAnh* _instance;
};
