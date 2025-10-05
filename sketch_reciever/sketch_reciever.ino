// ESP32 ПРИЁМНИК: WiFi + BLE Scanner → Serial JSON
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <math.h>
#include "DHT.h"
#include <EEPROM.h>

#define DHTPIN 4     
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

float currentTemp = 0.0;
float currentHumidity = 0.0;


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

const float WIFI_RSSI_AT_1M = -52;
// const float BLE_RSSI_AT_1M = -59; // теперь не используется

// === КАЛИБРОВКА BLE: RSSI НА 0.5 М И 1 М ===
float rssiAt05m[3] = {-40, -40, -40}; // RSSI на 0.5 м
float rssiAt1m[3] = {-59, -59, -59};   // RSSI на 1 м

// === КАЛИБРОВКА WiFi: RSSI НА 0.5 М И 1 М ===
float wifiRssiAt05m[3] = {-45, -45, -45}; // RSSI на 0.5 м
float wifiRssiAt1m[3] = {-52, -52, -52};  // RSSI на 1 м

// === ФУНКЦИИ ДЛЯ EEPROM ===================
void loadCalibration() {
  EEPROM.begin(60); // 12 (BLE 0.5m) + 12 (BLE 1m) + 12 (WiFi 0.5m) + 12 (WiFi 1m) + 12 (неиспользуемый) = 60
  for (int i = 0; i < 3; i++) {
    EEPROM.get(i * sizeof(float), rssiAt05m[i]);
    if (isnan(rssiAt05m[i]) || rssiAt05m[i] == 0.0) rssiAt05m[i] = -40;
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.get(12 + i * sizeof(float), rssiAt1m[i]);
    if (isnan(rssiAt1m[i]) || rssiAt1m[i] == 0.0) rssiAt1m[i] = -59;
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.get(24 + i * sizeof(float), wifiRssiAt05m[i]);
    if (isnan(wifiRssiAt05m[i]) || wifiRssiAt05m[i] == 0.0) wifiRssiAt05m[i] = -45;
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.get(36 + i * sizeof(float), wifiRssiAt1m[i]);
    if (isnan(wifiRssiAt1m[i]) || wifiRssiAt1m[i] == 0.0) wifiRssiAt1m[i] = -52;
  }
  Serial.println("Calibration loaded from EEPROM.");
}

void saveCalibration() {
  for (int i = 0; i < 3; i++) {
    EEPROM.put(i * sizeof(float), rssiAt05m[i]);
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.put(12 + i * sizeof(float), rssiAt1m[i]);
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.put(24 + i * sizeof(float), wifiRssiAt05m[i]);
  }
  for (int i = 0; i < 3; i++) {
    EEPROM.put(36 + i * sizeof(float), wifiRssiAt1m[i]);
  }
  EEPROM.commit();
  Serial.println("Calibration saved to EEPROM.");
}
// ========================================
const float PATH_LOSS = 2.5;

float prevX = 0, prevY = 0;
unsigned long prevTime = 0;
const float maxSpeed = 1.0; // м/с  // Фильтр движения: max скорость 1 м/с

struct Position {
  float x, y, accuracy;
  float wifiX, wifiY, wifiAccuracy;
  float bleX, bleY, bleAccuracy;
  float wifiWeight, bleWeight;
} currentPos;

BLEScan* pBLEScan;

#define SMOOTH_SIZE 7
float xBuffer[SMOOTH_SIZE] = {0};
float yBuffer[SMOOTH_SIZE] = {0};
int bufferIdx = 0;

// === МЕДИАННЫЙ ФИЛЬТР С ОТСЕЧКОЙ ВЫБРОСОВ ===
#define NUM_MEASUREMENTS 7
long wifiDistances[3][NUM_MEASUREMENTS] = {0};
long bleDistances[3][NUM_MEASUREMENTS] = {0};
int measurementIndex[3] = {0, 0, 0};
bool wifiBufferFull[3] = {false};
bool bleBufferFull[3] = {false};
bool wifiStable[3] = {false};
bool bleStable[3] = {false};

