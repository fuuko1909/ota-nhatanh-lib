/**
 * basic.ino — vi du toi gian dung OtaNhatAnh.
 * Sua DEVICE_ID + MQTT_PASS + OTA_TOKEN theo thiet bi tao tren ota.nhatanh.tech.
 */
#include <Arduino.h>
#include <OtaNhatAnh.h>

#define DEVICE_ID  "demo-esp32"
#define MQTT_PASS  "doi-mat-khau-lay-tu-UI"
#define OTA_TOKEN  "doi-token-lay-tu-UI"

OtaNhatAnh ota;

void setup() {
  Serial.begin(115200);

  ota.config()
     .deviceId(DEVICE_ID)
     .mqttHost("mqtt.nhatanh.tech", 8883)
     .mqttCredentials(DEVICE_ID, MQTT_PASS)
     .otaManifest("https://ota.nhatanh.tech/fw/" OTA_TOKEN "/manifest.json")
     .checkOtaEveryHours(6);

  ota.onMqttMessage([](String topic, String payload) {
    Serial.println("MQTT IN " + topic + " = " + payload);
  });

  ota.begin();
}

void loop() {
  ota.loop();

  static unsigned long t = 0;
  if (millis() - t > 30000) {
    t = millis();
    float fakeTemp = 25.0 + (random(0, 100) / 10.0);
    ota.publishSensor("nhiet-do", fakeTemp);
  }
}
