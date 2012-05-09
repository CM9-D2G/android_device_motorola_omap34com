/*
 * Copyright (C) 2012, rondoval (ms2), Epsylon3 (defy)
 * Copyright (C) 2012, Won-Kyu Park
 * Copyright (C) 2012, Raviprasad V Mummidi
 * Copyright (C) 2011, Ivan Zupan
 * Copyright (C) 2012, JB1tz
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
#define LOG_FULL_PARAMS

#include <hardware/camera.h>
#include <ui/Overlay.h>
#include <binder/IMemory.h>
#include <hardware/gralloc.h>
#include <utils/Errors.h>
#include <vector>
#include <ctype.h>

#define CLAMP(x, l, h)  (((x) > (h)) ? (h) : (((x) < (l)) ? (l) : (x)))
#define CAMHAL_GRALLOC_USAGE GRALLOC_USAGE_HW_TEXTURE | \
			     GRALLOC_USAGE_HW_RENDER | \
			     GRALLOC_USAGE_SW_READ_RARELY | \
			     GRALLOC_USAGE_SW_WRITE_NEVER

using namespace std;

#include "MotoCameraWrapper.h"

namespace android {

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
    OverlayFormats                 previewFormat;
};

inline void YUYVtoRGB565(char *rgb, char *yuyv, int width,
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

/* Overlay hooks */
static void processPreviewData(char *frame, size_t size,
                               legacy_camera_device *lcdev,
                               OverlayFormats format)
{
    buffer_handle_t *bufHandle = NULL;
    preview_stream_ops *window = NULL;
    int32_t stride;
    void *vaddr;
    int ret;

    if (lcdev->window == NULL)
        return;

    window = lcdev->window;
    ret = window->dequeue_buffer(window, &bufHandle, &stride);
    if (ret != NO_ERROR) {
        LOGE("%s: ERROR dequeueing the buffer\n", __FUNCTION__);
        return;
    }

    ret = window->lock_buffer(window, bufHandle);
    if (ret != NO_ERROR) {
        LOGE("%s: ERROR locking the buffer\n", __FUNCTION__);
        window->cancel_buffer(window, bufHandle);
        return;
    }

    ret = lcdev->gralloc->lock(lcdev->gralloc, *bufHandle, CAMHAL_GRALLOC_USAGE,
                             0, 0, lcdev->previewWidth, lcdev->previewHeight,
                             &vaddr);
    if (ret != NO_ERROR)
        return;

    switch (format) {
        case OVERLAY_FORMAT_YUV422I:
            YUYVtoRGB565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight, stride);
            break;
        case OVERLAY_FORMAT_RGB565:
            memcpy(vaddr, frame, size);
            break;
        default:
            LOGE("%s: Unknown video format, cannot convert!", __FUNCTION__);
    }
    lcdev->gralloc->unlock(lcdev->gralloc, *bufHandle);

    ret = window->enqueue_buffer(window, bufHandle);
    if (ret != NO_ERROR)
        LOGE("%s: could not enqueue gralloc buffer", __FUNCTION__);
}

static void overlayQueueBuffer(void *data, void *buffer, size_t size)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) data;
    if (data == NULL || buffer == NULL)
        return;

    OverlayFormats format = (OverlayFormats) lcdev->overlay->getFormat();
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
    if (clientData)
        memcpy(clientData->data, data, size);

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

    if (lcdev->data_callback && lcdev->request_memory) {
        if (lcdev->clientData)
            lcdev->clientData->release(lcdev->clientData);
        lcdev->clientData = GenClientData(dataPtr, lcdev);
        if (lcdev->clientData)
             lcdev->data_callback(msgType, lcdev->clientData, 0, NULL, lcdev->user);
    }

    if (msgType == CAMERA_MSG_PREVIEW_FRAME && lcdev->overlay == NULL) {
        mHeap = dataPtr->getMemory(&offset, &size);
        buffer = (char*)mHeap->getBase() + offset;
        processPreviewData(buffer, size, lcdev, lcdev->previewFormat);
    }
}

void CameraHAL_DataTSCb(nsecs_t timestamp, int32_t msg_type,
                         const sp<IMemory>& dataPtr, void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) user;
    camera_memory_t *mem = NULL;

    if (lcdev->data_timestamp_callback == NULL ||
        lcdev->request_memory == NULL) {
        return;
    }

    mem = GenClientData(dataPtr, lcdev);
    if (mem) {
        lcdev->sentFrames.push_back(mem);
        lcdev->data_timestamp_callback(timestamp, msg_type, mem, 0, lcdev->user);
        lcdev->hwif->releaseRecordingFrame(dataPtr);
    }
}

