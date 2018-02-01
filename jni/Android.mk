# Use hardware GPS implementation if available.
#

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

APP_ALLOW_MISSING_DEPS=true

#LOCAL_C_INCLUDES += /path/to/marshmallow/system/core/include
#LOCAL_C_INCLUDES += /path/to/marshmallow/hardware/libhardware/include
LOCAL_C_INCLUDES += /home/joerg/Development/tinker/LineageOS/system/core/include
LOCAL_C_INCLUDES += /home/joerg/Development/tinker/LineageOS/hardware/libhardware/include

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_CFLAGS += -DHAVE_GPS_HARDWARE
LOCAL_SHARED_LIBRARIES := libhardware libc libutils
LOCAL_LDLIBS := -llog  -L$(LOCAL_PATH) -lcutils
LOCAL_SRC_FILES := gps.c
#LOCAL_MODULE := gps.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE := gps.default
LOCAL_MODULE_FILENAME := gps.default
LOCAL_MODULE_TAGS := debug eng
include $(BUILD_SHARED_LIBRARY)

