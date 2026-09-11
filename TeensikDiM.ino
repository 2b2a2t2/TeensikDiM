#include <FastLED.h>
#include <Control_Surface.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "src/MPR121_GestureHelper.h"

USING_CS_NAMESPACE;

// ============================================================
// CHORD MODEL TYPES (defined early for Arduino preprocessor)
// ============================================================

// ChordRole — semantic identity of each chord tone
enum ChordRole : uint8_t {
  ROLE_ROOT,
  ROLE_THIRD,
  ROLE_FIFTH,
  ROLE_SEVENTH,
  ROLE_NINTH,
  ROLE_ELEVENTH,
  ROLE_THIRTEENTH,
  ROLE_ADDED,
  ROLE_COUNT
};

// ChordTone — a single tone within a chord structure
struct ChordTone {
  ChordRole role;
  uint8_t pitchClass;    // 0-11, semitones above chord root
  uint8_t octaveOffset;  // 0 = root octave, 1 = +12, 2 = +24
  bool present;          // active in the chord
  bool altered;          // Alteration has modified pitchClass
};

// ChordStructure — complete musical representation of a chord
struct ChordStructure {
  ChordTone tones[ROLE_COUNT];  // indexed by ChordRole
  uint8_t toneCount;            // count of present tones
  uint8_t rootMIDI;             // absolute MIDI note of chord root
  uint8_t rootPitchClass;       // 0-11, pitch class of chord root
};

// Scale info for Discovery
struct ScaleInfo {
  uint8_t pitchClasses[12];  // pitch classes in scale, sorted ascending
  uint8_t count;             // number of pitch classes
};

// (DiscoveryCandidate removed — not needed in chord generator mode)

// ==================== BANKABLE KEYBOARD ====================
// Custom MIDI output element for MPR121 touch → MIDI Note with bank support

BEGIN_CS_NAMESPACE

class BankableTouchNote : public MIDIOutputElement {
public:
  BankableTouchNote(OutputBankConfig<> config, MIDIAddress address,
                    uint8_t velocity = 100)
    : address(config, address), velocity(velocity), isNoteOn(false) {}

  void begin() final override {}
  void update() final override {
    // Touch state is set externally before Control_Surface.loop()
    bool touched = currentTouch;
    if (touched && !isNoteOn) {
      address.lock();
      Control_Surface.sendNoteOn(address.getActiveAddress(), velocity);
      isNoteOn = true;
    } else if (!touched && isNoteOn) {
      Control_Surface.sendNoteOff(address.getActiveAddress(), velocity);
      address.unlock();
      isNoteOn = false;
    }
  }

  void setTouchState(bool touched) { currentTouch = touched; }

private:
  Bankable::SingleAddress address;
  uint8_t velocity;
  bool isNoteOn;
  bool currentTouch = false;
};

END_CS_NAMESPACE

// Keyboard bank: 11 octaves (-2 to 8), 12 semitones per octave
// selectionOffset=-4 so select(5) (octave 3) maps to offset +12
// and select(0) (octave -2) maps to offset -48
Bank<11> keyboardBank(12, 5, -4);

// 12 keyboard notes (C3 to B3 = notes 48-59 as base)
MIDIAddress keyAddresses[12] = {
  {48, Channel_1}, {49, Channel_1}, {50, Channel_1}, {51, Channel_1},
  {52, Channel_1}, {53, Channel_1}, {54, Channel_1}, {55, Channel_1},
  {56, Channel_1}, {57, Channel_1}, {58, Channel_1}, {59, Channel_1},
};

BankableTouchNote keyNotes[12] = {
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[0]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[1]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[2]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[3]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[4]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[5]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[6]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[7]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[8]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[9]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[10]},
  {{keyboardBank, BankType::ChangeAddress}, keyAddresses[11]},
};

enum ButtonType {
  TYPE_PAD,
  TYPE_CONTROL,
  TYPE_KEYBOARD
};

struct ButtonMap {
  uint8_t sensor;
  uint8_t channel;
  const char* name;
  ButtonType type;
  uint8_t midiNote;
  bool isPressed;
  unsigned long pressTime;
  bool isModeButton;
};

// ============================================
// LED and MIDI Configuration
// ============================================

USBMIDI_Interface midi;
HardwareSerialMIDI_Interface serialmidi2{ Serial2, MIDI_BAUD };
BidirectionalMIDI_PipeFactory<2> pipes;

// LED Array
Array<CRGB, 42> leds{};
constexpr uint8_t ledpin = 6;

// LED mapping - matches your layout
const byte ledMapping[42] = {
  // Pad LEDs (0-15)
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  // Nav LEDs (16-17)
  16, 17,
  // Menu LEDs (18-20)
  18, 19, 20,
  // Func LEDs (21-24)
  21, 22, 23, 24,
  // Keyboard
  25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31,  
  32, 33, 34, 35, 36
};

// ==================== BANKS CONFIGURATION ====================
Bank<16> bankKeys(1);      // Keyboard bank
Bank<16> bankEnc(1);       // Encoder bank

// ==================== BANK MODE DEFINITIONS ====================
enum BankMode {
  BANK_NONE,
  BANK_KEYS,
  BANK_ENC
};

BankMode currentBankMode = BANK_NONE;
bool modeButtonHeld = false;
BankMode lastActiveMode = BANK_NONE;
bool seqModeActive = false;
bool keyModeActive = false;
bool keyPadTouched = false;

// Key mode settings
int8_t keyOctave = 3;
uint8_t keyScale = 0;   // 0=Chromatic
uint8_t keyRoot = 0;     // 0=C
bool keyChordOn = false;
bool keyButtonHeld = false;
bool prevButtonHeld = false;
bool keyPlayingChord = false;
bool keyLockOn = false;
bool perfOn = false;
uint16_t keyboardHeldMask = 0;  // bitmask of currently held keyboard keys (bit 0=key0, etc.)
uint8_t midiChannel = 1;        // MIDI channel (1-16) for display

const char* scaleNames[] = {
  "Chrom", "Major", "Minor", "Dorian", "Mixolyd",
  "Pent", "Blues", "HarmMj", "HarmMn", "MelMin",
  "Phryg", "Lydian", "Locrian"
};
const uint8_t numScales = sizeof(scaleNames) / sizeof(scaleNames[0]);
const char* rootNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// Scale patterns as 12-bit masks (bit 0=C, bit 1=C#, ..., bit 11=B)
// Chromatic includes all 12 notes
const uint16_t scalePatterns[] = {
  0b111111111111, // Chrom (all)
  0b101010110101, // Major (W W H W W W H)
  0b101101011010, // Minor (natural)
  0b101101010110, // Dorian
  0b101011010110, // Mixolydian
  0b101001011001, // Pentatonic major
  0b101001011010, // Blues (hexatonic)
  0b101010110101, // HarmMj (major with b7)
  0b101101011010, // HarmMn (same as natural minor)
  0b101101010110, // MelMin (ascending)
  0b101101010110, // Phrygian
  0b101011011010, // Lydian
  0b101101101010, // Locrian
};

// Keyboard LED indices in ledMapping for notes C..B (indices 25-36)
// Physical LED positions: 25,37,26,38,27,28,39,29,40,30,41,31
const uint8_t keyLedIndex[12] = {
  25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31
};

void updateKeyModeLEDs() {
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
  for (int i = 0; i < 12; i++) {
    if (rotated & (1 << i)) {
      leds[keyLedIndex[i]] = CRGB(0, 0, 18); // dim blue
    } else {
      leds[keyLedIndex[i]] = CRGB::Black;
    }
  }
  FastLED.show();
}

// ==================== CHORD GENERATOR MODE ====================

// Chord type indices (M1=0, M2=1, M3=2, NEXT=3)
#define CHORD_TYPE_NONE  0
#define CHORD_TYPE_MAJ   1
#define CHORD_TYPE_MIN   2
#define CHORD_TYPE_DIM   3
#define CHORD_TYPE_SUS   4
#define CHORD_TYPE_COUNT 5

// Extension bitmask flags (F1=bit0, F2=bit1, F3=bit2, F4=bit3)
#define EXT_6     0x01
#define EXT_m7    0x02
#define EXT_M7    0x04
#define EXT_9     0x08

// Chord type quality mapping: index → quality for qualityIntervals
const uint8_t chordTypeQuality[] = { 0, 0, 1, 2, 5 }; // None=Maj, Maj=0, Min=1, Dim=2, Sus=5(Sus4)

// Chord type names for display
const char* chordTypeNames[] = { "---", "Maj", "Min", "Dim", "Sus" };

// Extension names for display
const char* extNames[] = { "m7", "7", "add9", "m9" };

// Voicing encoder names for display
const char* voicingNames[] = { "Voic", "Bass", "Sprd", "Dens" };

