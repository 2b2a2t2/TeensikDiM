#include <FastLED.h>
#include <Control_Surface.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "src/MPR121_GestureHelper.h"

USING_CS_NAMESPACE;

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
      else if (delta < 0) keyChordOn = false;
      continue;
    }
    keyEncAccum[i] += delta;
    if (abs(keyEncAccum[i]) < keyEncSensitivity) continue;
    int8_t step = (keyEncAccum[i] > 0) ? 1 : -1;
    keyEncAccum[i] = 0;
    switch (i) {
      case 0:
        keyOctave = constrain(keyOctave + step, -2, 8);
        keyboardBank.select(keyOctave + 2);
        break;
      case 1: keyScale = constrain(keyScale + step, 0, numScales - 1); updateKeyModeLEDs(); break;
      case 2: keyRoot = constrain(keyRoot + step, 0, 11); updateKeyModeLEDs(); break;
      case 4: /* TODO: KEY enc4 function */ break;
      case 5: /* TODO: KEY enc5 function */ break;
      case 6: /* TODO: KEY enc6 function */ break;
      case 7: /* TODO: KEY enc7 function */ break;
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
      u8g2.setCursor(colX[i], 26);
      u8g2.print(labels[i]);
      int valW = u8g2.getStrWidth(values[i]);
      int vx = colX[i] + (colW[i] - valW) / 2;
      u8g2.setCursor(vx, 40);
      u8g2.print(values[i]);
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

    // Always send the MIDI note for control buttons on Channel_1
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

  // In KEY mode, only send notes that are in the selected scale
  if (keyModeActive && !isNoteInScale(keyIndex)) {
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