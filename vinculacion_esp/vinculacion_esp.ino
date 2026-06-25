#include <Preferences.h>


Preferences prefs;

//Programa para borrar las credenciales guardadas en memoria.
void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("borro credecniales...");
    prefs.begin("wifi", false);
    prefs.clear(); 
    prefs.end();
    Serial.println("Memoria borrada");
}

void loop() {
}