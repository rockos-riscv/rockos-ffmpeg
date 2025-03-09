#include "config_components.h"

#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_esmpp.h"
#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixfmt.h"

#include "internal.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#include "framesync.h"
#include "es_mpp.h"
#include "mpp_err.h"
#include "mpp_tde_api.h"
#include "mpp_buffer.h"
#include "vf_esmpp_common.h"

typedef struct _ESMPPContext {
    MppBufferGroupPtr buf_grp;

    AVBufferRef *hw_device_ref;
    AVBufferRef *in_hw_frame_ref;
    AVBufferRef *out_hw_frame_ref;

    AVESMPPFramesContext *hw_dev_ctx;
    AVHWFramesContext *in_hw_frame_ctx;
    AVHWFramesContext *out_hw_frame_ctx;

    int nb_inputs;

    enum AVPixelFormat *in_sw_fmt;
    enum AVPixelFormat out_sw_fmt;
    enum AVPixelFormat format;
    int output_width;
    int output_height;
} ESMPPContext;

#define ESMPP_STACK_DUMP (0)
#define HSTACK_NAME "hstack_esmpp"
#define VSTACK_NAME "vstack_esmpp"
#define XSTACK_NAME "xstack_esmpp"
#define HWContext ESMPPContext
#define StackHWContext StackESMPPContext
#include "stack_internal.h"

typedef struct StackESMPPContext {
    StackBaseContext base;
    RECT_S *rects;
    int nb_inputs;
} StackESMPPContext;
typedef struct MppInputParams {
    AVFrame **in;
    RECT_S *src_rects;
    RECT_S *dst_rects;
    uint32_t background_color;
    int fillcolor_enable;
} MppInputParams;

