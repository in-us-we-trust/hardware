#include "MqttClient.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <MQTT.h>        // 256dpi/arduino-mqtt
#include <ArduinoJson.h> // bblanchon/ArduinoJson (ya es dep del proyecto)
#include <time.h>

// ---------------------------------------------------------------------------
// Config por build flags (con defaults). Se pueden pisar desde platformio.ini
// igual que WOKWI_SEED_*, sin tocar este archivo.
// ---------------------------------------------------------------------------
#ifndef MQTT_BUFFER_SIZE
// press/stream_end/capture son JSON chicos (<1KB). Si algun dia se manda la
// imagen base64 en el press, subir esto (una QVGA base64 ronda 15-35 KB).
#define MQTT_BUFFER_SIZE 2048
#endif

#ifndef MQTT_KEEPALIVE_SECONDS
#define MQTT_KEEPALIVE_SECONDS 30
#endif

#ifndef MQTT_RECONNECT_INTERVAL_MS
#define MQTT_RECONNECT_INTERVAL_MS 3000
#endif

namespace
{
    const char *TOPIC_PRESS = "home/doorbell/events/press";
    const char *TOPIC_STREAM_END = "home/doorbell/events/stream_end";
    const char *TOPIC_CAPTURE_IMAGE = "home/doorbell/events/capture_image";
    const char *TOPIC_CAPTURE_AUDIO = "home/doorbell/events/capture_audio";
    const char *TOPIC_IA_RESPONSE = "home/doorbell/events/ia_response";

    WiFiClient net;
    MQTTClient mqtt(MQTT_BUFFER_SIZE);

    String brokerHost;
    uint16_t brokerPort = 1883;
    String deviceId;
    String clientId;

    MqttClient::IaResponseHandler iaHandler = nullptr;

    bool started = false;
    unsigned long lastConnectAttempt = 0;

    // Epoch en milisegundos UTC, o 0 si el reloj todavia no esta sincronizado
    // por NTP (WifiManager::connect hace la sincro).
    int64_t nowEpochMillis()
    {
        time_t now = time(nullptr);
        if (now < 1600000000L) // ~2020-09; antes de eso el reloj no arranco
        {
            return 0;
        }
        return static_cast<int64_t>(now) * 1000LL;
    }

    void handleIaResponse(const String &payload)
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (err)
        {
            Serial.printf("[MQTT] ia_response JSON invalido: %s\n", err.c_str());
            return;
        }

        MqttClient::IaResponse r;
        r.trackId = doc["trackId"] | "";
        r.deviceId = doc["deviceId"] | "";
        r.chunkType = doc["chunkType"] | "";
        r.action = doc["action"] | "";
        r.severity = doc["severity"] | "";
        r.message = doc["message"] | "";

        // Filtrado por deviceId: ignorar instrucciones de otros timbres.
        // if (r.deviceId.length() > 0 && r.deviceId != deviceId)
        // {
        //     return;
        // }

        Serial.printf("[MQTT] ia_response track=%s chunk=%s action=%s sev=%s\n",
                      r.trackId.c_str(), r.chunkType.c_str(),
                      r.action.c_str(), r.severity.c_str());

        if (iaHandler)
        {
            iaHandler(r);
        }
    }

    // Callback de arduino-mqtt (firma simple: topic + payload).
    void onMessage(String &topic, String &payload)
    {
        if (topic == TOPIC_IA_RESPONSE)
        {
            handleIaResponse(payload);
        }
    }

    bool publishJson(const char *topic, JsonDocument &doc)
    {
        if (!mqtt.connected())
        {
            Serial.printf("[MQTT] publish a %s descartado: sin conexion\n", topic);
            return false;
        }

        String payload;
        serializeJson(doc, payload);

        // arduino-mqtt: publish(topic, payload, retained=false, qos=1)
        bool ok = mqtt.publish(topic, payload, false, 1);
        if (!ok)
        {
            Serial.printf("[MQTT] publish a %s fallo (err=%d)\n",
                          topic, mqtt.lastError());
        }
        return ok;
    }

    void connectNow()
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            return; // sin WiFi no hay nada que hacer
        }

        lastConnectAttempt = millis();
        Serial.printf("[MQTT] conectando a %s:%u como %s...\n",
                      brokerHost.c_str(), brokerPort, clientId.c_str());

        // Lab: broker plano sin auth. Para user/pass usar connect(id, user, pass).
        if (mqtt.connect(clientId.c_str()))
        {
            Serial.println("[MQTT] conectado");
            mqtt.subscribe(TOPIC_IA_RESPONSE, 1);
            Serial.printf("[MQTT] suscripto a %s (QoS 1)\n", TOPIC_IA_RESPONSE);
        }
        else
        {
            Serial.printf("[MQTT] conexion fallo (err=%d, ret=%d)\n",
                          mqtt.lastError(), mqtt.returnCode());
        }
    }
}

