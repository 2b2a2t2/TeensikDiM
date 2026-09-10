# TeensikDiM - Touch MIDI Controller

A Teensy-based MIDI controller with capacitive touch sensors, rotary encoders, OLED display, and NeoPixel LEDs.

## Hardware

- **MCU**: Teensy (4.x recommended)
- **Touch**: 4x MPR121 capacitive sensors (I2C addresses 0x5A-0x5D) providing 48 touch channels
- **Display**: SSD1306 128x64 OLED via I2C (U8g2 library)
- **LEDs**: 42 NeoPixel LEDs on pin 6
- **Encoders**: 8 rotary encoders (shared AHEncoder objects for MIDI + raw pin reading in KEY mode)
- **MIDI**: USB MIDI + Serial2 MIDI (MIDI DIN output)

## MIDI Mapping

### Pads (Channel 1)
16 pads sending notes 59-74 (B3-E5). Velocity is fixed at 127.

### Keyboard (Channel 1+)
12 keys (C4-B4) using `BankableTouchNote` with address-based banking.
- Bank selects octave: select(0) = C-2, select(5) = C3 (default), select(10) = C8
- Notes sent on Channel 1

### Controls (Channel 1)
14 control buttons sending notes 75-100.

| Button | Note | Function |
|--------|------|----------|
| PREV | 75 | Previous |
| NEXT | 76 | Next |
| M1 | 77 | Toggle Discovery Mode (in KEY mode) |
| M2 | 78 | Menu button |
| M3 | 79 | Menu button |
| F1 | 80 | Discovery page 1 — CHORD params |
| F2 | 81 | Discovery page 2 — COLOR params |
| F3 | 82 | Discovery page 3 — VOICING params |
| F4 | 83 | Discovery page 4 — DISCOVERY params |
| ENC | 96 | **Mode button** — Hold for encoder bank mode |
| SEQ | 97 | Sequencer mode toggle |
| KEY | 98 | **Mode button** — Tap for KEY mode, hold for keyboard bank |
| PLAY | 99 | Play |
| REC | 100 | Record |

### Encoders (Channel 1)
8 encoders sending CC values 0-127. Bank switching changes which CC numbers are active.

| Encoder | CC | Bank switching |
|---------|-----|----------------|
| enc0 | 74 | ChangeChannel via bankEnc |
| enc1 | 71 | ChangeChannel via bankEnc |
| enc2 | 75 | ChangeChannel via bankEnc |
| enc3 | 76 | ChangeChannel via bankEnc |
| enc4 | 91 | ChangeChannel via bankEnc |
| enc5 | 92 | ChangeChannel via bankEnc |
| enc6 | 94 | ChangeChannel via bankEnc |
| enc7 | 7 | ChangeChannel via bankEnc |

## Operating Modes

### Normal Mode (Default)
- Pads send MIDI notes on CH1
- Keyboard sends notes via `BankableTouchNote` (octave changes with bank)
- Encoders send CC values
- Display shows note ranges and bank info

### Key Mode (KEY tap)
Tap KEY button to toggle. When active:
- Keyboard LEDs show scale pattern (dim blue for in-scale, off for out-of-scale)
- Out-of-scale keys flash red and are blocked
- 4 encoders repurposed (raw pin reading, no MIDI):
  - **ENC0**: Octave (-2 to 8)
  - **ENC1**: Scale selection (13 scales)
  - **ENC2**: Root note (C through B)
  - **ENC3**: Chord on/off (clockwise = ON, CCW = OFF)
- Encoders 4-7: Discovery Mode page parameters (when Discovery is active)
- Encoder positions saved/restored on mode transitions to prevent CC jumps
- Display shows Oct, Scale, Root, Chord

### Discovery Mode (M1 in KEY mode)
Chord engine activated by M1 button. 4 pages of parameters controlled by encoders 4-7:

