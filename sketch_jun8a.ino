#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include "time.h"

// ------------------ Konfigurasi WiFi & Telegram ------------------
const char* ssid = "Leo";
const char* password = "113333555555";
#define BOTtoken "8241490855:AAGiVflQyI2pzVCyjz6B01D4HTObBxZwCV8"
#define CHAT_ID "2033720484"

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
Preferences prefs;

// ------------------ Servo ------------------
Servo feederServo;
const int servoPin = 14;
bool isFeeding = false;

// ------------------ Serial ke Arduino ------------------
HardwareSerial SerialArduino(2); // RX=16, TX=17

// ------------------ Time ------------------
const long gmtOffset_sec = 8*3600; // WITA
const int daylightOffset_sec = 0;

// ------------------ Telegram ------------------
unsigned long lastBotCheck = 0;
const unsigned long BOT_CHECK_INTERVAL = 2000;

// ------------------ Fuzzy Tsukamoto ------------------
// ranges: 0-30 Low, 30-60 Medium, 60-90 High
float z_low_min=0.0, z_low_max=30.0;
float z_med_min=30.0, z_med_max=60.0;
float z_high_min=60.0, z_high_max=90.0;

void setup() {
  Serial.begin(115200);
  SerialArduino.begin(9600, SERIAL_8N1, 16,17);

  feederServo.attach(servoPin);
  feederServo.write(0);

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  int t=0;
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
    Serial.print(".");
    if(++t>60) break;
  }
  if(WiFi.status()==WL_CONNECTED) Serial.println("\nWiFi connected!");
  else Serial.println("\nWiFi failed!");

  client.setInsecure();
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org","time.google.com");
  prefs.begin("feeder", false);

  if(WiFi.status()==WL_CONNECTED) bot.sendMessage(CHAT_ID,"🐔 Feeder online!","");
}

// ------------------ Fungsi Fuzzy Tsukamoto ------------------
float z_high_from_alpha(float a){ return z_high_min + a*(z_high_max - z_high_min); }
float z_med_from_alpha(float a){ return z_med_min + a*(z_med_max - z_med_min); }

float defuzz_tsukamoto(float mu_p, float mu_s, float mu_m){
  float num=0.0, den=0.0;
  if(mu_p>0){ float z=z_high_from_alpha(mu_p); num+=mu_p*z; den+=mu_p; }
  if(mu_s>0){ float z=z_high_from_alpha(mu_s); num+=mu_s*z; den+=mu_s; }
  if(mu_m>0){ float z=z_med_from_alpha(mu_m); num+=mu_m*z; den+=mu_m; }
  if(den<=0.0001) return -1.0;
  return num/den;
}

// ------------------ Utility ------------------
String getTimeString(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "Waktu error";
  char buf[40];
  strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&timeinfo);
  return String(buf);
}

void pushLog(String entry){
  String all = prefs.getString("log","");
  if(all.length()==0) all = entry;
  else all = entry + "|" + all;
  int count=0, idx=0;
  while(count<20 && (idx = all.indexOf('|', idx+1))!=-1) count++;
  if(idx!=-1) all = all.substring(0,idx);
  prefs.putString("log",all);
}

String getLogsText(){
  String all = prefs.getString("log","");
  if(all=="") return "Belum ada riwayat.";
  String out="";
  int start=0;
  while(true){
    int p = all.indexOf('|',start);
    if(p==-1){ out+="- "+all.substring(start); break;}
    out+="- "+all.substring(start,p)+"\n";
    start = p+1;
  }
  return out;
}