// Chord generator state
struct ChordGenState {
  bool active;              // chord mode enabled (ENC3 ON)
  uint8_t chordType;        // CHORD_TYPE_NONE..CHORD_TYPE_SUS
  uint8_t extensions;       // bitmask of EXT_* flags
  uint8_t voicingIndex;     // ENC4: inversion index (0=root, 1=1st, 2=2nd, 3=3rd)
  uint8_t bassVoicing;      // ENC5: bass note position (0=root, 1=3rd, 2=5th, 3=7th)
  uint8_t spread;           // ENC6: 0-3
  uint8_t density;          // ENC7: 0-3
  int8_t noteRoot;          // current root MIDI note, -1 if none
  int8_t heldKeyIndex;      // physical key held, -1 if none
  uint8_t heldCount;        // number of keys held
};

ChordGenState chordGen = {
  false, CHORD_TYPE_NONE, 0,
  0, 0, 0, 0,
  -1, -1, 0
};

uint8_t chordNotesPlaying[8] = {};
uint8_t chordNoteCount = 0;

// Build chord name string from chordGen state (e.g. "GMaj69")
void buildChordName(char* buf, size_t len) {
  buf[0] = '\0';
  if (chordGen.noteRoot < 0) return;

  const char* root = rootNames[chordGen.noteRoot % 12];
  const char* type = chordTypeNames[chordGen.chordType];
  if (chordGen.chordType == CHORD_TYPE_NONE) type = "";

  // Extensions
  char ext[12] = "";
  if (chordGen.extensions & EXT_6)  strcat(ext, "6");
  if (chordGen.extensions & EXT_m7) strcat(ext, "m7");
  if (chordGen.extensions & EXT_M7) strcat(ext, "M7");
  if (chordGen.extensions & EXT_9)  strcat(ext, "9");

  snprintf(buf, len, "%s%s%s", root, type, ext);
}

// ==================== CHORD ENGINE ====================

// Quality triad intervals
const uint8_t qualityIntervals[][3] = {
  {0, 4, 7},  // Major
  {0, 3, 7},  // Minor
  {0, 3, 6},  // Diminished
  {0, 4, 8},  // Augmented
  {0, 2, 7},  // Sus2
  {0, 5, 7},  // Sus4
};

// Which scale degree index to add for each size (above the triad)
// Index 0 = scale degree 7, index 1 = degree 9, etc.
const uint8_t sizeCount[] = { 0, 1, 2, 3, 4 };

// ============================================================
// SCALE UTILITIES
// ============================================================

// Build ScaleInfo from current keyRoot and keyScale
ScaleInfo buildScaleInfo() {
  ScaleInfo info;
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
  info.count = 0;
  for (uint8_t i = 0; i < 12; i++) {
    if (rotated & (1 << i)) {
      info.pitchClasses[info.count++] = i;
    }
  }
  return info;
}

// Get rotated scale bitmask
uint16_t getRotatedScale() {
  uint16_t pattern = scalePatterns[keyScale];
  return ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
}

// Find scale degree of a pitch class (-1 if not in scale)
int8_t getScaleDegree(uint8_t pitchClass, const ScaleInfo &scale) {
  for (uint8_t i = 0; i < scale.count; i++) {
    if (scale.pitchClasses[i] == pitchClass) return i;
  }
  return -1;
}

// ============================================================
// CHORD GENERATOR — PLAY / STOP
// ============================================================

void stopChord() {
  for (uint8_t i = 0; i < chordNoteCount; i++) {
    Control_Surface.sendNoteOff({chordNotesPlaying[i], Channel::createChannel(midiChannel)}, 0);
  }
  chordNoteCount = 0;
}

void playChord(int8_t midiNote) {
  stopChord();
  if (midiNote < 0) return;

  uint8_t quality = chordTypeQuality[chordGen.chordType];

  // Clamp chord root
  int8_t chordRoot = constrain(midiNote, 36, 84);

  // Build chord notes
  uint8_t notes[12];
  uint8_t noteCount = 0;

  // 1. Triad from quality intervals
  for (int i = 0; i < 3; i++) {
    notes[noteCount++] = chordRoot + qualityIntervals[quality][i];
  }

  // 2. Add extensions from F buttons (m7=10st, 7=10st, add9=14st, m9=13st)
  if (chordGen.extensions & EXT_6)  notes[noteCount++] = chordRoot + 9;
  if (chordGen.extensions & EXT_m7) notes[noteCount++] = chordRoot + 10;
  if (chordGen.extensions & EXT_M7) notes[noteCount++] = chordRoot + 11;
  if (chordGen.extensions & EXT_9)  notes[noteCount++] = chordRoot + 14;

  // 3. Apply voicing (inversion): move lowest note up an octave
  uint8_t inv = chordGen.voicingIndex % noteCount;
  if (inv > 0 && noteCount > 1) {
    uint8_t topNote = notes[inv];
    for (uint8_t i = inv; i < noteCount - 1; i++) {
      notes[i] = notes[i + 1];
    }
    notes[noteCount - 1] = topNote + 12;
  }

  // 4. Apply bass voicing: reorder bass note
  uint8_t bassIdx = chordGen.bassVoicing % noteCount;
  if (bassIdx > 0 && bassIdx < noteCount) {
    uint8_t bassNote = notes[bassIdx];
    for (uint8_t i = bassIdx; i > 0; i--) {
      notes[i] = notes[i - 1];
    }
    notes[0] = bassNote;
  }

  // 5. Apply spread (octave doublings)
  if (chordGen.spread >= 1 && noteCount > 0 && noteCount < 8) {
    notes[noteCount++] = notes[0] + 12;
  }
  if (chordGen.spread >= 3 && noteCount > 0 && noteCount < 8) {
    notes[noteCount++] = notes[0] + 24;
  }

  // 6. Apply density: remove 5th (index 2) if density >= 1, remove 7th if >= 2
  if (chordGen.density >= 1 && noteCount > 2) {
    for (uint8_t i = 2; i < noteCount - 1; i++) {
      notes[i] = notes[i + 1];
    }
    noteCount--;
  }
  if (chordGen.density >= 2 && noteCount > 3) {
    for (uint8_t i = 3; i < noteCount - 1; i++) {
      notes[i] = notes[i + 1];
    }
    noteCount--;
  }

  // Sort notes ascending
  for (uint8_t i = 0; i < noteCount - 1; i++) {
    for (uint8_t j = i + 1; j < noteCount; j++) {
      if (notes[j] < notes[i]) {
        uint8_t tmp = notes[i];
        notes[i] = notes[j];
        notes[j] = tmp;
      }
    }
  }

  // Clamp to MIDI range and remove duplicates
  uint8_t unique[12];
  uint8_t uniqueCount = 0;
  for (uint8_t i = 0; i < noteCount; i++) {
    uint8_t n = constrain(notes[i], 36, 96);
    bool dup = false;
    for (uint8_t j = 0; j < uniqueCount; j++) {
      if (unique[j] == n) { dup = true; break; }
    }
    if (!dup) unique[uniqueCount++] = n;
  }

  // Send MIDI notes
  chordNoteCount = 0;
  for (uint8_t i = 0; i < uniqueCount && i < 8; i++) {
    Control_Surface.sendNoteOn({unique[i], Channel::createChannel(midiChannel)}, 100);
    chordNotesPlaying[chordNoteCount++] = unique[i];
  }

  // Light keyboard LEDs for played notes
  updateKeyModeLEDs();
  for (uint8_t i = 0; i < chordNoteCount; i++) {
    uint8_t pc = chordNotesPlaying[i] % 12;
    leds[keyLedIndex[pc]] = CRGB(0, 40, 80);
  }
  FastLED.show();
}

// ==================== MIDI LED CONTROLLER ====================
template<uint8_t RangeLen>
class CustomNoteLED : public MatchingMIDIInputElement<MIDIMessageType::NoteOn,
                                                      TwoByteRangeMIDIMatcher> {
public:
  CustomNoteLED(CRGB *ledcolors, const uint8_t *ledIndexMap, MIDIAddress address)
    : MatchingMIDIInputElement<MIDIMessageType::NoteOn,
                               TwoByteRangeMIDIMatcher>({ address, RangeLen }),
      ledcolors(ledcolors), ledIndexMap(ledIndexMap) {}

  void begin() override {}

  void handleUpdate(typename TwoByteRangeMIDIMatcher::Result match) override {
    if (chordGen.active) return;
    updateLED(match.index, match.value);
  }

  void updateLED(uint8_t index, uint8_t velocity) {
    if (index >= totalNotes) return;

    uint8_t ledIndex = ledIndexMap[index];
    if (velocity > 0) {
      uint8_t hue = map(velocity, 0, 127, 0, 255);
      ledcolors[ledIndex] = CHSV(hue, 255, 255);
    } else {
      ledcolors[ledIndex] = CRGB::Black;
    }
    dirty = true;
  }

  bool getDirty() const { return dirty; }
  void clearDirty() { dirty = false; }

private:
  CRGB *ledcolors;
  const uint8_t *ledIndexMap;
  bool dirty = false;
  static const uint8_t totalNotes = 42;
};

CustomNoteLED<42> midiled{ leds.data, ledMapping, MIDI_Notes::B[3] };

// ==================== ENCODERS ====================

