// ===== H2 dispenser controller - Step 2a: redundant pressure cross-check =====
#include "sensor_420.h"
#include "redundancy.h"

#define PIN_LED_FAULT  PC6
#define PIN_LED_HEART  PA5
#define PIN_BTN_RESET  PB4     // NEW: fault reset push-button (to GND)

Sensor sensors[] = {
  // tag    unit    pin  lo     hi
  { "PT1", "MPa",  PA0,   0.0f, 100.0f },
  { "PT2", "MPa",  PA1,   0.0f, 100.0f },
  { "TT1", "C",    PA4, -50.0f, 100.0f },
  { "GD1", "%LEL", PA6,   0.0f, 100.0f },
};
const int N_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

// PT1 and PT2 measure the same pipe: allow 2 MPa (2 % of span) for 500 ms
SensorPair pressure = { "P", &sensors[0], &sensors[1], 2.0f, 500 };

const uint32_t SAMPLE_MS = 10, PRINT_MS = 1000;
uint32_t lastSample = 0, lastPrint = 0;

void logEvent(uint32_t now, const char* what, const char* from, const char* to) {
  Serial.print("["); Serial.print(now); Serial.print(" ms] ");
  Serial.print(what); Serial.print(" ");
  Serial.print(from); Serial.print(" -> "); Serial.println(to);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  Serial.println("H2 dispenser - Step 2a: PT1/PT2 cross-check");
}

void loop() {
  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;

    for (int i = 0; i < N_SENSORS; i++) {
      Sensor& s = sensors[i];
      SensorStatus before = s.status;
      if (sensorUpdate(s, (uint16_t)analogRead(s.pin)))
        logEvent(now, s.tag, statusName(before), statusName(s.status));
    }

    PairState before = pressure.state;
    if (pairUpdate(pressure, now))
      logEvent(now, "PRESSURE", pairName(before), pairName(pressure.state));

    if (digitalRead(PIN_BTN_RESET) == LOW) pairReset(pressure);

    bool bad = pressure.state != PAIR_OK;
    for (int i = 2; i < N_SENSORS; i++) if (!usable(sensors[i])) bad = true;
    digitalWrite(PIN_LED_FAULT, bad ? HIGH : LOW);
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    Serial.print("PT1="); Serial.print(sensors[0].value, 1);
    Serial.print("  PT2="); Serial.print(sensors[1].value, 1);
    Serial.print("  diff="); Serial.print(pressure.diff, 1);
    Serial.print("  P_used="); Serial.print(pressure.value, 1);
    Serial.print(" MPa ("); Serial.print(pairName(pressure.state)); Serial.print(")");
    Serial.print("  TT1="); Serial.print(sensors[2].value, 1);
    Serial.print("C  GD1="); Serial.print(sensors[3].value, 1); Serial.println("%LEL");
  }
}
