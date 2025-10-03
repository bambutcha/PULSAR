// ГИБРИДНЫЙ ПРИЁМНИК: WiFi + BLE Scanner + WebSocket Server
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <math.h>

// ========== КОНФИГУРАЦИЯ ==========

// Координаты маяков (в метрах)
struct Beacon {
  int id;
  const char* wifiName;
  const char* bleName;
  float x, y;
  
  // Данные WiFi
  int wifiRSSI;
  float wifiDistance;
  
  // Данные BLE
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

// Калибровочные параметры
const float WIFI_RSSI_AT_1M = -40;
const float BLE_RSSI_AT_1M = -59;
const float PATH_LOSS = 2.5;

// Текущая позиция
struct Position {
  float x, y;
  float accuracy;
  float wifiX, wifiY, wifiAccuracy;
  float bleX, bleY, bleAccuracy;
  float wifiWeight, bleWeight;
} currentPos;

// BLE сканер
BLEScan* pBLEScan;

// WebSocket сервер на порту 81
WebSocketsServer webSocket = WebSocketsServer(81);

// Буферы для сглаживания
#define SMOOTH_SIZE 3
float xBuffer[SMOOTH_SIZE] = {0};
float yBuffer[SMOOTH_SIZE] = {0};
int bufferIdx = 0;

// Таймеры
unsigned long lastScan = 0;
unsigned long lastUpdate = 0;
const int SCAN_INTERVAL = 2000;  // Сканирование каждые 2 сек
const int UPDATE_INTERVAL = 500;  // Отправка данных каждые 0.5 сек

// ========== SETUP ==========

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("  HYBRID POSITIONING SYSTEM");
  Serial.println("  WiFi + BLE + WebSocket");
  Serial.println("========================================\n");
  
  // Инициализация WiFi (станция для сканирования)
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  
  // Инициализация BLE
  Serial.println("[BLE] Initializing...");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("[BLE] ✅ Ready");
  
  // Создаем свою WiFi точку для WebSocket
  Serial.println("\n[WiFi] Creating Access Point...");
  WiFi.softAP("PositioningSystem", "12345678");
  WiFi.scanNetworks(true);  // true = async scan
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] Connect to: ");
  Serial.println(IP);
  Serial.println("[WiFi] Open: http://" + IP.toString());
  
  // Запуск WebSocket сервера
  Serial.println("\n[WebSocket] Starting server on port 81...");
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("[WebSocket] ✅ Ready");
  
  Serial.println("\n========================================");
  Serial.println("✅ SYSTEM READY");
  Serial.println("========================================\n");
}

// ========== MAIN LOOP ==========

void loop() {
  webSocket.loop();
  
  unsigned long now = millis();
  
  // Сканирование маяков
  if (now - lastScan > SCAN_INTERVAL) {
    lastScan = now;
    
    Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("🔍 SCANNING BEACONS...");
    
    scanWiFi();
    scanBLE();
    
    calculatePosition();
    
    printStatus();
  }
  
  // Отправка данных клиентам
  if (now - lastUpdate > UPDATE_INTERVAL) {
    lastUpdate = now;
    sendDataToClients();
  }
}

// ========== WiFi СКАНИРОВАНИЕ ==========

