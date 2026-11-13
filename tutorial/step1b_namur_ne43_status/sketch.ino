// ===== H2 dispenser controller - Step 1b: NAMUR NE43 status + non-blocking timing =====
#define PIN_PT1        PA0
#define PIN_LED_FAULT  PC6

const float ADC_VREF   = 3.3f;
const float ADC_MAX    = 4095.0f;
const float SHUNT_OHMS = 150.0f;

const float PT_LO = 0.0f;      // MPa at 4 mA
const float PT_HI = 100.0f;    // MPa at 20 mA

// ---------- 1. NAMUR NE43 classifier ----------
enum SensorStatus { SNS_OK, SNS_UNDER, SNS_OVER, SNS_FAULT_OPEN, SNS_FAULT_SHORT };

SensorStatus classify(float ma) {
  // FAULT checks first: 2.0 mA is also "< 3.8", so if UNDER were
  // tested first, a cut wire would be reported as only "UNDER".
  if (ma < 3.6f)  return SNS_FAULT_OPEN;
  if (ma > 21.0f) return SNS_FAULT_SHORT;
  if (ma < 3.8f)  return SNS_UNDER;
  if (ma > 20.5f) return SNS_OVER;
  return SNS_OK;
}

const char* statusName(SensorStatus s) {
  switch (s) {
    case SNS_OK:          return "OK";
    case SNS_UNDER:       return "UNDER";
    case SNS_OVER:        return "OVER";
    case SNS_FAULT_OPEN:  return "FAULT-OPEN";
    case SNS_FAULT_SHORT: return "FAULT-SHORT";
  }
  return "?";
}

bool isFault(SensorStatus s) {
  return s == SNS_FAULT_OPEN || s == SNS_FAULT_SHORT;
}

// ---------- 2. Scaling with clamp ----------
float maToEng(float ma, float lo, float hi) {
  if (ma < 4.0f)  ma = 4.0f;      // never report below range bottom
  if (ma > 20.0f) ma = 20.0f;     // never report above range top
  return lo + (ma - 4.0f) * (hi - lo) / 16.0f;
}

// ---------- 3. Non-blocking timing ----------
const uint32_t SAMPLE_MS = 10;    // sample at 100 Hz
const uint32_t PRINT_MS  = 500;   // print at 2 Hz
uint32_t lastSample = 0;
uint32_t lastPrint  = 0;

float        ma     = 0;
float        mpa    = 0;
SensorStatus status = SNS_OK;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_LED_FAULT, OUTPUT);
  Serial.println("Lesson 1b: NE43 status");
}

void loop() {
  uint32_t now = millis();

  // ---- sample task, every 10 ms ----
  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;

    int counts = analogRead(PIN_PT1);
    float volts = counts * (ADC_VREF / ADC_MAX);
    ma     = volts / SHUNT_OHMS * 1000.0f;
    status = classify(ma);
    mpa    = maToEng(ma, PT_LO, PT_HI);

    digitalWrite(PIN_LED_FAULT, isFault(status) ? HIGH : LOW);
  }

  // ---- print task, every 500 ms ----
  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    Serial.print("PT1  mA=");  Serial.print(ma, 2);
    Serial.print("  MPa=");    Serial.print(mpa, 2);
    Serial.print("  status="); Serial.println(statusName(status));
  }
}
