/*
  Pro Mini – Luftqualität + DFPlayer + 2 Servos + Touch
  - DFPlayer: SoftwareSerial RX=6 (<= DF TX), TX=7 (=> DF RX)
  - Servos:   D9 (Temp), D10 (CO2)
  - Touch:    D11 (TTP223, A=0/B=0 → Momenttaster, active HIGH)
  - BME280 an I2C, SGP30 an I2C
  - Erwartet /MP3/0001.mp3 (OK) und /MP3/0002.mp3 (Alarm)
*/

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SGP30.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ---------------- Schwellwerte bestimmen----------------
#define TEMP_THRESHOLD 28.0
#define CO2_THRESHOLD  800

// Pins
#define DF_RX 6    // Arduino RX  <= DF TX
#define DF_TX 7    // Arduino TX  => DF RX
#define SERVO1_PIN 9     // Temperatur-Servo
#define SERVO2_PIN 10    // CO2-Servo
#define TOUCH_PIN 11     // TTP223 aktif hihg 
#define LED_PIN 13

// Servo-Impulse 
#define SERVO_STOP     91
#define SERVO_FORWARD  105
#define SERVO_BACKWARD 75

// Zeiten
const unsigned long PHASE_DURATION = 5000;   // 5 s pro Richtung
const unsigned long AUDIO_DURATION = 5000;   // mp3 wie lange
const unsigned long TOUCH_DEBOUNCE_MS = 40;  // Entprellzeit
const unsigned long TOUCH_MIN_PRESS  = 100;  // Mindestdruckdauer

// ---------------- Objekte ----------------
Adafruit_BME280 bme;
Adafruit_SGP30  sgp;
Servo servo1, servo2;
SoftwareSerial dfSerial(DF_RX, DF_TX);   // Reihenfolge: RX, TX
DFRobotDFPlayerMini mp3;

// ---------------- Zustände ----------------
enum Phase { IDLE, BACKWARD, FORWARD, DONE };
Phase tempPhase = IDLE, co2Phase = IDLE;

unsigned long tempPhaseStart = 0, co2PhaseStart = 0, audioStartTime = 0;
bool actionRunning = false, audioPlaying = false, motorAktiv = false, firstTouchIgnored = false;

// ---------------- Hilfsfunktionen ----------------
bool dfInit() {
  delay(2000);                // DFPlayer Bootzeit
  dfSerial.begin(9600);
  
  if (!mp3.begin(dfSerial, true, true)) {
    Serial.println(F("DFPlayer nicht gefunden!"));
    Serial.println(F("- RX/TX prüfen (D7->DF RX, DF TX->D6), GND, 5V + Elko, SD FAT32"));
    return false;
  }
  mp3.volume(25);
  Serial.println(F("DFPlayer bereit."));
  return true;
}

void starteMessung() {
  // Reset
  servo1.write(SERVO_STOP);
  servo2.write(SERVO_STOP);
  tempPhase = co2Phase = IDLE;
  actionRunning = false;
  audioPlaying = false;
  mp3.stop();
  Serial.println(F("Touch erkannt – Messung startet"));

  // SGP30: erste Messungen stabilisieren
  sgp.IAQmeasure();
  delay(100);
  sgp.IAQmeasure();

  float temperature = bme.readTemperature();
  uint16_t eCO2 = sgp.eCO2;

  Serial.print(F("Temperatur: ")); Serial.println(temperature);
  Serial.print(F("eCO2: "));       Serial.println(eCO2);

  motorAktiv = false;

  if (temperature >= TEMP_THRESHOLD) {
    tempPhase = BACKWARD;
    tempPhaseStart = millis();
    servo1.write(SERVO_BACKWARD);
    motorAktiv = true;
    Serial.println(F("Temperatur zu hoch – Servo1 rückwärts"));
  }

  if (eCO2 >= CO2_THRESHOLD) {
    co2Phase = BACKWARD;
    co2PhaseStart = millis();
    servo2.write(SERVO_BACKWARD);
    motorAktiv = true;
    Serial.println(F("CO2 zu hoch – Servo2 rückwärts"));
  }

  if (!motorAktiv) {
    Serial.println(F("Spielt 0001.mp3 (alles ok)"));
    mp3.playMp3Folder(1);     // /MP3/0001.mp3
  } else {
    Serial.println(F("Spielt 0002.mp3 (Alarrrm)"));
    mp3.playMp3Folder(2);     // /MP3/0002.mp3
  }

  audioStartTime = millis();
  audioPlaying = true;
  actionRunning = motorAktiv;
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(9600);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); delay(200); digitalWrite(LED_PIN, LOW);

  // Touch-Eingang 
  pinMode(TOUCH_PIN, INPUT);

  // DFPlayer
  if (!dfInit()) { while (true) {} }

  // Sensoren
  if (!bme.begin(0x76) && !bme.begin(0x77)) {
    Serial.println(F("BME280 nicht gefunden!")); while (true) {}
  }
  if (!sgp.begin()) {
    Serial.println(F("SGP30 nicht gefunden!")); while (true) {}
  }
  sgp.IAQinit();

  // Servos
  servo1.attach(SERVO1_PIN);
  servo2.attach(SERVO2_PIN);
  servo1.write(SERVO_STOP);
  servo2.write(SERVO_STOP);

  Serial.println(F("Setup abgeschlossen"));
}