BEGIN_CS_NAMESPACE
namespace Bankable {
template <uint8_t NumBanks>
struct BorrowedCCAbsoluteEncoder
    : BorrowedMIDIAbsoluteEncoder<NumBanks, SingleAddress, ContinuousCCSender> {
  BorrowedCCAbsoluteEncoder(BankConfig<NumBanks> config, AHEncoder &encoder,
                            MIDIAddress address, int16_t speedMultiply = 1,
                            uint8_t pulsesPerStep = 4)
      : BorrowedMIDIAbsoluteEncoder<NumBanks, SingleAddress, ContinuousCCSender>{
            {config, address}, encoder, speedMultiply, pulsesPerStep, {}} {}
};
} // namespace Bankable
END_CS_NAMESPACE

// Shared AHEncoder objects (one per physical encoder)
AHEncoder enc0_hw{40, 39};
AHEncoder enc1_hw{36, 35};
AHEncoder enc2_hw{34, 33};
AHEncoder enc3_hw{31, 32};
AHEncoder enc4_hw{38, 37};
AHEncoder enc5_hw{26, 25};
AHEncoder enc6_hw{27, 28};
AHEncoder enc7_hw{29, 30};

// MIDI CC encoders (borrowed - share AHEncoder, no MIDI in KEY mode)
Bankable::BorrowedCCAbsoluteEncoder<16> enc0{ { bankEnc, BankType::ChangeChannel }, enc0_hw, { 74, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc1{ { bankEnc, BankType::ChangeChannel }, enc1_hw, { 71, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc2{ { bankEnc, BankType::ChangeChannel }, enc2_hw, { 75, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc3{ { bankEnc, BankType::ChangeChannel }, enc3_hw, { 76, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc4{ { bankEnc, BankType::ChangeChannel }, enc4_hw, { 91, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc5{ { bankEnc, BankType::ChangeChannel }, enc5_hw, { 92, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc6{ { bankEnc, BankType::ChangeChannel }, enc6_hw, { 94, Channel_1 }, 7 };
Bankable::BorrowedCCAbsoluteEncoder<16> enc7{ { bankEnc, BankType::ChangeChannel }, enc7_hw, { 7, Channel_1 }, 7 };

// Track last encoder values for display updates
uint16_t lastEncoderValues[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
bool encoderValueChanged = false;

// KEY mode encoder reading via raw pins (no MIDI)
// enc0=Octave, enc1=Scale, enc2=Root, enc3=Chord, enc4–enc7=additional KEY functions
const uint8_t keyEncPins[8][2] = { {40,39}, {36,35}, {34,33}, {31,32}, {38,37}, {26,25}, {27,28}, {29,30} };
uint8_t keyEncState[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
int8_t keyEncAccum[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
const int8_t keyEncSensitivity = 4; // apply 1 step per N transitions (higher = less sensitive)

// Save/restore AHEncoder positions to prevent CC jumps on mode transitions
int32_t savedEncPositions[8];

void saveKeyEncPositions() {
  savedEncPositions[0] = enc0_hw.read();
  savedEncPositions[1] = enc1_hw.read();
  savedEncPositions[2] = enc2_hw.read();
  savedEncPositions[3] = enc3_hw.read();
  savedEncPositions[4] = enc4_hw.read();
  savedEncPositions[5] = enc5_hw.read();
  savedEncPositions[6] = enc6_hw.read();
  savedEncPositions[7] = enc7_hw.read();
}

void restoreKeyEncPositions() {
  enc0_hw.write(savedEncPositions[0]);
  enc1_hw.write(savedEncPositions[1]);
  enc2_hw.write(savedEncPositions[2]);
  enc3_hw.write(savedEncPositions[3]);
  enc4_hw.write(savedEncPositions[4]);
  enc5_hw.write(savedEncPositions[5]);
  enc6_hw.write(savedEncPositions[6]);
  enc7_hw.write(savedEncPositions[7]);
}

void syncKeyEncodersToParams() {
  for (int i = 0; i < 8; i++)
    keyEncState[i] = (digitalRead(keyEncPins[i][0]) << 1) | digitalRead(keyEncPins[i][1]);
}

void readENC3Always() {
  // When PREV held in chord mode: read ENC0-5 (overlay params)
  // When chord mode active without PREV: read ENC3-7 (chord toggle + voicing/bass/spread/density)
  // Otherwise: only read ENC3 (chord mode toggle from key mode)
  int endIdx = (prevButtonHeld && chordGen.active) ? 5 : 7;
  int startIdx = (prevButtonHeld && chordGen.active) ? 0 : 3;

  for (int i = startIdx; i <= endIdx; i++) {
    uint8_t newState = (digitalRead(keyEncPins[i][0]) << 1) | digitalRead(keyEncPins[i][1]);
    uint8_t transition = (keyEncState[i] << 2) | newState;
    keyEncState[i] = newState;
    static const int8_t encoderTable[16] = { 0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0 };
    int8_t delta = -encoderTable[transition];
    if (delta == 0) continue;

    if (prevButtonHeld && chordGen.active) {
      // PREV overlay: ENC0-2=oct/scale/root, ENC3-5=latch/klock/perf
      if (i == 0) {
        keyOctave = constrain(keyOctave + ((delta > 0) ? 1 : -1), -2, 8);
        updateKeyModeLEDs();
        Serial.print("  -> Octave: ");
        Serial.println(keyOctave);
      } else if (i == 1) {
        keyScale = constrain(keyScale + ((delta > 0) ? 1 : -1), 0, numScales - 1);
        updateKeyModeLEDs();
        Serial.print("  -> Scale: ");
        Serial.println(scaleNames[keyScale]);
      } else if (i == 2) {
        keyRoot = constrain(keyRoot + ((delta > 0) ? 1 : -1), 0, 11);
        updateKeyModeLEDs();
        Serial.print("  -> Root: ");
        Serial.println(rootNames[keyRoot]);
      } else if (i == 3) {
        keyChordOn = (delta > 0);
        Serial.print("  -> Latch: ");
        Serial.println(keyChordOn ? "ON" : "OFF");
      } else if (i == 4) {
        keyLockOn = (delta > 0);
        Serial.print("  -> KeyLock: ");
        Serial.println(keyLockOn ? "ON" : "OFF");
      } else if (i == 5) {
        perfOn = (delta > 0);
        Serial.print("  -> Perf: ");
        Serial.println(perfOn ? "ON" : "OFF");
      }
    } else if (i == 3 && keyModeActive) {
      // ENC3 in key mode: toggle chord mode
      if (delta > 0) {
        chordGen.active = true;
      } else if (delta < 0) {
        chordGen.active = false;
        stopChord();
        chordGen.noteRoot = -1;
        chordGen.heldKeyIndex = -1;
        chordGen.heldCount = 0;
        keyboardHeldMask = 0;
        updateKeyModeLEDs();
      }
    } else if (chordGen.active && i >= 4) {
      // ENC4-7 in chord mode: voicing/bass/spread/density
      if (i == 4) {
        chordGen.voicingIndex = constrain(chordGen.voicingIndex + ((delta > 0) ? 1 : -1), 0, 3);
        if (chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        Serial.print("  -> Voicing: ");
        Serial.println(chordGen.voicingIndex);
      } else if (i == 5) {
        chordGen.bassVoicing = constrain(chordGen.bassVoicing + ((delta > 0) ? 1 : -1), 0, 3);
        if (chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        Serial.print("  -> Bass: ");
        Serial.println(chordGen.bassVoicing);
      } else if (i == 6) {
        chordGen.spread = constrain(chordGen.spread + ((delta > 0) ? 1 : -1), 0, 3);
        if (chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        Serial.print("  -> Spread: ");
        Serial.println(chordGen.spread);
      } else if (i == 7) {
        chordGen.density = constrain(chordGen.density + ((delta > 0) ? 1 : -1), 0, 3);
        if (chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        Serial.print("  -> Density: ");
        Serial.println(chordGen.density);
      }
    }
  }
}

void readKeyModeEncoders() {
  for (int i = 0; i < 8; i++) {
    uint8_t newState = (digitalRead(keyEncPins[i][0]) << 1) | digitalRead(keyEncPins[i][1]);
    uint8_t transition = (keyEncState[i] << 2) | newState;
    keyEncState[i] = newState;
    static const int8_t encoderTable[16] = { 0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0 };
    int8_t delta = -encoderTable[transition]; // negated for correct direction
    if (delta == 0) continue;
    if (i == 3) {
      // ENC3 handled by readENC3Always
      continue;
    }
    keyEncAccum[i] += delta;
    if (abs(keyEncAccum[i]) < keyEncSensitivity) continue;
    int8_t step = (keyEncAccum[i] > 0) ? 1 : -1;
    keyEncAccum[i] = 0;
    switch (i) {
      case 0: {
        keyOctave = constrain(keyOctave + step, -2, 8);
        keyboardBank.select(keyOctave + 2);
        if (chordGen.active && chordGen.heldKeyIndex >= 0) {
          stopChord();
          int8_t newNote = 48 + chordGen.heldKeyIndex + (keyOctave - 3) * 12;
          chordGen.noteRoot = newNote;
          playChord(newNote);
        }
        break;
      }
      case 1: {
        keyScale = constrain(keyScale + step, 0, numScales - 1);
        updateKeyModeLEDs();
        if (chordGen.active && chordGen.heldKeyIndex >= 0) {
          stopChord();
          playChord(chordGen.noteRoot);
        }
        break;
      }
      case 2: {
        keyRoot = constrain(keyRoot + step, 0, 11);
        updateKeyModeLEDs();
        if (chordGen.active && chordGen.heldKeyIndex >= 0) {
          stopChord();
          playChord(chordGen.noteRoot);
        }
        break;
      }
      case 4: {
        chordGen.voicingIndex = constrain(chordGen.voicingIndex + step, 0, 3);
        if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        break;
      }
      case 5: {
        chordGen.bassVoicing = constrain(chordGen.bassVoicing + step, 0, 3);
        if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        break;
      }
      case 6: {
        chordGen.spread = constrain(chordGen.spread + step, 0, 3);
        if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        break;
      }
      case 7: {
        chordGen.density = constrain(chordGen.density + step, 0, 3);
        if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
        break;
      }
    }
  }
}

void disableKeyEncoders() {}
void enableKeyEncoders() {}

void disableAllEncoders() {
  enc0.disable(); enc1.disable(); enc2.disable(); enc3.disable();
  enc4.disable(); enc5.disable(); enc6.disable(); enc7.disable();
}

void enableAllEncoders() {
  enc0.enable(); enc1.enable(); enc2.enable(); enc3.enable();
  enc4.enable(); enc5.enable(); enc6.enable(); enc7.enable();
}

// ==================== DISPLAY SETUP ====================
// U8G2 for all display rendering
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

class MyDisplay : public DisplayInterface {
public:
  void begin() override {
    u8g2.begin();
    u8g2.setFont(u8g2_font_5x7_tr);
  }

  void clear() override { u8g2.clearBuffer(); }
  void display() override { u8g2.sendBuffer(); }
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (color) u8g2.drawPixel(x, y);
  }
  void setTextColor(uint16_t) override {}
  void setTextSize(uint8_t) override {}
  void setCursor(int16_t x, int16_t y) override { u8g2.setCursor(x, y); }
  size_t write(uint8_t c) override { return u8g2.write(c); }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t) override {
    u8g2.drawLine(x0, y0, x1, y1);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t) override {
    u8g2.drawVLine(x, y, h);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t) override {
    u8g2.drawHLine(x, y, w);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t) override {
    u8g2.drawBox(x, y, w, h);
  }
  void drawXBitmap(int16_t, int16_t, const uint8_t*, int16_t, int16_t, uint16_t) override {}

  void drawHeader() {
    if (seqModeActive) {
      u8g2.setCursor(42, 12);
      u8g2.print("SEQ MODE");
      return;
    }
    if (chordGen.active) {
      // Single line: #Ch CHORD MODE [chord name]  ^ (latch)
      u8g2.setCursor(0, 10);
      u8g2.print("#");
      u8g2.print(midiChannel);
      u8g2.print(" CHORD MODE ");
      char chordName[24];
      buildChordName(chordName, sizeof(chordName));
      if (strlen(chordName) > 0) {
        u8g2.print(chordName);
      }
      if (keyChordOn || prevButtonHeld) {
        u8g2.setCursor(121, 10);
        u8g2.print("^");
      }
      return;
    }
    if (keyModeActive) {
      u8g2.setCursor(0, 10);
      u8g2.print("#");
      u8g2.print(midiChannel);
      u8g2.print(" KEY MODE ");
      char chordName[24];
      buildChordName(chordName, sizeof(chordName));
      if (strlen(chordName) > 0) {
        u8g2.print(chordName);
      }
      return;
    }

    u8g2.setCursor(0, 12);
    u8g2.print("K:");
    u8g2.setCursor(24, 12);
    u8g2.print(bankKeys.getSelection() + 1);

    u8g2.drawBox(46, 8, 3, 3);
    u8g2.drawVLine(50, 5, 8);

    u8g2.setCursor(54, 12);
    u8g2.print("E:");
    u8g2.setCursor(78, 12);
    u8g2.print(bankEnc.getSelection() + 1);
  }

  void drawBackground() override {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();
  }

  uint16_t getEncoderValue(uint8_t index) {
    if (index < 8) return lastEncoderValues[index];
    return 0;
  }

  void displayEncLabels(bool forceFullRefresh = false) {
    static uint16_t lastDisplayValues[8] = { 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535 };
    static uint8_t lastBank = 255;
    uint8_t currentBank = bankEnc.getSelection();

    uint16_t currentValues[8];
    for (int i = 0; i < 8; i++)
      currentValues[i] = getEncoderValue(i);

    bool needsUpdate = forceFullRefresh || (currentBank != lastBank);
    for (int i = 0; i < 8; i++) {
      if (currentValues[i] != lastDisplayValues[i]) {
        needsUpdate = true;
        lastDisplayValues[i] = currentValues[i];
      }
    }
    if (!needsUpdate) return;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();
    lastBank = currentBank;

    for (int i = 0; i < 8; i++) {
      int x = (i % 4) * 32;
      int y = (i < 4) ? 22 : 44;
      const uint8_t potH = 14;

      u8g2.drawFrame(x, y, 10, 16);
      uint8_t fillH = map(currentValues[i], 0, 127, 0, potH);
      u8g2.drawBox(x + 1, y + 1 + (potH - fillH), 8, fillH);

      u8g2.setCursor(x + 12, y + 5);
      u8g2.print(i + 1);
      u8g2.setCursor(x + 12, y + 13);
      u8g2.print(currentValues[i]);
    }

    u8g2.sendBuffer();
  }

  void displayBankLabels() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();

    u8g2.setCursor(0, 22);
    u8g2.print("Keys Bank: ");
    u8g2.print(bankKeys.getSelection() + 1);

    u8g2.setCursor(0, 32);
    u8g2.print("Enc Bank: ");
    u8g2.print(bankEnc.getSelection() + 1);

    u8g2.setCursor(0, 42);
    u8g2.print("Mode: ");
    switch (currentBankMode) {
      case BANK_KEYS: u8g2.print("Keys"); break;
      case BANK_ENC:  u8g2.print("Enc"); break;
      case BANK_NONE: u8g2.print("Normal"); break;
    }

    u8g2.sendBuffer();
  }

  void displayNormalMode() {
    if (lastActiveMode == BANK_ENC) {
      displayEncLabels(true);
      return;
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();

    u8g2.setCursor(0, 22);
    u8g2.print("Pads: Notes 59-74 (CH1)");
    u8g2.setCursor(0, 32);
    u8g2.print("Keys: Notes 84-95 (CH1)");
    u8g2.setCursor(0, 42);
    u8g2.print("Hold KEY/ENC for banks");

    u8g2.sendBuffer();
  }

  void displaySequencerMode() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();
    u8g2.sendBuffer();
  }

  void displayKeyMode() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();

    const char* labels[] = { "Oct", "Scale", "Root", chordGen.active ? "Latch" : "---" };
    const char* values[] = {
      nullptr,
      scaleNames[keyScale],
      rootNames[keyRoot],
      keyChordOn ? "ON" : "OFF"
    };
    char octBuf[5];
    snprintf(octBuf, sizeof(octBuf), "%d", keyOctave);
    values[0] = octBuf;

    const int colX[] = { 2, 26, 60, 90 };
    const int colW[] = { 18, 30, 24, 30 };
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 24);
      u8g2.print(labels[i]);
      int valW = u8g2.getStrWidth(values[i]);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 33);
      u8g2.print(values[i]);
    }

    u8g2.sendBuffer();
  }

  void displayChordMode() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();

    const int colX[] = { 2, 34, 66, 98 };
    const int colW[] = { 28, 28, 28, 28 };

    // Row 1: Chord type buttons (M1 Maj, M2 Min, M3 Dim, NEXT Sus) — shifted down for 2-line header
    const char* typeLabels[] = { "Maj", "Min", "Dim", "Sus" };
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 24);
      u8g2.print(typeLabels[i]);
      // Highlight active chord type
      if (chordGen.chordType == i + 1) {
        u8g2.drawBox(colX[i], 27, u8g2.getStrWidth(typeLabels[i]), 2);
      }
    }

    // Row 2: Extension buttons (F1 6, F2 m7, F3 M7, F4 9)
    const char* extLabels[] = { "6", "m7", "M7", "9" };
    uint8_t extFlags[] = { EXT_6, EXT_m7, EXT_M7, EXT_9 };
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 38);
      u8g2.print(extLabels[i]);
      if (chordGen.extensions & extFlags[i]) {
        u8g2.drawBox(colX[i], 41, u8g2.getStrWidth(extLabels[i]), 2);
      }
    }

    // Row 3: Voicing params (ENC4 Voic, ENC5 Bass, ENC6 Sprd, ENC7 Dens)
    const char* voicLabels[] = { "Voic", "Bass", "Sprd", "Dens" };
    uint8_t voicValues[] = { chordGen.voicingIndex, chordGen.bassVoicing, chordGen.spread, chordGen.density };
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 50);
      u8g2.print(voicLabels[i]);
      char valBuf[4];
      snprintf(valBuf, sizeof(valBuf), "%d", voicValues[i]);
      int valW = u8g2.getStrWidth(valBuf);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 60);
      u8g2.print(valBuf);
    }

    u8g2.sendBuffer();
  }

  void displayPrevOverlay() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);

    const int colX[] = { 2, 34, 66, 98 };
    const int colW[] = { 28, 28, 28, 28 };

    // Row 1: ENC0-3
    const char* row1Labels[] = { "Oct", "Scale", "Root", "Latch" };
    const char* row1Values[] = {
      nullptr,
      scaleNames[keyScale],
      rootNames[keyRoot],
      keyChordOn ? "ON" : "OFF"
    };
    char octBuf[5];
    snprintf(octBuf, sizeof(octBuf), "%d", keyOctave);
    row1Values[0] = octBuf;
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 14);
      u8g2.print(row1Labels[i]);
      int valW = u8g2.getStrWidth(row1Values[i]);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 23);
      u8g2.print(row1Values[i]);
    }

    // Row 2: ENC4-7
    const char* row2Labels[] = { "KLock", "Perf", "Type", "Amt" };
    const char* klockVal = keyLockOn ? "ON" : "OFF";
    const char* perfVal = perfOn ? "ON" : "OFF";
    const char* row2Values[] = { klockVal, perfVal, "Strum", "0" };
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 38);
      u8g2.print(row2Labels[i]);
      int valW = u8g2.getStrWidth(row2Values[i]);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 47);
      u8g2.print(row2Values[i]);
    }

    // Bottom: hint
    u8g2.setCursor(20, 62);
    u8g2.print("Turn knob to change");

    u8g2.sendBuffer();
  }
};

