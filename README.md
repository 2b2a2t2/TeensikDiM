# TeensikDiM - Touch MIDI Controller

A Teensy-based MIDI controller with capacitive touch sensors, rotary encoders, OLED display, and NeoPixel LEDs.

## Hardware

- **MCU**: Teensy (4.x recommended)
- **Touch**: 4x MPR121 capacitive sensors (I2C addresses 0x5A-0x5D) providing 48 touch channels
- **Display**: SSD1306 128x64 OLED via I2C (using U8g2 library)
- **LEDs**: 42 NeoPixel LEDs on pin 6
- **Encoders**: 8 rotary encoders with push buttons
- **MIDI**: USB MIDI + Serial2 MIDI (MIDI DIN output)

## MIDI Mapping

### Pads (Channel 1)
16 pads sending notes 59-74 (B3-E5). Velocity is fixed at 127.

### Keyboard (Channel 1+)
12 keys (C4-B4) sending notes 84-95. Channel changes with bank selection (Bank 1 = CH1, Bank 2 = CH2, etc.).

### Controls (Channel 1)
14 control buttons sending notes 75-100.

| Button | Note | Function |
|--------|------|----------|
| PREV | 75 | Previous |
| NEXT | 76 | Next |
| M1-M3 | 77-79 | Menu buttons |
| F1-F4 | 80-83 | Function buttons |
| ENC | 96 | **Mode button** - Hold to enter encoder bank mode |
| SEQ | 97 | **Sequencer mode toggle** - Toggles MIDI I/O on/off |
| KEY | 98 | **Mode button** - Tap for key settings, hold for keyboard bank mode |
| PLAY | 99 | Play |
| REC | 100 | Record |

### Encoders (Channel 1)
8 encoders sending CC values 0-127. Bank switching changes which CC numbers are active.

## Operating Modes

### Normal Mode (Default)
- Pads send MIDI notes on CH1
- Keyboard sends MIDI notes on CH1+bank
- Encoders send CC values
- Display shows note ranges and bank info

### Sequencer Mode (SEQ button)
Toggle with SEQ button. When active:
- **All MIDI I/O disabled** - no notes or CC sent
- All LEDs turn off
- Encoders are disabled
- Display shows "SEQ MODE" header
- Press SEQ again to exit

### Key Mode (KEY button tap)
Tap KEY button to toggle. When active:
- Keyboard keys send notes with scale/root transposition
- 4 encoders repurposed for key settings:
  - **ENC0**: Octave (-2 to 8)
  - **ENC1**: Scale selection (Chromatic, Major, Minor, Dorian, etc.)
  - **ENC2**: Root note (C through B)
  - **ENC3**: Chord on/off toggle
- Display shows current octave, scale, root, and chord status
- Pads still send MIDI normally

### Encoder Bank Mode (ENC button hold)
Hold ENC button to enter. While held:
- Pads select encoder bank (1-16)
- Display shows real-time encoder values as vertical bars
- LEDs highlight selected bank

### Keyboard Bank Mode (KEY button hold)
Hold KEY button + touch any pad to enter. While held:
- Pads select keyboard bank (1-16)
- Changes MIDI channel for keyboard keys
- LEDs highlight selected bank

## Key Mode Settings

### Scales
| # | Name | # | Name |
|---|------|---|------|
| 0 | Chromatic | 7 | Harmonic Major |
| 1 | Major | 8 | Harmonic Minor |
| 2 | Minor | 9 | Melodic Minor |
| 3 | Dorian | 10 | Phrygian |
| 4 | Mixolydian | 11 | Lydian |
| 5 | Pentatonic | 12 | Locrian |
| 6 | Blues | | |

### Root Notes
C, C#, D, D#, E, F, F#, G, G#, A, A#, B

### Octave Range
-2 to 8 (default: 3)

## Display Modes

- **Normal**: Pads: Notes 59-74 (CH1), Keys: Notes 84-95 (CH1)
- **Key Mode**: Shows Oct, Scale, Root, Chord values
- **Encoder Mode**: 8 vertical bars with values (0-127)
- **Sequencer Mode**: "SEQ MODE" header, blank body
- **Bank Mode**: Shows Keys Bank, Enc Bank, and active mode

## Gesture Support

The touch sensors support gestures via `MPR121_GestureHelper`:
- **Long Press**: Sends CC message for pads (disabled in sequencer mode)
- **Double Tap**: Sends note with velocity 100 (disabled in sequencer mode)

## Dependencies

- [FastLED](https://github.com/FastLED/FastLED)
- [Control Surface](https://github.com/tttapa/Control-Surface)
- [U8g2](https://github.com/olikraus/u8g2) - Display library
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