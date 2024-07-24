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

#include "config.h"

#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "avassert.h"
#include "hwcontext.h"
#include "hwcontext_esmpp.h"
#include "hwcontext_internal.h"
#include "imgutils.h"

static const struct {
    enum AVPixelFormat pixfmt;
    uint32_t drm_format;
} supported_formats[] = {
    /* grayscale */
    {AV_PIX_FMT_GRAY8, DRM_FORMAT_R8},
    /* fully-planar YUV */
    {
        AV_PIX_FMT_YUV420P,
        DRM_FORMAT_YUV420,
    },
    {
        AV_PIX_FMT_YUV422P,
        DRM_FORMAT_YUV422,
    },
    {
        AV_PIX_FMT_YUV444P,
        DRM_FORMAT_YUV444,
    },
    /* semi-planar YUV */
    {
        AV_PIX_FMT_NV12,
        DRM_FORMAT_NV12,
    },
    {
        AV_PIX_FMT_NV21,
        DRM_FORMAT_NV21,
    },
    {
        AV_PIX_FMT_NV16,
        DRM_FORMAT_NV16,
    },
    {
        AV_PIX_FMT_NV24,
        DRM_FORMAT_NV24,
    },
    /* semi-planar YUV 10-bit */
    {
        AV_PIX_FMT_P010,
        DRM_FORMAT_P010,
    },
    {
        AV_PIX_FMT_P210,
        DRM_FORMAT_P210,
    },
    /* packed YUV */
    {
        AV_PIX_FMT_YUYV422,
        DRM_FORMAT_YUYV,
    },
    {
        AV_PIX_FMT_YVYU422,
        DRM_FORMAT_YVYU,
    },
    {
        AV_PIX_FMT_UYVY422,
        DRM_FORMAT_UYVY,
    },
    /* packed RGB */
    {
        AV_PIX_FMT_RGB444LE,
        DRM_FORMAT_XRGB4444,
    },
    {
        AV_PIX_FMT_RGB444BE,
        DRM_FORMAT_XRGB4444 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_BGR444LE,
        DRM_FORMAT_XBGR4444,
    },
    {
        AV_PIX_FMT_BGR444BE,
        DRM_FORMAT_XBGR4444 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_RGB555LE,
        DRM_FORMAT_XRGB1555,
    },
    {
        AV_PIX_FMT_RGB555BE,
        DRM_FORMAT_XRGB1555 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_BGR555LE,
        DRM_FORMAT_XBGR1555,
    },
    {
        AV_PIX_FMT_BGR555BE,
        DRM_FORMAT_XBGR1555 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_RGB565LE,
        DRM_FORMAT_RGB565,
    },
    {
        AV_PIX_FMT_RGB565BE,
        DRM_FORMAT_RGB565 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_BGR565LE,
        DRM_FORMAT_BGR565,
    },
    {
        AV_PIX_FMT_BGR565BE,
        DRM_FORMAT_BGR565 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_RGB24,
        DRM_FORMAT_RGB888,
    },
    {
        AV_PIX_FMT_BGR24,
        DRM_FORMAT_BGR888,
    },
    {
        AV_PIX_FMT_RGBA,
        DRM_FORMAT_ABGR8888,
    },
    {
        AV_PIX_FMT_RGB0,
        DRM_FORMAT_XBGR8888,
    },
    {
        AV_PIX_FMT_BGRA,
        DRM_FORMAT_ARGB8888,
    },
    {
        AV_PIX_FMT_BGR0,
        DRM_FORMAT_XRGB8888,
    },
    {
        AV_PIX_FMT_ARGB,
        DRM_FORMAT_BGRA8888,
    },
    {
        AV_PIX_FMT_0RGB,
        DRM_FORMAT_BGRX8888,
    },
    {
        AV_PIX_FMT_ABGR,
        DRM_FORMAT_RGBA8888,
    },
    {
        AV_PIX_FMT_0BGR,
        DRM_FORMAT_RGBX8888,
    },
    {
        AV_PIX_FMT_X2RGB10LE,
        DRM_FORMAT_XRGB2101010,
    },
    {
        AV_PIX_FMT_X2RGB10BE,
        DRM_FORMAT_XRGB2101010 | DRM_FORMAT_BIG_ENDIAN,
    },
    {
        AV_PIX_FMT_X2BGR10LE,
        DRM_FORMAT_XBGR2101010,
    },
    {
        AV_PIX_FMT_X2BGR10BE,
        DRM_FORMAT_XBGR2101010 | DRM_FORMAT_BIG_ENDIAN,
    },
};

