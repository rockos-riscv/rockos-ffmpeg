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
#include "libavutil/detection_bbox.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "vf_esmpp_common.h"
#include "filters.h"
#include "libavutil/esmpp_utils.h"
#include "es_mpp.h"
#include "vf_esmpp_drawtext.h"
#include "libavcodec/esmpp_comm.h"
#include "mpp_drawtext_api.h"
#include "mpp_drawtext_cfg.h"
#include "mpp_drawtext_type.h"

#define OFFSET(x) offsetof(MppDrawTextContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

#define ESMPP_DRAWTEXT_DUMP (0)
#define HWTYPE_TDE 0
#define HWTYPE_CPU 1

static int esmpp_drawtext_query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_GRAY8,
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
    for (int i = 0; i < ctx->nb_inputs; i++)
    {
        if ((ret = ff_formats_ref(ff_make_format_list(pixel_formats), &ctx->inputs[i]->outcfg.formats)) < 0)
        {
            return ret;
        }
        if ((ret = ff_formats_ref(ff_all_color_spaces(), &ctx->inputs[i]->outcfg.color_spaces)) < 0)
        {
            return ret;
        }
        if ((ret = ff_formats_ref(ff_all_color_ranges(), &ctx->inputs[i]->outcfg.color_ranges)) < 0)
        {
            return ret;
        }
    }
    ret = ff_formats_ref(ff_make_format_list(pixel_formats), &ctx->outputs[0]->incfg.formats);
    return ret;
}

static int esmpp_drawtext_init_bufgroup(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    int ret = 0;

    if (inlink->hw_frames_ctx)
    {
        AVHWFramesContext *hwfc = NULL;
        AVESMPPFramesContext *mppfc = NULL;
        ret = init_hwcontext_buf(
            inlink, &s->out_hw_device_ref, &s->out_hw_frm_ref, s->in_fmt, s->device_idx, s->die_idx);
        if (ret)
        {
            return ret;
        }

        hwfc = (AVHWFramesContext *)s->out_hw_frm_ref->data;
        mppfc = hwfc->hwctx;
        s->buf_grp = mppfc->buf_group;
    }
    else
    {
        ret = mpp_buffer_group_get_internal(&s->buf_grp, MPP_BUFFER_TYPE_DMA_HEAP);
        if (ret)
        {
            av_log(ctx, AV_LOG_ERROR, "Create buffer group with type %d failed: %d.\n", MPP_BUFFER_TYPE_DMA_HEAP, ret);
            return FAILURE;
        }
        ret = mpp_buffer_group_limit_config(s->buf_grp, 0, 0);
        if (ret)
        {
            av_log(ctx, AV_LOG_ERROR, "Limit buffer group with no limit failed: %d.\n", ret);
            return FAILURE;
        }
    }

    return ret;
}

static int esmpp_drawtext_init_mpp_drawtext(AVFilterContext *ctx)
{
    MppDrawTextContext *s;
    MppDrawtextCfgPtr drawtext_cfg;
    int ret = SUCCESS;

    ES_RETURN_VAL_IF_FAIL(ctx, FAILURE);
    ES_RETURN_VAL_IF_FAIL(ctx->priv, FAILURE);

    s = (MppDrawTextContext *)ctx->priv;
    if (s->draw_init)
    {
        return MPP_OK;
    }

    if ((ret = esmpp_create(&s->draw_mctx, MPP_CTX_DRAWTEXT, MPP_VIDEO_CodingUnused)) != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_select_dev(s->draw_mctx, s->device_idx, s->die_idx)) != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "Failed to select MPP dev: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_init(s->draw_mctx)) != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_drawtext_cfg_init(&drawtext_cfg)) != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "Failed to init mpp_drawtext_cfg_init: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        return ret;
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "success to init mpp_drawtext_cfg_init: %d\n", ret);
    }

    ret = esmpp_control(s->draw_mctx, MPP_DRAWTEXT_GET_CFG, drawtext_cfg);
    if (ret != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "get cfg failed ret: %d\n", ret);
        mpp_drawtext_cfg_deinit(&drawtext_cfg);
        return ret;
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "get cfg success ret: %d\n", ret);
    }
    mpp_drawtext_cfg_set_string(drawtext_cfg, "file_path", s->ttf_path);

    ret = esmpp_control(s->draw_mctx, MPP_DRAWTEXT_SET_CFG, drawtext_cfg);
    if (ret != MPP_OK)
    {
        av_log(ctx, AV_LOG_ERROR, "set cfg failed\n");
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "set cfg success\n");
    }
    mpp_drawtext_cfg_deinit(&drawtext_cfg);

    ret = esmpp_open(s->draw_mctx);
    s->draw_init = ES_TRUE;
    return ret;

