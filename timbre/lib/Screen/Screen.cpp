#include "Screen.h"
#include <WebServer.h>
#include <WiFi.h>

namespace
{
    WebServer server(80);
    bool started = false;

    // SSE: un solo cliente (la pagina abierta), igual que WebPortal.
    WiFiClient sseClient;
    bool sseConnected = false;

    // Ultimo mensaje, para mostrarlo apenas se abre/recarga la pagina.
    String lastText = "";
    String lastSev = "NORMAL";

    // Pagina de operacion: lista de mensajes que se van agregando por SSE.
    const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Timbre - Mensajes</title>
<style>
body{font-family:system-ui,Arial;margin:0;background:#0f1115;color:#e6e6e6;}
header{padding:16px 20px;background:#171a21;font-size:18px;font-weight:600;
 border-bottom:1px solid #262b36;}
#log{padding:16px 20px;display:flex;flex-direction:column;gap:8px;}
.msg{padding:10px 14px;border-radius:8px;background:#171a21;
 border-left:4px solid #3a3f4b;white-space:pre-wrap;}
.msg .t{font-size:12px;color:#8a90a0;margin-bottom:2px;}
.sev-HIGH{border-left-color:#e5484d;}
.sev-MEDIUM{border-left-color:#f5a623;}
.sev-LOW{border-left-color:#f2d600;}
.sev-NORMAL{border-left-color:#30a46c;}
#empty{color:#6a7080;padding:20px;}
</style></head><body>
<header>Timbre &middot; Mensajes en vivo</header>
<div id='log'><div id='empty'>Esperando mensajes del gateway...</div></div>
<script>
function add(sev,text){
  var e=document.getElementById('empty'); if(e) e.remove();
  var log=document.getElementById('log');
  var d=document.createElement('div'); d.className='msg sev-'+sev;
  var t=new Date().toLocaleTimeString();
  var head=document.createElement('div'); head.className='t';
  head.textContent=t+' \u00b7 '+sev;
  var body=document.createElement('div'); body.textContent=text;
  d.appendChild(head); d.appendChild(body);
  log.insertBefore(d, log.firstChild);
}
var es=new EventSource('/events');
es.onmessage=function(e){
  try{var o=JSON.parse(e.data); add(o.sev||'NORMAL', o.text||'');}catch(err){}
};
</script>
</body></html>
)HTML";

    String escapeJson(const String &s)
    {
        String o;
        for (size_t i = 0; i < s.length(); i++)
        {
            char c = s[i];
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') { o += "\\n"; }
            else if (c == '\r') { /* drop */ }
            else o += c;
        }
        return o;
    }

    // Empuja un evento SSE al cliente conectado (si hay).
    void pushSse(const String &sev, const String &text)
    {
        if (sseConnected && sseClient.connected())
        {
            sseClient.print("data: {\"sev\":\"" + sev + "\",\"text\":\"" +
                            escapeJson(text) + "\"}\n\n");
        }
    }

    void handleRoot()
    {
        server.send_P(200, "text/html", PAGE);
    }

    void handleEvents()
    {
        sseClient = server.client();
        sseConnected = true;
        server.sendContent(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n");
        // Al abrir/recargar, mandamos el ultimo mensaje conocido.
        if (lastText.length() > 0) pushSse(lastSev, lastText);
    }
}

namespace Screen
{
    void begin()
    {
        server.on("/", handleRoot);
        server.on("/events", handleEvents);
        server.begin();
        started = true;
        Serial.printf("[SCREEN] server web en http://%s/\n",
                      WiFi.localIP().toString().c_str());
    }

    void handleClient()
    {
        if (started) server.handleClient();
    }

    void stop()
    {
        if (!started) return;
        server.stop();
        started = false;
        sseConnected = false;
    }

    void showMessage(const String &text, const String &severity)
    {
        lastText = text;
        lastSev = severity.length() ? severity : "NORMAL";
        Serial.printf("[SCREEN] %s | %s\n", lastSev.c_str(), text.c_str());
        pushSse(lastSev, text);
    }
}
