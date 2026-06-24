#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASSWORD ""

#define DATABASE_URL "https://smart-inventory-iot-407f4-default-rtdb.asia-southeast1.firebasedatabase.app"

#define UID_CARD_IN  "01:02:03:04"
#define UID_CARD_OUT "05:06:07:08"

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
WiFiClientSecure  sslClient;
LiquidCrystal_I2C lcd(0x27, 16, 2);

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
void   seedInventory();
int    fbPut(String path, String json);
int    fbPost(String path, String json);
bool   pushTransaksi(String id_barang, String nama, String tipe,
                     int stok_after, int ambang, String metode);
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
  lcd.init();
  lcd.backlight();
  updateLCD("Smart Inventory", "  Booting...");
  delay(1000);

  connectWiFi();

  if (wifiConnected) {
    sslClient.setInsecure();
    firebaseReady = true;

    updateLCD("Firebase...", "Seeding data...");
    seedInventory();
  }

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

  if (offlineCount > 0 && firebaseReady &&
      millis() - lastSyncAttempt > SYNC_RETRY_MS) {
    lastSyncAttempt = millis();
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) syncOfflineLogs();
  }
}

// --- Firebase REST API helpers ---

int fbPut(String path, String json) {
  HTTPClient http;
  String url = String(DATABASE_URL) + path + ".json";
  http.begin(sslClient, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(json);
  String resp = http.getString();
  http.end();
  Serial.println("[FB PUT] " + path + " → " + String(code));
  if (code != 200) Serial.println("[FB ERR] " + resp);
  return code;
}

int fbPost(String path, String json) {
  HTTPClient http;
  String url = String(DATABASE_URL) + path + ".json";
  http.begin(sslClient, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  String resp = http.getString();
  http.end();
  Serial.println("[FB POST] " + path + " → " + String(code));
  if (code != 200) Serial.println("[FB ERR] " + resp);
  return code;
}

// --- Seed data awal ---

void seedInventory() {
  Serial.println("[SEED] Mengirim data awal ke Firebase...");
  int sukses = 0;

  for (int i = 0; i < jumlahBarang; i++) {
    BarangItem &b = barangList[i];
    bool isAlert = (b.stok < b.ambang_batas);

    String json = "{\"nama_barang\":\"" + b.nama + "\","
                  "\"stok_sekarang\":" + String(b.stok) + ","
                  "\"ambang_batas\":" + String(b.ambang_batas) + ","
                  "\"status_alert\":" + (isAlert ? "true" : "false") + ","
                  "\"terakhir_diubah\":" + String((int)millis()) + "}";

    String path = "/inventory/" + b.id;
    updateLCD("Seeding...", b.nama.substring(0, 16));

    if (fbPut(path, json) == 200) {
      sukses++;
      Serial.println("[SEED] OK: " + b.id);
    } else {
      Serial.println("[SEED] FAIL: " + b.id);
      updateLCD("SEED GAGAL!", b.id.substring(0, 16));
      delay(1500);
    }
  }

  if (sukses == jumlahBarang) {
    updateLCD("Seed OK!", String(sukses) + " barang");
  } else {
    updateLCD("Seed Parsial", String(sukses) + "/" + String(jumlahBarang));
  }
  delay(1000);
}

// --- Transaksi ---

void processCard(String uid) {
  if (uid == UID_CARD_IN) {
    doTransaksi(selectedIndex, "IN");
  } else if (uid == UID_CARD_OUT) {
    doTransaksi(selectedIndex, "OUT");
  } else {
    Serial.println("[RFID] Kartu tidak dikenal: " + uid);
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
                 (isAlert ? " ALERT" : ""));

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

  if (wifiConnected && firebaseReady) {
    if (!pushTransaksi(b.id, b.nama, tipe, b.stok, b.ambang_batas, "realtime")) {
      addToOfflineLog(b.id, tipe, b.stok);
    } else if (isAlert) {
      pushAlert(b.id, b.nama, b.stok, b.ambang_batas);
    }
  } else {
    addToOfflineLog(b.id, tipe, b.stok);
  }

  showSelectedItem();
}

bool pushTransaksi(String id_barang, String nama, String tipe,
                   int stok_after, int ambang, String metode) {
  if (!firebaseReady) return false;

  bool isAlert = (stok_after < ambang);

  String invJson = "{\"nama_barang\":\"" + nama + "\","
                   "\"stok_sekarang\":" + String(stok_after) + ","
                   "\"ambang_batas\":" + String(ambang) + ","
                   "\"status_alert\":" + (isAlert ? "true" : "false") + ","
                   "\"terakhir_diubah\":" + String((int)millis()) + "}";

  bool ok = (fbPut("/inventory/" + id_barang, invJson) == 200);

  String logJson = "{\"id_barang\":\"" + id_barang + "\","
                   "\"tipe\":\"" + tipe + "\","
                   "\"jumlah\":1,"
                   "\"timestamp\":" + String((int)millis()) + ","
                   "\"metode\":\"" + metode + "\"}";

  ok &= (fbPost("/log_transaksi", logJson) == 200);

  if (ok) Serial.println("[FB OK] " + tipe + " " + id_barang + " stok:" + String(stok_after));
  return ok;
}

bool pushAlert(String id_barang, String nama, int stok, int ambang) {
  String json = "{\"id_barang\":\"" + id_barang + "\","
                "\"pesan\":\"Peringatan! Stok " + nama +
                " kritis: " + String(stok) +
                " (ambang batas: " + String(ambang) + ").\","
                "\"timestamp\":" + String((int)millis()) + ","
                "\"status\":\"unread\"}";

  bool ok = (fbPost("/notifikasi_alert", json) == 200);
  if (ok) Serial.println("[FB ALERT] Alert dikirim untuk " + id_barang);
  return ok;
}

// --- Offline buffer ---

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
    String nama = "Barang";
    for (int j = 0; j < jumlahBarang; j++) {
      if (barangList[j].id == offlineBuffer[i].id_barang) {
        ambang = barangList[j].ambang_batas;
        nama   = barangList[j].nama;
        break;
      }
    }
    if (pushTransaksi(offlineBuffer[i].id_barang, nama, offlineBuffer[i].tipe,
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

// --- Navigasi ---

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

// --- Utilitas ---

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
    Serial.println("\n[WIFI] Gagal -> offline mode.");
    updateLCD("WiFi GAGAL", "Mode Offline");
  }
  delay(1000);
}
