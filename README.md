# OtaNhatAnh

Thư viện Arduino cho ESP32/ESP8266 kết nối **ota.nhatanh.tech**.

## Tính năng

- WiFi captive portal (lần đầu chưa biết SSID/pass → bật AP, có web config)
- MQTT TLS tới `mqtt.nhatanh.tech:8883` với username/password
- Topic chuẩn: `thiet-bi/<id>/trang-thai`, `thiet-bi/<id>/sensor/<ten>/state`, `thiet-bi/<id>/he-thong/thong-tin`
- Birth + Will message tự động
- OTA pull theo manifest URL mỗi N giờ
- Heartbeat tự động mỗi 60s

## Cài đặt PlatformIO

`platformio.ini`:

```ini
lib_deps =
  https://github.com/nhatanh-tech/ota-nhatanh-lib.git
```

## Cách dùng

```cpp
#include <Arduino.h>
#include <OtaNhatAnh.h>

OtaNhatAnh ota;

void setup() {
  Serial.begin(115200);

  ota.config()
     .deviceId("kho1-cam-bien-1")
     .mqttHost("mqtt.nhatanh.tech", 8883)
     .mqttCredentials("kho1-cam-bien-1", "mqtt-password-tu-DB")
     .otaManifest("https://ota.nhatanh.tech/fw/<token>/manifest.json")
     .checkOtaEveryHours(6);

  ota.onMqttMessage([](String topic, String payload) {
    Serial.println(topic + " = " + payload);
  });

  ota.begin();
}

void loop() {
  ota.loop();
  static unsigned long t = 0;
  if (millis() - t > 30000) {
    t = millis();
    ota.publishSensor("nhiet-do", 25.5);
  }
}
```

## License

MIT
