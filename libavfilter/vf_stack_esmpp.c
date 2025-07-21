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
#include "libavutil/time.h"

#include "internal.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

#include "framesync.h"
#include "es_mpp.h"
#include "mpp_err.h"
#include "mpp_tde_api.h"
#include "mpp_buffer.h"
#include "mpp_tde_cfg.h"
#include "vf_esmpp_common.h"
#include "libavutil/esmpp_transfer.h"

#define OFFSET(x) offsetof(StackHWContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
#define ES_SET_OUTPUT_REGION(region, rx, ry, rw, rh, layer) \
    do {                                                    \
        region->x = rx;                                     \
        region->y = ry;                                     \
        region->width = rw;                                 \
        region->height = rh;                                \
        region->layer = layer;                              \
    } while (0)

typedef struct _ESMPPContext {
    MppBufferGroupPtr buf_grp;

    AVBufferRef *out_hw_frame_ref;
    AVBufferRef *out_hw_device_ref;

    int nb_inputs;

    enum AVPixelFormat *in_sw_fmt;
    enum AVPixelFormat out_sw_fmt;
    enum AVPixelFormat format;
    int output_width;
    int output_height;

    int die_idx;
    int device_idx;
    ES_BOOL mpp_init;
    MppCtxPtr mctx;
} ESMPPContext;

#define ESMPP_STACK_DUMP (0)
#define HWContext ESMPPContext
#define StackHWContext StackESMPPContext

typedef struct ESRegion {
    int x;
    int y;
    int width;
    int height;
    int layer;
} ESRegion;

enum { STACK_H = 0, STACK_V = 1, STACK_X = 2 };

static const AVFilterPad complex_snapshot_output;

typedef struct StackESMPPContext {
    HWContext hwctx; /**< must be the first field */

    FFFrameSync fs;
    ES_BOOL fs_init;
    int mode;
    uint8_t fillcolor[4];
    int fillcolor_enable;
    ESRegion *regions;

    /* Options */
    int nb_inputs;  // stack element number
    int shortest;
    int tile_width;
    int tile_height;
    int nb_grid_columns;
    int nb_grid_rows;
    char *layout;
    char *fillcolor_str;
    int o_width;
    int o_height;
    AVFrame **in;

    int enable_logo;
    int logo_x_dir;
    int logo_y_dir;
    int logo_x;
    int logo_y;
    RECT_S logo_disp_rect;
    int rand_logo_duration;
    int rand_logo_frames;
    int rand_logo_count;
    int rand_logo_sent;
    int rand_logo_pos;
    int32_t snap_interval;
    int32_t out_cnt;
    int32_t snap_cnt;
#ifdef TRANSCODE_INFO_REPORT
    TdeReportInfo report_info;
    ES_BOOL process_started;
    ES_S32 in_buffer_num;
    ES_S32 out_buffer_num;
    ES_S32 fail_buffer_num;
    ES_S32 snapshot_cnt;
#endif
} StackESMPPContext;

typedef struct MppInputParams {
    AVFrame **in;
    RECT_S *src_rects;
    RECT_S *dst_rects;
    uint32_t background_color;
    int fillcolor_enable;
} MppInputParams;

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

