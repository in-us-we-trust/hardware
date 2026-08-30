#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Screen: "pantalla" de operacion del timbre, expuesta por un server web en
// modo normal (ya conectado a la red de casa). Muestra en vivo los mensajes
// que van llegando (ia_response del gateway), empujandolos por SSE al browser.
//
// Es la contraparte de WebPortal: WebPortal = configuracion (AP + QR); Screen =
// operacion (red normal + mensajes). Mas adelante se puede unificar para que
// Screen tambien sirva el QR.
//
// Como todavia no hay display fisico en la placa, "la pantalla" es esta pagina
// web: entras con el browser a http://<ip-del-timbre>/ y ves los mensajes.
// ---------------------------------------------------------------------------

namespace Screen
{
    // Levanta el server web (puerto 80). Llamar en modo normal, con WiFi ya
    // conectado (usa la IP de la STA).
    void begin();

    // Atiende clientes HTTP. Llamar en cada vuelta del loop() principal.
    void handleClient();

    // Apaga el server (libera el puerto).
    void stop();

    // Muestra un mensaje nuevo: lo guarda como "ultimo" y lo empuja por SSE a
    // la pagina abierta. severity ("HIGH"/"MEDIUM"/"LOW"/"NORMAL") colorea.
    void showMessage(const String &text, const String &severity = "NORMAL");
}