static int esmpp_stack_get_offset_stride(
    const AVFrame *in, enum AVPixelFormat fmt, int *stride, int *offset, uint32_t *pic_size) {
    int hshift, vshift, planes;
    int offset_sum = 0;
    uint32_t i = 0;

    if (!in || !stride || !offset) {
        av_log(NULL, AV_LOG_ERROR, "input invaild params, frmae: %p,stride: %p, stride: %p\n", in, stride, offset);
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

static int esmpp_stack_set_mpp_frame(MppFramePtr mpp_frame,
                                     const AVFrame *in,
                                     ROTATION_E rotation,
                                     ES_S32 global_alpha,
                                     int *stride,
                                     int *offset,
                                     enum AVPixelFormat fmt) {
    mpp_frame_set_stride(mpp_frame, stride);
    mpp_frame_set_offset(mpp_frame, offset);
    mpp_frame_set_width(mpp_frame, in->width);
    mpp_frame_set_height(mpp_frame, in->height);
    mpp_frame_set_fmt(mpp_frame, ff_fmt_to_mpp_fmt(fmt));
    mpp_frame_set_rotation(mpp_frame, rotation);
    mpp_frame_set_global_alpha(mpp_frame, global_alpha);
    return 0;
}

static int esmpp_stack_filter_frame(int input_nb, AVFilterContext *avctx, MppInputParams *inParams, AVFrame *oframe) {
    AVFrame **in_src = NULL;
    AVFilterContext *ctx = NULL;
    ESMPPContext *esctx = NULL;
    AVFrame *out = NULL;
    int ret = SUCCESS;
    MPP_RET mpp_ret = MPP_OK;
    MppFramePtr *src_mpp_frame = NULL;
    MppFramePtr dst_mpp_frame = NULL;
    MppBufferPtr *src_mpp_buf = NULL;
    MppBufferPtr dst_mpp_buf = NULL;
    ES_BOOL is_hw = ES_FALSE;
    size_t *in_frame_size = NULL, output_frame_size = 0;

    if (!avctx || !inParams || !oframe) {
        av_log(NULL,
               AV_LOG_ERROR,
               "esmpp_stack_filter_frame input invaild params, avctx: %p, inParams: %p, oframe: %p\n",
               avctx,
               inParams,
               oframe);
        return FAILURE;
    }

    in_src = inParams->in;
    ctx = avctx;
    esctx = ctx->priv;

    if (esctx->format == AV_PIX_FMT_DRM_PRIME && !in_src[0]->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        ret = FAILURE;
        goto exit2;
    }
    src_mpp_frame = av_calloc(avctx->nb_inputs, sizeof(MppFramePtr));
    if (!src_mpp_frame) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }
    src_mpp_buf = av_calloc(avctx->nb_inputs, sizeof(MppBufferPtr));
    if (!src_mpp_buf) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }
    in_frame_size = av_calloc(avctx->nb_inputs, sizeof(size_t));
    if (!src_mpp_buf) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }

    // handle src mpp frame
    for (ES_S32 i = 0; i < input_nb; i++) {
        MppFramePtr mpp_frame = NULL;
        MppBufferPtr mpp_buf = NULL;
        int stride[4] = {0}, offset[4] = {0};
        uint32_t pic_size = 0;

        if (mpp_frame_init(&mpp_frame) != MPP_OK) {
            av_log(ctx, AV_LOG_ERROR, "Init src mpp frame failed.\n");
            ret = FAILURE;
            goto exit1;
        }

        esmpp_stack_get_offset_stride(in_src[i], esctx->in_sw_fmt[i], stride, offset, &pic_size);
        esmpp_stack_set_mpp_frame(mpp_frame, in_src[i], 0, -1, stride, offset, esctx->in_sw_fmt[i]);
        if (esctx->format == AV_PIX_FMT_DRM_PRIME && in_src[i]->hw_frames_ctx) {
            is_hw = ES_TRUE;
            if (in_src[i]->buf[0]) {
                mpp_buf = (MppBufferPtr)in_src[i]->buf[0]->data;
                if (!mpp_buf) {
                    av_log(ctx, AV_LOG_ERROR, "mpp_buf is NULL\n");
                    goto exit2;
                }
                mpp_buffer_inc_ref(mpp_buf);
            } else {
                av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
                goto exit2;
            }
            mpp_frame_set_buffer(mpp_frame, mpp_buf);
            in_frame_size[i] = mpp_buffer_get_size(mpp_buf);
            src_mpp_frame[i] = mpp_frame;
            src_mpp_buf[i] = mpp_buf;
        } else {
            // in_frame_size[i] = esmpp_get_frame_data_size(esctx->in_sw_fmt[i], in_src[i]);
            in_frame_size[i] = pic_size;
            mpp_ret = mpp_buffer_get(esctx->buf_grp, &mpp_buf, in_frame_size[i]);
            if (mpp_ret) {
                av_log(ctx,
                       AV_LOG_ERROR,
                       "Get buffer from group with %zu failed: %d.\n",
                       in_src[i]->buf[0]->size,
                       mpp_ret);
                ret = FAILURE;
                goto exit3;
            }
            esmpp_memcpy_host2device(esctx->in_sw_fmt[i], in_src[i], mpp_buffer_get_ptr(mpp_buf));
            mpp_frame_set_buffer(mpp_frame, mpp_buf);
            mpp_frame_set_buf_size(mpp_frame, in_frame_size[i]);
            src_mpp_frame[i] = mpp_frame;
            src_mpp_buf[i] = mpp_buf;
        }
    }

    // handle dst mpp frame
    {
        AVBufferRef *buf;
        int plane = 0, stride[3] = {0}, offset[3] = {0};

        if (mpp_frame_init(&dst_mpp_frame) != MPP_OK) {
            av_log(ctx, AV_LOG_ERROR, "Init dst mpp frame failed.\n");
            ret = FAILURE;
            goto exit2;
        }
        out = oframe;
        output_frame_size = get_pic_buf_info(esctx->out_sw_fmt,
                                             out->width,
                                             out->height,
                                             get_alignment_by_format(esctx->out_sw_fmt),
                                             2,
                                             stride,
                                             offset,
                                             &plane);
        av_log(ctx,
               AV_LOG_DEBUG,
               "out info size:%ld plane:%d stride:%d-%d-%d, offset:%d-%d-%d.\n",
               output_frame_size,
               plane,
               stride[0],
               stride[1],
               stride[2],
               offset[0],
               offset[1],
               offset[2]);

        if (esctx->out_hw_frame_ctx && oframe->buf[0]) {
            dst_mpp_buf = (MppBufferPtr)oframe->buf[0]->data;
            if (!dst_mpp_buf) {
                av_log(ctx, AV_LOG_ERROR, "dst_mpp_buf is NULL\n");
                goto exit2;
            }
            mpp_buffer_inc_ref(dst_mpp_buf);
        } else {
            mpp_ret = mpp_buffer_get(esctx->buf_grp, &dst_mpp_buf, output_frame_size);
            if (mpp_ret) {
                av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", output_frame_size, mpp_ret);
                ret = FAILURE;
                goto exit3;
            }
        }
        esmpp_stack_set_mpp_frame(dst_mpp_frame, out, 0, -1, stride, offset, esctx->out_sw_fmt);
        mpp_frame_set_buffer(dst_mpp_frame, dst_mpp_buf);
        mpp_frame_set_buf_size(dst_mpp_frame, output_frame_size);

        if (esctx->out_hw_frame_ctx) {
            esmpp_buffer_export_frame(out, dst_mpp_frame, plane, offset, stride);
        } else {
            buf = av_buffer_create(mpp_buffer_get_ptr(dst_mpp_buf),
                                   sizeof(dst_mpp_buf),
                                   esmpp_free_frame_buf,
                                   dst_mpp_buf,
                                   AV_BUFFER_FLAG_READONLY);
            if (!buf) {
                goto exit3;
            }
            out->buf[0] = buf;
            for (int i = 0; i < plane; i++) {
                out->linesize[i] = stride[i];
                out->data[i] = out->buf[0]->data + offset[i];
            }
        }
    }

