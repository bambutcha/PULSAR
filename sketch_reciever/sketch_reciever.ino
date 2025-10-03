// ESP32 ПРИЁМНИК: WiFi + BLE Scanner → Serial JSON
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <math.h>
#include <SPI.h>
#include <MFRC522.h>

struct Beacon {
  int id;
  const char* wifiName;
  const char* bleName;
  float x, y;
  int wifiRSSI;
  float wifiDistance;
  int bleRSSI;
  float bleDistance;
  bool wifiFound;
  bool bleFound;
};

Beacon beacons[3] = {
  {1, "Beacon_1", "BLE_Beacon_1", 0.0, 0.0, 0, 0, 0, 0, false, false},
  {2, "Beacon_2", "BLE_Beacon_2", 5.0, 0.0, 0, 0, 0, 0, false, false},
  {3, "Beacon_3", "BLE_Beacon_3", 2.5, 5.0, 0, 0, 0, 0, false, false}
};

const float WIFI_RSSI_AT_1M = -40;
const float BLE_RSSI_AT_1M = -59;
const float PATH_LOSS = 2.5;
const byte ZERO_POINT_UID[] = {0xDE, 0xAD, 0xBE, 0xEF}; // ТЕСТОВЫЙ UID
const byte UID_SIZE = 4; // Обычно UID имеет размер 4 байта

#define RST_PIN   22   // RST (Reset) на ESP32 GPIO 22
#define SS_PIN    21   // SDA (Slave Select) на ESP32 GPIO 21
MFRC522 rfid(SS_PIN, RST_PIN); 
bool isCalibrated = false;

struct Position {
  float x, y, accuracy;
  float wifiX, wifiY, wifiAccuracy;
  float bleX, bleY, bleAccuracy;
  float wifiWeight, bleWeight;
} currentPos;

BLEScan* pBLEScan;

#define SMOOTH_SIZE 3
float xBuffer[SMOOTH_SIZE] = {0};
float yBuffer[SMOOTH_SIZE] = {0};
int bufferIdx = 0;

unsigned long lastScan = 0;
const int SCAN_INTERVAL = 2000;

void setup() {
  Serial.begin(115200);
  delay(1000);

  SPI.begin();       // Инициализация SPI шины
  rfid.PCD_Init();   // Инициализация считывателя RC522
  Serial.println(F("RFID инициирован. Ожидание метки Нулевой Точки..."));
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

// --- RFID: Функция калибровки ---
void calibrateRFID() {
  // Проверка, есть ли карта и может ли она быть прочитана
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    
    // Сравнение считанного UID с UID нулевой точки
    if (compareUIDs(rfid.uid.uidByte, ZERO_POINT_UID, rfid.uid.size)) {
      
      Serial.println("\n*** RFID КАЛИБРОВКА УСПЕШНА ***");
      Serial.println("Установка местоположения: X=0.0, Y=0.0");
      
      // 1. Установка текущих координат в (0, 0)
      currentPos.x = 0.0;
      currentPos.y = 0.0;
      currentPos.accuracy = 0.0;
      
      // 2. Сброс буферов сглаживания
      for (int i = 0; i < SMOOTH_SIZE; i++) {
        xBuffer[i] = 0.0;
        yBuffer[i] = 0.0;
      }
      bufferIdx = 0;
      
      // 3. Флаг, что калибровка выполнена
      isCalibrated = true;
      
      // 4. Оповещение об успешной калибровке
      Serial.println("*** СИСТЕМА ПОЗИЦИОНИРОВАНИЯ ГОТОВА ***\n");
      
    } else {
      Serial.print("Обнаружена неизвестная RFID метка. UID: ");
      for (byte i = 0; i < rfid.uid.size; i++) {
        Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
        Serial.print(rfid.uid.uidByte[i], HEX);
      }
      Serial.println();
    }
    
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1(true);
  }
}

void loop() {
  if (!isCalibrated) {
    calibrateRFID();
    delay(500); // Непрерывный опрос
    return; 
  }

  unsigned long now = millis();
  
  if (now - lastScan > SCAN_INTERVAL) {
    lastScan = now;
    
    scanWiFi();
    scanBLE();
    calculatePosition();
    printStatus();
    sendJSON();
  }
}

