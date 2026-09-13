#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// LOGICA RELÈ: Active LOW (LOW = Acceso, HIGH = Spento)
#define RELE_ACCESO  LOW
#define RELE_SPENTO  HIGH

// PIN SENSORI E ATTUATORI
#define DHTPIN 2
#define DHTTYPE DHT11
#define PIN_SOIL_MOISTURE A0
#define PIN_ULTRASONIC_TRIG 8
#define PIN_ULTRASONIC_ECHO 9
#define PIN_VENTOLA 4
#define PIN_POMPA 3

// PIN DISPLAY ST7789 (2.0" GMT020-02 7-PIN)
// Nota: SCL -> Pin 13 (SPI SCK), SDA -> Pin 11 (SPI MOSI)
#define TFT_CS   10   // Nessun pin CS sul display a 7 pin
#define TFT_DC    7   // Pin Data/Command
#define TFT_RST   6   // Pin Reset

DHT dht(DHTPIN, DHTTYPE);
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// DEFINIZIONE COLORI CUSTOM (RGB565)
#define COLOR_BG        0x0821  // Sfondo blu scuro
#define COLOR_CARD      0x18C5  // Grigio scuro per i box
#define COLOR_BORDER    0x319F  // Bordo dei box
#define COLOR_TEXT_MAIN 0xFFFF  // Bianco per valori
#define COLOR_TEXT_SUB  0x96B5  // Grigio chiaro per titoli
#define COLOR_TEMP      0xFA40  // Arancione/Rosso
#define COLOR_HUM       0x05BF  // Celeste
#define COLOR_SOIL      0x9E60  // Marrone/Terra
#define COLOR_WATER     0x041F  // Blu brillante
#define COLOR_ON        0x07E0  // Verde
#define COLOR_OFF       0xF800  // Rosso

// SOGLIE
const float TEMP_SOGLIA = 35.0;       // Ventola ON se > 35.0 °C
const float HUM_ARIA_SOGLIA = 60.0;   // Ventola ON se > 60.0 %
const int SOIL_SOGLIA_PERC = 10;      // Pompa ON se terreno < 10 %

const int DISTANZA_MIN_ACQUA = 3;     // Serbatoio pieno (15000ml = 100%)
const int DISTANZA_MAX_ACQUA = 35;    // Serbatoio vuoto (0ml = 0%)
const int CAPACITA_MAX_ML = 15000;     // 15 Litri = 15000 ml

bool ventolaState = false;
bool pompaState = false;
bool bloccoAcqua = false;

unsigned long lastUpdate = 0;
float temp = 0.0;
float humAria = 0.0;
int humTerreno = 0;

int volAcquaMl = 0;      // Volume calcolato in ml (0 - 15000 ml)
int volAcquaPerc = 0;    // Percentuale serbatoio (0 - 100%)

void applicaStatoRelay();
void inizializzaLayoutDisplay();
void aggiornaDisplay();
void inviaDatiSeriale();
void disegnaBarra(int x, int y, int w, int h, int percentuale, uint16_t colore);

void setup() {
  Serial.begin(115200);

  // Inizializzazione relè allo stato SPENTO
  digitalWrite(PIN_VENTOLA, RELE_SPENTO);
  digitalWrite(PIN_POMPA, RELE_SPENTO);
  pinMode(PIN_VENTOLA, OUTPUT);
  pinMode(PIN_POMPA, OUTPUT);

  dht.begin();
  pinMode(PIN_ULTRASONIC_TRIG, OUTPUT);
  pinMode(PIN_ULTRASONIC_ECHO, INPUT);

  // Sequenza di Reset Hardware del Display
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(10);
  digitalWrite(TFT_RST, LOW);
  delay(10);
  digitalWrite(TFT_RST, HIGH);
  delay(50);

  // Inizializzazione Display ST7789 (240x320)
  tft.init(240, 320);
  tft.setRotation(3); // Orizzontale (320x240)
  tft.fillScreen(COLOR_BG);
  
  inizializzaLayoutDisplay();
}

