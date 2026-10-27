# Build a native Zeal 8-bit OS program with the official ZOS SDCC SDK.
#
# The paths below match the host-mode environment in ~/work (see
# ~/work/zealenv.sh).  Override them on the command line or in the
# environment if your SDK lives elsewhere.

SDCC_BIN     ?= $(HOME)/work/tools/sdcc-4.4.0/bin
ZOS_PATH     ?= $(HOME)/work/sdk/Zeal-8-bit-OS
ZVB_SDK_PATH ?= $(HOME)/work/sdk/Zeal-VideoBoard-SDK
ZGDK_PATH    ?= $(HOME)/work/sdk/zeal-game-dev-kit

ZEAL_NATIVE ?= $(HOME)/work/zeal-native-linux-x64/zeal-native
OS_IMAGE    ?= $(HOME)/work/zeal-native-linux-x64/zos/os_with_romdisk.img

BIN        = tetris.bin
INPUT_DIR  = src
OUTPUT_DIR = bin

# Make sure sdcc, sdldz80 and sdobjcopy are found even when the environment
# has not been sourced.
export PATH := $(SDCC_BIN):$(PATH)
OBJCOPY ?= sdobjcopy

ZOS_CFLAGS  += -I$(ZVB_SDK_PATH)/include -I$(ZGDK_PATH)/include --opt-code-speed
ZOS_LDFLAGS += -k $(ZVB_SDK_PATH)/lib -l zvb_gfx -l zvb_sound -k $(ZGDK_PATH)/lib -l zgdk

include $(ZOS_PATH)/sdk/sdcc/base_sdcc.mk

.PHONY: run
run: $(OUTPUT_DIR)/$(BIN)
	$(ZEAL_NATIVE) --rom $(OS_IMAGE) -u $(OUTPUT_DIR)/$(BIN)
