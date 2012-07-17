BUILD_WFD_FROM_SOURCE := device/ti/common-open/wfd/wfd-products.mk
#If the above path for source code is present, then the code
#is used for build, otherwise the libraries are copied and used.
ifeq ($(wildcard $(BUILD_WFD_FROM_SOURCE)),)

LOCAL_PATH := $(call my-dir)



define _add-wfd-lib
    include $$(CLEAR_VARS)
    $(if $(word 2,$1),$(error Invalid WFD module name $1))
    LOCAL_MODULE := $(basename $(notdir $1))
    LOCAL_SRC_FILES := $1
    LOCAL_MODULE_TAGS := optional
    LOCAL_MODULE_SUFFIX := $(suffix $1)
    LOCAL_MODULE_CLASS := SHARED_LIBRARIES
    LOCAL_MODULE_PATH := $$(TARGET_OUT)$(abspath /$(dir $1))
    LOCAL_STRIP_MODULE := false
    OVERRIDE_BUILT_MODULE_PATH := $$(TARGET_OUT_INTERMEDIATE_LIBRARIES)
    include $$(BUILD_PREBUILT)
endef

prebuilt_wfd_libs := \
    lib/libwfdservice.so \
    lib/libwfd_mpeg2tsrtp.so \

prebuilt_wfd_modules := \
    $(foreach _file,$(prebuilt_wfd_libs),$(notdir $(basename $(_file))))



include $(CLEAR_VARS)
LOCAL_MODULE := ti_wfd_libs
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := $(prebuilt_wfd_modules)
include $(BUILD_PHONY_PACKAGE)

$(foreach _file,$(prebuilt_wfd_libs),\
  $(eval $(call _add-wfd-lib,$(_file))))

prebuilt_wfd_modules :=
prebuilt_wfd_libs :=
_add-wfd-lib :=

endif
