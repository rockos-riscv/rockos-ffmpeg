#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_esmpp.h"
#include "libavcodec/defs.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "avfilter.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "mpp_tde_api.h"
#include "mpp_buffer.h"
#include "video.h"

#define SUCCESS (0)
#define FAILURE (-1)
#define OFFSET(x) offsetof(MppFilterContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define MAX_INPUT_NB 2

#define ESMPP_COMPLEX_DUMP (1)
#define ESMPP_ACTIVE (1)

typedef struct MppFilterContext {
    const AVClass *class;
    FFFrameSync fs;
    int nb_inputs;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;

    AVBufferRef *hw_device_ref;
    AVBufferRef *input_hw_frm_ref;
    AVBufferRef *output_hw_frm_ref;
    AVESMPPFramesContext *mpp_dev_ctx;
    AVHWFramesContext *input_hw_frm_ctx;
    AVHWFramesContext *out_hw_frame_ctx;

    MppBufferGroupPtr buf_grp;
    RECT_S src_rect;
    RECT_S dst_rect;
    ROTATION_E src_rotation;
    ROTATION_E dst_rotation;
    ES_S32 src_global_alpha;
    ES_S32 dst_global_alpha;
    ES_S32 blend_mode;

    char *crop_set;
    char *clip_set;
    int32_t output_w_set;
    int32_t output_h_set;
    int32_t output_fmt_set;
    char *rotation_set;
    int32_t src_global_alpha_set;
    int32_t dst_global_alpha_set;
    int32_t blend_mode_set;
} MppFilterContext;

static int complex_query_formats(AVFilterContext *ctx) {
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_P010LE,
        AV_PIX_FMT_YVYU422,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_ARGB,
        AV_PIX_FMT_ABGR,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_DRM_PRIME,
        AV_PIX_FMT_NONE,
    };
    AVFilterFormats *pix_fmts = ff_make_format_list(pixel_formats);
    int ret = ff_set_common_formats(ctx, pix_fmts);
    if (ret < 0) return ret;
    return 0;
}

static av_cold int init(AVFilterContext *ctx) {
    MPP_RET mpp_ret;
    MppFilterContext *s = NULL;

    if (!ctx || !ctx->priv) {
        return FAILURE;
    }

    s = (MppFilterContext *)ctx->priv;
    s->src_global_alpha = -1;
    s->dst_global_alpha = -1;
    s->nb_inputs = 1;
    if (s->blend_mode_set != -1) {
        s->nb_inputs = 2;
    }

    mpp_ret = mpp_buffer_group_get_internal(&s->buf_grp, MPP_BUFFER_TYPE_DMA_HEAP);
    if (mpp_ret) {
        av_log(ctx, AV_LOG_ERROR, "Create buffer group with type %d failed: %d.\n", MPP_BUFFER_TYPE_DMA_HEAP, mpp_ret);
        return FAILURE;
    }
    mpp_ret = mpp_buffer_group_limit_config(s->buf_grp, 0, 0);
    if (mpp_ret) {
        av_log(ctx, AV_LOG_ERROR, "Limit buffer group with no limit failed: %d.\n", mpp_ret);
        return FAILURE;
    }

#ifdef ESMPP_ACTIVE
    for (int i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = {0};

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("in%d", i);
        if (!pad.name) return AVERROR(ENOMEM);

        if (ff_append_inpad_free_name(ctx, &pad) < 0) return FAILURE;
    }
#endif
    return SUCCESS;
}

static av_cold void uninit(AVFilterContext *ctx) {
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    if (!ctx || !ctx->priv) {
        return;
    }

    if (s->input_hw_frm_ref) {
        av_buffer_unref(&s->input_hw_frm_ref);
    }
    if (s->output_hw_frm_ref) {
        av_buffer_unref(&s->output_hw_frm_ref);
    }
    if (s->hw_device_ref) {
        av_buffer_unref(&s->hw_device_ref);
    }
    if (s->buf_grp) {
        mpp_buffer_group_put(s->buf_grp);
        s->buf_grp = NULL;
    }
}

static int set_output_fmt(MppFilterContext *s) {
    if (!s) {
        return FAILURE;
    }
    s->out_fmt = (s->output_fmt_set == -1) ? s->in_fmt : (enum AVPixelFormat)s->output_fmt_set;
    av_log(NULL, AV_LOG_INFO, "output format is %s\n", av_get_pix_fmt_name(s->out_fmt));
    return SUCCESS;
}

