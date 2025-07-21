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
#include "libavutil/time.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "mpp_tde_api.h"
#include "mpp_tde_cfg.h"
#include "video.h"
#include "vf_esmpp_common.h"
#include "filters.h"
#include "libavutil/esmpp_transfer.h"
#include "es_mpp.h"

#define OFFSET(x) offsetof(MppFilterContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define MAX_INPUT_NB 2

#define ESMPP_COMPLEX_DUMP (0)

static const AVFilterPad complex_snapshot_output;

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

static int init_mpp_tde(AVFilterContext *ctx) {
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    MppTdeCfgPtr tde_cfg;
    int ret = SUCCESS;

    if (!ctx || !ctx->priv) {
        return FAILURE;
    }
    if (s->mpp_init) {
        return MPP_OK;
    }

    if ((ret = esmpp_create(&s->mctx, MPP_CTX_TDE, MPP_VIDEO_CodingUnused)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_select_dev(s->mctx, s->device_idx, s->die_idx)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to select MPP dev: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_init(s->mctx)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_tde_cfg_init(&tde_cfg)) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp_tde_cfg_ret: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        return ret;
    } else {
        av_log(ctx, AV_LOG_DEBUG, "success to init mpp_tde_cfg_ret: %d\n", ret);
    }

    ret = esmpp_control(s->mctx, MPP_TDE_GET_CFG, tde_cfg);
    if (ret != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "get complex cfg failed ret: %d\n", ret);
        mpp_tde_cfg_deinit(&tde_cfg);
        return ret;
    } else {
        av_log(ctx, AV_LOG_DEBUG, "get complex cfg success ret: %d\n", ret);
    }
    mpp_tde_cfg_set_s32(tde_cfg, "moudle_mode", TDE_MODULE_COMPLEX);

    ret = esmpp_control(s->mctx, MPP_TDE_SET_CFG, tde_cfg);
    if (ret != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "complex cfg failed\n");
    } else {
        av_log(ctx, AV_LOG_DEBUG, "complex cfg success\n");
    }
    mpp_tde_cfg_deinit(&tde_cfg);

    ret = esmpp_open(s->mctx);
    s->mpp_init = ES_TRUE;
    return ret;

fail:
    if (s->mctx) {
        esmpp_destroy(s->mctx);
    }
    return ret;
}

