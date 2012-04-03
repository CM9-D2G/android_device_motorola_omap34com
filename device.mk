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

# This file includes all definitions that apply to ALL omap34com devices, and
# are also specific to omap34com devices
#
# Everything in this directory will become public

DEVICE_PREBUILT := device/motorola/omap34com/prebuilt
DEVICE_PACKAGE_OVERLAYS := device/motorola/omap34com/overlay

# This device is xhdpi.  However the platform doesn't
# currently contain all of the bitmaps at xhdpi density so
# we do this little trick to fall back to the hdpi version
# if the xhdpi doesn't exist.
PRODUCT_AAPT_CONFIG := normal hdpi
PRODUCT_AAPT_PREF_CONFIG := hdpi

# Camera
PRODUCT_PACKAGES := \
	camera.omap3 \
	hwcomposer.default \
	libcamera \
	libui

# Modem
PRODUCT_PACKAGES += \
	rild \
	libril \
	libreference-ril \
	libreference-cdma-sms \
	radiooptions

# ICS graphics
PRODUCT_PACKAGES += libGLESv2 libEGL libGLESv1_CM

# for jpeg hw encoder/decoder
PRODUCT_PACKAGES += libOMX.TI.JPEG.Encoder libOMX.TI.JPEG.decoder libstagefrighthw

# OMX
PRODUCT_PACKAGES += \
	dspexec \
	libbridge \
	libLCML \
	libOMX_Core \
	libOMX.TI.AAC.decode \
	libOMX.TI.AAC.encode \
	libOMX.TI.AMR.decode \
	libOMX.TI.MP3.decode \
	libOMX.TI.Video.Decoder \
	libOMX.TI.Video.encoder \
	libOMX.TI.WBAMR.decode \
	libOMX.TI.WBAMR.encode \
	libOMX.TI.WMA.decode \
	libOMX.TI.VPP

# Wifi
PRODUCT_PACKAGES += \
	libCustomWifi \
	wlan_loader

# Apps and bin
PRODUCT_PACKAGES += \
	Superuser \
	su \
	mot_boot_mode \
	charge_only_mode \
	com.android.future.usb.accessory \
	FileManager \
	Torch \
	Usb

# Live Wallpapers
PRODUCT_PACKAGES += \
	LiveWallpapers \
	LiveWallpapersPicker \
	VisualizationWallpapers \
	librs_jni

# Key Layouts
PRODUCT_COPY_FILES := \
	$(DEVICE_PREBUILT)/usr/idc/cpcap-key.idc:system/usr/idc/cpcap-key.idc \
	$(DEVICE_PREBUILT)/usr/idc/light-prox.idc:system/usr/idc/light-prox.idc \
	$(DEVICE_PREBUILT)/usr/idc/qtouch-touchscreen.idc:system/usr/idc/qtouch-touchscreen.idc \
	$(DEVICE_PREBUILT)/usr/idc/sholes-keypad.idc:system/usr/idc/sholes-keypad.idc \
	$(DEVICE_PREBUILT)/usr/keylayout/cpcap-key.kl:system/usr/keylayout/cpcap-key.kl \
	$(DEVICE_PREBUILT)/usr/keylayout/qtouch-touchscreen.kl:system/usr/keylayout/qtouch-touchscreen.kl \
	$(DEVICE_PREBUILT)/usr/keylayout/sholes-keypad.kl:system/usr/keylayout/sholes-keypad.kl \
	$(DEVICE_PREBUILT)/usr/keychars/cpcap-key.kcm:system/usr/keychars/cpcap-key.kcm \
	$(DEVICE_PREBUILT)/usr/keychars/qtouch-touchscreen.kcm:system/usr/keychars/qtouch-touchscreen.kcm \
	$(DEVICE_PREBUILT)/usr/keychars/sholes-keypad.kcm:system/usr/keychars/sholes-keypad.kcm

# Graphics
PRODUCT_COPY_FILES += \
	$(DEVICE_PREBUILT)/imgtec/pvrsrvinit:system/bin/pvrsrvinit \
	$(DEVICE_PREBUILT)/imgtec/libEGL_POWERVR_SGX530_125.so:system/lib/egl/libEGL_POWERVR_SGX530_125.so \
	$(DEVICE_PREBUILT)/imgtec/libGLESv1_CM_POWERVR_SGX530_125.so:system/lib/egl/libGLESv1_CM_POWERVR_SGX530_125.so \
	$(DEVICE_PREBUILT)/imgtec/libGLESv2_POWERVR_SGX530_125.so:system/lib/egl/libGLESv2_POWERVR_SGX530_125.so \
	$(DEVICE_PREBUILT)/imgtec/libglslcompiler.so:system/lib/libglslcompiler.so \
	$(DEVICE_PREBUILT)/imgtec/libHPImgApi.so:system/lib/libHPImgApi.so \
	$(DEVICE_PREBUILT)/imgtec/libIMGegl.so:system/lib/libIMGegl.so \
	$(DEVICE_PREBUILT)/imgtec/libpvr2d.so:system/lib/libpvr2d.so \
	$(DEVICE_PREBUILT)/imgtec/libpvrANDROID_WSEGL.so:system/lib/libpvrANDROID_WSEGL.so \
	$(DEVICE_PREBUILT)/imgtec/libsrv_init.so:system/lib/libsrv_init.so \
	$(DEVICE_PREBUILT)/imgtec/libsrv_um.so:system/lib/libsrv_um.so \
	$(DEVICE_PREBUILT)/imgtec/libusc.so:system/lib/libusc.so \
	$(DEVICE_PREBUILT)/imgtec/libeglinfo.so:system/lib/egl/libeglinfo.so \
	$(DEVICE_PREBUILT)/imgtec/libgles1_texture_stream.so:system/lib/egl/libgles1_texture_stream.so \
	$(DEVICE_PREBUILT)/imgtec/libgles2_texture_stream.so:system/lib/egl/libgles2_texture_stream.so \
	$(DEVICE_PREBUILT)/imgtec/gralloc.omap3.so:system/lib/hw/gralloc.omap3.so

