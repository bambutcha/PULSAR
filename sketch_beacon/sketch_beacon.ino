#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// ⚠️ ВАЖНО: Для каждого маяка меняйте ID!
// Маяк 1: BEACON_ID = 1
// Маяк 2: BEACON_ID = 2
// Маяк 3: BEACON_ID = 3
#define BEACON_ID 1

// Имена для WiFi и BLE
String wifiName = "Beacon_" + String(BEACON_ID);
String bleName = "BLE_Beacon_" + String(BEACON_ID);

const char* wifiPassword = "12345678";

// BLE характеристики
BLEServer* pServer = NULL;
BLEAdvertising* pAdvertising = NULL;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("   HYBRID BEACON: WiFi + BLE");
  Serial.println("========================================");
  Serial.print("Beacon ID: ");
  Serial.println(BEACON_ID);
  
  // ========== НАСТРОЙКА WiFi AP ==========
  setupWiFi();
  
  // ========== НАСТРОЙКА BLE ==========
  setupBLE();
  
  Serial.println("========================================");
  Serial.println("✅ Beacon is broadcasting!");
  Serial.println("========================================\n");
}

void setupWiFi() {
  Serial.println("\n[WiFi] Setting up Access Point...");
  
  // Создаем точку доступа WiFi
  WiFi.softAP(wifiName.c_str(), wifiPassword);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] SSID: ");
  Serial.println(wifiName);
  Serial.print("[WiFi] IP: ");
  Serial.println(IP);
}

void setupBLE() {
  Serial.println("\n[BLE] Setting up Bluetooth Beacon...");
  
  // Инициализация BLE
  BLEDevice::init(bleName.c_str());
  
  // Создаем BLE сервер
  pServer = BLEDevice::createServer();
  
  // Настраиваем рекламу (advertising)
  pAdvertising = BLEDevice::getAdvertising();
  
  // Устанавливаем мощность передатчика (для калибровки RSSI)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  
  // Добавляем данные в рекламу
  BLEAdvertisementData advertisementData;
  advertisementData.setName(bleName.c_str());
  
  String manufacturerData;
  manufacturerData += (char)0xFF;  // Company ID (custom)
  manufacturerData += (char)0xFF;
  manufacturerData += (char)BEACON_ID;  // Наш ID маяка
  
  advertisementData.setManufacturerData(manufacturerData);
  pAdvertising->setAdvertisementData(advertisementData);
  
  // Параметры рекламы
  pAdvertising->setMinPreferred(0x06);  // 7.5ms
  pAdvertising->setMaxPreferred(0x12);  // 22.5ms
  
  // Запускаем рекламу
  pAdvertising->start();
  
  Serial.print("[BLE] Name: ");
  Serial.println(bleName);
  Serial.println("[BLE] Broadcasting started");
}

void loop() {
  delay(1000);
  
  // Показываем статистику каждые 5 секунд
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    
    Serial.println("─────────────────────────────");
    Serial.print("⏱️  Uptime: ");
    Serial.print(millis() / 1000);
    Serial.println(" sec");
    
    Serial.print("📡 WiFi Clients: ");
    Serial.println(WiFi.softAPgetStationNum());
    
    Serial.print("🔵 BLE Status: ");
    Serial.println("Broadcasting");
    
    Serial.println("─────────────────────────────\n");
  }
}