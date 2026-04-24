#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#define SDA_PIN    5
#define SCL_PIN    4
#define BUTTON_PIN 2
#define TOUCH_PIN  3

Adafruit_SH1106G display(128, 64, &Wire, -1);

// ── Time (stored 24hr internally) ───────────────────────────
int hours   = 12;
int minutes = 45;
int seconds = 0;
unsigned long lastTick = 0;

// ── Display sleep ────────────────────────────────────────────
bool          displayOn      = true;
unsigned long lastActivityMs = 0;
const unsigned long SLEEP_TIMEOUT = 5000; // 5 seconds of inactivity

// ── Touch sensor (HW-763: HIGH = touched) ───────────────────
bool lastRawTouch      = LOW;
bool lastStableTouch   = LOW;
unsigned long touchDebounce = 0;
const unsigned long TOUCH_DEBOUNCE = 40; // ms

// Touch press timing (for short vs long press via touch)
unsigned long touchPressStart  = 0;
bool          touchLongHandled = false;

// ── Button (pin 2, kept for set-mode long-press) ─────────────
bool lastButtonState = HIGH;
bool lastRawReading  = HIGH;
unsigned long pressStart    = 0;
bool longPressHandled = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// ── Screens: 0=clock, 1=face ─────────────────────────────────
int  currentScreen = 0;
bool setMode       = false;
int  setStep       = 0;

// ── Blink (set-mode cursor) ──────────────────────────────────
unsigned long lastBlink = 0;
bool blinkOn = true;

// ── Face animation ───────────────────────────────────────────
#define EYE_OPEN   0
#define EYE_BLINK  1
#define EYE_SQUINT 2
#define EYE_CLOSED 3

struct Vec2 { int x; int y; };

int  eyeState    = EYE_OPEN;
Vec2 pupilOffset = {0, 0};

unsigned long faceTimer   = 0;
int           facePhase   = 0;
int           faceSubStep = 0;

// ─────────────────────────────────────────────────────────────
// 12HR HELPERS
// ─────────────────────────────────────────────────────────────
int to12hr(int h24) {
  int h = h24 % 12;
  return (h == 0) ? 12 : h;
}
String getAmPm(int h24) {
  return (h24 < 12) ? "AM" : "PM";
}

// ─────────────────────────────────────────────────────────────
// DISPLAY SLEEP HELPERS
// ─────────────────────────────────────────────────────────────
void wakeDisplay() {
  if (!displayOn) {
    display.oled_command(0xAF); // SH1106 command: Display ON
    displayOn = true;
  }
  lastActivityMs = millis();
}

void checkSleep() {
  if (displayOn && (millis() - lastActivityMs >= SLEEP_TIMEOUT)) {
    display.clearDisplay();
    display.display();
    display.oled_command(0xAE); // SH1106 command: Display OFF
    displayOn = false;
  }
}

// ─────────────────────────────────────────────────────────────
// SETUP & LOOP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN,  INPUT);          // HW-763 drives the pin, no pull needed

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  if (!display.begin(0x3C, true)) {
    Serial.println("Display init failed!");
    while (true);
  }

  display.setTextColor(SH110X_WHITE);
  display.clearDisplay();
  display.display();

  lastActivityMs = millis();
  faceTimer      = millis();
}

void loop() {
  updateTime();
  handleTouch();   // touch = wake + screen nav
  handleButton();  // button = set-mode only
  updateBlink();
  checkSleep();    // auto-off after 5s

  if (displayOn) {
    if (currentScreen == 1) updateFaceAnimation();
    drawScreen();
  }
}

