import socket
import threading
import sounddevice as sd

# =====================================================
# CONFIGURACIÓN, OJO, PONER LA IP CORRECTA DEL esp32
# =====================================================

# IP DEL XIAO, cuando se corre el timbre nos da la ip
XIAO_IP = "192.168.1.120" 

# =====================================================
# AUDIO
# =====================================================

SAMPLE_RATE = 16000
CHANNELS = 1

# 20 ms
SAMPLES_PER_PACKET = 320

# =====================================================
# UDP
# =====================================================

# XIAO -> PC
PC_RX_PORT = 5000

# PC -> XIAO
XIAO_RX_PORT = 5001


# =====================================================
# SOCKET PARA RECIBIR AUDIO DEL XIAO
# =====================================================

rx_socket = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM
)

rx_socket.bind(
    ("0.0.0.0", PC_RX_PORT)
)


# =====================================================
# SOCKET PARA ENVIAR AUDIO AL XIAO
# =====================================================

tx_socket = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM
)


# =====================================================
# PARLANTES DE LA PC
# =====================================================

speaker_stream = sd.RawOutputStream(
    samplerate=SAMPLE_RATE,
    channels=CHANNELS,
    dtype="int16",
    blocksize=SAMPLES_PER_PACKET
)


# =====================================================
# MICRÓFONO DE LA PC
# =====================================================

microphone_stream = sd.RawInputStream(
    samplerate=SAMPLE_RATE,
    channels=CHANNELS,
    dtype="int16",
    blocksize=SAMPLES_PER_PACKET
)


# =====================================================
# XIAO -> PC
# =====================================================

def receive_xiao_audio():

    print("Recepción XIAO -> PC iniciada")

    while True:

        data, address = rx_socket.recvfrom(2048)

        speaker_stream.write(data)


# =====================================================
# PC -> XIAO
# =====================================================

def send_pc_microphone():

    print("Transmisión PC -> XIAO iniciada")

    while True:

        data, overflowed = microphone_stream.read(
            SAMPLES_PER_PACKET
        )

        tx_socket.sendto(
            bytes(data),
            (XIAO_IP, XIAO_RX_PORT)
        )


# =====================================================
# MAIN
# =====================================================

print()
print("XIAO ESP32 PHONE CALL")
print()

print("XIAO:", XIAO_IP)
print("PC RX:", PC_RX_PORT)
print("XIAO RX:", XIAO_RX_PORT)
print()

print("Inicializando audio...")

speaker_stream.start()
microphone_stream.start()

print("Audio iniciado.")
print()

# =====================================================
# HILOS
# =====================================================

receive_thread = threading.Thread(
    target=receive_xiao_audio,
    daemon=True
)

send_thread = threading.Thread(
    target=send_pc_microphone,
    daemon=True
)

receive_thread.start()
send_thread.start()

print("========================================")
print("          LLAMADA CONECTADA")
print("========================================")
print()
print("XIAO -> PC : micrófono")
print("PC -> XIAO : micrófono")
print()
print("Ctrl+C para terminar")
print()

try:

    while True:
        receive_thread.join(1)

except KeyboardInterrupt:

    print()
    print("Finalizando llamada...")

finally:

    microphone_stream.stop()
    microphone_stream.close()

    speaker_stream.stop()
    speaker_stream.close()

    rx_socket.close()
    tx_socket.close()

    print("Llamada finalizada.")