static av_cold int init(AVFilterContext *ctx) {
    MppFilterContext *s = NULL;
    MppTdeCfgPtr tde_cfg;
    int ret = SUCCESS;

    if (!ctx || !ctx->priv) {
        return FAILURE;
    }

    s = (MppFilterContext *)ctx->priv;
    s->src_global_alpha = -1;
    s->dst_global_alpha = -1;
    s->nb_inputs = 1;
    s->device_idx = 0;
    s->die_idx = 0;
    s->mpp_init = ES_FALSE;
    s->fs_init = ES_FALSE;
    if (s->enable_logo) {
        s->blend_mode_set = 4;
        s->rand_logo_frames = 0;
        s->rand_logo_count = 0;
        s->rand_logo_sent = 0;
        s->rand_logo_pos = 0;
    }
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

    if (s->snap_interval) {
        AVFilterPad pad = complex_snapshot_output;

        if (ret = ff_append_outpad(ctx, &pad) < 0) {
            av_log(ctx, AV_LOG_ERROR, "snapshot add outpad failed! ret = %d\n", ret);
            return ret;
        } else {
            av_log(ctx, AV_LOG_DEBUG, "snapshot add outpad success.\n");
        }
        s->snap_cnt = 0;
    }
    s->out_cnt = 0;
#ifdef TRANSCODE_INFO_REPORT
    s->report_info.inSCnt = s->nb_inputs;
    s->report_info.beginDateMs = get_ticks_by_ms();
#endif

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx) {
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    ES_BOOL use_hwaccel = ES_FALSE;

    if (!ctx || !ctx->priv) {
        return;
    }

    if (s->fs_init) {
        ff_framesync_uninit(&s->fs);
        s->fs_init = ES_FALSE;
    }

    if (s->out_hw_frm_ref) {
        av_buffer_unref(&s->out_hw_frm_ref);
        use_hwaccel = ES_TRUE;
    }
    if (s->out_hw_device_ref) {
        av_buffer_unref(&s->out_hw_device_ref);
    }
    if (s->buf_grp) {
        if (!use_hwaccel) {
            mpp_buffer_group_put(s->buf_grp);
        }
        s->buf_grp = NULL;
    }
    if (s->mctx) {
        esmpp_close(s->mctx);
    }
    if (s->mctx) {
        esmpp_destroy(s->mctx);
    }
    s->mpp_init = ES_FALSE;
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

static int esmpp_complex_send_snapshot_frame(AVFilterLink *outlink, AVFrame *frame, AVFilterContext *avctx) {
    MppFilterContext *s = (MppFilterContext *)avctx->priv;
    int ret = FAILURE;

    if (outlink && frame) {
        ret = ff_filter_frame(outlink, frame);
        if (ret >= 0) {
            s->snap_cnt++;
            av_log(avctx, AV_LOG_DEBUG, "snap send success. out1 cnt:%d, snap cnt:%d\n", s->out_cnt, s->snap_cnt);
        } else {
            av_log(avctx,
                   AV_LOG_ERROR,
                   "snap send failed. ret:%d, out1 send cnt:%d, snap send cnt:%d\n",
                   ret,
                   s->out_cnt,
                   s->snap_cnt);
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "snap send failed.\n");
    }

    return ret;
}

static int esmpp_complex_filter_frame(int input_nb, AVFilterLink **link, AVFrame **in) {
    AVFilterLink *link_src = link[0];
    AVFrame *in_src = NULL, *in_dst = NULL;
    AVFilterContext *ctx = link_src->dst;
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *outlink_snap = NULL;
    AVFrame *out = NULL;
    AVFrame *frame_snap = NULL;
    ES_BOOL bneed_snap = ES_FALSE;
    int ret = SUCCESS;
    MPP_RET mpp_ret = MPP_OK;
    MppFramePtr src_mpp_frame = NULL;
    MppFramePtr dst_mpp_frame = NULL;
    MppBufferPtr src_mpp_buf = NULL;
    MppBufferPtr dst_mpp_buf = NULL;
    ES_S32 usage = 0;
    size_t in_frame_size = 0, output_frame_size = 0;
    uint32_t pic_size = 0;
    int plane = 0, stride[4] = {0}, offset[4] = {0};

    // overlay
    if (input_nb == 2) {
        in_dst = in[0];  // the 1st input is the main image.
        in_src = in[1];  // the 2nd input is the overlay image.
    } else {
        in_src = in[0];
    }

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

    esmpp_stack_get_offset_stride(in_src, s->in_fmt, stride, offset, &pic_size);
    esmpp_set_mpp_frame(src_mpp_frame,
                        ff_fmt_to_mpp_fmt(s->in_fmt),
                        in_src->width,
                        in_src->height,
                        stride,
                        offset,
                        s->src_rotation,
                        s->src_global_alpha);
#ifdef TRANSCODE_INFO_REPORT
    s->report_info.inWidth = in_src->width;
    s->report_info.inHeight = in_src->height;
#endif

    in_frame_size = pic_size;

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
        // in_frame_size = esmpp_get_frame_data_size(s->in_fmt, in_src);
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

    esmpp_set_mpp_frame(dst_mpp_frame,
                        ff_fmt_to_mpp_fmt(out->format),
                        out->width,
                        out->height,
                        stride,
                        offset,
                        s->dst_rotation,
                        s->dst_global_alpha);
    mpp_frame_set_buffer(dst_mpp_frame, dst_mpp_buf);
    mpp_frame_set_buf_size(dst_mpp_frame, output_frame_size);

#ifdef TRANSCODE_INFO_REPORT
    s->report_info.outWidth = out->width;
    s->report_info.outHeight = out->height;
    mpp_frame_set_report_opaque(dst_mpp_frame, (ES_S64)&s->report_info);
#endif

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
    if (s->blend_mode_set != -1) {
        if (s->enable_logo) {
            if (ctx->inputs[1] && s->rand_logo_duration > 0) {
                s->rand_logo_count--;
                if (s->rand_logo_count <= 0) {
                    srand(av_gettime());
                    s->rand_logo_count = s->rand_logo_frames;
                    s->rand_logo_sent = 0;
                    s->rand_logo_pos = rand() & 0x3;
                    av_log(ctx,
                           AV_LOG_TRACE,
                           "vf[esmpp_complex] rand_logo_count:%d, rand_logo_pos:%d.\n",
                           s->rand_logo_count,
                           s->rand_logo_pos);
                }
                if (!s->rand_logo_sent) {  // set display rect when change
                    s->rand_logo_sent = 1;
                    switch (s->rand_logo_pos) {
                        default:
                        case 0:  // left top
                            s->disp_rect.x = (s->logo_x) & ~0x1;
                            s->disp_rect.y = (s->logo_y) & ~0x1;
                            break;
                        case 1:  // right top
                            s->disp_rect.x = (outlink->w - s->logo_x - s->src_rect.width) & ~0x1;
                            s->disp_rect.y = (s->logo_y) & ~0x1;
                            break;
                        case 2:  // left bottom
                            s->disp_rect.x = (s->logo_x) & ~0x1;
                            s->disp_rect.y = (outlink->h - s->logo_y - s->src_rect.height) & ~0x1;
                            break;
                        case 3:  // right bottom
                            s->disp_rect.x = (outlink->w - s->logo_x - s->src_rect.width) & ~0x1;
                            s->disp_rect.y = (outlink->h - s->logo_y - s->src_rect.height) & ~0x1;
                            break;
                    }
                    s->disp_rect.width = s->src_rect.width;
                    s->disp_rect.height = s->src_rect.height;
                    av_log(ctx,
                           AV_LOG_TRACE,
                           "vf[esmpp_complex] logo display rect x:%d, y:%d, w:%d, h:%d, rand_logo_frames:%d.\n",
                           s->disp_rect.x,
                           s->disp_rect.y,
                           s->disp_rect.width,
                           s->disp_rect.height,
                           s->rand_logo_frames);
                }
            } else if (s->disp_rect.width == 0 || s->disp_rect.height == 0) {  // set display rect once if no duration
                int logo_x = s->logo_x, logo_y = s->logo_y;
                if (s->logo_x_dir) {
                    logo_x = outlink->w - logo_x - s->src_rect.width;
                }
                if (s->logo_y_dir) {
                    logo_y = outlink->h - logo_y - s->src_rect.height;
                }
                if (logo_x < 0) {
                    logo_x = 0;
                }
                if (logo_x >= outlink->w - s->src_rect.width) {
                    logo_x = outlink->w - s->src_rect.width;
                }

                if (logo_y < 0) {
                    logo_y = 0;
                }
                if (logo_y >= outlink->h - s->src_rect.height) {
                    logo_y = outlink->h - s->src_rect.height;
                }

                s->disp_rect.x = logo_x;
                s->disp_rect.y = logo_y;
                s->disp_rect.width = s->src_rect.width;
                s->disp_rect.height = s->src_rect.height;
                av_log(ctx,
                       AV_LOG_TRACE,
                       "vf[esmpp_complex] logo display rect x:%d, y:%d, w:%d, h:%d, rand_logo_frames:%d.\n",
                       s->disp_rect.x,
                       s->disp_rect.y,
                       s->disp_rect.width,
                       s->disp_rect.height,
                       s->rand_logo_frames);
            }
        }
        mpp_ret = esmpp_process_frame_sync(
            s->mctx, src_mpp_frame, 0, dst_mpp_frame, &s->src_rect, ES_NULL, &s->disp_rect, usage | TDE_USAGE_LOGO);
    } else {
        mpp_ret = esmpp_process_frame_sync(
            s->mctx, src_mpp_frame, 0, dst_mpp_frame, &s->src_rect, &s->dst_rect, ES_NULL, TDE_USAGE_COMPLEX);
    }
    if (mpp_ret != MPP_OK) {
        ret = FAILURE;
        goto exit3;
    }

    ret = SUCCESS;

#ifdef TRANSCODE_HOST
    if (out->format != AV_PIX_FMT_DRM_PRIME) {
        AVFrame *frame = NULL;
        frame = av_frame_alloc();
        frame->format = out->format;
        frame->width = out->width;
        frame->height = out->height;
        frame->buf[0] = av_buffer_alloc(output_frame_size);
        for (int i = 0; i < plane; i++) {
            frame->linesize[i] = stride[i];
            frame->data[i] = frame->buf[0]->data + offset[i];
        }

        esmpp_hwframe_transfer_bysize(frame, out, output_frame_size);
        if (out) {
            av_frame_free(&out);
        }
        out = frame;
    }
#else
#if ESMPP_COMPLEX_DUMP
    write_buffer_to_file(
        mpp_buffer_get_ptr(dst_mpp_buf), output_frame_size, NULL, out->width, out->height, s->out_fmt, 2);
#endif
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
        ret = ff_filter_frame(outlink, out);
        if (ret >= 0) {
            s->out_cnt++;
            av_log(ctx, AV_LOG_DEBUG, "filter frame send success. statistic cnt:%d\n", s->out_cnt);
        } else {
            av_log(ctx, AV_LOG_ERROR, "filter frame send failed in frame num:%d ret:%d\n", s->out_cnt, ret);
        }

        bneed_snap = s->snap_interval ? 0 == s->out_cnt % s->snap_interval : ES_FALSE;
        if (bneed_snap) {
            if (ctx->nb_outputs > 1) {
                outlink_snap = ctx->outputs[1];
                frame_snap = av_frame_alloc();
                if (av_frame_ref(frame_snap, out) >= 0) {
                    esmpp_complex_send_snapshot_frame(outlink_snap, frame_snap, ctx);
#ifdef TRANSCODE_INFO_REPORT
                    s->report_info.snapshotCnt = ++s->snapshot_cnt;
                    s->report_info.snapshotTotalBits = 0;
#endif
                } else {
                    av_frame_free(&frame_snap);
                    av_log(ctx, AV_LOG_ERROR, "snap av_frame_ref failed\n");
                }
            } else {
                av_log(ctx,
                       AV_LOG_ERROR,
                       "need snap but no snap shot outpad,snap_interval=%d, nb_outputs=%d\n",
                       s->snap_interval,
                       ctx->nb_outputs);
            }
        }
    } else {
        av_log(ctx, AV_LOG_ERROR, "complex filtered frame sends to next link failed\n");
    }

    return ret;
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
#ifdef TRANSCODE_INFO_REPORT
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    s->report_info.bReported = s->process_started;
    if (!s->process_started) {
        s->report_info.beginMediaTs = in[0]->pts;
        s->process_started = ES_TRUE;
    }
    s->report_info.curDateMs = get_ticks_by_ms();
    s->report_info.AliveDurationMs = s->report_info.curDateMs - s->report_info.beginDateMs;
    s->report_info.curMediaTs = in[0]->pts;
    s->report_info.inBufferNum = ++s->in_buffer_num;
    s->report_info.inFrameRate = av_q2d(inlink->frame_rate);
    s->report_info.outFrameRate = av_q2d(outlink->frame_rate);
#endif
    ret = esmpp_complex_filter_frame(ctx->nb_inputs, ctx->inputs, in);
#ifdef TRANSCODE_INFO_REPORT
    if (ret) {
        s->report_info.failBufferNum = ++s->fail_buffer_num;
    } else {
        s->report_info.outBufferNum = ++s->out_buffer_num;
    }
#endif

    return ret;
}