// ─────────────────────────────────────────────────────────────
// CLOCK
// ─────────────────────────────────────────────────────────────
void updateTime() {
  if (!setMode && millis() - lastTick >= 1000) {
    lastTick = millis();
    if (++seconds >= 60) {
      seconds = 0;
      if (++minutes >= 60) {
        minutes = 0;
        if (++hours >= 24) hours = 0;
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────
// TOUCH HANDLER
// HW-763 is HIGH while finger is present, LOW when released.
// Tap  = touch released in < 600ms  → wake OR cycle screens
// Hold = touch held ≥ 600ms         → (reserved / back from face)
// ─────────────────────────────────────────────────────────────
void handleTouch() {
  bool raw = digitalRead(TOUCH_PIN);

  // Debounce
  if (raw != lastRawTouch) {
    touchDebounce = millis();
    lastRawTouch  = raw;
  }
  if (millis() - touchDebounce <= TOUCH_DEBOUNCE) return;

  bool stable = raw;

  // Rising edge → finger just landed
  if (lastStableTouch == LOW && stable == HIGH) {
    touchPressStart  = millis();
    touchLongHandled = false;
  }

  // While held → check for long press
  if (stable == HIGH && !touchLongHandled) {
    if (millis() - touchPressStart >= 600) {
      touchLongHandled = true;
      onTouchLong();
    }
  }

  // Falling edge → finger lifted → short tap
  if (lastStableTouch == HIGH && stable == LOW) {
    if (!touchLongHandled) {
      onTouchTap();
    }
  }

  lastStableTouch = stable;
}

void onTouchTap() {
  if (!displayOn) {
    // Display was off → just wake it, don't change screen
    wakeDisplay();
    return;
  }

  // Display already on → count as activity + cycle screen
  wakeDisplay(); // resets sleep timer

  if (setMode) {
    // In set mode a tap increments the active field
    if (setStep == 0) hours   = (hours   + 1) % 24;
    else              minutes = (minutes + 1) % 60;
    seconds  = 0;
    lastTick = millis();
  } else {
    currentScreen = (currentScreen == 0) ? 1 : 0;
    if (currentScreen == 1) {
      eyeState    = EYE_OPEN;
      pupilOffset = {0, 0};
      facePhase   = 0;
      faceSubStep = 0;
      faceTimer   = millis();
    }
  }
}

void onTouchLong() {
  wakeDisplay();
  if (currentScreen == 1) {
    currentScreen = 0;
  }
}

// ─────────────────────────────────────────────────────────────
// BUTTON HANDLER  (pin 2, LOW-active, used only for set-mode)
// Short press in set-mode = same as touch tap (increment)
// Long press = enter/advance/exit set-mode
// ─────────────────────────────────────────────────────────────
void handleButton() {
  bool rawReading = digitalRead(BUTTON_PIN);
  if (rawReading != lastRawReading) {
    lastDebounceTime = millis();
    lastRawReading   = rawReading;
  }
  if ((millis() - lastDebounceTime) <= debounceDelay) return;

  bool stable = rawReading;

  if (lastButtonState == HIGH && stable == LOW) {
    pressStart       = millis();
    longPressHandled = false;
  }
  if (stable == LOW && !longPressHandled) {
    if (millis() - pressStart >= 600) {
      handleLongPress();
      longPressHandled = true;
      wakeDisplay();
    }
  }
  if (lastButtonState == LOW && stable == HIGH) {
    if (!longPressHandled) {
      wakeDisplay();
    }
  }
  lastButtonState = stable;
}

void handleLongPress() {
  if (currentScreen == 1) {
    currentScreen = 0;
    return;
  }
  if (!setMode) {
    setMode  = true;
    setStep  = 0;
    seconds  = 0;
    lastTick = millis();
  } else {
    setStep++;
    if (setStep > 1) {
      setMode = false;
      setStep = 0;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// BLINK (set-mode cursor)
// ─────────────────────────────────────────────────────────────
void updateBlink() {
  if (millis() - lastBlink >= 500) {
    blinkOn   = !blinkOn;
    lastBlink = millis();
  }
}

// ─────────────────────────────────────────────────────────────
// FACE ANIMATION SEQUENCER
// ─────────────────────────────────────────────────────────────
void updateFaceAnimation() {
  unsigned long now = millis();
  switch (facePhase) {

    case 0: // idle open
      eyeState    = EYE_OPEN;
      pupilOffset = {0, 0};
      if (now - faceTimer > 800) { facePhase = 1; faceSubStep = 0; faceTimer = now; }
      break;

    case 1: // single blink
      if (faceSubStep == 0) { eyeState = EYE_BLINK; if (now - faceTimer > 100) { faceSubStep = 1; faceTimer = now; } }
      else                  { eyeState = EYE_OPEN;  if (now - faceTimer > 120) { facePhase   = 2; faceTimer = now; } }
      break;

    case 2: // look left
      eyeState = EYE_OPEN; pupilOffset = {-4, 0};
      if (now - faceTimer > 450) { facePhase = 3; faceTimer = now; }
      break;

    case 3: // look right
      pupilOffset = {4, 0};
      if (now - faceTimer > 450) { facePhase = 4; faceTimer = now; }
      break;

    case 4: // center
      pupilOffset = {0, 0};
      if (now - faceTimer > 250) { facePhase = 5; faceTimer = now; }
      break;

    case 5: // look up
      pupilOffset = {0, -3};
      if (now - faceTimer > 400) { facePhase = 6; faceTimer = now; }
      break;

    case 6: // look down
      pupilOffset = {0, 3};
      if (now - faceTimer > 400) { facePhase = 7; faceTimer = now; }
      break;

    case 7: // center
      pupilOffset = {0, 0};
      if (now - faceTimer > 250) { facePhase = 8; faceSubStep = 0; faceTimer = now; }
      break;

    case 8: { // eye roll
      const Vec2 rollPath[] = {{4,0},{3,3},{0,4},{-3,3},{-4,0},{-3,-3},{0,-4},{3,-3}};
      pupilOffset = rollPath[faceSubStep % 8];
      if (now - faceTimer > 140) {
        faceSubStep++;
        faceTimer = now;
        if (faceSubStep >= 8) { facePhase = 9; faceSubStep = 0; pupilOffset = {0,0}; faceTimer = now; }
      }
      break;
    }

    case 9: // squint then open
      if (faceSubStep == 0) {
        eyeState = EYE_SQUINT;
        if (now - faceTimer > 350) { faceSubStep = 1; faceTimer = now; }
      } else {
        eyeState = EYE_OPEN;
        if (now - faceTimer > 300) { facePhase = 10; faceSubStep = 0; faceTimer = now; }
      }
      break;

    case 10: { // double blink fast
      eyeState = (faceSubStep % 2 == 0) ? EYE_BLINK : EYE_OPEN;
      if (now - faceTimer > 80) {
        faceSubStep++;
        faceTimer = now;
        if (faceSubStep >= 4) { facePhase = 0; faceSubStep = 0; faceTimer = now; }
      }
      break;
    }

    default: facePhase = 0; break;
  }
}

// ─────────────────────────────────────────────────────────────
// DRAW
// ─────────────────────────────────────────────────────────────
void drawScreen() {
  display.clearDisplay();
  if (setMode)                 showSetScreen();
  else if (currentScreen == 1) showFaceScreen();
  else                         showClock();
  display.display();
}

// ─────────────────────────────────────────────────────────────
// CLOCK SCREEN
// ─────────────────────────────────────────────────────────────
void showClock() {
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("LOGICSURU");

  int    h12     = to12hr(hours);
  String ampm    = getAmPm(hours);
  String timeStr = twoDigit(h12) + ":" + twoDigit(minutes) + ":" + twoDigit(seconds);

  display.setTextSize(2);
  drawCentered(timeStr, 18);

  display.setTextSize(1);
  drawCentered(ampm, 46);
}

// ─────────────────────────────────────────────────────────────
// FACE SCREEN
// ─────────────────────────────────────────────────────────────
#define EYE_W        22
#define EYE_H        20
#define EYE_SQUINT_H  8
#define EYE_BLINK_H   3
#define PUPIL_R       5
#define EYE_L_CX     38
#define EYE_R_CX     90
#define EYE_CY       26

void drawEye(int cx, int cy, int eyeH, Vec2 pupil) {
  int halfW = EYE_W / 2;
  int halfH = eyeH / 2;
  display.fillRoundRect(cx - halfW, cy - halfH, EYE_W, eyeH, 5, SH110X_WHITE);
  if (eyeH > 6) {
    display.fillRoundRect(cx - halfW + 2, cy - halfH + 2, EYE_W - 4, eyeH - 4, 4, SH110X_BLACK);
    display.fillCircle(cx + pupil.x, cy + pupil.y, PUPIL_R, SH110X_WHITE);
    display.fillCircle(cx + pupil.x + 2, cy + pupil.y - 2, 1, SH110X_BLACK);
  }
}

void showFaceScreen() {
  int eyeH;
  if      (eyeState == EYE_BLINK)  eyeH = EYE_BLINK_H;
  else if (eyeState == EYE_SQUINT) eyeH = EYE_SQUINT_H;
  else                             eyeH = EYE_H;

  Vec2 po = (eyeState == EYE_OPEN) ? pupilOffset : (Vec2){0, 0};
  drawEye(EYE_L_CX, EYE_CY, eyeH, po);
  drawEye(EYE_R_CX, EYE_CY, eyeH, po);

  // Rosy cheeks
  for (int d = -4; d <= 4; d += 2) {
    display.drawPixel(EYE_L_CX + d, EYE_CY + 16 + (abs(d) % 2), SH110X_WHITE);
    display.drawPixel(EYE_R_CX + d, EYE_CY + 16 + (abs(d) % 2), SH110X_WHITE);
  }

  // Smile arc
  int smileCX = 64, smileCY = 44, smileR = 10;
  for (int angle = 20; angle <= 160; angle += 5) {
    float rad = angle * 3.14159f / 180.0f;
    int px = smileCX + (int)(smileR * cos(rad));
    int py = smileCY + (int)(smileR * sin(rad));
    display.drawPixel(px, py,     SH110X_WHITE);
    display.drawPixel(px, py + 1, SH110X_WHITE);
  }
}

// ─────────────────────────────────────────────────────────────
// SET SCREEN
// ─────────────────────────────────────────────────────────────
void showSetScreen() {
  display.setTextSize(1);
  drawCentered("SET TIME", 2);

  int    h12  = to12hr(hours);
  String ampm = getAmPm(hours);

  String hrStr  = (setStep == 0 && !blinkOn) ? "  " : twoDigit(h12);
  String minStr = (setStep == 1 && !blinkOn) ? "  " : twoDigit(minutes);
  String secStr = twoDigit(seconds);

  display.setTextSize(2);
  drawCentered(hrStr + ":" + minStr + ":" + secStr, 16);

  if (setStep == 0) display.drawLine(16, 38, 39, 38, SH110X_WHITE);
  else              display.drawLine(52, 38, 75, 38, SH110X_WHITE);

  display.setTextSize(1);
  drawCentered(ampm, 44);
  drawCentered("Tap=+1  Hold btn=Next", 55);
}

// ─────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────
String twoDigit(int num) {
  return (num < 10 ? "0" : "") + String(num);
}

void drawCentered(String text, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, y);
  display.print(text);
}
