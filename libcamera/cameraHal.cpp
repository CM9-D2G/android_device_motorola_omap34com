/* vim:et:sts=4:sw=4
 *
 * Copyright (C) 2012, The Android Open Source Project
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

#define LOG_TAG "CameraHAL"
//#define LOG_NDEBUG 0
//#define LOG_FULL_PARAMS
//#define LOG_EACH_FRAME

#include <binder/IMemory.h>
#include <hardware/camera.h>
#include <hardware/gralloc.h>
#include <camera/Overlay.h>
#include <utils/Errors.h>
#include <vector>
#include <ctype.h>

#define CLAMP(x, l, h)  (((x) > (h)) ? (h) : (((x) < (l)) ? (l) : (x)))
#define USAGE_WIN \
    (GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN)
#define USAGE_BUF \
    (GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER)

#ifdef LOG_EACH_FRAME
# define LOGVF(...) LOGV(__VA_ARGS__)
#else
# define LOGVF(...)
#endif

using namespace std;

#include "MotoCameraWrapper.h"

namespace android {

static long long mLastPreviewTime = 0;
static bool mThrottlePreview = false;
static bool mPreviousVideoFrameDropped = false;
static int mNumVideoFramesDropped = 0;

/* When the media encoder is not working fast enough,
   the number of allocated but yet unreleased frames
   in memory could start to grow without limit.

   Three thresholds are used to deal with such condition.
   First, if the number of frames gets over the PREVIEW_THROTTLE_THRESHOLD,
   just limit the preview framerate to relieve the CPU to help the encoder
   to catch up.
   If it is not enough and the number gets also over the SOFT_DROP_THRESHOLD,
   start dropping new frames coming from camera, but only one at a time,
   never two consecutive ones.
   If the number gets even over the HARD_DROP_THRESHOLD, drop the frames
   without further conditions. */

const unsigned int PREVIEW_THROTTLE_THRESHOLD = 6;
const unsigned int SOFT_DROP_THRESHOLD = 12;
const unsigned int HARD_DROP_THRESHOLD = 15;

struct legacy_camera_device {
    camera_device_t device;
    int id;

    /* New world */
    camera_notify_callback         notify_callback;
    camera_data_callback           data_callback;
    camera_data_timestamp_callback data_timestamp_callback;
    camera_request_memory          request_memory;
    void                           *user;
    preview_stream_ops             *window;

    /* Old world */
    sp<CameraHardwareInterface>    hwif;
    gralloc_module_t const         *gralloc;
    camera_memory_t*               clientData;
    vector<camera_memory_t*>       sentFrames;
    sp<Overlay>                    overlay;

    int32_t                        previewWidth;
    int32_t                        previewHeight;
    Overlay::Format                previewFormat;
};

static inline void YUYVtoRGB565(char *rgb, char *yuyv, int width,
                                int height, int stride)
{
    int row, pos;
    int yuvIndex = 0;
    int rgbIndex = 0;
    int y1, u, y2, v;
    int r, g, b;
    int yy1, yy2, uv, uu, vv;
    int padding = (stride - width) * 2;

    for (row = 0; row < height; row++) {
        for (pos = 0; pos < width / 2; pos++) {
            y1 = (0xff & yuyv[yuvIndex++]) - 16;
            u  = (0xff & yuyv[yuvIndex++]) - 128;
            y2 = (0xff & yuyv[yuvIndex++]) - 16;
            v  = (0xff & yuyv[yuvIndex++]) - 128;

            if (y1 < 0) y1 = 0;
            if (y2 < 0) y2 = 0;

            yy1 = 1192 * y1;
            yy2 = 1192 * y2;
            uv = 833 * v + 400 * u;
            uu = 2066 * u;
            vv = 1634 * v;

            r = CLAMP(yy1 + vv, 0, 262143);
            g = CLAMP(yy1 - uv, 0, 262143);
            b = CLAMP(yy1 + uu, 0, 262143);

            rgb[rgbIndex++] = ((g >> 7) & 0xe0) | ((b >> 13) & 0x1f);
            rgb[rgbIndex++] = ((r >> 10) & 0xf8) | ((g >> 15) & 0x07);

            r = CLAMP(yy2 + vv, 0, 262143);
            g = CLAMP(yy2 - uv, 0, 262143);
            b = CLAMP(yy2 + uu, 0, 262143);

            rgb[rgbIndex++] = ((g >> 7) & 0xe0) | ((b >> 13) & 0x1f);
            rgb[rgbIndex++] = ((r >> 10) & 0xf8) | ((g >> 15) & 0x07);
        }
        rgbIndex += padding;
    }
}