// Функция для получения усредненного значения с отсечкой выбросов
long getFilteredDistance(long arr[]) {
  long sorted[NUM_MEASUREMENTS];
  memcpy(sorted, arr, sizeof(long) * NUM_MEASUREMENTS);
  for (int i = 0; i < NUM_MEASUREMENTS - 1; ++i) {
    for (int j = 0; j < NUM_MEASUREMENTS - 1 - i; ++j) {
      if (sorted[j] > sorted[j + 1]) {
        long temp = sorted[j];
        sorted[j] = sorted[j + 1];
        sorted[j + 1] = temp;
      }
    }
  }
  // Используем медиану как опорную точку
  long median = sorted[NUM_MEASUREMENTS / 2];

  // Считаем усредненное значение с отсечкой выбросов (±20 см)
  long sum = 0;
  int count = 0;
  long threshold = 20 * 100; // 20 см в мм
  for (int i = 0; i < NUM_MEASUREMENTS; i++) {
    if (abs(arr[i] - median) <= threshold) {
      sum += arr[i];
      count++;
    }
  }
  if (count == 0) {
    return median; // если все выбросы — возвращаем медиану
  }
  return sum / count;
}
// ===================================

unsigned long lastScan = 0;
const int SCAN_INTERVAL = 1000; // 1 секунда

