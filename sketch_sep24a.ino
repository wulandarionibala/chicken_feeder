#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
SoftwareSerial espSerial(3, 2);  // RX=3, TX=2

const int isdPin = 7; // P–L ISD1820
long lastSentMarker = -1;

// ============================
//   FUZZY MEMBERSHIP FUNCTIONS
// ============================

// Fungsi keanggotaan pagi: 3–7 (triangle)
float mu_pagi(float x) {
  if (x <= 3.0) return 0.0;
  else if (x > 3.0 && x <= 5.0) return (x - 3.0) / 2.0;
  else if (x > 5.0 && x < 7.0) return (7.0 - x) / 2.0;
  else return 0.0;
}

// Fungsi keanggotaan sore: 15–19 (triangle)
float mu_sore(float x) {
  if (x <= 15.0) return 0.0;
  else if (x > 15.0 && x <= 17.0) return (x - 15.0) / 2.0;
  else if (x > 17.0 && x < 19.0) return (19.0 - x) / 2.0;
  else return 0.0;
}

// helper format float 2 dec
String fmt2(float v) {
  char b[12];
  dtostrf(v, 0, 2, b);
  return String(b);
}

void setup() {
  Serial.begin(115200);
  espSerial.begin(9600);
  lcd.init();
  lcd.backlight();
  pinMode(isdPin, OUTPUT);
  digitalWrite(isdPin, LOW);

  if (!rtc.begin()) {
    lcd.clear();
    lcd.print("RTC Error!");
    while (1);
  }

  lcd.clear();
  lcd.print("Feeder Siap...");
  delay(1200);
}

void loop() {
  DateTime now = rtc.now();
  int hour = now.hour();
  int minute = now.minute();
  int day = now.day();

  float hourf = hour + (minute / 60.0);

  // compute memberships
  float mup = mu_pagi(hourf);
  float mus = mu_sore(hourf);

  // Automatic schedule: Pagi 05:30, Sore 17:00
bool waktuPagi = (hour == 5 && minute == 30);
bool waktuSore = (hour == 17 && minute == 0);

// unique marker: cukup day + minute (anti double)
long marker = day * 100 + minute;

if ((waktuPagi || waktuSore) && marker != lastSentMarker) {
    float max_mu = max(mup, mus); 
    if (max_mu > 0.20) {
        String msg = "OPEN:" + fmt2(mup) + ":" + fmt2(mus);
        espSerial.println(msg);
    }
    lastSentMarker = marker;
}

  // handle incoming from ESP32
  if (espSerial.available()) {
    String pesan = espSerial.readStringUntil('\n');
    pesan.trim();
    Serial.println("Dari ESP32: " + pesan);

    if (pesan == "SOUND") {
      // ---------------- Bunyi ISD1820 dulu ----------------
      digitalWrite(isdPin, HIGH);      // mulai bunyi
      delay(3500);                      // durasi suara (sesuaikan rekaman)
      digitalWrite(isdPin, LOW);       // berhenti
      espSerial.println("DONE");        // beri tahu ESP32
      Serial.println("ISD selesai, DONE dikirim");
    }
    else if (pesan == "FED") {
      lcd.clear();
      lcd.print("Pakan Diberikan");
      lcd.setCursor(0, 1);
      char buf[6];
      snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
      lcd.print(buf);
      delay(3500);
      lcd.clear();
      lcd.print("Feeder Siap...");
    }
    else if (pesan == "ERROR") {
      lcd.clear();
      lcd.print("Servo GAGAL!");
      Serial.println("⚠️ Terima ERROR dari ESP32: Servo gagal");
      delay(3500);
      lcd.clear();
      lcd.print("Feeder Siap...");
    }
  }

  delay(200);
}