static inline void YUV420spToRGB565(char* rgb, char* yuv420sp, int width,
                                    int height, int stride)
{
    int frameSize = width * height;
    int padding = (stride - width) * 2;
    int r, g, b;
    for (int j = 0, yp = 0, k = 0; j < height; j++) {
        int uvp = frameSize + (j >> 1) * width, u = 0, v = 0;
        for (int i = 0; i < width; i++, yp++) {
            int y = (0xff & ((int) yuv420sp[yp])) - 16;
            if (y < 0) y = 0;
            if ((i & 1) == 0) {
                v = (0xff & yuv420sp[uvp++]) - 128;
                u = (0xff & yuv420sp[uvp++]) - 128;
            }

            int y1192 = 1192 * y;
            r = CLAMP(y1192 + 1634 * v, 0, 262143);
            g = CLAMP(y1192 - 833 * v - 400 * u, 0, 262143);
            b = CLAMP(y1192 + 2066 * u, 0, 262143);

            rgb[k++] = ((g >> 7) & 0xe0) | ((b >> 13) & 0x1f);
            rgb[k++] = ((r >> 10) & 0xf8) | ((g >> 15) & 0x07);
        }
        k += padding;
    }
}

/* Overlay hooks */
static void processPreviewData(char *frame, size_t size,
                               legacy_camera_device *lcdev,
                               Overlay::Format format)
{
    buffer_handle_t *bufHandle = NULL;
    preview_stream_ops *window = NULL;
    int32_t stride;
    void *vaddr;
    int ret;

    LOGVF("%s: frame=%p, size=%d, lcdev=%p", __FUNCTION__, frame, size, lcdev);
    if (lcdev->window == NULL) {
        return;
    }

    window = lcdev->window;
    ret = window->dequeue_buffer(window, &bufHandle, &stride);
    if (ret != NO_ERROR) {
        LOGW("%s: ERROR dequeueing the buffer\n", __FUNCTION__);
        return;
    }

    ret = window->lock_buffer(window, bufHandle);
    if (ret != NO_ERROR) {
        LOGE("%s: ERROR locking the buffer\n", __FUNCTION__);
        window->cancel_buffer(window, bufHandle);
        return;
    }

    ret = lcdev->gralloc->lock(lcdev->gralloc, *bufHandle, USAGE_BUF,
                             0, 0, lcdev->previewWidth, lcdev->previewHeight,
                             &vaddr);
    if (ret != NO_ERROR) {
        LOGE("%s: could not lock gralloc buffer", __FUNCTION__);
        return;
    }

    switch (format) {
        case Overlay::FORMAT_YUV422I:
            YUYVtoRGB565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight, stride);
            break;
        case Overlay::FORMAT_YUV420P:
        case Overlay::FORMAT_YUV420SP:
            YUV420spToRGB565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight, stride);
            break;
        case Overlay::FORMAT_RGB565:
            memcpy(vaddr, frame, size);
            break;
        default:
            LOGE("%s: Unknown video format, cannot convert!", __FUNCTION__);
    }
    lcdev->gralloc->unlock(lcdev->gralloc, *bufHandle);

    ret = window->enqueue_buffer(window, bufHandle);
    if (ret != NO_ERROR) {
        LOGE("%s: could not enqueue gralloc buffer", __FUNCTION__);
    }
}

static void overlayQueueBuffer(void *data, void *buffer, size_t size)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) data;
    long long now = systemTime();

    if ((now - mLastPreviewTime) < (mThrottlePreview ? 200000000 : 60000000)) {
        return;
    }

    if (data == NULL || buffer == NULL) {
        return;
    }

    mLastPreviewTime = now;
    Overlay::Format format = (Overlay::Format) lcdev->overlay->getFormat();
    processPreviewData((char*)buffer, size, lcdev, format);
}

