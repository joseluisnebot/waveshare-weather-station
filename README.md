# Estación Meteorológica — Waveshare ESP32-S3-Touch-LCD-7

Estación meteorológica con pantalla táctil 800×480 que muestra datos de interior (sensor DHT11) y exterior (API Open-Meteo) con interfaz LVGL.

## Hardware

| Componente | Detalle |
|------------|---------|
| Placa | Waveshare ESP32-S3-Touch-LCD-7 |
| Pantalla | RGB 800×480, táctil capacitivo GT911 |
| Sensor interior | DHT11 en GPIO6 (vía RMT) |
| IP en red local | 192.168.1.225 |

## Stack

- **Framework:** Arduino + PlatformIO
- **UI:** LVGL 8.3.x
- **Meteorología exterior:** Open-Meteo API (Vinaròs, 40.471°N 0.4746°E)
- **OTA:** ElegantOTA v3
- **Libs:** DHT (vía RMT), ArduinoJson
- **Partición:** default_16MB.csv (flash 16MB, PSRAM OPI 8MB)

## Estructura

```
src/
  main.cpp      # Código principal: WiFi, UI LVGL, DHT11, Open-Meteo, OTA
  lv_conf.h     # Configuración LVGL (resolución, memoria, fuentes)
platformio.ini  # Configuración PlatformIO
SETUP_GUIDE.md  # Guía técnica completa de la placa (pines, bugs, timings)
```

## Compilar y flashear

```bash
# Compilar
pio run

# Flashear por USB (primera vez)
pio run --target upload

# OTA (actualizaciones posteriores)
# Binario generado en: .pio/build/waveshare_esp32s3_touch_lcd_7/firmware.bin
# Subir en: http://192.168.1.225/ota/upload
# Usuario: admin | Contraseña: ota1234
# IMPORTANTE: apagar y encender tras OTA (el reset SW no reinicializa el panel RGB)
```

## Fixes críticos aplicados

| Problema | Solución |
|----------|----------|
| CH422G no respondía | Registro salida: `0x38` (no `0x24`) |
| Pantalla sin backlight | Registro OC `0x23` con `0xFF` |
| Contención PSRAM/display | `pclk_hz: 8MHz` |
| Banda negra en pantalla | `pad_all=0` en screen object LVGL |
| DHT11 desplazaba imagen | Usar RMT en lugar de Adafruit DHT |
| ROCm no detectaba GPU del host | `HSA_OVERRIDE_GFX_VERSION=10.3.0` |

## Datos mostrados

- **Interior:** temperatura y humedad (DHT11)
- **Exterior:** temperatura actual, mín/máx del día, lluvia, viento, código meteorológico (Open-Meteo)
- **UI:** fondo oscuro `0x0D1B2A`, actualización cada 10 minutos

## WiFi

- Red: `nbcasa`
- IP fija: `192.168.1.225`

## Referencia técnica

Ver [`SETUP_GUIDE.md`](SETUP_GUIDE.md) para: pines, secuencia de arranque CH422G, timings RGB, lectura GT911, configuración LVGL y checklist para proyectos nuevos.