void loop() {
  // LETTURA SENSORI AGGIORNATA OGNI SECONDO
  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temp = t;
    if (!isnan(h)) humAria = h;

    int rawSoil = analogRead(PIN_SOIL_MOISTURE);
    humTerreno = map(rawSoil, 1023, 300, 0, 100);
    humTerreno = constrain(humTerreno, 0, 100);

    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
    
    // Formula fisica standard: (durata * 0.0343) / 2
    long duration = pulseIn(PIN_ULTRASONIC_ECHO, HIGH, 10000);
    int distanceCm = (duration > 0) ? (duration * 0.034 / 2) : DISTANZA_MAX_ACQUA;

    // Calcolo volume in ml scalato su 15 Litri (0 - 15000 ml)
    volAcquaMl = map(distanceCm, DISTANZA_MAX_ACQUA, DISTANZA_MIN_ACQUA, 0, CAPACITA_MAX_ML);
    volAcquaMl = constrain(volAcquaMl, 0, CAPACITA_MAX_ML);

    // Calcolo Percentuale reale (15000ml = 100%)
    volAcquaPerc = map(volAcquaMl, 0, CAPACITA_MAX_ML, 0, 100);

    // Blocco acqua se il serbatoio scende sotto il 10% (ovvero 1500 ml)
    bloccoAcqua = (volAcquaPerc < 10);

    applicaStatoRelay();
    aggiornaDisplay();
    inviaDatiSeriale();
  }
}

void applicaStatoRelay() {
  // Ventola ON se Temp > 35°C OPPURE Umidità Aria > 90%
  ventolaState = (temp > TEMP_SOGLIA) || (humAria > HUM_ARIA_SOGLIA);

  // Pompa ON solo se Umidità Terreno < 10% e c'è abbastanza acqua nel serbatoio
  pompaState = (!bloccoAcqua && (humTerreno < SOIL_SOGLIA_PERC));

  digitalWrite(PIN_VENTOLA, ventolaState ? RELE_ACCESO : RELE_SPENTO);
  digitalWrite(PIN_POMPA, pompaState ? RELE_ACCESO : RELE_SPENTO);
}

// Invia le letture al Monitor Seriale ad ogni ciclo
void inviaDatiSeriale() {
  Serial.print("Temp: "); Serial.print(temp, 1); Serial.print(" C | ");
  Serial.print("Umid. Aria: "); Serial.print(humAria, 1); Serial.print(" % | ");
  Serial.print("Umid. Terreno: "); Serial.print(humTerreno); Serial.print(" % | ");
  Serial.print("Acqua: "); Serial.print(volAcquaPerc); Serial.print("% ("); Serial.print(volAcquaMl); Serial.print(" ml) | ");
  Serial.print("Ventola: "); Serial.print(ventolaState ? "ON" : "OFF"); Serial.print(" | ");
  Serial.print("Pompa: "); Serial.println(bloccoAcqua ? "BLOCCO (NO H2O)" : (pompaState ? "ON" : "OFF"));
}

