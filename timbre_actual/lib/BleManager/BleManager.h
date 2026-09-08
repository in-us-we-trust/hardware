#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// BleManager: servidor BLE con 2 characteristics:
//  - CHARACTERISTIC_UUID: recibe credenciales WiFi, notifica deviceId+mac
//  - CHARACTERISTIC_CONFIRM_UUID: recibe confirmacion de registro de la app
// ---------------------------------------------------------------------------
namespace BleManager
{
    // Inicializa BLEDevice, crea el servicio y arranca advertising.
    void begin(const String &bleName);

    // Apaga el stack BLE (deinit).
    void stop();

    // Debe llamarse en cada loop() del programa principal.
    // Los callbacks de escritura BLE (onWrite) corren en la tarea interna
    // BTC_TASK del stack Bluedroid, que tiene muy poco stack (~4KB) y NO
    // alcanza para un handshake TLS/mTLS. Por eso el callback solo guarda
    // las credenciales recibidas, y es este loop() -corriendo en la tarea
    // principal, con stack grande- el que efectivamente conecta el WiFi y
    // llama al gateway.
    void loop();
}