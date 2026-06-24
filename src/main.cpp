#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_LiquidCrystal.h>
#include <Wire.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASSWORD ""

#define API_KEY      "AIzaSyAijPXyQWz4LnmCTujlnENTWMxEGdwmANg"
#define DATABASE_URL "https://smart-inventory-iot-407f4-default-rtdb.asia-southeast1.firebasedatabase.app"

#define UID_CARD_IN  "AA:BB:CC:DD"  
#define UID_CARD_OUT "11:22:33:44"  

#define RFID_SS_PIN   5
#define RFID_RST_PIN  27
#define LED_RED       2
#define BUZZER_PIN    4
#define BTN_NEXT      12    
#define BTN_PREV      14  
#define LCD_SDA       21
#define LCD_SCL       22

#define SCAN_COOLDOWN_MS  1500
#define SYNC_RETRY_MS     5000
#define OFFLINE_BUF_SIZE  50
#define DEBOUNCE_MS       200
#define MAX_ITEMS         10

MFRC522           rfid(RFID_SS_PIN, RFID_RST_PIN);
FirebaseData      fbdo;
FirebaseAuth      auth;
FirebaseConfig    config;
Adafruit_LiquidCrystal lcd(0x27);

struct BarangItem {
  String id;     
  String nama;     
  int    stok;
  int    ambang_batas;
};

struct LogEntry {
  String id_barang;
  String tipe;       
  int    stok_after;
  unsigned long ts;
};

BarangItem barangList[MAX_ITEMS] = {
  { "barang_001", "Sensor Suhu", 15, 3 },
  { "barang_002", "Komponen B",  8, 5 },
  { "barang_003", "Komponen C", 20, 5 },
};
int jumlahBarang  = 3; 
int selectedIndex = 0; 

bool          firebaseReady   = false;
bool          wifiConnected   = false;
LogEntry      offlineBuffer[OFFLINE_BUF_SIZE];
int           offlineCount    = 0;
unsigned long lastSyncAttempt = 0;
unsigned long lastScanTime    = 0;
unsigned long lastBtnNextTime = 0;
unsigned long lastBtnPrevTime = 0;

void   connectWiFi();
void   initFirebase();
bool   pushTransaksi(String id_barang, String tipe, int stok_after,
                     int ambang, String metode);
bool   pushAlert(String id_barang, String nama, int stok, int ambang);
void   syncOfflineLogs();
void   addToOfflineLog(String id_barang, String tipe, int stok_after);
void   processCard(String uid);
void   doTransaksi(int idx, String tipe);
void   handleNavNext();
void   handleNavPrev();
void   showSelectedItem();
void   triggerAlert(bool active);
void   beepSuccess();
void   beepAlert();
void   beepError();
void   updateLCD(String line1, String line2);
String getCardUID();

void setup() {
  Serial.begin(115200);

  pinMode(LED_RED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_PREV,   INPUT_PULLUP);
  digitalWrite(LED_RED,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("[RFID] MFRC522 ready.");

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.begin(16, 2);
  lcd.setBacklight(HIGH);
  updateLCD("Smart Inventory", "  Booting...");
  delay(1000);

  connectWiFi();
  if (wifiConnected) initFirebase();

  showSelectedItem();
  Serial.println("[BOOT] Siap. Pilih barang lalu scan kartu IN/OUT.");
}


void loop() {
  if (digitalRead(BTN_NEXT) == LOW &&
      millis() - lastBtnNextTime > DEBOUNCE_MS) {
    lastBtnNextTime = millis();
    handleNavNext();
  }
  if (digitalRead(BTN_PREV) == LOW &&
      millis() - lastBtnPrevTime > DEBOUNCE_MS) {
    lastBtnPrevTime = millis();
    handleNavPrev();
  }

  if (millis() - lastScanTime > SCAN_COOLDOWN_MS) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      lastScanTime = millis();
      String uid = getCardUID();
      Serial.println("[RFID] Scan: " + uid);
      processCard(uid);
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  if (offlineCount > 0 && millis() - lastSyncAttempt > SYNC_RETRY_MS) {
    lastSyncAttempt = millis();
    wifiConnected   = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      if (!firebaseReady) initFirebase();
      syncOfflineLogs();
    }
  }

  if (firebaseReady) Firebase.ready();
}