void scanWiFi() {
  // Сброс предыдущих значений
  for (int i = 0; i < 3; i++) {
    beacons[i].wifiFound = false;
  }
  
  int n = WiFi.scanNetworks();
  int beaconsFound = 0;
  
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    
    // Ищем наши маяки
    for (int j = 0; j < 3; j++) {
      if (ssid.equals(beacons[j].wifiName)) {
        beacons[j].wifiRSSI = rssi;
        beacons[j].wifiFound = true;
        beaconsFound++;
        
        // Расчет расстояния
        float ratio = (WIFI_RSSI_AT_1M - rssi) / (10.0 * PATH_LOSS);
        beacons[j].wifiDistance = pow(10, ratio);
        
        Serial.print("  ✅ ");
        Serial.print(beacons[j].wifiName);
        Serial.print(" | RSSI: ");
        Serial.print(rssi);
        Serial.print(" | Dist: ");
        Serial.print(beacons[j].wifiDistance, 1);
        Serial.println("m");
      }
    }
  }
  
  Serial.print("WiFi: ");
  Serial.print(beaconsFound);
  Serial.println("/3");
}

void scanBLE() {
  // Сброс предыдущих значений
  for (int i = 0; i < 3; i++) {
    beacons[i].bleFound = false;
  }
  
  BLEScanResults* foundDevices = pBLEScan->start(1, false);
  int beaconsFound = 0;
  
  for (int i = 0; i < foundDevices->getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices->getDevice(i);
    String name = device.getName().c_str();
    int rssi = device.getRSSI();
    
    // Ищем наши маяки
    for (int j = 0; j < 3; j++) {
      if (name == beacons[j].bleName) {
        beacons[j].bleRSSI = rssi;
        beacons[j].bleFound = true;
        beaconsFound++;
        
        // Расчет расстояния
        float ratio = (BLE_RSSI_AT_1M - rssi) / (10.0 * PATH_LOSS);
        beacons[j].bleDistance = pow(10, ratio);
        
        Serial.print("  ✅ ");
        Serial.print(beacons[j].bleName);
        Serial.print(" | RSSI: ");
        Serial.print(rssi);
        Serial.print(" | Dist: ");
        Serial.print(beacons[j].bleDistance, 1);
        Serial.println("m");
      }
    }
  }
  
  Serial.print("BLE: ");
  Serial.print(beaconsFound);
  Serial.println("/3");
  
  pBLEScan->clearResults();
}

bool trilaterate(float d1, float d2, float d3, 
                 float x1, float y1, float x2, float y2, float x3, float y3,
                 float &x, float &y, float &accuracy) {
  
  if (d1 <= 0 || d2 <= 0 || d3 <= 0) return false;
  
  float A = 2 * (x2 - x1);
  float B = 2 * (y2 - y1);
  float C = d1*d1 - d2*d2 - x1*x1 + x2*x2 - y1*y1 + y2*y2;
  
  float D = 2 * (x3 - x2);
  float E = 2 * (y3 - y2);
  float F = d2*d2 - d3*d3 - x2*x2 + x3*x3 - y2*y2 + y3*y3;
  
  float denominator = (E*A - B*D);
  if (abs(denominator) < 0.001) return false;
  
  x = (C*E - F*B) / denominator;
  y = (C*D - A*F) / (B*D - A*E);
  
  if (isnan(x) || isnan(y)) return false;
  
  accuracy = 0;
  float dx, dy, calc_dist;
  
  dx = x - x1; dy = y - y1;
  calc_dist = sqrt(dx*dx + dy*dy);
  accuracy += abs(calc_dist - d1);
  
  dx = x - x2; dy = y - y2;
  calc_dist = sqrt(dx*dx + dy*dy);
  accuracy += abs(calc_dist - d2);
  
  dx = x - x3; dy = y - y3;
  calc_dist = sqrt(dx*dx + dy*dy);
  accuracy += abs(calc_dist - d3);
  
  accuracy /= 3.0;
  
  return true;
}

