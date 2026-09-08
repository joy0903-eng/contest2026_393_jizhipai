# Contest 2026 team 393 - AI-VOX3 board sources

nuttx_add_library(board)
target_sources(board PRIVATE board_boot.c aivox3_bringup.c)

if(CONFIG_AIVOX3_LCD)
  target_sources(board PRIVATE aivox3_lcd.c)
endif()

if(CONFIG_AIVOX3_BUTTONS)
  target_sources(board PRIVATE aivox3_buttons.c)
endif()

if(CONFIG_AIVOX3_SERVO)
  target_sources(board PRIVATE aivox3_servo.c)
endif()

if(CONFIG_AIVOX3_WS2812)
  target_sources(board PRIVATE aivox3_ws2812.c)
endif()
