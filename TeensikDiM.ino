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

// Discovery candidate
struct DiscoveryCandidate {
  uint8_t relation;          // 0=Rt, 1=3rd, 2=5th, 3=7th, 4=Ext
  int8_t rootDegreeOffset;   // degrees to subtract from selected degree
  uint8_t requiredSize;      // minimum Size needed (0=Tri, 1=7th, 2=9th, 3=11th, 4=13th)
  uint8_t rootPitchClass;    // computed root pitch class
  bool valid;                // false if degree arithmetic produces invalid root
};

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
  0b101011010101, // Major (W W H W W W H)
  0b101101011010, // Minor (natural)
  0b101101010110, // Dorian
  0b101011010110, // Mixolydian
  0b101001011001, // Pentatonic major
  0b110101011010, // Blues (hexatonic)
  0b101011010101, // HarmMj (same intervals as major for notes)
  0b101101011010, // HarmMn (same as natural minor)
  0b101101010110, // MelMin (ascending, same as dorian-ish)
  0b101010110110, // Phrygian
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

// ==================== DISCOVERY MODE (M1) ====================

// F1 — CHORD parameters (controlled by ENC4-7)
const char* qualityNames[] = { "Maj", "Min", "Dim", "Aug", "Sus2", "Sus4" };
const uint8_t numQualities = 6;

const char* sizeNames[] = { "Tri", "7th", "9th", "11th", "13th" };
const uint8_t numSizes = 5;

const char* extensionNames[] = { "Off", "9", "11", "13" };
const uint8_t numExtensions = 4;

const char* densityNames[] = { "Full", "Reduced", "Sparse" };
const uint8_t numDensities = 3;

// F2 — COLOR parameters (controlled by ENC4-7)
const char* addedNames[] = { "Off", "2", "4", "6", "13" };
const uint8_t numAdded = 5;

const char* suspendNames[] = { "Off", "Sus2", "Sus4", "2+4" };
const uint8_t numSuspensions = 4;

const char* alterNames[] = { "Off", "b5", "#5", "b9", "#9", "#11", "b13" };
const uint8_t numAlterations = 7;

const char* spreadNames[] = { "Tight", "Med", "Open", "Wide" };
const uint8_t numSpreads = 4;

// F3 — VOICING parameters (controlled by ENC4-7)
const char* topVoiceNames[] = { "Root", "3rd", "5th", "7th" };
const uint8_t numTopVoices = 4;

const char* spacingNames[] = { "Close", "Med", "Open", "Wide" };
const uint8_t numSpacings = 4;

const char* registerNames[] = { "Low", "LoMd", "HiMd", "High" };
const uint8_t numRegisters = 4;

const char* bassNames[] = { "Root", "3rd", "5th", "7th" };
const uint8_t numBassOptions = 4;

// F4 — DISCOVERY parameters (controlled by ENC4-7)
const char* relationNames[] = { "Rt", "3rd", "5th", "7th", "Ext" };
const uint8_t numRelations = 5;

const char* diatChromNames[] = { "Dia", "Mix", "Chr" };
const uint8_t numDiatChrom = 3;

const char* simpleCompNames[] = { "Smp", "Med", "Cplx" };
const uint8_t numSimpleComp = 3;

const char* famUnexpNames[] = { "Fam", "Mix", "Unx" };
const uint8_t numFamUnexp = 3;

// Chord state
struct ChordState {
  uint8_t pageIndex;         // 0=F1, 1=F2, 2=F3, 3=F4
  uint8_t params[4][4];      // [page][encoder] — current value per param
  int8_t noteRoot;            // MIDI note of most recently pressed key, -1 if none
  int8_t heldKeyIndex;        // physical key index (0-11) of most recently pressed, -1 if none
  uint8_t heldCount;          // number of keyboard notes currently held
  bool active;                // true when Discovery Mode is on
};

ChordState chord = {
  0,
  {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}},
  -1, -1, 0, false
};

uint8_t chordNotesPlaying[8] = {};
uint8_t chordNoteCount = 0;

// Get parameter max value for a given page and encoder (ENC4-7 → enc 0-3)
uint8_t chordParamMax(uint8_t page, uint8_t enc) {
  switch (page) {
    case 0: // F1 — CHORD
      switch (enc) {
        case 0: return numQualities - 1;
        case 1: return numSizes - 1;
        case 2: return numExtensions - 1;
        case 3: return numDensities - 1;
      }
      break;
    case 1: // F2 — COLOR
      switch (enc) {
        case 0: return numAdded - 1;
        case 1: return numSuspensions - 1;
        case 2: return numAlterations - 1;
        case 3: return numSpreads - 1;
      }
      break;
    case 2: // F3 — VOICING
      switch (enc) {
        case 0: return numTopVoices - 1;
        case 1: return numSpacings - 1;
        case 2: return numRegisters - 1;
        case 3: return numBassOptions - 1;
      }
      break;
    case 3: // F4 — DISCOVERY
      switch (enc) {
        case 0: return numRelations - 1;
        case 1: return numDiatChrom - 1;
        case 2: return numSimpleComp - 1;
        case 3: return numFamUnexp - 1;
      }
      break;
  }
  return 0;
}

// Get display label for a parameter
const char* chordParamLabel(uint8_t page, uint8_t enc) {
  switch (page) {
    case 0:
      switch (enc) {
        case 0: return "Qual";
        case 1: return "Size";
        case 2: return "Ext";
        case 3: return "Dens";
      }
      break;
    case 1:
      switch (enc) {
        case 0: return "Add";
        case 1: return "Susp";
        case 2: return "Alt";
        case 3: return "Sprd";
      }
      break;
    case 2:
      switch (enc) {
        case 0: return "Top";
        case 1: return "Spc";
        case 2: return "Reg";
        case 3: return "Bas";
      }
      break;
    case 3:
      switch (enc) {
        case 0: return "Rel";
        case 1: return "Dia";
        case 2: return "Smp";
        case 3: return "Fam";
      }
      break;
  }
  return "";
}

