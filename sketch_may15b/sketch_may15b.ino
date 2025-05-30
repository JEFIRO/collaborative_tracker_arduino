#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ==================== OBJETOS E CONFIG ====================
Preferences preferences;

String ssid;
String password;
String trackerId;
String deviceMacAddress;
bool shouldSendData = false;

// Variáveis globais

const char* trackerEndpoint = "https://rastreamento-colaborativo-v1.onrender.com/tracker/post";
unsigned long lastRequestTime = 0;
const unsigned long interval = 1 * 60 * 1000UL;
bool attemptedReconnect = false;

// ==================== FUNÇÕES AUXILIARES ====================
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

  // DEBUG: Mostra o valor de deviceMacAddress antes de construir o JSON
  Serial.println("buildCombinedJson: Usando deviceMacAddress: " + deviceMacAddress);

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

// ==================== BLE CALLBACK COM SALVAMENTO ====================
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef01-1234-5678-1234-56789abcdef0"

class WiFiCredentialCallbacks : public BLECharacteristicCallbacks {
 void onWrite(BLECharacteristic *pCharacteristic) override {
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

      // DEBUG: Mostra os valores recebidos via BLE
      Serial.println("onWrite: ssid recebido: " + received_ssid);
      Serial.println("onWrite: deviceMacAddress recebido: " + received_deviceMacAddress);

      // Salvar nas preferências
      preferences.begin("config", false);
      preferences.putString("ssid", received_ssid);
      preferences.putString("password", received_password);
      preferences.putString("trackerId", received_trackerId);
      preferences.putString("deviceMacAddress", received_deviceMacAddress);
      preferences.end();

      ssid = received_ssid;
      password = received_password;
      trackerId = received_trackerId;
      deviceMacAddress = received_deviceMacAddress;

      // DEBUG: Mostra o valor de deviceMacAddress APÓS a atribuição às variáveis globais
      Serial.println("onWrite: deviceMacAddress APÓS atribuição global: " + deviceMacAddress);

      Serial.println("Variáveis globais atualizadas após BLE:");
      Serial.println("SSID: " + ssid);
      Serial.println("Tracker ID: " + trackerId);
      Serial.println("Device MAC: " + deviceMacAddress);
      Serial.println("Credenciais atualizadas e salvas!");

      // Conecta imediatamente ao novo Wi-Fi
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
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
        shouldSendData = true; // Define para enviar dados na próxima iteração do loop
        lastRequestTime = millis();
        attemptedReconnect = false; // libera tentativas futuras se necessário
      } else {
        Serial.println("\nFalha ao conectar após BLE.");
      }
    } else {
      Serial.println("Erro ao parsear JSON recebido.");
    }
  }
};


void setupBLE() {
  BLEDevice::init("ESP32_Tracker");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_READ
  );

  pCharacteristic->setCallbacks(new WiFiCredentialCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE pronto e anunciando.");
}


// ==================== SETUP E LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Lê as credenciais salvas
  preferences.begin("config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  trackerId = preferences.getString("trackerId", "default-tracker");
  deviceMacAddress = preferences.getString("deviceMacAddress", "");

  
  preferences.end();

  // DEBUG: Mostra o valor de deviceMacAddress carregado do Preferences no setup
  Serial.println("Setup: SSID carregado das preferências: " + ssid);
  Serial.println("Setup: Password carregado das preferências: " + password);
  Serial.println("Setup: deviceMacAddress carregado das preferências: " + deviceMacAddress);


  

  setupBLE();

  if (ssid == "" || password == "") {
    Serial.println("Nenhuma credencial salva. Aguardando BLE...");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.print("Conectando ao Wi-Fi: ");
  Serial.println(ssid);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado!");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    delay(2000);
    // Não enviar dados imediatamente aqui. Esperar pela configuração BLE.
    lastRequestTime = millis();
  } else {
    Serial.println("\nFalha ao conectar. Aguardando nova configuração via BLE.");
  }
}

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
        lastRequestTime = millis(); // reinicia temporizador
        attemptedReconnect = false; // resetar a flag
      } else {
        Serial.println("\nFalha na reconexão. Aguardando nova configuração via BLE.");
        attemptedReconnect = true; // evita novas tentativas até que BLE configure novamente
      }
    }

    static unsigned long lastBleAdvertise = 0;
    unsigned long currentMillis = millis();

    // Reinicia a publicidade BLE se ela parar (a cada 60 segundos)
    if (currentMillis - lastBleAdvertise >= 60000) {
      BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
     
      Serial.println("BLE não está anunciando ou tempo limite atingido. Reiniciando anúncio...");
      pAdvertising->start();
      lastBleAdvertise = currentMillis;
    }

    delay(10000); // Espera antes de tentar novamente ou verificar BLE
    return; // Não prossegue com o envio de dados se não houver conexão WiFi
  }

  // Lógica para enviar dados periodicamente
  unsigned long now = millis();
  if (now - lastRequestTime >= interval) {
    sendTrackerData();
    lastRequestTime = now;
  }

  // Lógica para enviar dados imediatamente após a configuração BLE bem-sucedida
  if (shouldSendData && WiFi.status() == WL_CONNECTED) {
    sendTrackerData();
    lastRequestTime = millis();
    shouldSendData = false; // Reseta a flag para evitar envios múltiplos
  }
}