/* HAL helper functions. */
void CameraHAL_NotifyCb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user)
{
    legacy_camera_device *lcdev = (legacy_camera_device *) user;
    if (lcdev->notify_callback)
        lcdev->notify_callback(msg_type, ext1, ext2, lcdev->user);
}

inline void destroyOverlay(legacy_camera_device *lcdev)
{
    if (lcdev->overlay != NULL) {
        lcdev->overlay.clear();
        if (lcdev->hwif != NULL)
            lcdev->hwif->setOverlay(lcdev->overlay);
    }
}

static void releaseCameraFrames(legacy_camera_device *lcdev)
{
    vector<camera_memory_t*>::iterator it;
    for (it = lcdev->sentFrames.begin(); it != lcdev->sentFrames.end(); it++)
        (*it)->release(*it);
    lcdev->sentFrames.clear();
}

/* Hardware Camera interface handlers. */
int camera_set_preview_window(struct camera_device *device,
                              struct preview_stream_ops *window)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    int rv = -EINVAL;
    int min_bufs = -1;
    const int kBufferCount = 6;

    if (!device)
        return rv;

    if (lcdev->window == window)
        destroyOverlay(lcdev);

    lcdev->window = window;
    if (!window) {
        destroyOverlay(lcdev);
        return NO_ERROR;
    }

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

    if (min_bufs >= kBufferCount) {
        LOGE("%s: min undequeued buffer count %i is too high"
             " (expecting at most %i)", __FUNCTION__, min_bufs, kBufferCount - 1);
    }

    LOGD("%s: setting buffer count to %i", __FUNCTION__, kBufferCount);
    if (window->set_buffer_count(window, kBufferCount)) {
        LOGE("%s: could not set buffer count", __FUNCTION__);
        return -1;
    }

    CameraParameters params(lcdev->hwif->getParameters());
    params.getPreviewSize(&lcdev->previewWidth, &lcdev->previewHeight);
    const char *previewFormat = params.getPreviewFormat();
    lcdev->previewFormat = getOverlayFormatFromString(previewFormat);

    if (window->set_usage(window, CAMHAL_GRALLOC_USAGE)) {
        LOGE("%s: could not set usage on gralloc buffer", __FUNCTION__);
        return -1;
    }

    if (window->set_buffers_geometry(window,
                                     lcdev->previewWidth,
                                     lcdev->previewHeight,
                                     HAL_PIXEL_FORMAT_RGB_565)) {
        LOGE("%s: could not set buffers geometry", __FUNCTION__);
        return -1;
    }

    if (lcdev->hwif->useOverlay()) {
        lcdev->overlay = new Overlay(lcdev->previewWidth,
                                     lcdev->previewHeight,
                                     OVERLAY_FORMAT_YUV422I,
                                     overlayQueueBuffer,
                                     (void *) lcdev);
        lcdev->hwif->setOverlay(lcdev->overlay);
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
    lcdev->hwif->enableMsgType(msg_type);
}

void camera_disable_msg_type(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;

    if (msg_type == CAMERA_MSG_VIDEO_FRAME)
         releaseCameraFrames(lcdev);

    lcdev->hwif->disableMsgType(msg_type);
}

int camera_msg_type_enabled(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
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
    return lcdev->hwif->startRecording();
}

void camera_stop_recording(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
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
    vector<camera_memory_t*>::iterator it;
    camera_memory_t *mem = NULL;

    if (!opaque)
        return;

    for (it = lcdev->sentFrames.begin(); it != lcdev->sentFrames.end(); it++) {
        mem = *it;
        if (mem->data == opaque) {
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
    if (params)
        free(params);
}

int camera_send_command(struct camera_device *device,
                        int32_t cmd, int32_t arg0, int32_t arg1)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->sendCommand(cmd, arg0, arg1);
}

void camera_release(struct camera_device *device)
{
    legacy_camera_device *lcdev = (legacy_camera_device*) device;
    destroyOverlay(lcdev);
    releaseCameraFrames(lcdev);

    if (lcdev->clientData)
        lcdev->clientData->release(lcdev->clientData);

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
    if (lcdev) {
        if (lcdev->device.ops)
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
    struct legacy_camera_device *lcdev;
    camera_device_t* camera_device;
    camera_device_ops_t* camera_ops;

    if (!name)
        return ret;

    int cameraId = atoi(name);
    LOGD("%s: name:%s device:%p cameraId:%d\n", __FUNCTION__, name, device, cameraId);

    lcdev = (legacy_camera_device *)malloc(sizeof(*lcdev));
    if (!lcdev)
        return -ENOMEM;

    camera_ops = (camera_device_ops_t *)malloc(sizeof(*camera_ops));
    if (!camera_ops) {
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
