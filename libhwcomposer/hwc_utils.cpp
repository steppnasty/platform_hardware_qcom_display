/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012, The Linux Foundation All rights reserved.
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

#include <EGL/egl.h>
#include <overlay.h>
#include <cutils/properties.h>
#include <gralloc_priv.h>
#include <fb_priv.h>
#include "hwc_utils.h"
#include "mdp_version.h"
#include "hwc_video.h"
#include "hwc_qbuf.h"
#include "hwc_copybit.h"
#include "external.h"
#include "hwc_mdpcomp.h"
#include "hwc_extonly.h"
#include "QService.h"

namespace qhwc {

// Opens Framebuffer device
static void openFramebufferDevice(hwc_context_t *ctx)
{
    hw_module_t const *module;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
        framebuffer_open(module, &(ctx->mFbDev));
    }
}

void initContext(hwc_context_t *ctx)
{
    openFramebufferDevice(ctx);
    ctx->mOverlay = overlay::Overlay::getInstance();
    ctx->mQService = qService::QService::getInstance(ctx);
    ctx->qbuf = new QueuedBufferStore();
    ctx->mMDP.version = qdutils::MDPVersion::getInstance().getMDPVersion();
    ctx->mMDP.hasOverlay = qdutils::MDPVersion::getInstance().hasOverlay();
    ctx->mMDP.panel = qdutils::MDPVersion::getInstance().getPanelType();
    ctx->mExtDisplay = new ExternalDisplay(ctx);
    memset(ctx->dpys,(int)EGL_NO_DISPLAY, MAX_NUM_DISPLAYS);
    MDPComp::init(ctx);

    ALOGI("Initializing Qualcomm Hardware Composer");
    ALOGI("MDP version: %d", ctx->mMDP.version);
}

void closeContext(hwc_context_t *ctx)
{
    if(ctx->mOverlay) {
        delete ctx->mOverlay;
        ctx->mOverlay = NULL;
    }

    if(ctx->mCopybitEngine) {
        delete ctx->mCopybitEngine;
        ctx->mCopybitEngine = NULL;
    }

    if(ctx->mFbDev) {
        framebuffer_close(ctx->mFbDev);
        ctx->mFbDev = NULL;
    }

    if(ctx->qbuf) {
        delete ctx->qbuf;
        ctx->qbuf = NULL;
    }

    if(ctx->mExtDisplay) {
        delete ctx->mExtDisplay;
        ctx->mExtDisplay = NULL;
    }
}

void dumpLayer(hwc_layer_1_t const* l)
{
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}"
          ", {%d,%d,%d,%d}",
          l->compositionType, l->flags, l->handle, l->transform, l->blending,
          l->sourceCrop.left,
          l->sourceCrop.top,
          l->sourceCrop.right,
          l->sourceCrop.bottom,
          l->displayFrame.left,
          l->displayFrame.top,
          l->displayFrame.right,
          l->displayFrame.bottom);
}

void getLayerStats(hwc_context_t *ctx, const hwc_display_contents_1_t *list)
{
    //Video specific stats
    int yuvCount = 0;
    int yuvLayerIndex = -1;
    bool isYuvLayerSkip = false;
    int skipCount = 0;
    int ccLayerIndex = -1; //closed caption
    int extLayerIndex = -1; //ext-only or block except closed caption
    int extCount = 0; //ext-only except closed caption
    bool isExtBlockPresent = false; //is BLOCK layer present
    bool yuvSecure = false;

    for (size_t i = 0; i < list->numHwLayers; i++) {
        private_handle_t *hnd =
            (private_handle_t *)list->hwLayers[i].handle;

        if (UNLIKELY(isYuvBuffer(hnd))) {
            yuvCount++;
            yuvLayerIndex = i;
            yuvSecure = isSecureBuffer(hnd);
            //Animating
            //Do not mark as SKIP if it is secure buffer
            if (isSkipLayer(&list->hwLayers[i]) && !yuvSecure) {
                isYuvLayerSkip = true;
                skipCount++;
            }
        } else if(UNLIKELY(isExtCC(hnd))) {
            ccLayerIndex = i;
        } else if(UNLIKELY(isExtBlock(hnd))) {
            extCount++;
            extLayerIndex = i;
            isExtBlockPresent = true;
        } else if(UNLIKELY(isExtOnly(hnd))) {
            extCount++;
            //If BLOCK layer present, dont cache index, display BLOCK only.
            if(isExtBlockPresent == false) extLayerIndex = i;
        } else if (isSkipLayer(&list->hwLayers[i])) {
            skipCount++;
        }
    }

    VideoOverlay::setStats(yuvCount, yuvLayerIndex, isYuvLayerSkip,
            ccLayerIndex);
    ExtOnly::setStats(extCount, extLayerIndex, isExtBlockPresent);
    CopyBit::setStats(yuvCount, yuvLayerIndex, isYuvLayerSkip);
    MDPComp::setStats(skipCount);

    ctx->numHwLayers = list->numHwLayers;
    return;
}

