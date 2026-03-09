/**
 * lv_conf.h - Configuracion LVGL para Waveshare ESP32-S3-Touch-LCD-7
 * Resolucion: 800x480, color: RGB565 (16 bits)
 */

#if 1 /* Activar este archivo */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Profundidad de color: 16 = RGB565 */
#define LV_COLOR_DEPTH 16

/* Swap de bytes en color (necesario en algunos casos con ESP32) */
#define LV_COLOR_16_SWAP 0

/* Memoria interna de LVGL (para objetos UI pequeños) */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64 * 1024U)   /* 64 KB */

/* Usar malloc/free del sistema */
#define LV_MEM_ADR 0

/* Tamaño de la pila del timer de LVGL */
#define LV_TIMER_CUSTOM 0

/* Tick: gestionado externamente via lv_tick_inc() */
#define LV_TICK_CUSTOM 0

/* Resolución maxima de pantalla */
#define LV_HOR_RES_MAX 800
#define LV_VER_RES_MAX 480

/* DPI de la pantalla 7" a ~133ppi */
#define LV_DPI_DEF 130

/* ─── FUENTES ─────────────────────────────────────────────────────────────── */
#define LV_FONT_MONTSERRAT_14 1   /* Textos pequeños (estado, humedad) */
#define LV_FONT_MONTSERRAT_24 1   /* Labels de seccion */
#define LV_FONT_MONTSERRAT_48 1   /* Temperatura (numero grande) */

/* Fuente por defecto */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Soporte de simbolos integrados (iconos de FontAwesome) */
#define LV_USE_FONT_SUBPX 1
#define LV_FONT_SUBPX_BGR 0

/* ─── WIDGETS NECESARIOS ──────────────────────────────────────────────────── */
/* Habilitar todos los widgets base para evitar errores de dependencia */
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1
#define LV_USE_METER      1

/* ─── OTROS ───────────────────────────────────────────────────────────────── */
#define LV_USE_LOG        1
#define LV_LOG_LEVEL      LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF     1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

/* Limitar profundidad de objetos anidados */
#define LV_OBJ_STYLE_MAX_CACHED_STYLE_LIST 8

/* Tamaño buffer de gradientes */
#define LV_GRAD_CACHE_DEF_SIZE 0

/* Dithering para suavizar gradientes en RGB565 (elimina el efecto de franjas) */
#define LV_DITHER_GRADIENT        1
#define LV_DITHER_ERROR_DIFFUSION 0

#define LV_USE_USER_DATA 1

#endif /* LV_CONF_H */
#endif /* Fin del archivo */
