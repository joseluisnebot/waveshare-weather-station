# Waveshare ESP32-S3-Touch-LCD-7 — Guía de configuración rápida

Pantalla táctil 800×480 con ESP32-S3 integrado. Esta guía recoge todo lo que
hay que saber para reutilizar la placa en cualquier proyecto sin perder tiempo
depurando los mismos bugs.

---

## 1. platformio.ini mínimo

```ini
[env:waveshare_esp32s3_touch_lcd_7]
platform  = espressif32
board     = esp32-s3-devkitc-1
framework = arduino

board_build.arduino.memory_type = qio_opi   ; PSRAM OPI 8 MB
board_build.partitions           = default_16MB.csv
board_upload.flash_size          = 16MB
board_upload.maximum_size        = 16777216

upload_speed   = 460800
monitor_speed  = 115200

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1        ; USB-CDC
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DLV_CONF_INCLUDE_SIMPLE
    -I src
```

---

## 2. Pines fijos (NO cambiar)

### LCD RGB
| Señal | GPIO |
|-------|------|
| HSYNC | 46 |
| VSYNC | 3  |
| DE    | 5  |
| PCLK  | 7  |
| R3-R7 | 1, 2, 42, 41, 40 |
| G2-G7 | 39, 0, 45, 48, 47, 21 |
| B3-B7 | 14, 38, 18, 17, 10 |

### I2C (touch + IO expander)
| Señal | GPIO |
|-------|------|
| SDA   | 8    |
| SCL   | 9    |

### I2C devices
| Dispositivo | Dirección | Función |
|-------------|-----------|---------|
| CH422G modo | 0x24      | Configurar IOs como salidas |
| CH422G out  | 0x38      | bits: 0=LCD_RST, 1=TP_RST, 2=LCD_BL |
| CH422G OC   | 0x23      | Backlight PWM (0xFF = máximo) |
| GT911       | 0x5D      | Controlador táctil |

### GPIOs disponibles para el proyecto
Los GPIOs **libres** típicos (no usados por LCD/I2C/flash/PSRAM):
`GPIO 4, 6, 11, 12, 13, 15, 16`

---

## 3. Secuencia de arranque obligatoria

```cpp
// 1. I2C
Wire.begin(8, 9);
Wire.setClock(400000);

// 2. CH422G: modo output
Wire.beginTransmission(0x24); Wire.write(0x00); Wire.endTransmission();
delay(10);

// 3. Reset LCD + Touch (activo en bajo)
Wire.beginTransmission(0x38); Wire.write(0x00); Wire.endTransmission(); // todo bajo
delay(50);
Wire.beginTransmission(0x38); Wire.write(0x07); Wire.endTransmission(); // bit0=LCD_RST, bit1=TP_RST, bit2=BL
delay(120);

// 4. Backlight al máximo
Wire.beginTransmission(0x23); Wire.write(0xFF); Wire.endTransmission();
```

---

## 4. Panel RGB (timings correctos para 800×480)

```cpp
esp_lcd_rgb_panel_config_t cfg = {};
cfg.clk_src                       = LCD_CLK_SRC_PLL160M;
cfg.timings.pclk_hz               = 16 * 1000 * 1000;
cfg.timings.h_res                 = 800;
cfg.timings.v_res                 = 480;
cfg.timings.hsync_pulse_width     = 4;
cfg.timings.hsync_back_porch      = 43;
cfg.timings.hsync_front_porch     = 210;
cfg.timings.vsync_pulse_width     = 4;
cfg.timings.vsync_back_porch      = 12;
cfg.timings.vsync_front_porch     = 22;
cfg.timings.flags.pclk_active_neg = true;
cfg.data_width                    = 16;
cfg.psram_trans_align             = 64;
cfg.sram_trans_align              = 4;
cfg.flags.fb_in_psram             = true;   // framebuffer en PSRAM
```

Orden de bits de datos (16 bits RGB565):
```cpp
const int pins[16] = {
    14, 38, 18, 17, 10,          // B3-B7
    39,  0, 45, 48, 47, 21,      // G2-G7
     1,  2, 42, 41, 40           // R3-R7
};
```

---

## 5. GT911 — Lectura de coordenadas

