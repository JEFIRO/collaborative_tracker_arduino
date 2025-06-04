#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

Preferences preferences;

String ssid;
String password;
String trackerId;
String deviceMacAddress;
bool shouldSendData = false;

const char* trackerEndpoint = "https://rastreamento-colaborativo-v1.onrender.com/tracker/post";
unsigned long lastRequestTime = 0;
const unsigned long interval = 1 * 60 * 1000UL;
bool attemptedReconnect = false;

#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef01-1234-5678-1234-56789abcdef0"

// ==================== GERAÇÃO DE JSON ====================
String generateUUID() {
  uint8_t uuid[16];
  for (int i = 0; i < 16; i++) uuid[i] = random(0, 256);
  uuid[6] = (uuid[6] & 0x0F) | 0x40;
  uuid[8] = (uuid[8] & 0x3F) | 0x80;
  char buf[37];
  snprintf(buf, sizeof(buf),
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    uuid[0], uuid[1], uuid[2], uuid[3],
    uuid[4], uuid[5],
    uuid[6], uuid[7],
    uuid[8], uuid[9],
    uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
  return String(buf);
}

String buildCombinedJson() {
  DynamicJsonDocument doc(4096);

  JsonObject locationRequest = doc.createNestedObject("locationRequestDTO");
  locationRequest["idLocate"] = generateUUID();
  locationRequest["deviceMacAddress"] = deviceMacAddress;
  locationRequest["trackerId"] = trackerId;
  locationRequest["rssi"] = 15;

  JsonObject wifiList = doc.createNestedObject("wifiListDTO");
  JsonArray wifiArray = wifiList.createNestedArray("wifiAccessPoints");

  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n && i < 10; i++) {
    JsonObject ap = wifiArray.createNestedObject();
    ap["macAddress"] = WiFi.BSSIDstr(i);
    ap["signalStrength"] = WiFi.RSSI(i);
    ap["channel"] = WiFi.channel(i);
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  return jsonStr;
}

// ==================== ENVIO DE DADOS ====================
void sendTrackerData() {
  HTTPClient http;
  http.begin(trackerEndpoint);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = buildCombinedJson();
  Serial.println("JSON enviado para Tracker:");
  Serial.println(jsonPayload);

  int httpCode = http.POST(jsonPayload);
  Serial.printf("Envio para Tracker API: HTTP %d\n", httpCode);
  if (httpCode > 0) {
    Serial.println(http.getString());
  } else {
    Serial.println("Erro ao enviar para Tracker API.");
  }
  http.end();
}

// ==================== CALLBACK BLE ====================
class WiFiCredentialCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String jsonStr = String(pCharacteristic->getValue().c_str());
    Serial.println("Recebido via BLE:");
    Serial.println(jsonStr);

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (!error) {
      ssid = doc["ssid"] | "";
      password = doc["password"] | "";
      trackerId = doc["trackerId"] | "";
      deviceMacAddress = doc["deviceMacAddress"] | "";

      preferences.begin("config", false);
      preferences.putString("ssid", ssid);
      preferences.putString("password", password);
      preferences.putString("trackerId", trackerId);
      preferences.putString("deviceAddress", deviceMacAddress);
      preferences.end();

      WiFi.disconnect();
      WiFi.begin(ssid.c_str(), password.c_str());

      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConectado após BLE!");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        delay(2000);
        shouldSendData = true;
        lastRequestTime = millis();
        attemptedReconnect = false;
      } else {
        Serial.println("\nFalha ao conectar após BLE.");
      }
    } else {
      Serial.println("Erro ao parsear JSON recebido.");
    }
  }
};

// ==================== BLE ====================
void setupBLE() {
  BLEDevice::init("ESP32_Tracker");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );

  pCharacteristic->setCallbacks(new WiFiCredentialCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE ativo aguardando credenciais...");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin("config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  trackerId = preferences.getString("trackerId", "default-tracker");
  deviceMacAddress = preferences.getString("deviceAddress", "");
  preferences.end();

  Serial.println("Setup:");
  Serial.println("SSID: " + ssid);
  Serial.println("Password: " + password);
  Serial.println("Tracker ID: " + trackerId);
  Serial.println("MAC Address: " + deviceMacAddress);

  if (ssid == "" || password == "") {
    Serial.println("Nenhuma credencial salva. Ativando BLE.");
    setupBLE();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.println("Conectando ao Wi-Fi...");

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado!");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    lastRequestTime = millis();
  } else {
    Serial.println("\nFalha ao conectar. Ativando BLE.");
    setupBLE();
  }
}

// ==================== LOOP ====================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!attemptedReconnect) {
      Serial.println("Wi-Fi desconectado. Tentando reconectar...");
      WiFi.begin(ssid.c_str(), password.c_str());

      unsigned long startTime = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
        delay(500);
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nReconectado!");
        lastRequestTime = millis();
        attemptedReconnect = false;
      } else {
        Serial.println("\nReconexão falhou. Reiniciando BLE.");
        setupBLE();
        attemptedReconnect = true;
      }
    }

    delay(10000);
    return;
  }

  unsigned long now = millis();
  if (now - lastRequestTime >= interval) {
    sendTrackerData();
    lastRequestTime = now;
  }

  if (shouldSendData) {
    sendTrackerData();
    lastRequestTime = millis();
    shouldSendData = false;
  }
}