static int parse_rect(char *rect_cmd, RECT_S *rect) {
    if (!rect_cmd || !rect) {
        return FAILURE;
    }
    if (sscanf(rect_cmd, "%dx%dx%dx%d", &rect->x, &rect->y, &rect->width, &rect->height) == 4) {
        return SUCCESS;
    }
    return FAILURE;
}

static int parse_rotation(char *rotation_cmd, ROTATION_E *rotation) {
    if (!rotation_cmd || !rotation) {
        return FAILURE;
    }
    if (!av_strcasecmp(rotation_cmd, "90")) {
        *rotation = ROTATION_90;
    } else if (!av_strcasecmp(rotation_cmd, "180")) {
        *rotation = ROTATION_180;
    } else if (!av_strcasecmp(rotation_cmd, "270")) {
        *rotation = ROTATION_270;
    } else if (!av_strcasecmp(rotation_cmd, "h")) {
        *rotation = ROTATION_FLIP_H;
    } else if (!av_strcasecmp(rotation_cmd, "v")) {
        *rotation = ROTATION_FLIP_V;
    } else if (!av_strcasecmp(rotation_cmd, "0")) {
        *rotation = ROTATION_0;
    } else {
        return FAILURE;
    }
    return SUCCESS;
}

static int parse_blend_mode(int blend_mode_cmd, int *mode) {
    if (!mode) {
        return FAILURE;
    }
    switch (blend_mode_cmd) {
        case 0:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blending mode is 'SRC'");
            *mode = TDE_USAGE_BLEND_SRC;
            break;
        case 1:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'DST'");
            *mode = TDE_USAGE_BLEND_DST;
            break;
        case 2:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'SRC over DST'");
            *mode = TDE_USAGE_BLEND_SRC_OVER;
            break;
        case 3:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'DST over SRC'");
            *mode = TDE_USAGE_BLEND_DST_OVER;
            break;
        case 4:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'SRC in DST'");
            *mode = TDE_USAGE_BLEND_SRC_IN;
            break;
        case 5:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'DST in SRC'");
            *mode = TDE_USAGE_BLEND_DST_IN;
            break;
        case 6:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'SRC out DST'");
            *mode = TDE_USAGE_BLEND_SRC_OUT;
            break;
        case 7:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'DST out SRC'");
            *mode = TDE_USAGE_BLEND_DST_OUT;
            break;
        case 8:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'SRC ATOP'");
            *mode = TDE_USAGE_BLEND_SRC_ATOP;
            break;
        case 9:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'DST ATOP'");
            *mode = TDE_USAGE_BLEND_DST_ATOP;
            break;
        case 10:
            av_log(NULL, AV_LOG_DEBUG, "Alpha blend mode is 'XOR'");
            *mode = TDE_USAGE_BLEND_XOR;
            break;
        default:
            return FAILURE;
    }
    return SUCCESS;
}

