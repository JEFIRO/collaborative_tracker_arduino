#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define SERVICE_UUID "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef01-1234-5678-1234-56789abcdef0"

unsigned long lastRequestTime = 0;
const unsigned long interval = 5 * 60 * 1000UL;

volatile bool sendRequest = false;
volatile bool scanDone = false;
int scanCount = 0;

bool shouldSendData = false;

class SavePreferences {
public:
  String ssid;
  String password;
  String trackerId;
  String deviceMacAddress;
};

SavePreferences saved;

class SendLocate {
public:
  static String generateUUIDStatic() {
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
};

String buildCombinedJsonWithScan(int scanCount) {
  DynamicJsonDocument doc(4096);

  JsonObject locationRequest = doc.createNestedObject("locationRequestDTO");
  locationRequest["idLocate"] = SendLocate::generateUUIDStatic();
  locationRequest["deviceMacAddress"] = saved.deviceMacAddress;
  locationRequest["trackerId"] = saved.trackerId;
  locationRequest["rssi"] = 15;

  JsonObject wifiList = doc.createNestedObject("wifiListDTO");
  JsonArray wifiArray = wifiList.createNestedArray("wifiAccessPoints");

  for (int i = 0; i < scanCount && i < 10; i++) {
    JsonObject ap = wifiArray.createNestedObject();
    ap["macAddress"] = WiFi.BSSIDstr(i);
    ap["signalStrength"] = WiFi.RSSI(i);
    ap["channel"] = WiFi.channel(i);
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  return jsonStr;
}

void doWiFiScan() {
  scanCount = WiFi.scanNetworks(false, true);
  scanDone = true;
}

void sendTask(void *param) {
  while (true) {
    if (sendRequest) {
      sendRequest = false;

      scanDone = false;
      doWiFiScan();

      if (scanDone) {
        String jsonPayload = buildCombinedJsonWithScan(scanCount);

        HTTPClient http;
        if (WiFi.status() == WL_CONNECTED) {
          http.begin("https://rastreamento-colaborativo-v1.onrender.com/tracker/post");
          http.addHeader("Content-Type", "application/json");
          int httpCode = http.POST(jsonPayload);
          Serial.printf("HTTP POST status: %d\n", httpCode);
          if (httpCode > 0) {
            Serial.println(http.getString());
          } else {
            Serial.println("Erro ao enviar dados");
          }
          http.end();
        } else {
          Serial.println("Wi-Fi não conectado para enviar dados.");
        }
      } else {
        Serial.println("Scan Wi-Fi não foi concluído.");
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

class MyPreferences {
private:
  Preferences preferences;

public:
  void savePreferences(BLECharacteristic *pCharacteristic) {
    String jsonStr = String(pCharacteristic->getValue().c_str());
    Serial.println("Recebido via BLE:");
    Serial.println(jsonStr);

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (!error) {
      String received_ssid = doc["ssid"] | "";
      String received_password = doc["password"] | "";
      String received_trackerId = doc["trackerId"] | "";
      String received_deviceMacAddress = doc["deviceMacAddress"] | "";

      preferences.begin("config", false);
      preferences.putString("ssid", received_ssid);
      preferences.putString("password", received_password);
      preferences.putString("trackerId", received_trackerId);
      preferences.putString("macAddress", received_deviceMacAddress);
      preferences.end();

      Serial.println("Preferências salvas. Reiniciando ESP...");
      delay(1000);
      ESP.restart();  // Reinicia para aplicar novas credenciais
    } else {
      Serial.println("Erro ao fazer parse do JSON.");
    }
  }

  SavePreferences getPreferences() {
    SavePreferences data;
    preferences.begin("config", true);
    data.deviceMacAddress = preferences.getString("macAddress", "");
    data.ssid = preferences.getString("ssid", "");
    data.password = preferences.getString("password", "");
    data.trackerId = preferences.getString("trackerId", "");
    preferences.end();
    return data;
  }
};

class WiFiCredentialCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    MyPreferences prefs;
    prefs.savePreferences(pCharacteristic);
  }
};

void setupBLE() {
  BLEDevice::init("ESP32_Tracker");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_READ);

  pCharacteristic->setCallbacks(new WiFiCredentialCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE pronto e anunciando.");
}

bool tentaConectarWiFi(SavePreferences &saved) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(saved.ssid.c_str(), saved.password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(1000);
    Serial.print(".");
  }

  return WiFi.status() == WL_CONNECTED;
}

void verificaSeTemPreferenciasSalvas() {
  MyPreferences prefs;
  while (true) {
    saved = prefs.getPreferences();
    if (!saved.ssid.isEmpty() && !saved.password.isEmpty() && !saved.trackerId.isEmpty() && !saved.deviceMacAddress.isEmpty()) {
      Serial.println("Tentando conectar ao Wi-Fi...");
      if (tentaConectarWiFi(saved)) {
        Serial.println("\nWi-Fi conectado com sucesso!");
        shouldSendData = true;
        configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("Sincronizando hora...");
        time_t now = time(nullptr);
        while (now < 8 * 3600 * 2) {  // Espera até o horário ser sincronizado
          delay(500);
          Serial.print(".");
          now = time(nullptr);
        }
        Serial.println();
        Serial.println("Hora sincronizada: " + String(ctime(&now)));

        break;
      } else {
        Serial.println("\nFalha na conexão Wi-Fi. Aguardando nova configuração via BLE...");
      }
    } else {
      Serial.println("Aguardando configuração via BLE...");
    }

    delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  setupBLE();
  verificaSeTemPreferenciasSalvas();

  xTaskCreate(sendTask, "SendTask", 8192, NULL, 1, NULL);

  Serial.println("Dados salvos:");
  Serial.println("SSID: " + saved.ssid);
  Serial.println("Password: " + saved.password);
  Serial.println("Tracker ID: " + saved.trackerId);
  Serial.println("Device MAC: " + saved.deviceMacAddress);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi desconectado. Tentando reconectar...");
    MyPreferences prefs;
    saved = prefs.getPreferences();
    if (tentaConectarWiFi(saved)) {
      Serial.println("Reconectado com sucesso!");
      shouldSendData = true;
    } else {
      Serial.println("Falha ao reconectar.");
      delay(5000);
      return;
    }
  }

  unsigned long now = millis();
  if (shouldSendData || now - lastRequestTime >= interval) {
    shouldSendData = false;
    lastRequestTime = now;
    sendRequest = true;  // sinaliza para a task enviar dados
  }

  delay(1000);
}
