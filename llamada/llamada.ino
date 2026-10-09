#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP_I2S.h>

const char* WIFI_SSID = "WIFI";
const char* WIFI_PASSWORD = "PASSWORD";

// IMPORTANTE:
// Reemplazar por la IP REAL de PC.
const char* PC_IP = "192.168.1.100";

// =====================================================
// PUERTOS UDP
// =====================================================

// XIAO -> PC
const uint16_t PC_RX_PORT = 5000;

// PC -> XIAO
const uint16_t XIAO_RX_PORT = 5001;

// =====================================================
// AUDIO
// =====================================================

#define SAMPLE_RATE 16000

// 20 ms de audio
#define SAMPLES_PER_PACKET 320

// =====================================================
// MICRÓFONO PDM DEL XIAO 
// =====================================================

#define MIC_CLK  42
#define MIC_DATA 41

// =====================================================
// MAX98357A
// =====================================================

//conexión física:
//
// D4  -> LRC
// D6  -> BCLK
// D8  -> DIN
//
// GPIO reales:
//
// D4 = GPIO5
// D6 = GPIO43
// D8 = GPIO7

#define SPK_BCLK 43
#define SPK_LRC  5
#define SPK_DIN  7

// =====================================================
// I2S
// =====================================================

// Micrófono
I2SClass micI2S(I2S_NUM_0);

// MAX98357
I2SClass speakerI2S(I2S_NUM_1);

// =====================================================
// UDP
// =====================================================

WiFiUDP udpMic;
WiFiUDP udpSpeaker;

// =====================================================
// BUFFERS
// =====================================================

// Micrófono: mono 16-bit
int16_t micBuffer[SAMPLES_PER_PACKET];

// PC -> XIAO:
// recibimos mono
int16_t receivedMono[SAMPLES_PER_PACKET];

// Para MAX98357:
// estéreo: L + R
int16_t speakerBuffer[SAMPLES_PER_PACKET * 2];

// =====================================================
// MICRÓFONO
// =====================================================

void setupMicrophone()
{
    Serial.println("Inicializando micrófono...");

    micI2S.setPinsPdmRx(
        MIC_CLK,
        MIC_DATA
    );

    bool ok = micI2S.begin(
        I2S_MODE_PDM_RX,
        SAMPLE_RATE,
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO
    );

    if (!ok)
    {
        Serial.println("ERROR: no se pudo iniciar el micrófono");

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println("Micrófono OK");
}

// =====================================================
// CONFIGURAR MAX98357A
// =====================================================

void setupSpeaker()
{
    Serial.println("Inicializando MAX98357A...");

    speakerI2S.setPins(
        SPK_BCLK,
        SPK_LRC,
        SPK_DIN
    );

    bool ok = speakerI2S.begin(
        I2S_MODE_STD,
        SAMPLE_RATE,
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO
    );

    if (!ok)
    {
        Serial.println("ERROR: no se pudo iniciar MAX98357A");

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println("MAX98357A OK");
}

// =====================================================
// WIFI
// =====================================================

void connectWiFi()
{
    Serial.print("Conectando a Wi-Fi");

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    Serial.println("Wi-Fi conectado");

    Serial.print("IP del XIAO: ");
    Serial.println(WiFi.localIP());

    Serial.print("IP de la PC: ");
    Serial.println(PC_IP);
}

// =====================================================
// XIAO -> PC
// =====================================================

void sendMicrophoneAudio()
{
    size_t bytesRead = micI2S.readBytes(
        (char*)micBuffer,
        sizeof(micBuffer)
    );

    if (bytesRead == 0)
    {
        return;
    }

    udpMic.beginPacket(
        PC_IP,
        PC_RX_PORT
    );

    udpMic.write(
        (uint8_t*)micBuffer,
        bytesRead
    );

    udpMic.endPacket();
}

// =====================================================
// PC -> XIAO
// =====================================================

void receiveSpeakerAudio()
{
    int packetSize = udpSpeaker.parsePacket();

    if (packetSize <= 0)
    {
        return;
    }

    // Un paquete normal debería ser:
    //
    // 320 muestras × 2 bytes
    // = 640 bytes
    if (packetSize != sizeof(receivedMono))
    {
        Serial.print("Paquete inesperado: ");
        Serial.print(packetSize);
        Serial.println(" bytes");

        while (udpSpeaker.available())
        {
            udpSpeaker.read();
        }

        return;
    }

    int bytesRead = udpSpeaker.read(
        (uint8_t*)receivedMono,
        sizeof(receivedMono)
    );

    if (bytesRead != sizeof(receivedMono))
    {
        return;
    }

    // =================================================
    // MONO -> ESTÉREO
    // =================================================

    for (int i = 0; i < SAMPLES_PER_PACKET; i++)
    {
        int16_t sample = receivedMono[i];

        // Left
        speakerBuffer[i * 2] = sample;

        // Right
        speakerBuffer[i * 2 + 1] = sample;
    }

    // =================================================
    // ENVIAR AL MAX98357A
    // =================================================

    speakerI2S.write(
        (uint8_t*)speakerBuffer,
        sizeof(speakerBuffer)
    );
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);

    delay(1000);
    connectWiFi();

    setupMicrophone();

    setupSpeaker();

    // Puerto donde recibimos audio de la PC
    if (!udpSpeaker.begin(XIAO_RX_PORT))
    {
        Serial.println("ERROR iniciando UDP");

        while (true)
        {
            delay(1000);
        }
    }

    Serial.print("Esperando audio de PC en UDP ");
    Serial.println(XIAO_RX_PORT);

    Serial.println();
    Serial.println("==============================");
    Serial.println(" AUDIO FULL-DUPLEX ACTIVO");
    Serial.println("==============================");
}

void loop()
{
    // PC -> XIAO
    receiveSpeakerAudio();

    // XIAO -> PC
    sendMicrophoneAudio();
}