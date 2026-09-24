#include "BleManager.h"
#include "WebPortal.h"
#include "WifiManager.h"
#include "Device.h"
#include "Storage.h"
#include "ApiClient.h"
#include "CameraSource.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <WiFi.h>

#define SERVICE_UUID "12345678-1234-1234-1234-123456789000"
#define CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789001"
#define CHARACTERISTIC_CONFIRM_UUID "12345678-1234-1234-1234-123456789002"

namespace
{
    BLECharacteristic *pCharacteristicWifi = nullptr;

    // "Buzon" de trabajo pendiente entre el callback BLE (BTC_TASK, poco
    // stack) y BleManager::loop() (tarea principal, stack grande). El
    // callback SOLO escribe estas variables; loop() es quien realmente
    // conecta WiFi y llama al gateway.
    volatile bool pendingWifiConnect = false;
    String pendingSsid;
    String pendingPassword;
    String bootTimestamp;
    bool gatewayReady = false;
    bool awaitingRegistrationConfirmation = false;
    unsigned long lastEventSentAt = 0;

    // -----------------------------------------------------------------------
    // Callback: maneja conexion/desconexion BLE
    // Si el cliente se desconecta (por error o cierre desde la app),
    // el ESP32 reinicia el advertising para quedar listo para reconectar.
    // -----------------------------------------------------------------------
    class ServerCallbacks : public BLEServerCallbacks
    {
        void onConnect(BLEServer *pServer) override
        {
            Serial.println("\nCliente BLE conectado");
        }

        void onDisconnect(BLEServer *pServer) override
        {
            Serial.println("\nCliente BLE desconectado. Reiniciando advertising...");
            delay(500);
            BLEDevice::getAdvertising()->start();
            Serial.println("Advertising reiniciado. Listo para reconectar.");
        }
    };

    // -----------------------------------------------------------------------
    // Callback: recibe {"status":"success"} o {"status":"failed"} desde la app
    // -----------------------------------------------------------------------
    class ConfirmCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCharacteristic) override
        {
            String json = pCharacteristic->getValue().c_str();
            if (json.length() == 0)
                return;

            Serial.println("\nConfirmacion recibida:");
            Serial.println(json);

            JsonDocument doc;
            if (deserializeJson(doc, json))
            {
                Serial.println("JSON invalido en confirmacion");
                return;
            }

            String status = doc["status"].as<String>();

            if (status == "success")
            {
                WebPortal::sendSSE("✅ Dispositivo vinculado!");
                Storage::saveDeviceRegistered(true);
                awaitingRegistrationConfirmation = false;
                gatewayReady = true;
                delay(500);
                BleManager::stop();
                WebPortal::stop();
            }
            else
            {
                WebPortal::sendSSE("❌ Registro fallido. Reiniciando...");
                delay(1000);
                ESP.restart(); // Vuelve a mostrar el QR desde cero
            }
        }
    };

    // -----------------------------------------------------------------------
    // Callback: recibe credenciales WiFi.
    // Nota: este callback corre en la tarea BTC_TASK de Bluedroid, que
    // tiene ~4KB de stack. NUNCA debe llamar a WiFiManager::connect ni a
    // ApiClient (TLS/mTLS) directamente aca -> revienta el stack (panic
    // "Stack canary watchpoint triggered (BTC_TASK)"). Por eso solo
    // parseamos el JSON y dejamos el trabajo pesado para BleManager::loop().
    // -----------------------------------------------------------------------
    class WifiCallback : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *pCharacteristic) override
        {
            String json = pCharacteristic->getValue().c_str();
            if (json.length() == 0)
                return;

            Serial.println("\nJSON recibido:");
            Serial.println(json);

            JsonDocument doc;
            if (deserializeJson(doc, json))
            {
                Serial.println("JSON invalido");
                return;
            }

            pendingSsid = doc["ssid"].as<String>();
            pendingPassword = doc["password"].as<String>();

            WebPortal::sendSSE("Credenciales recibidas, conectando...");

            Storage::saveWifiCredentials(pendingSsid, pendingPassword);

            // Señal para que loop() (tarea principal) haga el trabajo pesado.
            pendingWifiConnect = true;
        }
    };
}

