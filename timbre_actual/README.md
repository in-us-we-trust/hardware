# Timbre actual

Firmware para un timbre basado en Seeed XIAO ESP32-S3 Sense. El flujo vigente del proyecto es deliberadamente más simple que una llamada WebRTC: el ESP32 permanece en reposo, activa la cámara cuando se pulsa el botón, sirve capturas JPEG por HTTP y la página web las muestra. La sesión termina desde el botón **Colgar** de la página.

La llamada bidireccional de audio y la pila WebRTC se conservaron en el repositorio como código experimental o pendiente, pero no forman parte del flujo funcional actual y no deben considerarse una dependencia para probar el video.

## Estado actual

Actualmente funciona o está implementado lo siguiente:

- Provisionamiento inicial de WiFi mediante un portal HTTP local, BLE y una aplicación externa.
- Registro del dispositivo en el gateway mediante HTTPS/mTLS.
- Persistencia en la memoria NVS del ESP32 de las credenciales WiFi, el identificador del dispositivo y el estado de registro.
- Modo de reposo con cámara apagada, WiFi en modem sleep y CPU a 80 MHz.
- Activación del video mediante el botón físico conectado a GPIO1/D0.
- Servidor HTTP temporal en el ESP32, en el puerto 8081.
- Visualización del video en una página web servida por Node.js.
- Finalización desde la página web mediante `/hangup`, apagado de cámara y retorno al reposo.

No forma parte del flujo actual:

- Una llamada WebRTC real.
- Audio bidireccional entre el ESP32 y el navegador.
- Uso del micrófono o del altavoz como transporte de una llamada.
- Envío periódico de imágenes a `/api/v1/events`; el código de ese cliente permanece, pero esa lógica fue retirada del flujo de ejecución.

## Requisitos

- Seeed XIAO ESP32-S3 Sense.
- PlatformIO y un entorno Arduino para ESP32.
- Node.js y npm para la página web.
- Computador y ESP32 conectados a la misma red WiFi durante la prueba.
- Aplicación de configuración BLE compatible con las UUID del proyecto, por ejemplo nRF Connect o la aplicación que utilice el equipo.

## Estructura principal

### Firmware

- `src/main.cpp`: arranque, lectura del estado guardado, interrupción del botón y ciclo principal.
- `lib/Storage`: lectura y escritura de credenciales WiFi, UUID y flag de registro en NVS.
- `lib/Device`: nombre BLE e identificador persistente del dispositivo.
- `lib/WebPortal`: punto de acceso inicial y página de configuración con QR y eventos SSE.
- `lib/BleManager`: servicio BLE que recibe credenciales y la confirmación de registro.
- `lib/WifiManager`: conexión WiFi, dirección IP y sincronización NTP.
- `lib/ApiClient`: registro HTTPS del dispositivo en el gateway mediante mTLS.
- `lib/Certs`: certificados usados por `ApiClient`.
- `lib/CameraManager`: inicialización, captura y apagado de la cámara.
- `lib/LowPower`: estado de reposo.
- `lib/CallManager`: servidor HTTP temporal del video y endpoint para colgar.
- `lib/AudioManager`: código I2S conservado como base experimental; no implementa audio de llamada.

### Aplicación web local

- `webrtc-signal/server.js`: servidor HTTP de la página y relay WebSocket experimental.
- `webrtc-signal/public/index.html`: interfaz actual; consulta el stream JPEG y contiene el botón **Colgar**.
- `webrtc-signal/package.json`: dependencia `ws` y comando `npm start`.

El nombre `webrtc-signal` es histórico. La página vigente no usa `RTCPeerConnection` ni abre un WebSocket.

## Flujo de configuración inicial

### 1. Arranque sin credenciales

Al iniciar, `src/main.cpp` lee las credenciales guardadas en NVS.

Si no existen, o si existen pero la conexión falla, el ESP32 entra en configuración:

1. Crea un punto de acceso WiFi abierto cuyo SSID es el nombre del dispositivo, normalmente `TIMBRE-XXXXXX`.
2. Levanta un servidor HTTP en el puerto 80 del propio ESP32.
3. Activa BLE y comienza a anunciar el mismo nombre del dispositivo.
4. La página del portal se abre desde el punto de acceso y muestra un QR.
5. El QR contiene el nombre BLE del dispositivo. No contiene las credenciales WiFi.
6. La página mantiene una conexión SSE con `/events` para mostrar estados del proceso.

Para abrir el portal durante esta etapa, hay que conectarse desde el celular o computador al AP del timbre y visitar:

```text
http://192.168.4.1/
```

La dirección habitual del AP es `192.168.4.1`, porque es la dirección predeterminada de `WiFi.softAP()`.

### 2. Envío de credenciales por BLE

La aplicación BLE debe conectarse al servicio:

```text
Servicio:              12345678-1234-1234-1234-123456789000
Credenciales WiFi:     12345678-1234-1234-1234-123456789001
Confirmación registro: 12345678-1234-1234-1234-123456789002
```

