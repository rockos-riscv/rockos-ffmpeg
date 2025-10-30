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
#include "video.h"
#include "vf_esmpp_common.h"
#include "filters.h"
#include "libavutil/esmpp_utils.h"
#include "vf_esmpp_select.h"
#include "libavcodec/esmpp_comm.h"


#define OFFSET(x) offsetof(MppSelectContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define MAX_INPUT_NB 2

#define ESMPP_COMPLEX_DUMP (0)

static int esmpp_select_write_detection_status(AVFilterContext *ctx,
                                               int start, int stop,
                                               int64_t start_time,
                                               int64_t end_time)
{
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    int ret = 0;

    if (!s->json_fp) {
        av_log(ctx, AV_LOG_WARNING, "cannot write to the file, file fp not exist!\n");
        return FAILURE;
    }

    if (fseek(s->json_fp, 0, SEEK_SET) != 0) {
        av_log(ctx, AV_LOG_WARNING, "fseek file failed!\n");
        return FAILURE;
    }

    fprintf(s->json_fp, "{\n");
    fprintf(s->json_fp, "    \"start\":%s,\n", start ? "True" : "False");
    fprintf(s->json_fp, "    \"stop\":%s,\n", stop ? "True" : "False");
    fprintf(s->json_fp, "    \"start_time\": %" PRId64 ",\n", start_time);
    fprintf(s->json_fp, "    \"end_time\": %" PRId64 "\n", end_time);
    fprintf(s->json_fp, "}\n");

    fflush(s->json_fp);

    return ret;
}