MyDisplay display;

// ==================== BANK HANDLER ====================
class BankLEDHandler {
public:
  static void updateBankLEDs() {
    // Clear step LEDs
    for (int i = 0; i < 16; i++) {
      leds[i] = CRGB::Black;
    }
    
    // Highlight current bank selection based on mode
    switch(currentBankMode) {
      case BANK_KEYS:
        if (bankKeys.getSelection() < 16)
          leds[bankKeys.getSelection()] = CRGB::SeaGreen;
        break;
      case BANK_ENC:
        if (bankEnc.getSelection() < 16)
          leds[bankEnc.getSelection()] = CRGB::Purple;
        break;
      case BANK_NONE:
        // No bank mode, clear LEDs
        break;
    }
    
    FastLED.show();
  }
  
  static void enterBankMode(BankMode mode) {
    currentBankMode = mode;
    lastActiveMode = mode;
    modeButtonHeld = true;
    updateBankLEDs();
    
    if (mode == BANK_ENC) {
      display.displayEncLabels(true);
    } else {
      display.displayBankLabels();
    }
  }
  
  static void exitBankMode() {
    currentBankMode = BANK_NONE;
    modeButtonHeld = false;
    // Clear step LEDs
    for (int i = 0; i < 16; i++) {
      leds[i] = CRGB::Black;
    }
    FastLED.show();
    
    // Show appropriate display based on last active mode
    if (lastActiveMode == BANK_ENC) {
      display.displayEncLabels(true);
    } else {
      display.displayNormalMode();
    }
  }
};

