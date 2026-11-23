#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#define BLYNK_AUTH_TOKEN "your-token"
const int VOLT_PIN = 34, TEMP_PIN = 35, VIB_PIN = 32;
void setup() {
  Serial.begin(115200);
  Blynk.begin(BLYNK_AUTH_TOKEN, "Wokwi-GUEST", "");
}
void loop() {
  Blynk.run();
  int volt = analogRead(VOLT_PIN);
  int temp = analogRead(TEMP_PIN);
  int vib = digitalRead(VIB_PIN);
  Blynk.virtualWrite(V0, volt);
  Blynk.virtualWrite(V1, temp);
  if (vib) Blynk.logEvent("theft", "Vibration detected!");
  delay(2000);
}
