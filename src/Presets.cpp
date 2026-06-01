/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Preset handler
//
// Presets.cpp -- preset persistence (LittleFS) + the serial patch console.
#include "Presets.h"
#include "GloveState.h"     // shared musical/tempo state a patch captures
#include "ScaleQuant.h"     // NoteSpread(), SCALE_MAX (Scale clamp)
#include "TempoControl.h"   // setTempo()
#include "LedDisplay.h"     // NQ_/FQ_ enums
#include "DebugSerial.h"

#include <string.h>
#include <stdlib.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;

// -----------------------------------------------------------------------------
// Patch persistence (internal flash via Adafruit LittleFS).
// Each slot is stored as a small binary file "/patchNN.bin".
// -----------------------------------------------------------------------------
static void patchPath(int slot, char out[24])
{
  snprintf(out, 24, "/patch%02d.bin", slot);
}

// Standard CRC-32 (poly 0xEDB88320).
static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      uint32_t mask = -(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

// CRC over every byte of the Patch EXCEPT its own trailing crc field.
static uint32_t patchCrc(const Patch &p)
{
  return crc32_buf((const uint8_t *)&p, sizeof(Patch) - sizeof(p.crc));
}

// True only if magic, version, AND the stored CRC all check out. No side effects.
static bool patchValid(const Patch &p)
{
  return p.magic == PATCH_MAGIC &&
         p.version == PATCH_VERSION &&
         p.crc == patchCrc(p);
}

bool savePatch(int slot)
{
  if (slot < 0 || slot >= PRESET_COUNT) return false;

  Patch p;
  p.magic            = PATCH_MAGIC;
  p.version          = PATCH_VERSION;
  p.tempo            = tempo;
  p.octave           = octave;
  p.keyTranspose     = keyTranspose;
  p.rootNote         = RootNote;
  p.noteQuantizeMode = noteQuantizeMode;
  p.flexQuantizeMode = flexQuantizeMode;
  p.scale            = Scale;
  p.spread           = Spread;
  p.minNote          = minNote;
  p.maxNote          = maxNote;
  p.brightnessLevel  = brightnessLevel;
  p.tempoBendEnabled = tempoBendEnabled ? 1 : 0;
  p.ccSwapped        = ccSwapped ? 1 : 0;
  p.crc              = patchCrc(p);   // checksum over all preceding fields

  char path[24];
  patchPath(slot, path);

  InternalFS.remove(path); // overwrite cleanly

  File f(InternalFS);
  if (!f.open(path, FILE_O_WRITE)) {
    DBG_PRINT("savePatch: open failed for ");
    DBG_PRINTLN(path);
    return false;
  }
  size_t wrote = f.write((const uint8_t *)&p, sizeof(p));
  f.close();

  if (wrote != sizeof(p)) {
    DBG_PRINT("savePatch: short write ");
    DBG_PRINTLN(wrote);
    return false;
  }
  DBG_PRINT("Patch saved to slot ");
  DBG_PRINTLN(slot);
  // The saved tempo becomes the new project reference for tempo-bend tracking.
  projectTempo = tempo;
  lastSentBendTempo = -1;
  return true;
}

bool applyPatch(const Patch &p)
{
  if (!patchValid(p)) {
    return false;
  }

  // Apply to live globals (with defensive clamps).
  octave           = p.octave;
  if (octave < 0) octave = 0;
  if (octave > OCTAVE_MAX_INDEX) octave = OCTAVE_MAX_INDEX;
  keyTranspose     = p.keyTranspose;
  if (keyTranspose < KEY_TRANSPOSE_MIN) keyTranspose = KEY_TRANSPOSE_MIN;
  if (keyTranspose > KEY_TRANSPOSE_MAX) keyTranspose = KEY_TRANSPOSE_MAX;
  RootNote         = p.rootNote;
  if (RootNote > 127) RootNote = 127;
  noteQuantizeMode = p.noteQuantizeMode;
  if (noteQuantizeMode < 0 || noteQuantizeMode >= NQ_MODE_COUNT) noteQuantizeMode = NQ_OFF;
  flexQuantizeMode = p.flexQuantizeMode;
  if (flexQuantizeMode < 0 || flexQuantizeMode >= FQ_MODE_COUNT) flexQuantizeMode = FQ_OFF;
  Scale            = p.scale;
  if (Scale >= SCALE_MAX) Scale = 0; // bound: Scale indexes scale_chromatic_map[]
  Spread           = p.spread;
  if (Spread < 1) Spread = 1;
  if (Spread > 4) Spread = 4;
  minNote          = p.minNote;
  maxNote          = p.maxNote;
  if (minNote < 0) minNote = 0;
  if (maxNote > 127) maxNote = 127;
  if (minNote >= maxNote) { minNote = 48; maxNote = 84; }
  brightnessLevel  = p.brightnessLevel;
  if (brightnessLevel < 0) brightnessLevel = 0;
  if (brightnessLevel >= BRIGHTNESS_LEVEL_COUNT) brightnessLevel = BRIGHTNESS_LEVEL_COUNT - 1;
  tempoBendEnabled = (p.tempoBendEnabled != 0);
  // Restore the CC74/CC1 axis-swap mapping (set absolutely from the saved flag).
  ccSwapped = (p.ccSwapped != 0);
  if (ccSwapped) { CCAccelX = 1;  CCAccelY = 74; }
  else           { CCAccelX = 74; CCAccelY = 1;  }
  // The loaded preset's tempo is the project reference the sample was made at.
  projectTempo = p.tempo;
  lastSentBendTempo = -1;

  // Rebuild the chromatic Keys[] from the restored spread, and re-derive the
  // flex grid from the restored tempo + flex-quantize mode.
  NoteSpread(RootNote, Spread, RootNoteOffset);
  setTempo(p.tempo);
  noteQuantizeNextGridMs = 0; // re-align the note grid in the (possibly new) mode
  applyStripBrightness(brightnessLevel); // apply restored brightness (main.cpp)

  DBG_PRINTLN("Patch applied");
  return true;
}

bool loadPatch(int slot)
{
  if (slot < 0 || slot >= PRESET_COUNT) return false;

  char path[24];
  patchPath(slot, path);

  File f(InternalFS);
  if (!f.open(path, FILE_O_READ)) {
    DBG_PRINT("loadPatch: no patch in slot ");
    DBG_PRINTLN(slot);
    return false;
  }
  Patch p;
  size_t got = f.read((uint8_t *)&p, sizeof(p));
  f.close();

  if (got != sizeof(p)) {
    DBG_PRINT("loadPatch: invalid/empty patch in slot ");
    DBG_PRINTLN(slot);
    return false;
  }
  if (!applyPatch(p)) {
    DBG_PRINT("loadPatch: bad magic/version in slot ");
    DBG_PRINTLN(slot);
    return false;
  }
  DBG_PRINT("Patch loaded from slot ");
  DBG_PRINTLN(slot);
  return true;
}

// Arm the brief save/load confirmation flash (a 300 ms gradient bar).
//   loaded == false -> SAVE confirmation (orange -> cyan)
//   loaded == true  -> LOAD confirmation (green  -> cyan)
void TriggerPatchConfirm(bool loaded)
{
  patchConfirmKind    = loaded ? 1 : 0;
  patchConfirmStartMs = millis();
  patchConfirmActive  = true;
}

// -----------------------------------------------------------------------------
// Serial patch console.  Commands (newline-terminated, 115200 baud):
//   HELP | LIST | DUMP | DUMP <n> | LOAD <slot> <base64> | FORMAT YES
// Each patch is one self-contained "PATCH <slot> <base64>" line; the struct's
// trailing CRC32 lets the loader reject a corrupted paste before writing flash.
// Serviced from loop() ABOVE the BLE gate, so it works with no MIDI host.
// -----------------------------------------------------------------------------

// Case-insensitive check that `s` begins with command word `kw` (length n).
static bool cmdIs(const char *s, const char *kw, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    char a = s[i], b = kw[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return false;
  }
  return true;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *src, size_t len, char *dst)
{
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = (uint32_t)src[i] << 16;
    int rem = (int)(len - i);
    if (rem > 1) n |= (uint32_t)src[i + 1] << 8;
    if (rem > 2) n |= (uint32_t)src[i + 2];
    dst[o++] = B64[(n >> 18) & 0x3F];
    dst[o++] = B64[(n >> 12) & 0x3F];
    dst[o++] = (rem > 1) ? B64[(n >> 6) & 0x3F] : '=';
    dst[o++] = (rem > 2) ? B64[n & 0x3F]        : '=';
  }
  dst[o] = '\0';
}

