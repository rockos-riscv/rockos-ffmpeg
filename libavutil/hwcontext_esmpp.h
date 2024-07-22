/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVUTIL_HWCONTEXT_ESMPP_H
#define AVUTIL_HWCONTEXT_ESMPP_H

#include <stddef.h>
#include <stdint.h>
#include <drm/drm_fourcc.h>
#include <es_mpp.h>
#include <mpp_buffer.h>

#include "hwcontext_drm.h"


/**
 * DRM Prime Frame descriptor for ESMPP HWDevice.
 */
typedef struct AVESMPPDRMFrameDescriptor {
    /**
     * Backwards compatibility with AVDRMFrameDescriptor.
     */
    AVDRMFrameDescriptor drm_desc;

    /**
     * References to MppBuffer instances which are used
     * on each drm frame index.
     */
    MppBufferPtr buffers[AV_DRM_MAX_PLANES];
} AVESMPPDRMFrameDescriptor;

/**
 * ESMPP-specific data associated with a frame pool.
 *
 * Allocated as AVHWFramesContext.hwctx.
 */
typedef struct AVESMPPFramesContext {
    /**
     * MPP buffer group.
     */
    MppBufferGroupPtr buf_group;

    /**
     * The descriptors of all frames in the pool after creation.
     * Only valid if AVHWFramesContext.initial_pool_size was positive.
     * These are intended to be used as the buffer of ESMPP decoder.
     */
    AVESMPPDRMFrameDescriptor *frames;
    int nb_frames;
    size_t buf_size;
} AVESMPPFramesContext;

/**
 * ESMPP device details.
 *
 * Allocated as AVHWDeviceContext.hwctx
 */
typedef struct AVESMPPDeviceContext {
    /**
     * MPP buffer allocation flags.
     */
    int flags;
} AVESMPPDeviceContext;

#endif /* AVUTIL_HWCONTEXT_ESMPP_H */
