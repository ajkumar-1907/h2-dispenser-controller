// ===== H2 dispenser controller - Step 2b: rate-of-change plausibility =====
#include "sensor_420.h"
#include "redundancy.h"
#include "rate_check.h"

#define PIN_LED_FAULT  PC6
#define PIN_LED_HEART  PA5
#define PIN_BTN_RESET  PB4

Sensor sensors[] = {
  // tag    unit    pin  lo     hi
  { "PT1", "MPa",  PA0,   0.0f, 100.0f },
  { "PT2", "MPa",  PA1,   0.0f, 100.0f },
  { "TT1", "C",    PA4, -50.0f, 100.0f },
  { "GD1", "%LEL", PA6,   0.0f, 100.0f },
};
const int N_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

SensorPair pressure = { "P", &sensors[0], &sensors[1], 2.0f, 500 };

// NEW: physical speed limits (example values)
//   pressure:    20 MPa/s   (a fill ramps well below 1 MPa/s)
//   temperature:  5 C/s     (gas temperature has thermal mass)
//   gas detector: NO rate check - a fast rise is exactly what a real leak looks like
RateCheck rates[] = {
  { &sensors[0], 20.0f, 100 },
  { &sensors[1], 20.0f, 100 },
  { &sensors[2],  5.0f, 100 },
};
const int N_RATES = sizeof(rates) / sizeof(rates[0]);

const uint32_t SAMPLE_MS = 10, PRINT_MS = 1000;
uint32_t lastSample = 0, lastPrint = 0;
bool     resetWasPressed = false;

void logTime(uint32_t now) {
  Serial.print("["); Serial.print(now); Serial.print(" ms] ");
}
void logEvent(uint32_t now, const char* what, const char* from, const char* to) {
  logTime(now);
  Serial.print(what); Serial.print(" ");
  Serial.print(from); Serial.print(" -> "); Serial.println(to);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  Serial.println("H2 dispenser - Step 2b: rate-of-change check");
}

void loop() {
  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;

    // 1. sensors (wiring faults)
    for (int i = 0; i < N_SENSORS; i++) {
      Sensor& s = sensors[i];
      SensorStatus before = s.status;
      if (sensorUpdate(s, (uint16_t)analogRead(s.pin)))
        logEvent(now, s.tag, statusName(before), statusName(s.status));
    }

    // 2. redundancy (PT1 vs PT2)
    PairState before = pressure.state;
    if (pairUpdate(pressure, now))
      logEvent(now, "PRESSURE", pairName(before), pairName(pressure.state));

    // 3. NEW: rate of change
    for (int i = 0; i < N_RATES; i++) {
      if (rateUpdate(rates[i], now)) {
        logTime(now);
        Serial.print(rates[i].s->tag); Serial.print(" IMPLAUSIBLE RATE ");
        Serial.print(rates[i].rate, 1); Serial.print(" "); Serial.print(rates[i].s->unit);
        Serial.print("/s (limit "); Serial.print(rates[i].maxRate, 1); Serial.println(")");
      }
    }

    // 4. NEW: operator reset, act on the PRESS (edge), and log it
    bool pressed = digitalRead(PIN_BTN_RESET) == LOW;
    if (pressed && !resetWasPressed) {
      logTime(now); Serial.print("RESET pressed:");
      bool any = false;
      if (pairReset(pressure)) { Serial.print(" PRESSURE-DISAGREE"); any = true; }
      for (int i = 0; i < N_RATES; i++)
        if (rateReset(rates[i])) { Serial.print(" "); Serial.print(rates[i].s->tag); Serial.print("-RATE"); any = true; }
      Serial.println(any ? " cleared" : " nothing to clear (or condition still present)");
    }
    resetWasPressed = pressed;

    // 5. fault LED
    bool bad = pressure.state != PAIR_OK;
    for (int i = 2; i < N_SENSORS; i++) if (!usable(sensors[i])) bad = true;
    for (int i = 0; i < N_RATES; i++)   if (rates[i].fault) bad = true;
    digitalWrite(PIN_LED_FAULT, bad ? HIGH : LOW);
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    Serial.print("P_used="); Serial.print(pressure.value, 1);
    Serial.print(" MPa ("); Serial.print(pairName(pressure.state)); Serial.print(")");
    Serial.print("  dP1/dt="); Serial.print(rates[0].rate, 1);
    Serial.print("  dP2/dt="); Serial.print(rates[1].rate, 1);
    Serial.print("  TT1="); Serial.print(sensors[2].value, 1);
    Serial.print("C  dT/dt="); Serial.print(rates[2].rate, 1);
    Serial.print("  GD1="); Serial.print(sensors[3].value, 1); Serial.print("%LEL");
    Serial.print(rates[0].fault || rates[1].fault || rates[2].fault ? "  [RATE FAULT]" : "");
    Serial.println();
  }
}
