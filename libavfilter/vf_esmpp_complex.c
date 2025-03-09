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
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "mpp_tde_api.h"
#include "video.h"
#include "vf_esmpp_common.h"

#define OFFSET(x) offsetof(MppFilterContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define MAX_INPUT_NB 2

#define ESMPP_COMPLEX_DUMP (0)

typedef struct MppFilterContext {
    const AVClass *class;
    FFFrameSync fs;
    int nb_inputs;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;

    // output hwcontext
    AVBufferRef *out_hw_device_ref;
    AVBufferRef *out_hw_frm_ref;
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
    return ff_set_common_formats_from_list(ctx, pixel_formats);
}

static int init_hwcontext_buf(AVFilterLink *filterlink,
                              AVBufferRef **hw_device_ref,
                              AVBufferRef **hw_frm_ref,
                              int pixelfmt) {
    AVFilterContext *ctx = filterlink->dst;
    AVHWFramesContext *hwfc = NULL;
    AVESMPPFramesContext *mppfc;
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

    mppfc = hwfc->hwctx;
    mppfc->die_idx = 0;  // TODO: adapt multi-dies
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

static av_cold int init(AVFilterContext *ctx) {
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

    for (int i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = {0};

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("in%d", i);
        if (!pad.name) return AVERROR(ENOMEM);

        if (ff_append_inpad_free_name(ctx, &pad) < 0) return FAILURE;
    }
    return SUCCESS;
}

static av_cold void uninit(AVFilterContext *ctx) {
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    if (!ctx || !ctx->priv) {
        return;
    }

    ff_framesync_uninit(&s->fs);

    if (s->out_hw_frm_ref) {
        av_buffer_unref(&s->out_hw_frm_ref);
    }
    if (s->out_hw_device_ref) {
        av_buffer_unref(&s->out_hw_device_ref);
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
    av_log(NULL, AV_LOG_DEBUG, "output format is %s\n", av_get_pix_fmt_name(s->out_fmt));
    return SUCCESS;
}

static int parse_rect(const char *rect_cmd, RECT_S *rect) {
    if (!rect_cmd || !rect) {
        return FAILURE;
    }
    if (sscanf(rect_cmd, "%dx%dx%dx%d", &rect->x, &rect->y, &rect->width, &rect->height) == 4) {
        return SUCCESS;
    }
    return FAILURE;
}

static int parse_rotation(const char *rotation_cmd, ROTATION_E *rotation) {
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

static int esmpp_set_src_mpp_frame(MppFramePtr mpp_frame, const AVFrame *in, const MppFilterContext *s) {
    mpp_frame_set_width(mpp_frame, in->width);
    mpp_frame_set_height(mpp_frame, in->height);
    mpp_frame_set_fmt(mpp_frame, ff_fmt_to_mpp_fmt(s->in_fmt));
    mpp_frame_set_rotation(mpp_frame, s->src_rotation);
    mpp_frame_set_global_alpha(mpp_frame, s->src_global_alpha);
    return 0;
}

static int esmpp_set_dst_mpp_frame(MppFramePtr mpp_frame, const AVFrame *out, const MppFilterContext *s) {
    mpp_frame_set_width(mpp_frame, out->width);
    mpp_frame_set_height(mpp_frame, out->height);
    mpp_frame_set_fmt(mpp_frame, ff_fmt_to_mpp_fmt(out->format));
    mpp_frame_set_rotation(mpp_frame, s->dst_rotation);
    mpp_frame_set_global_alpha(mpp_frame, s->dst_global_alpha);
    mpp_frame_set_hor_stride(mpp_frame, get_alignment_by_format(out->format));
    return 0;
}

static int esmpp_complex_filter_frame(int input_nb, AVFilterLink **link, AVFrame **in) {
    AVFilterLink *link_src = link[0];
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
    ES_S32 usage = 0;
    size_t in_frame_size = 0, output_frame_size = 0;
    int plane = 0, stride[3] = {0}, offset[3] = {0};

    if (link_src->format == AV_PIX_FMT_DRM_PRIME && !in_src->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        ret = FAILURE;
        goto exit1;
    }

    // handle src mpp frame
    if (mpp_frame_init(&src_mpp_frame) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Init src mpp frame failed.\n");
        ret = FAILURE;
        goto exit1;
    }
    esmpp_set_src_mpp_frame(src_mpp_frame, in_src, s);

    if (link_src->format == AV_PIX_FMT_DRM_PRIME) {
        if (in_src->data[0]) {
            AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)in_src->data[0];
            src_mpp_buf = desc->buffers[0];
            if (!src_mpp_buf) {
                av_log(ctx, AV_LOG_ERROR, "src_mpp_buf is NULL\n");
                goto exit1;
            }
            mpp_buffer_inc_ref(src_mpp_buf);
        } else {
            av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
            goto exit1;
        }

        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        in_frame_size = mpp_buffer_get_size(src_mpp_buf);
    } else {
        in_frame_size = esmpp_get_frame_data_size(s->in_fmt, in_src);
        mpp_ret = mpp_buffer_get(s->buf_grp, &src_mpp_buf, in_frame_size);
        if (mpp_ret) {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", in_src->buf[0]->size, mpp_ret);
            ret = FAILURE;
            goto exit1;
        }

        esmpp_memcpy_host2device(s->in_fmt, in_src, mpp_buffer_get_ptr(src_mpp_buf));
        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        mpp_frame_set_buf_size(src_mpp_frame, in_frame_size);
    }

    // prepare for out avframe
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

    // handle dst mpp frame
    if (mpp_frame_init(&dst_mpp_frame) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Init dst mpp frame failed.\n");
        ret = FAILURE;
        goto exit3;
    }
    output_frame_size = get_pic_buf_info(s->out_fmt,
                                         s->dst_rect.width,
                                         s->dst_rect.height,
                                         get_alignment_by_format(s->out_fmt),
                                         2,
                                         stride,
                                         offset,
                                         &plane);
    av_log(ctx,
           AV_LOG_TRACE,
           "out info size:%ld plane:%d stride:%d-%d-%d, offset:%d-%d-%d fmt:%d.\n",
           output_frame_size,
           plane,
           stride[0],
           stride[1],
           stride[2],
           offset[0],
           offset[1],
           offset[2],
           s->out_fmt);

    // for blend hwcxt, will use the original buf
    if (outlink->hw_frames_ctx && in_dst) {
        if (in_dst->data[0]) {
            AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)in_dst->data[0];
            dst_mpp_buf = desc->buffers[0];
            if (!dst_mpp_buf) {
                av_log(ctx, AV_LOG_ERROR, "dst_mpp_buf is NULL\n");
                ret = FAILURE;
                goto exit3;
            }
            mpp_buffer_inc_ref(dst_mpp_buf);
        } else {
            av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
            ret = FAILURE;
            goto exit3;
        }
    } else {
        mpp_ret = mpp_buffer_get(s->buf_grp, &dst_mpp_buf, output_frame_size);
        if (mpp_ret) {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", output_frame_size, mpp_ret);
            ret = FAILURE;
            goto exit3;
        }
    }

    esmpp_set_dst_mpp_frame(dst_mpp_frame, out, s);
    mpp_frame_set_buffer(dst_mpp_frame, dst_mpp_buf);
    mpp_frame_set_buf_size(dst_mpp_frame, output_frame_size);

    if (outlink->hw_frames_ctx) {
        out->format = AV_PIX_FMT_DRM_PRIME;
        out->hw_frames_ctx = av_buffer_ref(outlink->hw_frames_ctx);

        esmpp_buffer_export_frame(out, dst_mpp_frame, plane, offset, stride);
    } else {
        AVBufferRef *buf;
        buf = av_buffer_create(mpp_buffer_get_ptr(dst_mpp_buf),
                               sizeof(dst_mpp_buf),
                               esmpp_free_frame_buf,
                               dst_mpp_buf,
                               AV_BUFFER_FLAG_READONLY);
        if (!buf) {
            ret = FAILURE;
            goto exit3;
        }

        if (in_dst) {
            esmpp_memcpy_host2device(s->out_fmt, in_dst, mpp_buffer_get_ptr(dst_mpp_buf));
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
            mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, s->out_fmt, 1);
    }
#endif
    mpp_ret = es_tde_complex_process(src_mpp_frame, dst_mpp_frame, NULL, &s->src_rect, &s->dst_rect, NULL, usage);
    if (mpp_ret != MPP_OK) {
        ret = FAILURE;
        goto exit3;
    }

    ret = SUCCESS;

#if ESMPP_COMPLEX_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, s->out_fmt, 2);
#endif
    goto exit2;

exit3:
    if (out) {
        av_frame_free(&out);
    }
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
exit2:
    if (dst_mpp_frame) {
        mpp_frame_deinit(&dst_mpp_frame);
    }
exit1:
    if (src_mpp_buf) {
        mpp_buffer_put(src_mpp_buf);
    }
    if (src_mpp_frame) {
        mpp_frame_deinit(&src_mpp_frame);
    }

    if (ret == SUCCESS) {
        return ff_filter_frame(outlink, out);
    } else {
        return ret;
    }
}

static int process_frame(FFFrameSync *fs) {
    AVFilterContext *ctx = fs->parent;
    MppFilterContext *s = fs->opaque;
    AVFrame *in[MAX_INPUT_NB] = {0};
    int i, ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if ((ret = ff_framesync_get_frame(&s->fs, i, &in[i], 0)) < 0) {
            av_log(ctx, AV_LOG_INFO, "vf[esmpp_complex] ff_framesync_get_frame i=%d null. ret:%d\n", i, ret);
            return ret;
        }
    }

    return esmpp_complex_filter_frame(ctx->nb_inputs, ctx->inputs, in);
}