En la characteristic de credenciales se escribe un JSON con este formato:

```json
{"ssid":"NOMBRE_DE_RED","password":"CONTRASEÑA"}
```

El callback BLE guarda las credenciales y deja el trabajo pesado para el ciclo principal. Después el ESP32:

1. Intenta conectarse a la red WiFi.
2. Sincroniza la hora mediante NTP, necesaria para TLS.
3. Genera o recupera el identificador persistente del dispositivo.
4. Notifica por BLE el `deviceId` y la dirección MAC.
5. Si todavía no está registrado, llama al gateway mediante `POST /api/v1/devices` usando mTLS.

Si la conexión WiFi falla, las credenciales permanecen guardadas, se notifica `wifi_failed` por BLE y el dispositivo sigue en configuración.

### 3. Confirmación de registro

Cuando el registro en el gateway responde correctamente, el firmware solicita que la aplicación escriba lo siguiente en la characteristic de confirmación:

```json
{"status":"success"}
```

Al recibirlo, el ESP32:

1. Muestra `Dispositivo vinculado!` en el portal.
2. Guarda el flag `registered=true`.
3. Detiene BLE.
4. Detiene el portal y apaga el punto de acceso.

Si recibe otro estado, muestra un error y reinicia el ESP32 para volver al flujo de configuración.

Hay una consideración importante para quien continúe el proyecto: el código también guarda `registered=true` inmediatamente después de que el gateway acepta el registro, antes de recibir la confirmación BLE. Por eso el registro remoto y la confirmación de la aplicación no son exactamente la misma etapa. Además, si el dispositivo ya aparece como registrado, el código marca el gateway como listo y no vuelve a registrar el dispositivo.

## Flujo normal de reposo y video

Cuando el ESP32 arranca y encuentra credenciales válidas:

1. Se conecta a WiFi.
2. Imprime su IP local por el monitor serial.
3. No activa el portal ni BLE.
4. Apaga la cámara.
5. Activa el sleep del WiFi.
6. Baja la CPU a 80 MHz.
7. Queda esperando el botón.

El botón está configurado en `GPIO1/D0`, con `INPUT_PULLUP` e interrupción por flanco descendente. Hay un debounce de 200 ms.

### Inicio del stream

Antes de pulsar el botón, la página web debe estar abierta y el servidor Node.js debe estar ejecutándose.

Al pulsar el botón:

1. `CallManager::beginCall()` sube la CPU a 240 MHz.
2. Desactiva el sleep del WiFi.
3. Inicializa la cámara.
4. Crea un servidor HTTP en el puerto `8081`.
5. Comienza a atender `/stream`.

Cada solicitud a `/stream` captura una imagen JPEG, la devuelve y cierra la conexión. No es un stream MJPEG persistente ni WebRTC. La página solicita una nueva imagen cada 250 ms, por lo que el efecto visual es un video basado en capturas sucesivas.

Las rutas del ESP32 son:

```text
http://IP_DEL_ESP32:8081/
http://IP_DEL_ESP32:8081/stream
http://IP_DEL_ESP32:8081/hangup
```

La ruta `/` es una página HTML mínima de diagnóstico. La ruta `/stream` entrega una imagen JPEG. La ruta `/hangup` finaliza la llamada.

Las pulsaciones posteriores mientras la llamada está activa se ignoran. La llamada no termina con otra pulsación del botón.

### Página del computador

Desde la raíz de `webrtc-signal`:

```bash
npm install
npm start
```

La página queda disponible en:

```text
http://localhost:8080/
```

En el campo `ESP32` se debe escribir la URL del dispositivo, por ejemplo:

```text
http://192.168.1.46:8081
```

La página consulta automáticamente:

```text
http://192.168.1.46:8081/stream
```

Si comienza a recibir imágenes, muestra el estado de llamada activa y el video. Si todavía no se pulsó el timbre, las solicitudes fallan y la página permanece esperando.

### Finalización desde la página

La responsabilidad de colgar pertenece actualmente al computador. Al pulsar **Colgar**, la página:

1. Detiene temporalmente el polling del video.
2. Llama a `http://IP_DEL_ESP32:8081/hangup`.
3. El ESP32 marca la llamada como inactiva.
4. En el siguiente ciclo, detiene el servidor del puerto 8081.
5. Apaga la cámara.
6. Activa el sleep del WiFi.
7. Baja la CPU a 80 MHz.
8. Vuelve al estado de reposo y queda listo para otra pulsación.

## Cómo probar el proyecto

### Subir el firmware

Desde la raíz del repositorio:

```bash
pio run -e seeed_xiao_esp32s3
pio run -e seeed_xiao_esp32s3 -t upload
pio device monitor -b 115200
```

El monitor serial permite confirmar la IP del ESP32, la conexión WiFi, la activación de cámara y la transición a reposo.

### Probar el portal inicial