static int select_query_formats(AVFilterContext *ctx) {
    av_log(ctx, AV_LOG_DEBUG, "calling select_query_formats.\n");
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

static int esmpp_select_init_bufgroup(AVFilterLink *inlink) {
    AVFilterContext *ctx = inlink->dst;
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    int ret = 0;

    if (inlink->hw_frames_ctx) {
        AVHWFramesContext *hwfc = NULL;
        AVESMPPFramesContext *mppfc = NULL;
        ret = init_hwcontext_buf(
              inlink, &s->out_hw_device_ref, &s->out_hw_frm_ref, s->in_fmt, s->device_idx, s->die_idx);
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

static av_cold int select_config_props(AVFilterLink *inlink) {
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    ES_DOUBLE in_frame_rate = 0.0;
    int ret = SUCCESS;

    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx) {
        AVHWFramesContext *input_hw_frm_ctx = NULL;
        input_hw_frm_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        s->in_fmt = input_hw_frm_ctx->sw_format;
        outlink->format = inlink->format;
    } else {
        s->in_fmt = inlink->format;
        outlink->format = s->in_fmt;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    get_dev_info(ctx->hw_device_ctx, inlink->hw_frames_ctx, &s->device_idx, &s->die_idx);
    if (!s->buf_grp) {
        esmpp_select_init_bufgroup(inlink);
    }
    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx) {
        av_buffer_unref(&inlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frm_ref);
    }

    outlink->frame_rate = inlink->frame_rate;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base = inlink->time_base;

    in_frame_rate = av_q2d(inlink->frame_rate);

    if (0 == s->roi_frame) {
        outlink->frame_rate = av_d2q(in_frame_rate, 1000);
    } else {
        s->roi_skip_interval += 1;
        outlink->frame_rate = av_d2q(in_frame_rate / s->roi_skip_interval, 1000);
    }

    av_log(ctx,
           AV_LOG_DEBUG,
           "out w:%d h:%d @%.2f -> w:%d h:%d @%.2f\n",
           inlink->w,
           inlink->h,
           in_frame_rate,
           outlink->w,
           outlink->h,
           av_q2d(outlink->frame_rate));

    return ret;
}

static av_cold int esmpp_select_init(AVFilterContext *ctx) {
    MppSelectContext *s = NULL;
    int ret = SUCCESS;

    if (!ctx || !ctx->priv) {
        av_log(ctx, AV_LOG_ERROR, "Initializing esmpp_select filter failed!\n");
        return FAILURE;
    }
    av_log(ctx, AV_LOG_INFO, "Initializing esmpp_select filter.\n");

    s = (MppSelectContext *)ctx->priv;
    s->device_idx = 0;
    s->die_idx = 0;
    s->frame_cnt = 0;
    s->frame_roi_cnt = 0;
    s->buf_grp = NULL;
    s->select_inited = ES_FALSE;
    s->target_enter_cnt = 0;
    s->target_exit_cnt = 0;
    s->target_state = ESMPP_SELECT_TARGET_LEAVE;

    if (s->target_detect) {
        s->json_fp = esmpp_filter_file_write_open(s->json_file);
        if (!s->json_fp) {
            av_log(ctx, AV_LOG_ERROR, "Initializing esmpp_select filter.\n");
            return FAILURE;
        }
        esmpp_select_write_detection_status(ctx, 0, 0, -1, -1);
    }

    return ret;
}

static av_cold void esmpp_select_uninit(AVFilterContext *ctx) {
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    ES_BOOL use_hwaccel = ES_FALSE;

    if (!ctx || !ctx->priv) {
        av_log(ctx, AV_LOG_ERROR, "uninit esmpp_select filter failed!\n");
        return;
    }
    av_log(ctx, AV_LOG_DEBUG, "esmpp select uninit\n");
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

    if (s->target_detect) {
        esmpp_filter_dump_file_close(s->json_fp);
    }
}

static void esmpp_select_handle_detection(AVFilterContext *ctx, ES_BOOL has_target) {
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    time_t now;

    av_log(ctx, AV_LOG_DEBUG, "esmpp select frame_cnt:%d, target state:%d.\n", s->frame_cnt, s->target_state);
    if (has_target) {
        s->target_enter_cnt++;
        s->target_exit_cnt = 0;
        if (ESMPP_SELECT_TARGET_LEAVE == s->target_state && s->target_enter_cnt >= s->target_detect_thres) {
            time(&now);
            s->target_state = ESMPP_SELECT_TARGET_ENTER;
            esmpp_select_write_detection_status(ctx, 1, 0, now, -1);
            av_log(ctx, AV_LOG_DEBUG, "esmpp select target enter, frame_cnt:%d.\n", s->frame_cnt);
        }
    } else {
        s->target_enter_cnt = 0;
        s->target_exit_cnt++;
        if (ESMPP_SELECT_TARGET_ENTER == s->target_state && s->target_exit_cnt >= s->target_detect_thres) {
            time(&now);
            s->target_state = ESMPP_SELECT_TARGET_LEAVE;
            esmpp_select_write_detection_status(ctx, 0, 1, -1, now);
            av_log(ctx, AV_LOG_DEBUG, "esmpp select target leave, frame_cnt:%d.\n", s->frame_cnt);
        }
    }
}

static int esmpp_select_filter_frame(AVFilterLink *link, AVFrame *frame) {
    AVFilterContext *ctx = link->dst;
    MppSelectContext *s = (MppSelectContext *)ctx->priv;
    AVFrameSideData *sd_roi = NULL;
    AVFrameSideData *sd_roi_proc = NULL;
    ES_BOOL has_target = ES_FALSE;

    s->frame_cnt++;
    sd_roi = av_frame_get_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);

    if (s->target_detect) {
        has_target = sd_roi != NULL ? ES_TRUE : ES_FALSE;
        sd_roi_proc = av_frame_get_side_data(frame, AV_FRAME_DATA_ESMPP_SIDE_DATA);
        if (sd_roi_proc) {
            esmpp_select_handle_detection(ctx, has_target);
        }
    }

    if (s->roi_frame) {
        if (!sd_roi) {
            frame->flags |= AV_FRAME_FLAG_DISCARD;
            av_log(ctx, AV_LOG_DEBUG, "esmpp current frame no roi, frame_num:%d \n", s->frame_cnt);
            return ff_filter_frame(ctx->outputs[0], frame);
        }
        s->frame_roi_cnt++;
        av_log(ctx,
               AV_LOG_DEBUG,
               "esmpp select frame roi, frame_num:%d, roi frames:%d \n",
               s->frame_cnt, s->frame_roi_cnt);

        return ff_filter_frame(ctx->outputs[0], frame);
    }
    av_log(ctx, AV_LOG_DEBUG, "esmpp select filer send frame, frame_num:%d \n", s->frame_cnt);
    return ff_filter_frame(ctx->outputs[0], frame);
}

static const AVOption options[] = {
    {"roi_frame", "select the frame with roi side data",
      OFFSET(roi_frame), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    {"roi_skip_interval", "skip some frames to improve the ROI performance",
      OFFSET(roi_skip_interval), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, .flags = FLAGS},
    {"target_detect", "detect target enter into the region function enable",
      OFFSET(target_detect), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    {"target_detect_thres", "target is considered present after being detected in consecutive frames threshold.",
      OFFSET(target_detect_thres), AV_OPT_TYPE_INT, {.i64 = 3}, 0, INT_MAX, .flags = FLAGS},
    {"json_file", "dump the target enter and exit time to json file",
      OFFSET(json_file), AV_OPT_TYPE_STRING, {.str = "/home/eswin/roi.json"}, .flags = FLAGS},
    {NULL},
};

static const AVClass esmpp_select_class = {
    .class_name = "esmpp_select",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad esmpp_select_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = esmpp_select_filter_frame,
        .config_props = select_config_props,
    },
};

AVFilter ff_vf_select_esmpp = {
    .name = "esmpp_select",
    .description = NULL_IF_CONFIG_SMALL("eswin esmpp select filter"),
    .init = esmpp_select_init,
    .uninit = esmpp_select_uninit,
    .priv_size = sizeof(MppSelectContext),
    .priv_class = &esmpp_select_class,
    FILTER_INPUTS(esmpp_select_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC(select_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags = AVFILTER_FLAG_HWDEVICE,
};