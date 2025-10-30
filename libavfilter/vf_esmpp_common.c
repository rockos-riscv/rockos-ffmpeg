#include "vf_esmpp_common.h"
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE_PATH (200)

ES_BOOL write_buffer_to_file(const void *buffer,
                             ES_S32 size,
                             const char *path,
                             ES_U32 width,
                             ES_U32 height,
                             enum AVPixelFormat format,
                             ES_S32 index) {
    char file_path[MAX_FILE_PATH] = {0};
    FILE *file = NULL;
    size_t returnSize = 0;
    size_t offset = 0;
    size_t totalSize = 0;
    size_t writeSize = size;

    ES_RETURN_VAL_IF_FAIL(buffer, ES_FALSE);
    ES_RETURN_VAL_IF_FAIL(size > 0, ES_FALSE);

    snprintf(file_path,
             MAX_FILE_PATH,
             "%s/out_%d_%ux%u_%s.raw",
             path ? path : ".",
             index,
             width,
             height,
             av_get_pix_fmt_name(format));
    file = fopen(file_path, "wb+");
    if (!file) {
        av_log(NULL, AV_LOG_ERROR, "Can't open %s.\n", file_path);
        return ES_FALSE;
    }

    do {
        returnSize = fwrite(buffer + offset, 1, writeSize, file);
        if (returnSize == 0) {
            av_log(NULL, AV_LOG_ERROR, "fwrite return error.\n");
            fclose(file);
            return ES_FALSE;
        } else if (returnSize < writeSize) {
            offset += returnSize;
            writeSize -= returnSize;
            totalSize += returnSize;
        } else {
            totalSize += returnSize;
            av_log(NULL, AV_LOG_INFO, "Success write %zu bytes to %s\n", totalSize, file_path);
            break;
        }
    } while (1);
    fclose(file);
    return ES_TRUE;
}

void esmpp_free_frame_buf(void *opaque, uint8_t *data) {
    MppBufferPtr dst_mpp_buf = opaque;
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
}

void esmpp_free_drm_desc(void *opaque, uint8_t *data) {
    AVESMPPDRMFrameDescriptor *drm_desc = (AVESMPPDRMFrameDescriptor *)opaque;
    av_free(drm_desc);
}

int esmpp_get_frame_data_size(const enum AVPixelFormat fmt, const AVFrame *in) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int total_size = 0;

    ES_RETURN_VAL_IF_FAIL(in, FAILURE);

    desc = av_pix_fmt_desc_get(fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_get_frame_data_size get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(fmt));
        return FAILURE;
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(in->data) && in->data[i]; i++) {
        height = in->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        total_size += in->linesize[i] * height;
    }

    return total_size;
}

int esmpp_memcpy_host2device(const enum AVPixelFormat fmt, const AVFrame *in, void *out_vir) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int cp_size = 0;
    int totol_size = 0;

    ES_RETURN_VAL_IF_FAIL(in, FAILURE);
    ES_RETURN_VAL_IF_FAIL(out_vir, FAILURE);

    desc = av_pix_fmt_desc_get(fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_memcpy_host2device get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(fmt));
        return FAILURE;
    }
    for (int i = 0; i < FF_ARRAY_ELEMS(in->data) && in->data[i]; i++) {
        height = in->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        cp_size = in->linesize[i] * height;
        mpp_buffer_memcpy(out_vir + totol_size, in->data[i], cp_size);
        totol_size += cp_size;
    }
    return SUCCESS;
}

int esmpp_memcpy_device2host(const enum AVPixelFormat fmt, const AVFrame *out, void *in_vir) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int cp_size = 0;
    int totol_size = 0;

    ES_RETURN_VAL_IF_FAIL(out, FAILURE);
    ES_RETURN_VAL_IF_FAIL(in_vir, FAILURE);

    desc = av_pix_fmt_desc_get(fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_memcpy_host2device get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(fmt));
        return FAILURE;
    }
    for (int i = 0; i < FF_ARRAY_ELEMS(out->data) && out->data[i]; i++) {
        height = out->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        cp_size = out->linesize[i] * height;
        mpp_buffer_memcpy(out->data[i], in_vir + totol_size, cp_size);
        totol_size += cp_size;
    }
    return SUCCESS;
}

