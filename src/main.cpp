#include <Arduino.h>
#include <ir_Samsung.h>

const uint16_t IR_SEND_PIN = 4;
IRSamsungAc ac(IR_SEND_PIN);

void sendCommand() {
  Serial.println("Sending: Power ON, Cool, 24C, Fan Auto...");
  ac.on();
  ac.setMode(kSamsungAcCool);
  ac.setTemp(24);
  ac.setFan(kSamsungAcFanAuto);
  ac.send();
  Serial.println("Sent.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  ac.begin();
  delay(2000);
  sendCommand();
}

void loop() {
  delay(10000);
  sendCommand();
}