namespace BleManager
{
    void begin(const String &bleName)
    {
        BLEDevice::init(bleName.c_str());
        BLEServer *serverBle = BLEDevice::createServer();
        serverBle->setCallbacks(new ServerCallbacks());
        BLEService *service = serverBle->createService(BLEUUID(SERVICE_UUID), 8); // 8 handles para 2 characteristics

        // Characteristic 1: recibe credenciales WiFi, notifica deviceId
        BLECharacteristic *wifiChar = service->createCharacteristic(
            CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_WRITE_NR |
                BLECharacteristic::PROPERTY_NOTIFY);
        wifiChar->setCallbacks(new WifiCallback());
        wifiChar->addDescriptor(new BLE2902());
        pCharacteristicWifi = wifiChar;

        // Characteristic 2: recibe confirmacion de registro desde la app
        BLECharacteristic *confirmChar = service->createCharacteristic(
            CHARACTERISTIC_CONFIRM_UUID,
            BLECharacteristic::PROPERTY_WRITE |
                BLECharacteristic::PROPERTY_WRITE_NR);
        confirmChar->setCallbacks(new ConfirmCallback());

        service->start();
        BLEDevice::getAdvertising()->start();
        Serial.println("\nBLE activo: " + bleName);
    }

    void stop()
    {
        Serial.println("\nDesactivando BLE...");
        BLEDevice::deinit(true);
        Serial.println("BLE desactivado");
    }

    void loop()
    {
        if (!pendingWifiConnect)
        {
            if (!gatewayReady)
            {
                return;
            }

            if (millis() - lastEventSentAt < 30000)
            {
                return;
            }

            lastEventSentAt = millis();
            String imageBase64 = Camera.captureBase64();
            if (imageBase64.isEmpty())
            {
                WebPortal::sendSSE(" No se pudo capturar imagen para events");
                return;
            }

            String apiResponse;
            bool sent = ApiClient::sendEvent("motion", imageBase64, &apiResponse);
            if (sent)
            {
                WebPortal::sendSSE("Event enviado al gateway");
            }
            else
            {
                WebPortal::sendSSE("Fallo el envio del event");
            }

            return;
        }

        // Consumimos el pedido primero para no reprocesarlo si algo tarda.
        pendingWifiConnect = false;
        String ssid = pendingSsid;
        String password = pendingPassword;

        WebPortal::sendSSE("Conectando a WiFi: " + ssid);

        if (WifiManager::connect(ssid, password))
        {
            String deviceId = Device::getDeviceId();
            String mac = WiFi.macAddress();
            String response = "{\"deviceId\":\"" + deviceId + "\",\"mac\":\"" + mac + "\"}";
            WebPortal::sendSSE("✅ Conectado! RED: " + ssid + " | Registrando en el gateway...");

            if (pCharacteristicWifi != nullptr)
            {
                pCharacteristicWifi->setValue(response.c_str());
                pCharacteristicWifi->notify();
            }

            String lastSeen = WifiManager::nowIso();
            if (bootTimestamp.isEmpty())
            {
                bootTimestamp = lastSeen;
            }

            bool deviceAlreadyRegistered = Storage::loadDeviceRegistered();
            bool registered = deviceAlreadyRegistered;
            String apiResponse;

            if (!deviceAlreadyRegistered)
            {
                registered = ApiClient::registerDevice(
                    Device::getBleName(),
                    lastSeen,
                    bootTimestamp,
                    "", -1, "", "",
                    &apiResponse);

                if (registered)
                {
                    Storage::saveDeviceRegistered(true);
                }
            }

            if (registered)
            {
                if (deviceAlreadyRegistered)
                {
                    gatewayReady = true;
                    lastEventSentAt = millis() - 30000;
                    WebPortal::sendSSE("✅ Dispositivo ya registrado. Enviando events...");
                }
                else
                {
                    awaitingRegistrationConfirmation = true;
                    WebPortal::sendSSE("✅ Dispositivo registrado en el gateway. Enviá confirmacion en nRF Connect al characteristic 12345678-1234-1234-1234-123456789002 con {\"status\":\"success\"}.");
                }
            }
            else
            {
                WebPortal::sendSSE("WiFi OK pero fallo el registro en el gateway.");
            }
        }
        else
        {
            WebPortal::sendSSE("❌ No se pudo conectar. Verificá las credenciales.");
            if (pCharacteristicWifi != nullptr)
            {
                pCharacteristicWifi->setValue("{\"error\":\"wifi_failed\"}");
                pCharacteristicWifi->notify();
            }
        }
    }
}