//Crops source buffer against destination and FB boundaries
void calculate_crop_rects(hwc_rect_t& crop, hwc_rect_t& dst,
        const int fbWidth, const int fbHeight) {

    int& crop_x = crop.left;
    int& crop_y = crop.top;
    int& crop_r = crop.right;
    int& crop_b = crop.bottom;
    int crop_w = crop.right - crop.left;
    int crop_h = crop.bottom - crop.top;

    int& dst_x = dst.left;
    int& dst_y = dst.top;
    int& dst_r = dst.right;
    int& dst_b = dst.bottom;
    int dst_w = dst.right - dst.left;
    int dst_h = dst.bottom - dst.top;

    if(dst_x < 0) {
        float scale_x =  crop_w * 1.0f / dst_w;
        float diff_factor = (scale_x * abs(dst_x));
        crop_x = crop_x + (int)diff_factor;
        crop_w = crop_r - crop_x;

        dst_x = 0;
        dst_w = dst_r - dst_x;;
    }
    if(dst_r > fbWidth) {
        float scale_x = crop_w * 1.0f / dst_w;
        float diff_factor = scale_x * (dst_r - fbWidth);
        crop_r = crop_r - diff_factor;
        crop_w = crop_r - crop_x;

        dst_r = fbWidth;
        dst_w = dst_r - dst_x;
    }
    if(dst_y < 0) {
        float scale_y = crop_h * 1.0f / dst_h;
        float diff_factor = scale_y * abs(dst_y);
        crop_y = crop_y + diff_factor;
        crop_h = crop_b - crop_y;

        dst_y = 0;
        dst_h = dst_b - dst_y;
    }
    if(dst_b > fbHeight) {
        float scale_y = crop_h * 1.0f / dst_h;
        float diff_factor = scale_y * (dst_b - fbHeight);
        crop_b = crop_b - diff_factor;
        crop_h = crop_b - crop_y;

        dst_b = fbHeight;
        dst_h = dst_b - dst_y;
    }
}

void wait4fbPost(hwc_context_t* ctx) {
    framebuffer_device_t *fbDev = ctx->mFbDev;
    if(fbDev) {
        private_module_t* m = reinterpret_cast<private_module_t*>(
                              fbDev->common.module);
        //wait for the fb_post to be called
        pthread_mutex_lock(&m->fbPostLock);
        while(m->fbPostDone == false) {
            pthread_cond_wait(&(m->fbPostCond), &(m->fbPostLock));
        }
        m->fbPostDone = false;
        pthread_mutex_unlock(&m->fbPostLock);
    }
}

void wait4Pan(hwc_context_t* ctx) {
    framebuffer_device_t *fbDev = ctx->mFbDev;
    if(fbDev) {
        private_module_t* m = reinterpret_cast<private_module_t*>(
                              fbDev->common.module);
        //wait for the fb_post's PAN to finish
        pthread_mutex_lock(&m->fbPanLock);
        while(m->fbPanDone == false) {
            pthread_cond_wait(&(m->fbPanCond), &(m->fbPanLock));
        }
        m->fbPanDone = false;
        pthread_mutex_unlock(&m->fbPanLock);
    }
}

int hwc_sync(hwc_display_contents_1_t* list) {
    int ret = 0;
#ifdef USE_FENCE_SYNC
    struct mdp_buf_sync data;
    int acquireFd[10];
    int count = 0;
    int releaseFd = -1;
    int fbFd = -1;
    data.flags = 0;
    data.acq_fen_fd = acquireFd;
    data.rel_fen_fd = &releaseFd;
    //Accumulate acquireFenceFds
    for(uint32_t i = 0; i < list->numHwLayers; i++) {
        if(list->hwLayers[i].compositionType == HWC_OVERLAY &&
            list->hwLayers[i].acquireFenceFd != -1) {
            acquireFd[count++] = list->hwLayers[i].acquireFenceFd;
        }
    }

    if (count) {
        data.acq_fen_fd_cnt = count;

        //Open fb0 for ioctl
        fbFd = open("/dev/graphics/fb0", O_RDWR);
        if (fbFd < 0) {
            ALOGE("%s: /dev/graphics/fb0 not available", __FUNCTION__);
            return -1;
        }

        //Waits for acquire fences, returns a release fence
        ret = ioctl(fbFd, MSMFB_BUFFER_SYNC, &data);
        if(ret < 0) {
            ALOGE("ioctl MSMFB_BUFFER_SYNC failed, err=%s",
                    strerror(errno));
        }
        close(fbFd);

        for(uint32_t i = 0; i < list->numHwLayers; i++) {
            if(list->hwLayers[i].compositionType == HWC_OVERLAY) {
                //Close the acquireFenceFds
                if(list->hwLayers[i].acquireFenceFd > 0) {
                    close(list->hwLayers[i].acquireFenceFd);
                    list->hwLayers[i].acquireFenceFd = -1;
                }
                //Populate releaseFenceFds.
                if (releaseFd != -1)
                    list->hwLayers[i].releaseFenceFd = dup(releaseFd);
            }
        }
        if (releaseFd != -1)
            close(releaseFd);
    }
#endif
    return ret;
}

};//namespace
