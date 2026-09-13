#include "Screen.h"
#include <WebServer.h>
#include <WiFi.h>

namespace
{
    WebServer server(80);
    bool started = false;

    WiFiClient sseClient;
    bool sseConnected = false;

    String lastText = "";
    String lastSev = "NORMAL";

    void (*hangupCb)() = nullptr;

    // Pagina UNIFICADA: stream MJPEG + mensajes SSE + boton colgar.
    // El stream viene de :8081 (MJPEG continuo, core 0). El /hangup esta en
    // este mismo server (:80, mismo origen, sin CORS).
    const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Timbre</title>
<style>
body{font-family:system-ui,Arial;margin:0;background:#0f1115;color:#e6e6e6;}
header{padding:16px 20px;background:#171a21;font-size:18px;font-weight:600;
 border-bottom:1px solid #262b36;display:flex;justify-content:space-between;
 align-items:center;gap:12px;}
#status{font-size:13px;color:#8a90a0;font-weight:400;}
#hangup{background:#e5484d;color:#fff;border:0;border-radius:8px;
 padding:8px 16px;font-size:14px;font-weight:600;cursor:pointer;}
#hangup:hover{background:#c93b40;}
main{padding:16px 20px;display:flex;flex-direction:column;gap:16px;
 max-width:720px;margin:0 auto;}
#video{width:100%;border-radius:10px;background:#000;display:block;
 aspect-ratio:4/3;object-fit:contain;}
#novideo{color:#6a7080;text-align:center;padding:40px;background:#171a21;
 border-radius:10px;display:none;}
#log{display:flex;flex-direction:column;gap:8px;}
.msg{padding:10px 14px;border-radius:8px;background:#171a21;
 border-left:4px solid #3a3f4b;white-space:pre-wrap;}
.msg .t{font-size:12px;color:#8a90a0;margin-bottom:2px;}
.sev-HIGH{border-left-color:#e5484d;}
.sev-MEDIUM{border-left-color:#f5a623;}
.sev-LOW{border-left-color:#f2d600;}
.sev-NORMAL{border-left-color:#30a46c;}
#empty{color:#6a7080;padding:20px;}
</style></head><body>
<header>
  <span>Timbre <span id='status'></span></span>
  <button id='hangup'>Colgar</button>
</header>
<main>
  <img id='video' alt='stream'>
  <div id='novideo'>Sin video (timbre en reposo)</div>
  <div id='log'><div id='empty'>Esperando mensajes del gateway...</div></div>
</main>
<script>
var host = location.hostname;
var img = document.getElementById('video');
var novideo = document.getElementById('novideo');

// --- Video: MJPEG continuo desde :8081 ---
var streamUrl = 'http://' + host + ':8081/';
img.src = streamUrl;
img.onload = function(){ img.style.display='block'; novideo.style.display='none'; };
img.onerror = function(){ img.style.display='none'; novideo.style.display='block'; };

// Retry: si el stream se corto (hangup) y vuelve (nuevo timbre), reconectar.
setInterval(function(){
  if(img.style.display==='none'){
    img.src = '';
    img.src = streamUrl + '?t=' + Date.now();
  }
}, 2000);

// --- Colgar: /hangup esta en :80 (mismo origen, sin CORS) ---
document.getElementById('hangup').onclick = function(){
  fetch('/hangup').then(function(){
    img.style.display='none'; novideo.style.display='block';
  }).catch(function(){});
};

// --- Mensajes del backend (SSE) ---
function add(sev,text){
  var log=document.getElementById('log');
  var t=new Date().toLocaleTimeString();
  log.innerHTML='';
  var d=document.createElement('div'); d.className='msg sev-'+sev;
  var head=document.createElement('div'); head.className='t';
  head.textContent=t+' \u00b7 '+sev;
  var body=document.createElement('div'); body.textContent=text;
  d.appendChild(head); d.appendChild(body);
  log.appendChild(d);
}
var es=new EventSource('/events');
es.onmessage=function(e){
  try{
    var o=JSON.parse(e.data);
    if(o.clear){
      document.getElementById('log').innerHTML=
        '<div id="empty">Esperando mensajes del gateway...</div>';
      return;
    }
    add(o.sev||'NORMAL', o.text||'');
  }catch(err){}
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
            if (c == '"' || c == '\\')
            {
                o += '\\';
                o += c;
            }
            else if (c == '\n')
            {
                o += "\\n";
            }
            else if (c == '\r')
            { /* drop */
            }
            else
                o += c;
        }
        return o;
    }

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
        if (lastText.length() > 0)
            pushSse(lastSev, lastText);
    }

    void handleHangup()
    {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/plain", "OK");
        if (hangupCb)
            hangupCb();
    }
}

namespace Screen
{
    void begin()
    {
        server.on("/", handleRoot);
        server.on("/events", handleEvents);
        server.on("/hangup", handleHangup);
        server.begin();
        started = true;
        Serial.printf("[SCREEN] server web en http://%s/\n",
                      WiFi.localIP().toString().c_str());
    }

    void handleClient()
    {
        if (started)
            server.handleClient();
    }

    void stop()
    {
        if (!started)
            return;
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

    void clear()
    {
        lastText = "";
        lastSev = "NORMAL";
        if (sseConnected && sseClient.connected())
        {
            sseClient.print("data: {\"clear\":true}\n\n");
        }
        Serial.println("[SCREEN] limpio mensajes");
    }

    void onHangup(void (*callback)())
    {
        hangupCb = callback;
    }
}