static int b64val(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1; // '=' or invalid
}

static int base64_decode(const char *src, uint8_t *dst, size_t dstCap)
{
  int acc = 0, bits = 0;
  size_t o = 0;
  for (const char *p = src; *p; p++) {
    if (*p == '=' || *p == '\r' || *p == '\n' || *p == ' ') continue;
    int v = b64val(*p);
    if (v < 0) return -1;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= dstCap) return -1;
      dst[o++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)o;
}

static bool readPatchBytes(int slot, Patch &p)
{
  if (slot < 0 || slot >= PRESET_COUNT) return false;
  char path[24];
  patchPath(slot, path);
  File f(InternalFS);
  if (!f.open(path, FILE_O_READ)) return false;
  size_t got = f.read((uint8_t *)&p, sizeof(p));
  f.close();
  if (got != sizeof(p)) return false;
  return patchValid(p);
}

static bool writePatchBytes(int slot, const Patch &p)
{
  if (slot < 0 || slot >= PRESET_COUNT) return false;
  char path[24];
  patchPath(slot, path);
  InternalFS.remove(path);
  File f(InternalFS);
  if (!f.open(path, FILE_O_WRITE)) return false;
  size_t wrote = f.write((const uint8_t *)&p, sizeof(p));
  f.close();
  return wrote == sizeof(p);
}

