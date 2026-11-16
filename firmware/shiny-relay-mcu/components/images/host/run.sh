#!/bin/zsh
# run.sh BUILD_DIR [TMP]: lane fwshrink1. Draw every picture with LVGL on this Mac, built with the firmware's own LVGL config
# (BUILD_DIR/config/sdkconfig.h), as the original RGB565 converter output (git 4db558a) and as the indexed form here; the
# frame buffers must be the same bytes (render.c). Takes ~1 min (LVGL is compiled once into TMP).
set -e
H=${0:A:h}; IM=$H/..; LV=$IM/../../managed_components/lvgl__lvgl; SDK=${1:A}/config/sdkconfig.h; T=${2:-/tmp/fwshrink1_lvhost}
CF=(-O1 -w -I$LV -I$LV/src "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"$SDK\"" -DLV_LVGL_H_INCLUDE_SIMPLE)
mkdir -p $T/lv $T/img $T/orig
if [ ! -f $T/liblv.a ]; then
  for f in $(find $LV/src -name '*.c'); do clang $CF -c $f -o $T/lv/$(echo ${f#$LV/src/} | tr '/' '_' | sed 's/\.c$/.o/'); done
  ar rcs $T/liblv.a $T/lv/*.o
fi
for f in $IM/*.c; do n=$(basename $f .c)
  git -C $IM show 4db558a:components/images/$n.c > $T/orig/$n.c
  clang $CF -c $f -o $T/img/new_$n.o
  clang $CF -D${n}=orig_${n} -D${n}_map=orig_${n}_map -c $T/orig/$n.c -o $T/img/orig_$n.o
done
clang $CF $H/render.c $T/img/*.o $T/liblv.a -o $T/render
$T/render
