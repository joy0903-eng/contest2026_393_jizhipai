#!/usr/bin/env python3
# Bulk-convert init-chain syslog() calls to ets_printf() so that early boot
# logging does not block on the (possibly stalled) USB-CDC console write path.
# This is a temporary diagnostic measure; the real fix is a non-blocking
# console, but ets_printf (ROM direct write) is proven to work pre-console.
import os

SRC = "C:/Users/admin/Desktop/zzq/deliverables/ai-vox3/openvela/contest_repo/staging/vendor_espressif/boards/esp32s3/ai_vox3/src"
FILES = [
    "ai_vox3_audio.c",
    "ai_vox3_buttons.c",
    "ai_vox3_lcd.c",
    "ai_vox3_sd.c",
    "ai_vox3_servo.c",
    "ai_vox3_ws2812.c",
]

EXTERN = 'extern int ets_printf(const char *fmt, ...);\n'

for fn in FILES:
    p = os.path.join(SRC, fn)
    with open(p, "r", encoding="utf-8") as f:
        s = f.read()
    before = s.count("syslog(LOG_ERR, ") + s.count("syslog(LOG_INFO, ")
    # Replace only the leading token; continuation args are left intact.
    s = s.replace("syslog(LOG_ERR, ", "ets_printf(")
    s = s.replace("syslog(LOG_INFO, ", "ets_printf(")
    # Ensure ets_printf is declared.
    if "extern int ets_printf" not in s:
        s = s.replace("#include <syslog.h>\n",
                      "#include <syslog.h>\n" + EXTERN, 1)
    with open(p, "w", encoding="utf-8") as f:
        f.write(s)
    after = s.count("syslog(LOG_ERR, ") + s.count("syslog(LOG_INFO, ")
    print(f"{fn}: replaced {before} -> remaining {after}; "
          f"ets_printf declared={'extern int ets_printf' in s}")