static void printPatchLine(int slot, const Patch &p)
{
  char b64[((sizeof(Patch) + 2) / 3) * 4 + 1];
  base64_encode((const uint8_t *)&p, sizeof(Patch), b64);
  Serial.print("PATCH ");
  Serial.print(slot);
  Serial.print(' ');
  Serial.println(b64);
}

static void consoleDumpSlot(int slot)
{
  Patch p;
  if (readPatchBytes(slot, p)) {
    printPatchLine(slot, p);
  } else {
    Serial.print("; slot ");
    Serial.print(slot);
    Serial.println(" empty/invalid");
  }
}

static void consoleDumpAll()
{
  Serial.println("; ---- patch dump begin ----");
  int found = 0;
  for (int s = 0; s < PRESET_COUNT; s++) {
    Patch p;
    if (readPatchBytes(s, p)) { printPatchLine(s, p); found++; }
  }
  Serial.print("; ---- patch dump end (");
  Serial.print(found);
  Serial.println(" patches) ----");
}

static void consoleList()
{
  Serial.println("; slots with a valid patch:");
  int found = 0;
  for (int s = 0; s < PRESET_COUNT; s++) {
    Patch p;
    if (readPatchBytes(s, p)) {
      Serial.print(";   slot ");
      Serial.print(s);
      Serial.print("  tempo=");
      Serial.print(p.tempo);
      Serial.print(" scale=");
      Serial.print(p.scale);
      Serial.print(" oct=");
      Serial.println(p.octave);
      found++;
    }
  }
  if (!found) Serial.println(";   (none)");
}

static void consoleLoad(const char *args)
{
  while (*args == ' ') args++;
  int slot = atoi(args);
  while (*args && *args != ' ') args++;
  while (*args == ' ') args++;
  if (slot < 0 || slot >= PRESET_COUNT) {
    Serial.println("; LOAD error: slot out of range (0..62)");
    return;
  }
  if (!*args) {
    Serial.println("; LOAD error: missing data");
    return;
  }

  uint8_t buf[sizeof(Patch)];
  int n = base64_decode(args, buf, sizeof(buf));
  if (n != (int)sizeof(buf)) {
    Serial.print("; LOAD error: bad length (got ");
    Serial.print(n);
    Serial.print(", expected ");
    Serial.print((int)sizeof(buf));
    Serial.println(")");
    return;
  }

  Patch p;
  memcpy(&p, buf, sizeof(Patch));
  if (!patchValid(p)) {
    Serial.println("; LOAD error: invalid patch (bad CRC/magic/version) - not written");
    return;
  }
  if (writePatchBytes(slot, p)) {
    Serial.print("; LOAD ok: slot ");
    Serial.print(slot);
    Serial.println(" written");
  } else {
    Serial.print("; LOAD error: write failed for slot ");
    Serial.println(slot);
  }
}

static void consoleFormat(const char *args)
{
  while (*args == ' ') args++;
  if (strncmp(args, "YES", 3) != 0) {
    Serial.println("; FORMAT: type 'FORMAT YES' to confirm (erases ALL patches)");
    return;
  }
  Serial.println("; formatting LittleFS...");
  InternalFS.format();
  if (InternalFS.begin()) Serial.println("; format complete, FS remounted");
  else                    Serial.println("; format done but remount FAILED");
}

static void printConsoleHelp()
{
  Serial.println("; commands: HELP | LIST | DUMP | DUMP <n> | LOAD <slot> <b64> | FORMAT YES");
}

void serviceSerialConsole()
{
  static char line[256];
  static size_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      char *cmd = line;
      while (*cmd == ' ') cmd++;
      if (*cmd) {
        if      (cmdIs(cmd, "HELP", 4))   printConsoleHelp();
        else if (cmdIs(cmd, "LIST", 4))   consoleList();
        else if (cmdIs(cmd, "DUMP", 4)) {
          const char *a = cmd + 4;
          while (*a == ' ') a++;
          if (*a) consoleDumpSlot(atoi(a));
          else    consoleDumpAll();
        }
        else if (cmdIs(cmd, "LOAD", 4))   consoleLoad(cmd + 4);
        else if (cmdIs(cmd, "FORMAT", 6)) consoleFormat(cmd + 6);
        else {
          Serial.print("; unknown command: ");
          Serial.println(cmd);
          printConsoleHelp();
        }
      }
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0;
      Serial.println("; input line too long - discarded");
    }
  }
}