// ============================================
// Touch Sensor Configuration
// ============================================

// 16 PADS - All pads use Channel_1
ButtonMap padButtons[] = {
  { 1, 1, "PAD1", TYPE_PAD, 59, false, 0, false },
  { 1, 4, "PAD2", TYPE_PAD, 60, false, 0, false },
  { 1, 2, "PAD3", TYPE_PAD, 61, false, 0, false },
  { 3, 10, "PAD4", TYPE_PAD, 62, false, 0, false },
  { 1, 0, "PAD5", TYPE_PAD, 63, false, 0, false },
  { 3, 8, "PAD6", TYPE_PAD, 64, false, 0, false },
  { 3, 2, "PAD7", TYPE_PAD, 65, false, 0, false },
  { 2, 0, "PAD8", TYPE_PAD, 66, false, 0, false },
  { 3, 0, "PAD9", TYPE_PAD, 67, false, 0, false },
  { 2, 5, "PAD10", TYPE_PAD, 68, false, 0, false },
  { 3, 5, "PAD11", TYPE_PAD, 69, false, 0, false },
  { 2, 1, "PAD12", TYPE_PAD, 70, false, 0, false },
  { 0, 0, "PAD13", TYPE_PAD, 71, false, 0, false },
  { 2, 7, "PAD14", TYPE_PAD, 72, false, 0, false },
  { 2, 9, "PAD15", TYPE_PAD, 73, false, 0, false },
  { 0, 1, "PAD16", TYPE_PAD, 74, false, 0, false }
};

const int NUM_PADS = sizeof(padButtons) / sizeof(padButtons[0]);

// CONTROLS - SEQ button is NOT a mode button anymore
ButtonMap controlButtons[] = {
  { 3, 9, "PREV", TYPE_CONTROL, 75, false, 0, false },
  { 1, 5, "NEXT", TYPE_CONTROL, 76, false, 0, false },
  { 1, 3, "M1", TYPE_CONTROL, 77, false, 0, false },
  { 3, 6, "M2", TYPE_CONTROL, 78, false, 0, false },
  { 3, 11, "M3", TYPE_CONTROL, 79, false, 0, false },
  { 3, 7, "F1", TYPE_CONTROL, 80, false, 0, false },
  { 3, 3, "F2", TYPE_CONTROL, 81, false, 0, false },
  { 3, 1, "F3", TYPE_CONTROL, 82, false, 0, false },
  { 3, 4, "F4", TYPE_CONTROL, 83, false, 0, false },
  { 0, 10, "ENC", TYPE_CONTROL, 96, false, 0, true },  // Mode button
  { 0, 11, "SEQ", TYPE_CONTROL, 97, false, 0, false }, // NOT a mode button
  { 0, 9, "KEY", TYPE_CONTROL, 98, false, 0, true },   // Mode button
  { 0, 8, "PLAY", TYPE_CONTROL, 99, false, 0, false },
  { 0, 7, "REC", TYPE_CONTROL, 100, false, 0, false }
};

const int NUM_CONTROLS = sizeof(controlButtons) / sizeof(controlButtons[0]);

// KEYBOARD
ButtonMap keyboardButtons[] = {
  { 2, 2, "KEY_C", TYPE_KEYBOARD, 84, false, 0, false },
  { 0, 5, "KEY_C#", TYPE_KEYBOARD, 85, false, 0, false },
  { 2, 3, "KEY_D", TYPE_KEYBOARD, 86, false, 0, false },
  { 0, 4, "KEY_D#", TYPE_KEYBOARD, 87, false, 0, false },
  { 2, 4, "KEY_E", TYPE_KEYBOARD, 88, false, 0, false },
  { 2, 10, "KEY_F", TYPE_KEYBOARD, 89, false, 0, false },
  { 0, 2, "KEY_F#", TYPE_KEYBOARD, 90, false, 0, false },
  { 2, 6, "KEY_G", TYPE_KEYBOARD, 91, false, 0, false },
  { 0, 3, "KEY_G#", TYPE_KEYBOARD, 92, false, 0, false },
  { 2, 11, "KEY_A", TYPE_KEYBOARD, 93, false, 0, false },
  { 0, 6, "KEY_A#", TYPE_KEYBOARD, 94, false, 0, false },
  { 2, 8, "KEY_B", TYPE_KEYBOARD, 95, false, 0, false }
};

const int NUM_KEYS = sizeof(keyboardButtons) / sizeof(keyboardButtons[0]);

MPR121_GestureHelper gestureHelper;

// ============================================
// Touch Helper Functions
// ============================================

ButtonMap* findButton(uint8_t sensor, uint8_t channel) {
  for (int i = 0; i < NUM_PADS; i++) {
    if (padButtons[i].sensor == sensor && padButtons[i].channel == channel) {
      return &padButtons[i];
    }
  }

  for (int i = 0; i < NUM_CONTROLS; i++) {
    if (controlButtons[i].sensor == sensor && controlButtons[i].channel == channel) {
      return &controlButtons[i];
    }
  }

  for (int i = 0; i < NUM_KEYS; i++) {
    if (keyboardButtons[i].sensor == sensor && keyboardButtons[i].channel == channel) {
      return &keyboardButtons[i];
    }
  }

  return nullptr;
}