static int get_alignment_by_format(enum AVPixelFormat fmt) {
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

static void adjust_width_height_by_format(enum AVPixelFormat fmt, int *width, int *height) {
    int align = get_alignment_by_format(fmt);

    *width = FFALIGN(*width, align);
    *height = FFALIGN(*height, 2);
}

static MppFrameFormat ff_fmt_to_mpp_fmt(enum AVPixelFormat ff_fmt) {
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

static size_t get_pic_buf_info(
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

#if ESMPP_COMPLEX_DUMP
#define MAX_FILE_PATH (200)
static ES_BOOL write_buffer_to_file(const void *buffer,
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
#endif

static void esmpp_free_frame_buf(void *opaque, uint8_t *data) {
    MppBufferPtr dst_mpp_buf = opaque;
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
}

static int esmpp_set_mpp_frame(MppFramePtr mpp_frame, const AVFrame *in, const MppFilterContext *s) {
    mpp_frame_set_width(mpp_frame, in->width);
    mpp_frame_set_height(mpp_frame, in->height);
    mpp_frame_set_fmt(mpp_frame, ff_fmt_to_mpp_fmt(s->in_fmt));
    mpp_frame_set_rotation(mpp_frame, s->src_rotation);
    mpp_frame_set_global_alpha(mpp_frame, s->src_global_alpha);
    return 0;
}

static int esmpp_get_frame_data_size(const MppFilterContext *s, const AVFrame *in) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int total_size = 0;

    if (!in) {
        av_log(NULL, AV_LOG_ERROR, "esmpp_get_frame_data_size invaild paras, in: %p\n", in);
        return FAILURE;
    }

    desc = av_pix_fmt_desc_get(s->in_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_get_frame_data_size get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(s->out_fmt));
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

static int esmpp_memcpy_host2device(const MppFilterContext *s, const AVFrame *in, void *out_vir) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int cp_size = 0;
    int totol_size = 0;

    desc = av_pix_fmt_desc_get(s->in_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_memcpy_host2device get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(s->out_fmt));
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

static int esmpp_complex_filter_frame(int input_nb, AVFilterLink **link, AVFrame **in) {
    AVFilterLink *link_src = link[0], *link_dst = input_nb > 1 ? link[1] : NULL;
    AVFrame *in_src = in[0], *in_dst = input_nb > 1 ? in[1] : NULL;
    AVFilterContext *ctx = link_src->dst;
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    int ret = SUCCESS;
    MPP_RET mpp_ret = MPP_OK;
    MppFramePtr src_mpp_frame = NULL;
    MppFramePtr dst_mpp_frame = NULL;
    MppBufferPtr src_mpp_buf = NULL;
    MppBufferPtr dst_mpp_buf = NULL;
    ES_BOOL is_hw = ES_FALSE;
    ES_S32 usage = 0;
    int in_frame_size = 0;

    if (link_src->format == AV_PIX_FMT_DRM_PRIME && !in_src->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        ret = FAILURE;
        goto exit2;
    }

    if (mpp_frame_init(&src_mpp_frame) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Init src mpp frame failed.\n");
        ret = FAILURE;
        goto exit1;
    }
    if (mpp_frame_init(&dst_mpp_frame) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Init dst mpp frame failed.\n");
        ret = FAILURE;
        goto exit2;
    }

    // handle src mpp frame
    esmpp_set_mpp_frame(src_mpp_frame, in_src, s);
    in_frame_size = esmpp_get_frame_data_size(s, in_src);  // in_src->buf[0]->size;
    if (link_src->format == AV_PIX_FMT_DRM_PRIME && in_src->hw_frames_ctx) {
        is_hw = ES_TRUE;
        if (in_src->buf[0]) {
            src_mpp_buf = (MppBufferPtr)in_src->buf[0]->data;
            if (!src_mpp_buf) {
                av_log(ctx, AV_LOG_ERROR, "src_mpp_buf is NULL\n");
                goto exit2;
            }
            mpp_buffer_inc_ref(src_mpp_buf);
        } else {
            av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
            goto exit2;
        }

        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        mpp_frame_set_buf_size(src_mpp_frame, in_frame_size);
    } else {
        // size_t input_frame_size = in_src->buf[0]->size;
        mpp_ret = mpp_buffer_get(s->buf_grp, &src_mpp_buf, in_frame_size);
        if (mpp_ret) {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", in_src->buf[0]->size, mpp_ret);
            ret = FAILURE;
            goto exit3;
        }
        // memcpy(mpp_buffer_get_ptr(src_mpp_buf), in_src->buf[0]->data, in_frame_size);
        esmpp_memcpy_host2device(s, in_src, mpp_buffer_get_ptr(src_mpp_buf));
        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        mpp_frame_set_buf_size(src_mpp_frame, in_frame_size);
    }

    // handle dst mpp frame
    {
        size_t output_frame_size = 0;
        AVBufferRef *buf;
        int plane = 0, stride[3] = {0}, offset[3] = {0};

        out = av_frame_alloc();
        if (!out) {
            av_log(ctx, AV_LOG_ERROR, "av_frame_alloc error.\n");
            goto exit3;
        }

        if (in_dst) {
            av_frame_copy_props(out, in_dst);
        } else {
            av_frame_copy_props(out, in_src);
        }

        out->format = s->out_fmt;
        out->width = s->dst_rect.width;
        out->height = s->dst_rect.height;

        output_frame_size = get_pic_buf_info(
            out->format, out->width, out->height, get_alignment_by_format(out->format), 2, stride, offset, &plane);

        av_log(ctx,
               AV_LOG_INFO,
               "out info size:%ld plane:%d stride:%d-%d-%d, offset:%d-%d-%d.\n",
               output_frame_size,
               plane,
               stride[0],
               stride[1],
               stride[2],
               offset[0],
               offset[1],
               offset[2]);

        mpp_ret = mpp_buffer_get(s->buf_grp, &dst_mpp_buf, output_frame_size);
        if (mpp_ret) {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", output_frame_size, mpp_ret);
            ret = FAILURE;
            goto exit3;
        }
        mpp_frame_set_width(dst_mpp_frame, out->width);
        mpp_frame_set_height(dst_mpp_frame, out->height);
        mpp_frame_set_fmt(dst_mpp_frame, ff_fmt_to_mpp_fmt(out->format));
        mpp_frame_set_rotation(dst_mpp_frame, s->dst_rotation);
        mpp_frame_set_global_alpha(dst_mpp_frame, s->dst_global_alpha);
        mpp_frame_set_hor_stride(dst_mpp_frame, get_alignment_by_format(out->format));

        mpp_frame_set_buffer(dst_mpp_frame, dst_mpp_buf);
        mpp_frame_set_buf_size(dst_mpp_frame, output_frame_size);

        buf = av_buffer_create(mpp_buffer_get_ptr(dst_mpp_buf),
                               mpp_buffer_get_size(dst_mpp_buf),
                               esmpp_free_frame_buf,
                               dst_mpp_buf,
                               AV_BUFFER_FLAG_READONLY);
        if (!buf) {
            goto exit3;
        }

        if (in_dst) {
            memcpy(mpp_buffer_get_ptr(dst_mpp_buf), in_dst->buf[0]->data, output_frame_size);
        }

        out->buf[0] = buf;
        for (int i = 0; i < plane; i++) {
            out->linesize[i] = stride[i];
            out->data[i] = out->buf[0]->data + offset[i];
        }
    }

    if (s->blend_mode & TDE_USAGE_BLEND_MASK) {
        usage |= s->blend_mode;
    }
#if ESMPP_COMPLEX_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(src_mpp_buf), in_frame_size, NULL, in_src->width, in_src->height, s->in_fmt, 0);
    if (in_dst) {
        write_buffer_to_file(
            mpp_buffer_get_ptr(dst_mpp_buf), out->buf[0]->size, NULL, out->width, out->height, out->format, 1);
    }
#endif
    mpp_ret = es_tde_complex_process(src_mpp_frame, dst_mpp_frame, NULL, &s->src_rect, &s->dst_rect, NULL, usage);
    av_log(ctx, AV_LOG_WARNING, "es_tde_complex_process return %d\n", mpp_ret);
    if (mpp_ret != MPP_OK) {
        ret = FAILURE;
        goto exit3;
    }

    ret = SUCCESS;

#if ESMPP_COMPLEX_DUMP
    write_buffer_to_file(out->buf[0]->data, out->buf[0]->size, NULL, out->width, out->height, out->format, 2);
#endif
    goto exit2;

exit3:
    if (!is_hw && out) {
        av_frame_free(&out);
    }
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
exit2:
    if (src_mpp_buf) {
        mpp_buffer_put(src_mpp_buf);
    }
    if (dst_mpp_frame) {
        mpp_frame_deinit(&dst_mpp_frame);
    }
exit1:
    if (src_mpp_frame) {
        mpp_frame_deinit(&src_mpp_frame);
    }
    av_frame_free(&in_src);

    if (ret == SUCCESS) {
        return ff_filter_frame(outlink, out);
    } else {
        return ret;
    }
}

static int complex_filter_frame(AVFilterLink *link, AVFrame *frame) {
    return esmpp_complex_filter_frame(1, &link, &frame);
}

static int process_frame(FFFrameSync *fs) {
    AVFilterContext *ctx = fs->parent;
    MppFilterContext *s = fs->opaque;
    AVFrame *in[MAX_INPUT_NB] = {0};
    int i, ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if ((ret = ff_framesync_get_frame(&s->fs, i, &in[i], 1)) < 0) {
            return ret;
        }
        av_log(ctx,
               AV_LOG_INFO,
               "vf[esmpp_complex] process_frame nb_inputs:%d i:%d size:%ld.\n",
               s->nb_inputs,
               i,
               in[i]->buf[0]->size);
    }

    return esmpp_complex_filter_frame(ctx->nb_inputs, ctx->inputs, in);
}

static av_cold int complex_config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0], *inlink_dst = ctx->inputs[1];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    FFFrameSyncIn *in;
    int i, ret;

    s->src_rect.x = 0;
    s->src_rect.y = 0;
    s->src_rect.width = inlink->w;
    s->src_rect.height = inlink->h;
    if (s->crop_set) {
        if (parse_rect(s->crop_set, &s->src_rect)) {
            av_log(ctx, AV_LOG_ERROR, "vf[esmpp_complex] parse crop cmd failed.\n");
            return FAILURE;
        }
    }
    // If not set clip/o_w/o_h, output as src_rect after crop
    memcpy(&s->dst_rect, &s->src_rect, sizeof(RECT_S));

    if (s->clip_set) {
        if (parse_rect(s->clip_set, &s->dst_rect)) {
            av_log(ctx, AV_LOG_ERROR, "vf[esmpp_complex] parse clip cmd failed.\n");
            return FAILURE;
        }
    }

    if (s->rotation_set) {
        if (parse_rotation(s->rotation_set, &s->dst_rotation)) {
            av_log(ctx, AV_LOG_ERROR, "vf[esmpp_complex] parse dst rotation cmd failed.\n");
            return FAILURE;
        }
        if (s->dst_rotation == ROTATION_90 || s->dst_rotation == ROTATION_270) {
            int tmp = s->dst_rect.width;
            s->dst_rect.width = s->dst_rect.height;
            s->dst_rect.height = tmp;
        }
    }
    if (s->output_w_set) {
        s->dst_rect.width = s->output_w_set;
    }
    if (s->output_h_set) {
        s->dst_rect.height = s->output_h_set;
    }
    outlink->w = s->dst_rect.width;
    outlink->h = s->dst_rect.height;

    s->src_global_alpha = s->src_global_alpha_set;
    s->dst_global_alpha = s->dst_global_alpha_set;

    if (s->blend_mode_set != -1) {
        if (parse_blend_mode(s->blend_mode_set, &s->blend_mode)) {
            av_log(ctx, AV_LOG_ERROR, "vf[esmpp_complex] parse blend mode cmd failed.\n");
            return FAILURE;
        }
    }

    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx) {
        s->input_hw_frm_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        s->mpp_dev_ctx = (AVESMPPFramesContext *)s->input_hw_frm_ctx->device_ctx->hwctx;
        s->input_hw_frm_ref = av_buffer_ref(inlink->hw_frames_ctx);
        if (!s->input_hw_frm_ref) {
            av_log(ctx, AV_LOG_ERROR, "av_buffer_ref(inlink->hw_frames_ctx) failed\n");
            return FAILURE;
        }
        s->hw_device_ref = av_buffer_ref(s->input_hw_frm_ctx->device_ref);
        if (!s->hw_device_ref) {
            av_log(ctx, AV_LOG_ERROR, "av_buffer_ref(input_hw_frm_ctx->device_ref) failed\n");
            av_buffer_unref(&s->input_hw_frm_ref);
            return FAILURE;
        }
        s->in_fmt = s->input_hw_frm_ctx->sw_format;
        set_output_fmt(s);
        outlink->format = inlink->format;

        av_buffer_unref(&s->output_hw_frm_ref);
        s->output_hw_frm_ref = av_hwframe_ctx_alloc(s->hw_device_ref);
        if (!s->output_hw_frm_ref) {
            av_log(ctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed \n");
            av_buffer_unref(&s->input_hw_frm_ref);
            av_buffer_unref(&s->hw_device_ref);
            return AVERROR(EINVAL);
        }
        s->out_hw_frame_ctx = (AVHWFramesContext *)s->output_hw_frm_ref->data;
        s->out_hw_frame_ctx->format = AV_PIX_FMT_DRM_PRIME;
        s->out_hw_frame_ctx->sw_format = s->out_fmt;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);
        s->out_hw_frame_ctx->width = outlink->w;
        s->out_hw_frame_ctx->height = outlink->h;
        outlink->hw_frames_ctx = av_buffer_ref(s->output_hw_frm_ref);
    } else {
        s->in_fmt = inlink->format;
        set_output_fmt(s);
        outlink->format = s->out_fmt;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);
    }

