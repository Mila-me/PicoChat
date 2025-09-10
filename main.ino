// OnScreenKeyboard_TFT_eSPI_480_CHAT_FIX.ino
// ESP32/ESP8266 + TFT_eSPI (+dotyk), klawiatura 480 px, czat z dymkami
// - Wysłane: prawa strona
// - Odebrane: lewa strona
// - Kalibracja dotyku w LittleFS
// - Klawisz Shift (Aa) przełącza małe/duże litery
// - Wysyłanie/odbiór przez Serial

#include <TFT_eSPI.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>

// === UART framing ===
#define MY_ID        0x01          // ustaw swój ID nadawcy (0..255)
#define MAX_PAYLOAD  255           // maksymalnie 255 bajtów ładunku

// Prosty stanowy parser ramek z UART
enum RxState : uint8_t { RX_WAIT_ID, RX_WAIT_LEN, RX_WAIT_PAYLOAD };
static RxState rxState = RX_WAIT_ID;
static uint8_t rxSender = 0;
static uint8_t rxLen    = 0;
static uint8_t rxIdx    = 0;
static uint8_t rxBuf[MAX_PAYLOAD]; // tu zbieramy payload

// ==== TYPES FOR KEYBOARD (must come BEFORE any functions using Key) ====
enum KeyType : uint8_t {
  KEY_LETTER,
  KEY_SPACE,
  KEY_BACKSPACE,
  KEY_CLEAR,
  KEY_SEND,
  KEY_SHIFT
};

struct Key {
  KeyType type;       // <- MUSI BYĆ w tej definicji
  String  label;
  int16_t x, y, w, h;
  char    letter;     // bazowa mała litera dla KEY_LETTER
};

// Global vars (exactly one definition in the whole sketch)
Key  keys[80];
int  keyCount = 0;
bool shiftOn  = false;

TFT_eSPI tft = TFT_eSPI();

// =================== KALIBRACJA DOTYKU ===================
#define CALIBRATION_FILE "/TouchCalData2"
#define FILESYSTEM LittleFS
#define REPEAT_CAL true

void touch_calibrate() {
  uint16_t calData[5];
  uint8_t calDataOK = 0;

  if (!FILESYSTEM.begin()) {
    // FILESYSTEM.format(); // użyj tylko świadomie
    FILESYSTEM.begin();
  }

  if (!REPEAT_CAL && FILESYSTEM.exists(CALIBRATION_FILE)) {
    File f = FILESYSTEM.open(CALIBRATION_FILE, "r");
    if (f) {
      if (f.readBytes((char *)calData, 14) == 14) calDataOK = 1;
      f.close();
    }
  }

  if (calDataOK) {
    tft.setTouch(calData);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Kalibracja dotyku", tft.width()/2, tft.height()/2 - 20);
    tft.drawString("Dotknij wskazanych punktow",  tft.width()/2, tft.height()/2 + 10);

    tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

    File f = FILESYSTEM.open(CALIBRATION_FILE, "w");
    if (f) { f.write((const unsigned char *)calData, 14); f.close(); }

    tft.setTouch(calData);
  }
}
// =================== /KALIBRACJA DOTYKU ===================


// =================== UI / KOLORY ===================
static const int16_t SCR_W = 480;
static const int16_t SCR_H = 320;   // dostosuj do panelu

#define BG_COLOR       TFT_BLACK
#define FG_COLOR       TFT_WHITE
#define KEY_COLOR      TFT_DARKGREY
#define KEY_TXT        TFT_WHITE
#define KEY_ACTIVE     TFT_BLUE
#define FRAME_COLOR    TFT_NAVY

// Dymki
#define BUBBLE_SENT_BG     TFT_BLUE
#define BUBBLE_SENT_TXT    TFT_WHITE
#define BUBBLE_RECV_BG     TFT_DARKGREY
#define BUBBLE_RECV_TXT    TFT_WHITE

// --- pole "Napisz": zmniejszamy na 44 px, aby odzyskać miejsce ---
static const int16_t INPUT_X = 8;
static const int16_t INPUT_Y = 8;
static const int16_t INPUT_W = SCR_W - 16;
static const int16_t INPUT_H = 44;   // było 60

// --- parametry klawiatury: niższe klawisze + mniejsze odstępy ---
static const int    KEY_H   = 36;    // było 44
static const int    ROW_GAP = 2;     // było 4

// --- obszar czatu: liczymy dynamicznie, żeby się zmieścić ---
static const int16_t CHAT_X = 8;
static const int16_t CHAT_Y = INPUT_Y + INPUT_H + 6;
static       int16_t CHAT_W = SCR_W - 16;
static       int16_t CHAT_H = 0;     // policzymy w setup()