void calculatePosition() {
  // Трилатерация по WiFi
  bool wifiOK = false;
  if (beacons[0].wifiFound && beacons[1].wifiFound && beacons[2].wifiFound) {
    wifiOK = trilaterate(
      beacons[0].wifiDistance, beacons[1].wifiDistance, beacons[2].wifiDistance,
      beacons[0].x, beacons[0].y, beacons[1].x, beacons[1].y, beacons[2].x, beacons[2].y,
      currentPos.wifiX, currentPos.wifiY, currentPos.wifiAccuracy
    );
  }
  
  // Трилатерация по BLE
  bool bleOK = false;
  if (beacons[0].bleFound && beacons[1].bleFound && beacons[2].bleFound) {
    bleOK = trilaterate(
      beacons[0].bleDistance, beacons[1].bleDistance, beacons[2].bleDistance,
      beacons[0].x, beacons[0].y, beacons[1].x, beacons[1].y, beacons[2].x, beacons[2].y,
      currentPos.bleX, currentPos.bleY, currentPos.bleAccuracy
    );
  }
  
  // FUSION: Объединение WiFi и BLE
  if (wifiOK && bleOK) {
    // Веса обратно пропорциональны ошибке
    float w1 = 1.0 / (currentPos.wifiAccuracy + 0.1);
    float w2 = 1.0 / (currentPos.bleAccuracy + 0.1);
    float total = w1 + w2;
    
    currentPos.wifiWeight = w1 / total;
    currentPos.bleWeight = w2 / total;
    
    currentPos.x = currentPos.wifiWeight * currentPos.wifiX + currentPos.bleWeight * currentPos.bleX;
    currentPos.y = currentPos.wifiWeight * currentPos.wifiY + currentPos.bleWeight * currentPos.bleY;
    currentPos.accuracy = currentPos.wifiWeight * currentPos.wifiAccuracy + currentPos.bleWeight * currentPos.bleAccuracy;
    
  } else if (wifiOK) {
    currentPos.x = currentPos.wifiX;
    currentPos.y = currentPos.wifiY;
    currentPos.accuracy = currentPos.wifiAccuracy;
    currentPos.wifiWeight = 1.0;
    currentPos.bleWeight = 0.0;
    
  } else if (bleOK) {
    currentPos.x = currentPos.bleX;
    currentPos.y = currentPos.bleY;
    currentPos.accuracy = currentPos.bleAccuracy;
    currentPos.wifiWeight = 0.0;
    currentPos.bleWeight = 1.0;
    
  } else {
    return; // Недостаточно маяков
  }
  
  // Сглаживание
  xBuffer[bufferIdx] = currentPos.x;
  yBuffer[bufferIdx] = currentPos.y;
  bufferIdx = (bufferIdx + 1) % SMOOTH_SIZE;
  
  float smoothX = 0, smoothY = 0;
  for (int i = 0; i < SMOOTH_SIZE; i++) {
    smoothX += xBuffer[i];
    smoothY += yBuffer[i];
  }
  currentPos.x = smoothX / SMOOTH_SIZE;
  currentPos.y = smoothY / SMOOTH_SIZE;
}

void printStatus() {
  Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("📊 STATUS SUMMARY\n");
  
  Serial.println("Beacon Status:");
  for (int i = 0; i < 3; i++) {
    Serial.print("  Beacon "); Serial.print(i+1); Serial.print(": ");
    Serial.print(beacons[i].wifiFound ? "WiFi✅" : "WiFi❌");
    Serial.print(" | ");
    Serial.println(beacons[i].bleFound ? "BLE✅" : "BLE❌");
  }
  
  Serial.print("\nPosition: ");
  Serial.print(currentPos.x, 1);
  Serial.print(", ");
  Serial.print(currentPos.y, 1);
  Serial.print(" (±");
  Serial.print(currentPos.accuracy, 1);
  Serial.println("m)");
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

void sendJSON() {
  StaticJsonDocument<1024> doc;
  
  doc["timestamp"] = millis();
  
  JsonObject pos = doc.createNestedObject("position");
  pos["x"] = currentPos.x;
  pos["y"] = currentPos.y;
  pos["accuracy"] = currentPos.accuracy;
  
  JsonObject wifi = doc.createNestedObject("wifi");
  for (int i = 0; i < 3; i++) {
    JsonObject b = wifi.createNestedObject("beacon" + String(i+1));
    b["rssi"] = beacons[i].wifiRSSI;
    b["distance"] = beacons[i].wifiDistance;
    b["found"] = beacons[i].wifiFound;
  }
  
  JsonObject ble = doc.createNestedObject("ble");
  for (int i = 0; i < 3; i++) {
    JsonObject b = ble.createNestedObject("beacon" + String(i+1));
    b["rssi"] = beacons[i].bleRSSI;
    b["distance"] = beacons[i].bleDistance;
    b["found"] = beacons[i].bleFound;
  }
  
  JsonObject fusion = doc.createNestedObject("fusion");
  fusion["wifi_weight"] = currentPos.wifiWeight;
  fusion["ble_weight"] = currentPos.bleWeight;
  
  // Выводим JSON в Serial
  serializeJson(doc, Serial);
  Serial.println(); // Перевод строки для парсинга
}