fail:
    if (s->draw_mctx)
    {
        esmpp_destroy(s->draw_mctx);
    }
    return ret;
}

static av_cold int esmpp_drawtext_config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    int ret = SUCCESS;

    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx)
    {
        AVHWFramesContext *input_hw_frm_ctx = NULL;
        input_hw_frm_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        outlink->format = inlink->format;
        s->in_fmt = input_hw_frm_ctx->sw_format;
    }
    else
    {
        s->in_fmt = inlink->format;
        outlink->format = s->in_fmt;
    }

    if(s->in_fmt != AV_PIX_FMT_NV12 && s->hw_type == HWTYPE_CPU) {
        av_log(ctx, AV_LOG_ERROR, "cpu hardware only support NV12 format!\n");
        return AVERROR_EXTERNAL;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    get_dev_info(ctx->hw_device_ctx, inlink->hw_frames_ctx, &s->device_idx, &s->die_idx);
    // esmpp_set_dev(s->device_idx, s->die_idx);
    if (!s->buf_grp)
    {
        esmpp_drawtext_init_bufgroup(inlink);
    }
    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx)
    {
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frm_ref);
    }

    av_log(ctx, AV_LOG_DEBUG, "es drawtext init enter\n");
    if (!s->ttf_path)
    {
        av_log(ctx, AV_LOG_ERROR, "ttf_path is not set\n");
        return ret;
    }
    ret = esmpp_drawtext_init_mpp_drawtext(ctx);
    if (MPP_OK != ret)
    {
        av_log(ctx, AV_LOG_ERROR, "es drawtext init failed. ret = %d\n", ret);
        return ret;
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "es drawtext init success\n");
    }

    return ret;
}

static av_cold int esmpp_drawtext_init(AVFilterContext *ctx)
{
    MppDrawTextContext *s = NULL;
    int ret = SUCCESS;

    if (!ctx || !ctx->priv)
    {
        return FAILURE;
    }
    av_log(ctx, AV_LOG_INFO, "Initializing esmpp_drawtext filter.\n");

    s = (MppDrawTextContext *)ctx->priv;
    s->device_idx = 0;
    s->die_idx = 0;
    s->buf_grp = NULL;
    s->draw_init = ES_FALSE;

    return ret;
}

static av_cold void esmpp_drawtext_uninit(AVFilterContext *ctx)
{
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    ES_BOOL use_hwaccel = ES_FALSE;

    esmpp_set_device(s->draw_mctx);

    if (!ctx || !ctx->priv)
    {
        return;
    }
    av_log(ctx, AV_LOG_DEBUG, "es drawtext uninit enter.\n");
    if (s->out_hw_frm_ref)
    {
        av_buffer_unref(&s->out_hw_frm_ref);
        use_hwaccel = ES_TRUE;
    }
    if (s->out_hw_device_ref)
    {
        av_buffer_unref(&s->out_hw_device_ref);
    }
    if (s->buf_grp)
    {
        if (!use_hwaccel)
        {
            mpp_buffer_group_put(s->buf_grp);
        }
        s->buf_grp = NULL;
    }
    if (s->draw_init)
    {
        if (s->draw_mctx)
        {
            esmpp_close(s->draw_mctx);
            esmpp_destroy(s->draw_mctx);
        }
        s->draw_init = false;
    }
    av_log(ctx, AV_LOG_DEBUG, "es drawtext uninit success.\n");
}

