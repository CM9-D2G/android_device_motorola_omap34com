/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "MotoCameraWrapper"

#include <cmath>
#include <dlfcn.h>
#include <fcntl.h>
#include <cutils/properties.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <camera/Camera.h>
#include "MotoCameraWrapper.h"

namespace android {

wp<MotoCameraWrapper> MotoCameraWrapper::singleton;

static bool deviceCardMatches(const char *device, const char *matchCard)
{
    struct v4l2_capability caps;
    int fd = ::open(device, O_RDWR);
    bool ret;

    if (fd < 0) {
        return false;
    }

    if (::ioctl(fd, VIDIOC_QUERYCAP, &caps) < 0) {
        ret = false;
    } else {
        const char *card = (const char *) caps.card;

        LOGD("device %s card is %s\n", device, card);
        ret = strstr(card, matchCard) != NULL;
    }

    ::close(fd);

    return ret;
}

static sp<CameraHardwareInterface>
openMotoInterface(const char *libName, const char *funcName)
{
    sp<CameraHardwareInterface> interface;
    void *libHandle = ::dlopen(libName, RTLD_NOW);

    if (libHandle != NULL) {
        typedef sp<CameraHardwareInterface> (*OpenCamFunc)();
        OpenCamFunc func = (OpenCamFunc) ::dlsym(libHandle, funcName);
        if (func != NULL) {
            interface = func();
        } else {
            LOGE("Could not find library entry point!");
        }
    } else {
        LOGE("dlopen() error: %s\n", dlerror());
    }

    return interface;
}

static void setSocTorchMode(bool enable)
{
    int fd = ::open("/sys/class/leds/torch-flash/flash_light", O_WRONLY);
    if (fd >= 0) {
        const char *value = enable ? "100" : "0";
        write(fd, value, strlen(value));
        close(fd);
    }
}

sp<MotoCameraWrapper> MotoCameraWrapper::createInstance(int cameraId)
{
    if (singleton != NULL) {
        sp<MotoCameraWrapper> hardware = singleton.promote();
        if (hardware != NULL) {
            return hardware;
        }
    }

    CameraType type = UNKNOWN;
    sp<CameraHardwareInterface> motoInterface;
    sp<MotoCameraWrapper> hardware;
    char motodevice[PROPERTY_VALUE_MAX];

    /* Device detection */
    property_get("ro.product.device", motodevice, "");
    if (strcmp(motodevice, "shadow") == 0) {
        type = DROIDX;
    } else if (strcmp(motodevice, "droid2") == 0) {
        type = DROID2;
    } else if (strcmp(motodevice, "droid2we") == 0) {
        type = DROID2WE;
    } else if (strcmp(motodevice, "venus2") == 0) {
        type = DROIDPRO;
    } else if (strcmp(motodevice, "jordan") == 0) {
        if (deviceCardMatches("/dev/video3", "camise")) {
            type = DEFY_GREEN;
        } else if (deviceCardMatches("/dev/video0", "mt9p012")) {
            type = DEFY_RED;
        }
    }

    /* Load our Gingerbread library entry point */
    switch (type) {
        case DROIDX:
        case DROID2:
        case DROID2WE:
        case DROIDPRO:
            motoInterface = openMotoInterface("libcamera.so", "_ZN7android9CameraHal14createInstanceEv");
            break;
        case DEFY_RED:
            motoInterface = openMotoInterface("libbayercamera.so", "_ZN7android9CameraHal14createInstanceEv");
            break;
        case DEFY_GREEN:
            motoInterface = openMotoInterface("libsoccamera.so", "_ZN7android16CameraHalSocImpl14createInstanceEv");
            break;
        case UNKNOWN:
        default:
            break;
    }

    if (motoInterface != NULL) {
        hardware = new MotoCameraWrapper(motoInterface, type);
        singleton = hardware;
    } else {
        LOGE("Could not open hardware interface");
    }

    return hardware;
}

MotoCameraWrapper::MotoCameraWrapper(sp<CameraHardwareInterface>& motoInterface, CameraType type) :
    mMotoInterface(motoInterface),
    mCameraType(type),
    mVideoMode(false),
    mNotifyCb(NULL),
    mDataCb(NULL),
    mDataCbTimestamp(NULL),
    mCbUserData(NULL)
{
    if (type == DEFY_GREEN)
        mTorchThread = new TorchEnableThread(this);
}

MotoCameraWrapper::~MotoCameraWrapper()
{
    if (mCameraType == DEFY_GREEN) {
        setSocTorchMode(false);
        mTorchThread->cancelAndWait();
        mTorchThread.clear();
    }
}

void
MotoCameraWrapper::toggleTorchIfNeeded()
{
    if (mCameraType == DEFY_GREEN)
        setSocTorchMode(mFlashMode == CameraParameters::FLASH_MODE_TORCH);
}

sp<IMemoryHeap> MotoCameraWrapper::getPreviewHeap() const
{
    return mMotoInterface->getPreviewHeap();
}

sp<IMemoryHeap> MotoCameraWrapper::getRawHeap() const
{
    return mMotoInterface->getRawHeap();
}

void MotoCameraWrapper::setCallbacks(notify_callback notify_cb,
                                     data_callback data_cb,
                                     data_callback_timestamp data_cb_timestamp,
                                     void* user)
{
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCbUserData = user;

    if (mNotifyCb != NULL)
        notify_cb = &MotoCameraWrapper::notifyCb;

    if (mDataCb != NULL)
        data_cb = &MotoCameraWrapper::dataCb;

    if (mDataCbTimestamp != NULL)
        data_cb_timestamp = &MotoCameraWrapper::dataCbTimestamp;

    mMotoInterface->setCallbacks(notify_cb, data_cb, data_cb_timestamp, this);
}

void MotoCameraWrapper::notifyCb(int32_t msgType, int32_t ext1, int32_t ext2, void* user)
{
    MotoCameraWrapper *_this = (MotoCameraWrapper *) user;
    user = _this->mCbUserData;

    if (msgType == CAMERA_MSG_FOCUS)
        _this->toggleTorchIfNeeded();

    _this->mNotifyCb(msgType, ext1, ext2, user);
}

void MotoCameraWrapper::dataCb(int32_t msgType, const sp<IMemory>& dataPtr, void* user)
{
    MotoCameraWrapper *_this = (MotoCameraWrapper *) user;
    user = _this->mCbUserData;

    if (msgType == CAMERA_MSG_COMPRESSED_IMAGE)
        _this->fixUpBrokenGpsLatitudeRef(dataPtr);

    _this->mDataCb(msgType, dataPtr, user);

    if (msgType == CAMERA_MSG_RAW_IMAGE ||
        msgType == CAMERA_MSG_COMPRESSED_IMAGE) {
        if (_this->mTorchThread != NULL)
            _this->mTorchThread->scheduleTorch();
    }
 }

void MotoCameraWrapper::dataCbTimestamp(nsecs_t timestamp, int32_t msgType,
                                        const sp<IMemory>& dataPtr, void* user)
{
    MotoCameraWrapper *_this = (MotoCameraWrapper *) user;
    user = _this->mCbUserData;

    _this->mDataCbTimestamp(timestamp, msgType, dataPtr, user);
}

/*
 * Motorola's libcamera fails in writing the GPS latitude reference
 * tag properly. Instead of writing 'N' or 'S', it writes 'W' or 'E'.
 * Below is a very hackish workaround for that: We search for the GPS
 * latitude reference tag by pattern matching into the first couple of
 * data bytes. As the output format of Motorola's libcamera is static,
 * this should be fine until Motorola fixes their lib.
 */
void MotoCameraWrapper::fixUpBrokenGpsLatitudeRef(const sp<IMemory>& dataPtr)
{
    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap = dataPtr->getMemory(&offset, &size);
    uint8_t *data = (uint8_t*)heap->base();

    if (data) {
        data += offset;

        /* scan first 512 bytes for GPS latitude ref marker */
        static const unsigned char sLatitudeRefMarker[] = {
            0x01, 0x00, /* GPS Latitude ref tag */
            0x02, 0x00, /* format: string */
            0x02, 0x00, 0x00, 0x00 /* 2 bytes long */
        };

        for (size_t i = 0; i < 512 && i < (size - 10); i++) {
            if (memcmp(data + i, sLatitudeRefMarker, sizeof(sLatitudeRefMarker)) == 0) {
                char *ref = (char *) (data + i + sizeof(sLatitudeRefMarker));
                if ((*ref == 'W' || *ref == 'E') && *(ref + 1) == '\0') {
                    LOGI("Found broken GPS latitude ref marker, offset %d, item %c",
                         i + sizeof(sLatitudeRefMarker), *ref);
                    *ref = (*ref == 'W') ? 'N' : 'S';
                }
                break;
            }
        }
    }
}

void MotoCameraWrapper::enableMsgType(int32_t msgType)
{
    mMotoInterface->enableMsgType(msgType);
}

void MotoCameraWrapper::disableMsgType(int32_t msgType)
{
    mMotoInterface->disableMsgType(msgType);
}

bool MotoCameraWrapper::msgTypeEnabled(int32_t msgType)
{
    return mMotoInterface->msgTypeEnabled(msgType);
}

status_t MotoCameraWrapper::startPreview()
{
    return mMotoInterface->startPreview();
}

bool MotoCameraWrapper::useOverlay()
{
    return mMotoInterface->useOverlay();
}

status_t MotoCameraWrapper::setOverlay(const sp<Overlay> &overlay)
{
    return mMotoInterface->setOverlay(overlay);
}

void MotoCameraWrapper::stopPreview()
{
    mMotoInterface->stopPreview();
}

bool MotoCameraWrapper::previewEnabled()
{
    return mMotoInterface->previewEnabled();
}

status_t MotoCameraWrapper::startRecording()
{
    toggleTorchIfNeeded();
    return mMotoInterface->startRecording();
}

void MotoCameraWrapper::stopRecording()
{
    toggleTorchIfNeeded();
    mMotoInterface->stopRecording();
}

bool MotoCameraWrapper::recordingEnabled()
{
    return mMotoInterface->recordingEnabled();
}

void MotoCameraWrapper::releaseRecordingFrame(const sp<IMemory>& mem)
{
    return mMotoInterface->releaseRecordingFrame(mem);
}

status_t MotoCameraWrapper::autoFocus()
{
    return mMotoInterface->autoFocus();
}

status_t MotoCameraWrapper::cancelAutoFocus()
{
    return mMotoInterface->cancelAutoFocus();
}

status_t MotoCameraWrapper::takePicture()
{
    return mMotoInterface->takePicture();
}

status_t MotoCameraWrapper::cancelPicture()
{
    return mMotoInterface->cancelPicture();
}

status_t MotoCameraWrapper::setParameters(const CameraParameters& params)
{
    CameraParameters pars(params.flatten());
    String8 oldFlashMode = mFlashMode;
    String8 sceneMode;
    status_t retval;
    int width, height;
    char buf[10];
    bool isWide;

    /*
     * getInt returns -1 if the value isn't present and 0 on parse failure,
     * so if it's larger than 0, we can be sure the value was parsed properly
     */
    mVideoMode = pars.getInt("cam-mode") > 0;
    pars.remove("cam-mode");

    pars.getPreviewSize(&width, &height);
    isWide = width == 848 && height == 480;

    if (isWide && !mVideoMode)
        pars.setPreviewFrameRate(24);

    if (mCameraType == DEFY_RED && mVideoMode)
        pars.setPreviewFrameRate(24);

    sceneMode = pars.get(CameraParameters::KEY_SCENE_MODE);
    if (sceneMode != CameraParameters::SCENE_MODE_AUTO) {
        /* The lib doesn't seem to update the flash mode correctly when a scene
         * mode is set, so we need to do it here. Also do focus mode, just do
         * be on the safe side.
         */
        pars.set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);

        if (sceneMode == CameraParameters::SCENE_MODE_PORTRAIT ||
            sceneMode == CameraParameters::SCENE_MODE_NIGHT_PORTRAIT)
        {
            pars.set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_AUTO);
        } else {
            pars.set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
        }
    }

    mFlashMode = pars.get(CameraParameters::KEY_FLASH_MODE);
    float exposure = pars.getFloat(CameraParameters::KEY_EXPOSURE_COMPENSATION);
    /* Exposure-compensation comes multiplied in the -9...9 range, while
     * we need it in the -3...3 range -> adjust for that
     */
    exposure /= 3;

    /* format the setting in a way the lib understands */
    bool even = (exposure - round(exposure)) < 0.05;
    snprintf(buf, sizeof(buf), even ? "%.0f" : "%.2f", exposure);
    pars.set("mot-exposure-offset", buf);

    /* kill off the original setting */
    pars.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, "0");

    retval = mMotoInterface->setParameters(pars);

    if (oldFlashMode != mFlashMode) {
        toggleTorchIfNeeded();
    }

    return retval;
}

