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
#include "internal.h"
#include "video.h"
#include "vf_esmpp_common.h"
#include "filters.h"
#include "libavutil/esmpp_utils.h"
#include "es_mpp.h"
#include "vf_esmpp_addroi.h"
#include "libavcodec/esmpp_comm.h"
#include "es_roi_api.h"
#include "mpp_drawtext_api.h"
#include "mpp_drawtext_cfg.h"
#include "mpp_drawtext_type.h"
#include "libavutil/detection_bbox.h"

#define OBJECT_DETECT_INTERVAL 3
#define ES_VENC_MAX_ROI_NUM 8

#define OFFSET(x) offsetof(MppAddRoiContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define MAX_INPUT_NB 2

#define ESMPP_COMPLEX_DUMP (0)

static int addroi_query_formats(AVFilterContext *ctx)
{
    av_log(ctx, AV_LOG_DEBUG, "calling addroi_query_formats.\n");
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

static int roi_init_bufgroup(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
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

static int init_mpp_draw(AVFilterContext *ctx)
{
    MppAddRoiContext *s;
    MppDrawtextCfgPtr drawtext_cfg;
    int ret = SUCCESS;

    ES_RETURN_VAL_IF_FAIL(ctx, FAILURE);
    ES_RETURN_VAL_IF_FAIL(ctx->priv, FAILURE);

    s = (MppAddRoiContext *)ctx->priv;
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

static av_cold int addroi_config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];

    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
    int ret = SUCCESS;

    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx)
    {
        AVHWFramesContext *input_hw_frm_ctx = NULL;
        input_hw_frm_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        s->in_fmt = input_hw_frm_ctx->sw_format;
        outlink->format = inlink->format;
    }
    else
    {
        s->in_fmt = inlink->format;
        outlink->format = s->in_fmt;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    get_dev_info(ctx->hw_device_ctx, inlink->hw_frames_ctx, &s->device_idx, &s->die_idx);
    // esmpp_set_dev(s->device_idx, s->die_idx);
    if (!s->buf_grp)
    {
        roi_init_bufgroup(inlink);
    }

    if (inlink->format == AV_PIX_FMT_DRM_PRIME && inlink->hw_frames_ctx)
    {
        av_buffer_unref(&outlink->hw_frames_ctx);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frm_ref);
    }

    ret = es_roi_create(&s->mctx, &s->roi_info, ESCL_ROI_MAX_BOX_COUNT, s->device_idx, s->die_idx);
    if (ret < 0)
    {
        av_log(ctx, AV_LOG_ERROR, "es roi create failed. ret = %d\n", ret);
        return ret;
    }

    if (s->rand_roi_num > 0)
    {
        es_roi_proc_no_model(s->rand_roi_num);
        av_log(ctx, AV_LOG_DEBUG, "es roi not use NPU models\n");
    }

    av_log(ctx, AV_LOG_DEBUG, "es roi init enter\n");
    ret = es_roi_init(s->mctx, s->model_dir, ESCL_ROI_MAX_BOX_COUNT);
    if (MPP_OK != ret)
    {
        av_log(ctx, AV_LOG_ERROR, "es roi init failed. ret = %d\n", ret);
        return ret;
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "es roi init success\n");
        s->roi_inited = true;
    }

    if (s->draw_enable)
    {
        if (!s->ttf_path)
        {
            av_log(ctx, AV_LOG_ERROR, "ttf_path is not set\n");
            return ret;
        }
        ret = init_mpp_draw(ctx);
        if (MPP_OK != ret)
        {
            av_log(ctx, AV_LOG_ERROR, "es draw text init failed. ret = %d\n", ret);
            return ret;
        }
        else
        {
            av_log(ctx, AV_LOG_DEBUG, "es draw text init success\n");
        }
    }

    return ret;
}