void handlePadButton(ButtonMap* button, bool pressed) {
  button->isPressed = pressed;

  // In sequencer mode, skip all pad MIDI
  if (seqModeActive) return;

  // If KEY button is held and pad is touched, enter bank selection mode
  if (pressed && controlButtons[11].isPressed) { // KEY button index 11
    keyPadTouched = true;
    if (currentBankMode != BANK_KEYS) {
      BankLEDHandler::enterBankMode(BANK_KEYS);
      Serial.println("  -> Entered KEYBOARD mode (pad touched during KEY hold)");
    }
  }

  // In keys bank mode, pads only select banks, no MIDI
  if (currentBankMode == BANK_KEYS) {
    if (pressed && modeButtonHeld) {
      int padIndex = button->midiNote - 59;
      bankKeys.select(padIndex);
      midiChannel = padIndex + 1;
      BankLEDHandler::updateBankLEDs();
      display.displayBankLabels();
    }
    return;
  }

  if (pressed) {
    button->pressTime = millis();
    Serial.print("PAD pressed: ");
    Serial.print(button->name);
    Serial.print(" (MIDI Note: ");
    Serial.print(button->midiNote);
    Serial.println(")");
    
    // ALWAYS send MIDI note for pads on Channel_1
    Control_Surface.sendNoteOn({button->midiNote, Channel_1}, 127);
    
    // If a mode button is held, also change bank selection
    if (modeButtonHeld) {
      int padIndex = button->midiNote - 59; // Convert to 0-15
      
      switch(currentBankMode) {
        case BANK_KEYS:
          bankKeys.select(padIndex);
          Serial.print("  -> Selected Keys Bank: ");
          Serial.println(padIndex + 1);
          break;
        case BANK_ENC:
          bankEnc.select(padIndex);
          Serial.print("  -> Selected Enc Bank: ");
          Serial.println(padIndex + 1);
          break;
        case BANK_NONE:
          // No bank mode active
          break;
      }
      
      // Update bank LEDs and display
      BankLEDHandler::updateBankLEDs();
      
      if (currentBankMode == BANK_ENC) {
        display.displayEncLabels(true);
      } else if (currentBankMode != BANK_NONE) {
        display.displayBankLabels();
      }
    }

  } else {
    unsigned long duration = millis() - button->pressTime;
    Serial.print("PAD released: ");
    Serial.print(button->name);
    Serial.print(" - held for ");
    Serial.print(duration);
    Serial.println(" ms");

    // ALWAYS send note off for pads on Channel_1
    Control_Surface.sendNoteOff({button->midiNote, Channel_1}, 0);
  }
}

void handleControlButton(ButtonMap* button, bool pressed) {
  button->isPressed = pressed;

  // SEQ button always works (for toggling sequencer mode on/off)
  if (strcmp(button->name, "SEQ") == 0) {
    if (pressed) {
      seqModeActive = !seqModeActive;
      if (seqModeActive) {
        keyModeActive = false;
        setKeyboardBypassGestures(false);
        disableKeyEncoders();
        Serial.println("  -> Entered SEQUENCER mode (all MIDI I/O disabled)");
        disableAllEncoders();
        // Clear all LEDs
        for (int i = 0; i < 42; i++) {
          leds[i] = CRGB::Black;
        }
        FastLED.show();
        display.displaySequencerMode();
      } else {
        Serial.println("  -> Exited SEQUENCER mode");
        enableAllEncoders();
        BankLEDHandler::exitBankMode();
      }
    }
    return;
  }

  // In sequencer mode, skip all other control button MIDI
  if (seqModeActive) return;

  if (pressed) {
    Serial.print("CONTROL pressed: ");
    Serial.println(button->name);

    // In KEY MODE: handle chord type/extension buttons
    if (keyModeActive && strcmp(button->name, "ENC") != 0) {
      // M1=Maj, M2=Min, M3=Dim, NEXT=Sus → set chord type
      if (strcmp(button->name, "M1") == 0) { chordGen.chordType = CHORD_TYPE_MAJ; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "M2") == 0) { chordGen.chordType = CHORD_TYPE_MIN; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "M3") == 0) { chordGen.chordType = CHORD_TYPE_DIM; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "NEXT") == 0) { chordGen.chordType = CHORD_TYPE_SUS; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      // F1-F4 → momentary extensions (held = on, released = off)
      if (strcmp(button->name, "F1") == 0) { chordGen.extensions |= EXT_6; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "F2") == 0) { chordGen.extensions |= EXT_m7; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "F3") == 0) { chordGen.extensions |= EXT_M7; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      if (strcmp(button->name, "F4") == 0) { chordGen.extensions |= EXT_9; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } return; }
      // PREV: track held state for latch toggle and overlay
      if (strcmp(button->name, "PREV") == 0 && chordGen.active) { prevButtonHeld = true; return; }
      // All other control buttons: no MIDI in KEY MODE
      return;
    }

    // Normal mode: send MIDI for control buttons
    Control_Surface.sendNoteOn({button->midiNote, Channel_1}, 127);

    // Handle mode switching based on button
    if (strcmp(button->name, "ENC") == 0) {
      if (keyModeActive) {
        restoreKeyEncPositions();
        enableAllEncoders();
        keyModeActive = false;
        setKeyboardBypassGestures(false);
      }
      BankLEDHandler::enterBankMode(BANK_ENC);
      Serial.println("  -> Entered ENCODER mode");
    } 
    else if (strcmp(button->name, "KEY") == 0) {
      // Don't enter bank mode yet — wait for release to detect tap vs hold
      keyPadTouched = false;
      Serial.println("  -> KEY pressed (waiting for release)");
    }
    // PREV: track held state for latch toggle (only in chord mode)
    if (strcmp(button->name, "PREV") == 0 && chordGen.active) {
      prevButtonHeld = true;
    }
    // Other control buttons just send MIDI

  } else {
    Serial.print("CONTROL released: ");
    Serial.println(button->name);
    
    // In KEY MODE: clear chord type/extension on release, skip note off
    if (keyModeActive) {
      if (strcmp(button->name, "M1") == 0 || strcmp(button->name, "M2") == 0 ||
          strcmp(button->name, "M3") == 0 || strcmp(button->name, "NEXT") == 0) {
        chordGen.chordType = CHORD_TYPE_NONE;
        if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); }
      }
      if (strcmp(button->name, "F1") == 0) { chordGen.extensions &= ~EXT_6; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } }
      if (strcmp(button->name, "F2") == 0) { chordGen.extensions &= ~EXT_m7; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } }
      if (strcmp(button->name, "F3") == 0) { chordGen.extensions &= ~EXT_M7; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } }
      if (strcmp(button->name, "F4") == 0) { chordGen.extensions &= ~EXT_9; if (chordGen.active && chordGen.noteRoot >= 0) { stopChord(); playChord(chordGen.noteRoot); } }
      if (strcmp(button->name, "PREV") == 0) prevButtonHeld = false;
      return;
    }

    // Always send note off on Channel_1
    Control_Surface.sendNoteOff({button->midiNote, Channel_1}, 0);
    
    // For mode buttons, check if it's still being held by another finger
    if (button->isModeButton) {
      // Update modeButtonHeld status
      modeButtonHeld = false;
      for (int i = 0; i < NUM_CONTROLS; i++) {
        if (controlButtons[i].isModeButton && controlButtons[i].isPressed) {
          modeButtonHeld = true;
          Serial.print("  -> Another mode button still held: ");
          Serial.println(controlButtons[i].name);
          break;
        }
      }
      
      // If no mode buttons are held, handle KEY tap vs hold
      if (!modeButtonHeld) {
        if (strcmp(button->name, "PREV") == 0) prevButtonHeld = false;
        if (strcmp(button->name, "KEY") == 0) {
          unsigned long holdDuration = millis() - button->pressTime;
          if (holdDuration > 300 && keyPadTouched) {
            // Long hold with pad touch — already entered bank mode via pad handler
            Serial.println("  -> KEY: bank mode (held with pads)");
          } else {
            // Short tap or no pads touched — show key mode display
            if (currentBankMode == BANK_KEYS) {
              BankLEDHandler::exitBankMode();
            }
            keyModeActive = !keyModeActive;
            setKeyboardBypassGestures(keyModeActive);
            if (keyModeActive) {
              saveKeyEncPositions();
              disableAllEncoders();
              syncKeyEncodersToParams();
              updateKeyModeLEDs();
              Serial.println("  -> Entered KEY MODE display");
              display.displayKeyMode();
            } else {
              if (chordGen.active) {
                stopChord();
                chordGen.noteRoot = -1;
                chordGen.heldKeyIndex = -1;
                chordGen.heldCount = 0;
                keyboardHeldMask = 0;
              }
              restoreKeyEncPositions();
              enableAllEncoders();
              for (int i = 0; i < 12; i++) leds[keyLedIndex[i]] = CRGB::Black;
              FastLED.show();
              Serial.println("  -> Exited KEY MODE display");
              display.displayNormalMode();
            }
          }
        } else {
          Serial.println("  -> Exiting bank mode");
          BankLEDHandler::exitBankMode();
        }
      }
    }
  }
}

bool isNoteInScale(uint8_t noteIndex) {
  // noteIndex: 0=C .. 11=B, keyRoot shifts the pattern so root is always included
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
  return rotated & (1 << noteIndex);
}

// KeyLock: find the best diatonic chord for any played note
// Returns the scale degree (0-6) of the nearest diatonic root
int8_t keylockFindDegree(uint8_t playedPC) {
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;

  int8_t bestDegree = 0;
  int8_t bestDist = 12;
  int8_t degree = 0;

  for (int s = 0; s < 12; s++) {
    if (rotated & (1 << ((keyRoot + s) % 12))) {
      int8_t dist = abs((int8_t)playedPC - (int8_t)((keyRoot + s) % 12));
      if (dist > 6) dist = 12 - dist;
      if (dist < bestDist) {
        bestDist = dist;
        bestDegree = degree;
      }
      degree++;
    }
  }
  return bestDegree;
}

