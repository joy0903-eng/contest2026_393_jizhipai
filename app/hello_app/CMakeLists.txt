# Contest 2026 team 393 - AI-VOX3 desktop companion demo
# (lives in app/hello_app so the contest manifest linkfile deploys it to
#  packages/demos/contest2026_393_hello_app)

if(CONFIG_LVX_USE_DEMO_CONTEST2026_393_DESKTOP_COMPANION)
  nuttx_add_application(
    NAME desktop_companion
    SRCS desktop_companion_main.c
    STACKSIZE 4096)
endif()
