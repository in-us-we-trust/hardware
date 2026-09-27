#include "Screen.h"
#include "Config.h"
#include <WebServer.h>
#include <WiFi.h>

namespace
{
  WebServer server(Config::Http::PORT);
  bool started = false;

  WiFiClient sseClient;
  bool sseConnected = false;

  // --- Estado de la pantalla (para re-enviarlo si la pagina se reconecta) ---
  // status: linea efimera (progreso / eventos locales).
  // card:   resultado FIJO. La IA del backend siempre tiene prioridad sobre lo
  //         local; entre resultados del mismo origen gana el de severidad
  //         igual o mayor (el peor del evento).
  struct Resultado
  {
    String text;
    String sev;
    String src;
    bool esIA; // true: ia_response del backend. false: tiny model / microfono
  };

  String lastStatus = "";
  bool hayCard = false;
  Resultado card;

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
#viz{width:100%;height:48px;display:block;}
#statusline{font-size:18px;color:#a8aebd;padding:12px 16px;background:#171a21;
 border-radius:8px;min-height:18px;display:flex;align-items:center;gap:8px;}
#statusline .dot{width:10px;height:10px;border-radius:50%;background:#6a7080;
 flex:none;animation:pulse 1.2s ease-in-out infinite;}
@keyframes pulse{0%,100%{opacity:.3}50%{opacity:1}}
#card{padding:14px 16px;border-radius:10px;background:#1b1f28;
 border-left:6px solid #3a3f4b;font-size:17px;font-weight:600;white-space:pre-wrap;}
#card .t{font-size:12px;color:#8a90a0;font-weight:400;margin-bottom:4px;}
#card.sev-PELIGROSO{border-left-color:#e5484d;background:#3a1618;}
#card.sev-SOSPECHOSO{border-left-color:#f5a623;}
#card.sev-LLAMATIVO{border-left-color:#f2d600;}
#card.sev-NORMAL{border-left-color:#30a46c;}
#empty{color:#6a7080;padding:20px;}
</style></head><body>
<header>
  <span>Timbre <span id='status'></span></span>
  <button id='hangup'>Colgar</button>
</header>
<main>
  <div id='card'><div id='empty'>Sin analisis todavia</div></div>
  <img id='video' alt='stream'>
  <div id='novideo'>Sin video (timbre en reposo)</div>
  <canvas id='viz' height='48'></canvas>
  <div id='statusline'><span class='dot'></span><span id='statustext'>Timbre en reposo</span></div>
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

// --- Mensajes (SSE) ---
// card: resultado fijo arriba del video (lo decide el ESP32: gana el de mayor
// severidad del evento). status: lo que se esta haciendo, abajo, sin colores.
function setCard(o){
  var c=document.getElementById('card');
  c.className='sev-'+(o.sev||'NORMAL'); c.innerHTML='';
  var h=document.createElement('div'); h.className='t';
  h.textContent=new Date().toLocaleTimeString()+' \u00b7 '+(o.src||'')+' \u00b7 '+(o.sev||'NORMAL');
  var b=document.createElement('div'); b.textContent=o.text||'';
  c.appendChild(h); c.appendChild(b);
}
function setStatus(t){ document.getElementById('statustext').textContent=t; }
function limpiar(){
  setStatus('Timbre en reposo');
  var c=document.getElementById('card'); c.className='';
  c.innerHTML='<div id="empty">Sin analisis todavia</div>';
}
var es=new EventSource('/events');
es.onmessage=function(e){
  try{
    var o=JSON.parse(e.data);
    if(o.clear){ limpiar(); targetLevel=0; return; }
    if(typeof o.level!=='undefined'){ targetLevel=o.level; return; }
    if(o.kind==='status'){ setStatus(o.text||''); return; }
    if(o.kind==='card'){ setCard(o); return; }
  }catch(err){}
};