camera_memory_t* GenClientData(const sp<IMemory> &dataPtr,
                                         legacy_camera_device *lcdev)
{
    ssize_t offset;
    size_t size;
    void *data;
    camera_memory_t *clientData = NULL;
    sp<IMemoryHeap> mHeap;

    mHeap = dataPtr->getMemory(&offset, &size);
    data = (void *)((char *)(mHeap->base()) + offset);

    clientData = lcdev->request_memory(-1, size, 1, lcdev->user);
    if (clientData != NULL) {
        LOGVF("%s: clientData=%p clientData->data=%p", __FUNCTION__,
              clientData, clientData->data);
        memcpy(clientData->data, data, size);
    } else {
        LOGW("%s: ERROR allocating memory from client", __FUNCTION__);
    }

    return clientData;
}

void CameraHAL_DataCb(int32_t msgType, const sp<IMemory>& dataPtr,
                      void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) user;
    camera_memory_t *mem = NULL;
    sp<IMemoryHeap> mHeap;
    ssize_t offset;
    size_t  size;
    char *buffer;

    LOGVF("%s: msgType:%d user:%p", __FUNCTION__, msgType, user);
    if (lcdev->data_callback && lcdev->request_memory) {
        if (lcdev->clientData != NULL) {
            lcdev->clientData->release(lcdev->clientData);
        }
        lcdev->clientData = GenClientData(dataPtr, lcdev);
        if (lcdev->clientData != NULL) {
             LOGVF("%s: Posting data to client", __FUNCTION__);
             lcdev->data_callback(msgType, lcdev->clientData, 0, NULL, lcdev->user);
        }
    }

    if (msgType == CAMERA_MSG_PREVIEW_FRAME && lcdev->overlay == NULL) {
        mHeap = dataPtr->getMemory(&offset, &size);
        buffer = (char*)mHeap->getBase() + offset;
        LOGVF("%s: preview size = %dx%d", __FUNCTION__,
              lcdev->previewWidth, lcdev->previewHeight);
        processPreviewData(buffer, size, lcdev, lcdev->previewFormat);
    }
}

void CameraHAL_DataTSCb(nsecs_t timestamp, int32_t msg_type,
                         const sp<IMemory>& dataPtr, void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) user;
    camera_memory_t *mem = NULL;
    int framesSent = 0;

    LOGVF("%s: timestamp:%lld msg_type:%d user:%p",
          __FUNCTION__, timestamp /1000, msg_type, user);

    framesSent = lcdev->sentFrames.size();
    if (framesSent > PREVIEW_THROTTLE_THRESHOLD) {
        mThrottlePreview = true;
        LOGW("%s: preview throttled (fr. queued/throttle thres.: %d/%d)",
             __FUNCTION__, framesSent, PREVIEW_THROTTLE_THRESHOLD);
        if ((mPreviousVideoFrameDropped == false && framesSent > SOFT_DROP_THRESHOLD) ||
             framesSent > HARD_DROP_THRESHOLD)
        {
            LOGW("Frame has to be dropped! (fr. queued/soft thres./hard thres.: %d/%d/%d)",
                 framesSent, SOFT_DROP_THRESHOLD, HARD_DROP_THRESHOLD);
            mPreviousVideoFrameDropped = true;
            mNumVideoFramesDropped++;
            lcdev->hwif->releaseRecordingFrame(dataPtr);
            return;
        }
    } else {
        mThrottlePreview = false;
    }

    if (lcdev->data_timestamp_callback == NULL ||
        lcdev->request_memory == NULL) {
        return;
    }

    mem = GenClientData(dataPtr, lcdev);
    if (mem != NULL) {
        mPreviousVideoFrameDropped = false;
        LOGVF("%s: Posting data to client timestamp:%lld",
              __FUNCTION__, systemTime());
        lcdev->sentFrames.push_back(mem);
        lcdev->data_timestamp_callback(timestamp, msg_type, mem, 0, lcdev->user);
        lcdev->hwif->releaseRecordingFrame(dataPtr);
    } else {
        LOGV("%s: ERROR allocating memory from client", __FUNCTION__);
    }
}

/* HAL helper functions. */
void CameraHAL_NotifyCb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) user;
    LOGVF("%s: msg_type:%d ext1:%d ext2:%d user:%p",
          __FUNCTION__, msgType, ext1, ext2, user);
    if (lcdev->notify_callback != NULL) {
        lcdev->notify_callback(msg_type, ext1, ext2, lcdev->user);
    }
}

