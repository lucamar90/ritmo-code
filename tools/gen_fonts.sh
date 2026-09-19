#!/usr/bin/env bash
#
# gen_fonts.sh — rigenera i font LVGL del tema "Terminale" (firmware/ritmo_code/font_jbm_*.c)
#
# JetBrains Mono (OFL) per testo e cifre; i simboli che non ha (✻ ↻ ↵ ✎ e lo spinner
# di Claude Code · ✢ ✳ ✶ ✽) arrivano da DejaVu Sans Mono. Richiede node (npx) e curl.
# Dopo la conversione adatta l'output a LVGL 9.2: toglie la glyph cache (campo rimosso
# in v9) e imposta Montserrat 14 come fallback per i LV_SYMBOL_* della tastiera.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
OUT="firmware/ritmo_code"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -sSL -o "$TMP/jbm.zip" https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
curl -sSL -o "$TMP/dv.zip"  https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip
(cd "$TMP" && unzip -q -o jbm.zip && unzip -q -o dv.zip)
JB="$TMP/fonts/ttf"
DV="$TMP/dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono.ttf"

# ASCII + Latin-1 (accenti) + frecce, blocchi, mini-linea, ≡ ⌫ ✕
JBR="0x20-0x7E,0xA0-0xFF,0x2022,0x20AC,0x2190-0x2193,0x2261,0x232B,0x2581-0x2588,0x258C,0x2715"
# ✢ ✳ ✶ ✻ ✽ (spinner), ↻ ↵ ✎
DVR="0x2722,0x2733,0x2736,0x273B,0x273D,0x21BB,0x21B5,0x270E"

conv() {  # size weight
  npx -y lv_font_conv@1.5.2 --bpp 4 --size "$1" --no-compress --lv-include lvgl.h --format lvgl \
    --font "$JB/JetBrainsMono-$2.ttf" -r "$JBR" --font "$DV" -r "$DVR" -o "$OUT/font_jbm_$1.c"
}
conv 12 Regular
conv 14 Regular
conv 22 Medium
# numero grande: solo cifre, %, - e spazio
npx -y lv_font_conv@1.5.2 --bpp 4 --size 54 --no-compress --lv-include lvgl.h --format lvgl \
  --font "$JB/JetBrainsMono-ExtraBold.ttf" -r "0x20,0x25,0x2D,0x30-0x39" -o "$OUT/font_jbm_54.c"
# orologio da scrivania: cifre e due punti
npx -y lv_font_conv@1.5.2 --bpp 4 --size 96 --no-compress --lv-include lvgl.h --format lvgl   --font "$JB/JetBrainsMono-Medium.ttf" -r "0x20,0x2D,0x30-0x3A" -o "$OUT/font_jbm_96.c"
# orologio notturno a tutto schermo
npx -y lv_font_conv@1.5.2 --bpp 4 --size 150 --no-compress --lv-include lvgl.h --format lvgl \n  --font "$JB/JetBrainsMono-Medium.ttf" -r "0x20,0x2D,0x30-0x3A" -o "$OUT/font_jbm_150.c"

python3 - "$OUT" <<'PY'
import sys, glob
for f in glob.glob(sys.argv[1] + "/font_jbm_*.c"):
    t = open(f, encoding="utf-8").read()
    t = t.replace("static  lv_font_fmt_txt_glyph_cache_t cache;\n", "")
    t = t.replace("    .bitmap_format = 0,\n#if LV_VERSION_CHECK(8, 0, 0)\n    .cache = &cache\n#endif\n", "    .bitmap_format = 0,\n")
    if ".fallback" not in t:
        t = t.replace("    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */\n",
                      "    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */\n"
                      "    .fallback = &lv_font_montserrat_14,\n")
    open(f, "w", encoding="utf-8", newline="").write(t)
    print("ok", f)
PY
