#pragma once

// ---------------------------------------------------------------------------
// ModoOperacion: el timbre funcionando (ya vinculado).
//
// Compone los singletons de operacion — EventFlow (coordinador del evento:
// backend + stream local) y Screen (pantalla web) — y las entradas que
// disparan el timbrado (boton fisico + serial de prueba).
//
// Adentro, el estado Reposo <-> TimbreActivo lo maneja EventFlow; este modo
// solo lo arranca y le bombea el loop.
// ---------------------------------------------------------------------------
namespace ModoOperacion
{
    // Deriva el deviceId, engancha el boton, levanta Screen + EventFlow y deja
    // el dispositivo en reposo (bajo consumo). Requiere WiFi ya conectado.
    void begin();

    // Una vuelta: bombea EventFlow + Screen y despacha boton/serial.
    void loop();
}
