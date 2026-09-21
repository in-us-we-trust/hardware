#pragma once

// ---------------------------------------------------------------------------
// MqttCerts: certificados y clave privada del dispositivo para el handshake
// mTLS contra el broker MQTT. El contenido real (los PEM) vive en MqttCerts.cpp,
// que se genera a partir de lib/MqttClient/certs/*.pem y NO se edita a mano.
//
// Mismo patron que Certs.h / Certs.cpp.
// ---------------------------------------------------------------------------
extern const char MQTT_CA_CERT[];
extern const char MQTT_DEVICE_CERT[];
extern const char MQTT_DEVICE_KEY[];