# Prebuilts
PRODUCT_COPY_FILES += \
	$(DEVICE_PREBUILT)/bin/mount_ext3.sh:system/bin/mount_ext3.sh \
	$(DEVICE_PREBUILT)/bin/adbd:system/bin/adbd \
	$(DEVICE_PREBUILT)/etc/apns-conf.xml:system/etc/apns-conf.xml \
	$(DEVICE_PREBUILT)/etc/egl.cfg:system/etc/egl.cfg \
	$(DEVICE_PREBUILT)/etc/gps.conf:system/etc/gps.conf \
	$(DEVICE_PREBUILT)/etc/media_profiles.xml:system/etc/media_profiles.xml \
	$(DEVICE_PREBUILT)/etc/powervr.ini:system/etc/powervr.ini \
	$(DEVICE_PREBUILT)/etc/vold.fstab:system/etc/vold.fstab \
	$(DEVICE_PREBUILT)/etc/sysctl.conf:system/etc/sysctl.conf \
	$(DEVICE_PREBUILT)/etc/init.d/12scheduler:system/etc/init.d/12scheduler \
	$(DEVICE_PREBUILT)/etc/init.d/13kernel:system/etc/init.d/13kernel \
	$(DEVICE_PREBUILT)/etc/init.d/14multitouch:system/etc/init.d/14multitouch \
	$(DEVICE_PREBUILT)/xbin/multitouch:system/xbin/multitouch \
	$(DEVICE_PREBUILT)/media/bootanimation.zip:system/media/bootanimation.zip


# Permissions files
PRODUCT_COPY_FILES += \
	frameworks/base/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
	frameworks/base/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
	frameworks/base/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
	frameworks/base/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
	frameworks/base/data/etc/android.hardware.sensor.compass.xml:system/etc/permissions/android.hardware.sensor.compass.xml \
	frameworks/base/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
	frameworks/base/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
	frameworks/base/data/etc/android.hardware.touchscreen.multitouch.distinct.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.distinct.xml \
	frameworks/base/data/etc/android.hardware.usb.host.xml:system/etc/permissions/android.hardware.usb.host.xml \
	frameworks/base/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml \
	frameworks/base/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
	frameworks/base/data/etc/android.software.sip.voip.xml:system/etc/permissions/android.software.sip.voip.xml \
	packages/wallpapers/LivePicker/android.software.live_wallpaper.xml:/system/etc/permissions/android.software.live_wallpaper.xml \

# these need to be here for the installer, just put them here for now
PRODUCT_COPY_FILES += \
	device/motorola/omap34com/releaseutils/mke2fs:system/bin/mke2fs \
	device/motorola/omap34com/releaseutils/tune2fs:system/bin/tune2fs \
	device/motorola/omap34com/releaseutils/check_kernel:system/etc/releaseutils/check_kernel \
	device/motorola/omap34com/releaseutils/finalize_release:system/etc/finalize_release

# Hijack files
PRODUCT_COPY_FILES += \
	$(DEVICE_PREBUILT)/bin/hijack:system/bin/hijack \
	$(DEVICE_PREBUILT)/bin/hijack.log_dump:system/bin/hijack.log_dump \

# Copy all common kernel modules
PRODUCT_COPY_FILES += $(shell \
	find device/motorola/omap34com/modules -name '*.ko' \
	| sed -r 's/^\/?(.*\/)([^/ ]+)$$/\1\2:system\/lib\/modules\/\2/' \
	| tr '\n' ' ')

# Some overrides never change
PRODUCT_PROPERTY_OVERRIDES += \
	mobiledata.interfaces=ppp0 \
	persist.ril.mux.retries=500 \
	persist.ril.mux.sleep=2 \
	persist.ril.mux.ttydevice=/dev/ttyS0 \
	persist.ril.pppd.start.fail.max=16 \
	ro.cdma.data_retry_config=default_randomization=2000,0,0,120000,180000,540000,960000 \
	ro.cdma.home.operator.alpha=Verizon \
	ro.cdma.home.operator.numeric=310004 \
	ro.cdma.homesystem=64,65,76,77,78,79,80,81,82,83 \
	ro.com.google.gmsversion=2.3_r3 \
	ro.kernel.android.ril=yes \
	ro.product.multi_touch_enabled=true \
	ro.media.dec.jpeg.memcap=20000000 \
	ro.setupwizard.enable_bypass=1 \
	ro.setupwizard.mode=OPTIONAL \
	ro.telephony.call_ring.delay=1000 \
	ro.telephony.call_ring.multiple=false \
	ro.kernel.android.checkjni=0 \
	dalvik.vm.checkjni=false \
	dalvik.vm.dexopt-data-only=1

# we have enough storage space to hold precise GC data
PRODUCT_TAGS += dalvik.gc.type-precise

# still need to set english for audio init
PRODUCT_LOCALES += en_US

$(call inherit-product, frameworks/base/build/phone-hdpi-512-dalvik-heap.mk)
$(call inherit-product, hardware/ti/omap3/Android.mk)
$(call inherit-product-if-exists, vendor/cm/config/common_full_phone.mk)
$(call inherit-product-if-exists, vendor/motorola/omap34com/device-vendor.mk)