static int complex_init_output_bufgroup(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    int ret = 0;

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *hwfc = NULL;
        AVESMPPFramesContext *mppfc;
        ret = init_hwcontext_buf(
            outlink, &s->out_hw_device_ref, &s->out_hw_frm_ref, s->out_fmt, s->device_idx, s->die_idx);
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

    if (s->fs_init) return 0;

    if ((ret = ff_framesync_init(&s->fs, avctx, s->nb_inputs)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_framesync_init failed, ret = %d.\n", ret);
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

    if ((ret = ff_framesync_configure(&s->fs)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_framesync_configure , ret = %d.\n", ret);
    }

    s->fs_init = ES_TRUE;

    return ret;
}

#ifdef TRANSCODE_INFO_REPORT
static TdeFuncType get_func_types(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0], *inlink_dst = ctx->inputs[1];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;

    TdeFuncType type = TDE_FUNC_TYPE_BUTT;
    do {
        if (s->blend_mode_set != -1) {
            type = av_pix_fmt_count_planes(s->in_fmt) == 1 ? TDE_FUNC_TYPE_LOGO_RGB : TDE_FUNC_TYPE_LOGO_YUV;
            break;
        }
        if (s->dst_rotation == ROTATION_90 || s->dst_rotation == ROTATION_270) {
            type = TDE_FUNC_TYPE_ROTATION_90_270;
            break;
        }
        if (s->dst_rotation == ROTATION_180) {
            type = TDE_FUNC_TYPE_ROTATION_180;
            break;
        }
        if (s->in_fmt != s->out_fmt) {
            type = TDE_FUNC_TYPE_CSC;
            break;
        }
        if (s->crop_set) {
            type = TDE_FUNC_TYPE_CROP;
            break;
        }
        if (inlink->w != outlink->w || inlink->h != outlink->h) {
            type = TDE_FUNC_TYPE_RESIZE;
            break;
        }

    } while (0);

    return type;
}
#endif

static av_cold int complex_config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink_main = ctx->inputs[0], *inlink_overlay = ctx->inputs[1];
    MppFilterContext *s = (MppFilterContext *)ctx->priv;
    const ES_S32 output = outlink->srcpad - ctx->output_pads;
    ES_DOUBLE in_frame_rate = 0.0;

    // for blend: clip/o_w/0_h work for dst frame, other opt for src frame.
    if (ctx->nb_inputs == 1) {
        s->src_rect.x = 0;
        s->src_rect.y = 0;
        s->src_rect.width = inlink_main->w;
        s->src_rect.height = inlink_main->h;

        if (s->crop_set) {
            if (parse_rect(s->crop_set, &s->src_rect)) {
                av_log(ctx, AV_LOG_ERROR, "vf[esmpp_complex] parse crop cmd failed.\n");
                return FAILURE;
            }
        }

        s->dst_rect.x = 0;
        s->dst_rect.y = 0;
        s->dst_rect.width = s->src_rect.width;
        s->dst_rect.height = s->src_rect.height;
    } else {
        s->src_rect.x = 0;
        s->src_rect.y = 0;
        s->src_rect.width = inlink_overlay->w;
        s->src_rect.height = inlink_overlay->h;

        s->dst_rect.x = 0;
        s->dst_rect.y = 0;
        s->dst_rect.width = inlink_main->w;
        s->dst_rect.height = inlink_main->h;
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

    if (inlink_main->format == AV_PIX_FMT_DRM_PRIME && inlink_main->hw_frames_ctx) {
        AVHWFramesContext *input_hw_frm_ctx = NULL;
        input_hw_frm_ctx = (AVHWFramesContext *)inlink_main->hw_frames_ctx->data;
        s->in_fmt = input_hw_frm_ctx->sw_format;
        set_output_fmt(s);
        outlink->format = inlink_main->format;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);

        get_dev_info(ctx->hw_device_ctx, inlink_main->hw_frames_ctx, &s->device_idx, &s->die_idx);
        init_mpp_tde(ctx);

        if (!s->buf_grp) {
            complex_init_output_bufgroup(outlink);
        }
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frm_ref);
    } else {
        s->in_fmt = inlink_main->format;
        set_output_fmt(s);
        outlink->format = s->out_fmt;
        adjust_width_height_by_format(s->out_fmt, &outlink->w, &outlink->h);

        get_dev_info(ctx->hw_device_ctx, inlink_main->hw_frames_ctx, &s->device_idx, &s->die_idx);
        init_mpp_tde(ctx);

        if (!s->buf_grp) {
            complex_init_output_bufgroup(outlink);
        }
    }
    outlink->frame_rate = inlink_main->frame_rate;
    outlink->sample_aspect_ratio = inlink_main->sample_aspect_ratio;

    in_frame_rate = av_q2d(inlink_main->frame_rate);
    if (output == 0) {
        av_log(ctx,
               AV_LOG_DEBUG,
               "[out] w:%d h:%d @%.2f -> w:%d h:%d @%.2f\n",
               inlink_main->w,
               inlink_main->h,
               in_frame_rate,
               outlink->w,
               outlink->h,
               in_frame_rate);
        outlink->frame_rate = av_d2q(in_frame_rate, 1000);
    } else {
        av_log(ctx,
               AV_LOG_DEBUG,
               "[out1] w:%d h:%d @%.2f -> w:%d h:%d @%.2f\n",
               inlink_main->w,
               inlink_main->h,
               in_frame_rate,
               outlink->w,
               outlink->h,
               in_frame_rate / s->snap_interval);
        outlink->frame_rate = av_d2q(in_frame_rate / s->snap_interval, 1000);
    }

    init_framesync(ctx);
    outlink->time_base = s->fs.time_base;

    if (s->enable_logo) {
        if (s->logo_x < 0) {
            s->logo_x = 0;
        }
        if (s->logo_y < 0) {
            s->logo_y = 0;
        }

        s->rand_logo_frames = s->rand_logo_duration * av_q2d(outlink->frame_rate) / 1000;
    } else if (s->blend_mode_set != -1) {
        s->disp_rect.x = 0;
        s->disp_rect.y = 0;
        s->disp_rect.width = s->src_rect.width;
        s->disp_rect.height = s->src_rect.height;
    }

    // if (esmpp_select_dev(s->mctx, s->device_idx, s->die_idx) != MPP_OK) {
    //     av_log(ctx, AV_LOG_ERROR, "Failed to select MPP dev\n");
    // }

#ifdef TRANSCODE_INFO_REPORT
    s->report_info.timeBaseNum = outlink->time_base.num;
    s->report_info.timeBaseDen = outlink->time_base.den;
    s->report_info.funcTypes = get_func_types(outlink);
#endif

    return SUCCESS;
}

static int esmpp_complex_filter_activate(AVFilterContext *ctx) {
    MppFilterContext *s = ctx->priv;

    if (s->fs.eof) {
        av_log(ctx, AV_LOG_DEBUG, "All input streams have ended.\n");
        return AVERROR_EOF;
    }
    return ff_framesync_activate(&s->fs);
}

static const AVOption options[] = {
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
    {
        "snapi",
        "snapshot interval (number of frames).",
        OFFSET(snap_interval),
        AV_OPT_TYPE_INT,
        {.i64 = 0},
        0,
        INT_MAX,
        FLAGS,
    },
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
    {"shortest",
     "force termination when the shortest input terminates",
     OFFSET(fs.opt_shortest),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     FLAGS},
    {"repeatlast",
     "repeat overlay of the last overlay frame",
     OFFSET(fs.opt_repeatlast),
     AV_OPT_TYPE_BOOL,
     {.i64 = 1},
     0,
     1,
     FLAGS},
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

static const AVFilterPad complex_snapshot_output = {
    .name = "snapshot",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = complex_config_props,
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
    .flags = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_HWDEVICE,
};
