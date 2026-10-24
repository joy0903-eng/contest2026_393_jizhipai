import difflib

REF = "c:/Users/admin/Desktop/zzq/toolchain/openvela-ref-nuttx"
P = "arch/xtensa/src/esp32s3/esp32s3_irq.c"

with open(f"{REF}/{P}", encoding="utf-8") as f:
    src = f.read()

# 0009 and 0010 do NOT touch esp32s3_getcpuint(), so the getcpuint body is
# identical in the post-0009+0010 tree.  Generate 0011 directly from the clean
# reference; it will apply cleanly on top of 0009+0010.
old = (
    "  /* Enable the CPU interrupt now.  The interrupt is still not attached\n"
    "   * to any peripheral and thus has no effect.\n"
    "   */\n"
    "\n"
    "  if (ret >= 0)\n"
    "    {\n"
    "      xtensa_enable_cpuint(&per_cpu_var_smp(g_intenable, cpu), ret);\n"
    "    }\n"
    "\n"
    "  return ret;\n"
)
new = (
    "  /* FIX: do NOT enable the CPU interrupt here.  esp32s3_setup_irq() is\n"
    "   * called from up_irqinitialize() (board early init) via\n"
    "   * esp32s3_gpioirqinitialize()/rtcioirqinitialize() BEFORE the ISR is\n"
    "   * attached with irq_attach().  Enabling the cpuint now lets a pending\n"
    "   * peripheral interrupt vector straight to irq_unexpected_isr ->\n"
    "   * PANIC() (silent boot hang seen at @IG0/@IR0).  The driver enables\n"
    "   * the cpuint itself with up_enable_irq() after it attaches its handler.\n"
    "   */\n"
    "\n"
    "  return ret;\n"
)
assert old in src, "getcpuint tail not found"
mod = src.replace(old, new, 1)
assert mod != src

patch = (
    "Fix silent-boot hang on AI-VOX3 (2nd instance): esp32s3_getcpuint() must\n"
    "NOT enable the CPU interrupt at allocation time.  esp32s3_setup_irq() is\n"
    "called during up_irqinitialize() before the ISR is attached, so enabling\n"
    "the cpuint there lets a pending peripheral interrupt panic.  The driver\n"
    "enables the cpuint with up_enable_irq() after irq_attach().\n"
    + "".join(difflib.unified_diff(
        src.splitlines(keepends=True), mod.splitlines(keepends=True),
        fromfile=f"a/{P}", tofile=f"b/{P}", n=4))
)
with open("staging/patches/0011-esp32s3-getcpuint-no-enable.patch", "w", encoding="utf-8") as f:
    f.write(patch)
print("WROTE 0011, bytes=", len(patch))
