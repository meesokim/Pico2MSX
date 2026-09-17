cmake_minimum_required(VERSION 3.12)

set(PICO_PLATFORM rp2350)
# Don't hardcode PICO_BOARD here; we will set it per BOARD_TYPE below.

# Board selection
# Set BOARD_TYPE from command line: -DBOARD_TYPE=FRUITJAM or -DBOARD_TYPE=OLIMEXPC
# Default to FRUITJAM if not specified
if(NOT DEFINED BOARD_TYPE)
    set(BOARD_TYPE "FRUITJAM")
    message(STATUS "BOARD_TYPE not specified, defaulting to FRUITJAM (Adafruit Feather RP2350)")
endif()

# Validate board type
if(NOT ((BOARD_TYPE STREQUAL "FRUITJAM") OR (BOARD_TYPE STREQUAL "OLIMEXPC") OR (BOARD_TYPE STREQUAL "WAVESHARE")))
    message(FATAL_ERROR "Invalid BOARD_TYPE: ${BOARD_TYPE}. Must be FRUITJAM, OLIMEXPC or WAVESHARE")
endif()

message(STATUS "Building for board: ${BOARD_TYPE}")

# Select PICO_BOARD before importing the Pico SDK so the SDK doesn't default to pico2
if(BOARD_TYPE STREQUAL "WAVESHARE")
	# Ensure Pico SDK uses the Waveshare RP2350 Pi Zero board config shipped with PicoDVI
	set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Board type" FORCE)
	set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/PicoDVI/software/boards CACHE STRING "Additional board header dirs" FORCE)
else()
	# Default to Pico 2 board config for non-Waveshare targets
	if(NOT DEFINED PICO_BOARD)
		set(PICO_BOARD pico2 CACHE STRING "Board type" FORCE)
	endif()
endif()

# Pull in PICO SDK (must be before project)
include(pico_sdk_import.cmake)