static int esmpp_device_create(AVHWDeviceContext *hwdev, const char *device, AVDictionary *opts, int flags) {
    AVESMPPDeviceContext *hwctx = hwdev->hwctx;
    AVDictionaryEntry *opt_d = NULL;

    hwctx->flags = flags;

    opt_d = av_dict_get(opts, "dma32", NULL, 0);
    if (opt_d && !strtol(opt_d->value, NULL, 10)) hwctx->flags &= ~MPP_BUFFER_FLAGS_DMA32;

    opt_d = av_dict_get(opts, "cacheable", NULL, 0);
    if (opt_d && !strtol(opt_d->value, NULL, 10)) hwctx->flags &= ~MPP_BUFFER_FLAGS_CACHABLE;

    if (device) {
        av_log(hwdev, AV_LOG_INFO, "device(%s), flags: 0x%x\n", device, hwctx->flags);
    } else {
        av_log(hwdev, AV_LOG_WARNING, "device null flags: 0x%x\n", hwctx->flags);
    }

    return 0;
}

static int esmpp_frames_get_constraints(AVHWDeviceContext *hwdev,
                                        const void *hwconfig,
                                        AVHWFramesConstraints *constraints) {
    int i;

    constraints->min_width = 16;
    constraints->min_height = 16;

    constraints->valid_hw_formats = av_malloc_array(2, sizeof(enum AVPixelFormat));
    if (!constraints->valid_hw_formats) return AVERROR(ENOMEM);
    constraints->valid_hw_formats[0] = AV_PIX_FMT_DRM_PRIME;
    constraints->valid_hw_formats[1] = AV_PIX_FMT_NONE;

    constraints->valid_sw_formats = av_malloc_array(FF_ARRAY_ELEMS(supported_formats) + 1, sizeof(enum AVPixelFormat));
    if (!constraints->valid_sw_formats) return AVERROR(ENOMEM);
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        constraints->valid_sw_formats[i] = supported_formats[i].pixfmt;
    constraints->valid_sw_formats[i] = AV_PIX_FMT_NONE;
    return 0;
}

static void esmpp_free_drm_frame_descriptor(void *opaque, uint8_t *data) {
    MppBufferPtr mpp_buf = opaque;
    AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)data;
    int ret;

    if (!desc) return;

    if (mpp_buf) {
        ret = mpp_buffer_put(mpp_buf);
        if (ret != MPP_OK) av_log(NULL, AV_LOG_WARNING, "Failed to put MPP buffer: %d\n", ret);
    }

    memset(desc, 0, sizeof(*desc));
    av_free(desc);
}

static int esmpp_get_aligned_linesize(enum AVPixelFormat pix_fmt, int width, int plane) {
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(pix_fmt);
    const int is_rgb = pixdesc->flags & AV_PIX_FMT_FLAG_RGB;
    const int is_yuv = !is_rgb && pixdesc->nb_components >= 2;
    const int is_planar = pixdesc->flags & AV_PIX_FMT_FLAG_PLANAR;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);
    const int is_fully_planar = is_planar && pixdesc->comp[1].plane != pixdesc->comp[2].plane;
    int linesize;

#if 0
    if (pix_fmt == AV_PIX_FMT_NV15 ||
        pix_fmt == AV_PIX_FMT_NV20) {
        const int log2_chroma_w = plane == 1 ? 1 : 0;
        const int width_align_256_odds = FFALIGN(width << log2_chroma_w, 256) | 256;
        return FFALIGN(width_align_256_odds * 10 / 8, 64);
    }
#endif

    linesize = av_image_get_linesize(pix_fmt, width, plane);

    if (is_packed_fmt) {
        const int pixel_width = av_get_padded_bits_per_pixel(pixdesc) / 8;
        linesize = FFALIGN(linesize / pixel_width, 8) * pixel_width;
    } else if (is_yuv && is_fully_planar) {
        linesize = FFALIGN(linesize, 8);
    } else
        linesize = FFALIGN(linesize, 64);

    return linesize;
}