// Get display value for a parameter
const char* chordParamValue(uint8_t page, uint8_t enc) {
  uint8_t val = chord.params[page][enc];
  switch (page) {
    case 0:
      switch (enc) {
        case 0: return qualityNames[val];
        case 1: return sizeNames[val];
        case 2: return extensionNames[val];
        case 3: return densityNames[val];
      }
      break;
    case 1:
      switch (enc) {
        case 0: return addedNames[val];
        case 1: return suspendNames[val];
        case 2: return alterNames[val];
        case 3: return spreadNames[val];
      }
      break;
    case 2:
      switch (enc) {
        case 0: return topVoiceNames[val];
        case 1: return spacingNames[val];
        case 2: return registerNames[val];
        case 3: return bassNames[val];
      }
      break;
    case 3:
      switch (enc) {
        case 0: return relationNames[val];
        case 1: return diatChromNames[val];
        case 2: return simpleCompNames[val];
        case 3: return famUnexpNames[val];
      }
      break;
  }
  return "";
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
// CHORD ENGINE — PROCESSING PRECEDENCE
// ============================================================
//
// Each stage may ONLY modify what is listed under "Modifies".
// No later stage may silently redefine the semantic result
// of an earlier stage.
//
// Stage 1: Discovery    → determines chord root and required size
//   Modifies: external variables (chordRoot, requiredSize)
//   Does NOT touch ChordStructure yet.
//
// Stage 2: Quality      → defines triad intervals
//   Modifies: tones[THIRD].pitchClass, tones[FIFTH].pitchClass
//   Allowed: ±1 semitone chromatic shift on 3rd and/or 5th
//
// Stage 3: Size         → adds diatonic extensions
//   Modifies: .present flag on SEVENTH/NINTH/ELEVENTH/THIRTEENTH
//   Allowed: only setting .present = true
//
// Stage 4: Extension    → structurally adds extension roles
//   Modifies: .present flag on NINTH/ELEVENTH/THIRTEENTH
//   Allowed: only setting .present = true
//   Note: additive with Size. Both can set the same role present.
//
// Stage 5: Added Tone   → adds non-chord tones
//   Modifies: tones[ADDED].present, .pitchClass
//   Allowed: setting pitchClass and present on ROLE_ADDED only
//
// Stage 6: Suspension   → replaces 3rd pitch class
//   Modifies: tones[THIRD].pitchClass
//   Allowed: replacing pitchClass only (not role, not present)
//
// Stage 7: Alteration   → chromatically shifts present tones
//   Modifies: target tone's .pitchClass ±1, .altered flag
//   Allowed: only on already-present roles
//   Rule: ignore if target role not present
//
// Stage 8: Density      → removes tones for voicing sparsity
//   Modifies: .present = false on targeted roles
//   Allowed: only setting .present = false
//
// Stage 9: Top Voice / Bass → role-based voicing reorder
//   Modifies: voicingSlot assignment
//   Allowed: only reordering voicingSlot values
//
// Stage 10: Spacing     → minimum intervals between voices
//   Modifies: pitchClass, octaveOffset
//   Allowed: pushing tones up to meet minimum interval
//
// Stage 11: Spread      → octave doublings
//   Modifies: octaveOffset
//   Allowed: adding octave doublings
//
// Stage 12: Register    → global transposition
//   Modifies: global octave offset
//   Allowed: transposing all tones by fixed interval
//
// Stage 13: MIDI Output → sends NoteOn messages
//   Modifies: nothing (read-only)
// ============================================================

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
// DISCOVERY — CANDIDATE GENERATION
// ============================================================

// Build list of Discovery candidates for a given selected note
// Returns number of valid candidates (0-5)
uint8_t buildDiscoveryCandidates(
  uint8_t selectedNoteMIDI,
  uint8_t quality,
  const ScaleInfo &scale,
  uint8_t diatChromValue,
  DiscoveryCandidate candidates[5]
) {
  uint8_t selectedPC = selectedNoteMIDI % 12;
  int8_t selectedDegree = getScaleDegree(selectedPC, scale);

  // Degree offsets for each relation: how many scale degrees to go DOWN
  // to find the chord root from the selected note's degree
  const int8_t relationDegreeOffsets[5] = {
    0,   // Rt: selected is root (offset 0)
    -2,  // 3rd: selected is 3rd, go down 2 scale degrees to root
    -4,  // 5th: selected is 5th, go down 4 scale degrees to root
    -6,  // 7th: selected is 7th, go down 6 scale degrees to root
    -8   // Ext: selected is extension, go down 8 scale degrees to root
  };

  // Minimum size required for each relation
  const uint8_t relationMinSize[5] = {
    0,  // Rt: triad sufficient
    0,  // 3rd: triad sufficient
    0,  // 5th: triad sufficient
    1,  // 7th: needs Size >= 7th
    2   // Ext: needs Size >= 9th
  };

  uint8_t validCount = 0;

  for (uint8_t r = 0; r < 5; r++) {
    candidates[r].relation = r;
    candidates[r].rootDegreeOffset = relationDegreeOffsets[r];
    candidates[r].requiredSize = relationMinSize[r];
    candidates[r].valid = false;
    candidates[r].rootPitchClass = 0;

    if (selectedDegree < 0) {
      // Selected note is not in scale — only allow Relation=0 (Rt) for chromatic
      if (r == 0 && diatChromValue > 0) {
        candidates[r].valid = true;
        candidates[r].rootPitchClass = selectedPC;
        validCount++;
      }
      continue;
    }

    // Compute root degree by going DOWN from selected degree
    int16_t rootDegree = (int16_t)selectedDegree + relationDegreeOffsets[r];
    // Wrap around the scale
    while (rootDegree < 0) rootDegree += scale.count;
    rootDegree = rootDegree % scale.count;

    uint8_t rootPC = scale.pitchClasses[rootDegree];
    candidates[r].rootPitchClass = rootPC;
    candidates[r].valid = true;

    // Diatonic filter: check if all tones required by this candidate
    // (triad at minimum) are in the scale
    if (diatChromValue == 0) {
      // Fully diatonic: triad tones must all be in scale
      // Build a temporary triad from rootPC in this scale
      uint8_t triadDegrees[3] = {0, 2, 4};
      bool allInScale = true;
      for (uint8_t t = 0; t < 3; t++) {
        int16_t deg = rootDegree + triadDegrees[t];
        while (deg >= scale.count) deg -= scale.count;
        uint8_t triadPC = scale.pitchClasses[deg];
        // Apply quality shift to check
        int8_t shift = 0;
        if (t == 1) { // 3rd
          if (quality == 1 || quality == 3 || quality == 4) shift = -1; // Minor/Dim/Sus2
        } else if (t == 2) { // 5th
          if (quality == 2) shift = -1; // Dim
          else if (quality == 3) shift = +1; // Aug
        }
        uint8_t finalPC = (triadPC + shift + 12) % 12;
        // Check if this PC is in the scale
        bool found = false;
        for (uint8_t s = 0; s < scale.count; s++) {
          if (scale.pitchClasses[s] == finalPC) { found = true; break; }
        }
        if (!found) { allInScale = false; break; }
      }
      if (!allInScale) {
        candidates[r].valid = false;
      } else {
        validCount++;
      }
    } else {
      // Chromatic or mixed — allow
      validCount++;
    }
  }

  return validCount;
}

// Get chord root MIDI note from a candidate
int8_t computeCandidateRootMIDI(
  const DiscoveryCandidate &candidate,
  uint8_t selectedNoteMIDI,
  const ScaleInfo &scale
) {
  uint8_t selectedPC = selectedNoteMIDI % 12;
  int8_t selectedDegree = getScaleDegree(selectedPC, scale);
  if (selectedDegree < 0) return selectedNoteMIDI; // fallback

  // Compute how many semitones to go down from selected note to root
  uint8_t rootPC = candidate.rootPitchClass;
  int16_t semitoneOffset = (int16_t)rootPC - (int16_t)selectedPC;
  if (semitoneOffset > 0) semitoneOffset -= 12; // ensure we go DOWN
  return constrain(selectedNoteMIDI + semitoneOffset, 36, 84);
}

// ============================================================
// CHORDSTRUCTURE — INITIALIZATION
// ============================================================

// Initialize ChordStructure with all tones absent
void initChordStructure(ChordStructure &chord) {
  chord.toneCount = 0;
  chord.rootMIDI = 60;
  chord.rootPitchClass = 0;
  for (uint8_t i = 0; i < ROLE_COUNT; i++) {
    chord.tones[i].role = (ChordRole)i;
    chord.tones[i].pitchClass = 0;
    chord.tones[i].octaveOffset = 0;
    chord.tones[i].present = false;
    chord.tones[i].altered = false;
  }
}

// ============================================================
// STAGE 2: QUALITY — define triad intervals
// ============================================================

// Quality adjustments to the 3rd and 5th pitch classes
// [quality][0]=3rd shift, [quality][1]=5th shift
const int8_t qualityShifts[6][2] = {
  { 0,  0 },  // Major: 3rd as-is, 5th as-is
  {-1,  0 },  // Minor: flatten 3rd
  {-1, -1 },  // Diminished: flatten 3rd and 5th
  { 0, +1 },  // Augmented: sharpen 5th
  {-1,  0 },  // Sus2: handled separately (replaces 3rd)
  { 0,  0 },  // Sus4: handled separately (replaces 3rd)
};

void applyQuality(ChordStructure &chord, uint8_t quality) {
  const ScaleInfo scale = buildScaleInfo();
  uint8_t rootDegree = 0; // root is always degree 0

  // 3rd: scale degree 2, then apply quality shift
  uint8_t thirdDegree = (rootDegree + 2) % scale.count;
  uint8_t thirdPC = scale.pitchClasses[thirdDegree];
  int8_t shift = (quality < 6) ? qualityShifts[quality][0] : 0;
  chord.tones[ROLE_THIRD].pitchClass = (thirdPC + shift + 12) % 12;

  // 5th: scale degree 4, then apply quality shift
  uint8_t fifthDegree = (rootDegree + 4) % scale.count;
  uint8_t fifthPC = scale.pitchClasses[fifthDegree];
  shift = (quality < 6) ? qualityShifts[quality][1] : 0;
  chord.tones[ROLE_FIFTH].pitchClass = (fifthPC + shift + 12) % 12;

  // Root is always present with pitchClass 0
  chord.tones[ROLE_ROOT].pitchClass = 0;
  chord.tones[ROLE_ROOT].octaveOffset = 0;
  chord.tones[ROLE_ROOT].present = true;
  chord.tones[ROLE_ROOT].altered = false;

  // Mark triad as present
  chord.tones[ROLE_THIRD].octaveOffset = 0;
  chord.tones[ROLE_THIRD].present = true;
  chord.tones[ROLE_THIRD].altered = false;

  chord.tones[ROLE_FIFTH].octaveOffset = 0;
  chord.tones[ROLE_FIFTH].present = true;
  chord.tones[ROLE_FIFTH].altered = false;

  chord.toneCount = 3; // triad
}

// ============================================================
// STAGE 3: SIZE — diatonic extension traversal
// ============================================================

// Extension roles in order: 7th, 9th, 11th, 13th
// Each step advances by 2 scale degrees from the previous
// With octave carry: when degree >= scaleNoteCount, wrap and +1 octave
const ChordRole sizeRoles[4] = {
  ROLE_SEVENTH, ROLE_NINTH, ROLE_ELEVENTH, ROLE_THIRTEENTH
};

void applySize(ChordStructure &chord, uint8_t size) {
  if (size == 0) return; // Tri: no extensions

  const ScaleInfo scale = buildScaleInfo();

  // Triad uses degrees 0, 2, 4 (root, 3rd, 5th)
  // Extensions start at degree 6 (7th), then 8 (9th), 10 (11th), 12 (13th)
  // Each is 2 scale degrees apart
  uint8_t extDegrees[4] = {6, 8, 10, 12};

  for (uint8_t s = 0; s < size && s < 4; s++) {
    uint8_t degree = extDegrees[s];
    uint8_t octaveOff = 0;

    // Diatonic carry: wrap degree around scale length
    while (degree >= scale.count) {
      degree -= scale.count;
      octaveOff++;
    }

    uint8_t pc = scale.pitchClasses[degree];

    chord.tones[sizeRoles[s]].pitchClass = pc;
    chord.tones[sizeRoles[s]].octaveOffset = octaveOff;
    chord.tones[sizeRoles[s]].present = true;
    chord.tones[sizeRoles[s]].altered = false;
    chord.toneCount++;
  }
}

// ============================================================
// STAGE 4: EXTENSION — structurally add extension roles
// ============================================================

void applyExtension(ChordStructure &chord, uint8_t extension) {
  if (extension == 0) return; // Off

  const ScaleInfo scale = buildScaleInfo();

  // Extension values: 1=9th, 2=11th, 3=13th
  // sizeRoles[0]=7th, [1]=9th, [2]=11th, [3]=13th
  // Extension 1=9th → sizeRoles[1], Extension 2=11th → sizeRoles[2], Extension 3=13th → sizeRoles[3]
  uint8_t roleIndex = extension; // 1→1, 2→2, 3→3

  if (roleIndex >= 4) return;

  ChordRole role = sizeRoles[roleIndex];

  if (!chord.tones[role].present) {
    // Not yet present — add it with diatonic carry
    uint8_t extDegrees[4] = {6, 8, 10, 12};
    uint8_t degree = extDegrees[roleIndex];
    uint8_t octaveOff = 0;
    while (degree >= scale.count) {
      degree -= scale.count;
      octaveOff++;
    }
    chord.tones[role].pitchClass = scale.pitchClasses[degree];
    chord.tones[role].octaveOffset = octaveOff;
    chord.tones[role].present = true;
    chord.tones[role].altered = false;
    chord.toneCount++;
  }
}

// ============================================================
// STAGE 5: ADDED TONE — add non-chord tones
// ============================================================

// Added tone pitch classes relative to root (diatonic where possible)
// Off=absent, 2nd=scale degree 1, 4th=scale degree 3, 6th=scale degree 5, 13th=scale degree 5+oct
const uint8_t addedDegrees[4] = {1, 3, 5, 5}; // scale degrees for 2, 4, 6, 13
const uint8_t addedOctaves[4] = {0, 0, 0, 1}; // octave offset for each

void applyAdded(ChordStructure &chord, uint8_t added) {
  if (added == 0) return; // Off

  const ScaleInfo scale = buildScaleInfo();
  uint8_t idx = added - 1; // 1→0, 2→1, 3→2, 4→3

  if (idx >= 4) return;

  uint8_t degree = addedDegrees[idx];
  uint8_t octaveOff = addedOctaves[idx];

  // Wrap degree for 13th (may exceed scale length)
  while (degree >= scale.count) {
    degree -= scale.count;
    octaveOff++;
  }

  uint8_t pc = scale.pitchClasses[degree];

  chord.tones[ROLE_ADDED].pitchClass = pc;
  chord.tones[ROLE_ADDED].octaveOffset = octaveOff;
  chord.tones[ROLE_ADDED].present = true;
  chord.tones[ROLE_ADDED].altered = false;
  chord.toneCount++;
}

// ============================================================
// STAGE 6: SUSPENSION — replace 3rd pitch class
// ============================================================

void applySuspension(ChordStructure &chord, uint8_t suspension) {
  if (suspension == 0) return; // Off

  const ScaleInfo scale = buildScaleInfo();

  // Suspension replaces the 3rd's pitch class
  // Sus2: replace 3rd with 2nd scale degree (degree 1)
  // Sus4: replace 3rd with 4th scale degree (degree 3)
  // 2+4: replace 3rd with 2nd AND add 4th (handled specially)
  uint8_t targetDegree;
  if (suspension == 1) {
    targetDegree = 1; // 2nd
  } else if (suspension == 2) {
    targetDegree = 3; // 4th
  } else {
    targetDegree = 1; // 2+4: replace 3rd with 2nd, then add 4th in applyAdded
  }

  uint8_t pc = scale.pitchClasses[targetDegree % scale.count];
  chord.tones[ROLE_THIRD].pitchClass = pc;
  // Note: Suspension does NOT change the role or present flag

  // For 2+4: also add the 4th as an added tone
  if (suspension == 3) {
    uint8_t fourthPC = scale.pitchClasses[3 % scale.count];
    // Check if Added tone is already present
    if (chord.tones[ROLE_ADDED].present) {
      // Override with 4th
      chord.tones[ROLE_ADDED].pitchClass = fourthPC;
      chord.tones[ROLE_ADDED].octaveOffset = 0;
    } else {
      chord.tones[ROLE_ADDED].pitchClass = fourthPC;
      chord.tones[ROLE_ADDED].octaveOffset = 0;
      chord.tones[ROLE_ADDED].present = true;
      chord.tones[ROLE_ADDED].altered = false;
      chord.toneCount++;
    }
  }
}

// ============================================================
// STAGE 7: ALTERATION — chromatically shift present tones
// ============================================================

// Alteration target mapping:
// 1=b5 → ROLE_FIFTH, 2=#5 → ROLE_FIFTH
// 3=b9 → ROLE_NINTH, 4=#9 → ROLE_NINTH
// 5=#11 → ROLE_ELEVENTH
// 6=b13 → ROLE_THIRTEENTH
const ChordRole alterTargets[6] = {
  ROLE_FIFTH, ROLE_FIFTH, ROLE_NINTH, ROLE_NINTH, ROLE_ELEVENTH, ROLE_THIRTEENTH
};
const int8_t alterShifts[6] = {
  -1, +1, -1, +1, +1, -1
};

void applyAlteration(ChordStructure &chord, uint8_t alteration) {
  if (alteration == 0) return; // Off

  uint8_t idx = alteration - 1;
  if (idx >= 6) return;

  ChordRole target = alterTargets[idx];

  // Only alter if the target role is present
  if (!chord.tones[target].present) return;

  chord.tones[target].pitchClass = (chord.tones[target].pitchClass + alterShifts[idx] + 12) % 12;
  chord.tones[target].altered = true;
}

// ============================================================
// STAGE 8: DENSITY — remove tones for voicing sparsity
// ============================================================

void applyDensity(ChordStructure &chord, uint8_t density) {
  if (density == 0) return; // Full: no removal

  // Reduced: hide 5th
  if (density >= 1 && chord.tones[ROLE_FIFTH].present) {
    chord.tones[ROLE_FIFTH].present = false;
    chord.toneCount--;
  }

  // Sparse: also hide 7th
  if (density >= 2 && chord.tones[ROLE_SEVENTH].present) {
    chord.tones[ROLE_SEVENTH].present = false;
    chord.toneCount--;
  }
}

// ============================================================
// STAGE 9: TOP VOICE / BASS — role-based voicing reorder
// ============================================================

// Find a present tone by role, fallback to ROLE_ROOT
ChordTone* findToneByRole(ChordStructure &chord, ChordRole role) {
  if (chord.tones[role].present) return &chord.tones[role];
  return &chord.tones[ROLE_ROOT]; // fallback
}

// ============================================================
// STAGE 10: SPACING — minimum intervals between voices
// ============================================================

// Spacing intervals (semitones) for Close/Med/Open/Wide
const uint8_t spacingIntervals[4] = {0, 3, 4, 5}; // m3, M3, P4

// ============================================================
// STAGE 11: SPREAD — octave doublings
// ============================================================

// Spread configuration: which roles get octave doublings
// Tight: none, Med: root+12, Open: root+12 + 5th+12, Wide: root+24
const uint8_t spreadDoublings[4][3] = {
  {0, 0, 0},           // Tight: no doublings
  {1, 0, 0},           // Med: root+12
  {1, 1, 0},           // Open: root+12, 5th+12
  {1, 0, 1}            // Wide: root+12, root+24
};
const uint8_t spreadOctaves[4][3] = {
  {0, 0, 0},
  {1, 0, 0},
  {1, 1, 0},
  {1, 2, 0}
};

// ============================================================
// STAGE 12: REGISTER — global transposition
// ============================================================

// Register offsets (semitones)
const int8_t registerOffsets[4] = {-12, -6, +6, +12};

// ============================================================
// STAGE 13: MIDI OUTPUT — voicing pass
// ============================================================

// Convert ChordStructure to sorted MIDI notes
// Returns number of notes (up to maxNotes)
uint8_t chordStructureToMIDINotes(
  const ChordStructure &chord,
  uint8_t *midiNotes,      // output array
  uint8_t maxNotes,
  uint8_t topVoice,        // 0=Root, 1=3rd, 2=5th, 3=7th
  uint8_t bass,            // 0=Root, 1=3rd, 2=5th, 3=7th
  uint8_t spacing,         // 0=Close, 1=Med, 2=Open, 3=Wide
  uint8_t spread,          // 0=Tight, 1=Med, 2=Open, 3=Wide
  uint8_t reg              // 0=Low, 1=LoMd, 2=HiMd, 3=High
) {
  uint8_t count = 0;

  // Collect present tones as MIDI notes
  for (uint8_t i = 0; i < ROLE_COUNT && count < maxNotes; i++) {
    if (!chord.tones[i].present) continue;
    uint8_t midi = chord.rootMIDI + chord.tones[i].pitchClass
                   + (chord.tones[i].octaveOffset * 12);
    midiNotes[count++] = midi;
  }

  if (count == 0) return 0;

  // Sort ascending
  for (uint8_t i = 0; i < count - 1; i++) {
    for (uint8_t j = i + 1; j < count; j++) {
      if (midiNotes[j] < midiNotes[i]) {
        uint8_t tmp = midiNotes[i];
        midiNotes[i] = midiNotes[j];
        midiNotes[j] = tmp;
      }
    }
  }

  // Apply spacing: enforce minimum intervals between adjacent notes
  if (spacing > 0 && count > 1) {
    uint8_t minInterval = spacingIntervals[spacing];
    for (uint8_t i = 1; i < count; i++) {
      uint8_t minNote = midiNotes[i - 1] + minInterval;
      if (midiNotes[i] < minNote) {
        midiNotes[i] = minNote;
      }
    }
  }

  // Apply spread: octave doublings
  if (spread > 0 && count < maxNotes) {
    // Spread adds root an octave up (or two octaves for Wide)
    uint8_t rootMIDI = midiNotes[0]; // root should be first after sort
    if (spread == 1 || spread == 2) {
      midiNotes[count++] = rootMIDI + 12;
    }
    if (spread == 3) {
      midiNotes[count++] = rootMIDI + 24;
    }
    // Re-sort after spread
    for (uint8_t i = 0; i < count - 1; i++) {
      for (uint8_t j = i + 1; j < count; j++) {
        if (midiNotes[j] < midiNotes[i]) {
          uint8_t tmp = midiNotes[i];
          midiNotes[i] = midiNotes[j];
          midiNotes[j] = tmp;
        }
      }
    }
  }

  // Apply register: global transposition
  int8_t regOffset = registerOffsets[reg];
  for (uint8_t i = 0; i < count; i++) {
    midiNotes[i] = constrain(midiNotes[i] + regOffset, 36, 96);
  }

  // Remove duplicates
  uint8_t unique[maxNotes];
  uint8_t uniqueCount = 0;
  for (uint8_t i = 0; i < count; i++) {
    bool dup = false;
    for (uint8_t j = 0; j < uniqueCount; j++) {
      if (unique[j] == midiNotes[i]) { dup = true; break; }
    }
    if (!dup) unique[uniqueCount++] = midiNotes[i];
  }

  // Copy back
  for (uint8_t i = 0; i < uniqueCount && i < maxNotes; i++) {
    midiNotes[i] = unique[i];
  }

  return uniqueCount;
}

void stopChord() {
  for (uint8_t i = 0; i < chordNoteCount; i++) {
    Control_Surface.sendNoteOff({chordNotesPlaying[i], Channel_1}, 0);
  }
  chordNoteCount = 0;
}

void playChord(int8_t midiNote) {
  stopChord();
  if (midiNote < 0) return;

  uint8_t quality = chord.params[0][0];
  uint8_t size = chord.params[0][1];
  uint8_t ext = chord.params[0][2];
  uint8_t tension = chord.params[0][3];

  // F2 params
  uint8_t added = chord.params[1][0];
  uint8_t susp = chord.params[1][1];
  uint8_t alter = chord.params[1][2];
  uint8_t spread = chord.params[1][3];

  // F3 params
  uint8_t inversion = chord.params[2][0];
  uint8_t spc = chord.params[2][1];
  uint8_t reg = chord.params[2][2];
  uint8_t bass = chord.params[2][3];

  // F4 params — harmonic relation determines the root offset
  uint8_t relation = chord.params[3][0];

  // Determine chord root based on selected note and harmonic relation
  int8_t chordRoot = midiNote; // default: selected note is root

  if (relation == 1) {
    // Selected note is the 3rd → root is 4 semitones below (major) or 3 below (minor)
    // Use quality to determine interval
    uint8_t thirdInterval = qualityIntervals[quality][1];
    chordRoot = midiNote - thirdInterval;
  } else if (relation == 2) {
    // Selected note is the 5th → root is 7 semitones below
    chordRoot = midiNote - 7;
  } else if (relation == 3) {
    // Selected note is the 7th → root is 10 semitones below (dom7)
    chordRoot = midiNote - 10;
  } else if (relation == 4) {
    // Selected note is an extension → root is 14 below (9th)
    chordRoot = midiNote - 14;
  }

  // Clamp chord root to reasonable range
  chordRoot = constrain(chordRoot, 36, 84);

  // Get scale pattern rotated by keyRoot
  uint16_t pattern = scalePatterns[keyScale];
  uint16_t rotated = ((pattern << keyRoot) | (pattern >> (12 - keyRoot))) & 0xFFF;

  // Build list of scale step intervals (semitone distances from root for each scale degree)
  uint8_t scaleSteps[12];
  uint8_t scaleStepCount = 0;
  for (int i = 0; i < 12; i++) {
    if (rotated & (1 << i)) {
      scaleSteps[scaleStepCount++] = i;
    }
  }

  // Build chord note list (MIDI notes, sorted)
  uint8_t notes[12];
  uint8_t noteCount = 0;

  // 1. Add triad from quality — derived from scale degrees
  // quality determines which scale degrees: Maj=0,2,4 | Min=0,2,4 with flattened 3rd | etc.
  // Base triad uses scale degrees 0, 2, 4 (root, 3rd, 5th)
  uint8_t triadDegrees[3] = {0, 2, 4};
  // quality adjustments: shift the 3rd and/or 5th by ±1 semitone
  int8_t triadShift[6][3] = {
    {0, 0, 0},   // Major: root, 3rd, 5th as-is
    {0, -1, 0},  // Minor: flatten 3rd
    {0, -1, -1}, // Diminished: flatten 3rd and 5th
    {0, 0, +1},  // Augmented: sharpen 5th
    {0, -1, 0},  // Sus2: handled separately
    {0, 0, 0},   // Sus4: handled separately
  };
  for (int i = 0; i < 3; i++) {
    uint8_t deg = triadDegrees[i];
    if (deg < scaleStepCount) {
      int8_t shift = (quality < 6) ? triadShift[quality][i] : 0;
      notes[noteCount++] = chordRoot + scaleSteps[deg] + shift;
    }
  }

  // Sus2/Sus4: replace 3rd (degree 2) with degree 1 (2nd) or degree 3 (4th)
  if (quality == 4 && scaleStepCount > 1) {
    // Sus2: replace 3rd with 2nd scale degree
    if (noteCount > 1) notes[1] = chordRoot + scaleSteps[1];
  } else if (quality == 5 && scaleStepCount > 3) {
    // Sus4: replace 3rd with 4th scale degree
    if (noteCount > 1) notes[1] = chordRoot + scaleSteps[3];
  }

  // 2. Add scale degrees for size (7th, 9th, 11th, 13th)
  // Use scale degrees 6, 8, 10, 12 (7th=6th degree above root, etc.)
  uint8_t sizeDegrees[] = {6, 8, 10, 12};
  for (uint8_t s = 0; s < sizeCount[size]; s++) {
    uint8_t deg = sizeDegrees[s];
    if (deg < scaleStepCount) {
      notes[noteCount++] = chordRoot + scaleSteps[deg];
    }
  }

  // 3. Apply extension — modify specific tones
  if (ext > 0 && noteCount > 3) {
    // ext 1=b9, 2=#9, 3=#11, 4=b13
    // These modify the9th,11th,13th if present
    switch (ext) {
      case 1: // b9 — flatten 9th (index 4 in extended chords)
        if (noteCount > 4) notes[4] -= 1;
        break;
      case 2: // #9 — sharpen 9th
        if (noteCount > 4) notes[4] += 1;
        break;
      case 3: // #11 — sharpen 11th (index 5)
        if (noteCount > 5) notes[5] += 1;
        break;
      case 4: // b13 — flatten 13th (index 6)
        if (noteCount > 6) notes[6] -= 1;
        break;
    }
  }

  // 4. Apply added tone
  if (added > 0) {
    uint8_t addedInterval = 0;
    switch (added) {
      case 1: addedInterval = 2; break;  // 2nd
      case 2: addedInterval = 5; break;  // 4th
      case 3: addedInterval = 9; break;  // 6th
      case 4: addedInterval = 21; break; // 13th
    }
    if (addedInterval > 0) {
      notes[noteCount++] = chordRoot + addedInterval;
    }
  }

  // 5. Apply suspension — replace 3rd with scale-derived 2nd or 4th
  if (susp > 0 && noteCount > 1) {
    switch (susp) {
      case 1: // Sus2 — replace 3rd with 2nd scale degree
        if (scaleStepCount > 1) notes[1] = chordRoot + scaleSteps[1];
        break;
      case 2: // Sus4 — replace 3rd with 4th scale degree
        if (scaleStepCount > 3) notes[1] = chordRoot + scaleSteps[3];
        break;
      case 3: // 2+4 — replace 3rd with both 2nd and 4th
        if (scaleStepCount > 1) notes[1] = chordRoot + scaleSteps[1];
        if (scaleStepCount > 3) notes[noteCount++] = chordRoot + scaleSteps[3];
        break;
    }
  }

  // 6. Apply alteration
  if (alter > 0) {
    switch (alter) {
      case 1: // b5 — flatten 5th (index 2)
        if (noteCount > 2) notes[2] -= 1;
        break;
      case 2: // #5 — sharpen 5th
        if (noteCount > 2) notes[2] += 1;
        break;
      case 3: // b9 — flatten 9th
        if (noteCount > 4) notes[4] -= 1;
        break;
      case 4: // #9 — sharpen 9th
        if (noteCount > 4) notes[4] += 1;
        break;
      case 5: // #11 — sharpen 11th
        if (noteCount > 5) notes[5] += 1;
        break;
    }
  }

  // 7. Apply tension — remove chord tones
  // Tension 0=Low (all), 1=Med (remove 5th), 2=High (remove 5th and 7th)
  if (tension >= 1 && noteCount > 2) {
    // Remove 5th (index 2) by shifting notes down
    for (uint8_t i = 2; i < noteCount - 1; i++) {
      notes[i] = notes[i + 1];
    }
    noteCount--;
  }
  if (tension >= 2 && noteCount > 3) {
    // Remove 7th (now at index 2 after 5th removal, or index 3 originally)
    // After removing 5th, 7th is at index 2 (was index 3)
    for (uint8_t i = 2; i < noteCount - 1; i++) {
      notes[i] = notes[i + 1];
    }
    noteCount--;
  }

  // 8. Apply spread — duplicate notes across octaves
  if (spread >= 2 && noteCount > 0) {
    uint8_t extraOct = (spread == 2) ? 12 : (spread == 3) ? 24 : 0;
    if (extraOct > 0 && noteCount < 8) {
      notes[noteCount++] = notes[0] + extraOct;
    }
  }

  // 9. Apply voicing — inversion (top voice)
  // Move the inversion target note to the top
  if (inversion > 0 && noteCount > 1) {
    uint8_t invIdx = inversion;
    if (invIdx < noteCount) {
      uint8_t topNote = notes[invIdx];
      // Shift notes down to fill gap
      for (uint8_t i = invIdx; i < noteCount - 1; i++) {
        notes[i] = notes[i + 1];
      }
      notes[noteCount - 1] = topNote;
    }
  }

  // 10. Apply bass — move bass note to bottom
  if (bass > 0 && bass < noteCount) {
    uint8_t bassNote = notes[bass];
    // Shift notes up to make room at bottom
    for (uint8_t i = bass; i > 0; i--) {
      notes[i] = notes[i - 1];
    }
    notes[0] = bassNote;
  }

  // 11. Apply register — transpose entire chord
  int8_t regOffset = 0;
  switch (reg) {
    case 0: regOffset = -12; break; // Low
    case 1: regOffset = -6; break;  // Lo-Med (slightly lower)
    case 2: regOffset = 6; break;   // Hi-Med (slightly higher)
    case 3: regOffset = 12; break;  // High
  }

  // 12. Apply spacing — adjust intervals between voices
  // Spacing 0=Close (as-is), 1=Med (spread 3rds to 4ths), 2=Open (to 5ths), 3=Wide (to octaves)
  if (spc > 0 && noteCount > 2) {
    uint8_t spreadInterval = spc + 2; // 3=4ths, 4=5ths, 5=6ths
    for (uint8_t i = 1; i < noteCount; i++) {
      uint8_t minNote = notes[i - 1] + spreadInterval;
      if (notes[i] < minNote) {
        notes[i] = minNote;
      }
    }
  }

  // Sort notes
  for (uint8_t i = 0; i < noteCount - 1; i++) {
    for (uint8_t j = i + 1; j < noteCount; j++) {
      if (notes[j] < notes[i]) {
        uint8_t tmp = notes[i];
        notes[i] = notes[j];
        notes[j] = tmp;
      }
    }
  }

  // Apply register offset and clamp to MIDI range
  for (uint8_t i = 0; i < noteCount; i++) {
    notes[i] = constrain(notes[i] + regOffset, 36, 96);
  }

  // Remove duplicates
  uint8_t unique[12];
  uint8_t uniqueCount = 0;
  for (uint8_t i = 0; i < noteCount; i++) {
    bool dup = false;
    for (uint8_t j = 0; j < uniqueCount; j++) {
      if (unique[j] == notes[i]) { dup = true; break; }
    }
    if (!dup) unique[uniqueCount++] = notes[i];
  }

  // Send MIDI notes
  chordNoteCount = 0;
  for (uint8_t i = 0; i < uniqueCount && i < 8; i++) {
    Control_Surface.sendNoteOn({unique[i], Channel_1}, 100);
    chordNotesPlaying[chordNoteCount++] = unique[i];
  }

  // Light keyboard LEDs for played notes
  updateKeyModeLEDs();
  for (uint8_t i = 0; i < chordNoteCount; i++) {
    uint8_t pc = chordNotesPlaying[i] % 12;
    leds[keyLedIndex[pc]] = CRGB(0, 40, 80); // brighter blue for played chord
  }
  FastLED.show();
}

// MIDI LED controller starting at B3 (note 59)
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

void readKeyModeEncoders() {
  for (int i = 0; i < 8; i++) {
    uint8_t newState = (digitalRead(keyEncPins[i][0]) << 1) | digitalRead(keyEncPins[i][1]);
    uint8_t transition = (keyEncState[i] << 2) | newState;
    keyEncState[i] = newState;
    static const int8_t encoderTable[16] = { 0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0 };
    int8_t delta = -encoderTable[transition]; // negated for correct direction
    if (delta == 0) continue;
    if (i == 3) {
      // Chord: clockwise=ON, counterclockwise=OFF
      if (delta > 0) keyChordOn = true;
      else if (delta < 0) {
        keyChordOn = false;
        // Stop any sustaining chord
        if (chord.active) {
          stopChord();
          chord.noteRoot = -1;
          chord.heldKeyIndex = -1;
          chord.heldCount = 0;
          updateKeyModeLEDs();
        }
      }
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
        if (chord.active && chord.heldKeyIndex >= 0) {
          stopChord();
          int8_t newNote = 48 + chord.heldKeyIndex + (keyOctave - 3) * 12;
          chord.noteRoot = newNote;
          playChord(newNote);
        }
        break;
      }
      case 1: {
        keyScale = constrain(keyScale + step, 0, numScales - 1);
        updateKeyModeLEDs();
        if (chord.active && chord.heldKeyIndex >= 0) {
          stopChord();
          playChord(chord.noteRoot);
        }
        break;
      }
      case 2: {
        keyRoot = constrain(keyRoot + step, 0, 11);
        updateKeyModeLEDs();
        if (chord.active && chord.heldKeyIndex >= 0) {
          stopChord();
          playChord(chord.noteRoot);
        }
        break;
      }
      case 4: case 5: case 6: case 7: {
        if (!chord.active) break;
        uint8_t enc = i - 4;
        uint8_t maxVal = chordParamMax(chord.pageIndex, enc);
        chord.params[chord.pageIndex][enc] = constrain(chord.params[chord.pageIndex][enc] + step, 0, maxVal);
        if (chord.noteRoot >= 0) playChord(chord.noteRoot);
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
    if (chord.active) {
      u8g2.setCursor(24, 12);
      u8g2.print("DISCOVERY");
      char pageBuf[8];
      snprintf(pageBuf, sizeof(pageBuf), "M1/F%d", chord.pageIndex + 1);
      u8g2.setCursor(104, 12);
      u8g2.print(pageBuf);
      return;
    }
    if (keyModeActive) {
      u8g2.setCursor(42, 12);
      u8g2.print("KEY MODE");
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

    const char* labels[] = { "Oct", "Scale", "Root", "Chord" };
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

  void displayDiscoveryMode() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    drawHeader();

    const int colX[] = { 2, 26, 60, 90 };
    const int colW[] = { 18, 30, 24, 30 };

    // Row 1-2: enc0-3 (KEY MODE params) — same layout as displayKeyMode
    const char* row1Labels[] = { "Oct", "Scale", "Root", "Chord" };
    const char* row1Values[4];
    char octBuf[5];
    snprintf(octBuf, sizeof(octBuf), "%d", keyOctave);
    row1Values[0] = octBuf;
    row1Values[1] = scaleNames[keyScale];
    row1Values[2] = rootNames[keyRoot];
    row1Values[3] = keyChordOn ? "ON" : "OFF";
    for (int i = 0; i < 4; i++) {
      u8g2.setCursor(colX[i], 24);
      u8g2.print(row1Labels[i]);
      int valW = u8g2.getStrWidth(row1Values[i]);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 33);
      u8g2.print(row1Values[i]);
    }

    // Row 3-4: enc4-7 (F page params)
    for (int i = 0; i < 4; i++) {
      const char* label = chordParamLabel(chord.pageIndex, i);
      const char* value = chordParamValue(chord.pageIndex, i);
      u8g2.setCursor(colX[i], 45);
      u8g2.print(label);
      int valW = u8g2.getStrWidth(value);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 54);
      u8g2.print(value);
    }

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

    // In KEY MODE: control buttons do NOT send MIDI (except SEQ, already handled)
    // Handle M1 and F1-F4 for Discovery Mode
    if (keyModeActive && strcmp(button->name, "ENC") != 0) {
      if (strcmp(button->name, "M1") == 0) {
        // Toggle Discovery Mode
        chord.active = !chord.active;
        if (chord.active) {
          chord.pageIndex = 0;
          Serial.println("  -> Entered Discovery Mode");
        } else {
          stopChord();
          chord.noteRoot = -1;
          chord.heldKeyIndex = -1;
          chord.heldCount = 0;
          Serial.println("  -> Exited Discovery Mode");
        }
        return;
      }
      if (chord.active) {
        // F1-F4 switch pages
        if (strcmp(button->name, "F1") == 0) { chord.pageIndex = 0; Serial.println("  -> F1 selected"); return; }
        if (strcmp(button->name, "F2") == 0) { chord.pageIndex = 1; Serial.println("  -> F2 selected"); return; }
        if (strcmp(button->name, "F3") == 0) { chord.pageIndex = 2; Serial.println("  -> F3 selected"); return; }
        if (strcmp(button->name, "F4") == 0) { chord.pageIndex = 3; Serial.println("  -> F4 selected"); return; }
      }
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
      }
      BankLEDHandler::enterBankMode(BANK_ENC);
      Serial.println("  -> Entered ENCODER mode");
    } 
    else if (strcmp(button->name, "KEY") == 0) {
      // Don't enter bank mode yet — wait for release to detect tap vs hold
      keyPadTouched = false;
      Serial.println("  -> KEY pressed (waiting for release)");
    }
    // Other control buttons just send MIDI

  } else {
    Serial.print("CONTROL released: ");
    Serial.println(button->name);
    
    // In KEY MODE: skip note off (no MIDI was sent)
    if (keyModeActive) return;

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
            if (keyModeActive) {
              saveKeyEncPositions();
              disableAllEncoders();
              syncKeyEncodersToParams();
              updateKeyModeLEDs();
              Serial.println("  -> Entered KEY MODE display");
              display.displayKeyMode();
            } else {
              if (chord.active) {
                stopChord();
                chord.noteRoot = -1;
                chord.heldKeyIndex = -1;
                chord.heldCount = 0;
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

void handleKeyboardButton(ButtonMap* button, bool pressed) {
  button->isPressed = pressed;

  // Map button to keyNotes index (midiNote 84-95 → index 0-11)
  int keyIndex = button->midiNote - 84;
  if (keyIndex < 0 || keyIndex >= 12) return;

  // Discovery Mode: play chord based on pressed note
  if (chord.active) {
    if (pressed) {
      int8_t midiNote = 48 + keyIndex + (keyOctave - 3) * 12;
      chord.noteRoot = midiNote;
      chord.heldKeyIndex = keyIndex;
      chord.heldCount++;
      playChord(midiNote);
    } else {
      chord.heldCount--;
      if (!keyChordOn && chord.heldCount <= 0) {
        // Latch OFF: stop chord when all notes released
        stopChord();
        chord.noteRoot = -1;
        chord.heldKeyIndex = -1;
        chord.heldCount = 0;
        updateKeyModeLEDs();
      } else if (chord.heldCount < 0) {
        chord.heldCount = 0;
      }
    }
    return;
  }

  // Filter notes not in the selected scale (unless Chromatic is selected)
  if (keyScale != 0 && !isNoteInScale(keyIndex)) {
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

  // Set touch state — will be processed by Control_Surface.loop() → update()
  keyNotes[keyIndex].setTouchState(pressed);

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
  gestureHelper.begin(300, 200, 10);

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
  
  // Check for encoder changes
  if (!keyModeActive) {
    checkEncoderChanges();
  }
  
  // Update display based on mode (skip if in sequencer mode)
  static unsigned long lastDisplayUpdate = 0;
  unsigned long now = millis();
  
  if (!seqModeActive && !keyModeActive) {
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
      if (chord.active)
        display.displayDiscoveryMode();
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