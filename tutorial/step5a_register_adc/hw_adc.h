// hw_adc.h - STM32C0 ADC driven directly through its registers.
// Replaces analogRead(): same result, ~2 KB less flash (no HAL ADC driver).
// Reference: RM0490 (STM32C0x1 reference manual), chapter "ADC".
#pragma once
#include <Arduino.h>

// Every wait loop has a TIMEOUT. If the ADC ever stops answering, the
// function returns 0xFFFF instead of hanging the controller forever.
// 0xFFFF is above 4095, so the sensor code sees 21+ mA -> FAULT-SHORT:
// a dead ADC becomes a detected fault, not a frozen controller.
const uint16_t ADC_TIMEOUT = 0xFFFF;

static inline bool adcWait(volatile uint32_t& reg, uint32_t mask) {
  for (uint32_t i = 0; i < 20000; i++) if (reg & mask) return true;
  return false;
}

inline void adcInit() {
  RCC->APBENR2 |= RCC_APBENR2_ADCEN;                    // 1. clock on
  RCC->IOPENR  |= RCC_IOPENR_GPIOAEN;
  // 2. PA0, PA1, PA4, PA6 -> analog mode (MODER = 11)
  GPIOA->MODER |= (3u << (0 * 2)) | (3u << (1 * 2)) | (3u << (4 * 2)) | (3u << (6 * 2));
  ADC1->CFGR2 = (1u << ADC_CFGR2_CKMODE_Pos);           // 3. ADC clock = PCLK / 2
  ADC1->CR = ADC_CR_ADVREGEN;                           // 4. internal regulator on ...
  delayMicroseconds(30);                                //    ... needs ~20 us to settle
  ADC1->CR |= ADC_CR_ADCAL;                             // 5. self-calibration
  for (uint32_t i = 0; i < 20000 && (ADC1->CR & ADC_CR_ADCAL); i++) {}
  ADC1->SMPR = 7u;                                      // 6. longest sampling time (160.5 cycles)
  ADC1->ISR = ADC_ISR_ADRDY;                            // 7. enable and wait until ready
  ADC1->CR |= ADC_CR_ADEN;
  adcWait(ADC1->ISR, ADC_ISR_ADRDY);
}

// One 12-bit conversion of channel 'ch' (PA0=0, PA1=1, PA4=4, PA6=6).
inline uint16_t adcRead(uint8_t ch) {
  ADC1->ISR   = ADC_ISR_CCRDY;                          // select the channel ...
  ADC1->CHSELR = 1u << ch;
  if (!adcWait(ADC1->ISR, ADC_ISR_CCRDY)) return ADC_TIMEOUT;   // ... wait until applied
  ADC1->ISR = ADC_ISR_EOC | ADC_ISR_EOS;
  ADC1->CR |= ADC_CR_ADSTART;                           // start
  if (!adcWait(ADC1->ISR, ADC_ISR_EOC)) return ADC_TIMEOUT;     // wait end of conversion
  return (uint16_t)ADC1->DR;                            // reading DR clears EOC
}
