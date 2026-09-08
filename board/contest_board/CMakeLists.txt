# Contest 2026 team 393 - AI-VOX3 board (ESP32-S3-R8)
# This board is injected into the openvela tree at
#   vendor/openvela/boards/contest2026_393_board
# via the <linkfile> in contest2026_393_jizhipai.xml.

if(CONFIG_ARCH_BOARD_CONTEST2026_393_BOARD)
  add_subdirectory(src)
endif()