static ES_S32 esmpp_drawtext_init_draw_info_from_roi(AVFilterContext *ctx,
                                                    DrawtextDrawInfoList *draw_info,
                                                    AVFrame *frame)
{
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    AVDetectionBBoxHeader *hdr = NULL;
    AVFrameSideData *sd = NULL;
    int hdr_cnt = 0;

    ES_RETURN_VAL_IF_FAIL(frame, FAILURE);

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);
    if (sd) {
        hdr = (AVDetectionBBoxHeader *)sd->data;
        hdr_cnt = hdr->nb_bboxes;
    } else {
        draw_info->length = 0;
        return 0;
    }
    // draw_info->length = s->roi_info->box_num;
    draw_info->length = hdr_cnt;
    draw_info->drawInfo = malloc(sizeof(DrawtextDrawInfo) * draw_info->length);
    memset(draw_info->drawInfo, 0, sizeof(DrawtextDrawInfo) * draw_info->length);
    for (int32_t i = 0; i < draw_info->length; i++)
    {
        DrawtextRectangleInfo *rectangle_info = &draw_info->drawInfo[i].rectangleInfo;
        DrawtextTextInfoList *text_info = &draw_info->drawInfo[i].textinfo;
        Drawtext *text = NULL;
        AVDetectionBBox *bbox = NULL;
        char item_text[128];
        float confidence = 0.0;

        bbox = av_get_detection_bbox(hdr, i);

        // init rectangle info
        rectangle_info->color = s->rectangle_color;
        rectangle_info->thickness = s->rectangle_thickness;
        rectangle_info->rect.x = bbox->x;
        rectangle_info->rect.y = bbox->y;
        rectangle_info->rect.width = bbox->w;
        rectangle_info->rect.height = bbox->h;

        //init text info
        text_info->length = 1;
        text_info->drawTextInfo = (DrawtextTextInfo *)malloc(sizeof(DrawtextTextInfo) * text_info->length);
        text_info->drawTextInfo[0].bgColor = s->bg_color;
        text_info->drawTextInfo[0].fontColor = s->font_color;
        text_info->drawTextInfo[0].fontWidth = s->font_width;
        text_info->drawTextInfo[0].fontHeight = s->font_height;
        text_info->drawTextInfo[0].posX = rectangle_info->rect.x;
        text_info->drawTextInfo[0].posY = rectangle_info->rect.y;

        text = &text_info->drawTextInfo[0].text;
        text->length = 1;
        text->txt = (DrawLine *)malloc(sizeof(DrawLine) * text->length);

        confidence = (float)bbox->detect_confidence.num/bbox->detect_confidence.den;
        snprintf(item_text, 128, "%s, %.2f", bbox->detect_label, confidence);
        char *title[3] = {item_text, 0, 0};
        for (uint32_t t = 0; t < text->length; t++)
        {
            DrawLine *line_info = &text->txt[t];
            line_info->length = strlen(title[t]);
            line_info->line = (char *)malloc(sizeof(char) * line_info->length);
            mpp_buffer_memcpy(line_info->line, title[t], line_info->length);
        }
    }
    return 0;
}

static ES_S32 esmpp_drawtext_init_draw_both(AVFilterContext *ctx, DrawtextDrawInfoList *draw_info)
{
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;

    draw_info->length = 8;
    draw_info->drawInfo = malloc(sizeof(DrawtextDrawInfo) * draw_info->length);
    memset(draw_info->drawInfo, 0, sizeof(DrawtextDrawInfo) * draw_info->length);
    for (int32_t i = 0; i < draw_info->length; i++)
    {
        DrawtextRectangleInfo *rectangle_info = &draw_info->drawInfo[i].rectangleInfo;
        DrawtextTextInfoList *text_info = &draw_info->drawInfo[i].textinfo;
        Drawtext *text = NULL;
        char item_text[128];

        // init rectangle info
        rectangle_info->color = s->rectangle_color;
        rectangle_info->thickness = s->rectangle_thickness;
        rectangle_info->rect.x = 0 + 50 * i;
        rectangle_info->rect.y = 0 + 50 * i;
        rectangle_info->rect.width = 300;
        rectangle_info->rect.height = 300;

        //init text info
        text_info->length = 1;
        text_info->drawTextInfo = (DrawtextTextInfo *)malloc(sizeof(DrawtextTextInfo) * text_info->length);
        text_info->drawTextInfo[0].bgColor = s->bg_color;
        text_info->drawTextInfo[0].fontColor = s->font_color;
        text_info->drawTextInfo[0].fontWidth = s->font_width;
        text_info->drawTextInfo[0].fontHeight = s->font_height;
        text_info->drawTextInfo[0].posX = rectangle_info->rect.x;
        text_info->drawTextInfo[0].posY = rectangle_info->rect.y;

        text = &text_info->drawTextInfo[0].text;
        text->length = 1;
        text->txt = (DrawLine *)malloc(sizeof(DrawLine) * text->length);

        snprintf(item_text, 128, "%s, %.2f", "person", 0.82);
        char *title[3] = {item_text, 0, 0};
        for (uint32_t t = 0; t < text->length; t++)
        {
            DrawLine *line_info = &text->txt[t];
            line_info->length = strlen(title[t]);
            line_info->line = (char *)malloc(sizeof(char) * line_info->length);
            mpp_buffer_memcpy(line_info->line, title[t], line_info->length);
        }
    }
    return 0;
}