static av_cold int esmpp_addroi_init(AVFilterContext *ctx)
{
    MppAddRoiContext *s = NULL;
    int ret = SUCCESS;

    if (!ctx || !ctx->priv)
    {
        return FAILURE;
    }
    av_log(ctx, AV_LOG_INFO, "Initializing esmpp_addroi filter.\n");

    s = (MppAddRoiContext *)ctx->priv;
    s->device_idx = 0;
    s->die_idx = 0;
    s->frame_cnt = 0;
    s->buf_grp = NULL;
    s->max_boxes = ESCL_ROI_MAX_BOX_COUNT;
    s->roi_info = NULL;
    s->roi_number = 0;
    s->roi_inited = false;
    s->draw_init = ES_FALSE;
    s->roi_skip_interval += 1;
    av_log(ctx, AV_LOG_DEBUG, "addroi init qoffset:[num%d/den%d], roi skip interval:%d, model_dir: %s\n",
                               s->qoffset.num, s->qoffset.den, s->roi_skip_interval, s->model_dir);

    mpp_set_log_level(s->mpp_log_level);

    if (s->dump_roi_enable)
    {
        s->dump_roi_fp = esmpp_filter_dump_file_open(s->dump_roi_path, "esmpp_roi_box");
        if (!s->dump_roi_fp)
        {
            s->dump_roi_enable = 0;
        }
    }

    if(s->fifo_path) {
        s->pipe_file = NULL;
        int fifo_fd = open(s->fifo_path, O_WRONLY | O_NONBLOCK);
        if (fifo_fd == -1) {
            av_log(ctx, AV_LOG_ERROR, "Failed to open fifo_path %s, please open pipe reader first.\n", s->fifo_path);
            perror("fopen fifo failed");
            return AVERROR_EXTERNAL;
        }
        s->pipe_file = fdopen(fifo_fd, "w");
        if(s->pipe_file == NULL) {
            close(fifo_fd);
            av_log(ctx, AV_LOG_ERROR, "Failed to open fifo_path %s, please open pipe reader first.\n", s->fifo_path);
            perror("fopen fifo failed");
            return AVERROR_EXTERNAL;
        }
    }

    return ret;
}

