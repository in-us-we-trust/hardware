#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789000"
#define CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789001"

Preferences prefs;
WebServer server(80);

bool wifiConfigured = false;

String bleNameGlobal;

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

String htmlPage()//No funciona :c
{
    String jsonData = "{\"ble_name\":\"" + bleNameGlobal + "\"}";

    String page =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<title>Timbre Setup</title>"
"<script src='https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js'></script>"
"<style>"
"body{font-family:Arial;text-align:center;margin-top:40px;}"
"#qrcode{margin:auto;}"
"</style>"
"</head><body>"

"<h2>Configuracion Timbre</h2>"
"<div id='qrcode'></div>"
"<p id='ble'></p>"
"<p id='json'></p>"

"<script>"

"const data = '" + jsonData + "';"

"document.getElementById('ble').innerText = 'BLE: ' + JSON.parse(data).ble_name;"
"document.getElementById('json').innerText = data;"

"new QRCode(document.getElementById('qrcode'), {"
"text: data,"
"width: 256,"
"height: 256"
"});"

"</script>"

"</body></html>";

    return page;
}

void handleRoot()
{
    server.send(200, "text/html", htmlPage());
}

void startConfigPortal()
{
    WiFi.mode(WIFI_AP);

    WiFi.softAP(bleNameGlobal.c_str());

    Serial.println("\nAccess Point iniciado");
    Serial.print("SSID: ");
    Serial.println(bleNameGlobal);

    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", handleRoot);
    server.begin();

    Serial.println("Servidor web iniciado");
}

void startBle()
{
    BLEDevice::init(bleNameGlobal.c_str());

    BLEServer *serverBle = BLEDevice::createServer();

    BLEService *service = serverBle->createService(SERVICE_UUID);

    BLECharacteristic *characteristic =
        service->createCharacteristic(
            CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_WRITE
        );

    characteristic->setCallbacks(new WifiCallback());

    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->start();

    Serial.println("\nBLE activo: " + bleNameGlobal);
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

    bleNameGlobal = getBleName();

    if (ssid.length() > 0)
    {
        Serial.println("Credenciales encontradas");

        if (connectToWifi(ssid, password))
        {
            Serial.println("\nModo normal (sin BLE ni AP)");
            return;
        }
    }

    Serial.println("\nEntrando en modo configuracion");

    startConfigPortal();
    startBle();
}

void loop()
{
    server.handleClient();
}