project(picomsx C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Debug: print board config right before SDK init
message(STATUS "PICO_BOARD=${PICO_BOARD}")
message(STATUS "PICO_BOARD_HEADER_DIRS=${PICO_BOARD_HEADER_DIRS}")

# Initialize the Pico SDK
pico_sdk_init()

add_compile_definitions(PICO_PIO_USE_GPIO_BASE=1)

# Include directories
include_directories(picomsx)
include_directories(config)
include_directories(display)
include_directories(fatfs)
include_directories(fatfs/source)
include_directories(sd_driver)
include_directories(psram)
include_directories(flash)
include_directories(usb_kbd)
include_directories(msxbus)
include_directories(pico-pio-usb/src)
include_directories(.)

# MSX emulator sources
set(PICOMSX_SOURCES 
	picomsx/fmsx.c 
	picomsx/AY8910.c
	picomsx/Boot.c
	picomsx/Disk.c
	picomsx/I8251.c
	picomsx/I8255.c
	picomsx/Patch.c
	picomsx/SCC.c
	picomsx/Sound.c
	picomsx/V9938.c
	picomsx/YM2413.c
	picomsx/Z80.c
	picomsx/picomsx.cpp
)

# Display and HDMI sources
# FruitJam/Olimex use HSTX backend, Waveshare uses PicoDVI backend
set(DISPLAY_SOURCES 
	display/pico_dsp.cpp		
	display/emuapi.cpp
	display/AudioPlaySystem.cpp		
)

# Add display backend per board type
if(BOARD_TYPE STREQUAL "WAVESHARE")
	list(APPEND DISPLAY_SOURCES display/display_picodvi.c)
else()
	# HSTX for FruitJam/Olimex
	list(APPEND DISPLAY_SOURCES display/hdmi_framebuffer.cpp display/display_hstx.c)
endif()

# USB keyboard sources
# FruitJam and Waveshare use custom hid_app.c + PIO-USB
# Olimex uses usb_olimexpc library with native USB
if((BOARD_TYPE STREQUAL "FRUITJAM") OR (BOARD_TYPE STREQUAL "WAVESHARE"))
	set(USB_SOURCES usb_kbd/hid_app.c)
elseif(BOARD_TYPE STREQUAL "OLIMEXPC")
	set(USB_SOURCES)  # Will be populated with usb_olimexpc sources below
endif()

# Add PIO USB or usb_olimexpc based on BOARD_TYPE
# FruitJam/Waveshare use PIO-USB on GPIO pins
# Olimex uses native USB (pins 66/67) with usb_olimexpc library
if((BOARD_TYPE STREQUAL "FRUITJAM") OR (BOARD_TYPE STREQUAL "WAVESHARE"))
	message(STATUS "${BOARD_TYPE}: adding PIO USB TinyUSB drivers")
	list(APPEND USB_SOURCES
		${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
	)
elseif(BOARD_TYPE STREQUAL "OLIMEXPC")
	message(STATUS "Olimex: using usb_olimexpc library for native USB")
	# Include directories for usb_olimexpc
	include_directories(usb_olimexpc/include)
	include_directories(usb_olimexpc/common/include)
	
	# Collect USB handlers and common module
	file(GLOB USB_OLIMEXPC_HANDLERS "usb_olimexpc/library/hid_handler.c" "usb_olimexpc/library/usb_handler.c")
	file(GLOB USB_OLIMEXPC_COMMON "usb_olimexpc/common/library/common.c")
	
	# Add USB sources including MSX keyboard adapter
	list(APPEND USB_SOURCES ${USB_OLIMEXPC_HANDLERS} ${USB_OLIMEXPC_COMMON} usb_olimexpc/msx_keyboard_adapter.c)
	
	# Define flag to enable usb_olimexpc USB
	add_compile_definitions(USE_USB_OLIMEXPC=1)
	add_compile_definitions(DEBUG=1)  # Enable UART logging
	add_compile_definitions(CFG_TUSB_DEBUG=1)  # TinyUSB debug logs (low verbosity)
endif()

# FAT filesystem and SD card sources
set(FATSD_SOURCES 
	sd_driver/tf_card.c
	fatfs/source/ff.c
	fatfs/source/ffunicode.c		
	fatfs/source/ffsystem.c		
)

# PSRAM sources (per board)
# FruitJam: custom SPI PSRAM driver
# Waveshare: Sparkfun library for XIP PSRAM on GPIO47
if(BOARD_TYPE STREQUAL "FRUITJAM")
	set(PSRAM_SOURCES 
		psram/psram_spi.c
		psram/psram_t.cpp
	)
elseif(BOARD_TYPE STREQUAL "WAVESHARE")
	# Waveshare uses Sparkfun PSRAM library (added below as subdirectory)
	set(PSRAM_SOURCES)
else()
	set(PSRAM_SOURCES)
endif()

# Flash sources
set(FLASH_SOURCES 
	flash/flash_t.c
)

# MSX Bus sources
set(MSXBUS_SOURCES
	msxbus/msxbus.cpp
	msxbus/romdump.cpp
)

# Create executable
add_executable(picomsx
	${PICOMSX_SOURCES}
	${PSRAM_SOURCES}
	${FLASH_SOURCES}
	${DISPLAY_SOURCES}
	${FATSD_SOURCES}
	${USB_SOURCES}
	${MSXBUS_SOURCES}
)

# Ensure header precedence for critical configs
# 1) Project root: prefer our top-level headers (e.g., ffconf.h)
target_include_directories(picomsx BEFORE PRIVATE .)

# 2) Ensure usb_olimexpc config headers (tusb_config.h) take precedence for OLIMEXPC builds
if(BOARD_TYPE STREQUAL "OLIMEXPC")
	target_include_directories(picomsx BEFORE PRIVATE usb_olimexpc/include usb_olimexpc/common/include)
endif()

# 3) Ensure our FatFS config (ffconf.h) and sources are reachable early
target_include_directories(picomsx BEFORE PRIVATE fatfs fatfs/source)

# Optional: swap DP/DM on boards where wiring is reversed
option(USB_DPDM_INVERT "Swap USB D+ and D- pins on host" OFF)
if(USB_DPDM_INVERT)
	target_compile_definitions(picomsx PRIVATE USB_DPDM_INVERT=1)
	message(STATUS "USB_DPDM_INVERT is ON: building with D+ and D- swapped")
endif()

# If user provided DEFAULT_MSX_VERSION via CMake cache, propagate it as a preprocessor define
if(DEFINED DEFAULT_MSX_VERSION)
	message(STATUS "DEFAULT_MSX_VERSION provided: ${DEFAULT_MSX_VERSION}")
	target_compile_definitions(picomsx PRIVATE DEFAULT_MSX_VERSION=${DEFAULT_MSX_VERSION})
endif()

# Generate PIO headers
pico_generate_pio_header(picomsx ${CMAKE_CURRENT_LIST_DIR}/psram/psram_spi.pio)

# Add audio libraries from pico-extras
add_subdirectory(pico-extras/src/rp2_common/pico_audio_i2s)
add_subdirectory(pico-extras/src/common/pico_audio)
add_subdirectory(pico-extras/src/common/pico_util_buffer)

# Add Pico-PIO-USB library for USB host on GPIO pins
add_subdirectory(pico-pio-usb)

# Add PicoDVI library for Waveshare HDMI output
if(BOARD_TYPE STREQUAL "WAVESHARE")
	message(STATUS "Adding PicoDVI library for Waveshare")
	add_subdirectory(PicoDVI/software/libdvi)
	add_subdirectory(PicoDVI/software/libsprite)
	# Add Sparkfun PSRAM library for Waveshare 16MB PSRAM
	add_subdirectory(sparkfun-pico/sparkfun_pico)
	# Include PicoDVI headers
	target_include_directories(picomsx PRIVATE 
		PicoDVI/software/include
		PicoDVI/software/assets
		sparkfun-pico/sparkfun_pico/include
	)
endif()

# Audio configuration for I2S
add_compile_definitions(
	PICO_AUDIO_I2S_PIO=1 
	PICO_AUDIO_I2S_DMA_IRQ=1 
	PICO_AUDIO_I2S_DATA_PIN=24 
	PICO_AUDIO_I2S_CLOCK_PIN_BASE=26 
	PICO_AUDIO_I2S_MONO_INPUT=1
)

# Enable keyboard input (not joystick-only mode)
add_compile_definitions(KEYBOARD_ACTIVATED=1)

# UART configuration
# Route stdio UART per board to match wiring
if(BOARD_TYPE STREQUAL "OLIMEXPC")
	# Olimex RP2350-PC: use UART0 on GPIO0/1 (default pins)
	add_compile_definitions(
		PICO_DEFAULT_UART=0
		PICO_DEFAULT_UART_TX_PIN=0
		PICO_DEFAULT_UART_RX_PIN=1
	)
elseif(BOARD_TYPE STREQUAL "WAVESHARE")
	# Waveshare RP2350 HDMI: UART1 on GPIO4(TX) / GPIO5(RX) - confirmed working
	add_compile_definitions(
		PICO_DEFAULT_UART=1
		PICO_DEFAULT_UART_TX_PIN=4
		PICO_DEFAULT_UART_RX_PIN=5
	)
else()
	# FruitJam: keep UART0 on GPIO44/45 to avoid conflicts with GPIO0 button
	add_compile_definitions(
		PICO_DEFAULT_UART=0
		PICO_DEFAULT_UART_TX_PIN=44
		PICO_DEFAULT_UART_RX_PIN=45
	)
endif()

set(EXTRA_AUDIO_LIB pico_util_buffer pico_audio pico_audio_i2s hardware_i2c)

# Link libraries
target_link_libraries(picomsx 
	pico_multicore
	pico_stdlib
	hardware_dma
	hardware_spi
	hardware_adc
	hardware_pio
	hardware_pwm	    
	tinyusb_host 
	tinyusb_board

	${EXTRA_AUDIO_LIB}
)

# Link Pico-PIO-USB only for FruitJam and Waveshare builds (after target exists)
if((BOARD_TYPE STREQUAL "FRUITJAM") OR (BOARD_TYPE STREQUAL "WAVESHARE"))
	target_link_libraries(picomsx pico_pio_usb)
endif()

# Link PicoDVI and Sparkfun PSRAM only for Waveshare
if(BOARD_TYPE STREQUAL "WAVESHARE")
	target_link_libraries(picomsx 
		libdvi
		libsprite
		sparkfun_pico
	)
	# Add PicoDVI compile definitions
	target_compile_definitions(picomsx PRIVATE
		USE_LIBDVI=1
		DVI_DEFAULT_SERIAL_CONFIG=waveshare_rp2350_pizero_cfg
		DVI_DEFAULT_PIO_INST=pio1
		PICO_SCANVIDEO_ENABLE_VIDEO_CLOCK_DOWN=0
	)
endif()

# USB PIO configuration (per board DP pin)
if(BOARD_TYPE STREQUAL "FRUITJAM")
	target_compile_definitions(picomsx PRIVATE PIO_USB_DP_PIN_DEFAULT=2)
elseif(BOARD_TYPE STREQUAL "WAVESHARE")
	# Waveshare PIO-USB uses GPIO28 as D+
	target_compile_definitions(picomsx PRIVATE PIO_USB_DP_PIN_DEFAULT=28)
endif()
# Board type definition
target_compile_definitions(picomsx PRIVATE BOARD_${BOARD_TYPE}=1)


# Clock adjustment
target_compile_definitions(picomsx PRIVATE 
	PICO_CLOCK_AJDUST_PERI_CLOCK_WITH_SYS_CLOCK=1
)

# Number of SD cards
IF (NOT DEFINED N_SD_CARDS)
	SET(N_SD_CARDS 1)
ENDIF()
target_compile_definitions(picomsx PUBLIC N_SD_CARDS=${N_SD_CARDS})

# Optimization level
target_compile_options(picomsx PUBLIC -O3)

# Explicitly select stdio backends: disable USB stdio, enable UART
pico_enable_stdio_usb(picomsx FALSE)
pico_enable_stdio_uart(picomsx TRUE)

# Generate additional output formats (UF2, hex, bin, etc.)
pico_add_extra_outputs(picomsx)