static void esmpp_drawtext_free_draw_info(DrawtextDrawInfoList *draw_info)
{
    if (!draw_info)
        return;
    for (int32_t i = 0; i < draw_info->length; i++)
    {
        DrawtextTextInfoList *text_info = &draw_info->drawInfo[i].textinfo;
        for (uint32_t k = 0; k < text_info->length; k++)
        {
            DrawtextTextInfo *draw_info = &text_info->drawTextInfo[k];

            Drawtext *text = &draw_info->text;
            for (uint32_t t = 0; t < text->length; t++)
            {
                DrawLine *line_info = &text->txt[t];
                free(line_info->line);
            }
            free(text->txt);
        }
    }
    free(draw_info->drawInfo);
}

static int esmpp_drawtext_process_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    int ret = 0;
    MPP_RET mpp_ret = MPP_OK;
    AVFrame *in_src = frame;
    AVFilterLink *link_src = link;
    MppFramePtr src_mpp_frame = NULL;
    MppBufferPtr src_mpp_buf = NULL;
    int stride[4] = {0}, offset[4] = {0};
    size_t in_frame_size = 0;
    uint32_t pic_size = 0;
    DrawtextDrawInfoList draw_info = {0};

    if (link_src->format == AV_PIX_FMT_DRM_PRIME && !in_src->hw_frames_ctx)
    {
        av_log(ctx, AV_LOG_ERROR, "Private format used, input frame must have hardware context.\n");
        ret = FAILURE;
        goto exit1;
    }
    // handle src mpp frame
    if (mpp_frame_init(&src_mpp_frame) != MPP_OK)
    {
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
                        0,
                        0);

    in_frame_size = pic_size;

    if (link_src->format == AV_PIX_FMT_DRM_PRIME)
    {
        if (in_src->data[0])
        {
            AVESMPPDRMFrameDescriptor *desc = (AVESMPPDRMFrameDescriptor *)in_src->data[0];
            src_mpp_buf = desc->buffers[0];
            if (!src_mpp_buf)
            {
                av_log(ctx, AV_LOG_ERROR, "src_mpp_buf is NULL\n");
                goto exit1;
            }
            mpp_buffer_inc_ref(src_mpp_buf);
        }
        else
        {
            av_log(ctx, AV_LOG_WARNING, "frame buf is NULL\n");
            goto exit1;
        }

        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        in_frame_size = mpp_buffer_get_size(src_mpp_buf);
    }
    else
    {
        mpp_ret = mpp_buffer_get(s->buf_grp, &src_mpp_buf, in_frame_size);
        if (mpp_ret)
        {
            av_log(ctx, AV_LOG_ERROR, "Get buffer from group with %zu failed: %d.\n", in_src->buf[0]->size, mpp_ret);
            ret = FAILURE;
            goto exit1;
        }

        esmpp_memcpy_host2device(s->in_fmt, in_src, mpp_buffer_get_ptr(src_mpp_buf));
        mpp_frame_set_buffer(src_mpp_frame, src_mpp_buf);
        mpp_frame_set_buf_size(src_mpp_frame, in_frame_size);
    }

    if(s->mode == 0) {
        ret = esmpp_drawtext_init_draw_info_from_roi(ctx, &draw_info, frame);
        if(ret) {
            av_log(ctx, AV_LOG_ERROR, "esmpp_drawtext_init_draw_info_from_roi failed: %d.\n", ret);
            goto exit1;
        }
    }

    if(s->mode == 1) {
        ret = esmpp_drawtext_init_draw_both(ctx, &draw_info);
        if(ret) {
            av_log(ctx, AV_LOG_ERROR, "esmpp_drawtext_init_draw_both failed: %d.\n", ret);
            goto exit1;
        }
    }

    if (draw_info.length > 0)
    {
        mpp_ret = esmpp_process_drawtext_frame_sync(s->draw_mctx,
                                                    src_mpp_frame,
                                                    (void *)&draw_info,
                                                    0,
                                                    s->hw_type == HWTYPE_CPU ? DRAWTEXT_CPU : DRAWTEXT_TDE,
                                                    DRAWTEXT_USAGE_DRAW_ALL);
        if (mpp_ret != SUCCESS)
        {
            av_log(ctx, AV_LOG_ERROR, "esmpp_process_drawtext_frame_sync failed: %d.\n", mpp_ret);
            ret = FAILURE;
            goto exit1;
        }
        if (frame->format != AV_PIX_FMT_DRM_PRIME)
        {
            esmpp_memcpy_device2host(s->in_fmt, frame, mpp_buffer_get_ptr(src_mpp_buf));
        }
    }
    else
    {
        av_log(ctx, AV_LOG_ERROR, "no info to draw!\n");
    }