CameraParameters MotoCameraWrapper::getParameters() const
{
    CameraParameters ret = mMotoInterface->getParameters();

    /* cut down supported effects to values supported by framework */
    ret.set(CameraParameters::KEY_SUPPORTED_EFFECTS,
            "none,mono,sepia,negative,solarize,red-tint,green-tint,blue-tint");

    /* Motorola uses mot-exposure-offset instead of exposure-compensation
       for whatever reason -> adapt the values.
       The limits used here are taken from the lib, we surely also
       could parse it, but it's likely not worth the hassle */
    float exposure = ret.getFloat("mot-exposure-offset");
    int exposureParam = (int) round(exposure * 3);

    ret.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, exposureParam);
    ret.set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION, "9");
    ret.set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION, "-9");
    ret.set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, "0.3333333333333");
    ret.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT, CameraParameters::PIXEL_FORMAT_YUV422I);

    /* Device specific options */
    switch (mCameraType) {
        case DEFY_GREEN:
            /* The original zoom ratio string is '100,200,300,400,500,600',
             * but 500 and 600 are broken for the SOC camera, so limiting
             * it here
             */
            ret.set(CameraParameters::KEY_MAX_ZOOM, "3");
            ret.set(CameraParameters::KEY_ZOOM_RATIOS, "100,200,300,400");
            ret.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
                    "(1000,30000),(1000,25000),(1000,20000),(1000,24000),(1000,15000),(1000,10000)");
            ret.set(CameraParameters::KEY_PREVIEW_FPS_RANGE, "1000, 30000");
            break;
        case DEFY_RED:
            ret.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
                    "(1000,30000),(1000,25000),(1000,20000),(1000,24000),(1000,15000),(1000,10000)");
            ret.set(CameraParameters::KEY_PREVIEW_FPS_RANGE, "1000, 30000");
            break;
        default:
            break;
    }
    ret.set("cam-mode", mVideoMode ? "1" : "0");

    return ret;
}

status_t MotoCameraWrapper::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
    return mMotoInterface->sendCommand(cmd, arg1, arg2);
}

void MotoCameraWrapper::release()
{
    mMotoInterface->release();
}

status_t MotoCameraWrapper::dump(int fd, const Vector<String16>& args) const
{
    return mMotoInterface->dump(fd, args);
}

}; //namespace android