static int esmpp_stack_process_logo(AVFilterContext *avctx,
                                    AVFilterLink *logo_inlink,
                                    AVFrame *logo_frame,
                                    MppFramePtr dst_mpp_frame) {
    StackESMPPContext *s = avctx->priv;
    ESMPPContext *esctx = avctx->priv;
    MppFramePtr logo_mpp_frame;
    MppBufferPtr logo_mpp_buf = NULL;
    int stride[4] = {0}, offset[4] = {0};
    uint32_t pic_size = 0;
    enum AVPixelFormat logo_fmt;
    RECT_S logo_rect = {0};
    RECT_S dst_rect = {0};

    MPP_RET mpp_ret = MPP_OK;
    int ret = SUCCESS;

    if (!s->enable_logo) {
        return ret;
    }

    if (!logo_inlink || (logo_inlink->format == AV_PIX_FMT_DRM_PRIME && !logo_inlink->hw_frames_ctx)) {
        av_log(avctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        return FAILURE;
    }

    if (mpp_frame_init(&logo_mpp_frame) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Init logo mpp frame failed.\n");
        return FAILURE;
    }

    logo_fmt = (logo_frame->format == AV_PIX_FMT_DRM_PRIME)
                   ? ((AVHWFramesContext *)logo_inlink->hw_frames_ctx->data)->sw_format
                   : logo_frame->format;
    esmpp_stack_get_offset_stride(logo_frame, logo_fmt, stride, offset, &pic_size);
    esmpp_stack_set_mpp_frame(logo_mpp_frame, logo_frame, 0, -1, stride, offset, logo_fmt);

    if (logo_inlink->format == AV_PIX_FMT_DRM_PRIME) {
        if (logo_frame->buf[0]) {
            logo_mpp_buf = (MppBufferPtr)logo_frame->buf[0]->data;
            if (!logo_mpp_buf) {
                av_log(avctx, AV_LOG_ERROR, "logo_mpp_buf is NULL\n");
                ret = FAILURE;
                goto exit;
            }
            mpp_buffer_inc_ref(logo_mpp_buf);
        } else {
            av_log(avctx, AV_LOG_WARNING, "frame buf is NULL\n");
            ret = FAILURE;
            goto exit;
        }
        mpp_frame_set_buffer(logo_mpp_frame, logo_mpp_buf);
    } else {
        mpp_ret = mpp_buffer_get(esctx->buf_grp, &logo_mpp_buf, pic_size);
        if (mpp_ret) {
            av_log(
                avctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", logo_frame->buf[0]->size, mpp_ret);
            ret = FAILURE;
            goto exit;
        }
        esmpp_memcpy_host2device(logo_fmt, logo_frame, mpp_buffer_get_ptr(logo_mpp_buf));
        mpp_frame_set_buffer(logo_mpp_frame, logo_mpp_buf);
        mpp_frame_set_buf_size(logo_mpp_frame, pic_size);
    }

    logo_rect.width = logo_frame->width;
    logo_rect.height = logo_frame->height;
    dst_rect.width = mpp_frame_get_width(dst_mpp_frame);
    dst_rect.height = mpp_frame_get_height(dst_mpp_frame);

    if (s->rand_logo_duration > 0) {  // random logo
        s->rand_logo_count--;
        if (s->rand_logo_count <= 0) {
            srand(av_gettime());
            s->rand_logo_count = s->rand_logo_frames;
            s->rand_logo_sent = 0;
            s->rand_logo_pos = rand() & 0x3;
            av_log(avctx,
                   AV_LOG_TRACE,
                   "vf[esmpp_stack] rand_logo_count:%d, rand_logo_pos:%d.\n",
                   s->rand_logo_count,
                   s->rand_logo_pos);
        }
        if (!s->rand_logo_sent) {  // set display rect when change
            s->rand_logo_sent = 1;
            switch (s->rand_logo_pos) {
                default:
                case 0:  // left top
                    s->logo_disp_rect.x = (s->logo_x) & ~0x1;
                    s->logo_disp_rect.y = (s->logo_y) & ~0x1;
                    break;
                case 1:  // right top
                    s->logo_disp_rect.x = (dst_rect.width - s->logo_x - logo_rect.width) & ~0x1;
                    s->logo_disp_rect.y = (s->logo_y) & ~0x1;
                    break;
                case 2:  // left bottom
                    s->logo_disp_rect.x = (s->logo_x) & ~0x1;
                    s->logo_disp_rect.y = (dst_rect.height - s->logo_y - logo_rect.height) & ~0x1;
                    break;
                case 3:  // right bottom
                    s->logo_disp_rect.x = (dst_rect.width - s->logo_x - logo_rect.width) & ~0x1;
                    s->logo_disp_rect.y = (dst_rect.height - s->logo_y - logo_rect.height) & ~0x1;
                    break;
            }
            s->logo_disp_rect.width = logo_rect.width;
            s->logo_disp_rect.height = logo_rect.height;
            av_log(avctx,
                   AV_LOG_TRACE,
                   "vf[esmpp_stack] logo display rect x:%d, y:%d, w:%d, h:%d, rand_logo_frames:%d.\n",
                   s->logo_disp_rect.x,
                   s->logo_disp_rect.y,
                   s->logo_disp_rect.width,
                   s->logo_disp_rect.height,
                   s->rand_logo_frames);
        }
    } else if (s->logo_disp_rect.width == 0 || s->logo_disp_rect.height == 0) {  // set display rect once if no duration
        int logo_x = s->logo_x, logo_y = s->logo_y;
        if (s->logo_x_dir) {
            logo_x = dst_rect.width - logo_x - logo_rect.width;
        }
        if (s->logo_y_dir) {
            logo_y = dst_rect.height - logo_y - logo_rect.height;
        }
        if (logo_x < 0) {
            logo_x = 0;
        }
        if (logo_x >= dst_rect.width - logo_rect.width) {
            logo_x = dst_rect.width - logo_rect.width;
        }

        if (logo_y < 0) {
            logo_y = 0;
        }
        if (logo_y >= dst_rect.height - logo_rect.height) {
            logo_y = dst_rect.height - logo_rect.height;
        }

        s->logo_disp_rect.x = logo_x;
        s->logo_disp_rect.y = logo_y;
        s->logo_disp_rect.width = logo_rect.width;
        s->logo_disp_rect.height = logo_rect.height;
        av_log(avctx,
               AV_LOG_TRACE,
               "vf[esmpp_stack] logo display rect x:%d, y:%d, w:%d, h:%d.\n",
               s->logo_disp_rect.x,
               s->logo_disp_rect.y,
               s->logo_disp_rect.width,
               s->logo_disp_rect.height);
    }
    mpp_ret = esmpp_process_frame_sync(esctx->mctx,
                                       logo_mpp_frame,
                                       0,
                                       dst_mpp_frame,
                                       &logo_rect,
                                       ES_NULL,
                                       &s->logo_disp_rect,
                                       TDE_USAGE_BLEND_SRC_IN | TDE_USAGE_LOGO);

exit:
    if (logo_mpp_buf) {
        mpp_buffer_put(logo_mpp_buf);
    }
    if (logo_mpp_frame) {
        mpp_frame_deinit(&logo_mpp_frame);
    }

    return ret;
}

static int esmpp_stack_filter_frame(int input_nb, AVFilterContext *avctx, MppInputParams *inParams, AVFrame *oframe) {
    AVFrame **in_src = NULL;
    AVFilterContext *ctx = NULL;
    ESMPPContext *esctx = NULL;
    StackESMPPContext *es_stack_ctx = NULL;
    AVFrame *out = NULL;
    int ret = SUCCESS;
    MPP_RET mpp_ret = MPP_OK;
    MppFramePtr *src_mpp_frame = NULL;
    MppFramePtr dst_mpp_frame = NULL;
    MppBufferPtr *src_mpp_buf = NULL;
    MppBufferPtr dst_mpp_buf = NULL;
    ES_BOOL is_hw = ES_FALSE;
    size_t *in_frame_size = NULL, output_frame_size = 0;
    int plane = 0, stride[3] = {0}, offset[3] = {0};

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
    es_stack_ctx = ctx->priv;

    if (esctx->format == AV_PIX_FMT_DRM_PRIME && !in_src[0]->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        ret = FAILURE;
        goto exit2;
    }
    src_mpp_frame = av_calloc(esctx->nb_inputs, sizeof(MppFramePtr));
    if (!src_mpp_frame) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }
    src_mpp_buf = av_calloc(esctx->nb_inputs, sizeof(MppBufferPtr));
    if (!src_mpp_buf) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }
    in_frame_size = av_calloc(esctx->nb_inputs, sizeof(size_t));
    if (!in_frame_size) {
        ret = AVERROR(ENOMEM);
        goto exit2;
    }

    // handle src mpp frame
    for (ES_S32 i = 0; i < esctx->nb_inputs; i++) {
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
        if (mpp_frame_init(&dst_mpp_frame) != MPP_OK) {
            av_log(ctx, AV_LOG_ERROR, "Init dst mpp frame failed.\n");
            ret = FAILURE;
            goto exit2;
        }
#ifdef TRANSCODE_HOST
        if (oframe->format != AV_PIX_FMT_DRM_PRIME) {
            out = av_frame_alloc();
            out->width = oframe->width;
            out->height = oframe->height;
            out->format = oframe->format;
            av_frame_copy_props(out, oframe);
        } else {
            out = oframe;
        }
#else
        out = oframe;
#endif
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

        if (esctx->out_hw_frame_ref && out->buf[0]) {
            dst_mpp_buf = (MppBufferPtr)out->buf[0]->data;
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

        if (esctx->out_hw_frame_ref) {
            esmpp_buffer_export_frame(out, dst_mpp_frame, plane, offset, stride);
        } else {
            AVBufferRef *buf;
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

#ifdef TRANSCODE_INFO_REPORT
    mpp_frame_set_report_opaque(dst_mpp_frame, (ES_S64)&es_stack_ctx->report_info);
#endif

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

        mpp_ret = esmpp_process_frame_sync(
            esctx->mctx, ES_NULL, 1, dst_mpp_frame, ES_NULL, &rect_info, ES_NULL, TDE_USAGE_COLOR_FILL);
        if (mpp_ret != MPP_OK) {
            ret = FAILURE;
            av_log(ctx, AV_LOG_ERROR, "es_tde_fill_rect_array failed, ret: %d\n", mpp_ret);
            goto exit3;
        }
    }
#ifdef TRANSCODE_INFO_REPORT
    es_stack_ctx->report_info.bReported = ES_TRUE;
#endif
    mpp_ret = esmpp_process_frame_sync(esctx->mctx,
                                       src_mpp_frame,
                                       esctx->nb_inputs,
                                       dst_mpp_frame,
                                       inParams->src_rects,
                                       inParams->dst_rects,
                                       ES_NULL,
                                       TDE_USAGE_MULTISOURCE);
    if (mpp_ret != MPP_OK) {
        ret = FAILURE;
        av_log(ctx, AV_LOG_ERROR, "es_tde_multisource failed, ret: %d\n", mpp_ret);
        goto exit3;
    }

    ret = esmpp_stack_process_logo(
        avctx, ctx->inputs[esctx->nb_inputs], es_stack_ctx->in[esctx->nb_inputs], dst_mpp_frame);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "process logo failed ret:%d\n", ret);
        goto exit3;
    }
#ifdef TRANSCODE_HOST
    if (oframe->format != AV_PIX_FMT_DRM_PRIME) {
        AVBufferRef *buf;
        buf = av_buffer_alloc(output_frame_size);
        oframe->buf[0] = buf;
        for (int i = 0; i < plane; i++) {
            oframe->linesize[i] = stride[i];
            oframe->data[i] = oframe->buf[0]->data + offset[i];
        }
        esmpp_hwframe_transfer_bysize(oframe, out, output_frame_size);
    }
#else
#if ESMPP_STACK_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, out->format, 2);
#endif
#endif
    goto exit2;

exit3:
    if (dst_mpp_buf) {
        mpp_buffer_put(dst_mpp_buf);
    }
exit2:
#ifdef TRANSCODE_HOST
    if (oframe->format != AV_PIX_FMT_DRM_PRIME) {
        av_frame_free(&out);
    }
#endif
    if (src_mpp_buf) {
        for (int i = 0; i < esctx->nb_inputs; i++) mpp_buffer_put(src_mpp_buf[i]);
    }
    if (dst_mpp_frame) {
        mpp_frame_deinit(&dst_mpp_frame);
    }
exit1:
    if (src_mpp_frame) {
        for (int i = 0; i < esctx->nb_inputs; i++) mpp_frame_deinit(&src_mpp_frame[i]);
    }
    av_freep(&src_mpp_frame);
    av_freep(&src_mpp_buf);
    av_freep(&in_frame_size);

    return ret;
}

