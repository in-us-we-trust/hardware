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
*{box-sizing:border-box;}
body{font-family:system-ui,-apple-system,'Segoe UI',Arial;margin:0;
 background:#f4f2f8;color:#1c1b22;}
header{background:#fff;border-bottom:1px solid #e8e5ef;}
.bar{max-width:864px;margin:0 auto;padding:12px 16px;display:flex;
 justify-content:space-between;align-items:center;gap:12px;min-height:64px;}
.brand{display:flex;align-items:center;gap:10px;font-size:18px;font-weight:700;}
.logo{width:32px;height:32px;border-radius:8px;display:grid;place-items:center;
 background:linear-gradient(135deg,#6e56cf,#5a43b8);color:#fff;}
#hangup{display:none;align-items:center;gap:8px;background:#ef4444;color:#fff;
 border:0;border-radius:10px;padding:9px 18px;font-size:15px;font-weight:600;
 cursor:pointer;box-shadow:0 4px 14px rgba(239,68,68,.35);}
#hangup:hover{background:#dc2626;}
body.live #hangup{display:flex;}
main{max-width:864px;margin:0 auto;padding:28px 16px;display:flex;
 flex-direction:column;gap:16px;}
#banner{display:flex;align-items:center;gap:14px;padding:14px 20px;
 border-top:1px solid #e8e5ef;}
#banner .dot{width:12px;height:12px;border-radius:50%;background:#a19dab;flex:none;}
#banner .txt{flex:1;min-width:0;}
#btitle{font-size:16px;font-weight:600;}
#bsub{font-size:13px;color:#6f6b7a;margin-top:2px;}
#timer{display:none;font-size:13px;font-weight:700;font-variant-numeric:tabular-nums;}
body.live #banner .dot{animation:pulse 1.4s ease-in-out infinite;}
body.live #timer{display:block;}
@keyframes pulse{0%,100%{opacity:.45}50%{opacity:1}}
#panel{background:#fff;border:1px solid #e8e5ef;border-radius:18px;overflow:hidden;
 box-shadow:0 6px 24px rgba(40,30,80,.06);}
#stage{display:none;position:relative;background:#22212b;}
body.live #stage{display:block;}
#video{width:100%;display:block;aspect-ratio:4/3;object-fit:contain;}
.badge{position:absolute;top:14px;padding:5px 10px;border-radius:8px;
 background:rgba(10,10,14,.85);color:#fff;font-size:12px;font-weight:600;
 display:flex;align-items:center;gap:6px;}
.badge.l{left:14px;letter-spacing:.06em;}
.badge.r{right:14px;}
.badge .rec{width:7px;height:7px;border-radius:50%;background:#ef4444;
 animation:pulse 1.2s ease-in-out infinite;}
#viz{position:absolute;left:0;right:0;bottom:0;width:100%;height:48px;
 display:block;pointer-events:none;}
#empty{display:flex;flex-direction:column;align-items:center;justify-content:center;
 text-align:center;padding:24px;min-height:min(466px,70vw);}
body.live #empty{display:none;}
.eicon{width:84px;height:84px;border-radius:20px;background:#ede8f7;
 color:#6e56cf;display:grid;place-items:center;margin-bottom:18px;}
#empty h2{margin:0;font-size:18px;font-weight:700;}
#empty p{margin:6px 0 0;font-size:14px;color:#6f6b7a;}
#result{display:none;align-items:center;gap:14px;padding:14px 20px;
 border-radius:14px;background:#fff;border:1px solid #e8e5ef;}
#result.on{display:flex;}
/* Color de la tarjeta segun severidad: verde / amarillo / rojo */
#result.p-NORMAL{background:#dff1e8;border-color:#b9e2cc;color:#0b5b37;}
#result.p-LLAMATIVO,#result.p-SOSPECHOSO{background:#fdf4c7;border-color:#f3e08f;color:#713f12;}
#result.p-PELIGROSO{background:#fde2e2;border-color:#f6bcbc;color:#7f1d1d;}
.ricon{width:42px;height:42px;border-radius:10px;background:rgba(255,255,255,.6);
 color:inherit;display:grid;place-items:center;flex:none;}
#result .txt{flex:1;min-width:0;}
#rtext{font-size:17px;font-weight:600;white-space:pre-wrap;overflow-wrap:anywhere;}
#rmeta{font-size:13px;opacity:.75;margin-top:2px;}
#sev{flex:none;display:flex;align-items:center;gap:6px;padding:5px 12px;
 border-radius:999px;font-size:12px;font-weight:700;letter-spacing:.06em;
 background:rgba(255,255,255,.6);}