// KeyLock: determine chord quality for a scale degree in the current key/scale
// Returns qualityIntervals index: 0=Maj, 1=min, 2=dim, 5=sus4
uint8_t keylockDegreeQuality(int8_t degree) {
  if (keyScale == 0) return 0; // Chromatic: default Major

  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;

  // Collect scale degrees and their pitch classes
  uint8_t degPC[7];
  int8_t degCount = 0;
  for (int s = 0; s < 12 && degCount < 7; s++) {
    if (rotated & (1 << ((keyRoot + s) % 12))) {
      degPC[degCount++] = (keyRoot + s) % 12;
    }
  }

  if (degCount < 3) return 0; // Not enough notes for a triad

  int8_t d = degree % degCount;
  uint8_t rootPC = degPC[d];
  uint8_t thirdPC = degPC[(d + 2) % degCount];
  uint8_t fifthPC = degPC[(d + 4) % degCount];

  int8_t thirdInterval = (thirdPC - rootPC + 12) % 12;
  int8_t fifthInterval = (fifthPC - rootPC + 12) % 12;

  if (thirdInterval == 4 && fifthInterval == 7) return 0; // Major
  if (thirdInterval == 3 && fifthInterval == 7) return 1; // Minor
  if (thirdInterval == 3 && fifthInterval == 6) return 2; // Diminished
  return 0; // Default Major
}

// KeyLock: play the best diatonic chord for a given played note
void playKeyLockChord(int8_t midiNote) {
  stopChord();
  if (midiNote < 0) return;

  uint8_t playedPC = midiNote % 12;
  int8_t degree = keylockFindDegree(playedPC);
  uint8_t quality = keylockDegreeQuality(degree);

  // Find the MIDI note of the diatonic root closest to played note
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
  int8_t bestSemitoneDist = 12;
  uint8_t chordRoot = midiNote;
  for (int s = 0; s < 12; s++) {
    if (rotated & (1 << ((keyRoot + s) % 12))) {
      int8_t dist = abs((int8_t)playedPC - (int8_t)((keyRoot + s) % 12));
      if (dist > 6) dist = 12 - dist;
      if (dist < bestSemitoneDist) {
        bestSemitoneDist = dist;
        chordRoot = midiNote - ((int8_t)playedPC - (int8_t)((keyRoot + s) % 12));
      }
    }
  }

  chordRoot = constrain(chordRoot, 36, 84);

  // Override chordGen quality temporarily
  uint8_t savedType = chordGen.chordType;
  if (quality == 0) chordGen.chordType = CHORD_TYPE_MAJ;
  else if (quality == 1) chordGen.chordType = CHORD_TYPE_MIN;
  else if (quality == 2) chordGen.chordType = CHORD_TYPE_DIM;
  else chordGen.chordType = CHORD_TYPE_SUS;

  playChord(chordRoot);
  chordGen.chordType = savedType;

  // Store state for held tracking
  chordGen.noteRoot = chordRoot;
  keyPlayingChord = true;

  Serial.print("  -> KeyLock: deg=");
  Serial.print(degree);
  Serial.print(" q=");
  Serial.print(quality);
  Serial.print(" root=");
  Serial.println(chordRoot);
}

void setKeyboardBypassGestures(bool bypass) {
  for (int i = 0; i < NUM_KEYS; i++) {
    gestureHelper.setChannelBypassGestures(keyboardButtons[i].sensor, keyboardButtons[i].channel, bypass);
  }
}

void handleKeyboardButton(ButtonMap* button, bool pressed) {
  button->isPressed = pressed;

  // Map button to keyNotes index (midiNote 84-95 → index 0-11)
  int keyIndex = button->midiNote - 84;
  if (keyIndex < 0 || keyIndex >= 12) return;

  // Chord mode (ENC3 ON): play chord or single note
  if (chordGen.active) {
    if (pressed) {
      int8_t midiNote = 48 + keyIndex + (keyOctave - 3) * 12;
      chordGen.noteRoot = midiNote;
      chordGen.heldKeyIndex = keyIndex;
      chordGen.heldCount++;
      keyboardHeldMask |= (1 << keyIndex);

      if (keyLockOn) {
        // KeyLock: find best diatonic chord for this note
        playKeyLockChord(midiNote);
      } else if (chordGen.chordType != CHORD_TYPE_NONE) {
        // Chord type button held: play chord
        playChord(midiNote);
        keyPlayingChord = true;
      } else if (keyScale != 0 && keyModeActive) {
        // Key Mode active, no chord type held: auto-diatonic chord
        uint16_t pattern = scalePatterns[keyScale];
        uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
        int8_t semitoneDist = (keyIndex - keyRoot + 12) % 12;
        // Count scale degree
        int8_t scaleDeg = 0;
        for (int s = 0; s <= semitoneDist && s < 12; s++) {
          if (rotated & (1 << ((keyRoot + s) % 12))) {
            if (s == semitoneDist) break;
            scaleDeg++;
          }
        }
        // Major scale pattern: I=Maj ii=min iii=min IV=Maj V=Maj vi=min vii=dim
        uint8_t savedType = chordGen.chordType;
        if (scaleDeg == 0 || scaleDeg == 3 || scaleDeg == 4)
          chordGen.chordType = CHORD_TYPE_MAJ;
        else if (scaleDeg == 6)
          chordGen.chordType = CHORD_TYPE_DIM;
        else
          chordGen.chordType = CHORD_TYPE_MIN;
        playChord(midiNote);
        chordGen.chordType = savedType;
        keyPlayingChord = true;
      } else {
        // No chord type, no key mode: play single note
        int8_t noteNum = 48 + keyIndex + (keyOctave - 3) * 12;
        if (pressed)
          Control_Surface.sendNoteOn({(uint8_t)noteNum, Channel::createChannel(midiChannel)}, 100);
        else
          Control_Surface.sendNoteOff({(uint8_t)noteNum, Channel::createChannel(midiChannel)}, 0);
        keyPlayingChord = false;
        const uint8_t keyboardLEDs[] = { 25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31 };
        leds[keyboardLEDs[keyIndex]] = CRGB::White;
        FastLED.show();
      }
    } else {
      keyboardHeldMask &= ~(1 << keyIndex);
      chordGen.heldCount--;
      if (chordGen.heldCount < 0) chordGen.heldCount = 0;

      // Release single note if it was playing
      if (!keyPlayingChord) {
        int8_t noteNum = 48 + keyIndex + (keyOctave - 3) * 12;
        Control_Surface.sendNoteOff({(uint8_t)noteNum, Channel::createChannel(midiChannel)}, 0);
        // Restore scale color for this key
        uint16_t pattern = scalePatterns[keyScale];
        uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
        if (rotated & (1 << keyIndex)) {
          leds[keyLedIndex[keyIndex]] = CRGB(0, 0, 18);
        } else {
          leds[keyLedIndex[keyIndex]] = CRGB::Black;
        }
        FastLED.show();
      }

      if (keyboardHeldMask == 0) {
        // All keys released: stop chord only if latch is OFF
        if (keyPlayingChord && !keyChordOn) {
          stopChord();
          chordGen.noteRoot = -1;
          chordGen.heldKeyIndex = -1;
          chordGen.heldCount = 0;
          keyPlayingChord = false;
          updateKeyModeLEDs();
        } else if (keyPlayingChord && keyChordOn) {
          // Latch on: keep chord playing, keep chord LEDs lit
          keyPlayingChord = false;
          chordGen.heldKeyIndex = -1;
          chordGen.heldCount = 0;
          // Re-light chord LEDs (updateKeyModeLEDs would clear them)
          for (uint8_t i = 0; i < chordNoteCount; i++) {
            uint8_t pc = chordNotesPlaying[i] % 12;
            leds[keyLedIndex[pc]] = CRGB(0, 40, 80);
          }
          FastLED.show();
        } else if (!keyPlayingChord) {
          // Single note was playing, clean up state
          chordGen.noteRoot = -1;
          chordGen.heldKeyIndex = -1;
          chordGen.heldCount = 0;
        }
      } else {
        // Some keys still held: replay chord with last held key's root
        for (int k = 0; k < 12; k++) {
          if (keyboardHeldMask & (1 << k)) {
            int8_t midiNote = 48 + k + (keyOctave - 3) * 12;
            chordGen.noteRoot = midiNote;
            chordGen.heldKeyIndex = k;
            if (keyLockOn) {
              playKeyLockChord(midiNote);
            } else if (chordGen.chordType != CHORD_TYPE_NONE) {
              playChord(midiNote);
            } else if (keyScale != 0 && keyModeActive) {
              // replay auto-diatonic for this key
              uint16_t pattern = scalePatterns[keyScale];
              uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;
              int8_t semitoneDist = (k - keyRoot + 12) % 12;
              int8_t scaleDeg = 0;
              for (int s = 0; s <= semitoneDist && s < 12; s++) {
                if (rotated & (1 << ((keyRoot + s) % 12))) {
                  if (s == semitoneDist) break;
                  scaleDeg++;
                }
              }
              uint8_t savedType = chordGen.chordType;
              if (scaleDeg == 0 || scaleDeg == 3 || scaleDeg == 4)
                chordGen.chordType = CHORD_TYPE_MAJ;
              else if (scaleDeg == 6)
                chordGen.chordType = CHORD_TYPE_DIM;
              else
                chordGen.chordType = CHORD_TYPE_MIN;
              playChord(midiNote);
              chordGen.chordType = savedType;
            }
            break; // replay for the lowest held key
          }
        }
      }
    }
    return;
  }

  // Filter notes not in the selected scale (unless Chromatic or KeyLock is selected)
  if (keyScale != 0 && !keyLockOn && !isNoteInScale(keyIndex)) {
    if (pressed) {
      // Flash red briefly to indicate disabled
      const uint8_t keyboardLEDs[] = { 25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31 };
      leds[keyboardLEDs[keyIndex]] = CRGB(40, 0, 0);
      FastLED.show();
    } else {
      updateKeyModeLEDs();
    }
    return;
  }

  // Send MIDI note directly with current channel
  int8_t noteNum = 48 + keyIndex + (keyOctave - 3) * 12;
  if (pressed)
    Control_Surface.sendNoteOn({(uint8_t)noteNum, Channel::createChannel(midiChannel)}, 100);
  else
    Control_Surface.sendNoteOff({(uint8_t)noteNum, Channel::createChannel(midiChannel)}, 0);

  // Direct LED flash for keyboard touch feedback (preserve/restore existing color)
  const uint8_t keyboardLEDs[] = { 25, 37, 26, 38, 27, 28, 39, 29, 40, 30, 41, 31 };
  static CRGB savedColors[12] = {};
  uint8_t led = keyboardLEDs[keyIndex];
  if (pressed) {
    savedColors[keyIndex] = leds[led];
    leds[led] = CRGB::White;
  } else {
    leds[led] = savedColors[keyIndex];
  }
  FastLED.show();
}