static       int16_t KBD_Y  = 0;     // policzymy w setup()

// Tekst
String inputBuffer = "";

// =================== CZAT: MODEL DANYCH ===================
struct Msg {
  String text;
  bool sent;  // true = wysłana (prawa), false = odebrana (lewa)
};

#define CHAT_MAX 30
Msg chatBuf[CHAT_MAX];
int chatCount = 0;

// =================== NARZĘDZIA RYSOWANIA ===================
void drawFrame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, const char* title=nullptr) {
  tft.drawRect(x, y, w, h, color);
  if (title) {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(FG_COLOR, BG_COLOR);
    tft.drawString(title, x + 4, y - 2);
  }
}

void drawInputArea() {
  // Pole "Napisz"
  tft.fillRect(INPUT_X, INPUT_Y, INPUT_W, INPUT_H, BG_COLOR);
  drawFrame(INPUT_X, INPUT_Y, INPUT_W, INPUT_H, FRAME_COLOR, "Napisz:");
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(FG_COLOR, BG_COLOR);
  tft.setTextWrap(true);
  tft.setTextFont(2);
  tft.setCursor(INPUT_X + 6, INPUT_Y + 8);
  tft.print(inputBuffer);

  // Kursor
  int16_t cx = tft.getCursorX();
  int16_t cy = tft.getCursorY();
  tft.drawLine(cx + 2, cy - tft.fontHeight() + 2, cx + 2, cy + 2, FG_COLOR);
}

// ----- proste zawijanie wierszy -----
int wrapTextToLines(const String &text, String *lines, int maxLines, int maxPixelWidth) {
  String cur = "";
  int count = 0;
  int start = 0;
  int n = text.length();

  auto flushLine = [&](String &ln) {
    if (ln.length() == 0) return;
    if (count < maxLines) lines[count++] = ln;
    ln = "";
  };

  while (start < n) {
    int sp = text.indexOf(' ', start);
    if (sp < 0) sp = n;
    String word = text.substring(start, sp);
    String probe = (cur.length() == 0) ? word : (cur + " " + word);
    if (tft.textWidth(probe) <= maxPixelWidth) {
      cur = probe;
    } else {
      flushLine(cur);
      if (tft.textWidth(word) > maxPixelWidth) {
        String chunk = "";
        for (int i=0;i<(int)word.length();i++) {
          String test = chunk + word[i];
          if (tft.textWidth(test) <= maxPixelWidth) {
            chunk = test;
          } else {
            flushLine(chunk);
            chunk = String(word[i]);
          }
        }
        cur = chunk;
      } else {
        cur = word;
      }
    }
    if (sp == n) break;
    start = sp + 1;
  }
  flushLine(cur);
  return count;
}

void drawChatArea() {
  tft.fillRect(CHAT_X, CHAT_Y, CHAT_W, CHAT_H, BG_COLOR);
  drawFrame(CHAT_X, CHAT_Y, CHAT_W, CHAT_H, FRAME_COLOR, "Czat:");

  const int padX = 8;
  const int bubblePad = 5;
  const int bubbleRadius = 6;
  const int vGap = 4;
  const int maxLinesPerMsg = 12;
  const int maxBubbleWidth = CHAT_W * 72 / 100;

  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);

  int y = CHAT_Y + 4;
  int lastDrawn = -1;

  for (int i = max(0, chatCount - CHAT_MAX); i < chatCount; ++i) {
    Msg m = chatBuf[i];

    String lines[maxLinesPerMsg];
    int lineCount = wrapTextToLines(m.text, lines, maxLinesPerMsg, maxBubbleWidth - 2*bubblePad);

    int lineH = tft.fontHeight();
    int bubbleH = lineCount * lineH + 2*bubblePad;
    int bubbleW = 0;
    for (int k=0;k<lineCount;k++) {
      int w = tft.textWidth(lines[k]);
      if (w > bubbleW) bubbleW = w;
    }
    bubbleW += 2*bubblePad;

    int bx = m.sent ? (CHAT_X + CHAT_W - padX - bubbleW) : (CHAT_X + padX);
    int by = y;
    if (by + bubbleH + vGap > CHAT_Y + CHAT_H - 4) break;

    uint16_t bg = m.sent ? BUBBLE_SENT_BG : BUBBLE_RECV_BG;
    uint16_t fg = m.sent ? BUBBLE_SENT_TXT : BUBBLE_RECV_TXT;
    tft.fillRoundRect(bx, by, bubbleW, bubbleH, bubbleRadius, bg);
    tft.drawRoundRect(bx, by, bubbleW, bubbleH, bubbleRadius, TFT_BLACK);

    tft.setTextColor(fg, bg);
    int tx = bx + bubblePad;
    int ty = by + bubblePad;
    for (int k=0;k<lineCount;k++) {
      tft.drawString(lines[k], tx, ty);
      ty += lineH;
    }

    y = by + bubbleH + vGap;
    lastDrawn = i;
  }

  if (chatCount - (lastDrawn + 1) > 0) {
    tft.setTextColor(TFT_LIGHTGREY, BG_COLOR);
    tft.drawString("...", CHAT_X + CHAT_W - 16, CHAT_Y + 2);
  }
}