static int esmpp_sort_frame_by_layer(AVFilterContext *avctx, RECT_S *src_rects, RECT_S *dst_rects, AVFrame **in) {
    StackESMPPContext *sctx = avctx->priv;
    int index = 0;
    int layer = 0;
    int next_layer = sctx->regions[0].layer;
    for (int i = 0; i < sctx->nb_inputs; i++) {
        next_layer = next_layer > sctx->regions[i].layer ? sctx->regions[0].layer : next_layer;
    }
    av_log(avctx, AV_LOG_DEBUG, "esmpp_sort_frame_by_layer enter.\n");

    while (index < sctx->nb_inputs) {
        layer = next_layer;
        for (int i = 0; i < sctx->nb_inputs; i++) {
            if (index >= sctx->nb_inputs) break;

            if (layer == sctx->regions[i].layer) {
                dst_rects[index].x = sctx->regions[i].x;
                dst_rects[index].y = sctx->regions[i].y;
                dst_rects[index].width = sctx->regions[i].width;
                dst_rects[index].height = sctx->regions[i].height;
                src_rects[index].x = 0;
                src_rects[index].y = 0;
                src_rects[index].width = sctx->in[i]->width;
                src_rects[index].height = sctx->in[i]->height;
                in[index] = sctx->in[i];
                index++;
                av_log(avctx, AV_LOG_DEBUG, "sort frame region[%d], layer: %d.\n", i, layer);
            } else if (layer < sctx->regions[i].layer && (next_layer > sctx->regions[i].layer || next_layer == layer)) {
                next_layer = sctx->regions[i].layer;
                av_log(avctx, AV_LOG_DEBUG, "set next layer: %d.\n", next_layer);
            }
        }
    }
    return 0;
}

