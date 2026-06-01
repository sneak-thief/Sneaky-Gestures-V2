# Sneaky Gestures V2: Glove MIDI Controller — Player's Guide

A wearable Bluetooth MIDI instrument. You play notes by touching your thumb to
conductive pads on your fingers, and shape the sound by bending your index
finger, pressing with your thumb, and tilting your hand. A 7-LED strip on the
back of the hand shows you what's going on — see the
**[LED_Mode_Reference.html](https://htmlpreview.github.io/?https://github.com/sneak-thief/Sneaky-Gestures-V2/blob/main/documentation/LED_Mode_Reference.html)** for the full visual legend.

---

## 1. Getting started

1. **Power on.** The glove runs from its own LiPo battery. The LED strip lights
   briefly at boot.
2. **Pair over Bluetooth.** In your DAW or synth app, open its MIDI/Bluetooth
   settings and connect to the glove as a BLE-MIDI device.
3. **Check the battery any time** by double-tapping the glove (a sharp tap on
   the back of the hand). Green bars show charge level; a blinking red LED means
   low battery; solid blue means it's charging.

> Most controls only send MIDI once a host is connected. The battery display and
> LED animations work even while unpaired.

---

## 2. Playing notes

Your **thumb is the "play" contact.** Touching it to a finger pad plays that
note. The three pads on each finger give you twelve note pads in total:

| Finger | Pads |
|--------|------|
| Index  | 3 pads |
| Middle | 3 pads |
| Ring   | 3 pads |
| Pinky  | 3 pads |

The notes are laid out low-to-high across the hand, so you can play runs by
walking the thumb across the fingers. Which actual pitches sound depends on the
current **scale**, **key**, **octave**, and **spread** (all explained below).

When you touch a pad, the source knuckle lights up and a soft ripple of light
spreads across the strip. Releasing fades the light out.

### The four "modal" pads

Besides the twelve note pads, four special pads control the instrument:

| Pad             | Tap                              | Hold (1 second)                |
|-----------------|----------------------------------|--------------------------------|
| **Octave ↑**    | Shift up one octave              | Open the **Tempo / Perf menu** |
| **Octave ↓**    | Shift down one octave            | Open the **Preset browser**    |
| **Index modal** | Enable **flex notes** while held | —                              |
| **Pinky palm**  | Cycle **note spread** (1–4)      | Toggle **Scale edit**          |

---

## 3. Expressive controls

These run continuously while you play and are what make the glove feel like an
instrument rather than a button grid.

- **Index flex (bend sensor).** Bend your index finger to sweep pitch across a
  range of scale notes. Hold the **Index modal** pad to arm the flex note; the
  bend then glides smoothly through the available notes between the low and high
  limits you've set. Straight finger = lowest note, fully bent = highest.
- **Thumb pressure (FSR).** Press harder with the thumb to send channel
  aftertouch — use it for vibrato, swells, or filter movement, depending on how
  your synth maps aftertouch.
- **Hand tilt (motion sensor).** Tilting the hand sends continuous controller
  (CC) messages — by default a mod-wheel-style control on one axis and a
  brightness/cutoff control on the other. (You can swap which axis sends which
  CC from the performance menu.)

---

## 4. Tempo / Performance menu

**Hold Octave ↑ for one second** to enter. While you're in the menu, the note
pads you aren't using still play normally, so you can keep performing. Tap
Octave ↑ again (or wait) to leave. The strip blinks on the beat and ripples
outward at the current tempo.

Inside the menu:

- **Transpose the key** — nudge the whole instrument up or down in semitones.
- **Rotate the mode (root transpose)** — keeps the same notes but rotates which
  one is "home," turning, say, a major scale into Dorian, Phrygian, and so on.
  The center LED turns blue to show this mode is active.
- **Nudge the tempo** — bump the BPM up or down by one, handy for matching a
  click or a band.
- **Swap the tilt CCs** — flips which hand axis controls which CC.
- **Set the flex range** (held from inside the menu) — define the lowest and
  highest notes the index-finger bend will reach. A numeric readout on the strip
  shows the note value as you adjust it.

---

## 5. Scales, key, octave, and spread

- **Scale** decides which notes the pads are quantized to. **Hold the Pinky palm
  pad** to enter Scale edit (it latches on), then step through scales. The strip
  shows the current scale as a colored bar — red, then yellow, then blue bands as
  you move through the list. It exits on its own after a few seconds of no input.
- **Key / octave** are set with the Octave pads and the menu transpose options
  above.
- **Spread** changes how the twelve pads are voiced — from tight, adjacent notes
  to wider intervals. **Quick-tap the Pinky palm pad** to cycle through the four
  spread settings.

---

## 6. Presets

A preset stores your whole setup: tempo, octave, key, scale, spread, flex range,
quantize settings, brightness, and tilt-CC assignment.

**Hold Octave ↓ for one second** to open the preset browser. From there you can:

- **Scroll** through saved presets.
- **Cycle the note and flex quantize** settings (how tightly notes snap to the
  beat grid).
- **Toggle tempo pitch-bend** (auto-bends pitch when an external clock drifts
  from the preset tempo). Useful for staying at the same pitch of audio whose 
  pitch changes with tempo, eg. vinyl, CDJs in variable pitch mode, etc. 
- **Cycle LED brightness**, with a live rainbow preview.
- **Load** the highlighted preset and exit (Index modal pad). The strip flashes
  a green→cyan bar to confirm the load.
- **Save** to the current slot and exit (hold the Pinky palm pad). The strip
  flashes an orange→cyan bar to confirm the save.
- **Exit without saving** (hold Octave ↓).

Your host can also switch presets directly by sending a **MIDI Program Change** —
the strip flashes a band of color to confirm the load.

**Backing up and restoring presets.** Connect the glove over USB and open a serial monitor at
115200 baud. Type `DUMP` to print every saved preset as a `PATCH` line you can
copy and keep; paste a line back with `LOAD <slot> <line>` to restore it. Each
line is checksummed, so a bad paste is refused rather than written.

The `DUMP` command (no argument) is the bulk dump — it walks all 63 slots and prints a PATCH <slot> <base64> line for every slot that holds a valid patch, bracketed by ; ---- patch dump begin ---- / ; ---- patch dump end (N patches) ----. You copy those lines out of the serial monitor to back them up.
The full set of console commands (USB serial, 115200 baud, type and press Enter):

- `DUMP` — bulk dump of all non-empty slots
- `DUMP <n>` — dump just slot n
- `LIST` — show which slots have a valid patch (with tempo/scale/octave)
- `LOAD <slot> <base64>` — paste a line back to restore it to that slot (validated by CRC + magic + version before writing)
- `FORMAT YES` — reformat the filesystem
- `HELP` — list the commands

---

## 7. Reading the LEDs at a glance

| You see | It means |
|------------------------------------------|------------------------------------------|
| A knuckle lit red/white                  | That note pad is being held              |
| A ripple of note-specific coloured light | A note just triggered                    |
| Beat blink + outward ripple              | The Tempo menu; this is the tempo        |
| Solid blue center LED                    | Root-transpose (mode rotation) is active |
| A colored bar (red/yellow/blue)          | Scale edit — shows the selected scale    |
| Numeric readout                          | Adjusting tempo or flex-range values     |
| Orange→cyan bar (brief)                  | Preset saved                             |
| Green→cyan bar (brief)                   | Preset loaded                            |
| Green bars                               | Battery level (after double-tap or boot) |
| Blinking red LED                         | Battery low                              |
| Solid blue on first LED + green LEDs     | Charging                                 |

For the full breakdown of every LED state — exact colors, animations, and what
each display means in each mode — see the **[LED_Mode_Reference.html](https://htmlpreview.github.io/?https://github.com/sneak-thief/Sneaky-Gestures-V2/blob/main/documentation/LED_Mode_Reference.html)**.

---

## 8. Quick reference

| Action | How |
|--------|-----|
| Play a note | Touch thumb to a finger pad |
| Octave up / down | Tap Octave ↑ / Octave ↓ |
| Flex-note glide | Hold Index modal, bend index finger |
| Aftertouch | Press harder with the thumb |
| Tilt CCs | Tilt the hand |
| Change spread | Quick-tap Pinky palm |
| Change scale | Hold Pinky palm, then step through |
| Tempo / performance menu | Hold Octave ↑ |
| Preset browser | Hold Octave ↓ |
| Save preset | In browser, hold Pinky palm |
| Check battery | Double-tap the back of the hand |

---
## 9. Serial Debug

Serial deubgging can be set in DebugSerial.h

Set either to 1 to enable, 0 to disable.

The default is that both are enabled:
- #define DEBUG_SERIAL       1
- #define DEBUG_SERIAL_BOOT  1

---

*Tip: start simple — pick a scale, set your octave, and just play across the
finger pads. Add the flex glide and thumb pressure once the note layout feels
natural under your hand.*