void readDHT() {
  float h = dht.readHumidity();
  float t = dht.readTemperature(); 
  
  // Проверка на ошибки (датчик может вернуть NaN)
  if (isnan(h) || isnan(t)) {
    Serial.println(F("  ❌ ОШИБКА: Не удалось считать данные с DHT11."));
    return;
  }
  
  currentHumidity = h;
  currentTemp = t;

  Serial.print("  Temp: "); Serial.print(currentTemp, 1); Serial.print("°C | ");
  Serial.print("Humidity: "); Serial.print(currentHumidity, 1); Serial.println("%");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  dht.begin();
  
  loadCalibration(); // загружаем калибровку
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

void loop() {
  unsigned long now = millis();

  // === КАЛИБРОВКА ПО Serial ==================
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("cal_wifi_05")) {
      int beaconId = cmd.charAt(12) - '1';
      float newRssi = cmd.substring(14).toFloat();

      if (beaconId >= 0 && beaconId < 3) {
        wifiRssiAt05m[beaconId] = newRssi;
        Serial.print("WiFi Cal 0.5m Beacon ");
        Serial.print(beaconId + 1);
        Serial.print(" RSSI = ");
        Serial.print(newRssi, 1);
        Serial.println(" dBm");
        saveCalibration();
      } else {
        Serial.println("Invalid beacon ID. Use: cal_wifi_05 1 -45");
      }
    }
    else if (cmd.startsWith("cal_wifi_1")) {
      int beaconId = cmd.charAt(10) - '1';
      float newRssi = cmd.substring(12).toFloat();

      if (beaconId >= 0 && beaconId < 3) {
        wifiRssiAt1m[beaconId] = newRssi;
        Serial.print("WiFi Cal 1m Beacon ");
        Serial.print(beaconId + 1);
        Serial.print(" RSSI = ");
        Serial.print(newRssi, 1);
        Serial.println(" dBm");
        saveCalibration();
      } else {
        Serial.println("Invalid beacon ID. Use: cal_wifi_1 1 -50");
      }
    }
    else if (cmd.startsWith("cal_ble_05")) {
      int beaconId = cmd.charAt(11) - '1';
      float newRssi = cmd.substring(13).toFloat();

      if (beaconId >= 0 && beaconId < 3) {
        rssiAt05m[beaconId] = newRssi;
        Serial.print("BLE Cal 0.5m Beacon ");
        Serial.print(beaconId + 1);
        Serial.print(" RSSI = ");
        Serial.print(newRssi, 1);
        Serial.println(" dBm");
        saveCalibration();
      } else {
        Serial.println("Invalid beacon ID. Use: cal_ble_05 1 -42");
      }
    }
    else if (cmd.startsWith("cal_ble_1")) {
      int beaconId = cmd.charAt(9) - '1';
      float newRssi = cmd.substring(11).toFloat();

      if (beaconId >= 0 && beaconId < 3) {
        rssiAt1m[beaconId] = newRssi;
        Serial.print("BLE Cal 1m Beacon ");
        Serial.print(beaconId + 1);
        Serial.print(" RSSI = ");
        Serial.print(newRssi, 1);
        Serial.println(" dBm");
        saveCalibration();
      } else {
        Serial.println("Invalid beacon ID. Use: cal_ble_1 1 -55");
      }
    }
    else if (cmd.equals("reset_position")) {
      currentPos.x = 0.0;
      currentPos.y = 0.0;
      currentPos.accuracy = 0.0;
      currentPos.wifiX = 0.0;
      currentPos.wifiY = 0.0;
      currentPos.wifiAccuracy = 0.0;
      currentPos.bleX = 0.0;
      currentPos.bleY = 0.0;
      currentPos.bleAccuracy = 0.0;
      currentPos.wifiWeight = 0.0;
      currentPos.bleWeight = 0.0;

      // Очищаем буфер сглаживания
      for (int i = 0; i < SMOOTH_SIZE; i++) {
        xBuffer[i] = 0.0;
        yBuffer[i] = 0.0;
      }
      bufferIdx = 0;

      Serial.println("Position reset to (0, 0).");
    }
  }
  // ========================================

  if (now - lastScan > SCAN_INTERVAL) {
    lastScan = now;
    
    readDHT();
    
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
        
        // === ЛИНЕЙНАЯ ИНТЕРПОЛЯЦИЯ RSSI → DISTANCE ===
        float a = (0.5 - 1.0) / (wifiRssiAt05m[j] - wifiRssiAt1m[j]);
        float b = 1.0 - a * wifiRssiAt1m[j];
        beacons[j].wifiDistance = a * rssi + b;

        // Ограничиваем минимальное расстояние
        if (beacons[j].wifiDistance < 0.1) {
          beacons[j].wifiDistance = 0.1;
        }
        // ========================================

        if (beacons[j].wifiDistance > 10.0 || beacons[j].wifiDistance < 0.1) {
          Serial.print("  ⚠️ IGNORING ANOMALY: ");
          Serial.print(beacons[j].wifiName);
          Serial.print(" | Dist: ");
          Serial.print(beacons[j].wifiDistance, 1);
          Serial.println("m");
          continue; // пропускаем это значение
        }

        long rawWifiDistance = (long)(beacons[j].wifiDistance * 100); // в мм
        wifiDistances[j][measurementIndex[j]] = rawWifiDistance;
        measurementIndex[j]++;
        if (measurementIndex[j] >= NUM_MEASUREMENTS) {
          wifiBufferFull[j] = true;
          measurementIndex[j] = 0;
        }

        if (wifiBufferFull[j]) {
          long filtered = getFilteredDistance(wifiDistances[j]);
          beacons[j].wifiDistance = filtered / 100.0; // делим обратно
          // Проверим, стабильно ли значение
          wifiStable[j] = true; // можно улучшить проверку, если нужно
        }

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

        // === УСРЕДНЕНИЕ RSSI ==================
        static int count[3] = {0};  // счётчик для каждого маяка
        static int totalRssi[3] = {0}; // сумма RSSI

        totalRssi[j] += rssi;
        count[j]++;

        if (count[j] >= 3) { // усредняем за 3 измерения
          float avgRssi = (float)totalRssi[j] / count[j];
          count[j] = 0;
          totalRssi[j] = 0;

          if (avgRssi > -15 || avgRssi < -90) { // RSSI не должен быть > -15 или < -90
            Serial.print("  ⚠️ AVG RSSI ANOMALY for ");
            Serial.print(beacons[j].bleName);
            Serial.print(" avgRssi: ");
            Serial.print(avgRssi, 1);
            Serial.println("dBm");
            continue; // пропускаем
          }

          // === ЛИНЕЙНАЯ ИНТЕРПОЛЯЦИЯ RSSI → DISTANCE ===
          float a = (0.5 - 1.0) / (rssiAt05m[j] - rssiAt1m[j]);
          float b = 1.0 - a * rssiAt1m[j];
          beacons[j].bleDistance = a * avgRssi + b;

          // Ограничиваем минимальное расстояние
          if (beacons[j].bleDistance < 0.1) {
            beacons[j].bleDistance = 0.1;
          }
          // ========================================

          // === ПРОВЕРКА НА АНОМАЛЬНОЕ ЗНАЧЕНИЕ (расстояние) ===
          if (beacons[j].bleDistance > 10.0 || beacons[j].bleDistance < 0.1) {
            Serial.print("  ⚠️ IGNORING ANOMALY: ");
            Serial.print(beacons[j].bleName);
            Serial.print(" | Dist: ");
            Serial.print(beacons[j].bleDistance, 1);
            Serial.println("m");
            continue; // пропускаем это значение
          }

          // === ФИЛЬТРАЦИЯ РАССТОЯНИЯ ==================
          long rawBleDistance = (long)(beacons[j].bleDistance * 100); // в мм
          bleDistances[j][measurementIndex[j]] = rawBleDistance;
          measurementIndex[j]++;
          if (measurementIndex[j] >= NUM_MEASUREMENTS) {
            bleBufferFull[j] = true;
            measurementIndex[j] = 0;
          }

          if (bleBufferFull[j]) {
            long filtered = getFilteredDistance(bleDistances[j]);
            beacons[j].bleDistance = filtered / 100.0; // делим обратно
            bleStable[j] = true; // можно улучшить проверку, если нужно
          }
          // ==========================================

          Serial.print("  ✅ ");
          Serial.print(beacons[j].bleName);
          Serial.print(" | RSSI: ");
          Serial.print(avgRssi, 1); // показываем усреднённый RSSI
          Serial.print(" | Dist: ");
          Serial.print(beacons[j].bleDistance, 1);
          Serial.println("m");
        }
        // ========================================
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
  if (wifiStable[0] && wifiStable[1] && wifiStable[2]) {
    wifiOK = trilaterate(
      beacons[0].wifiDistance, beacons[1].wifiDistance, beacons[2].wifiDistance,
      beacons[0].x, beacons[0].y, beacons[1].x, beacons[1].y, beacons[2].x, beacons[2].y,
      currentPos.wifiX, currentPos.wifiY, currentPos.wifiAccuracy
    );
  }
  
  // Трилатерация по BLE
  bool bleOK = false;
  if (bleStable[0] && bleStable[1] && bleStable[2]) {
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

  // Фильтр движения: проверяет, возможна ли такая скорость
  unsigned long now = millis();
  float dt = (now - prevTime) / 1000.0;

  if (prevTime != 0 && dt > 0) {
    float dx = currentPos.x - prevX;
    float dy = currentPos.y - prevY;
    float dist = sqrt(dx * dx + dy * dy);
    float speed = dist / dt;

    if (speed > maxSpeed) {
      // Прыжок слишком большой — игнорируем
      currentPos.x = prevX;
      currentPos.y = prevY;
      Serial.println("  ⚠️ Movement filter: Ignoring large jump.");
    } else {
      // Все ок — обновляем
      prevX = currentPos.x;
      prevY = currentPos.y;
      prevTime = now;
    }
  } else {
    // Первый раз — просто обновляем
    prevX = currentPos.x;
    prevY = currentPos.y;
    prevTime = now;
  }
}

void printStatus() {
  Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("📊 STATUS SUMMARY\n");
  
  // --- ДОБАВИТЬ ВЫВОД СТАТУСА DHT ---
  Serial.print("Environment: Temp: "); Serial.print(currentTemp, 1); 
  Serial.print("°C, Humidity: "); Serial.print(currentHumidity, 1); Serial.println("%");
  // ------------------------------------
  
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
  
  // --- ДОБАВЛЕНИЕ DHT ДАННЫХ В JSON ---
  JsonObject env = doc.createNestedObject("environment");
  env["temperature"] = currentTemp;
  env["humidity"] = currentHumidity;
  // ------------------------------------
  
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