static int complex_init_output_bufgroup(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    int ret = 0;

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *hwfc = NULL;
        AVESMPPFramesContext *mppfc;
        ret = init_hwcontext_buf(outlink, &s->out_hw_device_ref, &s->out_hw_frm_ref, s->out_fmt);
        if (ret) {
            return ret;
        }

        hwfc = (AVHWFramesContext *)s->out_hw_frm_ref->data;
        mppfc = hwfc->hwctx;
        s->buf_grp = mppfc->buf_group;
    } else {
        ret = mpp_buffer_group_get_internal(&s->buf_grp, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "Create buffer group with type %d failed: %d.\n", MPP_BUFFER_TYPE_DMA_HEAP, ret);
            return FAILURE;
        }
        ret = mpp_buffer_group_limit_config(s->buf_grp, 0, 0);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "Limit buffer group with no limit failed: %d.\n", ret);
            return FAILURE;
        }
    }

    return ret;
}

static int init_framesync(AVFilterContext *avctx) {
    MppFilterContext *s = (MppFilterContext *)avctx->priv;
    int ret = 0;
    if ((ret = ff_framesync_init(&s->fs, avctx, s->nb_inputs)) < 0) {
        return ret;
    }

    for (int i = 0; i < s->nb_inputs; i++) {
        FFFrameSyncIn *in = &s->fs.in[i];

        in->before = EXT_STOP;
        in->after = EXT_INFINITY;
        in->sync = 1;
        in->time_base = avctx->inputs[i]->time_base;
    }

    s->fs.opaque = s;
    s->fs.on_event = process_frame;

    ret = ff_framesync_configure(&s->fs);
    return ret;
}

