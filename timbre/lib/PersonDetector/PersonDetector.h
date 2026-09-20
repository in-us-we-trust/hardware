#pragma once

#include <Arduino.h>

// Clasificador aislado: recibe un JPEG completo y devuelve si el modelo
// person_detection considera que contiene una persona.
//
// El modulo no captura imagenes, no inicia llamadas y no hace requests.
namespace PersonDetector
{
    // Inicializa TFLite Micro. Es opcional llamarlo de forma explicita:
    // detectJpeg() lo invoca automaticamente la primera vez.
    bool begin();

    // Recorta el centro de la imagen a un cuadrado, lo escala a 96x96,
    // convierte a grayscale/int8, ejecuta el modelo y compara person_score
    // contra el umbral fijo 0.40.
    //
    // Cualquier error se informa por Serial y devuelve false.
    bool detectJpeg(const uint8_t *jpegData, size_t jpegLength);

    bool isReady();
}