inline void destroyOverlay(legacy_camera_device *lcdev)
{
    if (lcdev->overlay != NULL) {
        lcdev->overlay.clear();
        if (lcdev->hwif != NULL) {
            lcdev->hwif->setOverlay(lcdev->overlay);
        }
    }
}

static void releaseCameraFrames(legacy_camera_device *lcdev)
{
    vector<camera_memory_t*>::iterator it;
    for (it = lcdev->sentFrames.begin(); it < lcdev->sentFrames.end(); ++it) {
        camera_memory_t *mem = *it;
        LOGVF("%s: releasing mem->data:%p", __FUNCTION__, mem->data);
        mem->release(mem);
    }
    lcdev->sentFrames.clear();
}

/* Hardware Camera interface handlers. */
int camera_set_preview_window(struct camera_device *device,
                              struct preview_stream_ops *window)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    int rv = -EINVAL;
    int min_bufs = -1;
    int kBufferCount;

    LOGV("%s: Window:%p", __FUNCTION__, window);
    if (device == NULL) {
        LOGE("%s: Invalid device.", __FUNCTION__);
        return rv;
    }

    if (lcdev->window == window && window) {
        LOGV("%s: reconfiguring window", __FUNCTION__);
        destroyOverlay(lcdev);
    }

    lcdev->window = window;
    if (window == NULL) {
        LOGV("%s: releasing previous window", __FUNCTION__);
        destroyOverlay(lcdev);
        return NO_ERROR;
    }

    LOGV("%s : OK window is %p", __FUNCTION__, window);
    if (!lcdev->gralloc) {
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                          (const hw_module_t **)&(lcdev->gralloc))) {
            LOGE("%s: Fail on loading gralloc HAL", __FUNCTION__);
        }
    }

    if (window->get_min_undequeued_buffer_count(window, &min_bufs)) {
        LOGE("%s: could not retrieve min undequeued buffer count", __FUNCTION__);
        return -1;
    }

    kBufferCount = min_bufs + 2;
    LOGD("%s: setting buffer count to %i", __FUNCTION__, kBufferCount);
    if (window->set_buffer_count(window, kBufferCount)) {
        LOGE("%s: could not set buffer count", __FUNCTION__);
        return -1;
    }

    CameraParameters params(lcdev->hwif->getParameters());
    params.getPreviewSize(&lcdev->previewWidth, &lcdev->previewHeight);
    const char *previewFormat = params.getPreviewFormat();
    LOGD("%s: preview format %s", __FUNCTION__, previewFormat);
    lcdev->previewFormat = Overlay::getFormatFromString(previewFormat);

    if (window->set_usage(window, USAGE_WIN)) {
        LOGE("%s: could not set usage on window buffer", __FUNCTION__);
        return -1;
    }

    if (window->set_buffers_geometry(window, lcdev->previewWidth,
                                     lcdev->previewHeight,
                                     HAL_PIXEL_FORMAT_RGB_565)) {
        LOGE("%s: could not set buffers geometry", __FUNCTION__);
        return -1;
    }

    if (lcdev->hwif->useOverlay()) {
        lcdev->overlay = new Overlay(lcdev->previewWidth,
                                     lcdev->previewHeight,
                                     Overlay::FORMAT_YUV422I,
                                     overlayQueueBuffer,
                                     (void *) lcdev);
        lcdev->hwif->setOverlay(lcdev->overlay);
    } else {
        LOGW("%s: Not using overlay !", __FUNCTION__);
    }

    return NO_ERROR;
}

void camera_set_callbacks(struct camera_device *device,
                             camera_notify_callback notify_cb,
                             camera_data_callback data_cb,
                             camera_data_timestamp_callback data_cb_timestamp,
                             camera_request_memory get_memory,
                             void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    lcdev->notify_callback = notify_cb;
    lcdev->data_callback = data_cb;
    lcdev->data_timestamp_callback = data_cb_timestamp;
    lcdev->request_memory = get_memory;
    lcdev->user = user;
    lcdev->hwif->setCallbacks(CameraHAL_NotifyCb,
                              CameraHAL_DataCb,
                              CameraHAL_DataTSCb,
                              (void *) lcdev);
}