static av_cold void esmpp_addroi_uninit(AVFilterContext *ctx)
{
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
    ES_BOOL use_hwaccel = ES_FALSE;

    if (!ctx || !ctx->priv)
    {
        return;
    }
    av_log(ctx, AV_LOG_DEBUG, "es roi uninit\n");
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
    if (s->roi_inited)
    {
        av_log(ctx, AV_LOG_DEBUG, "es roi uninit enter\n");
        es_roi_deinit(s->mctx, s->roi_info);
        av_log(ctx, AV_LOG_DEBUG, "es roi uninit success\n");
        s->roi_inited = false;
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

    if (s->dump_roi_enable)
    {
        esmpp_filter_dump_file_close(s->dump_roi_fp);
    }
    if(s->pipe_file) {
        fclose(s->pipe_file);
        s->pipe_file = NULL;
    }
}

static ES_S32 esmpp_init_draw_info(AVFilterContext *ctx, DrawtextDrawInfoList *draw_info)
{
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;

    draw_info->length = s->roi_info->box_num;
    draw_info->drawInfo = malloc(sizeof(DrawtextDrawInfo) * draw_info->length);
    memset(draw_info->drawInfo, 0, sizeof(DrawtextDrawInfo) * draw_info->length);
    for (int32_t i = 0; i < s->roi_info->box_num; i++)
    {
        DrawtextRectangleInfo *rectangle_info = &draw_info->drawInfo[i].rectangleInfo;
        DrawtextTextInfoList *text_info = &draw_info->drawInfo[i].textinfo;
        MppRoiBoxPos *box = &s->roi_info->box_pos[i];
        // char classId[128];
        // char confidence[128];
        // char name[128];
        char item_text[128];

        // init rectangle info
        rectangle_info->color = s->rectangle_color;
        rectangle_info->thickness = s->rectangle_thickness;
        rectangle_info->rect.x = box->left;
        rectangle_info->rect.y = box->top;
        rectangle_info->rect.width = box->right - box->left;
        rectangle_info->rect.height = box->bottom - box->top;

        //init text info
        text_info->length = 1;
        text_info->drawTextInfo = (DrawtextTextInfo *)malloc(sizeof(DrawtextTextInfo) * text_info->length);
        text_info->drawTextInfo[0].bgColor = s->bg_color;
        text_info->drawTextInfo[0].fontColor = s->font_color;
        text_info->drawTextInfo[0].fontWidth = s->font_width;
        text_info->drawTextInfo[0].fontHeight = s->font_height;
        text_info->drawTextInfo[0].posX = rectangle_info->rect.x;
        text_info->drawTextInfo[0].posY = rectangle_info->rect.y;

        Drawtext *text = &text_info->drawTextInfo[0].text;
        text->length = 1;
        text->txt = (DrawLine *)malloc(sizeof(DrawLine) * text->length);
        av_log(ctx, AV_LOG_DEBUG, "total box:%d, this is:%d, class_id: %d, confidence: %f, name: %s\n",
                                   s->roi_info->box_num, i, box->class_id, box->confidence, box->name);
        // snprintf(classId, 128, "%d", box->class_id);
        // snprintf(confidence, 128, "conf:%.1f", box->confidence);
        // snprintf(name, 128, "name:%s", box->name);
        snprintf(item_text, 128, "%s, %.2f", box->name, box->confidence);
        char *title[3] = {item_text, 0, 0};
        for (uint32_t t = 0; t < text->length; t++)
        {
            DrawLine *line_info = &text->txt[t];
            line_info->length = strlen(title[t]);
            line_info->line = (char *)malloc(sizeof(char) * line_info->length);
            mpp_buffer_memcpy(line_info->line, title[t], line_info->length);
        }
    }
    return 1;
}

static void esmpp_free_draw_info(DrawtextDrawInfoList *draw_info)
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

static int esmpp_addroi_send_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
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
    AVFrameSideData *sd = NULL;

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
                        s->src_rotation,
                        s->src_global_alpha);

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

    if (s->roi_info) {
        s->roi_info->box_num = 0;
    }

    if (0 == (s->frame_cnt % s->roi_skip_interval)) {
        ret = es_roi_proc_frame(s->mctx, src_mpp_frame, s->roi_info);
        if (ret != SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "es_roi_proc_frame failed: %d.\n", ret);
            ret = FAILURE;
            goto exit1;
        }
        if(s->pipe_file) {
            if (s->roi_info->box_num) {
                int64_t cur_date_ms = frame->pts;
                if (s->use_system_timestamp) {
                    cur_date_ms = get_ticks_by_ms();
                }
                fprintf(s->pipe_file, "{\"timestamp\":%ld,\"box_num\":%d,\"object\" : [", cur_date_ms, s->roi_info->box_num);
                for(int box_idx = 0; box_idx < s->roi_info->box_num; box_idx++) {
                    MppRoiBoxPos *t = s->roi_info->box_pos + box_idx;
                    fprintf(s->pipe_file, "{\"class_id\":%d,\"confidence\":%.1f,\"name\":\"%s\",\"topLeftX\":%d,\"topLeftY\":%d,\"bottomRightX\":%d,\"bottomRightY\":%d}", 
                                t->class_id, t->confidence, t->name, t->left, t->top, t->right, t->bottom);
                    if ( box_idx < s->roi_info->box_num - 1) {
                        fprintf(s->pipe_file, ",");
                    }
                }
                fprintf(s->pipe_file, "]}\n");
                fflush(s->pipe_file);
            }
        }
        sd = av_frame_get_side_data(frame, AV_FRAME_DATA_ESMPP_SIDE_DATA);
        if (!sd) {
            sd = av_frame_new_side_data(frame, AV_FRAME_DATA_ESMPP_SIDE_DATA, 0);
        }
    } else {
        av_log(ctx, AV_LOG_DEBUG, "frame_cnt:%d, roi_skip_interval:%d, no need AI inference.\n",
                                  s->frame_cnt, s->roi_skip_interval);
    }

    if (s->draw_enable)
    {
        esmpp_init_draw_info(ctx, &draw_info);
        if (draw_info.length > 0)
        {
            mpp_ret = esmpp_process_drawtext_frame_sync(s->draw_mctx,
                                                        src_mpp_frame,
                                                        (void *)&draw_info,
                                                        0,
                                                        DRAWTEXT_TDE,
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
    }
    // static count = 0;
    // write_buffer_to_file(mpp_buffer_get_ptr(src_mpp_buf),
    //                      in_frame_size,
    //                      NULL,
    //                      frame->width,
    //                      frame->height,
    //                      s->in_fmt,
    //                      count++);

exit1:
    if (src_mpp_buf)
    {
        mpp_buffer_put(src_mpp_buf);
    }
    if (src_mpp_frame)
    {
        mpp_frame_deinit(&src_mpp_frame);
    }
    if (s->draw_enable)
    {
        esmpp_free_draw_info(&draw_info);
    }

    return ret;
}

static int esmpp_add_detection_bbox(AVFilterContext *ctx,
                                    AVFrame *frame,
                                    const MppRoiBoxPos *boxes,
                                    int box_num)
{
    int total_cnt = 0, old_cnt = 0, i = 0;

    if (!box_num || !boxes) {
        av_log(ctx, AV_LOG_DEBUG, "esmpp add dtection bbox no info! box_num:%d\n", box_num);
        return 0;
    }

    AVFrameSideData *old_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);

    AVDetectionBBoxHeader *old_hdr = NULL;
    if (old_sd) {
        old_hdr = (AVDetectionBBoxHeader *)old_sd->data;
        old_cnt = old_hdr->nb_bboxes;
    }

    total_cnt = old_cnt + box_num;
    AVDetectionBBoxHeader *new_hdr =
        av_detection_bbox_create_side_data(frame, total_cnt);
    if (!new_hdr)
        return AVERROR(ENOMEM);

    for (i = 0; i < old_cnt; ++i) {
        AVDetectionBBox *dst = av_get_detection_bbox(new_hdr, i);
        const AVDetectionBBox *src = av_get_detection_bbox(old_hdr, i);
        *dst = *src;
    }

    for (i = 0; i < box_num; ++i) {
        AVDetectionBBox *bbox = av_get_detection_bbox(new_hdr, old_cnt + i);

        int x = boxes[i].left;
        int y = boxes[i].top;
        int w = boxes[i].right - boxes[i].left;
        int h = boxes[i].bottom - boxes[i].top;

        x = x < 0 ? 0 : x;
        x = x > frame->width ? frame->width : x;
        y = y < 0 ? 0 : y;
        y = y > frame->height ? frame->height : y;
        w = w < 0 ? 0 : w;
        w = (w + x) > frame->width ? (frame->width - x) : w;
        h = h < 0 ? 0 : h;
        h = (h + y) > frame->height ? (frame->height - y) : h;

        bbox->x = x;
        bbox->y = y;
        bbox->w = w;
        bbox->h = h;

        snprintf(bbox->detect_label, sizeof(bbox->detect_label), "%s", boxes[i].name);

        bbox->detect_confidence = (AVRational){ (int)(boxes[i].confidence * 100), 100 };
        bbox->classify_count = 0;
        av_log(ctx, AV_LOG_DEBUG,
               "esmpp add detection bbox[%d/%d]: pos(x:%d,y:%d) size(w:%d,h:%d) label:%s confidence:%d/%d\n",
               i, box_num,
               bbox->x, bbox->y,
               bbox->w, bbox->h,
               bbox->detect_label,
               bbox->detect_confidence.num,
               bbox->detect_confidence.den);
    }

    snprintf(new_hdr->source, sizeof(new_hdr->source), "esmpp_addroi");

    return 0;
}