// ------------------ Beri Pakan ------------------
void beriPakanFuzzy(float mu_p, float mu_s, bool manualFlag, String mode){
  if(isFeeding) return;
  isFeeding=true;

  float mu_m = manualFlag?1.0f:0.0f;
  float z = defuzz_tsukamoto(mu_p, mu_s, mu_m);
  if(z<0){ Serial.println("No fuzzy rule active -> skip feeding"); isFeeding=false; return; }

  if(z<0) z=0; if(z>90) z=90;

  Serial.println("Fuzzy angle="+String(z,1));

  // ---------------- Bunyi ISD dulu ----------------
  SerialArduino.println("SOUND");
  Serial.println("Minta Arduino bunyikan suara...");
  unsigned long start = millis();
  bool done=false;
  while(millis()-start<7000){
    if(SerialArduino.available()){
      String r = SerialArduino.readStringUntil('\n'); r.trim();
      if(r=="DONE"){ done=true; break; }
    }
  }
  if(!done) Serial.println("⚠️ Tidak ada DONE dari Arduino, lanjutkan");

  delay(500);

  // ---------------- Servo ----------------
  int before=feederServo.read();
  feederServo.write((int)z);
  int waitMs = map((int)z,0,90,1200,4000);
  delay(waitMs);
  int after=feederServo.read();
  bool servoError = abs(after-before)<15;
  if(servoError){
    Serial.println("❌ SERVO GAGAL!");
    SerialArduino.println("ERROR");
    if(WiFi.status()==WL_CONNECTED) bot.sendMessage(CHAT_ID,"⚠️ Servo gagal membuka!","");
    isFeeding=false;
    return;
  }

  // close servo
  delay(1200);
  feederServo.write(0);
  delay(300);

  // notify Arduino dan Telegram
  SerialArduino.println("FED");
  String waktu = getTimeString();
  String pesan="🐔 Ayam diberi pakan ("+mode+") pada "+waktu+"\nSudut servo: "+String(z,1)+"°";
  if(WiFi.status()==WL_CONNECTED) bot.sendMessage(CHAT_ID,pesan,"");
  pushLog(waktu+" ("+mode+"): angle="+String(z,1));

  isFeeding=false;
}

// ------------------ Parsing Arduino OPEN ------------------
bool parseOpenWithTM(const String &msg, float &out_mu_p, float &out_mu_s){
  if(!msg.startsWith("OPEN")) return false;
  int p1=msg.indexOf(':'); int p2=msg.indexOf(':',p1+1);
  if(p1>=0 && p2>p1){
    out_mu_p = msg.substring(p1+1,p2).toFloat();
    out_mu_s = msg.substring(p2+1).toFloat();
    return true;
  }
  return false;
}

// ------------------ Request TM ------------------
bool requestTMandWait(float &out_mu_p,float &out_mu_s,unsigned long timeout=2500){
  SerialArduino.println("REQ_TM");
  unsigned long t0=millis();
  while(millis()-t0<timeout){
    if(SerialArduino.available()){
      String r = SerialArduino.readStringUntil('\n'); r.trim();
      if(r.startsWith("TM:")){
        int p1=r.indexOf(':'); int p2=r.indexOf(':',p1+1);
        out_mu_p = r.substring(p1+1,p2).toFloat();
        out_mu_s = r.substring(p2+1).toFloat();
        Serial.println("Got TM -> mu_p="+String(out_mu_p)+" mu_s="+String(out_mu_s));
        return true;
      }
    }
  }
  return false;
}

// ------------------ LOOP ------------------
void loop(){
  // ---------------- Arduino Serial ----------------
  if(SerialArduino.available()){
    String cmd = SerialArduino.readStringUntil('\n'); cmd.trim();
    if(cmd.length()==0) return;
    float mu_p=0, mu_s=0;
    if(parseOpenWithTM(cmd,mu_p,mu_s)){
      beriPakanFuzzy(mu_p,mu_s,false,"otomatis");
    } else if(cmd=="OPEN"){
      float tp=0, ts=0;
      if(requestTMandWait(tp,ts)) beriPakanFuzzy(tp,ts,false,"otomatis");
      else Serial.println("No TM -> skip automatic feeding");
    }
  }

  // ---------------- Telegram ----------------
  if(WiFi.status()==WL_CONNECTED && millis()-lastBotCheck>BOT_CHECK_INTERVAL){
    int numNew = bot.getUpdates(bot.last_message_received+1);
    while(numNew){
      for(int i=0;i<numNew;i++){
        String chat_id=String(bot.messages[i].chat_id);
        String text=bot.messages[i].text;
        Serial.println("Msg: "+text);

        if(text=="/start"){
          bot.sendMessage(chat_id,"Feeder Ayam IoT 🐔\n/open - beri pakan manual\n/status - last logs\n/history - riwayat","");
        } else if(text=="/open"){
          float tp=0, ts=0;
          bool got=requestTMandWait(tp,ts);
          if(!got) Serial.println("No TM from Arduino (manual).");
          beriPakanFuzzy(tp,ts,true,"manual");
          bot.sendMessage(chat_id,"Perintah /open diterima. Membuka feeder...","");
        } else if(text=="/status"){
          bot.sendMessage(chat_id,"Status feeder:\n"+getLogsText(),"");
        } else if(text=="/history"){
          bot.sendMessage(chat_id,"Riwayat:\n"+getLogsText(),"");
        } else{
          bot.sendMessage(chat_id,"Perintah tidak dikenal.","");
        }
      }
      numNew = bot.getUpdates(bot.last_message_received+1);
    }
    lastBotCheck=millis();
  }
}