void addChatMessage(const String &text, bool sent) {
  chatBuf[chatCount % CHAT_MAX] = {text, sent};
  chatCount++;
  drawChatArea();
}

void clearInput() {
  inputBuffer = "";
  drawInputArea();
}

// =================== SERIAL ===================
void sendInput() {
  if (inputBuffer.length() == 0) return;

  // Wyślij w kawałkach po MAX_PAYLOAD bajtów
  const uint8_t* bytes = (const uint8_t*)inputBuffer.c_str();
  size_t total = inputBuffer.length();
  size_t off = 0;

  while (off < total) {
    uint8_t chunk = (uint8_t)min((size_t)MAX_PAYLOAD, total - off);
    uartSendFrame(MY_ID, bytes + off, chunk);

    // (UI) pokaż to samo w czacie po prawej
    String part;
    part.reserve(chunk);
    for (uint8_t i=0; i<chunk; ++i) part += (char)bytes[off+i];
    addChatMessage(part, true);

    off += chunk;
  }

  inputBuffer = "";
  drawInputArea();
}

void pollSerialReceive() {
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();

    switch (rxState) {
      case RX_WAIT_ID:
        rxSender = b;
        rxState  = RX_WAIT_LEN;
        break;

      case RX_WAIT_LEN:
        rxLen = b;      // 0..255
        rxIdx = 0;
        if (rxLen == 0) {
          // Pusta wiadomość – od razu dodaj do UI
          addChatMessage(String(""), (rxSender == MY_ID));
          rxState = RX_WAIT_ID;
        } else {
          rxState = RX_WAIT_PAYLOAD;
        }
        break;

      case RX_WAIT_PAYLOAD:
        rxBuf[rxIdx++] = b;
        if (rxIdx >= rxLen) {
          // Mamy całą ramkę
          String msg; 
          msg.reserve(rxLen);
          for (uint8_t i=0; i<rxLen; ++i) msg += (char)rxBuf[i];

          // Po lewej gdy to ktoś inny, po prawej gdy MY_ID (pętla lokalna/echo)
          bool isSentByMe = (rxSender == MY_ID);
          addChatMessage(msg, isSentByMe);

          // Reset na następną ramkę
          rxState = RX_WAIT_ID;
        }
        break;
    }
  }
}

// =================== KLAWIATURA: RYSOWANIE I LOGIKA ===================
void pushKey(KeyType type, const String &label, int16_t x, int16_t y, int16_t w, int16_t h, char letter = '\0') {
  keys[keyCount++] = {type, label, x, y, w, h, letter};
}

void drawKey(const Key &k, bool active=false) {
  tft.fillRect(k.x, k.y, k.w, k.h, active ? KEY_ACTIVE : KEY_COLOR);
  tft.drawRect(k.x, k.y, k.w, k.h, TFT_BLACK);
  tft.setTextColor(KEY_TXT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);

  String toDraw = k.label;
  if (k.type == KEY_LETTER) {
    char c = k.letter;
    if (shiftOn) c = toupper((int)c);
    toDraw = String(c);
  } else if (k.type == KEY_SHIFT) {
    toDraw = shiftOn ? "AA" : "Aa";
  }
  tft.drawString(toDraw, k.x + k.w/2, k.y + k.h/2);
}

int hitTestKey(int16_t tx, int16_t ty) {
  for (int i=0; i<keyCount; ++i) {
    const Key &k = keys[i];
    if (tx >= k.x && tx <= k.x + k.w && ty >= k.y && ty <= k.y + k.h) return i;
  }
  return -1;
}

void handleKeyPress(const Key &k) {
  switch (k.type) {
    case KEY_LETTER: {
      char c = k.letter;
      if (shiftOn) c = toupper((int)c);
      inputBuffer += String(c);
      drawInputArea();
      break;
    }
    case KEY_SPACE:
      inputBuffer += " ";
      drawInputArea();
      break;
    case KEY_BACKSPACE:
      if (inputBuffer.length() > 0) inputBuffer.remove(inputBuffer.length()-1);
      drawInputArea();
      break;
    case KEY_CLEAR:
      clearInput();
      break;
    case KEY_SEND:
      sendInput();
      break;
    case KEY_SHIFT:
      shiftOn = !shiftOn;
      for (int i=0;i<keyCount;i++) drawKey(keys[i], false);
      break;
  }
}