namespace MqttClient
{
    void begin(const String &host, uint16_t port, const String &id)
    {
        brokerHost = host;
        brokerPort = port;
        deviceId = id;
        clientId = "timbre-" + id; // unico por dispositivo; nunca "gateway-micro"

        mqtt.begin(brokerHost.c_str(), brokerPort, net);
        mqtt.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
        // cleanSession=true: no queremos cola persistente del lado del broker.
        mqtt.setOptions(MQTT_KEEPALIVE_SECONDS, true, 5000);
        mqtt.onMessage(onMessage);

        started = true;
        connectNow();
    }

    void onIaResponse(IaResponseHandler handler)
    {
        iaHandler = handler;
    }

    void loop()
    {
        if (!started)
        {
            return;
        }

        mqtt.loop();

        if (!mqtt.connected() &&
            (millis() - lastConnectAttempt) > MQTT_RECONNECT_INTERVAL_MS)
        {
            connectNow();
        }
    }

    bool connected()
    {
        return started && mqtt.connected();
    }

    bool publishPress(
        const String &eventId,
        const String &rtspUrl,
        const String &imageBase64,
        float batteryPercentage)
    {
        JsonDocument doc;
        doc["eventId"] = eventId; // == trackId de la sesion
        doc["deviceId"] = deviceId;
        doc["event"] = "press";
        doc["rtspUrl"] = rtspUrl;

        int64_t ts = nowEpochMillis();
        if (ts > 0)
        {
            doc["timestamp"] = ts;
        }
        if (imageBase64.length() > 0)
        {
            doc["image"] = imageBase64;
        }
        if (!isnan(batteryPercentage))
        {
            doc["batteryPercentage"] = batteryPercentage;
        }

        Serial.printf("[MQTT] -> press eventId=%s rtsp=%s\n",
                      eventId.c_str(), rtspUrl.c_str());
        return publishJson(TOPIC_PRESS, doc);
    }

    bool publishStreamEnd(const String &trackId)
    {
        JsonDocument doc;
        doc["trackId"] = trackId;
        doc["deviceId"] = deviceId;

        int64_t ts = nowEpochMillis();
        if (ts > 0)
        {
            doc["timestamp"] = ts;
        }

        Serial.printf("[MQTT] -> stream_end trackId=%s\n", trackId.c_str());
        return publishJson(TOPIC_STREAM_END, doc);
    }

    bool publishCaptureImage(const String &trackId, const String &rtspUrl)
    {
        JsonDocument doc;
        doc["deviceId"] = deviceId;
        if (trackId.length() > 0)
        {
            doc["trackId"] = trackId;
        }
        if (rtspUrl.length() > 0)
        {
            doc["rtspUrl"] = rtspUrl;
        }
        int64_t ts = nowEpochMillis();
        if (ts > 0)
        {
            doc["timestamp"] = ts;
        }
        return publishJson(TOPIC_CAPTURE_IMAGE, doc);
    }

    bool publishCaptureAudio(const String &trackId, const String &rtspUrl)
    {
        JsonDocument doc;
        doc["deviceId"] = deviceId;
        if (trackId.length() > 0)
        {
            doc["trackId"] = trackId;
        }
        if (rtspUrl.length() > 0)
        {
            doc["rtspUrl"] = rtspUrl;
        }
        int64_t ts = nowEpochMillis();
        if (ts > 0)
        {
            doc["timestamp"] = ts;
        }
        return publishJson(TOPIC_CAPTURE_AUDIO, doc);
    }
}