#sev::before{content:'';width:6px;height:6px;border-radius:50%;background:currentColor;}
.sev-NORMAL{color:#15803d;}
.sev-LLAMATIVO,.sev-SOSPECHOSO{color:#a16207;}
.sev-PELIGROSO{color:#dc2626;}
@media (max-width:520px){
 #result{flex-wrap:wrap;} #sev{margin-left:56px;}
}
</style></head><body>
<header><div class='bar'>
  <div class='brand'>
    <span class='logo'><svg width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.2' stroke-linecap='round'><rect x='6' y='2' width='12' height='20' rx='3'/><circle cx='12' cy='14' r='1.2' fill='currentColor'/><path d='M12 7v2'/></svg></span>
    Timbre
  </div>
  <button id='hangup'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M10.68 13.31a16 16 0 0 0 3.41 2.6l1.27-1.27a2 2 0 0 1 2.11-.45 12.84 12.84 0 0 0 2.81.7 2 2 0 0 1 1.72 2v3a2 2 0 0 1-2.18 2 19.79 19.79 0 0 1-8.63-3.07 19.42 19.42 0 0 1-3.33-2.67m-2.67-3.34a19.79 19.79 0 0 1-3.07-8.63A2 2 0 0 1 4.11 2h3a2 2 0 0 1 2 1.72 12.84 12.84 0 0 0 .7 2.81 2 2 0 0 1-.45 2.11L8.09 9.91'/><path d='M22 2 2 22'/></svg>Colgar</button>
</div></header>
<main>
  <div id='result'>
    <span class='ricon'><svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='8' r='4'/><path d='M4 21a8 8 0 0 1 16 0'/></svg></span>
    <div class='txt'><div id='rtext'></div><div id='rmeta'></div></div>
    <span id='sev'></span>
  </div>
  <div id='panel'>
    <div id='stage'>
      <img id='video' alt=''>
      <span class='badge l'><span class='rec'></span>EN VIVO</span>
      <span class='badge r'>C&aacute;mara &middot; Local</span>
      <canvas id='viz' height='48'></canvas>
    </div>
    <div id='empty'>
      <div class='eicon'><svg width='40' height='40' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'><path d='M10.66 6H14a2 2 0 0 1 2 2v2.5l5.25-3.06A.5.5 0 0 1 22 7.87v8.26'/><path d='M16 16a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h2'/><path d='m2 2 20 20'/></svg></div>
      <h2>Sin video</h2>
      <p>La c&aacute;mara est&aacute; en reposo. Todav&iacute;a no hay an&aacute;lisis.</p>
    </div>
    <div id='banner'>
      <span class='dot'></span>
      <div class='txt'><div id='btitle'>Timbre en reposo</div><div id='bsub'></div></div>
      <span id='timer'>00:00</span>
    </div>
  </div>
</main>
<script>
var host = location.hostname;
var img = document.getElementById('video');
var live = false;
var statusText = '';
var liveSince = 0;

function $(id){ return document.getElementById(id); }

// Banner de arriba: el titulo depende de si hay video; el subtitulo es la
// linea de estado efimera que manda el ESP32 (o un texto por defecto).
function renderBanner(){
  $('btitle').textContent = live ? 'Transmitiendo video' : 'Timbre en reposo';
  $('bsub').textContent = statusText || (live
    ? 'An\u00e1lisis en vivo en curso\u2026'
    : 'Se activar\u00e1 cuando alguien toque el timbre o se detecte movimiento.');
}
function setLive(on){
  if(on === live) return;
  live = on;
  liveSince = Date.now();
  document.body.classList.toggle('live', on);
  if(on) resizeViz();
  renderBanner(); tick();
}
function tick(){
  var s = live ? Math.floor((Date.now() - liveSince) / 1000) : 0;
  var mm = Math.floor(s / 60), ss = s % 60;
  $('timer').textContent = (mm < 10 ? '0' : '') + mm + ':' + (ss < 10 ? '0' : '') + ss;
}
setInterval(tick, 1000);
renderBanner();

// --- Video: MJPEG continuo desde :8081 ---
var streamUrl = 'http://' + host + ':8081/';
img.src = streamUrl;
img.onload = function(){ setLive(true); };
img.onerror = function(){ setLive(false); };

// Retry: si el stream se corto (hangup) y vuelve (nuevo timbre), reconectar.
setInterval(function(){
  if(!live){
    img.src = '';
    img.src = streamUrl + '?t=' + Date.now();
  }
}, 2000);

// --- Colgar: /hangup esta en :80 (mismo origen, sin CORS) ---
$('hangup').onclick = function(){
  fetch('/hangup').then(function(){ setLive(false); }).catch(function(){});
};

// --- Mensajes (SSE) ---
// card: resultado fijo arriba del video, coloreado por severidad (lo decide el
// ESP32: gana el de mayor severidad del evento). status: lo que se esta
// haciendo, en el banner neutro debajo del video.
function setCard(o){
  var sev = o.sev || 'NORMAL';
  var r = $('result');
  r.className = 'on p-' + sev;
  $('rtext').textContent = o.text || '';
  $('rmeta').textContent = new Date().toLocaleTimeString() +
    (o.src ? ' \u00b7 ' + o.src.replace(/\s*-\s*/g, ' \u00b7 ') : '');
  var b = $('sev'); b.className = 'sev-' + sev; b.textContent = sev;
}
function setStatus(t){ statusText = t; renderBanner(); }
function limpiar(){
  setStatus('');
  $('result').className = '';
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
function resizeViz(){ if(viz.clientWidth) viz.width=viz.clientWidth; }
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
    vctx.fillStyle='rgba(167,146,240,'+(0.25+bars[i]*0.6).toFixed(2)+')';
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
