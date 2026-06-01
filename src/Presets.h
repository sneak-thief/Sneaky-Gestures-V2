/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Preset handler
//
// Presets.h
//
// Preset (patch) persistence + the serial patch console. A preset is the full
// set of recallable settings, stored as a small binary blob per slot in
// internal flash (Adafruit LittleFS) with a magic + version + trailing CRC32.
//
// The musical/tempo state a patch captures lives in GloveState.h; a handful of
// other live globals it also restores (brightness, CC-swap, selected slot) are
// declared extern below. Brightness is applied through applyStripBrightness(),
// implemented in main.cpp, so this module needs no NeoPixel dependency.
#pragma once

#include <Arduino.h>

#define PATCH_MAGIC   0x47415450UL // 'PTAG'
#define PATCH_VERSION 6            // bumped to 6: added trailing CRC32 field

// -----------------------------------------------------------------------------
// FACTORY RESET flag.
//   Set to 1, build + flash, and power on ONCE. At boot the firmware reformats
//   the internal-flash filesystem -- wiping ALL saved patches AND the BLE
//   bonding data (Bluefruit stores bonds as files in the same InternalFS), then
//   continues to boot normally. This is the fix for "advertises but won't
//   connect" caused by corrupted bonds, and for a corrupted patch filesystem.
//   AFTER the reset boot: set this back to 0 and reflash, or every boot wipes.
//   Remember to also "forget"/remove the glove on the host's Bluetooth list.
// -----------------------------------------------------------------------------
#define FACTORY_RESET 0

struct Patch {
  uint32_t magic;
  uint32_t version;
  int   tempo;
  int   octave;
  int   keyTranspose;
  unsigned int rootNote;
  int   noteQuantizeMode;
  int   flexQuantizeMode;
  unsigned int scale;
  unsigned int spread;
  int   minNote;
  int   maxNote;
  int   brightnessLevel;
  int   tempoBendEnabled;
  int   ccSwapped;
  uint32_t crc;   // CRC32 over all preceding bytes; MUST be the last field
};

// --- Live globals a patch restores (defined in main.cpp) --------------------
extern const int PRESET_COUNT;
extern int selectedPreset;
extern int brightnessLevel;
extern const uint8_t BRIGHTNESS_LEVELS[7];
extern const int BRIGHTNESS_LEVEL_COUNT;
extern unsigned int CCAccelX;
extern unsigned int CCAccelY;
extern bool ccSwapped;

// Applies BRIGHTNESS_LEVELS[level] to the LED strip. Implemented in main.cpp so
// this module stays free of the NeoPixel dependency.
void applyStripBrightness(int level);

// --- Public API -------------------------------------------------------------
bool savePatch(int slot);            // write live settings to a slot
bool applyPatch(const Patch &p);     // validate (magic+version+CRC) + apply to globals
bool loadPatch(int slot);            // read a slot from flash + apply
void TriggerPatchConfirm(bool loaded); // arm the 300 ms save/load LED flash

// Serial patch console (HELP / LIST / DUMP / LOAD / FORMAT). Call often from
// loop(); non-blocking, accumulates one line at a time from Serial.
void serviceSerialConsole();
