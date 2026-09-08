#include "CallManager.h"
#include "CameraManager.h"
#include <esp_camera.h>
#include <WebServer.h>
#include <WiFi.h>

namespace
{
    WebServer *server = nullptr;
    volatile bool active = false;
    volatile bool remoteHangupRequested = false;
}

namespace CallManager
{
    void beginCall()
    {
        if (active)
            return;

        Serial.println("Iniciando llamada: activando camara y servidor de streaming...");
        setCpuFrequencyMhz(240);
        WiFi.setSleep(false);
        CameraManager::begin();
        remoteHangupRequested = false;

        server = new WebServer(8081);

        server->on("/", []() {
            server->sendHeader("Access-Control-Allow-Origin", "*");
            server->send(200, "text/html", "<html><body><h2>CallManager</h2><p>Stream: <a href='/stream'>/stream</a></p><p><a href='/hangup'>Hangup</a></p></body></html>");
        });

        server->on("/hangup", []() {
            server->sendHeader("Access-Control-Allow-Origin", "*");
            server->send(200, "text/plain", "OK");
            active = false;
            remoteHangupRequested = true;
        });

        server->on("/stream", []() {
            WiFiClient client = server->client();
            if (!client)
                return;

            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb)
            {
                server->send(503, "text/plain", "No frame available");
                return;
            }

            client.println("HTTP/1.1 200 OK");
            client.println("Content-Type: image/jpeg");
            client.println("Access-Control-Allow-Origin: *");
            client.println("Cache-Control: no-cache, no-store, max-age=0");
            client.println("Pragma: no-cache");
            client.println("Connection: close");
            client.print("Content-Length: ");
            client.println(String(fb->len));
            client.println();
            client.write(fb->buf, fb->len);
            client.stop();

            esp_camera_fb_return(fb);
        });

        server->begin();
        active = true;
        Serial.println("Servidor de streaming iniciado en puerto 8081");
    }

    void endCall()
    {
        if (!active && server == nullptr)
            return;

        Serial.println("Terminando llamada...");
        active = false;
        if (server)
        {
            server->stop();
            delete server;
            server = nullptr;
        }
        CameraManager::end();
        WiFi.setSleep(true);
        setCpuFrequencyMhz(80);
        remoteHangupRequested = false;
        Serial.println("Llamada terminada por la PC. ESP32 en reposo.");
    }

    void handle()
    {
        if (server)
        {
            server->handleClient();
        }

        if (remoteHangupRequested)
        {
            remoteHangupRequested = false;
            endCall();
        }
    }

    bool inCall()
    {
        return active;
    }

    bool isHangupRequested()
    {
        return remoteHangupRequested;
    }
}