#ifdef ESMPP_ACTIVE
    if ((ret = ff_framesync_init(&s->fs, ctx, s->nb_inputs)) < 0) {
        return ret;
    }
    in = s->fs.in;
    s->fs.opaque = s;
    s->fs.on_event = process_frame;

    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterLink *inlink = ctx->inputs[i];

        in[i].time_base = inlink->time_base;
        in[i].sync = 1;
        in[i].before = EXT_STOP;
        in[i].after = EXT_INFINITY;
    }
    ret = ff_framesync_configure(&s->fs);
    outlink->time_base = s->fs.time_base;
#endif
    return SUCCESS;
}

static int esmpp_complex_filter_activate(AVFilterContext *ctx) {
    MppFilterContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static const AVOption options[] = {
    {
        "crop",
        "Set the crop rectangle of source image: (xoffset)x(yoffset)x(width)x(height)",
        OFFSET(crop_set),
        AV_OPT_TYPE_STRING,
        {.str = NULL},
        0,
        0,
        FLAGS,
    },
    {
        "clip",
        "Set the clip rectangle of destination image: (xoffset)x(yoffset)x(width)x(height)",
        OFFSET(clip_set),
        AV_OPT_TYPE_STRING,
        {.str = NULL},
        0,
        0,
        FLAGS,
    },
    {"o_w", "Set output image width", OFFSET(output_w_set), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS, "o_w"},
    {"o_h", "Set output image width", OFFSET(output_h_set), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS, "o_h"},
    {"o_fmt", "output pixfmt", OFFSET(output_fmt_set), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, "fmt"},
    {"nv12", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, INT_MAX, FLAGS, "fmt"},
    {"nv21", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV21}, 0, INT_MAX, FLAGS, "fmt"},
    {"i420", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P}, 0, INT_MAX, FLAGS, "fmt"},
    {"gray", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAY8}, 0, INT_MAX, FLAGS, "fmt"},
    {"i010", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P10LE}, 0, INT_MAX, FLAGS, "fmt"},
    {"p010", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, INT_MAX, FLAGS, "fmt"},
    {"yvy2", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YVYU422}, 0, INT_MAX, FLAGS, "fmt"},
    {"yuy2", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUYV422}, 0, INT_MAX, FLAGS, "fmt"},
    {"uyvy", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_UYVY422}, 0, INT_MAX, FLAGS, "fmt"},
    {"nv16", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV16}, 0, INT_MAX, FLAGS, "fmt"},
    {"rgb24", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB24}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgr24", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR24}, 0, INT_MAX, FLAGS, "fmt"},
    {"argb", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ARGB}, 0, INT_MAX, FLAGS, "fmt"},
    {"abgr", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ABGR}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgra", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGRA}, 0, INT_MAX, FLAGS, "fmt"},
    {"rgba", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGBA}, 0, INT_MAX, FLAGS, "fmt"},
    {
        "blend_mode",
        "Set alpha blend mode: "
        "0[SRC] 1[DST] 2[SRC over DST] 3[DST over SRC] 4[SRC in DST] 5[DST in SRC] "
        "6[SRC out DST] 7[DST out SRC] 8[SRC ATOP] 9[DST ATOP] 10[XOR]",
        OFFSET(blend_mode_set),
        AV_OPT_TYPE_INT,
        {.i64 = -1},
        -1,
        10,
        FLAGS,
        "blend_mode",
    },
    {
        "rot",
        "Set destination rotation [90, 180, 270, h, v]",
        OFFSET(rotation_set),
        AV_OPT_TYPE_STRING,
        {.str = NULL},
        0,
        0,
        FLAGS,
    },
    {
        "src_alpha",
        "Set source global alhpa value [-1, 255]",
        OFFSET(src_global_alpha_set),
        AV_OPT_TYPE_INT,
        {.i64 = -1},
        -1,
        255,
        FLAGS,
        "src_alpha",
    },
    {
        "dst_alpha",
        "Set destination global alhpa value [-1, 255]",
        OFFSET(dst_global_alpha_set),
        AV_OPT_TYPE_INT,
        {.i64 = -1},
        -1,
        255,
        FLAGS,
        "dst_alpha",
    },
    {NULL},
};

static const AVClass complex_class = {
    .class_name = "esmpp_complex",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

// for dynamic inputs, input pads should be inited in func init.
static const AVFilterPad complex_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = complex_filter_frame,
    },
};

static const AVFilterPad complex_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = complex_config_props,
    },
};

AVFilter ff_vf_esmpp_complex = {
    .name = "esmpp_complex",
    .description = NULL_IF_CONFIG_SMALL("eswin esmpp complex filter"),

    .init = init,
    .uninit = uninit,

    .priv_size = sizeof(MppFilterContext),
    .priv_class = &complex_class,
#ifdef ESMPP_ACTIVE
    .activate = &esmpp_complex_filter_activate,
#else
    FILTER_INPUTS(complex_inputs),
#endif
    FILTER_OUTPUTS(complex_outputs),
    FILTER_QUERY_FUNC(complex_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