int frame_create_buf(
    AVFrame *frame, uint8_t *data, int size, void (*free)(void *opaque, uint8_t *data), void *opaque, int flags) {
    int i;

    ES_RETURN_VAL_IF_FAIL(frame, FAILURE);
    ES_RETURN_VAL_IF_FAIL(data, FAILURE);
    ES_RETURN_VAL_IF_FAIL(free, FAILURE);
    ES_RETURN_VAL_IF_FAIL(opaque, FAILURE);

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (!frame->buf[i]) {
            frame->buf[i] = av_buffer_create(data, size, free, opaque, flags);
            return frame->buf[i] ? 0 : AVERROR(ENOMEM);
        }
    }
    return AVERROR(EINVAL);
}

int esmpp_buffer_export_frame(AVFrame *frame, MppFramePtr mpp_frame, int nb_planes, int *offset, int *stride) {
    MppBufferPtr mpp_buf = NULL;
    // MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    AVESMPPDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    int ret;

    ES_RETURN_VAL_IF_FAIL(mpp_frame, FAILURE);
    ES_RETURN_VAL_IF_FAIL(offset, FAILURE);
    ES_RETURN_VAL_IF_FAIL(stride, FAILURE);

    mpp_buf = mpp_frame_get_buffer(mpp_frame);
    if (!mpp_buf) {
        av_log(NULL, AV_LOG_WARNING, "esmpp_buffer_export_frame mpp_buf is null\n");
        return AVERROR(EAGAIN);
    }

    desc = av_mallocz(sizeof(*desc));
    if (!desc) {
        return AVERROR(ENOMEM);
    }
    desc->drm_desc.nb_objects = 1;
    desc->buffers[0] = mpp_buf;
    desc->drm_desc.objects[0].fd = mpp_buffer_get_fd(mpp_buf);
    desc->drm_desc.objects[0].size = mpp_buffer_get_size(mpp_buf);
    desc->drm_desc.nb_layers = 1;
    layer = &desc->drm_desc.layers[0];
    layer->planes[0].object_index = 0;
    // mpp_fmt = mpp_frame_get_fmt(mpp_frame);
    // layer->format = DRM_FORMAT_NV12;
    // mpp_fmt_to_drm_format(mpp_fmt);
    layer->nb_planes = nb_planes;
    for (int i = 0; i < nb_planes; i++) {
        layer->planes[i].object_index = 0;
        layer->planes[i].offset = offset[i];
        layer->planes[i].pitch = stride[i];
        frame->linesize[i] = stride[i];
    }

    ret = frame_create_buf(frame, mpp_buf, sizeof(mpp_buf), esmpp_free_frame_buf, mpp_buf, AV_BUFFER_FLAG_READONLY);
    if (ret < 0) {
        return ret;
    }
    ret = frame_create_buf(frame, (uint8_t *)desc, sizeof(*desc), esmpp_free_drm_desc, desc, AV_BUFFER_FLAG_READONLY);
    if (ret < 0) {
        return ret;
    }

    frame->data[0] = (uint8_t *)desc;
    return SUCCESS;
}

MppFrameFormat ff_fmt_to_mpp_fmt(enum AVPixelFormat ff_fmt) {
    switch (ff_fmt) {
        case AV_PIX_FMT_NV12:
            return MPP_FMT_NV12;
        case AV_PIX_FMT_NV21:
            return MPP_FMT_NV21;
        case AV_PIX_FMT_YUV420P:
            return MPP_FMT_I420;
        case AV_PIX_FMT_GRAY8:
            return MPP_FMT_GRAY8;
        case AV_PIX_FMT_YUV420P10LE:
            return MPP_FMT_I010;
        case AV_PIX_FMT_P010LE:
            return MPP_FMT_P010;
        case AV_PIX_FMT_YVYU422:
            return MPP_FMT_YVY2;
        case AV_PIX_FMT_YUYV422:
            return MPP_FMT_YUY2;
        case AV_PIX_FMT_UYVY422:
            return MPP_FMT_UYVY;
        case AV_PIX_FMT_NV16:
            return MPP_FMT_NV16;
        case AV_PIX_FMT_RGB24:
            return MPP_FMT_R8G8B8;
        case AV_PIX_FMT_BGR24:
            return MPP_FMT_B8G8R8;
        case AV_PIX_FMT_ARGB:
            return MPP_FMT_A8R8G8B8;
        case AV_PIX_FMT_ABGR:
            return MPP_FMT_A8B8G8R8;
        case AV_PIX_FMT_BGRA:
            return MPP_FMT_B8G8R8A8;
        case AV_PIX_FMT_RGBA:
            return MPP_FMT_R8G8B8A8;
        default:
            return MPP_FMT_BUTT;
    }
}

