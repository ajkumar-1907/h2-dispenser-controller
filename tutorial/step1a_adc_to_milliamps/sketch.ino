// ===== H2 dispenser controller - Step 1a: ADC counts -> volts -> mA -> MPa =====
// One 4-20 mA pressure transmitter (PT1), simulated by a knob on PA0.
// The knob gives 0..3.3 V = the voltage across a 150 ohm shunt resistor.
#define PIN_PT1 PA0

const float ADC_VREF   = 3.3f;
const float ADC_MAX    = 4095.0f;
const float SHUNT_OHMS = 150.0f;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);          // STM32 ADC is 12-bit; Arduino defaults to 10
}

void loop() {
  int   counts = analogRead(PIN_PT1);
  float volts  = counts * (ADC_VREF / ADC_MAX);
  float ma     = volts / SHUNT_OHMS * 1000.0f;     // amps -> mA
  float mpa    = (ma - 4.0f) * (100.0f / 16.0f);   // 4 mA = 0 MPa, 20 mA = 100 MPa

  Serial.print("counts=");  Serial.print(counts);
  Serial.print("  V=");     Serial.print(volts, 3);
  Serial.print("  mA=");    Serial.print(ma, 2);
  Serial.print("  MPa=");   Serial.println(mpa, 2);
  delay(500);   // fine for this first step; removed in Step 1b
}