**F1 — CHORD**: Quality (Maj/Min/Dim/Aug/Sus2/Sus4), Size (Tri/7th/9th/11th/13th), Extension (Off/b9/#9/#11/b13), Tension (Low/Med/High)

**F2 — COLOR**: Added (Off/2/4/6/13), Suspension (Off/Sus2/Sus4/2+4), Alteration (Off/b5/#5/b9/#9/#11), Spread (Tight/Med/Open/Wide)

**F3 — VOICING**: Inversion (Root/1st/2nd/3rd), Spacing (Close/Med/Open/Wide), Register (Low/LoMd/HiMd/High), Bass (Root/3rd/5th/7th)

**F4 — DISCOVERY**: Relation (Rt/3rd/5th/7th/Ext), Diatonic/Chromatic (Dia/Mix/Chr), Simple/Complex (Smp/Med/Cplx), Familiar/Unexp (Fam/Mix/Unx)

Press keyboard keys to play chords. Parameters modify chord voicing in real-time.

### Encoder Bank Mode (ENC hold)
Hold ENC button to enter. While held:
- Pads select encoder bank (1-16)
- Display shows real-time encoder values as vertical bars
- LEDs highlight selected bank

### Keyboard Bank Mode (KEY hold + pad)
Hold KEY button + touch any pad to enter. While held:
- Pads select keyboard bank (1-16)
- Changes octave for keyboard keys
- LEDs highlight selected bank

### Sequencer Mode (SEQ toggle) — TODO: not implemented
Toggle with SEQ button. Currently:
- Disables all MIDI I/O
- Turns off all LEDs
- Disables encoders
- Display shows "SEQ MODE" header
- **No actual sequencer functionality** (no step sequencer, pattern editing, or playback)

## Key Mode Settings

### Scales
| # | Name | # | Name |
|---|------|---|------|
| 0 | Chromatic | 7 | Harmonic Major |
| 1 | Major | 8 | Harmonic Minor |
| 2 | Minor (Natural) | 9 | Melodic Minor |
| 3 | Dorian | 10 | Phrygian |
| 4 | Mixolydian | 11 | Lydian |
| 5 | Pentatonic | 12 | Locrian |
| 6 | Blues | | |

### Root Notes
C, C#, D, D#, E, F, F#, G, G#, A, A#, B

### Octave Range
-2 to 8 (default: 3)

## Display Modes

- **Normal**: `K:<n> | E:<n>` header, pad/keyboard note ranges
- **Key Mode**: Oct, Scale, Root, Chord values in 4 columns
- **Discovery Mode**: `DISCOVERY M1/Fn` header, row 1 = Key params, row 2 = F-page params
- **Encoder Mode**: 8 vertical bars with values (0-127)
- **Sequencer Mode**: `SEQ MODE` header, blank body

## Gesture Support

The touch sensors support gestures via `MPR121_GestureHelper`:
- **Long Press**: Sends CC message for pads
- **Double Tap**: Sends note with velocity 100

## Dependencies

- [FastLED](https://github.com/FastLED/FastLED)
- [Control Surface](https://github.com/tttapa/Control-Surface)
- [U8g2](https://github.com/olikraus/u8g2) — Display library
- [Adafruit MPR121](https://github.com/adafruit/Adafruit_MPR121_Library)

## Building

This is an Arduino/PlatformIO project. Open `TeensikDiM.ino` in the Arduino IDE with Teensy board support, or use PlatformIO with the appropriate platform configuration.

## File Structure

```
TeensikDiM/
├── TeensikDiM.ino          # Main firmware
└── src/
    └── MPR121_GestureHelper.h/.cpp  # Touch sensor abstraction
```

## TODO

- [ ] **Sequencer Mode**: Implement actual step sequencer (pattern recording, editing, playback)
- [ ] Remove diagnostic serial prints in `checkEncoderChanges()` (line 1629)
- [ ] `disableKeyEncoders()` / `enableKeyEncoders()` — stub functions, currently empty
- [ ] Discovery Mode: relation parameter (F4 enc0) only handles root offset for 3rd/5th/7th/9th, other harmonics not implemented
- [ ] Discovery Mode: diatonic/chromatic, simple/complex, familiar/unexpected parameters (F4 enc1-3) not wired to chord engine
- [ ] Chord engine: spread parameter only duplicates root note up 1-2 octaves, not full voice spreading
- [ ] Chord engine: spacing parameter applies minimum interval but doesn't redistribute voices musically