void buildKeyboard() {
  keyCount = 0;

  int16_t y = KBD_Y;

  // Rząd 1: 10 klawiszy
  const char* row1 = "qwertyuiop";
  int n1 = strlen(row1);
  int w1 = SCR_W / n1;
  for (int i=0; i<n1; i++) {
    pushKey(KEY_LETTER, " ", i * w1, y, w1, KEY_H, row1[i]);
  }

  // Rząd 2: 9 klawiszy
  y += KEY_H + ROW_GAP;
  const char* row2 = "asdfghjkl";
  int n2 = strlen(row2);
  int w2 = SCR_W / n2;
  for (int i=0; i<n2; i++) {
    pushKey(KEY_LETTER, " ", i * w2, y, w2, KEY_H, row2[i]);
  }

  // Rząd 3: Shift + 7 liter + Backspace + Clear = 10 slotów
  y += KEY_H + ROW_GAP;
  int w3 = SCR_W / 10;
  pushKey(KEY_SHIFT, "Aa",      0 * w3, y, w3, KEY_H);
  const char* row3 = "zxcvbnm";
  for (int i=0; i<7; i++) {
    pushKey(KEY_LETTER, " ", (i+1) * w3, y, w3, KEY_H, row3[i]);
  }
  pushKey(KEY_BACKSPACE, "<-",  8 * w3, y, w3, KEY_H);
  pushKey(KEY_CLEAR,    "Czysc",9 * w3, y, w3, KEY_H);

  // Rząd 4: Spacja + Wyślij (Wyślij = 2 sloty)
  y += KEY_H + ROW_GAP;
  int wSend = 2 * w3;
  pushKey(KEY_SPACE, "Spacja", 0,              y, SCR_W - wSend, KEY_H);
  pushKey(KEY_SEND,  "Wyslij", SCR_W - wSend,  y, wSend,         KEY_H);

  for (int i=0; i<keyCount; ++i) drawKey(keys[i], false);
}

// =================== DOTYK ===================
bool getTouchPoint(int16_t &x, int16_t &y) {
  uint16_t tx, ty;
  bool pressed = tft.getTouch(&tx, &ty);
  if (!pressed) return false;
  x = (int16_t)tx; y = (int16_t)ty;
  return true;
}

// Wyślij ramkę: [sender_id][len][payload...]
void uartSendFrame(uint8_t senderId, const uint8_t* data, uint8_t len) {
  Serial.write(senderId);
  Serial.write(len);
  if (len > 0) Serial.write(data, len);
  Serial.flush(); // opcjonalnie
}

// =================== SETUP / LOOP ===================
void setup() {
  Serial.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN); // np. RX=16, TX=17
  tft.init();
  tft.setRotation(1);      // rotacja przed kalibracją
  touch_calibrate();

// Wysokość całego bloku klawiatury (4 rzędy + 3 przerwy)
int keyboardBlockH = 4*KEY_H + 3*ROW_GAP;

// Ile zostaje na czat, zakładając marginesy
int margins = 8 /*top*/ + 6 /*po INPUT*/ + 6 /*po czacie*/ + 8 /*bottom*/;
int usedByInput = INPUT_Y + INPUT_H; // = 8 + INPUT_H
int remaining = SCR_H - (usedByInput + 6 /*gap po input*/ + keyboardBlockH + 8 /*bottom*/);

// Minimalna sensowna wysokość czatu (żeby nie zniknął całkiem)
int minChat = 60;
if (remaining < minChat) remaining = minChat;

CHAT_W = SCR_W - 16;
CHAT_H = remaining;
KBD_Y  = CHAT_Y + CHAT_H + 6;

  tft.fillScreen(BG_COLOR);
  tft.setTextColor(FG_COLOR, BG_COLOR);

  drawInputArea();
  drawChatArea();
  buildKeyboard();

}

void loop() {
  int16_t tx, ty;
  if (getTouchPoint(tx, ty)) {
    int idx = hitTestKey(tx, ty);
    if (idx >= 0) {
      drawKey(keys[idx], true);
      handleKeyPress(keys[idx]);
      delay(120);
      drawKey(keys[idx], false);

      // czekaj na puszczenie palca (antydublowanie)
      uint32_t t0 = millis();
      while (millis() - t0 < 300) {
        int16_t x2, y2;
        if (!getTouchPoint(x2, y2)) break;
        delay(10);
      }
    }
  }

  pollSerialReceive();
}