void camera_enable_msg_type(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    LOGV("%s: msg_type:%d\n", __FUNCTION__, msg_type);
    lcdev->hwif->enableMsgType(msg_type);
}

void camera_disable_msg_type(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    LOGV("%s: msg_type:%d\n", __FUNCTION__, msg_type);
    if (msg_type == CAMERA_MSG_VIDEO_FRAME) {
        LOGV("%s: releasing stale video frames", __FUNCTION__);
        releaseCameraFrames(lcdev);
    }
    lcdev->hwif->disableMsgType(msg_type);
}

int camera_msg_type_enabled(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    LOGV("%s: msgType:%d\n", __FUNCTION__, msg_type);
    return lcdev->hwif->msgTypeEnabled(msg_type);
}

int camera_start_preview(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->startPreview();
}

void camera_stop_preview(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    lcdev->hwif->stopPreview();
}

int camera_preview_enabled(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->previewEnabled();
}

int camera_store_meta_data_in_buffers(struct camera_device *device, int enable)
{
    return -EINVAL;
}

int camera_start_recording(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    mNumVideoFramesDropped = 0;
    mPreviousVideoFrameDropped = false;
    mThrottlePreview = false;
    return lcdev->hwif->startRecording();
}

void camera_stop_recording(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    LOGI("%s: Number of frames dropped by CameraHAL: %d", __FUNCTION__, mNumVideoFramesDropped);
    mThrottlePreview = false;
    lcdev->hwif->stopRecording();
}

int camera_recording_enabled(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->recordingEnabled() ? 1 : 0;
}

void camera_release_recording_frame(struct camera_device *device,
                                    const void *opaque)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;

    if (!opaque) {
        return;
    }

    LOGV("%s: opaque: %p", __FUNCTION__, opaque);
    vector<camera_memory_t*>::iterator it;
    for (it = lcdev->sentFrames.begin(); it != lcdev->sentFrames.end(); ++it) {
        camera_memory_t *mem = *it;
        if (mem->data == opaque) {
            LOGV("%s: found, removing", __FUNCTION__);
            mem->release(mem);
            lcdev->sentFrames.erase(it);
            break;
        }
    }
    return;
}

int camera_auto_focus(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->autoFocus();
}

int camera_cancel_auto_focus(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->cancelAutoFocus();
}

int camera_take_picture(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->takePicture();
}

int camera_cancel_picture(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->cancelPicture();
}

int camera_set_parameters(struct camera_device *device,
                          const char *params)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    String8 s(params);
    CameraParameters p(s);
#ifdef LOG_FULL_PARAMS
    p.dump();
#endif
    return lcdev->hwif->setParameters(p);
}

char *camera_get_parameters(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    CameraParameters params(lcdev->hwif->getParameters());
    int width = 0, height = 0;
    float ratio = 0.0;

    params.getPictureSize(&width, &height);
    if (width > 0 && height > 0) {
        ratio = (height * 1.0) / width;
        if (ratio < 0.70 && width >= 640) {
            params.setPreviewSize(848, 480);
            params.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "848x480");
        } else if (width == 848) {
            params.setPreviewSize(640, 480);
            params.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "640x480");
        }
    }

    params.getPreviewSize(&width, &height);
    if (width != lcdev->previewWidth || height != lcdev->previewHeight) {
        camera_set_preview_window(device, lcdev->window);
    }

#ifdef LOG_FULL_PARAMS
    params.dump();
#endif
    return strdup(params.flatten().string());
}

void camera_put_parameters(struct camera_device *device, char *params)
{
    if (params != NULL) {
        free(params);
    }
}

int camera_send_command(struct camera_device *device,
                        int32_t cmd, int32_t arg0, int32_t arg1)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    LOGV("%s: cmd:%d arg0:%d arg1:%d\n", __FUNCTION__, cmd, arg0, arg1);
    return lcdev->hwif->sendCommand(cmd, arg0, arg1);
}

void camera_release(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    destroyOverlay(lcdev);
    releaseCameraFrames(lcdev);

    if (lcdev->clientData != NULL) {
        lcdev->clientData->release(lcdev->clientData);
    }

    lcdev->hwif->release();
    lcdev->hwif.clear();
}

