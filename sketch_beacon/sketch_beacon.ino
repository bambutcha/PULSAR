#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#define BEACON_ID 3

// Имена для WiFi и BLE
String wifiName = "Beacon_" + String(BEACON_ID);
String bleName = "BLE_Beacon_" + String(BEACON_ID);

const char* wifiPassword = "12345678";

// BLE характеристики
BLEServer* pServer = NULL;
BLEAdvertising* pAdvertising = NULL;

void setup() {
  pinMode(2, OUTPUT);

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

  Serial.println("Blinking ID code...");
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
    delay(500);  // Небольшая пауза перед миганием

    for (int i = 0; i < BEACON_ID; i++) {
      digitalWrite(2, HIGH);  // Включить светодиод
      delay(250);             // Пауза 0.25 сек
      digitalWrite(2, LOW);   // Выключить светодиод
      delay(400);             // Пауза между миганиями
    }
    Serial.println("Blinking finished.");
  }
}