#if ESMPP_STACK_DUMP
    for (int i = 0; i < input_nb; i++) {
        write_buffer_to_file(mpp_buffer_get_ptr(src_mpp_buf[i]),
                             in_frame_size[i],
                             NULL,
                             in_src[i]->width,
                             in_src[i]->height,
                             esctx->in_sw_fmt[i],
                             i);
    }
#endif

#if ESMPP_STACK_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, esctx->out_sw_fmt, 1);
#endif

    if (inParams->fillcolor_enable) {
        TdeRectInfo rect_info;
        rect_info.rect.x = 0;
        rect_info.rect.y = 0;
        rect_info.rect.width = esctx->output_width;
        rect_info.rect.height = esctx->output_height;
        rect_info.color = inParams->background_color;
        rect_info.thickness = 0;

        mpp_ret = es_tde_fill_rect_array(dst_mpp_frame, &rect_info, 1);
        if (mpp_ret != MPP_OK) {
            ret = FAILURE;
            av_log(ctx, AV_LOG_ERROR, "es_tde_fill_rect_array failed, ret: %d\n", mpp_ret);
            goto exit3;
        }
    }

    mpp_ret = es_tde_multisource(src_mpp_frame, input_nb, inParams->src_rects, inParams->dst_rects, dst_mpp_frame);
    if (mpp_ret != MPP_OK) {
        ret = FAILURE;
        av_log(ctx, AV_LOG_ERROR, "es_tde_multisource failed, ret: %d\n", mpp_ret);
        goto exit3;
    }

    ret = SUCCESS;

#if ESMPP_STACK_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, out->format, 2);
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
        for (int i = 0; i < input_nb; i++) mpp_buffer_put(src_mpp_buf[i]);
    }
    if (dst_mpp_frame) {
        mpp_frame_deinit(&dst_mpp_frame);
    }
exit1:
    if (src_mpp_frame) {
        for (int i = 0; i < input_nb; i++) mpp_frame_deinit(&src_mpp_frame[i]);
    }
    av_freep(src_mpp_frame);
    av_freep(&src_mpp_buf);
    av_freep(&in_frame_size);

    return ret;
}