static AVBufferRef *esmpp_drm_pool_alloc(void *opaque, size_t size) {
    int ret;
    AVHWFramesContext *hwfc = opaque;
    AVESMPPFramesContext *avfc = hwfc->hwctx;
    AVESMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    AVESMPPDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    AVBufferRef *ref;
    size_t mpp_buf_size;

    int i;
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(hwfc->sw_format);
    const int bits_pp = av_get_padded_bits_per_pixel(pixdesc);
    int aligned_w;
    int aligned_h;

    MppBufferPtr mpp_buf = NULL;
    if (size == sizeof(AVESMPPDRMFrameDescriptor)) {
        aligned_w = FFALIGN(hwfc->width, 64);
        aligned_h = FFALIGN(hwfc->height, 2);
        mpp_buf_size = aligned_w * aligned_h * bits_pp / 8;
    } else {
        mpp_buf_size = size;
    }

    if (!avfc->buf_size) {
        mpp_buf_size = aligned_w * aligned_h * bits_pp / 8;
    }

    if (hwfc->initial_pool_size > 0 && avfc->nb_frames >= hwfc->initial_pool_size) {
        return NULL;
    }

    desc = av_mallocz(sizeof(*desc));
    if (!desc) return NULL;

    desc->drm_desc.nb_objects = 1;
    desc->drm_desc.nb_layers = 1;

    ret = mpp_buffer_get(avfc->buf_group, &mpp_buf, mpp_buf_size);
    if (ret != MPP_OK || !mpp_buf) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to get MPP buffer: %d\n", ret);
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    desc->buffers[0] = mpp_buf;

    desc->drm_desc.objects[0].fd = mpp_buffer_get_fd(mpp_buf);
    desc->drm_desc.objects[0].size = mpp_buffer_get_size(mpp_buf);

    layer = &desc->drm_desc.layers[0];
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++) {
        if (supported_formats[i].pixfmt == hwfc->sw_format) {
            layer->format = supported_formats[i].drm_format;
            break;
        }
    }
    layer->nb_planes = av_pix_fmt_count_planes(hwfc->sw_format);
    layer->planes[0].object_index = 0;
    layer->planes[0].offset = 0;
    layer->planes[0].pitch = esmpp_get_aligned_linesize(hwfc->sw_format, hwfc->width, 0);

    for (i = 1; i < layer->nb_planes; i++) {
        layer->planes[i].object_index = 0;
        layer->planes[i].offset =
            layer->planes[i - 1].offset
            + layer->planes[i - 1].pitch * (FFALIGN(hwfc->height, 2) >> (i > 1 ? pixdesc->log2_chroma_h : 0));
        layer->planes[i].pitch = esmpp_get_aligned_linesize(hwfc->sw_format, hwfc->width, i);
    }

    ref = av_buffer_create((uint8_t *)desc, sizeof(*desc), esmpp_free_drm_frame_descriptor, mpp_buf, 0);
    if (!ref) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create ESMPP buffer.\n");
        goto fail;
    }

    if (hwfc->initial_pool_size > 0) {
        av_assert0(avfc->nb_frames < hwfc->initial_pool_size);
        memcpy(&avfc->frames[avfc->nb_frames], desc, sizeof(*desc));
        ++avfc->nb_frames;
    }

    return ref;

fail:
    esmpp_free_drm_frame_descriptor(mpp_buf, (uint8_t *)desc);
    return NULL;
}