void processCard(String uid) {
  if (uid == UID_CARD_IN) {
    doTransaksi(selectedIndex, "IN");
  } else if (uid == UID_CARD_OUT) {
    doTransaksi(selectedIndex, "OUT");
  } else {
    Serial.println("[RFID] Kartu tidak dikenal: " + uid);
    Serial.println("       → Daftarkan UID ini ke UID_CARD_IN atau UID_CARD_OUT");
    updateLCD("Kartu Unknown!", uid.substring(0, 16));
    beepError();
    delay(1500);
    showSelectedItem();
  }
}

void doTransaksi(int idx, String tipe) {
  BarangItem &b = barangList[idx];  

  if (tipe == "OUT" && b.stok <= 0) {
    updateLCD("STOK KOSONG!", b.nama.substring(0, 16));
    beepError();
    delay(1500);
    showSelectedItem();
    return;
  }

  if (tipe == "IN")  b.stok++;
  else               b.stok = max(0, b.stok - 1);

  bool isAlert = (b.stok < b.ambang_batas);

  Serial.println("[" + tipe + "] " + b.nama +
                 " | Stok: " + String(b.stok) +
                 (isAlert ? " ⚠ ALERT" : ""));

  String line1 = (tipe == "IN" ? "MASUK  +1" : "KELUAR -1");
  updateLCD(line1, b.nama.substring(0, 16));
  beepSuccess();
  triggerAlert(isAlert);
  delay(800);

  String line2 = "Stok: " + String(b.stok);
  if (isAlert) line2 = "Stok:" + String(b.stok) + " RENDAH!";
  updateLCD(b.nama.substring(0, 16), line2);
  delay(1200);

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  String metode = wifiConnected && firebaseReady ? "realtime" : "offline_sync";

  if (wifiConnected && firebaseReady) {
    if (!pushTransaksi(b.id, tipe, b.stok, b.ambang_batas, "realtime")) {
      addToOfflineLog(b.id, tipe, b.stok);
    } else if (isAlert) {
      pushAlert(b.id, b.nama, b.stok, b.ambang_batas);
    }
  } else {
    addToOfflineLog(b.id, tipe, b.stok);
  }

  showSelectedItem();
}

bool pushTransaksi(String id_barang, String tipe, int stok_after,
                   int ambang, String metode) {
  if (!firebaseReady) return false;
  bool ok = true;

  String basePath = "/inventory/" + id_barang;
  bool   isAlert  = (stok_after < ambang);

  ok &= Firebase.RTDB.setInt( &fbdo, basePath + "/stok_sekarang",  stok_after);
  ok &= Firebase.RTDB.setBool(&fbdo, basePath + "/status_alert",   isAlert);
  ok &= Firebase.RTDB.setInt( &fbdo, basePath + "/terakhir_diubah",(int)millis());

  FirebaseJson logJson;
  logJson.set("id_barang", id_barang);
  logJson.set("tipe",      tipe);
  logJson.set("jumlah",    1);
  logJson.set("timestamp", (int)millis());
  logJson.set("metode",    metode);
  ok &= Firebase.RTDB.pushJSON(&fbdo, "/log_transaksi", &logJson);

  if (!ok) Serial.println("[FB ERR] " + fbdo.errorReason());
  else     Serial.println("[FB OK] " + tipe + " " + id_barang + " stok:" + String(stok_after));
  return ok;
}

bool pushAlert(String id_barang, String nama, int stok, int ambang) {
  FirebaseJson alertJson;
  alertJson.set("id_barang", id_barang);
  alertJson.set("pesan",     "Peringatan! Stok " + nama +
                             " kritis: " + String(stok) +
                             " (ambang batas: " + String(ambang) + ").");
  alertJson.set("timestamp", (int)millis());
  alertJson.set("status",    "unread");

  bool ok = Firebase.RTDB.pushJSON(&fbdo, "/notifikasi_alert", &alertJson);
  if (!ok) Serial.println("[FB ERR] push alert: " + fbdo.errorReason());
  else     Serial.println("[FB ALERT] Alert dikirim untuk " + id_barang);
  return ok;
}

