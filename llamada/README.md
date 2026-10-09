# Cambios en el código antes de ejecutar:
## En archivo del esp32:
Hay que cambiar estos 3 datos:

```go
const char* WIFI_SSID = "TU_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD";
const char* PC_IP = "192.168.1.100"; 
```

la ip se puede buscar con ipconfig

## En archivo pyhton de la pc:
Lo único a cambiar es buscar la ip real del esp32, sale en los prints al ejecutar el codgio del esp32.
Si no tenés `sounddevice` hay que instalarla`: pip install sounddevice`