// Disegna il layout iniziale
void inizializzaLayoutDisplay() {
  tft.setTextWrap(false);
  
  // Header Principale
  tft.fillRect(0, 0, 320, 26, 0x018C);
  tft.setTextColor(COLOR_TEXT_MAIN);
  tft.setTextSize(2);
  tft.setCursor(112, 5);
  tft.print("GREENHUB");

  // Box 1: Temperatura
  tft.fillRoundRect(10, 32, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(10, 32, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_TEMP);
  tft.setTextSize(2);
  tft.setCursor(18, 38); tft.print("TEMPERATURA");

  // Box 2: Umidità Aria
  tft.fillRoundRect(165, 32, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(165, 32, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_HUM);
  tft.setTextSize(2);
  tft.setCursor(173, 38); tft.print("UMID.ARIA");

  // Box 3: Umidità Terreno
  tft.fillRoundRect(10, 100, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(10, 100, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_SOIL);
  tft.setTextSize(2);
  tft.setCursor(18, 106); tft.print("TERRENO");

  // Box 4: Serbatoio Acqua
  tft.fillRoundRect(165, 100, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(165, 100, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_WATER);
  tft.setTextSize(2);
  tft.setCursor(173, 106); tft.print("H2O");

  // Box 5: Ventola
  tft.fillRoundRect(10, 168, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(10, 168, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_TEXT_SUB);
  tft.setTextSize(2);
  tft.setCursor(18, 174); tft.print("VENTOLA");

  // Box 6: Pompa H2O
  tft.fillRoundRect(165, 168, 145, 62, 5, COLOR_CARD);
  tft.drawRoundRect(165, 168, 145, 62, 5, COLOR_BORDER);
  tft.setTextColor(COLOR_TEXT_SUB);
  tft.setTextSize(2);
  tft.setCursor(173, 174); tft.print("POMPA");
}

void aggiornaDisplay() {
  // 1. Temperatura
  tft.fillRect(18, 62, 120, 24, COLOR_CARD);
  tft.setTextColor(COLOR_TEXT_MAIN);
  tft.setTextSize(3);
  tft.setCursor(18, 62);
  tft.print(temp, 1); tft.print(" C");

  // 2. Umidità Aria
  tft.fillRect(173, 62, 120, 24, COLOR_CARD);
  tft.setTextSize(3);
  tft.setCursor(173, 62);
  tft.print(humAria, 1); tft.print(" %");

  // 3. Umidità Terreno
  tft.fillRect(18, 130, 120, 24, COLOR_CARD);
  tft.setTextSize(3);
  tft.setCursor(18, 130);
  tft.print(humTerreno); tft.print(" %");

  // 4. Serbatoio Acqua
  tft.fillRect(225, 106, 75, 16, COLOR_CARD);
  tft.setTextSize(1);
  tft.setTextColor(bloccoAcqua ? COLOR_OFF : COLOR_TEXT_MAIN);
  tft.setCursor(225, 110);
  tft.print("LIV: ");
  tft.print(volAcquaPerc); tft.print("%");

  disegnaBarra(173, 134, 128, 16, volAcquaPerc, bloccoAcqua ? COLOR_OFF : COLOR_WATER);

  // 5. Ventola Stato
  tft.fillRect(18, 198, 120, 24, COLOR_CARD);
  tft.setTextSize(3);
  tft.setTextColor(ventolaState ? COLOR_ON : COLOR_OFF);
  tft.setCursor(18, 198);
  tft.print(ventolaState ? "ON" : "OFF");

  // 6. Pompa Stato / Allarme H2O
  tft.fillRect(173, 198, 130, 24, COLOR_CARD);
  tft.setTextSize(3);
  if (bloccoAcqua) {
    tft.setTextColor(COLOR_OFF);
    tft.setCursor(173, 198);
    tft.print("NO H2O");
  } else {
    tft.setTextColor(pompaState ? COLOR_ON : COLOR_OFF);
    tft.setCursor(173, 198);
    tft.print(pompaState ? "ON" : "OFF");
  }
}

// Funzione helper per la barra di carica del serbatoio
void disegnaBarra(int x, int y, int w, int h, int percentuale, uint16_t colore) {
  tft.drawRect(x, y, w, h, COLOR_BORDER);
  int fillWidth = map(percentuale, 0, 100, 0, w - 4);
  fillWidth = constrain(fillWidth, 0, w - 4);
  
  tft.fillRect(x + 2, y + 2, w - 4, h - 4, COLOR_BG);
  if (fillWidth > 0) {
    tft.fillRect(x + 2, y + 2, fillWidth, h - 4, colore);
  }
}