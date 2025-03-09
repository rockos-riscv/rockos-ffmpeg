#include "vf_esmpp_common.h"

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

    if (!buffer || size <= 0) {
        return ES_FALSE;
    }
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
            av_log(NULL, AV_LOG_WARNING, "Success write %zu bytes to %s\n", totalSize, file_path);
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

    if (!in) {
        av_log(NULL, AV_LOG_ERROR, "esmpp_get_frame_data_size invaild paras, in: %p\n", in);
        return FAILURE;
    }

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
        memcpy(out_vir + totol_size, in->data[i], cp_size);
        totol_size += cp_size;
    }
    return SUCCESS;
}

int frame_create_buf(
    AVFrame *frame, uint8_t *data, int size, void (*free)(void *opaque, uint8_t *data), void *opaque, int flags) {
    int i;

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

    *width = FFALIGN(*width, align);
    *height = FFALIGN(*height, 2);
}