void handleTouchEvent(uint8_t sensorIndex, uint8_t channel, bool touched) {
  ButtonMap* button = findButton(sensorIndex, channel);
  if (!button) return;

  switch (button->type) {
    case TYPE_PAD:
      handlePadButton(button, touched);
      break;
    case TYPE_CONTROL:
      handleControlButton(button, touched);
      break;
    case TYPE_KEYBOARD:
      handleKeyboardButton(button, touched);
      break;
  }
}

void handleGestureEvent(uint8_t sensorIndex, uint8_t channel, const char* gestureName, unsigned long duration) {
  ButtonMap* button = findButton(sensorIndex, channel);
  if (!button) return;

  Serial.print("Gesture on ");
  Serial.print(button->name);
  Serial.print(": ");
  Serial.print(gestureName);

  if (duration > 0) {
    Serial.print(" (");
    Serial.print(duration);
    Serial.print(" ms)");
  }
  Serial.println();

  // Handle gestures - you can add MIDI CC or other messages here
  if (seqModeActive) return; // Skip gesture MIDI in sequencer mode
  if (strcmp(gestureName, "double_tap") == 0) {
    // Send double tap as higher velocity or different message
    if (button->type == TYPE_PAD) {
      Control_Surface.sendNoteOn({button->midiNote, Channel_1}, 100); // Higher velocity for double tap
    }
  } else if (strcmp(gestureName, "long_press") == 0) {
    // Send long press as sustain or other control
    if (button->type == TYPE_PAD) {
      Control_Surface.sendControlChange({button->midiNote, Channel_1}, 127);
    }
  }
}

// Function to check encoder changes
void checkEncoderChanges() {
  // Skip encoder updates in sequencer mode
  if (seqModeActive) return;

  // Update all encoders
  enc0.update(); enc1.update(); enc2.update(); enc3.update();
  enc4.update(); enc5.update(); enc6.update(); enc7.update();
  
  // Now check if values have changed
  uint16_t currentValues[8] = {
    enc0.getValue(), enc1.getValue(), enc2.getValue(), enc3.getValue(),
    enc4.getValue(), enc5.getValue(), enc6.getValue(), enc7.getValue()
  };
  
  for (int i = 0; i < 8; i++) {
    if (currentValues[i] != lastEncoderValues[i]) {
      lastEncoderValues[i] = currentValues[i];
      encoderValueChanged = true;

      // TODO: remove - diagnostic
      Serial.print("ENC ch=");
      Serial.print(bankEnc.getSelection());
      Serial.print(" vals=[");
      for (int j = 0; j < 8; j++) {
        Serial.print(currentValues[j]);
        if (j < 7) Serial.print(",");
      }
      Serial.println("]");

      // Update display immediately if last active mode was ENC
      if (lastActiveMode == BANK_ENC) {
        display.displayEncLabels();
      }
    }
  }
}

// ============================================
// Setup and Main Loop
// ============================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("MIDI Controller Starting...");

  // Initialize LEDs
  FastLED.addLeds<NEOPIXEL, ledpin>(leds.data, leds.length);
  FastLED.setCorrection(TypicalPixelString);
  FastLED.setBrightness(128);
  FastLED.clear();
  FastLED.show();
  
  RelativeCCSender::setMode(MACKIE_CONTROL_RELATIVE);
  
  // Initialize MIDI
  Control_Surface | pipes | midi;
  Control_Surface | pipes | serialmidi2;
  Control_Surface.begin();

  // Initialize I2C for touch sensors
  Wire.begin();
  
  // Initialize Display
  display.begin();
  display.drawBackground();
  display.displayNormalMode();

  // Initialize MPR121 gesture helper
  gestureHelper.begin(300, 200, 3);

  // Add all 4 MPR121 sensors
  Serial.println("Initializing touch sensors...");
  if (!gestureHelper.addSensor(0x5A)) Serial.println("Sensor 0x5A failed");
  if (!gestureHelper.addSensor(0x5B)) Serial.println("Sensor 0x5B failed");
  if (!gestureHelper.addSensor(0x5C)) Serial.println("Sensor 0x5C failed");
  if (!gestureHelper.addSensor(0x5D)) Serial.println("Sensor 0x5D failed");

  // Set thresholds for all sensors
  for (int i = 0; i < 4; i++) {
    gestureHelper.setThresholds(i, 40, 20);
  }

  // Set up event handlers
  gestureHelper.onTouchEvent(handleTouchEvent);
  gestureHelper.onGesture(handleGestureEvent);

  Serial.println("\n=== System Ready ===");
  Serial.print("Pads: ");
  Serial.println(NUM_PADS);
  Serial.print("Controls: ");
  Serial.println(NUM_CONTROLS);
  Serial.print("Keyboard keys: ");
  Serial.println(NUM_KEYS);
  Serial.println("====================\n");
  Serial.println("Mode buttons: KEY (Keyboard bank), ENC (Encoder bank)");
  Serial.println("Hold mode button + tap pads 1-16 to select bank");
}

void loop() {
  // Update touch sensors first (sets touch states for bankable notes)
  gestureHelper.update();
  // Then update MIDI and bankable elements
  Control_Surface.loop();
  
  // Always read ENC3 for chord mode / latch toggle
  readENC3Always();

  // Check for encoder changes
  if (!keyModeActive) {
    checkEncoderChanges();
  }
  
  // Update display based on mode (skip if in sequencer mode)
  static unsigned long lastDisplayUpdate = 0;
  unsigned long now = millis();
  
  // PREV held in chord mode: show encoder roles overlay (highest priority)
  if (prevButtonHeld && chordGen.active) {
    display.displayPrevOverlay();
    lastDisplayUpdate = now;
  } else if (!seqModeActive && !keyModeActive) {
    // Always update encoder display when last active was ENC
    if (lastActiveMode == BANK_ENC) {
      // Update display when encoder values change OR periodically
      if (encoderValueChanged || (now - lastDisplayUpdate > 100)) {
        display.displayEncLabels();
        encoderValueChanged = false;
        lastDisplayUpdate = now;
      }
    } else if (currentBankMode != BANK_NONE) {
      // Update bank display for other modes less frequently
      if (now - lastDisplayUpdate > 500) {
        display.displayBankLabels();
        lastDisplayUpdate = now;
      }
    }
  } else if (keyModeActive) {
    readKeyModeEncoders();
    if (now - lastDisplayUpdate > 100) {
      if (chordGen.active)
        display.displayChordMode();
      else
        display.displayKeyMode();
      lastDisplayUpdate = now;
    }
  }

  // Update LEDs if needed
  if (seqModeActive) {
    // In sequencer mode, keep all LEDs off
    bool anyLit = false;
    for (int i = 0; i < 42; i++) {
      if (leds[i]) { anyLit = true; break; }
    }
    if (anyLit) {
      FastLED.clear();
      FastLED.show();
    }
  } else if (midiled.getDirty()) {
    FastLED.show();
    midiled.clearDirty();
  }

  // Status indicator
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 30000) {
    lastStatus = millis();
    Serial.println("System running...");
  }
}