static int esmpp_frames_init(AVHWFramesContext *hwfc) {
    AVESMPPFramesContext *avfc = hwfc->hwctx;
    AVESMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    int i, ret;
    size_t size;

    if (hwfc->pool) return 0;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i].pixfmt == hwfc->sw_format) break;
    if (i >= FF_ARRAY_ELEMS(supported_formats)) {
        av_log(hwfc, AV_LOG_ERROR, "Unsupported format: %s.\n", av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(EINVAL);
    }

    avfc->nb_frames = 0;
    avfc->frames = NULL;
    if (hwfc->initial_pool_size > 0) {
        avfc->frames = av_malloc(hwfc->initial_pool_size * sizeof(*avfc->frames));
        if (!avfc->frames) {
            av_log(hwfc, AV_LOG_ERROR, "alloc frames failed initial_pool_size: %d\n", hwfc->initial_pool_size);
            return AVERROR(ENOMEM);
        }
    }

    ret = mpp_buffer_group_get_internal(&avfc->buf_group, MPP_BUFFER_TYPE_DMA_HEAP | hwctx->flags);
    if (ret != MPP_OK) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to get MPP internal buffer group: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    size = !avfc->buf_size ? sizeof(AVESMPPDRMFrameDescriptor) : avfc->buf_size;
    ffhwframesctx(hwfc)->pool_internal = av_buffer_pool_init2(size, hwfc, esmpp_drm_pool_alloc, NULL);
    if (!ffhwframesctx(hwfc)->pool_internal) {
        av_log(hwfc, AV_LOG_ERROR, "Failed to create ESMPP buffer pool.\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void esmpp_frames_uninit(AVHWFramesContext *hwfc) {
    AVESMPPFramesContext *avfc = hwfc->hwctx;

    av_freep(&avfc->frames);

    if (avfc->buf_group) {
        mpp_buffer_group_put(avfc->buf_group);
        avfc->buf_group = NULL;
    }
}

static int esmpp_get_buffer(AVHWFramesContext *hwfc, AVFrame *frame) {
    frame->buf[0] = av_buffer_pool_get(hwfc->pool);
    if (!frame->buf[0]) return AVERROR(ENOMEM);

    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = hwfc->width;
    frame->height = hwfc->height;
    frame->data[0] = (uint8_t *)frame->buf[0]->data;

    return 0;
}

typedef struct ESMPPDRMMapping {
    // Address and length of each mmap()ed region.
    int nb_regions;
    int sync_flags;
    int object[AV_DRM_MAX_PLANES];
    void *address[AV_DRM_MAX_PLANES];
    size_t length[AV_DRM_MAX_PLANES];
    int unmap[AV_DRM_MAX_PLANES];
} ESMPPDRMMapping;

static void esmpp_unmap_frame(AVHWFramesContext *hwfc, HWMapDescriptor *hwmap) {
    AVESMPPDeviceContext *hwctx = hwfc->device_ctx->hwctx;
    ESMPPDRMMapping *map = hwmap->priv;

    for (int i = 0; i < map->nb_regions; i++) {
        if (map->address[i] && map->unmap[i]) munmap(map->address[i], map->length[i]);
    }

    av_free(map);
}

static int esmpp_map_frame(AVHWFramesContext *hwfc, AVFrame *dst, const AVFrame *src, int flags) {
    const AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)src->data[0];
    ESMPPDRMMapping *map;
    int err, i, p, plane;
    int mmap_prot;
    void *addr;

    map = av_mallocz(sizeof(*map));
    if (!map) return AVERROR(ENOMEM);

    mmap_prot = 0;
    if (flags & AV_HWFRAME_MAP_READ) mmap_prot |= PROT_READ;
    if (flags & AV_HWFRAME_MAP_WRITE) mmap_prot |= PROT_WRITE;

    if (desc->drm_desc.objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(hwfc, AV_LOG_ERROR, "Transfer non-linear DRM_PRIME frame is not supported!\n");
        return AVERROR(ENOSYS);
    }

    av_assert0(desc->drm_desc.nb_objects <= AV_DRM_MAX_PLANES);
    for (i = 0; i < desc->drm_desc.nb_objects; i++) {
        addr = NULL;
        if (desc->buffers[i]) addr = mpp_buffer_get_ptr(desc->buffers[i]);
        if (addr) {
            map->unmap[i] = 0;
        } else {
            addr = mmap(NULL, desc->drm_desc.objects[i].size, mmap_prot, MAP_SHARED, desc->drm_desc.objects[i].fd, 0);
            if (addr == MAP_FAILED) {
                err = AVERROR(errno);
                av_log(hwfc,
                       AV_LOG_ERROR,
                       "Failed to map ESMPP object %d to "
                       "memory: %d.\n",
                       desc->drm_desc.objects[i].fd,
                       errno);
                goto fail;
            }
            map->unmap[i] = 1;
        }

        map->address[i] = addr;
        map->length[i] = desc->drm_desc.objects[i].size;
        map->object[i] = desc->drm_desc.objects[i].fd;
    }
    map->nb_regions = i;

    plane = 0;
    for (i = 0; i < desc->drm_desc.nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->drm_desc.layers[i];
        for (p = 0; p < layer->nb_planes; p++) {
            dst->data[plane] = (uint8_t *)map->address[layer->planes[p].object_index] + layer->planes[p].offset;
            dst->linesize[plane] = layer->planes[p].pitch;
            ++plane;
        }
    }
    av_assert0(plane <= AV_DRM_MAX_PLANES);

    dst->width = src->width;
    dst->height = src->height;

    err = ff_hwframe_map_create(src->hw_frames_ctx, dst, src, &esmpp_unmap_frame, map);
    if (err < 0) goto fail;

    return 0;

fail:
    for (i = 0; i < desc->drm_desc.nb_objects; i++) {
        if (map->address[i] && map->unmap[i]) munmap(map->address[i], map->length[i]);
    }
    av_free(map);
    return err;
}

static int esmpp_transfer_get_formats(AVHWFramesContext *ctx,
                                      enum AVHWFrameTransferDirection dir,
                                      enum AVPixelFormat **formats) {
    enum AVPixelFormat *pix_fmts;

    pix_fmts = av_malloc_array(2, sizeof(*pix_fmts));
    if (!pix_fmts) return AVERROR(ENOMEM);

    pix_fmts[0] = ctx->sw_format;
    pix_fmts[1] = AV_PIX_FMT_NONE;

    *formats = pix_fmts;

    return 0;
}

static int esmpp_transfer_data_from(AVHWFramesContext *hwfc, AVFrame *dst, const AVFrame *src) {
    AVFrame *map;
    int err;

    if (dst->width > hwfc->width || dst->height > hwfc->height) return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map) return AVERROR(ENOMEM);
    map->format = dst->format;

    err = esmpp_map_frame(hwfc, map, src, AV_HWFRAME_MAP_READ);
    if (err) goto fail;

    map->width = dst->width;
    map->height = dst->height;

    err = av_frame_copy(dst, map);
    if (err) goto fail;

    err = 0;
fail:
    av_frame_free(&map);

    return err;
}