static int esmpp_stack_send_snapshot_frame(AVFilterLink *outlink, AVFrame *frame, AVFilterContext *avctx) {
    StackESMPPContext *s = (StackESMPPContext *)avctx->priv;
    int ret = FAILURE;

    if (outlink && frame) {
        ret = ff_filter_frame(outlink, frame);
        if (ret >= 0) {
            s->snap_cnt++;
            av_log(avctx, AV_LOG_DEBUG, "stack snap send success.out cnt:%d, snap cnt:%d\n", s->out_cnt, s->snap_cnt);
        } else {
            av_log(avctx,
                   AV_LOG_ERROR,
                   "stack snap send failed. ret:%d, out cnt:%d, snap cnt:%d\n",
                   ret,
                   s->out_cnt,
                   s->snap_cnt);
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "stack snap send failed.\n");
    }

    return ret;
}

static int process_frame(FFFrameSync *fs) {
    AVFilterContext *avctx = fs->parent;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFilterLink *outlink_snap = NULL;
    StackESMPPContext *ctx = fs->opaque;
    ESMPPContext *esctx = fs->opaque;
    MppInputParams iparams = {0};
    AVFrame *oframe, *iframe;
    AVFrame *frame_snap = NULL;
    ES_BOOL bneed_snap = ES_FALSE;
    int ret = 0;
#ifdef TRANSCODE_INFO_REPORT
    ctx->report_info.inBufferNum = ++ctx->in_buffer_num;
#endif

    oframe = av_frame_alloc();
    if (!oframe) {
        return AVERROR(ENOMEM);
    }
    oframe->width = outlink->w;
    oframe->height = outlink->h;

    if (esctx->out_hw_frame_ref) {
        oframe->format = AV_PIX_FMT_DRM_PRIME;
        oframe->hw_frames_ctx = av_buffer_ref(esctx->out_hw_frame_ref);
    } else {
        oframe->format = outlink->format;
    }

    iparams.src_rects = av_calloc(avctx->nb_inputs, sizeof(RECT_S));
    if (!iparams.src_rects) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    iparams.dst_rects = av_calloc(avctx->nb_inputs, sizeof(RECT_S));
    if (!iparams.dst_rects) {
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

        ctx->in[i] = iframe;
        av_log(avctx,
               AV_LOG_DEBUG,
               "stack input %d: %s, %ux%u (%" PRId64 ") (ret:%d,%d,%d,%d).\n",
               i,
               av_get_pix_fmt_name(iframe->format),
               iframe->width,
               iframe->height,
               iframe->pts,
               ctx->regions[i].x,
               ctx->regions[i].y,
               ctx->regions[i].width,
               ctx->regions[i].height);
    }

    esmpp_sort_frame_by_layer(avctx, iparams.src_rects, iparams.dst_rects, iparams.in);

#ifdef TRANSCODE_INFO_REPORT
    AVFilterLink *inlink = avctx->inputs[0];
    ctx->report_info.bReported = ctx->process_started;
    if (!ctx->process_started) {
        ctx->report_info.beginMediaTs = iparams.in[0]->pts;
        ctx->process_started = ES_TRUE;
    }
    ctx->report_info.curDateMs = get_ticks_by_ms();
    ctx->report_info.AliveDurationMs = ctx->report_info.curDateMs - ctx->report_info.beginDateMs;
    ctx->report_info.curMediaTs = iparams.in[0]->pts;
    ctx->report_info.inFrameRate = av_q2d(inlink->frame_rate);
    ctx->report_info.outFrameRate = av_q2d(outlink->frame_rate);
#endif
    if (ctx->fillcolor_enable) {
        iparams.fillcolor_enable = 1;
        iparams.background_color =
            (ctx->fillcolor[3] << 24 | ctx->fillcolor[0] << 16 | ctx->fillcolor[1] << 8 | ctx->fillcolor[2]);
    } else if (ctx->mode = STACK_X) {
        iparams.fillcolor_enable = 1;
        iparams.background_color = 0x00000000;
    }
    av_log(avctx, AV_LOG_DEBUG, "background_color: 0x%08x.\n", iparams.background_color);

    oframe->pts = av_rescale_q(ctx->fs.pts, ctx->fs.time_base, outlink->time_base);
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
        av_log(avctx, AV_LOG_ERROR, "esmpp_stack_filter_frame failed.\n");
        goto fail;
    }

    ret = ff_filter_frame(outlink, oframe);
    if (ret >= 0) {
        ctx->out_cnt++;
        av_log(avctx, AV_LOG_DEBUG, "filter frame send success. statistic cnt:%d\n", ctx->out_cnt);
    } else {
        av_log(avctx, AV_LOG_ERROR, "filter frame send failed in frame num:%d ret:%d\n", ctx->out_cnt, ret);
    }

    bneed_snap = ctx->snap_interval ? 0 == ctx->out_cnt % ctx->snap_interval : ES_FALSE;
    if (bneed_snap) {
        if (avctx->nb_outputs > 1) {
            outlink_snap = avctx->outputs[1];
            frame_snap = av_frame_alloc();
            if (av_frame_ref(frame_snap, oframe) >= 0) {
                esmpp_stack_send_snapshot_frame(outlink_snap, frame_snap, avctx);
#ifdef TRANSCODE_INFO_REPORT
                ctx->report_info.snapshotCnt = ++ctx->snapshot_cnt;
                ctx->report_info.snapshotTotalBits = 0;
#endif
            } else {
                av_frame_free(&frame_snap);
                av_log(avctx, AV_LOG_ERROR, "snap av_frame_ref failed\n");
            }
        } else {
            av_log(avctx,
                   AV_LOG_ERROR,
                   "need snap but no snap shot outpad,snap_interval=%d, nb_outputs=%d\n",
                   ctx->snap_interval,
                   avctx->nb_outputs);
        }
    }

fail:
    if (iparams.src_rects) av_freep(&iparams.src_rects);
    if (iparams.dst_rects) av_freep(&iparams.dst_rects);
    if (iparams.in) av_freep(&iparams.in);
#ifdef TRANSCODE_INFO_REPORT
    if (ret) {
        ctx->report_info.failBufferNum = ++ctx->fail_buffer_num;
    } else {
        ctx->report_info.outBufferNum = ++ctx->out_buffer_num;
    }
#endif
    return ret;
}

