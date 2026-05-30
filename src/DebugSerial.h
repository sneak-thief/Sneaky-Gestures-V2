#ifndef DEBUG_SERIAL_H
#define DEBUG_SERIAL_H

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Debug serial logging. Two tiers so chatty per-event logging can be compiled
// out for release builds while keeping (or separately gating) startup/boot
// diagnostics. When a tier is disabled, its macros expand to nothing, so the
// Serial calls AND their string literals are removed by the compiler -- no CPU
// cost and no USB-CDC blocking stalls in hot paths.
//
//   DEBUG_SERIAL       : verbose per-event logging
//                        -> DBG_PRINT / DBG_PRINTLN / DBG_PRINTF
//   DEBUG_SERIAL_BOOT  : startup / init / battery diagnostics
//                        -> BOOT_PRINT / BOOT_PRINTLN / BOOT_PRINTF
//
// Set either to 1 to enable, 0 to disable.
#define DEBUG_SERIAL       1
#define DEBUG_SERIAL_BOOT  1

#if DEBUG_SERIAL
  #define DBG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)   ((void)0)
  #define DBG_PRINTLN(...) ((void)0)
  #define DBG_PRINTF(...)  ((void)0)
#endif

#if DEBUG_SERIAL_BOOT
  #define BOOT_PRINT(...)   Serial.print(__VA_ARGS__)
  #define BOOT_PRINTLN(...) Serial.println(__VA_ARGS__)
  #define BOOT_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
  #define BOOT_PRINT(...)   ((void)0)
  #define BOOT_PRINTLN(...) ((void)0)
  #define BOOT_PRINTF(...)  ((void)0)
#endif

#endif // DEBUG_SERIAL_H