static int get_plane_bpp(enum AVPixelFormat fmt, int bpp[3]) {
    bpp[0] = bpp[1] = bpp[2] = 0;
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:
            bpp[0] = 8;
            bpp[1] = bpp[2] = 2;
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            bpp[0] = 8;
            bpp[1] = 4;
            break;
        case AV_PIX_FMT_YVYU422:
        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
            bpp[0] = 16;
            break;
        case AV_PIX_FMT_NV16:
            bpp[0] = bpp[1] = 8;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            bpp[0] = 16;
            bpp[1] = bpp[2] = 4;
            break;
        case AV_PIX_FMT_P010LE:
            bpp[0] = 16;
            bpp[1] = 8;
            break;
        case AV_PIX_FMT_GRAY8:
            bpp[0] = 8;
            break;
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
            bpp[0] = 24;
            break;
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_RGBA:
            bpp[0] = 32;
            break;

        default:
            return -1;
    }
    return 0;
}

static int get_bpp(enum AVPixelFormat fmt) {
    int bpp[3] = {0, 0, 0};

    get_plane_bpp(fmt, bpp);

    return bpp[0] + bpp[1] + bpp[2];
}

size_t get_pic_buf_info(
    enum AVPixelFormat fmt, int width, int height, int align, int align_h, int *p_stride, int *p_offset, int *p_plane) {
    int bpp, plane, stride;
    int uStride, vStride, uOffset, vOffset, alignWidth, strideAlign;

    bpp = get_bpp(fmt);
    if (!bpp) return 0;
    alignWidth = (align > 0) ? FFALIGN(width, align) : width;
    align_h = (align_h > 0) ? FFALIGN(height, align_h) : height;
    strideAlign = (align < 2) ? 2 : FFALIGN(align, 2);
    stride = FFALIGN(alignWidth, strideAlign);

    switch (fmt) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            /*  WxH Y plane followed by (W)x(H/2) interleaved U/V plane. */
            stride = alignWidth;
            stride = FFALIGN(stride, strideAlign);
            uStride = vStride = stride;
            uOffset = vOffset = stride * align_h;
            plane = 2;
            break;
        case AV_PIX_FMT_NV16:
            stride = alignWidth;
            /*  WxH Y plane followed by WxH interleaved U/V(V/U) plane. */
            stride = FFALIGN(stride, strideAlign);
            uStride = vStride = stride;
            uOffset = vOffset = stride * align_h;
            plane = 2;
            break;
        case AV_PIX_FMT_P010LE:
            /*  WxH Y plane followed by (W)x(H/2) interleaved U/V plane. */
            stride = alignWidth * 2;
            stride = FFALIGN(stride, strideAlign);
            uStride = vStride = stride;
            uOffset = vOffset = stride * align_h;
            plane = 2;
            break;
        case AV_PIX_FMT_YUV420P:
            /*  WxH Y plane followed by (W/2)x(H/2) U and V planes. */
            uStride = vStride = (stride / 2);
            stride = FFALIGN(stride, strideAlign);
            uStride = FFALIGN(uStride, strideAlign / 2);
            vStride = FFALIGN(vStride, strideAlign / 2);
            uOffset = stride * align_h;
            vOffset = uOffset + vStride * align_h / 2;
            plane = 3;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            /*  WxH Y plane followed by (W/2)x(H/2) U and V planes. */
            stride = alignWidth * 2;
            uStride = vStride = (stride / 2);
            stride = FFALIGN(stride, strideAlign);
            uStride = FFALIGN(uStride, strideAlign / 2);
            vStride = FFALIGN(vStride, strideAlign / 2);
            uOffset = stride * align_h;
            vOffset = uOffset + uStride * align_h / 2;
            plane = 3;
            break;
        default:
            stride = (alignWidth * bpp) / 8;
            uStride = vStride = 0;
            uOffset = vOffset = 0;
            plane = 1;
            break;
    }

    if (p_stride) {
        p_stride[0] = stride;
        if (plane > 1) p_stride[1] = uStride;
        if (plane > 2) p_stride[2] = vStride;
    }
    if (p_offset) {
        p_offset[0] = 0;
        if (plane > 1) p_offset[1] = uOffset;
        if (plane > 2) p_offset[2] = vOffset;
    }
    if (p_plane) {
        *p_plane = plane;
    }

    return (size_t)alignWidth * align_h * bpp / 8;
}

