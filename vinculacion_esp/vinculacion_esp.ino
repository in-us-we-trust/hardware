#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789000"
#define CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789001"

Preferences prefs;

bool wifiConfigured = false;

String getBleName()
{
    uint64_t chipid = ESP.getEfuseMac();
    char buffer[13];

    sprintf(buffer, "%012llX", chipid);

    String fullId(buffer);
    String suffix = fullId.substring(6);

    return "TIMBRE-" + suffix;
}

void stopBle()
{
    Serial.println("\nDesactivando BLE...");

    BLEDevice::deinit(true);

    Serial.println("BLE desactivado");
}

bool connectToWifi(String ssid, String password)
{
    Serial.println("\nConectando WiFi...");
    Serial.println(ssid);

    WiFi.begin(ssid.c_str(), password.c_str());

    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
        delay(1000);
        Serial.print(".");
        attempts++;
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi conectado");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        
        return true;
    }

    Serial.println("No se pudo conectar");
    
    return false;
}

class WifiCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic) override
    {
        String json = pCharacteristic->getValue();

        if (json.length() == 0) return;

        Serial.println("\nJSON recibido:");
        Serial.println(json);

        JsonDocument doc;

        DeserializationError error = deserializeJson(doc, json);

        if (error)
        {
            Serial.println("JSON invalido");
            return;
        }

        String ssid = doc["ssid"].as<String>();
        String password = doc["password"].as<String>();

        Serial.println("\nGuardando credenciales...");

        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        prefs.end();

        Serial.println("Credenciales guardadas");

        bool connected = connectToWifi(ssid, password);

        if (connected)
        {
            stopBle();
        }
    }
};

void startBle()
{
    String bleName = getBleName();

    BLEDevice::init(bleName.c_str());

    BLEServer *server = BLEDevice::createServer();
    BLEService *service = server->createService(SERVICE_UUID);
    
    BLECharacteristic *characteristic = service->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );

    characteristic->setCallbacks(new WifiCallback());

    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->start();

    Serial.print("\nBLE activo: ");
    Serial.println(bleName);
}

void setup()
{
    Serial.begin(115200);

    delay(2000);

    Serial.println("\nIniciando...");

    prefs.begin("wifi", true);

    String ssid = prefs.getString("ssid", "");
    String password = prefs.getString("password", "");

    prefs.end();

    if (ssid.length() > 0)
    {
        Serial.println("Credenciales encontradas");

        bool connected = connectToWifi(ssid, password);

        if (connected)
        {
            Serial.println("\nModo normal");
            Serial.println("BLE deshabilitado");
            
            return;
        }
    }

    Serial.println("\nEntrando en modo configuracion");

    startBle();
}

void loop()
{
}