void addToOfflineLog(String id_barang, String tipe, int stok_after) {
  if (offlineCount >= OFFLINE_BUF_SIZE) {
    for (int i = 0; i < OFFLINE_BUF_SIZE - 1; i++)
      offlineBuffer[i] = offlineBuffer[i + 1];
    offlineCount = OFFLINE_BUF_SIZE - 1;
  }
  offlineBuffer[offlineCount++] = { id_barang, tipe, stok_after, millis() };
  Serial.println("[OFFLINE] Tersimpan lokal. Pending: " + String(offlineCount));
  updateLCD("OFFLINE MODE", "Pending: " + String(offlineCount));
  delay(800);
  showSelectedItem();
}

void syncOfflineLogs() {
  Serial.println("[SYNC] Sync " + String(offlineCount) + " entri...");
  updateLCD("Syncing...", String(offlineCount) + " entri");

  int synced = 0;
  for (int i = 0; i < offlineCount; i++) {
    int ambang = 5;
    for (int j = 0; j < jumlahBarang; j++) {
      if (barangList[j].id == offlineBuffer[i].id_barang) {
        ambang = barangList[j].ambang_batas; break;
      }
    }
    if (pushTransaksi(offlineBuffer[i].id_barang, offlineBuffer[i].tipe,
                      offlineBuffer[i].stok_after, ambang, "offline_sync")) {
      synced++;
    } else {
      int remaining = offlineCount - (i + 1);
      for (int j = 0; j < remaining; j++)
        offlineBuffer[j] = offlineBuffer[i + 1 + j];
      offlineCount = remaining;
      updateLCD("Sync Parsial", String(synced) + " OK");
      delay(1000);
      showSelectedItem();
      return;
    }
  }

  offlineCount = 0;
  Serial.println("[SYNC] Selesai!");
  updateLCD("Sync Selesai!", String(synced) + " entri");
  delay(1200);
  showSelectedItem();
}

void handleNavNext() {
  selectedIndex = (selectedIndex + 1) % jumlahBarang;
  showSelectedItem();
}

void handleNavPrev() {
  selectedIndex = (selectedIndex - 1 + jumlahBarang) % jumlahBarang;
  showSelectedItem();
}

void showSelectedItem() {
  BarangItem &b   = barangList[selectedIndex];
  bool        alert = (b.stok < b.ambang_batas);
  String line1 = b.nama.substring(0, 16);
  String line2 = "Stok:" + String(b.stok) +
                 (alert ? " RENDAH!" : "  [" + String(selectedIndex+1) +
                          "/" + String(jumlahBarang) + "]");
  updateLCD(line1, line2);
  triggerAlert(alert);
}

String getCardUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }
  uid.toUpperCase();
  return uid;
}

void triggerAlert(bool active) {
  digitalWrite(LED_RED, active ? HIGH : LOW);
}

void beepSuccess() {
  digitalWrite(BUZZER_PIN, HIGH); delay(80);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepAlert() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(150);
    digitalWrite(BUZZER_PIN, LOW);  delay(100);
  }
}

void beepError() {
  digitalWrite(BUZZER_PIN, HIGH); delay(500);
  digitalWrite(BUZZER_PIN, LOW);
}

void updateLCD(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1); lcd.print(line2.substring(0, 16));
}

void connectWiFi() {
  Serial.print("[WIFI] Connecting...");
  updateLCD("Connecting WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[WIFI] IP: " + WiFi.localIP().toString());
    updateLCD("WiFi Connected!", WiFi.localIP().toString());
  } else {
    wifiConnected = false;
    Serial.println("\n[WIFI] Gagal → offline mode.");
    updateLCD("WiFi GAGAL", "Mode Offline");
  }
  delay(1000);
}

void initFirebase() {
  config.api_key               = API_KEY;
  config.database_url          = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback; 

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (!Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("[FB WARN] signUp gagal: " +
                   String(config.signer.signupError.message.c_str()));
    Serial.println("[FB WARN] Coba lanjut dengan auth yang ada...");
  } else {
    Serial.println("[FB] signUp OK.");
  }

  unsigned long t = millis();
  while (!Firebase.ready() && millis() - t < 3000) delay(100);

  firebaseReady = Firebase.ready();
  if (firebaseReady) {
    Serial.println("[FB] Firebase Ready!");
    updateLCD("Firebase Ready!", ""); delay(800);
  } else {
    Serial.println("[FB ERR] Firebase tidak ready → fallback offline.");
    updateLCD("FB Gagal!", "Mode Offline"); delay(800);
  }
}