int get_alignment_by_format(enum AVPixelFormat fmt) {
    int align = 0;
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_NV16:
            align = 64;
            break;
        case AV_PIX_FMT_YVYU422:
        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
            align = 4;
            break;
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_YUV420P10LE:
            align = 128;
            break;

        default:
            align = 1;  // such as rgb etc.
            break;
    }
    return align;
}

void adjust_width_height_by_format(enum AVPixelFormat fmt, int *width, int *height) {
    int align = get_alignment_by_format(fmt);

    ES_RETURN_IF_FAIL(width);
    ES_RETURN_IF_FAIL(height);

    *width = FFALIGN(*width, align);
    *height = FFALIGN(*height, 2);
}

int esmpp_set_mpp_frame(MppFramePtr mpp_frame,
                        MppFrameFormat fmt,
                        int width,
                        int height,
                        int *stride,
                        int *offset,
                        ROTATION_E rotation,
                        ES_S32 global_alpha) {
    mpp_frame_set_stride(mpp_frame, stride);
    mpp_frame_set_offset(mpp_frame, offset);
    mpp_frame_set_width(mpp_frame, width);
    mpp_frame_set_height(mpp_frame, height);
    mpp_frame_set_fmt(mpp_frame, fmt);
    mpp_frame_set_rotation(mpp_frame, rotation);
    mpp_frame_set_global_alpha(mpp_frame, global_alpha);

    return 0;
}

int esmpp_stack_get_offset_stride(
    const AVFrame *in, enum AVPixelFormat fmt, int *stride, int *offset, uint32_t *pic_size) {
    int hshift, vshift, planes;
    int offset_sum = 0;
    uint32_t i = 0;

    ES_RETURN_VAL_IF_FAIL(in, FAILURE);
    ES_RETURN_VAL_IF_FAIL(stride, FAILURE);
    ES_RETURN_VAL_IF_FAIL(offset, FAILURE);

    av_pix_fmt_get_chroma_sub_sample(fmt, &hshift, &vshift);
    planes = av_pix_fmt_count_planes(fmt);
    for (i = 0; i < planes; i++) {
        *pic_size += in->linesize[i] * (in->height >> (i ? vshift : 0));
        stride[i] = in->linesize[i];
        offset[i] = offset_sum;
        offset_sum = *pic_size;
    }
    av_log(NULL,
           AV_LOG_DEBUG,
           "get stride[0]=%d, stride[1]=%d, stride[2]=%d, stride[3]=%d, offset[0]=%d, offset[1]=%d, offset[2]=%d, "
           "offset[3]=%d.\n",
           stride[0],
           stride[1],
           stride[2],
           stride[3],
           offset[0],
           offset[1],
           offset[2],
           offset[3]);
    return SUCCESS;
}

