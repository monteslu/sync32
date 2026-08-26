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
	src/hw_config.c
	src/font8.c
	src/font16.c
	src/font12.c
	src/usb_descriptors.c
	src/msc_disk.c
)
target_compile_options(sync32 PRIVATE -Wall -O2)
target_compile_definitions(sync32 PRIVATE
	CFG_TUD_MSC=1
	CFG_TUD_MSC_EP_BUFSIZE=512
	PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
	DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
	PICO_PIO_USE_GPIO_BASE=1
)
target_include_directories(sync32 PRIVATE src ../sdk/include libdvi)
pico_enable_stdio_usb(sync32 1)
pico_enable_stdio_uart(sync32 0)
pico_set_linker_script(sync32 ${CMAKE_CURRENT_LIST_DIR}/memmap_sync32.ld)
target_link_libraries(sync32 pico_stdlib pico_multicore pico_util pico_rand libdvi no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
pico_add_extra_outputs(sync32)
