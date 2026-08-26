cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD waveshare_rp2350_pizero)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(sync32_firmware C CXX ASM)
pico_sdk_init()

add_subdirectory(libdvi)
add_subdirectory(fatfs/src)

add_executable(sync32 
	src/main.c
	src/video.c
	src/api.c
	src/embedded_rom.c
	src/launcher.c
	src/sdcard.c
	src/disk.c
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
	${PICO_TINYUSB_PATH}/src/host/usbh.c
	${PICO_TINYUSB_PATH}/src/host/hub.c
	${PICO_TINYUSB_PATH}/src/class/hid/hid_host.c
	${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040/hcd_rp2040.c
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
	PICO_CORE1_STACK_SIZE=0x1000
	DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
	PICO_PIO_USE_GPIO_BASE=1
)
target_include_directories(sync32 PRIVATE src ../sdk/include libdvi tusb_xinput)
pico_enable_stdio_usb(sync32 1)
pico_enable_stdio_uart(sync32 0)
pico_set_linker_script(sync32 ${CMAKE_CURRENT_LIST_DIR}/memmap_sync32.ld)
target_link_libraries(sync32 hardware_flash pico_stdlib pico_multicore pico_util pico_rand libdvi no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
pico_add_extra_outputs(sync32)