static int esmpp_add_side_data(AVFilterLink *link, AVFrame *frame)
{
    AVFrameSideData *sd = NULL;
    AVFilterContext *ctx = link->dst;
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
    const AVRegionOfInterest *old_roi = NULL;
    AVRegionOfInterest *roi = NULL;
    AVBufferRef *roi_ref = NULL;
    uint32_t roi_size = 0;
    int old_roi_cnt = 0;
    int nb_roi = 0, i = 0;
    int ret = 0;

    if (!s->roi_info || !s->roi_info->box_pos)
    {
        av_log(ctx, AV_LOG_ERROR, "roi info is NULL!\n");
        return 0;
    }

    if (s->roi_info->box_num <= 0)
    {
        av_log(ctx, AV_LOG_DEBUG, "no roi need add, box_num = %d\n", s->roi_info->box_num);
        return 0;
    }

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (sd)
    {
        old_roi = (const AVRegionOfInterest *)sd->data;
        roi_size = old_roi->self_size;
        av_assert0(roi_size && sd->size % roi_size == 0);
        old_roi_cnt = sd->size / roi_size;
        av_log(ctx, AV_LOG_DEBUG, "side data exist, old_roi_cnt = %d\n", old_roi_cnt);
    }

    if (old_roi_cnt >= ES_VENC_MAX_ROI_NUM)
    {
        av_log(ctx, AV_LOG_INFO, "side data exceeds max number, not add any new, old_roi_cnt = %d\n", old_roi_cnt);
        return 0;
    }

    nb_roi = old_roi_cnt + s->roi_info->box_num > ES_VENC_MAX_ROI_NUM ? ES_VENC_MAX_ROI_NUM : old_roi_cnt + s->roi_info->box_num;
    av_log(ctx, AV_LOG_DEBUG, "origin roi num:%d, new box num:%d, maximum box num:%d\n",
           old_roi_cnt, s->roi_info->box_num, ES_VENC_MAX_ROI_NUM);

    roi_ref = av_buffer_alloc(sizeof(AVRegionOfInterest) * nb_roi);
    if (!roi_ref)
    {
        ret = AVERROR(ENOMEM);
        return ret;
    }

    roi = (AVRegionOfInterest *)roi_ref->data;

    if (old_roi)
    {
        for (i = 0; i < old_roi_cnt; ++i)
        {
            old_roi = (const AVRegionOfInterest *)(sd->data + old_roi_cnt * i);
            roi[i] = (AVRegionOfInterest){
                .self_size = sizeof(*roi),
                .top = old_roi->top,
                .bottom = old_roi->bottom,
                .left = old_roi->left,
                .right = old_roi->right,
                .qoffset = old_roi->qoffset,
            };
        }
    }

    if (s->roi_info)
    {
        MppRoiBoxPos *region = s->roi_info->box_pos;
        for (i = old_roi_cnt; i < nb_roi; ++i)
        {
            roi[i] = (AVRegionOfInterest){
                .self_size = sizeof(*roi),
                //TODO TODO
                .top = region[i - old_roi_cnt].top,
                .bottom = region[i - old_roi_cnt].bottom,
                .left = region[i - old_roi_cnt].left,
                .right = region[i - old_roi_cnt].right,
                .qoffset = s->qoffset,
            };
            av_log(ctx, AV_LOG_DEBUG, "frame_pts:%ld, %d roies added, the %d roi: top:%d bottom:%d left:%d right:%d ."
                                      "qoffset->num:%d, qoffset->den:%d\n",
                   frame->pts,
                   nb_roi - old_roi_cnt,
                   i,
                   roi[i].top,
                   roi[i].bottom,
                   roi[i].left,
                   roi[i].right,
                   roi[i].qoffset.num,
                   roi[i].qoffset.den);
        }
    }
    else
    {
        av_log(ctx, AV_LOG_INFO, "roi_info no more roi info added\n");
    }

    if (s->dump_roi_enable && s->dump_roi_fp) {
        for (i = 0; i < nb_roi; ++i) {
            int x = roi[i].left;
            int y = roi[i].top;
            int w = roi[i].right - roi[i].left;
            int h = roi[i].bottom - roi[i].top;

            x = x < 0 ? 0 : x;
            x = x > frame->width ? frame->width : x;
            y = y < 0 ? 0 : y;
            y = y > frame->height ? frame->height : y;
            w = w < 0 ? 0 : w;
            w = (w + x) > frame->width ? (frame->width - x) : w;
            h = h < 0 ? 0 : h;
            h = (h + y) > frame->height ? (frame->height - y) : h;
            fprintf(s->dump_roi_fp, "%d ", s->frame_cnt);
            fprintf(s->dump_roi_fp, "%d ", i);
            fprintf(s->dump_roi_fp, "%d ", x);
            fprintf(s->dump_roi_fp, "%d ", y);
            fprintf(s->dump_roi_fp, "%d ", w);
            fprintf(s->dump_roi_fp, "%d", h);
            fprintf(s->dump_roi_fp, "\n");
        }
    }

    if (sd)
        av_frame_remove_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);

    sd = av_frame_new_side_data_from_buf(frame,
                                         AV_FRAME_DATA_REGIONS_OF_INTEREST,
                                         roi_ref);
    if (!sd)
    {
        av_buffer_unref(&roi_ref);
        ret = AVERROR(ENOMEM);
        return ret;
    }

    return ret;
}