**BUG CONOCIDO de este board**: las coordenadas llegan en big-endian, al
contrario de la especificación GT911.

```cpp
#define GT911_ADDR        0x5D
#define GT911_STATUS_REG  0x814E
#define GT911_POINT1_REG  0x8150

bool gt911_touch(int16_t* x, int16_t* y) {
    uint8_t st = 0;
    // Leer status
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(GT911_STATUS_REG >> 8); Wire.write(GT911_STATUS_REG & 0xFF);
    if (Wire.endTransmission()) return false;
    Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)1);
    st = Wire.read();

    if (!(st & 0x80)) return false;   // buffer no listo

    uint8_t cnt = st & 0x0F;

    // SIEMPRE limpiar el status (si no, el GT911 se bloquea)
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(GT911_STATUS_REG >> 8); Wire.write(GT911_STATUS_REG & 0xFF);
    Wire.write(0x00);
    Wire.endTransmission();

    if (cnt == 0) return false;

    // Leer punto 1
    uint8_t pt[6];
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(GT911_POINT1_REG >> 8); Wire.write(GT911_POINT1_REG & 0xFF);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)GT911_ADDR, (uint8_t)6);
    for (int i = 0; i < 6 && Wire.available(); i++) pt[i] = Wire.read();

    // pt[0]=TrackID, pt[1]=X_H, pt[2]=X_L, pt[3]=Y_H, pt[4]=Y_L
    // ATENCION: big-endian en este board (al contrario del datasheet)
    *x = (pt[1] << 8) | pt[2];
    *y = (pt[3] << 8) | pt[4];
    return true;
}
```

---

## 6. LVGL 8 — Configuración mínima (lv_conf.h)

```c
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP    0
#define LV_MEM_SIZE        (64 * 1024U)
#define LV_HOR_RES_MAX     800
#define LV_VER_RES_MAX     480
#define LV_DPI_DEF         130
```

**Buffer de render**: asignarlo en SRAM interna (no PSRAM) para evitar
contención con el DMA del panel:

```cpp
lv_color_t* fb = (lv_color_t*)heap_caps_malloc(
    800 * 10 * sizeof(lv_color_t),
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
```

**Fuentes disponibles sin recompilar**: Montserrat 14, 24, 48 (solo ASCII).
Para caracteres españoles (ñ, á, é, í, ó, ú) compilar fuente personalizada
con la herramienta LVGL o sustituir por equivalentes ASCII.

---

## 7. RMT en ESP32-S3 (lectura de sensores 1-wire)

En ESP32-S3 con el API legacy de ESP-IDF:
- **Canales 0-3**: solo TX
- **Canales 4-7**: RX

```cpp
#define MI_SENSOR_RMT_CHANNEL  RMT_CHANNEL_4   // RX en ESP32-S3
```

Con channel 0 en modo RX → `ESP_ERR_INVALID_ARG` → abort() → boot loop.

---

## 8. Checklist para un proyecto nuevo

- [ ] Copiar `platformio.ini` con la configuración de memoria `qio_opi`
- [ ] Copiar `lv_conf.h` base
- [ ] Copiar la secuencia de arranque CH422G + LCD + GT911
- [ ] Para sensores RMT: usar channel 4-7
- [ ] Limpiar GT911 status siempre tras leer
- [ ] Coordenadas GT911: usar `(pt[1]<<8)|pt[2]` para X, `(pt[3]<<8)|pt[4]` para Y
- [ ] Strings de UI: solo ASCII (sin tildes ni ñ salvo fuente personalizada)
- [ ] Puerto serie: `/dev/ttyACM1` (el ACM0 es el puerto interno del ESP32-S3)

---

## 9. GPIOs disponibles y consumo

| Uso | GPIOs ocupados |
|-----|---------------|
| LCD RGB (16 bits) | 0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48 |
| I2C (touch+expander) | 8, 9 |
| Flash 16 MB (QIO) | 26, 27, 28, 29, 30, 31, 32 |
| PSRAM OPI 8 MB | 33, 34, 35, 36, 37 |
| **Libres** | **4, 6, 11, 12, 13, 15, 16** |

Consumo típico: ~250 mA a 5V con backlight al máximo. Con backlight reducido: ~120 mA.