int init_hwcontext_buf(AVFilterLink *filterlink,
                       AVBufferRef **hw_device_ref,
                       AVBufferRef **hw_frm_ref,
                       int pixelfmt,
                       int device_idx,
                       int die_idx) {
    AVFilterContext *ctx = NULL;
    AVHWFramesContext *hwfc = NULL;
    AVHWDeviceContext *hwdev;
    AVESMPPDeviceContext *hwctx;
    AVBufferRef *hw_device_ref_p, *hw_frm_ref_p;
    int flags = MPP_BUFFER_TYPE_DMA_HEAP;
    int ret = 0;

    ES_RETURN_VAL_IF_FAIL(filterlink, FAILURE);
    ES_RETURN_VAL_IF_FAIL(filterlink->dst, FAILURE);
    ES_RETURN_VAL_IF_FAIL(hw_device_ref, FAILURE);
    ES_RETURN_VAL_IF_FAIL(hw_frm_ref, FAILURE);

    ctx = filterlink->dst;

    if (*hw_frm_ref) {
        return 0;
    }

    if ((ret = av_hwdevice_ctx_create(&hw_device_ref_p, AV_HWDEVICE_TYPE_ESMPP, "esmpp", NULL, flags)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create a ESMPP hardware device: %d\n", ret);
        goto err;
    }

    hw_frm_ref_p = av_hwframe_ctx_alloc(hw_device_ref_p);
    if (!hw_frm_ref_p) {
        av_log(ctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed \n");
        goto err;
    }

    hwfc = (AVHWFramesContext *)hw_frm_ref_p->data;
    hwfc->format = AV_PIX_FMT_DRM_PRIME;
    hwfc->sw_format = pixelfmt;

    hwfc->width = filterlink->w;
    hwfc->height = filterlink->h;

    hwdev = hwfc->device_ctx;
    hwctx = hwdev->hwctx;
    hwctx->die_idx = die_idx;
    hwctx->device_idx = device_idx;
    av_log(ctx, AV_LOG_INFO, "%s set device: %d, die_idx: %d.\n", __func__, device_idx, die_idx);
    if ((ret = av_hwframe_ctx_init(hw_frm_ref_p)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init ESMPP frame pool\n");
        goto err;
    }

    *hw_device_ref = hw_device_ref_p;
    *hw_frm_ref = hw_frm_ref_p;

    return ret;
err:
    if (hw_frm_ref_p) {
        av_buffer_unref(&hw_frm_ref_p);
    }
    if (hw_device_ref_p) {
        av_buffer_unref(&hw_device_ref_p);
    }
    return ret;
}

uint64_t get_ticks_by_ms(void) {
    struct timeval tv;
    /* Return the time of day in milliseconds. */
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

void get_dev_info(AVBufferRef *hw_device_ctx, AVBufferRef *hw_frames_ctx, int *dev_id, int *die_id) {
    if (dev_id == NULL && die_id == NULL) {
        av_log(NULL, AV_LOG_ERROR, "get_dev_info invalid params");
        return;
    }

    if (hw_frames_ctx || hw_device_ctx) {
        AVHWDeviceContext *hwdev = NULL;
        AVESMPPDeviceContext *hwctx = NULL;

        if (hw_frames_ctx) {
            AVHWFramesContext *hw_frm_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
            hwdev = hw_frm_ctx->device_ctx;
        } else {
            hwdev = (AVHWDeviceContext *)hw_device_ctx->data;
        }
        hwctx = hwdev->hwctx;

        if (dev_id) {
            *dev_id = hwctx->device_idx;
        }
        if (die_id) {
            *die_id = hwctx->die_idx;
        }
        av_log(
            NULL, AV_LOG_DEBUG, "get_dev_info device_idx:%d, die_idx:%d\n", dev_id ? *dev_id : 0, die_id ? *die_id : 0);
    }

    return;
}

static ES_BOOL chck_if_yuv(enum AVPixelFormat fmt) {
    int planes = 0;

    planes = av_pix_fmt_count_planes(fmt);
    if (planes == 1 && (fmt != AV_PIX_FMT_YUYV422 && fmt != AV_PIX_FMT_UYVY422 && fmt != AV_PIX_FMT_YVYU422))
        return ES_FALSE;

    return ES_TRUE;
}

int check_rect_if_valid(RECT_S *rect, int frm_width, int frm_height, enum AVPixelFormat fmt) {
    ES_RETURN_VAL_IF_FAIL(rect, FAILURE);
    ES_RETURN_VAL_IF_FAIL(rect->x >= 0, FAILURE);
    ES_RETURN_VAL_IF_FAIL(rect->y >= 0, FAILURE);
    ES_RETURN_VAL_IF_FAIL(rect->width > 0, FAILURE);
    ES_RETURN_VAL_IF_FAIL(rect->height > 0, FAILURE);

    if ((rect->x + rect->width > frm_width) || ((rect->y + rect->height) > frm_height)) {
        av_log(NULL,
               AV_LOG_ERROR,
               "invalid rect: x: %d, y: %d, width: %d, height: %d, frma width: %d, frmae height: %d.\n",
               rect->x,
               rect->y,
               rect->width,
               rect->height,
               frm_width,
               frm_height);
        return FAILURE;
    }
    if (chck_if_yuv(fmt) && (rect->x % 2 || rect->y % 2 || rect->width % 2 || rect->height % 2)) {
        av_log(NULL,
               AV_LOG_ERROR,
               "invalid rect: x: %d, y: %d, width: %d, height: %d, fmt is yuv, must be aligned with 2.\n",
               rect->x,
               rect->y,
               rect->width,
               rect->height);
        return FAILURE;
    }

    return SUCCESS;
}

ES_BOOL esmpp_check_snapshot(int interval, int output_count, int snapshot_output) {
    if (interval && (0 == output_count % interval)) {
        if (snapshot_output) {
            return ES_TRUE;
        } else {
            av_log(NULL,
                   AV_LOG_ERROR,
                   "need snapshot but no outpad, interval=%d, output_count=%d, snapshot_output=%d\n",
                   interval,
                   output_count,
                   snapshot_output);
            return ES_FALSE;
        }
    }

    return ES_FALSE;
}

int esmpp_snapshot_send_frame(AVFilterLink *outlink, AVFrame *outframe) {
    AVFrame *frame_snapshot = NULL;
    int ret = FAILURE;

    ES_RETURN_VAL_IF_FAIL(outlink, FAILURE);
    ES_RETURN_VAL_IF_FAIL(outframe, FAILURE);

    frame_snapshot = av_frame_alloc();
    if (NULL == frame_snapshot) {
        return FAILURE;
    }

    if (av_frame_ref(frame_snapshot, outframe) >= 0) {
        ret = ff_filter_frame(outlink, frame_snapshot);
        if (ret >= 0) {
            ret = SUCCESS;
        } else {
            av_log(NULL, AV_LOG_ERROR, "snapshot ff_filter_frame failed\n");
        }
    } else {
        av_log(NULL, AV_LOG_ERROR, "snapshot av_frame_ref failed\n");
    }

    if (ret != SUCCESS) {
        av_frame_free(&frame_snapshot);
    }

    return ret;
}

static int parse_normalization_rgb(char *str, TdeRgb *rgb_info) {
    if (!str || !rgb_info) {
        return FAILURE;
    }

    if (sscanf(str,
              "%x/%x/%x",
               &rgb_info->r,
               &rgb_info->g,
               &rgb_info->b) != 3) {
        return FAILURE;
    }

    return SUCCESS;
}

int parse_normalization(MppFilterContext *s) {
    int ret = SUCCESS;

    ES_RETURN_VAL_IF_FAIL(s, FAILURE);

    s->nor_set = 0;

    if (s->nor_mode != 0 && s->nor_mode != 1) {
        av_log(NULL, AV_LOG_INFO, "do not normalization, nor_mode: %d\n", s->nor_mode);
        return SUCCESS;
    }

    if (s->nor_mode == 0 && (!s->nor_min || !s->nor_mm_r)) {
        av_log(NULL, AV_LOG_ERROR, "nor_mode is min_max, nor_min: %p, nor_mm_r: %p\n", s->nor_min, s->nor_mm_r);
        return FAILURE;
    }

    if (s->nor_mode == 1 && (!s->nor_mean || !s->nor_std_r)) {
        av_log(NULL, AV_LOG_ERROR, "nor_mode is z_score, nor_mean: %p, nor_std_r: %p\n", s->nor_mean, s->nor_std_r);
        return FAILURE;
    }

    s->nor_params.nn_mode = s->nor_mode;
    s->nor_params.step_reciprocal = s->nor_step_r;

    if (s->nor_mm_r) {
        ret = parse_normalization_rgb(s->nor_mm_r, &s->nor_params.min_max_reciprocal);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para min_max_reciprocal failed\n");
            return FAILURE;
        }
    }
    if (s->nor_min) {
        ret = parse_normalization_rgb(s->nor_min, &s->nor_params.min_value);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para min_value failed\n");
            return FAILURE;
        }
    }
    if (s->nor_mean) {
        ret = parse_normalization_rgb(s->nor_mean, &s->nor_params.mean_value);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para mean_value failed\n");
            return FAILURE;
        }
    }
    if (s->nor_std_r) {
        ret = parse_normalization_rgb(s->nor_std_r, &s->nor_params.std_reciprocal);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para std_reciprocal failed\n");
            return FAILURE;
        }
    }

    s->nor_set = 1;

    av_log(NULL, AV_LOG_DEBUG, "normalizationMode: %d,stepReciprocal: %x\n", s->nor_mode, s->nor_step_r);
    av_log(NULL,
           AV_LOG_DEBUG,
           "min_value.r: %x, min_value.g: %x, min_value.b: %x\n",
           s->nor_params.min_value.r,
           s->nor_params.min_value.g,
           s->nor_params.min_value.b);
    av_log(NULL,
           AV_LOG_DEBUG,
           "maxMinReciprocal.r: %x, maxMinReciprocal.g: %x, maxMinReciprocal.b: %x\n",
           s->nor_params.min_max_reciprocal.r,
           s->nor_params.min_max_reciprocal.g,
           s->nor_params.min_max_reciprocal.b);
    av_log(NULL,
           AV_LOG_DEBUG,
           "meanValue.r: %x, meanValue.g: %x, meanValue.b: %x\n",
           s->nor_params.mean_value.r,
           s->nor_params.mean_value.g,
           s->nor_params.mean_value.b);
    av_log(NULL,
           AV_LOG_DEBUG,
           "stdReciprocal.r: %x, stdReciprocal.g: %x, stdReciprocal.b: %x\n",
           s->nor_params.std_reciprocal.r,
           s->nor_params.std_reciprocal.g,
           s->nor_params.std_reciprocal.b);

    return SUCCESS;
}

