// ===== H2 dispenser controller - Step 1 (final): four 4-20 mA inputs =====

#define PIN_LED_FAULT  PC6
#define PIN_LED_HEART  PA5      // on-board LD4

const float ADC_VREF   = 3.3f;
const float ADC_MAX    = 4095.0f;
const float SHUNT_OHMS = 150.0f;

// ---------- Status ----------
// SNS_INIT is FIRST so it equals 0: every sensor starts "not yet proven healthy"
enum SensorStatus { SNS_INIT, SNS_OK, SNS_UNDER, SNS_OVER, SNS_FAULT_OPEN, SNS_FAULT_SHORT };

// ---------- One struct = everything about one sensor ----------
// (must come BEFORE the first function: see the note in the lesson)
struct Sensor {
  // configuration (filled in the table below)
  const char* tag;
  const char* unit;
  uint32_t    pin;
  float       lo, hi;          // value at 4 mA and at 20 mA
  // live data (start at 0 automatically)
  float        maRaw;
  float        maFilt;
  float        value;
  SensorStatus status;         // 0 = SNS_INIT
  SensorStatus candidate;
  uint8_t      count;
  uint32_t     faultEvents;
  bool         started;
};

SensorStatus classify(float ma) {
  if (ma < 3.6f)  return SNS_FAULT_OPEN;
  if (ma > 21.0f) return SNS_FAULT_SHORT;
  if (ma < 3.8f)  return SNS_UNDER;
  if (ma > 20.5f) return SNS_OVER;
  return SNS_OK;
}

const char* statusName(SensorStatus s) {
  switch (s) {
    case SNS_INIT:        return "INIT";
    case SNS_OK:          return "OK";
    case SNS_UNDER:       return "UNDER";
    case SNS_OVER:        return "OVER";
    case SNS_FAULT_OPEN:  return "FAULT-OPEN";
    case SNS_FAULT_SHORT: return "FAULT-SHORT";
  }
  return "?";
}

float maToEng(float ma, float lo, float hi) {
  if (ma < 4.0f)  ma = 4.0f;
  if (ma > 20.0f) ma = 20.0f;
  return lo + (ma - 4.0f) * (hi - lo) / 16.0f;
}

// The P&ID tag list of our dispenser
Sensor sensors[] = {
  // tag    unit    pin  lo     hi
  { "PT1", "MPa",  PA0,   0.0f, 100.0f },   // outlet pressure
  { "PT2", "MPa",  PA1,   0.0f, 100.0f },   // redundant outlet pressure
  { "TT1", "C",    PA4, -50.0f, 100.0f },   // gas temperature after pre-cooler
  { "GD1", "%LEL", PA6,   0.0f, 100.0f },   // hydrogen gas detector
};
const int N_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

const uint8_t CONFIRM_N = 3;
const float   ALPHA     = 0.3f;

// Can the safety logic use this value?
bool usable(const Sensor& s) {
  return s.status == SNS_OK || s.status == SNS_UNDER || s.status == SNS_OVER;
}

// ---------- Process one sample of one sensor ----------
void updateSensor(Sensor& s, uint32_t now) {
  int   counts = analogRead(s.pin);
  float ma     = counts * (ADC_VREF / ADC_MAX) / SHUNT_OHMS * 1000.0f;
  s.maRaw = ma;

  // VALUE path: filtered (smooth for display and control)
  if (!s.started) { s.maFilt = ma; s.started = true; }
  s.maFilt += ALPHA * (ma - s.maFilt);
  s.value   = maToEng(s.maFilt, s.lo, s.hi);

  // STATUS path: RAW current + confirmation (fast, no filter lag)
  SensorStatus raw = classify(s.maRaw);
  if (raw == s.status) { s.count = 0; return; }
  if (raw != s.candidate) { s.candidate = raw; s.count = 0; }
  if (++s.count < CONFIRM_N) return;

  SensorStatus previous = s.status;
  s.status = raw;
  s.count  = 0;
  if (!usable(s)) s.faultEvents++;

  // event log
  Serial.print("["); Serial.print(now); Serial.print(" ms] ");
  Serial.print(s.tag); Serial.print(" ");
  Serial.print(statusName(previous)); Serial.print(" -> ");
  Serial.print(statusName(s.status));
  Serial.print(" at "); Serial.print(s.maRaw, 2); Serial.print(" mA");
  Serial.print("  (faults: "); Serial.print(s.faultEvents); Serial.println(")");
}

// ---------- Timing ----------
const uint32_t SAMPLE_MS = 10;
const uint32_t PRINT_MS  = 1000;
uint32_t lastSample = 0, lastPrint = 0;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  Serial.println("H2 dispenser - Step 1: 4-20 mA inputs");
}

void loop() {
  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;
    bool anyBad = false;
    for (int i = 0; i < N_SENSORS; i++) {
      updateSensor(sensors[i], now);
      if (!usable(sensors[i])) anyBad = true;
    }
    digitalWrite(PIN_LED_FAULT, anyBad ? HIGH : LOW);
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    for (int i = 0; i < N_SENSORS; i++) {
      Sensor& s = sensors[i];
      Serial.print(s.tag);   Serial.print("=");
      Serial.print(s.value, 1); Serial.print(s.unit);
      Serial.print("("); Serial.print(statusName(s.status)); Serial.print(")  ");
    }
    Serial.println();
  }
}
