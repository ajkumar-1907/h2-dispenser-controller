// ===== H2 dispenser - Step 6: ESP32 HMI (operator display + remote STOP/ESD) =====
//
// The ESP32 is the MASTER on the link: it asks, the STM32 answers.
// It shows the station on a 20x4 LCD and lets the operator stop the fill.
// It NEVER makes anything less safe: no remote START, no remote RESET.
//
// Same protocol as Step 5 (link.h) and the same fault list (faults.h):
// one source of truth for both boards.
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "link.h"
#include "faults.h"

#define USE_SIM_CONTROLLER 1      // 1 = Wokwi (simulated STM32 inside), 0 = real STM32 on UART2
#if USE_SIM_CONTROLLER
#include "sim_controller.h"
SimController sim;
Pipe toCtrl, toHmi;               // the two directions of the "cable"
#endif

// ---- pins ----
#define PIN_BTN_PAGE    18        // HMI buttons (to GND)
#define PIN_BTN_STOP    19
#define PIN_BTN_ESD     23        // big red: remote emergency shutdown
#define PIN_BTN_NEXTCAR 5         // dedicated "next car" (fixes the Step 4 two-meaning STOP button)
#define PIN_LED_ALARM   26
#define PIN_SIM_START   4         // simulated LOCAL buttons on the dispenser (Wokwi only)
#define PIN_SIM_RESET   15
#define PIN_SIM_FAULT   13        // slide switch: inject GAS_WARN + P_DISAGREE
#define PIN_SIM_LINKCUT 14        // slide switch: cut the cable

LiquidCrystal_I2C lcd(0x27, 20, 4);

// ---------- link timing ----------
const uint32_t POLL_MS          = 500;   // ask for status twice a second (also our heartbeat)
const uint32_t REPLY_TIMEOUT_MS = 250;   // an answer must come within 250 ms
const uint8_t  LOST_AFTER       = 3;     // 3 missed answers in a row = LINK LOST

// ---------- types first (Arduino inserts function prototypes above the first function) ----------
struct Btn { uint8_t pin; bool last; };

// ---------- what the HMI knows about the station ----------
struct Station {
  char     state[10], phase[8];
  int32_t  p10, t10, soc;              // pressure x10, temperature x10, %
  uint32_t faults, ctrlBadFrames;
  uint32_t updatedMs;                  // when this data arrived (its AGE matters)
  bool     valid;
};
Station st = {};

enum LinkState { LS_UNKNOWN, LS_OK, LS_LOST };
LinkState linkState = LS_UNKNOWN;     // NOT "link": that is a POSIX function name on the ESP32
LineRx    rx;
bool      waiting = false;             // one request at a time
char      waitingFor[8] = "";
uint32_t  sentMs = 0, lastPollMs = 0;
uint8_t   missed = 0;
uint32_t  okCount = 0, timeoutCount = 0, badCount = 0;
char      queued[8] = "";              // one operator command waiting to be sent
char      msg[21] = "";                // bottom line: last ACK / NAK
uint32_t  msgMs = 0;
uint8_t   page = 0;

// ---------- sending ----------
void sendFrame(const char* body) {
  TxFrame t; txBegin(t); txStr(t, body);
  const char* line = txEnd(t);
#if USE_SIM_CONTROLLER
  toCtrl.putLine(line);
#else
  Serial2.println(line);
#endif
  Serial.print("TX "); Serial.println(line);
  strncpy(waitingFor, body, sizeof(waitingFor) - 1);
  waiting = true; sentMs = millis();
}

bool readByte(char& c) {
#if USE_SIM_CONTROLLER
  return toHmi.get(c);
#else
  if (!Serial2.available()) return false;
  c = (char)Serial2.read(); return true;
#endif
}

void showMsg(const char* m) { strncpy(msg, m, 20); msg[20] = 0; msgMs = millis(); }

// ---------- parsing a status answer ----------
// Split "ST,IDLE,-,145,250,20,00000,0" into fields. Returns field count.
int splitFields(char* s, char* f[], int maxF) {
  int n = 0; f[n++] = s;
  for (; *s && n < maxF; s++) if (*s == ',') { *s = 0; f[n++] = s + 1; }
  return n;
}