static int esmpp_init_framesync(AVFilterContext *avctx) {
    StackESMPPContext *sctx = avctx->priv;
    int ret;

    if (sctx->fs_init) return 0;

    ret = ff_framesync_init(&sctx->fs, avctx, avctx->nb_inputs);
    if (ret < 0) return ret;

    sctx->fs.on_event = process_frame;
    sctx->fs.opaque = sctx;

    for (int i = 0; i < avctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &sctx->fs.in[i];

        in->before = EXT_STOP;
        in->after = sctx->shortest ? EXT_STOP : EXT_INFINITY;
        in->sync = 1;
        in->time_base = avctx->inputs[i]->time_base;
    }
    ret = ff_framesync_configure(&sctx->fs);
    if (ret != 0) return ret;

    sctx->fs_init = ES_TRUE;
    return ret;
}

static int esmpp_output_parse(AVFilterLink *outlink) {
    AVFilterContext *avctx = outlink->src;
    StackESMPPContext *sctx = avctx->priv;
    AVFilterLink *inlink0 = avctx->inputs[0];
    int width, height;
    int layer = 0;
    int ret;

    if (sctx->mode == STACK_H) {
        height = sctx->tile_height;
        width = 0;

        if (!height) height = inlink0->h;

        for (int i = 0; i < sctx->nb_inputs; i++) {
            AVFilterLink *inlink = avctx->inputs[i];
            ESRegion *region = &sctx->regions[i];

            ES_SET_OUTPUT_REGION(region, width, 0, av_rescale(height, inlink->w, inlink->h), height, layer);
            width += av_rescale(height, inlink->w, inlink->h);
        }
    } else if (sctx->mode == STACK_V) {
        height = 0;
        width = sctx->tile_width;

        if (!width) width = inlink0->w;

        for (int i = 0; i < sctx->nb_inputs; i++) {
            AVFilterLink *inlink = avctx->inputs[i];
            ESRegion *region = &sctx->regions[i];

            ES_SET_OUTPUT_REGION(region, 0, height, width, av_rescale(width, inlink->h, inlink->w), layer);
            height += av_rescale(width, inlink->h, inlink->w);
        }
    } else if (sctx->nb_grid_rows && sctx->nb_grid_columns) {
        int xpos = 0, ypos = 0;
        int ow, oh, k = 0;

        ow = sctx->tile_width;
        oh = sctx->tile_height;

        if (!ow || !oh) {
            ow = avctx->inputs[0]->w;
            oh = avctx->inputs[0]->h;
        }

        for (int i = 0; i < sctx->nb_grid_columns; i++) {
            ypos = 0;

            for (int j = 0; j < sctx->nb_grid_rows; j++) {
                ESRegion *region = &sctx->regions[k++];

                ES_SET_OUTPUT_REGION(region, xpos, ypos, ow, oh, layer);
                ypos += oh;
            }

            xpos += ow;
        }

        width = ow * sctx->nb_grid_columns;
        height = oh * sctx->nb_grid_rows;
    } else {
        int index = 0;
        char *arg, *tokenizer, *args, *layout_str = sctx->layout;
        char *arg0, *tokenizer0;

        if (sctx->o_width <= 0 && sctx->o_height <= 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid layout parameter, o_width and o_height must be set\n");
            return AVERROR(EINVAL);
        }

        args = layout_str;
        while ((arg = av_strtok(args, "|", &tokenizer))) {
            args = NULL;
            layer = 0;
            ESRegion *region = &sctx->regions[index];
            ES_S32 w = 0, h = 0, left = 0, top = 0;
            av_log(avctx, AV_LOG_DEBUG, "%d arg=%s tokenizer=%s\n", __LINE__, arg, tokenizer);
            while ((arg0 = av_strtok(arg, " ", &tokenizer0))) {
                arg = NULL;
                av_log(avctx, AV_LOG_DEBUG, "%d arg0=%s tokenizer0=%s\n", __LINE__, arg0, tokenizer0);
                if (sscanf(arg0, "layer=%d", &layer) != 1
                    && sscanf(arg0, "pos=%dx%d@%d.%d", &w, &h, &left, &top) != 4) {
                    av_log(avctx, AV_LOG_ERROR, "Invalid layout parameter\n");
                    return AVERROR(EINVAL);
                }
            }
            av_log(avctx, AV_LOG_DEBUG, "set layout parameter, left: %d, top: %d, w: %d, h: %d\n", left, top, w, h);
            ES_SET_OUTPUT_REGION(region, left, top, w, h, layer);
            index++;
        }

        if (index != sctx->nb_inputs) {
            av_log(avctx, AV_LOG_DEBUG, "Invalid layout numbers, layout: %d, inputs: %d\n", index, sctx->nb_inputs);
            return AVERROR(EINVAL);
        }
        width = sctx->o_width;
        height = sctx->o_height;
    }

    outlink->w = width;
    outlink->h = height;
    outlink->frame_rate = inlink0->frame_rate;
    outlink->sample_aspect_ratio = inlink0->sample_aspect_ratio;

    for (int i = 1; i < sctx->nb_inputs; i++) {
        AVFilterLink *inlink = avctx->inputs[i];
        if (outlink->frame_rate.num != inlink->frame_rate.num || outlink->frame_rate.den != inlink->frame_rate.den) {
            av_log(avctx, AV_LOG_VERBOSE, "Video inputs[%d] have different frame rates, output will be VFR\n", i);
            outlink->frame_rate = av_make_q(1, 0);
            break;
        }
    }

    if (sctx->enable_logo) {
        if (sctx->logo_x < 0) {
            sctx->logo_x = 0;
        }
        if (sctx->logo_y < 0) {
            sctx->logo_y = 0;
        }

        if (outlink->frame_rate.den != 0) {
            sctx->rand_logo_frames = sctx->rand_logo_duration * av_q2d(outlink->frame_rate) / 1000;
        } else {
            sctx->rand_logo_frames = sctx->rand_logo_duration * av_q2d(inlink0->frame_rate) / 1000;
        }
    }

    ret = esmpp_init_framesync(avctx);
    if (ret < 0) return ret;

    outlink->time_base = sctx->fs.time_base;

    return 0;
}