1. Borrar las credenciales NVS o usar un dispositivo que todavía no haya sido configurado.
2. Reiniciar el ESP32.
3. Conectarse al AP cuyo nombre aparece en el monitor serial.
4. Abrir `http://192.168.4.1/`.
5. Conectarse por BLE usando el nombre mostrado en el portal o el QR.
6. Escribir el JSON de credenciales en la characteristic `...9001`.
7. Esperar a que el ESP32 se conecte y registre en el gateway.
8. Escribir `{"status":"success"}` en la characteristic `...9002`.
9. Confirmar en el monitor serial que el AP y BLE se apagaron.

La forma exacta de borrar NVS depende del procedimiento del equipo. No se debe borrar almacenamiento durante una prueba normal si se quiere conservar las credenciales.

### Probar el video

1. Conectar el computador y el ESP32 a la misma red WiFi.
2. Iniciar `webrtc-signal`.
3. Abrir `http://localhost:8080/`.
4. Obtener la IP del ESP32 del monitor serial.
5. Escribir `http://IP_DEL_ESP32:8081` en el campo `ESP32`.
6. Pulsar el botón físico.
7. Confirmar que la página muestra el video.
8. Pulsar **Colgar**.
9. Confirmar en el monitor serial que se detienen servidor y cámara y que vuelve el modo de reposo.
10. Pulsar nuevamente el botón para comprobar que puede iniciarse otra sesión.

Si el navegador no puede conectarse, revisar primero que ambas máquinas estén en la misma red, que la IP sea la actual y que el ESP32 haya impreso `Servidor de streaming iniciado en puerto 8081`.

## Parte bidireccional descartada

La llamada bidireccional no se va a implementar en el flujo actual, pero el código se conserva para que la persona que continúe pueda reconocerlo y decidir posteriormente si lo elimina o lo reutiliza.

Los elementos relacionados son:

- `lib/AudioManager/AudioManager.cpp` y `lib/AudioManager/AudioManager.h`: configuran I2S y leen muestras, pero no envían ni reciben audio por red. El comentario del código identifica el transporte WebRTC como trabajo pendiente.
- `lib/CallManager/CallManager.cpp`: tiene el nombre general de gestor de llamada, pero actualmente solo coordina cámara, HTTP, stream y hangup.
- `webrtc-signal/server.js`: incluye un relay WebSocket que reenvía mensajes `offer`, `answer`, `candidate`, `ice` y `hangup` entre roles. No implementa WebRTC por sí mismo.
- `webrtc-signal/public/index.html`: es la interfaz vigente, pero no crea `RTCPeerConnection`, no pide permisos de micrófono y no abre el WebSocket.
- `webrtc-signal/package.json`: conserva la dependencia `ws`, necesaria para el relay experimental.
- `lib/CameraManager/CameraManager.cpp`: contiene una función para capturar imágenes en Base64 que no es utilizada por el flujo actual de video HTTP.
- `lib/ApiClient/ApiClient.cpp`: conserva `sendEvent()` para eventos con imagen, pero el flujo actual no lo llama periódicamente.

Conclusión: estos elementos no son necesarios para probar el stream actual. No deben confundirse con dependencias funcionales del flujo botón -> video -> colgar -> reposo.

## Puntos de extensión para el trabajo futuro

La persona que continúe el proyecto puede agregar nuevas llamadas a APIs alrededor del flujo vigente, teniendo en cuenta:

- La configuración inicial y el registro se ejecutan desde `BleManager`.
- La IP del ESP32 aparece por Serial después de conectarse a WiFi.
- El inicio de la sesión ocurre en `CallManager::beginCall()`.
- El cierre y la limpieza ocurren mediante `/hangup`, `CallManager::handle()` y `LowPower::enterIdleMode()`.
- Las credenciales, el identificador y el flag de registro sobreviven a reinicios porque se guardan en NVS.
- Las llamadas HTTPS al gateway dependen de hora válida y certificados mTLS.
- El servidor de señalización Node.js no es necesario para que el ESP32 capture el video, pero sí es la forma incluida de servir la página web.

Antes de modificar el flujo de llamada, conviene conservar la prueba manual completa y comprobar que cualquier API nueva no bloquee el ciclo principal ni impida atender `/hangup`.

## Detención del servidor web

Para detener el servidor iniciado con `npm start`, pulsar `Ctrl+C` en la terminal donde está ejecutándose.

Si quedó un proceso Node separado:

```bash
pgrep -af "node server.js"
kill PID
```

Usar `kill -9 PID` solo si el proceso no termina de manera normal.

## Limitaciones conocidas

- El nombre `webrtc-signal` no refleja exactamente la implementación actual.
- El video se entrega como capturas JPEG sucesivas, no como WebRTC ni como MJPEG persistente.
- El navegador y el ESP32 deben poder comunicarse directamente por la red local.
- No hay autenticación en las rutas HTTP del ESP32; cualquier equipo con acceso a esa red podría intentar consultar `/stream` o llamar `/hangup`.
- El estado de registro se persiste antes de la confirmación BLE final, como se explicó arriba.
- La configuración requiere conocer las UUID BLE y utilizar una aplicación compatible.