static int process_frame(FFFrameSync *fs) {
    AVFilterContext *avctx = fs->parent;
    AVFilterLink *outlink = avctx->outputs[0];
    StackESMPPContext *sctx = fs->opaque;
    ESMPPContext *esctx = fs->opaque;
    MppInputParams iparams = {0};
    AVFrame *oframe, *iframe;
    int ret = 0;

    if (esctx->out_hw_frame_ref) {
        oframe = av_frame_alloc();
        if (!oframe) {
            return AVERROR(ENOMEM);
        }
        oframe->width = outlink->w;
        oframe->height = outlink->h;
        oframe->format = AV_PIX_FMT_DRM_PRIME;
        oframe->hw_frames_ctx = av_buffer_ref(esctx->out_hw_frame_ref);
    } else {
        oframe = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!oframe) return AVERROR(ENOMEM);
    }

    iparams.src_rects = av_calloc(avctx->nb_inputs, sizeof(RECT_S));
    if (!iparams.src_rects) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    iparams.in = av_calloc(avctx->nb_inputs, sizeof(AVFrame *));
    if (!iparams.in) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    for (int i = 0; i < avctx->nb_inputs; i++) {
        ret = ff_framesync_get_frame(fs, i, &iframe, 0);
        if (ret) goto fail;

        if (i == 0) {
            ret = av_frame_copy_props(oframe, iframe);
            if (ret < 0) goto fail;
        }

        iparams.src_rects[i].x = 0;
        iparams.src_rects[i].y = 0;
        iparams.src_rects[i].width = iframe->width;
        iparams.src_rects[i].height = iframe->height;

        iparams.dst_rects = sctx->rects;
        iparams.in[i] = iframe;
        av_log(avctx,
               AV_LOG_DEBUG,
               "stack input %d: %s, %ux%u (%" PRId64 ") (ret:%d,%d,%d,%d).\n",
               i,
               av_get_pix_fmt_name(iframe->format),
               iframe->width,
               iframe->height,
               iframe->pts,
               iparams.src_rects[i].x,
               iparams.src_rects[i].y,
               iparams.src_rects[i].width,
               iparams.src_rects[i].height);
    }

    if (sctx->base.fillcolor_enable) {
        iparams.fillcolor_enable = 1;
        iparams.background_color = (sctx->base.fillcolor[3] << 24 | sctx->base.fillcolor[0] << 16
                                    | sctx->base.fillcolor[1] << 8 | sctx->base.fillcolor[2]);
    } else if (sctx->base.mode = STACK_X) {
        iparams.fillcolor_enable = 1;
        iparams.background_color = 0x00000000;
    }
    av_log(avctx, AV_LOG_DEBUG, "background_color: 0x%08x.\n", iparams.background_color);

    oframe->pts = av_rescale_q(sctx->base.fs.pts, sctx->base.fs.time_base, outlink->time_base);
    oframe->sample_aspect_ratio = outlink->sample_aspect_ratio;

    av_log(avctx,
           AV_LOG_DEBUG,
           "stack output : %s, %ux%u (%" PRId64 ").\n",
           av_get_pix_fmt_name(oframe->format),
           oframe->width,
           oframe->height,
           oframe->pts);

    ret = esmpp_stack_filter_frame(avctx->nb_inputs, avctx, &iparams, oframe);
    if (ret) {
        av_log(avctx, AV_LOG_INFO, "esmpp_stack_filter_frame failed.\n");
        goto fail;
    }

    ret = ff_filter_frame(outlink, oframe);

fail:
    av_freep(&iparams.src_rects);
    av_freep(&iparams.in);

    return ret;
}