void scanWiFi() {
  Serial.println("\n[WiFi] Scanning networks...");
  
  // Сброс предыдущих значений
  for (int i = 0; i < 3; i++) {
    beacons[i].wifiFound = false;
  }
  
  int n = WiFi.scanNetworks();
  
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    
    // Ищем наши маяки
    for (int j = 0; j < 3; j++) {
      if (ssid == beacons[j].wifiName) {
        beacons[j].wifiRSSI = rssi;
        beacons[j].wifiFound = true;
        
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
}

// ========== BLE СКАНИРОВАНИЕ ==========

void scanBLE() {
  Serial.println("\n[BLE] Scanning devices...");
  
  // Сброс предыдущих значений
  for (int i = 0; i < 3; i++) {
    beacons[i].bleFound = false;
  }
  
  BLEScanResults* foundDevices = pBLEScan->start(1, false);
  
  for (int i = 0; i < foundDevices->getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices->getDevice(i);
    String name = device.getName().c_str();
    int rssi = device.getRSSI();
    
    // Ищем наши маяки
    for (int j = 0; j < 3; j++) {
      if (name == beacons[j].bleName) {
        beacons[j].bleRSSI = rssi;
        beacons[j].bleFound = true;
        
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
  
  pBLEScan->clearResults();
}

// ========== ТРИЛАТЕРАЦИЯ ==========

bool trilaterate(float d1, float d2, float d3, 
                 float x1, float y1, float x2, float y2, float x3, float y3,
                 float &x, float &y, float &accuracy) {
  
  // Проверка на валидные расстояния
  if (d1 <= 0 || d2 <= 0 || d3 <= 0) {
    return false;
  }
  
  // Метод наименьших квадратов
  float A = 2 * (x2 - x1);
  float B = 2 * (y2 - y1);
  float C = d1*d1 - d2*d2 - x1*x1 + x2*x2 - y1*y1 + y2*y2;
  
  float D = 2 * (x3 - x2);
  float E = 2 * (y3 - y2);
  float F = d2*d2 - d3*d3 - x2*x2 + x3*x3 - y2*y2 + y3*y3;
  
  float denominator = (E*A - B*D);
  if (abs(denominator) < 0.001) {
    return false;  // Вырожденная система
  }
  
  x = (C*E - F*B) / denominator;
  y = (C*D - A*F) / (B*D - A*E);
  
  // Проверка на NaN
  if (isnan(x) || isnan(y)) {
    return false;
  }
  
  // Расчет точности (средняя ошибка)
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

// ========== РАСЧЕТ ПОЗИЦИИ ==========

void calculatePosition() {
  Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("📐 CALCULATING POSITION...\n");
  
  // Трилатерация по WiFi
  bool wifiOK = false;
  if (beacons[0].wifiFound && beacons[1].wifiFound && beacons[2].wifiFound) {
    wifiOK = trilaterate(
      beacons[0].wifiDistance, beacons[1].wifiDistance, beacons[2].wifiDistance,
      beacons[0].x, beacons[0].y, beacons[1].x, beacons[1].y, beacons[2].x, beacons[2].y,
      currentPos.wifiX, currentPos.wifiY, currentPos.wifiAccuracy
    );
    
    if (wifiOK) {
      Serial.println("[WiFi] Position calculated:");
      Serial.print("  X: "); Serial.print(currentPos.wifiX, 2); Serial.println(" m");
      Serial.print("  Y: "); Serial.print(currentPos.wifiY, 2); Serial.println(" m");
      Serial.print("  Accuracy: ±"); Serial.print(currentPos.wifiAccuracy, 2); Serial.println(" m");
    }
  }
  
  // Трилатерация по BLE
  bool bleOK = false;
  if (beacons[0].bleFound && beacons[1].bleFound && beacons[2].bleFound) {
    bleOK = trilaterate(
      beacons[0].bleDistance, beacons[1].bleDistance, beacons[2].bleDistance,
      beacons[0].x, beacons[0].y, beacons[1].x, beacons[1].y, beacons[2].x, beacons[2].y,
      currentPos.bleX, currentPos.bleY, currentPos.bleAccuracy
    );
    
    if (bleOK) {
      Serial.println("\n[BLE] Position calculated:");
      Serial.print("  X: "); Serial.print(currentPos.bleX, 2); Serial.println(" m");
      Serial.print("  Y: "); Serial.print(currentPos.bleY, 2); Serial.println(" m");
      Serial.print("  Accuracy: ±"); Serial.print(currentPos.bleAccuracy, 2); Serial.println(" m");
    }
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
    
    Serial.println("\n[FUSION] Combined position:");
    Serial.print("  WiFi weight: "); Serial.println(currentPos.wifiWeight, 2);
    Serial.print("  BLE weight: "); Serial.println(currentPos.bleWeight, 2);
    
  } else if (wifiOK) {
    currentPos.x = currentPos.wifiX;
    currentPos.y = currentPos.wifiY;
    currentPos.accuracy = currentPos.wifiAccuracy;
    currentPos.wifiWeight = 1.0;
    currentPos.bleWeight = 0.0;
    Serial.println("\n[FUSION] Using WiFi only");
    
  } else if (bleOK) {
    currentPos.x = currentPos.bleX;
    currentPos.y = currentPos.bleY;
    currentPos.accuracy = currentPos.bleAccuracy;
    currentPos.wifiWeight = 0.0;
    currentPos.bleWeight = 1.0;
    Serial.println("\n[FUSION] Using BLE only");
    
  } else {
    Serial.println("\n⚠️ Not enough beacons found!");
    return;
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
  
  Serial.println("\n📍 FINAL POSITION:");
  Serial.print("  X: "); Serial.print(currentPos.x, 2); Serial.println(" m");
  Serial.print("  Y: "); Serial.print(currentPos.y, 2); Serial.println(" m");
  Serial.print("  Accuracy: ±"); Serial.print(currentPos.accuracy, 2); Serial.println(" m");
}

// ========== ВЫВОД СТАТУСА ==========

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
  
  Serial.print("\nWebSocket Clients: ");
  Serial.println(webSocket.connectedClients());
  
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

// ========== WEBSOCKET EVENTS ==========

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WebSocket] Client #%u disconnected\n", num);
      break;
      
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[WebSocket] Client #%u connected from %d.%d.%d.%d\n", 
          num, ip[0], ip[1], ip[2], ip[3]);
      }
      break;
  }
}

// ========== ОТПРАВКА ДАННЫХ ==========

void sendDataToClients() {
  if (webSocket.connectedClients() == 0) return;
  
  // Создаем JSON
  StaticJsonDocument<1024> doc;
  
  doc["timestamp"] = millis();
  
  // Позиция
  JsonObject pos = doc.createNestedObject("position");
  pos["x"] = currentPos.x;
  pos["y"] = currentPos.y;
  pos["accuracy"] = currentPos.accuracy;
  
  // WiFi данные
  JsonObject wifi = doc.createNestedObject("wifi");
  for (int i = 0; i < 3; i++) {
    JsonObject b = wifi.createNestedObject("beacon" + String(i+1));
    b["rssi"] = beacons[i].wifiRSSI;
    b["distance"] = beacons[i].wifiDistance;
    b["found"] = beacons[i].wifiFound;
  }
  
  // BLE данные
  JsonObject ble = doc.createNestedObject("ble");
  for (int i = 0; i < 3; i++) {
    JsonObject b = ble.createNestedObject("beacon" + String(i+1));
    b["rssi"] = beacons[i].bleRSSI;
    b["distance"] = beacons[i].bleDistance;
    b["found"] = beacons[i].bleFound;
  }
  
  // Fusion данные
  JsonObject fusion = doc.createNestedObject("fusion");
  fusion["wifi_weight"] = currentPos.wifiWeight;
  fusion["ble_weight"] = currentPos.bleWeight;
  
  // Сериализация и отправка
  String json;
  serializeJson(doc, json);
  webSocket.broadcastTXT(json);
}