void loop() {
  // ---- Touch mit Flanke + Debounce + Mindestdruck ----
  static bool lastRaw = false;
  static unsigned long lastChange = 0;
  static unsigned long pressStart = 0;

  bool raw = digitalRead(TOUCH_PIN);  // HIGH = berührt (A=0, B=0)
  unsigned long now = millis();

  if (raw != lastRaw) {
    lastChange = now; // Start Debounce-Fenster
  }
  if (now - lastChange > TOUCH_DEBOUNCE_MS) {
    // Rising Edge: LOW -> HIGH → Startzeit merken
    if (!lastRaw && raw && !actionRunning) {
      pressStart = now;
    }
    // Falling Edge: HIGH -> LOW → Mindestdruck prüfen
    if (lastRaw && !raw && pressStart) {
      if (now - pressStart >= TOUCH_MIN_PRESS) {
        if (!firstTouchIgnored) {
          Serial.println(F("Erste Touch-Flanke ignoriert (Startschutz)"));
          firstTouchIgnored = true;
        } else {
          starteMessung();
        }
      }
      pressStart = 0;
    }
    lastRaw = raw;
  }

  // ---- Temperaturmotor-Logik ----
  if (tempPhase == BACKWARD && now - tempPhaseStart >= PHASE_DURATION) {
    servo1.write(SERVO_FORWARD);
    tempPhase = FORWARD;
    tempPhaseStart = now;
    Serial.println(F("Temperatur: vorwärts"));
  } else if (tempPhase == FORWARD && now - tempPhaseStart >= PHASE_DURATION) {
    servo1.write(SERVO_STOP);
    tempPhase = DONE;
    Serial.println(F("Temperaturmotor gestoppt"));
  }

  // ---- CO2-Motor-Logik ----
  if (co2Phase == BACKWARD && now - co2PhaseStart >= PHASE_DURATION) {
    servo2.write(SERVO_FORWARD);
    co2Phase = FORWARD;
    co2PhaseStart = now;
    Serial.println(F("CO2: vorwärts"));
  } else if (co2Phase == FORWARD && now - co2PhaseStart >= PHASE_DURATION) {
    servo2.write(SERVO_STOP);
    co2Phase = DONE;
    Serial.println(F("CO2-Motor gestoppt"));
  }

  // ---- Audio stoppen nach AUDIO_DURATION ----
  if (audioPlaying && now - audioStartTime >= AUDIO_DURATION) {
    mp3.stop();
    audioPlaying = false;
    Serial.println(F("Audio gestoppt"));
  }

  // ---- Ablauf abgeschlossen ----
  if (actionRunning &&
      (tempPhase == DONE || tempPhase == IDLE) &&
      (co2Phase == DONE || co2Phase == IDLE)) {
    actionRunning = false;
    tempPhase = IDLE;
    co2Phase = IDLE;
    Serial.println(F("Bereit für neue Messung"));
  }
}