static int config_output(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    StackESMPPContext *sctx = ctx->priv;
    ESMPPContext *esctx = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];
    int ret;

    AVESMPPFramesContext *hw_device_ctx = NULL;
    AVHWFramesContext *in_hw_frames_ctx = NULL;
    AVHWFramesContext *out_hw_frames_ctx = NULL;

    esctx->format = inlink0->format;
    for (int i = 0; i < sctx->base.nb_inputs; i++) {
        AVFilterLink *inlink = ctx->inputs[i];
        if (inlink0->format == AV_PIX_FMT_DRM_PRIME && inlink0->hw_frames_ctx) {
            if (inlink0->format != inlink->format) {
                av_log(ctx, AV_LOG_ERROR, "Mixing hardware and software pixel formats is not supported.\n");
                return AVERROR(EINVAL);
            }
            esctx->in_sw_fmt[i] = ((AVHWFramesContext *)inlink->hw_frames_ctx->data)->sw_format;
        } else {
            esctx->in_sw_fmt[i] = inlink->format;
        }
    }

    ret = config_comm_output(outlink);
    if (ret < 0) return ret;

    if (inlink0->format == AV_PIX_FMT_DRM_PRIME && inlink0->hw_frames_ctx) {
        in_hw_frames_ctx = (AVHWFramesContext *)inlink0->hw_frames_ctx->data;
        hw_device_ctx = in_hw_frames_ctx->device_ctx->hwctx;

        av_log(ctx, AV_LOG_DEBUG, "config_output hw mode!\n");

        esctx->in_hw_frame_ref = av_buffer_ref(inlink0->hw_frames_ctx);
        esctx->hw_device_ref = av_buffer_ref(in_hw_frames_ctx->device_ref);

        esctx->hw_dev_ctx = hw_device_ctx;
        esctx->in_hw_frame_ctx = in_hw_frames_ctx;

        av_buffer_unref(&esctx->out_hw_frame_ref);
        esctx->out_hw_frame_ref = av_hwframe_ctx_alloc(esctx->hw_device_ref);
        if (!esctx->out_hw_frame_ref) {
            av_log(ctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed \n");
            if (!esctx->in_hw_frame_ref) av_buffer_unref(&esctx->in_hw_frame_ref);
            if (!esctx->out_hw_frame_ref) av_buffer_unref(&esctx->out_hw_frame_ref);
            if (!esctx->hw_device_ref) av_buffer_unref(&esctx->hw_device_ref);
            return AVERROR(EINVAL);
        }
        out_hw_frames_ctx = (AVHWFramesContext *)esctx->out_hw_frame_ref->data;
        esctx->out_hw_frame_ctx = out_hw_frames_ctx;
        out_hw_frames_ctx->format = AV_PIX_FMT_DRM_PRIME;
        out_hw_frames_ctx->sw_format = esctx->in_sw_fmt[0];
        out_hw_frames_ctx->width = outlink->w;
        out_hw_frames_ctx->height = outlink->h;
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(esctx->out_hw_frame_ref);
        outlink->format = inlink0->format;
        esctx->out_sw_fmt = esctx->in_sw_fmt[0];
    } else {
        esctx->out_sw_fmt = inlink0->format;
        outlink->format = esctx->out_sw_fmt;
        adjust_width_height_by_format(esctx->out_sw_fmt, &outlink->w, &outlink->h);
    }

    for (int i = 0; i < sctx->base.nb_inputs; i++) {
        sctx->rects[i].x = sctx->base.regions[i].x;
        sctx->rects[i].y = sctx->base.regions[i].y;
        sctx->rects[i].width = sctx->base.regions[i].width;
        sctx->rects[i].height = sctx->base.regions[i].height;
        av_log(ctx,
               AV_LOG_DEBUG,
               "stack dst rect[%d]: %d,%d,%d,%d.\n",
               i,
               sctx->rects[i].x,
               sctx->rects[i].y,
               sctx->rects[i].width,
               sctx->rects[i].height);
    }

    esctx->output_width = outlink->w;
    esctx->output_height = outlink->h;

    av_log(ctx, AV_LOG_DEBUG, "config_output outlink->w: %d, outlink->h: %d.\n", outlink->w, outlink->h);

    return ret;
}