static int esmpp_addroi_filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    MppAddRoiContext *s = (MppAddRoiContext *)ctx->priv;
    int ret = 0;

    ret = esmpp_addroi_send_frame(link, frame);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "esmpp addroi send frame failed, ret = %d\n", ret);
    }

    ret = esmpp_add_side_data(link, frame);
    if ( ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "esmpp addroi add side data failed, ret = %d\n", ret);
    }

    ret = esmpp_add_detection_bbox(ctx, frame, s->roi_info->box_pos, s->roi_info->box_num);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "esmpp addroi add detection bbox failed, ret = %d\n", ret);
    }

    s->frame_cnt++;

    return ff_filter_frame(ctx->outputs[0], frame);
}

static const AVOption options[] = {
    {"qoffset", "Quantisation offset to apply in the region.",
      OFFSET(qoffset), AV_OPT_TYPE_RATIONAL, {.dbl = 0}, -1, +1, FLAGS},
    {"model_dir", "NPU model path",
      OFFSET(model_dir), AV_OPT_TYPE_STRING, {.str = "/usr/local/lib/eswin"}, .flags = FLAGS},
    {"roi_skip_interval", "skip some frames to improve the ROI performance",
      OFFSET(roi_skip_interval), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, .flags = FLAGS},
    {"dump_path", "dump roi box directory",
      OFFSET(dump_roi_path), AV_OPT_TYPE_STRING, {.str = "/home/eswin"}, .flags = FLAGS},
    {"dump_roi_enable", "dump roi box info enabled",
      OFFSET(dump_roi_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    {"rand_roi_num", "use the random roi box rather than models detected",
      OFFSET(rand_roi_num), AV_OPT_TYPE_INT, {.i64 = 0}, 0, ES_VENC_MAX_ROI_NUM, .flags = FLAGS},
    {"log_level", "mpp log level",
      OFFSET(mpp_log_level), AV_OPT_TYPE_INT, {.i64 = 2}, 0, MPP_LOG_SILENT, .flags = FLAGS},
    {"draw_enable", "enable draw text",OFFSET(draw_enable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
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
    {"fifo_path", "fifo_path", OFFSET(fifo_path), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = FLAGS},
    {"use_system_timestamp", "use_system_timestamp", OFFSET(use_system_timestamp), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, .flags = FLAGS},

    {NULL},
};

static const AVClass esmpp_addroi_class = {
    .class_name = "esmpp_addroi",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad esmpp_addroi_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = esmpp_addroi_filter_frame,
        .config_props = addroi_config_props,
    },
};

AVFilter ff_vf_addroi_esmpp = {
    .name = "esmpp_addroi",
    .description = NULL_IF_CONFIG_SMALL("eswin esmpp addroi filter"),
    .init = esmpp_addroi_init,
    .uninit = esmpp_addroi_uninit,
    .priv_size = sizeof(MppAddRoiContext),
    .priv_class = &esmpp_addroi_class,
    FILTER_INPUTS(esmpp_addroi_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC(addroi_query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
    .flags = AVFILTER_FLAG_HWDEVICE,
};