static int esmpp_transfer_data_to(AVHWFramesContext *hwfc, AVFrame *dst, const AVFrame *src) {
    AVFrame *map;
    int err;

    if (src->width > hwfc->width || src->height > hwfc->height) return AVERROR(EINVAL);

    map = av_frame_alloc();
    if (!map) return AVERROR(ENOMEM);
    map->format = src->format;

    err = esmpp_map_frame(hwfc, map, dst, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
    if (err) goto fail;

    map->width = src->width;
    map->height = src->height;

    err = av_frame_copy(map, src);
    if (err) goto fail;

    err = 0;
fail:
    av_frame_free(&map);

    return err;
}

static int esmpp_map_from(AVHWFramesContext *hwfc, AVFrame *dst, const AVFrame *src, int flags) {
    int err;

    if (hwfc->sw_format != dst->format) return AVERROR(ENOSYS);

    err = esmpp_map_frame(hwfc, dst, src, flags);
    if (err) return err;

    err = av_frame_copy_props(dst, src);
    if (err) return err;

    return 0;
}

const HWContextType ff_hwcontext_type_esmpp = {
    .type = AV_HWDEVICE_TYPE_ESMPP,
    .name = "ESMPP",

    .device_hwctx_size = sizeof(AVESMPPDeviceContext),
    .frames_hwctx_size = sizeof(AVESMPPFramesContext),

    .device_create = &esmpp_device_create,

    .frames_get_constraints = &esmpp_frames_get_constraints,

    .frames_get_buffer = &esmpp_get_buffer,
    .frames_init = &esmpp_frames_init,
    .frames_uninit = &esmpp_frames_uninit,
    .transfer_get_formats = &esmpp_transfer_get_formats,
    .transfer_data_to = &esmpp_transfer_data_to,
    .transfer_data_from = &esmpp_transfer_data_from,
    .map_from = &esmpp_map_from,

    .pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NONE},
};
