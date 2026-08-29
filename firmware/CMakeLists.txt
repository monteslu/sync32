cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD waveshare_rp2350_pizero)
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(sync32_firmware C CXX ASM)
pico_sdk_init()

# replace the SDK's rp2040_usb.c with tusb_patch/rp2040_usb.c (see above):
# strip the original from the tinyusb interface lib or symbols collide
if (TARGET tinyusb_device_base)
    get_target_property(_tusb_dev_srcs tinyusb_device_base INTERFACE_SOURCES)
    list(FILTER _tusb_dev_srcs EXCLUDE REGEX "rp2040_usb\\.c$")
    set_target_properties(tinyusb_device_base PROPERTIES INTERFACE_SOURCES "${_tusb_dev_srcs}")
endif()

add_subdirectory(libdvi)
add_subdirectory(fatfs/src)

# Video backend selection. Composite needs its PIO program generated too.
if(NOT DEFINED S32_VIDEO)
	set(S32_VIDEO "pio-dvi")
endif()
if(S32_VIDEO STREQUAL "composite")
	set(S32_VIDEO_SRC src/video_composite.c)
elseif(S32_VIDEO STREQUAL "hstx")
	set(S32_VIDEO_SRC src/video_hstx.c)
else()
	set(S32_VIDEO_SRC src/video_pio_dvi.c)
endif()

add_executable(sync32 
	src/main.c
	src/video.c
	# video backend: exactly ONE of these. Each dictates clk_sys, so this is
	# a build-time choice (see src/video_backend.h). Select with
	# -DS32_VIDEO=pio-dvi (default), =hstx, or =composite.
	${S32_VIDEO_SRC}
	# analog audio, for backends with no in-band audio channel (composite)
	src/audio_pwm.c
	src/api.c
	src/embedded_rom.c
	src/launcher.c
	src/sdcard.c
	src/disk.c
	src/s32tar.c
	src/log.c
	src/xfer.c
	src/xipboot.c
	src/hw_config.c
	src/font8.c
	src/font16.c
	src/font12.c
	src/usb_descriptors.c
	src/msc_disk.c
	src/usb_input.c
	tusb_xinput/xinput_host.c
	# tinyusb host stack, added as sources (NOT the sdk's tinyusb_host lib:
	# that defines LIB_TINYUSB_HOST which kills stdio_usb, and the *_MODE
	# defines which #error on dual device+host builds; only one stack is
	# ever inited per boot, so sharing the binary is fine)
	tusb_patch/usbh.c   # patched: 450ms enum debounce -> 75ms (see file)
	${PICO_TINYUSB_PATH}/src/host/hub.c
	${PICO_TINYUSB_PATH}/src/class/hid/hid_host.c
	# patched copies: upstream panics on the attach/detach speed-read race
	# and on stale endpoint AVAIL after a mid-transfer disconnect; both
	# reboot-looped the console (8BitDo Ultimate C attach/drop cycle)
	tusb_patch/hcd_rp2040.c
	tusb_patch/rp2040_usb.c
)
target_compile_options(sync32 PRIVATE -Wall -O2)
target_compile_definitions(sync32 PRIVATE
	CFG_TUD_MSC=1
	CFG_TUD_MSC_EP_BUFSIZE=512
	CFG_TUH_ENABLED=1
	CFG_TUH_HUB=1
	CFG_TUH_DEVICE_MAX=8
	CFG_TUH_ENUMERATION_BUFSIZE=512
	CFG_TUH_XINPUT=2
	CFG_TUH_HID=4
	CFG_TUH_HID_EPIN_BUFSIZE=64
	CFG_TUH_HID_EPOUT_BUFSIZE=64
	PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
	PICO_STACK_SIZE=0x1000
	PICO_MALLOC_PANIC=0
	PICO_PANIC_FUNCTION=sync32_panic
	PICO_CORE1_STACK_SIZE=0x800
	DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
	# TMDS buffers are static (video.c): 9KB heap cannot fit libdvi's
	# 3x3.8KB mallocs (the real "Killer B": dvi_init panicked at boot)
	DVI_N_TMDS_BUFFERS=0
	PICO_PIO_USE_GPIO_BASE=1
)
if(S32_VIDEO STREQUAL "composite")
	pico_generate_pio_header(sync32 ${CMAKE_CURRENT_LIST_DIR}/src/composite.pio)
endif()

target_include_directories(sync32 PRIVATE src ../sdk/include libdvi tusb_xinput
	${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040
	${PICO_TINYUSB_PATH}/src/host)
pico_enable_stdio_usb(sync32 1)
# UART0 GP0/GP1: serial console + YMODEM for pad-connected consoles
# (usb-serial dongle, or the debugprobe's GP4/GP5 UART bridge)
pico_enable_stdio_uart(sync32 1)
pico_set_linker_script(sync32 ${CMAKE_CURRENT_LIST_DIR}/memmap_sync32.ld)
target_link_libraries(sync32 pico_stdlib pico_multicore pico_util pico_rand hardware_flash libdvi no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
pico_add_extra_outputs(sync32)