static av_cold int complex_config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0], *inlink_dst = ctx->inputs[1];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;

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
    // for blend: clip/o_w/0_h work for dst frame, other opt for src frame.
    if (!inlink_dst) {
        memcpy(&s->dst_rect, &s->src_rect, sizeof(RECT_S));
    } else {
        s->dst_rect.x = 0;
        s->dst_rect.y = 0;
        s->dst_rect.width = inlink_dst->w;
        s->dst_rect.height = inlink_dst->h;
    }

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
        AVHWFramesContext *input_hw_frm_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        s->in_fmt = input_hw_frm_ctx->sw_format;
        set_output_fmt(s);
        outlink->format = inlink->format;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);

        if (!s->buf_grp) {
            complex_init_output_bufgroup(outlink);
        }
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frm_ref);
        ctx->outputs[0]->hw_frames_ctx = outlink->hw_frames_ctx;
    } else {
        s->in_fmt = inlink->format;
        set_output_fmt(s);
        outlink->format = s->out_fmt;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);

        if (!s->buf_grp) {
            complex_init_output_bufgroup(outlink);
        }
    }

    init_framesync(ctx);
    outlink->time_base = s->fs.time_base;
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
    .activate = &esmpp_complex_filter_activate,
    FILTER_OUTPUTS(complex_outputs),
    FILTER_QUERY_FUNC(complex_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
