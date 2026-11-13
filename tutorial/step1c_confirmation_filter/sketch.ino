// ===== H2 dispenser controller - Step 1c: 3-sample status confirmation + low-pass filter =====
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

// ---------- 5. Non-blocking timing ----------
const uint32_t SAMPLE_MS = 10;    // sample at 100 Hz
const uint32_t PRINT_MS  = 500;   // print at 2 Hz
uint32_t lastSample = 0;
uint32_t lastPrint  = 0;

float        ma     = 0;
float        mpa    = 0;
SensorStatus status = SNS_OK;

// ---------- Lesson 1c: confirmation filter ----------
const uint8_t CONFIRM_N = 3;       // new status must repeat 3 samples = 30 ms
SensorStatus  candidate = SNS_OK;  // the status we think we are seeing
uint8_t       confirmCount = 0;
uint32_t      faultEvents  = 0;    // diagnostic counter

// returns true when the confirmed status changed
bool confirmStatus(SensorStatus raw) {
  if (raw == status) {             // same as confirmed: nothing to do
    confirmCount = 0;
    return false;
  }
  if (raw != candidate) {          // a different new status: start counting again
    candidate = raw;
    confirmCount = 0;
  }
  confirmCount++;
  if (confirmCount >= CONFIRM_N) { // seen N times in a row: accept it
    status = raw;
    confirmCount = 0;
    if (isFault(status)) faultEvents++;
    return true;
  }
  return false;
}

// ---------- Lesson 1c: low-pass filter ----------
const float ALPHA = 0.3f;          // 0..1, smaller = smoother but slower
float maFiltered = 0;
bool  firstSample = true;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_LED_FAULT, OUTPUT);
  Serial.println("Lesson 1c: confirmed status + event log");
}

void loop() {
  uint32_t now = millis();

  // ---- sample task, every 10 ms ----
  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;

    int counts = analogRead(PIN_PT1);
    float volts = counts * (ADC_VREF / ADC_MAX);
    float maRaw = volts / SHUNT_OHMS * 1000.0f;

    if (firstSample) { maFiltered = maRaw; firstSample = false; }
    maFiltered += ALPHA * (maRaw - maFiltered);   // y = y + a*(x - y)
    ma  = maFiltered;
    mpa = maToEng(ma, PT_LO, PT_HI);

    SensorStatus previous = status;
    if (confirmStatus(classify(ma))) {
      // EVENT LOG: print the moment the status changes, not 0.5 s later
      Serial.print("[");  Serial.print(now);  Serial.print(" ms] PT1 ");
      Serial.print(statusName(previous)); Serial.print(" -> ");
      Serial.print(statusName(status));
      Serial.print(" at "); Serial.print(ma, 2); Serial.print(" mA");
      Serial.print("  (fault events: "); Serial.print(faultEvents); Serial.println(")");
    }

    // ---- 4. fault LED ----
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