bool parseStatus(char* body) {
  char* f[10];
  if (splitFields(body, f, 10) != 8) return false;          // wrong field count
  int32_t p10 = atol(f[3]), t10 = atol(f[4]), soc = atol(f[5]);
  // plausibility on the HMI side too: never DISPLAY a value that cannot be real
  if (p10 < 0 || p10 > 1000 || t10 < -600 || t10 > 1500 || soc < 0 || soc > 120) return false;
  strncpy(st.state, f[1], 9); st.state[9] = 0;
  strncpy(st.phase, f[2], 7); st.phase[7] = 0;
  st.p10 = p10; st.t10 = t10; st.soc = soc;
  st.faults = strtoul(f[6], nullptr, 16);
  st.ctrlBadFrames = strtoul(f[7], nullptr, 10);
  st.updatedMs = millis(); st.valid = true;
  return true;
}

void handleReply(char* body) {
  if (!waiting) { badCount++; Serial.print("RX UNSOLICITED: "); Serial.println(body); return; }
  waiting = false;
  missed = 0;
  if (linkState != LS_OK) { Serial.println("LINK OK"); if (linkState == LS_LOST) showMsg("LINK RESTORED"); }
  linkState = LS_OK;
  okCount++;
  if (!strncmp(body, "ST,", 3)) { if (!parseStatus(body)) { badCount++; Serial.println("bad status frame"); } }
  else { showMsg(body); Serial.print("RX "); Serial.println(body); }   // ACK,... / NAK,...
}

void serviceLink(uint32_t now) {
  // 1. receive
  char c, *body;
  while (readByte(c)) {
    FrameResult fr = lineFeed(rx, c, body);
    if (fr == FR_OK) handleReply(body);
    else if (fr != FR_NONE) { badCount++; Serial.print("RX BAD FRAME code "); Serial.println(fr); }
  }
  // 2. timeout of the outstanding request
  if (waiting && now - sentMs > REPLY_TIMEOUT_MS) {
    waiting = false; timeoutCount++;
    if (++missed >= LOST_AFTER && linkState != LS_LOST) {
      linkState = LS_LOST; st.valid = false;          // old data must not look current
      Serial.println("LINK LOST");
    }
  }
  // 3. send the next request: operator commands first, then the status poll
  if (!waiting) {
    if (queued[0]) { sendFrame(queued); queued[0] = 0; }
    else if (now - lastPollMs >= POLL_MS) { lastPollMs = now; sendFrame("ST"); }
  }
}

// ---------- buttons ----------
Btn bPage = {PIN_BTN_PAGE}, bStop = {PIN_BTN_STOP}, bEsd = {PIN_BTN_ESD}, bNext = {PIN_BTN_NEXTCAR},
    bSimStart = {PIN_SIM_START}, bSimReset = {PIN_SIM_RESET};
bool pressed(Btn& b) {
  bool now = digitalRead(b.pin) == LOW;
  bool edge = now && !b.last; b.last = now; return edge;
}
void queueCmd(const char* c) {
  if (linkState == LS_LOST) { showMsg("NO LINK - not sent"); return; }   // be honest with the operator
  strncpy(queued, c, sizeof(queued) - 1);
}

// ---------- display ----------
void line(uint8_t row, const char* text) {           // always write all 20 chars: no flicker, no leftovers
  char b[21]; uint8_t i = 0;
  for (; text[i] && i < 20; i++) b[i] = text[i];
  for (; i < 20; i++) b[i] = ' ';
  b[20] = 0;
  lcd.setCursor(0, row); lcd.print(b);
}

void fmt1(char* out, int32_t v10) {                  // 452 -> "45.2"
  int32_t a = v10 < 0 ? -v10 : v10;
  sprintf(out, "%s%ld.%ld", v10 < 0 ? "-" : "", (long)(a / 10), (long)(a % 10));
}

int activeFaults(uint32_t f) { int n = 0; for (int b = 0; b < N_FAULT_BITS; b++) if (f & (1u << b)) n++; return n; }

