/*
  ╔══════════════════════════════════════════════════════════════╗
  ║     ESTACION METEOROLOGICA - Waveshare ESP32-S3-Touch-LCD-7  ║
  ║     800×480 · LVGL 8 · Open-Meteo ECMWF · DHT22             ║
  ╚══════════════════════════════════════════════════════════════╝

  Conexion DHT22:
    GPIO11  →  DATA  (+ resistencia 10kΩ entre DATA y 3.3V)
    3.3V    →  VCC
    GND     →  GND
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include "driver/rmt.h"
#include "freertos/ringbuf.h"
#include <lvgl.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════
//  CONFIGURACION  ← SOLO EDITA ESTO
// ═══════════════════════════════════════════════════════════════
#define WIFI_SSID      "nbcasa"
#define WIFI_PASSWORD  "Tresycuarto77"
#define DHT_PIN        6
// ═══════════════════════════════════════════════════════════════

// ─── PINES LCD RGB ────────────────────────────────────────────
#define LCD_HSYNC  46
#define LCD_VSYNC   3
#define LCD_DE      5
#define LCD_PCLK    7
#define LCD_R3   1
#define LCD_R4   2
#define LCD_R5  42
#define LCD_R6  41
#define LCD_R7  40
#define LCD_G2  39
#define LCD_G3   0
#define LCD_G4  45
#define LCD_G5  48
#define LCD_G6  47
#define LCD_G7  21
#define LCD_B3  14
#define LCD_B4  38
#define LCD_B5  18
#define LCD_B6  17
#define LCD_B7  10

// ─── I2C / TOUCH / IO EXPANDER ────────────────────────────────
#define I2C_SDA  8
#define I2C_SCL  9
#define GT911_ADDR        0x5D
#define CH422G_MODE_ADDR  0x24   // registro de modo / configuracion
#define CH422G_OUT_ADDR   0x38   // registro de salida pines 0-7
#define CH422G_LCD_RST (1<<0)
#define CH422G_TP_RST  (1<<1)
#define CH422G_LCD_BL  (1<<2)
#define GT911_STATUS_REG 0x814E
#define GT911_POINT1_REG 0x8150

// ─── LCD / LVGL ───────────────────────────────────────────────
#define LCD_W  800
#define LCD_H  480
#define LCD_PCLK_HZ (16*1000*1000)
#define LVGL_TICK_MS 5

// ─── OPEN-METEO ───────────────────────────────────────────────
const char* OWM_URL =
  "http://api.open-meteo.com/v1/forecast"
  "?latitude=40.471&longitude=0.4746"
  "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,apparent_temperature"
  "&daily=temperature_2m_max,temperature_2m_min,weather_code"
  "&forecast_days=4"
  "&timezone=Europe%2FMadrid";

// ─── NOTICIAS (RSS El Mundo portada) ──────────────────────────
#define NEWS_COUNT 5
#define NEWS_URL   "https://e00-elmundo.uecdn.es/rss/portada.xml"
#define NEWS_REFRESH_MS 900000   // 15 min

// ─── GLOBALES ─────────────────────────────────────────────────
static esp_lcd_panel_handle_t lcd_panel  = NULL;
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static lv_color_t*        lvgl_fb    = NULL;
static SemaphoreHandle_t  lvgl_mutex = NULL;
static SemaphoreHandle_t  vsync_sem  = NULL;
static WebServer ota_server(80);

// Datos actuales
static float  g_tempInt  = 0.0f;
static float  g_humInt   = 0.0f;
static float  g_tempExt  = 0.0f;
static float  g_feelExt  = 0.0f;
static float  g_windExt  = 0.0f;
static int    g_humExt   = 0;
static int    g_wmoCode  = -1;
static String g_descExt  = "Cargando...";
static bool   g_newIntData = false;
static bool   g_newExtData = false;

// Previsión 4 días
#define FCST_DAYS 4
static float  g_fcst_max[FCST_DAYS];
static float  g_fcst_min[FCST_DAYS];
static int    g_fcst_wmo[FCST_DAYS];
static bool   g_newFcstData = false;

// Pantallas y navegación táctil
static lv_obj_t* scr_main     = NULL;
static lv_obj_t* scr_forecast = NULL;
static int8_t    g_cur_screen = 0;       // 0=actual, 1=previsión
static volatile int8_t g_swipe = 0;     // -1=izq, +1=der, 0=ninguno

// Detección de swipe
static int16_t swipe_start_x = 0;
static int16_t swipe_last_x  = 0;
static bool    swipe_active  = false;

// Noticias
static String g_news[NEWS_COUNT];
static bool   g_newNewsData = false;

// Widgets
static lv_obj_t *lbl_city, *lbl_temp_big, *lbl_desc, *lbl_hum_ext;
static lv_obj_t *lbl_feel, *lbl_wind, *lbl_temp_int, *lbl_hum_int, *lbl_status;
static lv_obj_t *icon_canvas = NULL;
static uint8_t* icon_buf = NULL;            // PSRAM, formato ARGB (transparente)
// Noticias
static lv_obj_t* lbl_news[NEWS_COUNT];

// Previsión — fechas para nombres de día reales
static char g_fcst_date[FCST_DAYS][12] = {}; // "YYYY-MM-DD"

// ═══════════════════════════════════════════════════════════════
//  CH422G
// ═══════════════════════════════════════════════════════════════
static uint8_t ch422g_state = 0;
static void ch422g_write(uint8_t v) {
    ch422g_state = v;
    Wire.beginTransmission(CH422G_OUT_ADDR);  // 0x38 = registro de salida
    Wire.write(v);
    Wire.endTransmission();
}
static void ch422g_bit(uint8_t mask, bool on) {
    ch422g_write(on ? ch422g_state | mask : ch422g_state & ~mask);
}

// ═══════════════════════════════════════════════════════════════
//  GT911
// ═══════════════════════════════════════════════════════════════
static bool gt911_write_reg(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(val);
    return Wire.endTransmission() == 0;
}
static bool gt911_read(uint16_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(GT911_ADDR);
    Wire.write(reg >> 8); Wire.write(reg & 0xFF);
    if (Wire.endTransmission()) return false;
    Wire.requestFrom((uint8_t)GT911_ADDR, len);
    for (int i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
    return true;
}
static bool gt911_touch(int16_t* x, int16_t* y) {
    uint8_t st = 0;
    if (!gt911_read(GT911_STATUS_REG, &st, 1)) return false;
    if (!(st & 0x80)) return false;                  // buffer no listo
    uint8_t cnt = st & 0x0F;
    if (cnt == 0) {
        gt911_write_reg(GT911_STATUS_REG, 0x00);     // limpiar siempre
        return false;
    }
    uint8_t pt[6]; gt911_read(GT911_POINT1_REG, pt, 6);
    gt911_write_reg(GT911_STATUS_REG, 0x00);
    // El GT911 en este Waveshare envía X/Y en big-endian: pt[1]=X_H, pt[2]=X_L
    *x = (pt[1] << 8) | pt[2];
    *y = (pt[3] << 8) | pt[4];
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LVGL CALLBACKS
// ═══════════════════════════════════════════════════════════════
// Callback del panel RGB — se ejecuta en contexto ISR al terminar cada frame activo.
// En ese momento el DMA del panel deja de leer PSRAM → empieza el blanking (~5ms libre).
static bool IRAM_ATTR on_frame_done_cb(esp_lcd_panel_handle_t panel,
                                        esp_lcd_rgb_panel_event_data_t *edata,
                                        void *user_ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(vsync_sem, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
    // draw_bitmap corre siempre durante el blanking gracias a la sincronización en loop()
    esp_lcd_panel_draw_bitmap(lcd_panel, area->x1, area->y1, area->x2+1, area->y2+1, px);
    lv_disp_flush_ready(drv);
}
static void touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {

    int16_t x, y;
    if (gt911_touch(&x, &y)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x; data->point.y = y;
        if (!swipe_active) {
            swipe_start_x = x;
            swipe_active = true;
            Serial.printf("[TOUCH] Press x=%d\n", x);
        }
        swipe_last_x = x;
    } else {
        if (swipe_active) {
            int16_t dx = swipe_last_x - swipe_start_x;
            Serial.printf("[TOUCH] Release dx=%d\n", dx);
            if      (dx < -50) g_swipe = -1;
            else if (dx >  50) g_swipe =  1;
        }
        swipe_active = false;
        data->state = LV_INDEV_STATE_REL;
    }
}
static void tick_cb(void*) { lv_tick_inc(LVGL_TICK_MS); }

// ═══════════════════════════════════════════════════════════════
//  DISPLAY INIT
// ═══════════════════════════════════════════════════════════════
static void display_init() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    // CH422G: modo normal, todos los IO como salidas (A_IO=0, SLEEP=0)
    Wire.beginTransmission(CH422G_MODE_ADDR);
    Wire.write(0x00);
    Wire.endTransmission();
    delay(10);

    // Todos los pines a bajo (reset activo)
    ch422g_write(0x00); delay(50);
    // Sacar de reset LCD y touch
    ch422g_bit(CH422G_LCD_RST | CH422G_TP_RST, true); delay(120);

    // GT911: leer Product ID y resolución configurada
    delay(50);
    uint8_t pid[4] = {0};
    gt911_read(0x8140, pid, 4);
    Serial.printf("[GT911] ProductID: %c%c%c%c\n", pid[0], pid[1], pid[2], pid[3]);
    uint8_t res[4] = {0};
    gt911_read(0x8048, res, 4);
    uint16_t gt_xmax = res[0] | (res[1] << 8);
    uint16_t gt_ymax = res[2] | (res[3] << 8);
    Serial.printf("[GT911] Config: X_max=%d Y_max=%d\n", gt_xmax, gt_ymax);

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src                         = LCD_CLK_SRC_PLL160M;
    cfg.timings.pclk_hz                 = LCD_PCLK_HZ;
    cfg.timings.h_res                   = LCD_W;
    cfg.timings.v_res                   = LCD_H;
    // Timings correctos para panel 7" Waveshare 800x480
    cfg.timings.hsync_pulse_width       = 4;
    cfg.timings.hsync_back_porch        = 43;
    cfg.timings.hsync_front_porch       = 210;
    cfg.timings.vsync_pulse_width       = 4;
    cfg.timings.vsync_back_porch        = 12;
    cfg.timings.vsync_front_porch       = 22;
    cfg.timings.flags.pclk_active_neg   = true;
    cfg.data_width          = 16;
    cfg.sram_trans_align    = 4;
    cfg.psram_trans_align   = 64;
    cfg.hsync_gpio_num=LCD_HSYNC; cfg.vsync_gpio_num=LCD_VSYNC;
    cfg.de_gpio_num=LCD_DE;       cfg.pclk_gpio_num=LCD_PCLK;
    cfg.disp_gpio_num=GPIO_NUM_NC;
    cfg.flags.fb_in_psram = true;
    cfg.on_frame_trans_done = on_frame_done_cb;
    cfg.user_ctx = NULL;
    const int pins[16] = { LCD_B3,LCD_B4,LCD_B5,LCD_B6,LCD_B7,
                            LCD_G2,LCD_G3,LCD_G4,LCD_G5,LCD_G6,LCD_G7,
                            LCD_R3,LCD_R4,LCD_R5,LCD_R6,LCD_R7 };
    memcpy(cfg.data_gpio_nums, pins, sizeof(pins));
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    // LCD_RST y TP_RST en registro IO (0x38) ya activos
    // Backlight en registro OC (0x23) — dirección separada del CH422G
    Wire.beginTransmission(0x23);
    Wire.write(0xFF);
    Wire.endTransmission();
}

static void lvgl_hal_init() {
    lv_init();
    // Buffer en SRAM interna (no PSRAM) para evitar contención en el bus con el DMA del panel
    // 800×40 líneas = 64KB — cabe en SRAM, el panel DMA lee PSRAM sin interferencia
    lvgl_fb = (lv_color_t*)heap_caps_malloc(LCD_W * 10 * sizeof(lv_color_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(lvgl_fb);
    lv_disp_draw_buf_init(&disp_buf, lvgl_fb, NULL, LCD_W * 10);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res=LCD_W; disp_drv.ver_res=LCD_H;
    disp_drv.flush_cb=flush_cb; disp_drv.draw_buf=&disp_buf;
    disp_drv.user_data=lcd_panel;
    lv_disp_drv_register(&disp_drv);
    lv_indev_drv_init(&indev_drv);
    indev_drv.type=LV_INDEV_TYPE_POINTER; indev_drv.read_cb=touch_cb;
    lv_indev_drv_register(&indev_drv);
    esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback=tick_cb,.name="lvgl"};
    esp_timer_create(&ta,&t); esp_timer_start_periodic(t, LVGL_TICK_MS*1000ULL);
}

// ═══════════════════════════════════════════════════════════════
//  ICONOS DE TIEMPO  (canvas 120×120 en PSRAM, fondo transparente)
// ═══════════════════════════════════════════════════════════════
#define ICON_W 120
#define ICON_H 120

// Limpia el canvas dejando fondo transparente
static void icon_clear(lv_obj_t* cv) {
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
}

// Nube con 5 círculos superpuestos — más natural que rectángulos
static void draw_cloud_shape(lv_obj_t* cv, lv_color_t col, int ox, int oy) {
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = col; r.border_width = 0; r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(cv, ox,    oy+20, 78, 28, &r); // base
    lv_canvas_draw_rect(cv, ox+4,  oy+6,  28, 28, &r); // bola izq
    lv_canvas_draw_rect(cv, ox+22, oy,    34, 34, &r); // bola centro (grande)
    lv_canvas_draw_rect(cv, ox+48, oy+8,  26, 26, &r); // bola derecha
    lv_canvas_draw_rect(cv, ox+64, oy+16, 18, 18, &r); // bola extremo
}

static void draw_icon_sun(lv_obj_t* cv) {
    icon_clear(cv);
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0xFFB300); ln.width = 5;
    ln.round_start = true; ln.round_end = true;
    lv_point_t rays[8][2] = {
        {{60, 5},{60,22}}, {{60,98},{60,115}},
        {{ 5,60},{22,60}}, {{98,60},{115,60}},
        {{22,22},{33,33}}, {{87,87},{98,98}},
        {{98,22},{87,33}}, {{22,98},{33,87}}
    };
    for (int i = 0; i < 8; i++) lv_canvas_draw_line(cv, rays[i], 2, &ln);
    // Halo exterior
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.border_width = 0; r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(0xFFE082);
    lv_canvas_draw_rect(cv, 28, 28, 64, 64, &r);
    // Disco solar
    r.bg_color = lv_color_hex(0xFFD600);
    lv_canvas_draw_rect(cv, 33, 33, 54, 54, &r);
    // Brillo interior
    r.bg_color = lv_color_hex(0xFFF9C4);
    lv_canvas_draw_rect(cv, 42, 39, 22, 18, &r);
}

static void draw_icon_cloud(lv_obj_t* cv) {
    icon_clear(cv);
    // Sombra
    draw_cloud_shape(cv, lv_color_hex(0x8FA8C0), 12, 38);
    // Nube principal
    draw_cloud_shape(cv, lv_color_hex(0xCFDDED), 8, 34);
    // Brillo
    draw_cloud_shape(cv, lv_color_hex(0xE8F4FD), 8, 30);
}

static void draw_icon_partly(lv_obj_t* cv) {
    icon_clear(cv);
    // Sol de fondo (pequeño)
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0xFFB300); ln.width = 4;
    ln.round_start = true; ln.round_end = true;
    lv_point_t rays[6][2] = {
        {{30,2},{30,12}},  {{30,56},{30,66}},
        {{ 2,30},{12,30}}, {{48,30},{58,30}},
        {{ 9, 9},{17,17}}, {{43, 9},{35,17}}
    };
    for (int i = 0; i < 6; i++) lv_canvas_draw_line(cv, rays[i], 2, &ln);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.border_width = 0; r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(0xFFD600);
    lv_canvas_draw_rect(cv, 14, 14, 32, 32, &r);
    // Nube encima-derecha
    draw_cloud_shape(cv, lv_color_hex(0x8FA8C0), 32, 52);
    draw_cloud_shape(cv, lv_color_hex(0xCFDDED), 28, 48);
    draw_cloud_shape(cv, lv_color_hex(0xE8F4FD), 28, 44);
}

static void draw_icon_rain(lv_obj_t* cv) {
    icon_clear(cv);
    draw_cloud_shape(cv, lv_color_hex(0x607D8B), 8, 8);
    draw_cloud_shape(cv, lv_color_hex(0x78909C), 4, 4);
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0x29B6F6); ln.width = 3;
    ln.round_start = true; ln.round_end = true;
    // Lluvia diagonal
    lv_point_t drops[6][2] = {
        {{18,68},{12,86}}, {{32,68},{26,86}},
        {{46,68},{40,86}}, {{60,68},{54,86}},
        {{74,68},{68,86}}, {{25,82},{19,100}}
    };
    for (int i = 0; i < 6; i++) lv_canvas_draw_line(cv, drops[i], 2, &ln);
}

static void draw_icon_storm(lv_obj_t* cv) {
    icon_clear(cv);
    draw_cloud_shape(cv, lv_color_hex(0x455A64), 8, 5);
    draw_cloud_shape(cv, lv_color_hex(0x546E7A), 4, 1);
    // Rayo en Z con relleno
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0xFFEE58); ln.width = 7;
    ln.round_start = true; ln.round_end = true;
    lv_point_t b1[2] = {{58,56},{44,80}};
    lv_point_t b2[2] = {{44,80},{56,80}};
    lv_point_t b3[2] = {{56,80},{40,108}};
    lv_canvas_draw_line(cv, b1, 2, &ln);
    lv_canvas_draw_line(cv, b2, 2, &ln);
    lv_canvas_draw_line(cv, b3, 2, &ln);
    // Rayo delgado encima (brillo)
    ln.color = lv_color_hex(0xFFFDE7); ln.width = 2;
    lv_point_t h1[2] = {{57,58},{44,80}};
    lv_canvas_draw_line(cv, h1, 2, &ln);
}

// Dibuja copo de nieve centrado en (cx, cy)
static void draw_snowflake(lv_obj_t* cv, int cx, int cy, int r) {
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(0xE3F2FD); ln.width = 2;
    ln.round_start = true; ln.round_end = true;
    lv_point_t h[2] = {{cx-r, cy},   {cx+r, cy}};
    lv_point_t v[2] = {{cx,   cy-r}, {cx,   cy+r}};
    lv_point_t d1[2]= {{cx-r*7/10, cy-r*7/10}, {cx+r*7/10, cy+r*7/10}};
    lv_point_t d2[2]= {{cx+r*7/10, cy-r*7/10}, {cx-r*7/10, cy+r*7/10}};
    lv_canvas_draw_line(cv, h,  2, &ln);
    lv_canvas_draw_line(cv, v,  2, &ln);
    lv_canvas_draw_line(cv, d1, 2, &ln);
    lv_canvas_draw_line(cv, d2, 2, &ln);
    // Punto central
    lv_draw_rect_dsc_t rr; lv_draw_rect_dsc_init(&rr);
    rr.bg_color = lv_color_hex(0xFFFFFF); rr.radius = LV_RADIUS_CIRCLE; rr.border_width = 0;
    lv_canvas_draw_rect(cv, cx-2, cy-2, 5, 5, &rr);
}

static void draw_icon_snow(lv_obj_t* cv) {
    icon_clear(cv);
    draw_cloud_shape(cv, lv_color_hex(0x78909C), 8, 5);
    draw_cloud_shape(cv, lv_color_hex(0x90A4AE), 4, 1);
    draw_snowflake(cv, 22, 80, 9);
    draw_snowflake(cv, 46, 88, 9);
    draw_snowflake(cv, 70, 80, 9);
    draw_snowflake(cv, 34, 100, 7);
    draw_snowflake(cv, 58, 100, 7);
}

static void draw_icon_fog(lv_obj_t* cv) {
    icon_clear(cv);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.border_width = 0; r.radius = 8;
    // 4 bandas de niebla con distintos tonos
    uint32_t cols[4] = {0x78909C, 0x90A4AE, 0x78909C, 0x607D8B};
    int ys[4]  = {18, 40, 62, 84};
    int xs[4]  = {16,  8, 18, 28};
    int ws[4]  = {88, 104, 84, 70};
    for (int i = 0; i < 4; i++) {
        r.bg_color = lv_color_hex(cols[i]);
        lv_canvas_draw_rect(cv, xs[i], ys[i], ws[i], 14, &r);
    }
}

static void update_weather_icon_canvas(lv_obj_t* cv, int wmo) {
    if (!cv) return;
    if      (wmo == 0 || wmo == 1)              draw_icon_sun(cv);
    else if (wmo == 2)                          draw_icon_partly(cv);
    else if (wmo == 3)                          draw_icon_cloud(cv);
    else if (wmo == 45 || wmo == 48)            draw_icon_fog(cv);
    else if (wmo >= 51 && wmo <= 67)            draw_icon_rain(cv);
    else if (wmo >= 71 && wmo <= 77)            draw_icon_snow(cv);
    else if (wmo >= 80 && wmo <= 82)            draw_icon_rain(cv);
    else if (wmo >= 95 && wmo <= 99)            draw_icon_storm(cv);
    else                                        draw_icon_cloud(cv);
}
static void update_weather_icon(int wmo) { update_weather_icon_canvas(icon_canvas, wmo); }

// ═══════════════════════════════════════════════════════════════
//  DESCRIPCION WMO
// ═══════════════════════════════════════════════════════════════
static String wmo_desc(int c) {
    switch(c) {
        case 0:            return "Despejado";
        case 1:            return "Casi despejado";
        case 2:            return "Parcialmente nublado";
        case 3:            return "Nublado";
        case 45: case 48:  return "Niebla";
        case 51:           return "Llovizna ligera";
        case 53:           return "Llovizna";
        case 55:           return "Llovizna densa";
        case 61:           return "Lluvia ligera";
        case 63:           return "Lluvia moderada";
        case 65:           return "Lluvia intensa";
        case 71:           return "Nieve ligera";
        case 73:           return "Nieve moderada";
        case 75:           return "Nieve intensa";
        case 80:           return "Chubascos";
        case 81:           return "Chubascos fuertes";
        case 82:           return "Chubascos violentos";
        case 95:           return "Tormenta";
        case 96: case 99:  return "Tormenta con granizo";
        default:           return "Variable";
    }
}

// Devuelve el nombre del día de la semana a partir de "YYYY-MM-DD"
static const char* day_of_week(const char* date) {
    if (!date || date[0] == '\0') return "---";
    int y = (date[0]-'0')*1000 + (date[1]-'0')*100 + (date[2]-'0')*10 + (date[3]-'0');
    int m = (date[5]-'0')*10 + (date[6]-'0');
    int d = (date[8]-'0')*10 + (date[9]-'0');
    // Algoritmo de Tomohiko Sakamoto (0=Dom, 1=Lun, ..., 6=Sab)
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    static const char* names[] = {"Dom","Lun","Mar","Mie","Jue","Vie","Sab"};
    return names[dow];
}

// ═══════════════════════════════════════════════════════════════
//  INTERFAZ PRINCIPAL
//
//  ┌──────────────────────┬──────────────────────────┐
//  │ GPS VINAROS  25.3°C  │  NOTICIAS                │
//  │ Nublado    [ICONO]   │  - Titular 1...          │
//  │ Hum: 72%             │  - Titular 2...          │
//  │ Sens: 23°C           │  - Titular 3...          │
//  │ Viento: 15 km/h      │  - Titular 4...          │
//  │ ──────────────────── │  - Titular 5...          │
//  │ Hoy  Lun  Mar  Mie   │                          │
//  │ 28°  26°  24°  20°   │                          │
//  ├──────────────────────┴──────────────────────────┤
//  │  HOME INTERIOR  21.3°C     TINT 64%             │
//  └─────────────────────────────────────────────────┘
// ═══════════════════════════════════════════════════════════════
#define SPLIT_X 397   // divisor vertical izq/der

static void ui_create() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D1B2A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── BLOQUE IZQUIERDO ─────────────────────────────────────────────────────

    // Ciudad
    lbl_city = lv_label_create(scr);
    lv_label_set_text(lbl_city, LV_SYMBOL_GPS "  VINAROS");
    lv_obj_set_style_text_font(lbl_city, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_city, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(lbl_city, 18, 15);

    // Linea bajo ciudad
    lv_obj_t* line = lv_obj_create(scr);
    lv_obj_set_size(line, 372, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x2A4A6B), LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_pos(line, 18, 50);

    // Temperatura exterior grande
    lbl_temp_big = lv_label_create(scr);
    lv_label_set_text(lbl_temp_big, "--.-°C");
    lv_obj_set_style_text_font(lbl_temp_big, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_temp_big, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(lbl_temp_big, 18, 57);

    // Icono del tiempo (120x120) al lado derecho de la temp, mismo nivel
    icon_buf = (uint8_t*)heap_caps_malloc(
        ICON_W * ICON_H * LV_IMG_PX_SIZE_ALPHA_BYTE, MALLOC_CAP_SPIRAM);
    assert(icon_buf);
    icon_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(icon_canvas, icon_buf, ICON_W, ICON_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_set_style_bg_opa(icon_canvas, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon_canvas, 0, LV_PART_MAIN);
    lv_obj_set_pos(icon_canvas, 248, 182);
    draw_icon_cloud(icon_canvas);  // placeholder

    // Descripcion del tiempo
    lbl_desc = lv_label_create(scr);
    lv_label_set_text(lbl_desc, "Cargando...");
    lv_obj_set_style_text_font(lbl_desc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_desc, lv_color_hex(0xFFCC80), LV_PART_MAIN);
    lv_obj_set_width(lbl_desc, 225);
    lv_label_set_long_mode(lbl_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_desc, 18, 122);

    // Humedad exterior
    lbl_hum_ext = lv_label_create(scr);
    lv_label_set_text(lbl_hum_ext, LV_SYMBOL_TINT "  Humedad:  --%");
    lv_obj_set_style_text_font(lbl_hum_ext, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hum_ext, lv_color_hex(0x7EB8D4), LV_PART_MAIN);
    lv_obj_set_pos(lbl_hum_ext, 18, 210);

    // Sensacion termica
    lbl_feel = lv_label_create(scr);
    lv_label_set_text(lbl_feel, LV_SYMBOL_CHARGE "  Sens.:  -- C");
    lv_obj_set_style_text_font(lbl_feel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_feel, lv_color_hex(0x7EB8D4), LV_PART_MAIN);
    lv_obj_set_pos(lbl_feel, 18, 233);

    // Viento
    lbl_wind = lv_label_create(scr);
    lv_label_set_text(lbl_wind, LV_SYMBOL_LOOP "  Viento:  -- km/h");
    lv_obj_set_style_text_font(lbl_wind, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wind, lv_color_hex(0x7EB8D4), LV_PART_MAIN);
    lv_obj_set_pos(lbl_wind, 18, 256);

    // ── DIVISOR VERTICAL ─────────────────────────────────────────────────────
    lv_obj_t* vdiv = lv_obj_create(scr);
    lv_obj_set_size(vdiv, 2, 388);
    lv_obj_set_style_bg_color(vdiv, lv_color_hex(0x1E3A52), LV_PART_MAIN);
    lv_obj_set_style_border_width(vdiv, 0, LV_PART_MAIN);
    lv_obj_set_pos(vdiv, SPLIT_X, 0);

    // ── BLOQUE DERECHO — NOTICIAS ─────────────────────────────────────────────
    lv_obj_t* news_title = lv_label_create(scr);
    lv_label_set_text(news_title, LV_SYMBOL_LIST "  NOTICIAS");
    lv_obj_set_style_text_font(news_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(news_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(news_title, SPLIT_X + 15, 15);

    lv_obj_t* news_line = lv_obj_create(scr);
    lv_obj_set_size(news_line, 385, 2);
    lv_obj_set_style_bg_color(news_line, lv_color_hex(0x2A4A6B), LV_PART_MAIN);
    lv_obj_set_style_border_width(news_line, 0, LV_PART_MAIN);
    lv_obj_set_pos(news_line, SPLIT_X + 5, 50);

    for (int i = 0; i < NEWS_COUNT; i++) {
        lbl_news[i] = lv_label_create(scr);
        lv_label_set_text(lbl_news[i], "");
        lv_obj_set_style_text_font(lbl_news[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl_news[i], lv_color_hex(0xCFD8DC), LV_PART_MAIN);
        lv_obj_set_size(lbl_news[i], 375, 56);
        lv_label_set_long_mode(lbl_news[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(lbl_news[i], SPLIT_X + 10, 58 + i * 66);
    }

    // ── LINEA SEPARADORA INFERIOR ────────────────────────────────────────────
    lv_obj_t* sep = lv_obj_create(scr);
    lv_obj_set_size(sep, 800, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x1E3A52), LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_align(sep, LV_ALIGN_BOTTOM_MID, 0, -92);

    // ── BARRA INTERIOR (parte inferior) ─────────────────────────────────────────
    // Fondo barra interior
    lv_obj_t* bar_int = lv_obj_create(scr);
    lv_obj_set_size(bar_int, 800, 90);
    lv_obj_set_style_bg_color(bar_int, lv_color_hex(0x0A1520), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_int, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_int, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_int, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_int, 0, LV_PART_MAIN);
    lv_obj_align(bar_int, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar_int, LV_OBJ_FLAG_SCROLLABLE);

    // Titulo + temperatura + humedad juntos en la izquierda
    lv_obj_t* lbl_int_title = lv_label_create(bar_int);
    lv_label_set_text(lbl_int_title, LV_SYMBOL_HOME "  INTERIOR");
    lv_obj_set_style_text_font(lbl_int_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_int_title, lv_color_hex(0x4FC3F7), LV_PART_MAIN);
    lv_obj_set_pos(lbl_int_title, 30, 10);

    // Temperatura interior — alineada bajo el titulo
    lbl_temp_int = lv_label_create(bar_int);
    lv_label_set_text(lbl_temp_int, "--.-°C");
    lv_obj_set_size(lbl_temp_int, 130, 36);
    lv_obj_set_style_text_font(lbl_temp_int, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_temp_int, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(lbl_temp_int, 30, 40);

    // Humedad interior — justo a la derecha de la temperatura
    lbl_hum_int = lv_label_create(bar_int);
    lv_label_set_text(lbl_hum_int, LV_SYMBOL_TINT "  --%");
    lv_obj_set_size(lbl_hum_int, 150, 36);
    lv_obj_set_style_text_font(lbl_hum_int, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_hum_int, lv_color_hex(0x7EB8D4), LV_PART_MAIN);
    lv_obj_set_pos(lbl_hum_int, 165, 38);

    // Estado / ultima actualizacion
    lbl_status = lv_label_create(bar_int);
    lv_label_set_text(lbl_status, "Iniciando...");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x2A4050), LV_PART_MAIN);
    lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -30, 0);
}

// ── Actualizar solo labels interiores (DHT) ───────────────────
static void ui_update_int() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f°C", g_tempInt);
    lv_label_set_text(lbl_temp_int, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_TINT "  %d%%", (int)g_humInt);
    lv_label_set_text(lbl_hum_int, buf);
}

// ── Actualizar labels exteriores (API) ───────────────────────
static void ui_update_ext() {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.1f°C", g_tempExt);
    lv_label_set_text(lbl_temp_big, buf);
    lv_label_set_text(lbl_desc, g_descExt.c_str());
    snprintf(buf, sizeof(buf), LV_SYMBOL_TINT "  Humedad:  %d%%", g_humExt);
    lv_label_set_text(lbl_hum_ext, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE "  Sens.:  %.1f C", g_feelExt);
    lv_label_set_text(lbl_feel, buf);
    snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP "  Viento:  %.0f km/h", g_windExt);
    lv_label_set_text(lbl_wind, buf);
    uint32_t col = (g_tempExt < 10) ? 0x4FC3F7 :
                   (g_tempExt < 18) ? 0x81D4FA :
                   (g_tempExt < 24) ? 0xA5D6A7 :
                   (g_tempExt < 28) ? 0xFFCC80 : 0xEF9A9A;
    lv_obj_set_style_text_color(lbl_temp_big, lv_color_hex(col), LV_PART_MAIN);
    update_weather_icon(g_wmoCode);
    lv_label_set_text(lbl_status, LV_SYMBOL_OK "  Actualizado");
}

// ── Actualizar panel de noticias ─────────────────────────────
static void ui_update_news() {
    for (int i = 0; i < NEWS_COUNT; i++) {
        if (g_news[i].length() > 0)
            lv_label_set_text(lbl_news[i], g_news[i].c_str());
    }
}

// ═══════════════════════════════════════════════════════════════
//  PANTALLA PREVISIÓN
// ═══════════════════════════════════════════════════════════════
static lv_obj_t*   fcst_lbl_day[FCST_DAYS];
static lv_obj_t*   fcst_lbl_desc[FCST_DAYS];
static lv_obj_t*   fcst_lbl_temp[FCST_DAYS];
static lv_obj_t* fcst_canvas[FCST_DAYS];
static uint8_t*  fcst_icon_buf[FCST_DAYS]; // PSRAM, formato ARGB

static void forecast_screen_create() {
    scr_forecast = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr_forecast, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr_forecast, lv_color_hex(0x0D1B2A), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr_forecast, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_forecast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr_forecast, LV_OBJ_FLAG_SCROLLABLE);

    // Cabecera
    lv_obj_t* title = lv_label_create(scr_forecast);
    lv_label_set_text(title, LV_SYMBOL_GPS "  PREVISION 4 DIAS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, 30, 18);

    lv_obj_t* hint = lv_label_create(scr_forecast);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " desliza para volver");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x2A4050), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -20, 26);

    lv_obj_t* sep = lv_obj_create(scr_forecast);
    lv_obj_set_size(sep, 760, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x1E3A52), LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_pos(sep, 20, 60);

    // 4 tarjetas — los nombres de día se rellenan en forecast_screen_update()
    for (int i = 0; i < FCST_DAYS; i++) {
        int cx = 10 + i * 197;

        lv_obj_t* card = lv_obj_create(scr_forecast);
        lv_obj_set_size(card, 193, 405);
        lv_obj_set_pos(card, cx, 68);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x091525), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1E3A52), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Nombre del día (rellenado desde forecast_screen_update con día real)
        fcst_lbl_day[i] = lv_label_create(card);
        lv_label_set_text(fcst_lbl_day[i], "---");
        lv_obj_set_style_text_font(fcst_lbl_day[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(fcst_lbl_day[i], lv_color_hex(0xFFCC80), LV_PART_MAIN);
        lv_obj_align(fcst_lbl_day[i], LV_ALIGN_TOP_MID, 0, 12);

        // Icono — canvas ARGB con fondo transparente
        fcst_icon_buf[i] = (uint8_t*)heap_caps_malloc(
            ICON_W * ICON_H * LV_IMG_PX_SIZE_ALPHA_BYTE, MALLOC_CAP_SPIRAM);
        assert(fcst_icon_buf[i]);
        fcst_canvas[i] = lv_canvas_create(card);
        lv_canvas_set_buffer(fcst_canvas[i], fcst_icon_buf[i],
                             ICON_W, ICON_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_obj_set_style_bg_opa(fcst_canvas[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(fcst_canvas[i], 0, LV_PART_MAIN);
        lv_obj_align(fcst_canvas[i], LV_ALIGN_TOP_MID, 0, 50);
        icon_clear(fcst_canvas[i]);

        // Descripción
        fcst_lbl_desc[i] = lv_label_create(card);
        lv_label_set_text(fcst_lbl_desc[i], "---");
        lv_obj_set_style_text_font(fcst_lbl_desc[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(fcst_lbl_desc[i], lv_color_hex(0x7EB8D4), LV_PART_MAIN);
        lv_obj_set_style_text_align(fcst_lbl_desc[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fcst_lbl_desc[i], 180);
        lv_obj_align(fcst_lbl_desc[i], LV_ALIGN_TOP_MID, 0, 180);

        // Temp máx/mín
        fcst_lbl_temp[i] = lv_label_create(card);
        lv_label_set_text(fcst_lbl_temp[i], "--° / --°");
        lv_obj_set_style_text_font(fcst_lbl_temp[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(fcst_lbl_temp[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_align(fcst_lbl_temp[i], LV_ALIGN_TOP_MID, 0, 215);
    }
}

static void forecast_screen_update() {
    for (int i = 0; i < FCST_DAYS; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.0f° / %.0f°", g_fcst_max[i], g_fcst_min[i]);
        lv_label_set_text(fcst_lbl_temp[i], buf);
        lv_label_set_text(fcst_lbl_desc[i], wmo_desc(g_fcst_wmo[i]).c_str());
        // Nombre del día: "Hoy" para el primero, día de la semana para el resto
        if (i == 0) lv_label_set_text(fcst_lbl_day[i], "Hoy");
        else        lv_label_set_text(fcst_lbl_day[i], day_of_week(g_fcst_date[i]));
        uint32_t col = (g_fcst_max[i] < 10) ? 0x4FC3F7 :
                       (g_fcst_max[i] < 18) ? 0x81D4FA :
                       (g_fcst_max[i] < 24) ? 0xA5D6A7 :
                       (g_fcst_max[i] < 28) ? 0xFFCC80 : 0xEF9A9A;
        lv_obj_set_style_text_color(fcst_lbl_temp[i], lv_color_hex(col), LV_PART_MAIN);
        update_weather_icon_canvas(fcst_canvas[i], g_fcst_wmo[i]);
    }
}

// ═══════════════════════════════════════════════════════════════
//  DHT11 VIA RMT (sin bloquear interrupciones del núcleo)
//  El periférico RMT captura la señal DHT11 por hardware.
//  Nunca llama a noInterrupts()/portDISABLE_INTERRUPTS().
// ═══════════════════════════════════════════════════════════════
#define DHT_RMT_CHANNEL  RMT_CHANNEL_4   // ESP32-S3: canales RX son 4-7
static RingbufHandle_t dht_rb = NULL;

static void dht11_rmt_init() {
    rmt_config_t cfg = {};
    cfg.rmt_mode                      = RMT_MODE_RX;
    cfg.channel                       = DHT_RMT_CHANNEL;
    cfg.gpio_num                      = (gpio_num_t)DHT_PIN;
    cfg.clk_div                       = 80;    // 80MHz APB / 80 = 1µs/tick
    cfg.mem_block_num                 = 1;
    cfg.rx_config.filter_en           = true;
    cfg.rx_config.filter_ticks_thresh = 5;     // ignorar glitches < 5µs
    cfg.rx_config.idle_threshold      = 2000;  // 2ms de idle = fin de trama
    ESP_ERROR_CHECK(rmt_config(&cfg));
    ESP_ERROR_CHECK(rmt_driver_install(DHT_RMT_CHANNEL, 2048, 0));
    rmt_get_ringbuf_handle(DHT_RMT_CHANNEL, &dht_rb);
}

static bool dht11_rmt_read(float* temp, float* hum) {
    // ── 1. Señal de inicio: GPIO LOW 20ms sin deshabilitar interrupciones ──
    gpio_set_direction((gpio_num_t)DHT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)DHT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)DHT_PIN, 1);
    gpio_set_direction((gpio_num_t)DHT_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en((gpio_num_t)DHT_PIN);

    // ── 2. Vaciar ringbuffer e iniciar recepción ──────────────────────────
    { size_t sz; void* p; while ((p = xRingbufferReceive(dht_rb, &sz, 0)) != NULL) vRingbufferReturnItem(dht_rb, p); }
    rmt_rx_start(DHT_RMT_CHANNEL, true);

    // ── 3. Esperar trama completa (idle 2ms indica fin) ───────────────────
    size_t rx_size = 0;
    rmt_item32_t* items = (rmt_item32_t*)xRingbufferReceive(dht_rb, &rx_size, pdMS_TO_TICKS(150));
    rmt_rx_stop(DHT_RMT_CHANNEL);

    if (!items) { Serial.println("[DHT] Timeout"); return false; }
    int n = (int)(rx_size / sizeof(rmt_item32_t));

    // ── 4. Localizar primer bit de datos ──────────────────────────────────
    // Modo normal:   item0: l0=0 (preamble LOW ~50µs), bit en duration1
    // Modo invertido: RMT arranca en HIGH → item0: l0=1 d0~80µs (ACK HIGH del sensor)
    //                 datos en items[1..40]: l0=1 d0=bit_dur, l1=0 d1=preamble
    int start = -1;
    bool inverted = false;
    for (int i = 0; i < n; i++) {
        uint16_t d0 = items[i].duration0;
        if (items[i].level0 == 0 && d0 > 25 && d0 < 75) { start = i; break; }
        if (items[i].level0 == 1 && d0 > 60 && d0 < 120 && (i + 40) <= n) {
            start = i + 1; inverted = true; break;
        }
    }
    if (start < 0 || n - start < 40) {
        Serial.printf("[DHT] Layout inesperado: n=%d start=%d\n", n, start);
        vRingbufferReturnItem(dht_rb, items);
        return false;
    }

    // ── 5. Decodificar 40 bits ────────────────────────────────────────────
    uint8_t data[5] = {0};
    for (int i = 0; i < 40; i++) {
        uint16_t bit_dur = inverted ? items[start + i].duration0
                                    : items[start + i].duration1;
        if (bit_dur > 40)   // HIGH > 40µs → bit 1
            data[i / 8] |= (1 << (7 - (i % 8)));
    }
    vRingbufferReturnItem(dht_rb, items);

    // ── 6. Checksum ───────────────────────────────────────────────────────
    uint8_t ck = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
    if (ck != data[4]) { Serial.printf("[DHT] CRC: %02X!=%02X\n", ck, data[4]); return false; }

    *hum  = (float)data[0] + data[1] * 0.1f;
    *temp = (float)data[2] + data[3] * 0.1f;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  TAREAS
// ═══════════════════════════════════════════════════════════════
static void task_dht(void*) {
    dht11_rmt_init();
    vTaskDelay(pdMS_TO_TICKS(2000));
    while (true) {
        float h = 0, t = 0;
        if (dht11_rmt_read(&t, &h)) {
            xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
            g_tempInt = t; g_humInt = h; g_newIntData = true;
            xSemaphoreGive(lvgl_mutex);
            Serial.printf("[DHT11] %.1fC  %.0f%%\n", t, h);
        } else {
            Serial.println("[DHT11] Lectura fallida");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// Convierte caracteres UTF-8 acentuados a ASCII para la fuente Montserrat
static String strip_accents(const String& s) {
    String r; r.reserve(s.length());
    for (size_t i = 0; i < s.length(); ) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { r += (char)c; i++; continue; }
        if ((c & 0xE0) == 0xC0 && i + 1 < s.length()) {
            uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)s[i+1] & 0x3F);
            switch (cp) {
                case 0xE1: r += 'a'; break;  // á
                case 0xE9: r += 'e'; break;  // é
                case 0xED: r += 'i'; break;  // í
                case 0xF3: r += 'o'; break;  // ó
                case 0xFA: r += 'u'; break;  // ú
                case 0xFC: r += 'u'; break;  // ü
                case 0xF1: r += 'n'; break;  // ñ
                case 0xC1: r += 'A'; break;  // Á
                case 0xC9: r += 'E'; break;  // É
                case 0xCD: r += 'I'; break;  // Í
                case 0xD3: r += 'O'; break;  // Ó
                case 0xDA: r += 'U'; break;  // Ú
                case 0xD1: r += 'N'; break;  // Ñ
                case 0xBF: r += '?'; break;  // ¿
                case 0xAB: r += '"'; break;  // «
                case 0xBB: r += '"'; break;  // »
                default:   r += '?'; break;
            }
            i += 2; continue;
        }
        // 3 o 4 bytes UTF-8 — omitir
        if ((c & 0xF0) == 0xE0) { i += 3; r += '?'; continue; }
        if ((c & 0xF8) == 0xF0) { i += 4; r += '?'; continue; }
        i++;
    }
    return r;
}

// Extrae hasta max_count titulares de un buffer RSS
static int parse_rss_titles(const char* xml, String* out, int max_count) {
    int count = 0;
    const char* p = xml;
    while (count < max_count) {
        const char* item = strstr(p, "<item>");
        if (!item) break;
        p = item + 6;
        const char* item_end = strstr(p, "</item>");
        const char* ts = strstr(p, "<title>");
        if (!ts || (item_end && ts > item_end)) { p = item_end ? item_end + 7 : p + 1; continue; }
        ts += 7;
        const char* te = strstr(ts, "</title>");
        if (!te) break;
        int len = te - ts;
        if (len > 200) len = 200;
        char tmp[201]; strncpy(tmp, ts, len); tmp[len] = 0;
        String title = String(tmp);
        title.trim();
        if (title.startsWith("<![CDATA[")) {
            title = title.substring(9);
            int ci = title.lastIndexOf("]]>");
            if (ci >= 0) title = title.substring(0, ci);
            title.trim();
        }
        out[count++] = strip_accents(title);
        p = item_end ? item_end + 7 : te;
    }
    return count;
}

static void task_news(void*) {
    // Esperar WiFi
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(3000));  // pequeño retardo inicial

    while (true) {
        HTTPClient http;
        http.setTimeout(15000);
        http.begin(NEWS_URL);
        int code = http.GET();
        if (code == 200) {
            // Leer respuesta en PSRAM para no agotar SRAM
            size_t buf_sz = 48 * 1024;
            char* buf = (char*)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
            if (buf) {
                WiFiClient* stream = http.getStreamPtr();
                int n = stream->readBytes(buf, buf_sz - 1);
                buf[n] = 0;
                String titles[NEWS_COUNT];
                int cnt = parse_rss_titles(buf, titles, NEWS_COUNT);
                heap_caps_free(buf);
                xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
                for (int i = 0; i < NEWS_COUNT; i++)
                    g_news[i] = (i < cnt) ? titles[i] : String("");
                g_newNewsData = true;
                xSemaphoreGive(lvgl_mutex);
                Serial.printf("[NEWS] %d titulares\n", cnt);
            } else {
                Serial.println("[NEWS] Sin memoria PSRAM");
            }
        } else {
            Serial.printf("[NEWS] HTTP %d\n", code);
        }
        http.end();
        vTaskDelay(pdMS_TO_TICKS(NEWS_REFRESH_MS));
    }
}

static void task_api(void*) {
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
    while (true) {
        HTTPClient http; http.begin(OWM_URL); http.setTimeout(10000);
        if (http.GET() == 200) {
            DynamicJsonDocument doc(3072);
            if (!deserializeJson(doc, http.getString())) {
                xSemaphoreTake(lvgl_mutex, portMAX_DELAY);
                g_tempExt = doc["current"]["temperature_2m"].as<float>();
                g_humExt  = doc["current"]["relative_humidity_2m"].as<int>();
                g_feelExt = doc["current"]["apparent_temperature"].as<float>();
                g_windExt = doc["current"]["wind_speed_10m"].as<float>();
                g_wmoCode = doc["current"]["weather_code"].as<int>();
                g_descExt = wmo_desc(g_wmoCode);
                g_newExtData = true;
                auto daily = doc["daily"];
                for (int i = 0; i < FCST_DAYS; i++) {
                    g_fcst_max[i] = daily["temperature_2m_max"][i].as<float>();
                    g_fcst_min[i] = daily["temperature_2m_min"][i].as<float>();
                    g_fcst_wmo[i] = daily["weather_code"][i].as<int>();
                    const char* d = daily["time"][i];
                    if (d) strncpy(g_fcst_date[i], d, 10);
                }
                g_newFcstData = true;
                xSemaphoreGive(lvgl_mutex);
                Serial.printf("[API] %.1fC  %d%%  %s\n", g_tempExt, g_humExt, g_descExt.c_str());
            }
        } else { Serial.println("[API] Error"); }
        http.end();
        vTaskDelay(pdMS_TO_TICKS(300000));
    }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    lvgl_mutex = xSemaphoreCreateMutex();
    vsync_sem  = xSemaphoreCreateBinary();
    display_init();
    lvgl_hal_init();
    ui_create();
    scr_main = lv_scr_act();

    lv_label_set_text(lbl_status, LV_SYMBOL_WIFI "  Conectando...");
    lv_task_handler();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Conectando");
    for (int i=0; i<30 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] OK " + WiFi.localIP().toString());
        String ip = WiFi.localIP().toString();
        lv_label_set_text_fmt(lbl_status, LV_SYMBOL_OK "  %s  |  OTA: /update", ip.c_str());

        // ── OTA: actualización por navegador → http://IP/update ──────
        ota_server.on("/", []() {
            ota_server.send(200, "text/plain",
                "Estacion Meteorologica OK\nOTA: /update");
        });
        ElegantOTA.begin(&ota_server, "admin", "ota1234");  // user/pass OTA
        ota_server.begin();
        Serial.println("[OTA] Disponible en http://" +
                       WiFi.localIP().toString() + "/update");
    } else {
        lv_label_set_text(lbl_status, LV_SYMBOL_WARNING "  Sin WiFi");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xF44336), LV_PART_MAIN);
    }

    forecast_screen_create();  // crear aquí para evitar lag en el primer swipe

    xTaskCreatePinnedToCore(task_dht,  "dht",  4096,  NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(task_api,  "api",  8192,  NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(task_news, "news", 12288, NULL, 1, NULL, 0);
}

void loop() {
    xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(20));

    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (g_newIntData)  { ui_update_int();          g_newIntData  = false; }
        if (g_newExtData)  { ui_update_ext();          g_newExtData  = false; }
        if (g_newFcstData) { forecast_screen_update(); g_newFcstData = false; }
        if (g_newNewsData) { ui_update_news();         g_newNewsData = false; }
        int8_t sw = g_swipe; g_swipe = 0;
        xSemaphoreGive(lvgl_mutex);

        if (sw != 0) {
            Serial.printf("[SWIPE] sw=%d cur=%d\n", sw, g_cur_screen);
            if (sw < 0 && g_cur_screen == 0) {
                lv_scr_load_anim(scr_forecast, LV_SCR_LOAD_ANIM_MOVE_LEFT, 400, 0, false);
                g_cur_screen = 1;
            } else if (sw > 0 && g_cur_screen == 1) {
                lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 400, 0, false);
                g_cur_screen = 0;
            }
        }
    }

    lv_task_handler();

    ota_server.handleClient();
    ElegantOTA.loop();
}