static int config_output_snap(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    StackESMPPContext *sctx = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];
    ES_DOUBLE in_frame_rate = 0.0;
    int ret = 0;
    av_log(ctx, AV_LOG_DEBUG, "config_output_snap enter.\n");

    outlink->w = sctx->o_width;
    outlink->h = sctx->o_height;
    outlink->frame_rate = inlink0->frame_rate;
    outlink->sample_aspect_ratio = inlink0->sample_aspect_ratio;

    in_frame_rate = av_q2d(inlink0->frame_rate);
    av_log(ctx, AV_LOG_DEBUG, "[out1] w:%d h:%d @%.2f\n", outlink->w, outlink->h, in_frame_rate / sctx->snap_interval);
    outlink->frame_rate = av_d2q(in_frame_rate / sctx->snap_interval, 1000);

    av_log(ctx, AV_LOG_DEBUG, "config_output snap outlink->w: %d, snap outlink->h: %d.\n", outlink->w, outlink->h);

    ret = esmpp_init_framesync(ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "config_output snap init_framesync failed.\n");
        return ret;
    }

    outlink->time_base = sctx->fs.time_base;

    av_log(ctx, AV_LOG_DEBUG, "config_output_snap exit.\n");

    return ret;
}

static int esmpp_init_output_bufgroup(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    ESMPPContext *esctx = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];

    int ret = 0;

    if (inlink0->hw_frames_ctx) {
        AVHWFramesContext *hwfc = NULL;
        AVESMPPFramesContext *mppfc;
        ret = init_hwcontext_buf(outlink,
                                 &esctx->out_hw_device_ref,
                                 &esctx->out_hw_frame_ref,
                                 esctx->out_sw_fmt,
                                 esctx->device_idx,
                                 esctx->die_idx);
        if (ret) {
            return ret;
        }

        hwfc = (AVHWFramesContext *)esctx->out_hw_frame_ref->data;
        mppfc = hwfc->hwctx;
        esctx->buf_grp = mppfc->buf_group;
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(esctx->out_hw_frame_ref);
    } else {
        ret = mpp_buffer_group_get_internal(&esctx->buf_grp, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "Create buffer group with type %d failed: %d.\n", MPP_BUFFER_TYPE_DMA_HEAP, ret);
            return FAILURE;
        }
        ret = mpp_buffer_group_limit_config(esctx->buf_grp, 0, 0);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "Limit buffer group with no limit failed: %d.\n", ret);
            return FAILURE;
        }
    }

    return ret;
}

#ifdef TRANSCODE_INFO_REPORT
static uint64_t esmpp_get_func_types(AVFilterContext *ctx) {
    StackESMPPContext *sctx = ctx->priv;

    TdeFuncType type = TDE_FUNC_TYPE_BUTT;

    do {
        if (sctx->enable_logo && av_pix_fmt_count_planes(sctx->hwctx.out_sw_fmt) != 1) {
            type = TDE_FUNC_TYPE_LOGO_YUV;
            break;
        }

        type = TDE_FUNC_TYPE_MULTISOURCE;
    } while (0);

    return type;
}
#endif