void drawMain() {
  char l[24], a[10], b[10];
  bool ok = (linkState == LS_OK && st.valid);
  snprintf(l, sizeof l, "%-9s%-7s%4s", ok ? st.state : "----", ok ? st.phase : "", linkState == LS_OK ? "OK" : "LOST");   // 9+7+4 = 20 columns (bug #17a)
  line(0, l);
  if (ok) { fmt1(a, st.p10); fmt1(b, st.t10); snprintf(l, sizeof l, "P %5sMPa T %5sC", a, b); }
  else snprintf(l, sizeof l, "P  ---- MPa T ---- C");          // Step 1 rule: no number if not valid
  line(1, l);
  if (ok) {
    char bar[11]; int n = st.soc / 10; if (n > 10) n = 10;
    for (int i = 0; i < 10; i++) bar[i] = i < n ? '#' : '.';
    bar[10] = 0;
    snprintf(l, sizeof l, "SOC %3ld%% [%s]", (long)st.soc, bar);
  } else snprintf(l, sizeof l, "SOC ---%%");
  line(2, l);
  if (millis() - msgMs < 3000) line(3, msg);
  else if (linkState == LS_LOST) line(3, "!! LINK LOST !!");
  else if (st.valid && st.faults) { snprintf(l, sizeof l, "ALARMS: %d (PAGE)", activeFaults(st.faults)); line(3, l); }
  else line(3, "PAGE STOP ESD NEXT");
}

void drawAlarms() {
  char l[24];
  int n = st.valid ? activeFaults(st.faults) : 0;
  snprintf(l, sizeof l, "ALARMS %d   %s", n, linkState == LS_OK ? "" : "(OLD!)");
  line(0, l);
  int row = 1;
  for (int b = 0; b < N_FAULT_BITS && row < 4; b++) {
    if (!(st.faults & (1u << b))) continue;
    snprintf(l, sizeof l, "%c %s", "-WISE"[faultAction(1u << b)], faultName(b));   // W/I/S/E = action
    line(row++, l);
  }
  while (row < 4) line(row++, "");
}

void drawLink() {
  char l[24];
  line(0, linkState == LS_OK ? "LINK  OK" : (linkState == LS_LOST ? "LINK  LOST" : "LINK  waiting..."));
  snprintf(l, sizeof l, "ok %lu  timeout %lu", (unsigned long)okCount, (unsigned long)timeoutCount); line(1, l);
  snprintf(l, sizeof l, "bad hmi %lu ctrl %lu", (unsigned long)badCount, (unsigned long)st.ctrlBadFrames); line(2, l);
  if (st.valid) snprintf(l, sizeof l, "data age %lu ms", (unsigned long)(millis() - st.updatedMs));
  else          snprintf(l, sizeof l, "data age ----");      // "0 ms" would look fresh (bug #17b)
  line(3, l);
}

void setup() {
  Serial.begin(115200);
#if !USE_SIM_CONTROLLER
  Serial2.begin(115200, SERIAL_8N1, 16, 17);     // RX=16, TX=17 -> STM32 USART1
#endif
  for (uint8_t p : {PIN_BTN_PAGE, PIN_BTN_STOP, PIN_BTN_ESD, PIN_BTN_NEXTCAR, PIN_SIM_START,
                    PIN_SIM_RESET, PIN_SIM_FAULT, PIN_SIM_LINKCUT}) pinMode(p, INPUT_PULLUP);
  pinMode(PIN_LED_ALARM, OUTPUT);
  lcd.init(); lcd.backlight();
  line(0, "H2 DISPENSER HMI"); line(1, "connecting...");
#if USE_SIM_CONTROLLER
  simInit(sim);
#endif
  Serial.println("H2 HMI - Step 6");
}

uint32_t lastTick = 0, lastDraw = 0;

void loop() {
  uint32_t now = millis();
  if (now - lastTick < 10) return;
  lastTick = now;

  // operator buttons on the HMI
  if (pressed(bPage)) page = (page + 1) % 3;
  if (pressed(bStop)) queueCmd("STOP");
  if (pressed(bEsd))  queueCmd("ESD");
  if (pressed(bNext)) queueCmd("NC");

  serviceLink(now);

#if USE_SIM_CONTROLLER
  bool simStart = pressed(bSimStart), simReset = pressed(bSimReset);
  simStep(sim, toCtrl, toHmi, digitalRead(PIN_SIM_FAULT) == LOW, digitalRead(PIN_SIM_LINKCUT) == LOW,
          simStart, simReset, now);
#endif

  // alarm LED: solid = must act (INHIBIT or worse) or link lost, blinking = warnings only
  Action w = st.valid ? worstAction(st.faults) : ACT_NONE;
  bool alarm = linkState == LS_LOST || w >= ACT_INHIBIT || (w == ACT_WARN && (now / 500) % 2);
  digitalWrite(PIN_LED_ALARM, alarm);

  if (now - lastDraw >= 250) {
    lastDraw = now;
    if (page == 0) drawMain(); else if (page == 1) drawAlarms(); else drawLink();
  }
}