// --- Visualizador de amplitud (barras tipo ecualizador) ---
var viz=document.getElementById('viz');
var vctx=viz.getContext('2d');
var targetLevel=0;   // ultimo nivel recibido (0..32767)
var smooth=0;        // nivel suavizado para que no salte brusco
var NBARS=32;
var bars=new Array(NBARS).fill(0);
function resizeViz(){ viz.width=viz.clientWidth; }
resizeViz(); window.addEventListener('resize',resizeViz);
function drawViz(){
  // suavizado exponencial hacia el target
  smooth += (targetLevel - smooth) * 0.3;
  // decaimiento natural del target (si dejan de mandar, baja solo)
  targetLevel *= 0.9;
  // Noise gate: restamos el ruido de base del mic. Por debajo del piso, cero.
  var GATE=800;      // ruido de base a ignorar (subir si en silencio se mueve)
  var RANGE=4000;    // rango sobre el gate hasta el maximo (bajar = mas sensible)
  var gated=Math.max(0, smooth-GATE);
  var norm=Math.min(1, gated/RANGE);
  // desplazar barras (efecto scroll) y meter el nivel actual al frente
  bars.shift();
  bars.push(norm*(0.6+0.4*Math.random()));
  var w=viz.width, h=viz.height, bw=w/NBARS;
  vctx.clearRect(0,0,w,h);
  for(var i=0;i<NBARS;i++){
    var bh=Math.max(2, bars[i]*h);
    var g=Math.round(48+bars[i]*160);
    vctx.fillStyle='rgb('+Math.round(bars[i]*120)+','+g+',108)';
    vctx.fillRect(i*bw+1, (h-bh)/2, bw-2, bh);
  }
  requestAnimationFrame(drawViz);
}
drawViz();
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

  void sseSend(const String &json)
  {
    if (sseConnected && sseClient.connected())
      sseClient.print("data: " + json + "\n\n");
  }

  void pushStatus(const String &text)
  {
    sseSend("{\"kind\":\"status\",\"text\":\"" + escapeJson(text) + "\"}");
  }

  void pushResultado(const char *kind, const Resultado &r)
  {
    sseSend(String("{\"kind\":\"") + kind + "\",\"sev\":\"" + escapeJson(r.sev) +
            "\",\"src\":\"" + escapeJson(r.src) + "\",\"text\":\"" +
            escapeJson(r.text) + "\"}");
  }

  // NORMAL < LLAMATIVO < SOSPECHOSO < PELIGROSO (mismo orden que el backend).
  int rango(const String &sev)
  {
    if (sev == "PELIGROSO")
      return 3;
    if (sev == "SOSPECHOSO")
      return 2;
    if (sev == "LLAMATIVO")
      return 1;
    return 0;
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
    // La pagina pudo recargarse a mitad de evento: le re-mandamos todo.
    if (hayCard)
      pushResultado("card", card);
    if (lastStatus.length() > 0)
      pushStatus(lastStatus);
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

  void showStatus(const String &text)
  {
    lastStatus = text;
    Serial.printf("[SCREEN] estado | %s\n", text.c_str());
    pushStatus(text);
  }

  void showResult(const String &text, const String &severity, const String &source, bool esIA)
  {
    Resultado r{text, severity.length() ? severity : String("NORMAL"), source, esIA};

    // Tarjeta fija:
    //  - IA sobre local: siempre reemplaza (aunque sea de menor severidad).
    //  - local sobre IA: nunca.
    //  - mismo origen: solo si es igual o peor que la actual.
    bool fija;
    if (!hayCard)
      fija = true;
    else if (r.esIA != card.esIA)
      fija = r.esIA;
    else
      fija = rango(r.sev) >= rango(card.sev);
    if (fija)
    {
      card = r;
      hayCard = true;
      pushResultado("card", r);
    }
    Serial.printf("[SCREEN] resultado%s | %s | %s | %s\n", fija ? " (FIJO)" : "",
                  r.sev.c_str(), r.src.c_str(), r.text.c_str());
  }

  void clear()
  {
    lastStatus = "";
    hayCard = false;
    sseSend("{\"clear\":true}");
    Serial.println("[SCREEN] limpio mensajes");
  }

  void pushLevel(uint16_t level)
  {
    if (sseConnected && sseClient.connected())
    {
      sseClient.print("data: {\"level\":" + String(level) + "}\n\n");
    }
  }

  void onHangup(void (*callback)())
  {
    hangupCb = callback;
  }
}