static int init_mpp_tde(AVFilterContext *ctx) {
    int ret = 0;
    StackESMPPContext *s = NULL;
    ESMPPContext *esctx = NULL;

    if (!ctx || !ctx->priv) {
        return FAILURE;
    }
    s = (StackESMPPContext *)ctx->priv;
    esctx = (ESMPPContext *)ctx->priv;

    if (esctx->mpp_init) {
        return MPP_OK;
    }
    if ((ret = esmpp_create(&esctx->mctx, MPP_CTX_TDE, MPP_VIDEO_CodingUnused)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    if ((ret = esmpp_select_dev(esctx->mctx, esctx->device_idx, esctx->die_idx)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to select MPP dev: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    if ((ret = esmpp_init(esctx->mctx)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto error;
    }

    MppTdeCfgPtr tde_cfg;
    if ((ret = mpp_tde_cfg_init(&tde_cfg)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp_tde_cfg_ret: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto error;
    } else {
        av_log(ctx, AV_LOG_INFO, "success to init mpp_tde_cfg_ret: %d\n", ret);
    }

    ret = esmpp_control(esctx->mctx, MPP_TDE_GET_CFG, tde_cfg);
    if (ret != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "get stack cfg failed ret: %d\n", ret);
        mpp_tde_cfg_deinit(&tde_cfg);
        goto error;
    } else {
        av_log(ctx, AV_LOG_INFO, "get stack cfg success ret: %d\n", ret);
    }
    mpp_tde_cfg_set_s32(tde_cfg, "moudle_mode", TDE_MODULE_STACK);

    ret = esmpp_control(esctx->mctx, MPP_TDE_SET_CFG, tde_cfg);
    if (ret != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "stack cfg failed\n");
    } else {
        av_log(ctx, AV_LOG_INFO, "stack cfg success\n");
    }
    mpp_tde_cfg_deinit(&tde_cfg);

    esmpp_open(esctx->mctx);
    esctx->mpp_init = ES_TRUE;
    return SUCCESS;
error:
    if (esctx->mctx) {
        esmpp_destroy(esctx->mctx);
    }

    return AVERROR(ENOMEM);
}

static int config_output(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    StackESMPPContext *sctx = ctx->priv;
    ESMPPContext *esctx = ctx->priv;
    AVFilterLink *inlink0 = ctx->inputs[0];
    int ret;

    av_log(ctx, AV_LOG_DEBUG, "config_output enter.\n");

    esctx->format = inlink0->format;
    for (int i = 0; i < sctx->nb_inputs; i++) {
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

    ret = esmpp_output_parse(outlink);
    if (ret < 0) return ret;

    if (inlink0->format == AV_PIX_FMT_DRM_PRIME && inlink0->hw_frames_ctx) {
        outlink->format = inlink0->format;
        esctx->out_sw_fmt = esctx->in_sw_fmt[0];
    } else {
        esctx->out_sw_fmt = inlink0->format;
        outlink->format = esctx->out_sw_fmt;
        adjust_width_height_by_format(esctx->out_sw_fmt, &outlink->w, &outlink->h);
    }

    get_dev_info(ctx->hw_device_ctx, inlink0->hw_frames_ctx, &esctx->device_idx, &esctx->die_idx);
    init_mpp_tde(ctx);

    esmpp_init_output_bufgroup(outlink);

    for (int i = 0; i < sctx->nb_inputs; i++) {
        av_log(ctx,
               AV_LOG_DEBUG,
               "stack dst rect[%d]: %d,%d,%d,%d.\n",
               i,
               sctx->regions[i].x,
               sctx->regions[i].y,
               sctx->regions[i].width,
               sctx->regions[i].height);
    }

    esctx->output_width = outlink->w;
    esctx->output_height = outlink->h;
#ifdef TRANSCODE_INFO_REPORT
    sctx->report_info.inWidth = sctx->regions[0].width;
    sctx->report_info.inHeight = sctx->regions[0].height;
    sctx->report_info.outWidth = esctx->output_width;
    sctx->report_info.outHeight = esctx->output_height;
    sctx->report_info.timeBaseNum = sctx->fs.time_base.num;
    sctx->report_info.timeBaseDen = sctx->fs.time_base.den;
    sctx->report_info.funcTypes = esmpp_get_func_types(ctx);
#endif
    av_log(ctx, AV_LOG_DEBUG, "config_output outlink->w: %d, outlink->h: %d.\n", outlink->w, outlink->h);
    return ret;
}

static int esmpp_stack_init_parse(AVFilterContext *avctx) {
    StackESMPPContext *sctx = avctx->priv;
    int ret;

    if (sctx->mode == STACK_X) {
        int is_grid;
        is_grid = sctx->nb_grid_rows && sctx->nb_grid_columns;

        if (sctx->layout && is_grid) {
            av_log(avctx, AV_LOG_ERROR, "Both layout and grid were specified. Only one is allowed.\n");
            return AVERROR(EINVAL);
        }

        if (!sctx->layout && !is_grid) {
            if (sctx->nb_inputs == 2) {
                sctx->nb_grid_rows = 1;
                sctx->nb_grid_columns = 2;
                is_grid = 1;
            } else {
                av_log(avctx, AV_LOG_ERROR, "No layout or grid specified.\n");
                return AVERROR(EINVAL);
            }
        }

        if (is_grid) sctx->nb_inputs = sctx->nb_grid_rows * sctx->nb_grid_columns;

        if (strcmp(sctx->fillcolor_str, "none")
            && av_parse_color(sctx->fillcolor, sctx->fillcolor_str, -1, avctx) >= 0) {
            sctx->fillcolor_enable = 1;
        } else {
            sctx->fillcolor_enable = 0;
        }
    }

    for (int i = 0; i < sctx->nb_inputs + (sctx->enable_logo ? 1 : 0); i++) {
        AVFilterPad pad = {0};

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("input%d", i);

        if (!pad.name) return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(avctx, &pad)) < 0) return ret;
    }

    if (sctx->snap_interval) {
        AVFilterPad pad = complex_snapshot_output;
        int ret = SUCCESS;

        if (ret = ff_append_outpad(avctx, &pad) < 0) {
            av_log(avctx, AV_LOG_ERROR, "esmpp stack snapshot add outpad failed! ret = %d\n", ret);
            return ret;
        } else {
            av_log(avctx, AV_LOG_DEBUG, "esmpp stack snapshot add outpad success.\n");
        }
        sctx->snap_cnt = 0;
    }
    sctx->out_cnt = 0;
    return 0;
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

    s->fs_init = ES_FALSE;
    esctx->out_hw_frame_ref = NULL;
    esctx->out_hw_device_ref = NULL;
    esctx->device_idx = 0;
    esctx->die_idx = 0;
    esctx->mpp_init = ES_FALSE;
    esctx->buf_grp = NULL;
    esctx->mctx = NULL;

    ret = esmpp_stack_init_parse(ctx);
    if (ret) return ret;

    s->regions = av_calloc(s->nb_inputs, sizeof(*s->regions));
    if (!s->regions) {
        ret = AVERROR(ENOMEM);
        av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init alloc region memory failed.\n");
        goto error;
    }

    s->in = av_calloc(ctx->nb_inputs, sizeof(AVFrame *));
    if (!s->in) {
        ret = AVERROR(ENOMEM);
        av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init alloc input frame memory failed.\n");
        goto error;
    }

    esctx->in_sw_fmt = av_calloc(s->nb_inputs, sizeof(enum AVPixelFormat));
    if (!esctx->in_sw_fmt) {
        av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init alloc memory failed.\n");
        goto error;
    }
    esctx->nb_inputs = s->nb_inputs;
    av_log(ctx, AV_LOG_DEBUG, "esmpp_stack_init, nb_inputs: %d.\n", esctx->nb_inputs);

    s->rand_logo_frames = 0;
    s->rand_logo_count = 0;
    s->rand_logo_sent = 0;
    s->rand_logo_pos = 0;

#ifdef TRANSCODE_INFO_REPORT
    s->report_info.inSCnt = esctx->nb_inputs;
    s->report_info.beginDateMs = get_ticks_by_ms();
#endif

    av_log(ctx, AV_LOG_DEBUG, "esmpp_stack_init exit.\n");
    return SUCCESS;

error:
    av_log(ctx, AV_LOG_ERROR, "esmpp_stack_init error, free memory.\n");
    if (esctx->in_sw_fmt) av_freep(&esctx->in_sw_fmt);
    if (s->in) av_freep(&s->in);
    if (s->regions) av_freep(&s->regions);
    if (s->layout) av_freep(&s->layout);
    if (s->fillcolor_str) av_freep(&s->fillcolor_str);

    return AVERROR(ENOMEM);
}

static av_cold void esmpp_stack_uninit(AVFilterContext *ctx) {
    ESMPPContext *esctx = NULL;
    StackESMPPContext *s = NULL;
    ES_BOOL use_hwaccel = ES_FALSE;

    if (!ctx || !ctx->priv) {
        return;
    }

    av_log(ctx, AV_LOG_DEBUG, "esmpp_stack_uninit enter.\n");

    esctx = (ESMPPContext *)ctx->priv;
    s = (StackESMPPContext *)ctx->priv;

    if (s->fs_init) {
        ff_framesync_uninit(&s->fs);
        s->fs_init = ES_FALSE;
    }
    if (s->regions) {
        av_freep(&s->regions);
    }
    if (s->layout) av_freep(&s->layout);
    if (s->fillcolor_str) av_freep(&s->fillcolor_str);

    if (esctx->out_hw_frame_ref) {
        av_buffer_unref(&esctx->out_hw_frame_ref);
        use_hwaccel = ES_TRUE;
    }
    if (esctx->out_hw_device_ref) {
        av_buffer_unref(&esctx->out_hw_device_ref);
    }

    if (esctx->buf_grp) {
        if (!use_hwaccel) {
            mpp_buffer_group_put(esctx->buf_grp);
        }
        esctx->buf_grp = NULL;
    }
    if (esctx->in_sw_fmt) av_freep(&esctx->in_sw_fmt);
    if (s->in) av_freep(&s->in);
    if (esctx->mctx) {
        esmpp_close(esctx->mctx);
    }
    if (esctx->mctx) {
        esmpp_destroy(esctx->mctx);
    }
    av_log(ctx, AV_LOG_DEBUG, "esmpp_stack_uninit exit.\n");
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
    int ret;
    for (int i = 0; i < ctx->nb_inputs; i++) {
        if ((ret = ff_formats_ref(ff_make_format_list(pixel_formats), &ctx->inputs[i]->outcfg.formats)) < 0) {
            return ret;
        }
        if ((ret = ff_formats_ref(ff_all_color_spaces(), &ctx->inputs[i]->outcfg.color_spaces)) < 0) {
            return ret;
        }
        if ((ret = ff_formats_ref(ff_all_color_ranges(), &ctx->inputs[i]->outcfg.color_ranges)) < 0) {
            return ret;
        }
    }
    ret = ff_formats_ref(ff_make_format_list(pixel_formats), &ctx->outputs[0]->incfg.formats);
    return ret;
}

static int stack_activate(AVFilterContext *avctx) {
    StackESMPPContext *sctx = avctx->priv;
    return ff_framesync_activate(&sctx->fs);
}

static const AVFilterPad stack_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

static const AVFilterPad complex_snapshot_output = {
    .name = "snapshot",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_output_snap,
};

static const AVOption stack_esmpp_options[] = {
    {"inputs", "Set number of inputs", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 2}, 2, UINT16_MAX, .flags = FLAGS},
    {"o_width",
     "Set value of output frame width",
     OFFSET(o_width),
     AV_OPT_TYPE_INT,
     {.i64 = 1},
     1,
     UINT16_MAX,
     .flags = FLAGS},
    {"o_height",
     "Set number of output frmame height",
     OFFSET(o_height),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     UINT16_MAX,
     .flags = FLAGS},
    {"snapi",
     "snapshot interval (number of frames).",
     OFFSET(snap_interval),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     FLAGS},
    {"mode", "Set stack mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64 = 2}, 0, 2, .flags = FLAGS},
    {"shortest",
     "Force termination when the shortest input terminates",
     OFFSET(shortest),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {"height",
     "Set output height (0 to use the height of input 0)",
     OFFSET(tile_height),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     UINT16_MAX,
     FLAGS},
    {"width",
     "Set output width (0 to use the width of input 0)",
     OFFSET(tile_width),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     UINT16_MAX,
     FLAGS},
    {"layout", "Set custom layout", OFFSET(layout), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, .flags = FLAGS},
    {"grid",
     "set fixed size grid layout",
     OFFSET(nb_grid_columns),
     AV_OPT_TYPE_IMAGE_SIZE,
     {.str = NULL},
     0,
     0,
     .flags = FLAGS},
    {"grid_tile_size",
     "set tile size in grid layout",
     OFFSET(tile_width),
     AV_OPT_TYPE_IMAGE_SIZE,
     {.str = NULL},
     0,
     0,
     .flags = FLAGS},
    {"fill",
     "Set the color for unused pixels",
     OFFSET(fillcolor_str),
     AV_OPT_TYPE_STRING,
     {.str = "none"},
     .flags = FLAGS},
    {
        "logo",
        "Enable logo",
        OFFSET(enable_logo),
        AV_OPT_TYPE_INT,
        {.i64 = 0},
        0,
        1,
        FLAGS,
        "logo",
    },
    {"logo_x_dir",
     "Set logo start x direction",
     OFFSET(logo_x_dir),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS,
     "logo_x_dir"},
    {"left", "", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, FLAGS, "logo_x_dir"},
    {"right", "", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, FLAGS, "logo_x_dir"},
    {"logo_y_dir",
     "Set logo start y direction",
     OFFSET(logo_y_dir),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS,
     "logo_y_dir"},
    {"top", "", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, FLAGS, "logo_y_dir"},
    {"bottom", "", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, FLAGS, "logo_y_dir"},
    {"logo_x", "Set logo start x", OFFSET(logo_x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS, "logo_x"},
    {"logo_y", "Set logo start y", OFFSET(logo_y), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS, "logo_y"},
    {"rand_logo_duration",
     "Set logo rand display duration(ms)",
     OFFSET(rand_logo_duration),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     INT_MAX,
     FLAGS,
     "rand_logo_duration"},
    {NULL}};

static const AVClass stack_esmpp_class = {
    .class_name = "stack_esmpp",
    .item_name = av_default_item_name,
    .option = stack_esmpp_options,
    .version = LIBAVUTIL_VERSION_INT,
};

const AVFilter ff_vf_stack_esmpp = {
    .name = "stack_esmpp",
    .description = NULL_IF_CONFIG_SMALL("esmpp stack"),
    .priv_size = sizeof(StackHWContext),
    .priv_class = &stack_esmpp_class,
    .init = esmpp_stack_init,
    .uninit = esmpp_stack_uninit,
    .activate = stack_activate,
    FILTER_QUERY_FUNC(esmpp_stack_query_formats),
    FILTER_OUTPUTS(stack_outputs),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_HWDEVICE,
};
