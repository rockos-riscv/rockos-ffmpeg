#include "vf_esmpp_common.h"

#define MAX_FILE_PATH (200)

static inline void set_id_used(int id, uint32_t *id_bitmap) {
    id_bitmap[id / BITS_PER_UNIT] |= (1U << (id % BITS_PER_UNIT));
}

static inline void clear_id_used(int id, uint32_t *id_bitmap) {
    id_bitmap[id / BITS_PER_UNIT] &= ~(1U << (id % BITS_PER_UNIT));
}

static inline int is_id_used(int id, uint32_t *id_bitmap) {
    return (id_bitmap[id / BITS_PER_UNIT] & (1U << (id % BITS_PER_UNIT))) != 0;
}

int allocate_min_id(uint32_t *id_bitmap) {
    for (int i = 0; i < MAX_ID; i++) {
        if (!is_id_used(i, id_bitmap)) {
            set_id_used(i, id_bitmap);
            return i;
        }
    }
    return -1;
}

void release_id(int id, uint32_t *id_bitmap) {
    if (id >= 0 && id < MAX_ID) {
        clear_id_used(id, id_bitmap);
    }
}

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

int esmpp_complex_frame_clone(AVFrame **pdst_frame,
                              const AVFrame *ref_frame,
                              const AVFilterLink *outlink,
                              MppFramePtr *pdst_mpp_frame,
                              MppBufferPtr *pdst_mpp_buf,
                              ES_S32 width,
                              ES_S32 height,
                              ROTATION_E rotation,
                              ES_S32 global_alpha,
                              enum AVPixelFormat frame_fmt,
                              const MppBufferGroupPtr buf_grp) {
    AVFilterContext *ctx = outlink->src;
    AVFrame *dst_frame = NULL;
    MppBufferPtr dst_mpp_buf = NULL;
    MppFramePtr dst_mpp_frame = NULL;
    MPP_RET mpp_ret = MPP_OK;
    size_t output_frame_size = 0;
    int plane = 0, stride[3] = {0}, offset[3] = {0};
    int ret = FAILURE;

    if (NULL == ref_frame) {
        av_log(ctx, AV_LOG_ERROR, "referenc frame is NULL! \n");
        return ret;
    }

    // prepare for out avframe
    dst_frame = av_frame_alloc();
    if (!dst_frame) {
        av_log(ctx, AV_LOG_ERROR, "av_frame_alloc error.\n");
        goto exit;
    }

    av_frame_copy_props(dst_frame, ref_frame);

    dst_frame->format = frame_fmt;
    dst_frame->width = width;
    dst_frame->height = height;

    // handle dst mpp frame
    if (mpp_frame_init(&dst_mpp_frame) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Init dst mpp frame failed.\n");
        ret = FAILURE;
        goto exit;
    }
    output_frame_size =
        get_pic_buf_info(frame_fmt, width, height, get_alignment_by_format(frame_fmt), 2, stride, offset, &plane);
    av_log(ctx,
           AV_LOG_TRACE,
           "dst_frame info size:%ld plane:%d stride:%d-%d-%d, offset:%d-%d-%d fmt:%d.\n",
           output_frame_size,
           plane,
           stride[0],
           stride[1],
           stride[2],
           offset[0],
           offset[1],
           offset[2],
           frame_fmt);

    // for blend hwcxt, will use the original buf
    if (outlink->hw_frames_ctx && ref_frame) {
        if (ref_frame->data[0]) {
            AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)ref_frame->data[0];
            dst_mpp_buf = desc->buffers[0];
            if (!dst_mpp_buf) {
                av_log(ctx, AV_LOG_ERROR, "dst_mpp_buf is NULL\n");
                ret = FAILURE;
                goto exit;
            }
            mpp_buffer_inc_ref(dst_mpp_buf);
        } else {
            av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
            ret = FAILURE;
            goto exit;
        }
    } else {
        mpp_ret = mpp_buffer_get(buf_grp, &dst_mpp_buf, output_frame_size);
        if (mpp_ret) {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", output_frame_size, mpp_ret);
            ret = FAILURE;
            goto exit;
        }
    }

    esmpp_set_mpp_frame(dst_mpp_frame,
                        ff_fmt_to_mpp_fmt(dst_frame->format),
                        dst_frame->width,
                        dst_frame->height,
                        stride,
                        offset,
                        rotation,
                        global_alpha);
    mpp_frame_set_buffer(dst_mpp_frame, dst_mpp_buf);
    mpp_frame_set_buf_size(dst_mpp_frame, output_frame_size);

    if (outlink->hw_frames_ctx) {
        dst_frame->format = AV_PIX_FMT_DRM_PRIME;
        dst_frame->hw_frames_ctx = av_buffer_ref(outlink->hw_frames_ctx);
        esmpp_buffer_export_frame(dst_frame, dst_mpp_frame, plane, offset, stride);
    } else {
        AVBufferRef *buf = NULL;
        buf = av_buffer_create(mpp_buffer_get_ptr(dst_mpp_buf),
                               sizeof(dst_mpp_buf),
                               esmpp_free_frame_buf,
                               dst_mpp_buf,
                               AV_BUFFER_FLAG_READONLY);
        if (!buf) {
            ret = FAILURE;
            goto exit;
        }

        if (ref_frame) {
            esmpp_memcpy_host2device(frame_fmt, ref_frame, mpp_buffer_get_ptr(dst_mpp_buf));
        }

        dst_frame->buf[0] = buf;
        for (int i = 0; i < plane; i++) {
            dst_frame->linesize[i] = stride[i];
            dst_frame->data[i] = dst_frame->buf[0]->data + offset[i];
        }
    }

    *pdst_frame = dst_frame;
    *pdst_mpp_frame = dst_mpp_frame;
    *pdst_mpp_buf = dst_mpp_buf;

    return SUCCESS;
exit:
    if (dst_frame) {
        av_frame_free(&dst_frame);
    }
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
    if (dst_mpp_frame) {
        mpp_frame_deinit(&dst_mpp_frame);
    }

    return ret;
}

int esmpp_stack_get_offset_stride(
    const AVFrame *in, enum AVPixelFormat fmt, int *stride, int *offset, uint32_t *pic_size) {
    int hshift, vshift, planes;
    int offset_sum = 0;
    uint32_t i = 0;

    if (!in || !stride || !offset) {
        av_log(NULL, AV_LOG_ERROR, "input invaild params, frame: %p,stride: %p, stride: %p\n", in, stride, offset);
        return FAILURE;
    }

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
    AVFilterContext *ctx = filterlink->dst;
    AVHWFramesContext *hwfc = NULL;
    AVHWDeviceContext *hwdev;
    AVESMPPDeviceContext *hwctx;
    AVBufferRef *hw_device_ref_p, *hw_frm_ref_p;
    int flags = MPP_BUFFER_TYPE_DMA_HEAP;
    int ret = 0;

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