FILE *esmpp_filter_dump_file_open(const char *dump_path, const char *prefix_name) {
    FILE *dump_fp = NULL;
    char file_path[PATH_MAX];
    time_t now;
    char time_char[128];
    struct tm *tm;
    int ret;

    if (!dump_path) {
        av_log(NULL, AV_LOG_ERROR, "error !!! dump path is null\n");
        return NULL;
    }

    ret = access(dump_path, 0);
    if (ret == -1) {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s does not exist\n", dump_path);
        if (mkdir(dump_path, 0731) == -1) {
            av_log(NULL, AV_LOG_ERROR, "create dump_path: %s failed errno: %d\n", dump_path, errno);
            return NULL;
        }
    } else {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s exist\n", dump_path);
    }

    // get local time
    time(&now);
    tm = localtime(&now);
    strftime(time_char, sizeof(time_char), "%y%m%d%H%M%S", tm);

    snprintf(file_path,
             sizeof(file_path),
             "%s/%s_%s",
             dump_path,
             prefix_name,
             time_char);

    dump_fp = fopen(file_path, "wb");
    if (dump_fp) {
        av_log(NULL, AV_LOG_INFO, "open %s success\n", file_path);
    } else {
        av_log(NULL, AV_LOG_ERROR, "open %s failed\n", file_path);
        return NULL;
    }

    return dump_fp;
}

