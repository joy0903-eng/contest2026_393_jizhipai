# Contest 2026 team 393 - AI-VOX3 board (ESP32-S3-R8)
# This board is injected into the openvela tree at
#   vendor/openvela/boards/contest2026_393_board
# via the <linkfile> in contest2026_393_jizhipai.xml.

if(CONFIG_ARCH_BOARD_CONTEST2026_393_BOARD)
  add_subdirectory(src)

  # ★ 注册链接脚本（2026-09-15 run#11 诊断：LD_SCRIPT 属性为空导致
  #   nuttx/CMakeLists.txt:706 get_filename_component 参数不足而崩溃）
  # 使用 NuttX 自带的 esp32s3 通用链接脚本；找不到则逐个候选回退。
  set(_LD_CANDIDATES
    "${CMAKE_SOURCE_DIR}/boards/xtensa/esp32s3/common/scripts/esp32s3.ld"
    "${CMAKE_SOURCE_DIR}/boards/xtensa/esp32s3/common/scripts/esp32s3_bt.ld")
  foreach(_ld ${_LD_CANDIDATES})
    if(EXISTS "${_ld}")
      set_property(GLOBAL PROPERTY LD_SCRIPT "${_ld}")
      message(STATUS "contest2026_393_board: LD_SCRIPT = ${_ld}")
      break()
    endif()
  endforeach()
  get_property(_ld_check GLOBAL PROPERTY LD_SCRIPT)
  if(NOT _ld_check)
    message(FATAL_ERROR "contest2026_393_board: 无法定位 esp32s3 链接脚本，LD_SCRIPT 为空")
  endif()
endif()