#if ESMPP_DRAWTEXT_DUMP
    static count = 0;
    write_buffer_to_file(mpp_buffer_get_ptr(src_mpp_buf),
                         in_frame_size,
                         NULL,
                         frame->width,
                         frame->height,
                         s->in_fmt,
                         count++);
#endif

exit1:
    if (src_mpp_buf)
    {
        mpp_buffer_put(src_mpp_buf);
    }
    if (src_mpp_frame)
    {
        mpp_frame_deinit(&src_mpp_frame);
    }
    esmpp_drawtext_free_draw_info(&draw_info);

    return ret;
}

static int esmpp_drawtext_filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    MppDrawTextContext *s = (MppDrawTextContext *)ctx->priv;
    // AVFrameSideData *sd;

    // sd = av_frame_get_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);

    esmpp_drawtext_process_frame(link, frame);

    s->frame_cnt++;

    return ff_filter_frame(ctx->outputs[0], frame);
}

static const AVOption options[] = {
    {"hw_type",
     "set hardware type",
     OFFSET(hw_type),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     HWTYPE_TDE,
     INT_MAX,
     FLAGS,
     "hw_type"},
    {"tde", "Select tde to work", 0, AV_OPT_TYPE_CONST, {.i64 = HWTYPE_TDE}, 0, INT_MAX, FLAGS, "hw_type"},
    {"cpu", "Select cpu to work", 0, AV_OPT_TYPE_CONST, {.i64 = HWTYPE_CPU}, 0, INT_MAX, FLAGS, "hw_type"},
    {"mode", "Set draw text mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 3, FLAGS, "mode"},
    {"f_w", "Set font width", OFFSET(font_width), AV_OPT_TYPE_INT, {.i64 = 10}, 0, INT_MAX, FLAGS, "f_w"},
    {"f_h", "Set font height", OFFSET(font_height), AV_OPT_TYPE_INT, {.i64 = 10}, 0, INT_MAX, FLAGS, "f_h"},
    {"bg_color", "Set background color",
      OFFSET(bg_color), AV_OPT_TYPE_INT, {.i64 = 0xFF00FFFF}, 0, INT64_MAX, FLAGS, "bg_color"},
    {"f_color", "Set font color",
      OFFSET(font_color), AV_OPT_TYPE_INT, {.i64 = 0xFF000000}, 0, INT64_MAX, FLAGS, "f_color"},
    {"ttf_path", "Set set ttf file path", OFFSET(ttf_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS, "ttf_path"},
    {"r_color", "Set rectangle color",
      OFFSET(rectangle_color), AV_OPT_TYPE_INT, {.i64 = 0xFFFF0000}, 0, INT64_MAX, FLAGS, "rectangle_color"},
    {"r_thick", "Set rectangle thickness",
      OFFSET(rectangle_thickness), AV_OPT_TYPE_INT, {.i64 = 2}, 0, INT_MAX, FLAGS, "rectangle_thickness"},
    {NULL},
};

static const AVClass esmpp_drawtext_class = {
    .class_name = "esmpp_drawtext",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad esmpp_drawtext_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = esmpp_drawtext_filter_frame,
        .config_props = esmpp_drawtext_config_props,
    },
};

AVFilter ff_vf_drawtext_esmpp = {
    .name = "esmpp_drawtext",
    .description = NULL_IF_CONFIG_SMALL("eswin esmpp drawtext filter"),
    .init = esmpp_drawtext_init,
    .uninit = esmpp_drawtext_uninit,
    .priv_size = sizeof(MppDrawTextContext),
    .priv_class = &esmpp_drawtext_class,
    FILTER_INPUTS(esmpp_drawtext_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC(esmpp_drawtext_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags = AVFILTER_FLAG_HWDEVICE,
};