int esmpp_filter_dump_file_close(FILE *dump_fp) {
    if (!dump_fp) {
        av_log(NULL, AV_LOG_ERROR, "esmpp filter close dump file is NULL\n");
        return 0;
    }

    fflush(dump_fp);
    fclose(dump_fp);
    dump_fp = NULL;

    return 0;
}

FILE *esmpp_filter_file_write_open(const char *file_path) {
    FILE *fp = NULL;
    char *dir_path = NULL;
    char *last_slash;

    if (!file_path) {
        av_log(NULL, AV_LOG_ERROR, "Error: file path is null\n");
        return NULL;
    }

    last_slash = strrchr(file_path, '/');
    if (last_slash) {
        dir_path = av_strndup(file_path, last_slash - file_path);
        if (!dir_path) {
            av_log(NULL, AV_LOG_ERROR, "Error: out of memory\n");
            return NULL;
        }

        if (access(dir_path, F_OK) == -1) {
            av_log(NULL, AV_LOG_INFO, "Directory %s does not exist, creating...\n", dir_path);
            if (mkdir(dir_path, 0755) == -1) {
                av_log(NULL, AV_LOG_ERROR, "Failed to create directory %s, errno=%d (%s)\n",
                       dir_path, errno, strerror(errno));
                av_free(dir_path);
                return NULL;
            }
            av_log(NULL, AV_LOG_INFO, "Directory %s created successfully\n", dir_path);
        } else {
            av_log(NULL, AV_LOG_INFO, "Directory %s already exists\n", dir_path);
        }
        av_free(dir_path);
    }

    fp = fopen(file_path, "w");
    if (fp) {
        av_log(NULL, AV_LOG_INFO, "Opened file successfully: %s\n", file_path);
    } else {
        av_log(NULL, AV_LOG_ERROR, "Failed to open file: %s (errno=%d, %s)\n",
               file_path, errno, strerror(errno));
    }

    return fp;
}