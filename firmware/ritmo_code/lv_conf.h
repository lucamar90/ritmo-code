/**
 * lv_conf.h — configurazione di LVGL 9.2 per Ritmo Code (JC4832W535).
 *
 * Compilare con -DLV_CONF_INCLUDE_SIMPLE (vedi build.sh) perche' LVGL trovi
 * questo file tramite l'include path dello sketch. Alternativa: copiare questo file
 * nella cartella libraries di Arduino (un livello sopra la cartella `lvgl`).
 *
 * File parziale: cio' che non e' definito qui usa il default di
 * lv_conf_internal.h. Le opzioni qui sotto sono quelle che contano per questa scheda.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Guardia obbligatoria: lv_conf_internal.h include questo file anche durante
   l'assemblaggio dei .S di LVGL (es.: draw/sw/blend/helium/lv_blend_helium.S). Senza
   la guardia, l'assembler riceve i typedef di stdint.h e fallisce con
   "unknown opcode or format name 'typedef'". Vedi l'avviso in lv_conf_internal.h. */
#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

/*====================
   COLOR
 *====================*/
#define LV_COLOR_DEPTH 16
/* La pipeline validata nel bring-up usa la copia diretta RGB565 (senza swap).
   Se rosso e blu risultano invertiti, metti 1. */
#define LV_COLOR_16_SWAP 0

/*=========================
   MEMORIA
   Pool interno di LVGL (oggetti/stili). Il buffer di render a schermo intero
   (480x320x2) e' allocato a parte in PSRAM, dentro lo sketch.
 *=========================*/
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING   LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_BUILTIN
#define LV_MEM_SIZE            (96 * 1024U)

/*====================
   HAL / SISTEMA
 *====================*/
#define LV_USE_OS LV_OS_NONE
/* il tick arriva da lv_tick_set_cb(millis) nello sketch */

/*====================
   RENDER
 *====================*/
#define LV_USE_DRAW_SW 1

/*====================
   FONT (Montserrat usati nella UI)
 *====================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*====================
   WIDGET usati
 *====================*/
#define LV_USE_LABEL        1
#define LV_USE_BUTTON       1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_BAR          1
#define LV_USE_LIST         1
#define LV_USE_TEXTAREA     1
#define LV_USE_KEYBOARD     1
#define LV_USE_CANVAS       1
#define LV_USE_IMAGE        1
#define LV_USE_LINE         1
#define LV_USE_ARC          1
#define LV_USE_SPINNER      1
#define LV_USE_TILEVIEW     1
#define LV_USE_CHART        1

/*====================
   DIAGNOSTICA
   Overlay FPS/CPU (acceso/spento da Impostazioni -> Contatore FPS).
 *====================*/
#define LV_USE_SYSMON               1
#define LV_USE_PERF_MONITOR         1
#define LV_USE_PERF_MONITOR_POS     LV_ALIGN_BOTTOM_RIGHT
#define LV_USE_PERF_MONITOR_LOG_MODE 0

#endif /*LV_CONF_H*/
