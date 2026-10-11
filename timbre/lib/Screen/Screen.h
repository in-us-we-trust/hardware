#pragma once
#include <Arduino.h>

namespace Screen
{
    void begin();
    void handleClient();
    void stop();
    // Linea de ESTADO (efimera): progreso y eventos locales. Cada llamada
    // reemplaza la anterior. No toca el resultado.
    void showStatus(const String &text);

    // RESULTADO de un analisis (IA del backend o tiny model / microfono local).
    // Tarjeta fija arriba del video:
    //   - un resultado de la IA (esIA=true) SIEMPRE tiene prioridad sobre uno
    //     local: lo reemplaza, y un local nunca pisa a uno de la IA.
    //   - entre resultados del mismo origen, solo cambia si la severidad es
    //     igual o mayor (gana el peor del evento).
    // Severidades: NORMAL < LLAMATIVO < SOSPECHOSO < PELIGROSO.
    // source: de donde sale, p.ej. "IA - Imagen" o "Local - Camara".
    void showResult(const String &text, const String &severity, const String &source, bool esIA);

    // Limpia estado y tarjeta (fin del evento).
    void clear();

    // Empuja el nivel de audio (0..32767) a la pagina para la animacion.
    void pushLevel(uint16_t level);

    // Registra el callback de hangup. EventFlow lo llama con terminar().
    // El endpoint /hangup se agrega al server de Screen (:80).
    void onHangup(void (*callback)());
}
