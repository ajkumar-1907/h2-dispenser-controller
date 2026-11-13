// link.h - text protocol between the controller (STM32) and the HMI (ESP32).
//
// Frame:   $BODY*HH<newline>
//   BODY = comma-separated fields, e.g.  ST   or   STOP   or   ST,FILLING,RAMP,452,...
//   HH   = checksum: XOR of every character of BODY, as 2 hex digits
// Same framing idea as NMEA (GPS receivers). Human-readable, so you can type
// it into the Wokwi serial monitor and read the answers yourself.
//
// The HMI is the MASTER (it asks), the controller is the SLAVE (it answers):
// a slave never talks unless asked, so two devices never transmit at once.
#pragma once
#include <stdint.h>
#include <string.h>

const uint8_t FRAME_MAX = 64;          // longest frame we accept (worst case 54 chars: see docs/debugging-log.md #16)
                                      // (NOT "LINE_MAX": that name is already a macro in the ESP32 C library)

enum FrameResult { FR_NONE, FR_OK, FR_OVERFLOW, FR_NO_DOLLAR, FR_NO_STAR, FR_BAD_CS };

struct LineRx {
  char    buf[FRAME_MAX + 1];
  uint8_t len;
  bool    overflow;
};

inline uint8_t xorChecksum(const char* s, uint8_t n) {
  uint8_t cs = 0;
  for (uint8_t i = 0; i < n; i++) cs ^= (uint8_t)s[i];
  return cs;
}

inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Feed ONE received character. Returns FR_NONE until a line ends.
// On FR_OK, 'body' points to the NUL-terminated BODY inside r.buf.
inline FrameResult lineFeed(LineRx& r, char c, char*& body) {
  if (c != '\n' && c != '\r') {
    if (r.len < FRAME_MAX) r.buf[r.len++] = c;
    else r.overflow = true;           // too long: keep reading, but the line is dead
    return FR_NONE;
  }
  if (r.len == 0 && !r.overflow) return FR_NONE;   // empty line (e.g. "\r\n")
  r.buf[r.len] = 0;
  uint8_t n = r.len;
  bool ovf = r.overflow;
  r.len = 0; r.overflow = false;
  if (ovf) return FR_OVERFLOW;        // never parse a truncated frame
  if (r.buf[0] != '$') return FR_NO_DOLLAR;
  char* star = strchr(r.buf, '*');
  if (!star || star + 3 != r.buf + n) return FR_NO_STAR;
  int hi = hexVal(star[1]), lo = hexVal(star[2]);
  if (hi < 0 || lo < 0) return FR_BAD_CS;
  uint8_t want = (uint8_t)(hi * 16 + lo);
  *star = 0;                          // cut "*HH" off
  body = r.buf + 1;                   // skip '$'
  if (xorChecksum(body, (uint8_t)(star - body)) != want) return FR_BAD_CS;
  return FR_OK;
}

// ---- building an answer without printf (printf costs several KB of flash) ----
struct TxFrame { char buf[64]; uint8_t len; };
inline void txBegin(TxFrame& t)                 { t.len = 0; }
inline void txStr(TxFrame& t, const char* s)    { while (*s && t.len < 60) t.buf[t.len++] = *s++; }
inline void txInt(TxFrame& t, int32_t v) {
  char tmp[12]; uint8_t n = 0;
  bool neg = v < 0; uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
  do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (neg) tmp[n++] = '-';
  while (n && t.len < 60) t.buf[t.len++] = tmp[--n];
}
inline void txHex(TxFrame& t, uint32_t v, uint8_t digits) {
  for (int8_t i = (int8_t)digits - 1; i >= 0 && t.len < 60; i--)
    t.buf[t.len++] = "0123456789ABCDEF"[(v >> (i * 4)) & 0xF];
}
// Finish: returns the full line  $BODY*HH  (caller prints it + newline)
inline const char* txEnd(TxFrame& t) {
  uint8_t cs = xorChecksum(t.buf, t.len);
  static char out[70];
  out[0] = '$';
  memcpy(out + 1, t.buf, t.len);
  out[t.len + 1] = '*';
  out[t.len + 2] = "0123456789ABCDEF"[cs >> 4];
  out[t.len + 3] = "0123456789ABCDEF"[cs & 0xF];
  out[t.len + 4] = 0;
  return out;
}