int camera_dump(struct camera_device *device, int fd)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    Vector<String16> args;
    return lcdev->hwif->dump(fd, args);
}

int camera_device_close(hw_device_t* device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    if (lcdev != NULL) {
        if (lcdev->device.ops != NULL)
            free(lcdev->device.ops);
        destroyOverlay(lcdev);
        free(lcdev);
    }
    return NO_ERROR;
}

int camera_device_open(const hw_module_t* module, const char *name,
                       hw_device_t** device)
{
    int ret = NO_ERROR;
    int cameraId;
    struct legacy_camera_device *lcdev;
    camera_device_t* camera_device;
    camera_device_ops_t* camera_ops;

    if (name == NULL) {
        return ret;
    }

    cameraId = atoi(name);
    LOGD("%s: name:%s device:%p cameraId:%d\n", __FUNCTION__, name, device, cameraId);

    lcdev = (legacy_camera_device *)malloc(sizeof(*lcdev));
    if (lcdev == NULL) {
        return -ENOMEM;
    }

    camera_ops = (camera_device_ops_t *)malloc(sizeof(*camera_ops));
    if (camera_ops == NULL) {
        free(lcdev);
        return -ENOMEM;
    }

    memset(lcdev, 0, sizeof(*lcdev));
    memset(camera_ops, 0, sizeof(*camera_ops));
    lcdev->device.common.tag               = HARDWARE_DEVICE_TAG;
    lcdev->device.common.version           = 0;
    lcdev->device.common.module            = (hw_module_t *)(module);
    lcdev->device.common.close             = camera_device_close;
    lcdev->device.ops                      = camera_ops;

    camera_ops->set_preview_window         = camera_set_preview_window;
    camera_ops->set_callbacks              = camera_set_callbacks;
    camera_ops->enable_msg_type            = camera_enable_msg_type;
    camera_ops->disable_msg_type           = camera_disable_msg_type;
    camera_ops->msg_type_enabled           = camera_msg_type_enabled;
    camera_ops->start_preview              = camera_start_preview;
    camera_ops->stop_preview               = camera_stop_preview;
    camera_ops->preview_enabled            = camera_preview_enabled;
    camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
    camera_ops->start_recording            = camera_start_recording;
    camera_ops->stop_recording             = camera_stop_recording;
    camera_ops->recording_enabled          = camera_recording_enabled;
    camera_ops->release_recording_frame    = camera_release_recording_frame;
    camera_ops->auto_focus                 = camera_auto_focus;
    camera_ops->cancel_auto_focus          = camera_cancel_auto_focus;
    camera_ops->take_picture               = camera_take_picture;
    camera_ops->cancel_picture             = camera_cancel_picture;
    camera_ops->set_parameters             = camera_set_parameters;
    camera_ops->get_parameters             = camera_get_parameters;
    camera_ops->put_parameters             = camera_put_parameters;
    camera_ops->send_command               = camera_send_command;
    camera_ops->release                    = camera_release;
    camera_ops->dump                       = camera_dump;

    lcdev->id = cameraId;
    lcdev->hwif = MotoCameraWrapper::createInstance(cameraId);
    if (lcdev->hwif == NULL) {
        free(camera_ops);
        free(lcdev);
        return -EIO;
    }

    *device = &lcdev->device.common;
    return NO_ERROR;
}

static int get_number_of_cameras(void)
{
    return 1;
}

static int get_camera_info(int camera_id, struct camera_info *info)
{
    info->facing = CAMERA_FACING_BACK;
    info->orientation = 90;
    LOGD("%s: id:%i faceing:%i orientation: %i", __FUNCTION__,
          camera_id, info->facing, info->orientation);
    return 0;
}

} /* namespace android */

static hw_module_methods_t camera_module_methods = {
    open: android::camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 1,
        id: CAMERA_HARDWARE_MODULE_ID,
        name: "Camera HAL for ICS/CM9",
        author: "Won-Kyu Park, Raviprasad V Mummidi, Ivan Zupan, Epsylon3, rondoval",
        methods: &camera_module_methods,
        dso: NULL,
        reserved: {0},
    },
    get_number_of_cameras: android::get_number_of_cameras,
    get_camera_info: android::get_camera_info,
};