static av_cold int esmpp_stack_init(AVFilterContext *ctx) {
    int ret = 0;
    StackESMPPContext *s = NULL;
    ESMPPContext *esctx = NULL;

    if (!ctx || !ctx->priv) {
        return FAILURE;
    }
    s = (StackESMPPContext *)ctx->priv;
    esctx = (ESMPPContext *)ctx->priv;

    esctx->hw_device_ref = NULL;
    esctx->in_hw_frame_ref = NULL;
    esctx->out_hw_frame_ref = NULL;
    esctx->hw_dev_ctx = NULL;
    esctx->in_hw_frame_ctx = NULL;
    esctx->out_hw_frame_ctx = NULL;

    ret = stack_init(ctx);
    if (ret) return ret;

    s->rects = av_calloc(s->base.nb_inputs, sizeof(RECT_S));
    esctx->in_sw_fmt = av_calloc(s->base.nb_inputs, sizeof(enum AVPixelFormat));
    if (!s->rects || !esctx->in_sw_fmt) {
        av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init alloc memory failed.\n");
        goto error;
    }
    esctx->nb_inputs = s->nb_inputs = s->base.nb_inputs;
    av_log(ctx, AV_LOG_DEBUG, "esmpp_stack_init, nb_inputs: %d.\n", esctx->nb_inputs);
    ret = mpp_buffer_group_get_internal(&esctx->buf_grp, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "Create buffer group with type %d failed: %d.\n", MPP_BUFFER_TYPE_DMA_HEAP, ret);
        goto error;
    }
    ret = mpp_buffer_group_limit_config(esctx->buf_grp, 0, 0);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "Limit buffer group with no limit failed: %d.\n", ret);
        goto error;
    }

    return SUCCESS;

error:
    av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init error, free memory.\n");
    if (s->rects) av_freep(&s->rects);
    if (esctx->in_sw_fmt) av_freep(&esctx->in_sw_fmt);

    return AVERROR(ENOMEM);
}

static av_cold void esmpp_stack_uninit(AVFilterContext *ctx) {
    ESMPPContext *esctx = NULL;
    StackESMPPContext *s = NULL;

    if (!ctx || !ctx->priv) {
        return;
    }

    esctx = (ESMPPContext *)ctx->priv;
    s = (StackESMPPContext *)ctx->priv;

    stack_uninit(ctx);
    if (esctx->in_hw_frame_ref) {
        av_buffer_unref(&esctx->in_hw_frame_ref);
    }
    if (esctx->out_hw_frame_ref) {
        av_buffer_unref(&esctx->out_hw_frame_ref);
    }
    if (esctx->hw_device_ref) {
        av_buffer_unref(&esctx->hw_device_ref);
    }

    if (esctx->buf_grp) {
        mpp_buffer_group_put(esctx->buf_grp);
        esctx->buf_grp = NULL;
    }
    if (s->rects) av_freep(&s->rects);
    if (esctx->in_sw_fmt) av_freep(&esctx->in_sw_fmt);
}

static int esmpp_stack_query_formats(AVFilterContext *ctx) {
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

#include "stack_internal.c"

#if CONFIG_HSTACK_ESMPP_FILTER

DEFINE_HSTACK_OPTIONS(esmpp);
DEFINE_STACK_FILTER(hstack, esmpp, "ESMPP Multisource", AVFILTER_FLAG_HWDEVICE);

#endif

#if CONFIG_VSTACK_ESMPP_FILTER

DEFINE_VSTACK_OPTIONS(esmpp);
DEFINE_STACK_FILTER(vstack, esmpp, "ESMPP Multisource", AVFILTER_FLAG_HWDEVICE);

#endif

#if CONFIG_XSTACK_ESMPP_FILTER

DEFINE_XSTACK_OPTIONS(esmpp);
DEFINE_STACK_FILTER(xstack, esmpp, "ESMPP Multisource", AVFILTER_FLAG_HWDEVICE);

#endif
