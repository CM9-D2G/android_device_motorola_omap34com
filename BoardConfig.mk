#
# Copyright (C) 2011 The Android Open-Source Project
# Copyright (C) 2012 bikedude880
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# This variable is set first, so it can be overridden
# by BoardConfigVendor.mk
USE_CAMERA_STUB := false

# Use the non-open-source parts, if they're present
-include vendor/motorola/omap34com/BoardConfigVendor.mk

# Processor
TARGET_NO_BOOTLOADER := true
TARGET_BOARD_PLATFORM := omap3
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_SMP := false
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_ARCH_VARIANT_CPU := cortex-a8
TARGET_ARCH_VARIANT_FPU := neon
TARGET_GLOBAL_CFLAGS += -mtune=cortex-a8
TARGET_GLOBAL_CPPFLAGS += -mtune=cortex-a8

# Kernel
BOARD_KERNEL_BASE := 0x10000000
# BOARD_KERNEL_CMDLINE :=

# OMAP
OMAP_ENHANCEMENT := true
ifdef OMAP_ENHANCEMENT
COMMON_GLOBAL_CFLAGS += -DOMAP_ENHANCEMENT -DTARGET_OMAP3
endif

# Graphics
BOARD_EGL_CFG := device/motorola/omap34com/prebuilt/etc/egl.cfg
COMMON_GLOBAL_CFLAGS += -DMISSING_EGL_EXTERNAL_IMAGE -DMISSING_EGL_PIXEL_FORMAT_YV12 -DMISSING_GRALLOC_BUFFERS
COMMON_GLOBAL_CFLAGS += -DOMAP_COMPAT -DMOTOROLA_UIDS -DBINDER_COMPAT
BOARD_NO_RGBX_8888 := true
DEFAULT_FB_NUM := 0
BOARD_USE_YUV422I_DEFAULT_COLORFORMAT := true
BOARD_USES_OVERLAY := true

# Recovery
BOARD_HAS_LOCKED_BOOTLOADER := true
BOARD_ALWAYS_INSECURE := true
BOARD_HAS_LARGE_FILESYSTEM := true
TARGET_RECOVERY_PRE_COMMAND := "echo 1 > /data/.recovery_mode; sync;"
TARGET_RECOVERY_PRE_COMMAND_CLEAR_REASON := true

#TARGET_PROVIDES_INIT_RC := true
BOARD_MKE2FS := device/motorola/omap34com/releaseutils/mke2fs

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER := CUSTOM
WPA_SUPPLICANT_VERSION      := VER_0_6_X
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := libCustomWifi
#BOARD_HOSTAPD_DRIVER        := NL80211
#BOARD_HOSTAPD_PRIVATE_LIB   := lib_driver_cmd_bcmdhd
BOARD_WLAN_DEVICE           := wl1271
#BOARD_SOFTAP_DEVICE         := wl1271
WIFI_DRIVER_MODULE_PATH     := "/system/lib/modules/tiwlan_drv.ko"
BOARD_WLAN_TI_STA_DK_ROOT   := system/wlan/ti/wilink_6_1
WIFI_DRIVER_MODULE_ARG      := ""
WIFI_DRIVER_MODULE_NAME     := "tiwlan_drv"
WIFI_FIRMWARE_LOADER        := "wlan_loader"
WIFI_DRIVER_FW_STA_PATH     := "/system/etc/wifi/fw_wlan1271.bin"
WIFI_DRIVER_FW_AP_PATH      := "/system/etc/wifi/fw_tiwlan_ap.bin"

# Bluetooth
BOARD_HAVE_BLUETOOTH := true

# Sensors
ENABLE_SENSORS_COMPAT := true

# Camera
BOARD_OVERLAY_BASED_CAMERA_HAL := true

# OMX
HARDWARE_OMX := true
ifdef HARDWARE_OMX
OMX_VENDOR := ti
OMX_VENDOR_WRAPPER := TI_OMX_Wrapper
BOARD_OPENCORE_LIBRARIES := libOMX_Core
BOARD_OPENCORE_FLAGS := -DHARDWARE_OMX=1
BUILD_WITH_TI_AUDIO := 1
TARGET_USE_OMAP_COMPAT  := true
TARGET_USE_OMX_RECOVERY := true
endif
#BUILD_PV_VIDEO_ENCODERS := 1
#OMX_TI_OMAP_TIER_LEVEL := 10

# MOTOROLA
USE_MOTOROLA_CODE := true
ifdef USE_MOTOROLA_CODE
COMMON_GLOBAL_CFLAGS += -DUSE_MOTOROLA_CODE
endif
USE_MOTOROLA_USERS := true
ifdef USE_MOTOROLA_USERS
COMMON_GLOBAL_CFLAGS += -DUSE_MOTOROLA_USERS
endif

# Hijack
TARGET_NEEDS_MOTOROLA_HIJACK := true
#BOARD_HIJACK_LOG_ENABLE := true

# OTA Packaging
TARGET_PROVIDES_RELEASETOOLS := true
TARGET_RELEASETOOL_OTA_FROM_TARGET_SCRIPT := device/motorola/omap34com/releasetools/droid_ota_from_target_files
TARGET_RELEASETOOL_IMG_FROM_TARGET_SCRIPT := device/motorola/omap34com/releasetools/droid_img_from_target_files

# Misc.
#BOARD_USE_BATTERY_CHARGE_COUNTER := true
BOARD_FLASH_BLOCK_SIZE := 131072
BOARD_NEEDS_CUTILS_LOG := true
BOARD_USES_SECURE_SERVICES := true
BOARD_USES_KEYBOARD_HACK := true
BOARD_USE_LEGACY_TOUCHSCREEN := true
#BOARD_USE_KINETO_COMPATIBILITY := true

# TESTING
TARGET_USE_SCORPION_BIONIC_OPTIMIZATION := true
DEBUG_BIONIC_LIBC := true
