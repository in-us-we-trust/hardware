#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// EventFlow: POLITICA + ciclo de vida. Encapsula a BackendSession (el main no
// lo toca). Maneja el evento del timbre como una MAQUINA DE ESTADOS no
// bloqueante:
//
//   IDLE ──onTimbre()──► CONECTANDO ──(MQTT listo)──► EN_SESION ──cerrar──► IDLE
//                            │
//                            └─(timeout)─► IDLE
//
// Variante A: al tocar el timbre se dispara la conexion MQTT Y se saca la foto
// EN PARALELO (mientras la red se establece, el micro ya tiene la foto lista, y
// mas adelante podra analizarla localmente). Cuando el transporte esta listo,
// se manda el press con la foto.
// ---------------------------------------------------------------------------

namespace EventFlow
{
    // Configura la capa. Adentro hace BackendSession::initialize.
    void initialize(const String &deviceId);

    // Se llama al tocar el timbre: arranca conexion + saca la foto (no bloquea).
    void onTimbre();

    // Cierra el flujo/sesion en curso.
    void terminar();

    // Bombeo: avanza la maquina de estados y el transporte. Llamar en loop().
    void loop();
}
