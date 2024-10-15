#include "config.h"
#include "config_components.h"
#include <stdbool.h>

#include "esmppdec.h"
#include "esmpp_comm.h"
#include <mpp_frame.h>
#include <mpp_vdec_cfg.h>

#define ESMPP_DEC_WAIT_TIME 10000  // us

typedef struct {
    enum AVPixelFormat pixfmt;
    uint32_t drm_format;
    MppFrameFormat mppfmt;
} PixToFmt;

/* FIXME : FILLME */
static const PixToFmt pixtofmttable[] = {{AV_PIX_FMT_NV12, DRM_FORMAT_NV12, MPP_FMT_NV12},
                                         {AV_PIX_FMT_NV21, DRM_FORMAT_NV21, MPP_FMT_NV21},
                                         {AV_PIX_FMT_YUV420P, DRM_FORMAT_YUV420, MPP_FMT_I420},
                                         {AV_PIX_FMT_GRAY8, DRM_FORMAT_C8, MPP_FMT_GRAY8},
                                         {AV_PIX_FMT_P010LE, DRM_FORMAT_P010, MPP_FMT_P010},
                                         {AV_PIX_FMT_RGB24, DRM_FORMAT_RGB888, MPP_FMT_R8G8B8},
                                         {AV_PIX_FMT_BGR24, DRM_FORMAT_BGR888, MPP_FMT_B8G8R8},
                                         {AV_PIX_FMT_BGRA, DRM_FORMAT_BGRA8888, MPP_FMT_B8G8R8A8},
                                         {AV_PIX_FMT_RGBA, DRM_FORMAT_RGBA8888, MPP_FMT_R8G8B8A8},
                                         {AV_PIX_FMT_BGR0, DRM_FORMAT_BGRX8888, MPP_FMT_B8G8R8X8},
                                         {AV_PIX_FMT_RGB0, DRM_FORMAT_RGBX8888, MPP_FMT_R8G8B8X8}};

static MppFrameFormat ffmpeg_pixfmt_to_mpp_fmt(enum AVPixelFormat pixfmt) {
    for (int i = 0; i < sizeof(pixtofmttable); i++) {
        if (pixtofmttable[i].pixfmt == pixfmt) {
            return pixtofmttable[i].mppfmt;
        }
    }

    return MPP_FMT_BUTT;
}

static enum AVPixelFormat mpp_fmt_to_ffmpeg_pixfmt(MppFrameFormat mppfmt) {
    for (int i = 0; i < sizeof(pixtofmttable); i++) {
        if (pixtofmttable[i].mppfmt == mppfmt) {
            return pixtofmttable[i].pixfmt;
        }
    }

    return AV_PIX_FMT_NONE;
}

static uint32_t mpp_fmt_to_drm_format(MppFrameFormat mpp_fmt) {
    for (int i = 0; i < sizeof(pixtofmttable); i++) {
        if (pixtofmttable[i].mppfmt == mpp_fmt) {
            return pixtofmttable[i].drm_format;
        }
    }

    return AV_PIX_FMT_NONE;
}

static MppCodingType mpp_decode_get_coding_type(AVCodecContext *avctx) {
    switch (avctx->codec_id) {
        case AV_CODEC_ID_H264:
            return MPP_VIDEO_CodingAVC;
        case AV_CODEC_ID_HEVC:
            return MPP_VIDEO_CodingHEVC;
        case AV_CODEC_ID_MJPEG:
            return MPP_VIDEO_CodingMJPEG;
        default:
            return MPP_VIDEO_CodingUnused;
    }
}

static av_cold int mpp_decode_close(AVCodecContext *avctx) {
    ESMPPDecContext *r = avctx->priv_data;

    r->eof = 0;
    r->draining = 0;
    r->info_change = 0;
    r->errinfo_cnt = 0;

    if (r->mctx) {
        esmpp_reset(r->mctx);
        esmpp_close(r->mctx);
        esmpp_destroy(r->mctx);
        r->mctx = NULL;
    }

    if (r->hwframe) {
        av_buffer_unref(&r->hwframe);
    }
    if (r->hwdevice) {
        av_buffer_unref(&r->hwdevice);
    }

    if (r->buf_mode == ESMPP_DEC_PURE_EXTERNAL) {
        av_log(NULL, AV_LOG_INFO, "close esmpp put buf_group: %p\n", r->buf_group);
        mpp_buffer_group_put(r->buf_group);
        r->buf_group = NULL;
    }

    return 0;
}

static int mpp_decode_get_crop(ESMPPDecContext *mpp_dec_ctx) {
    if (!mpp_dec_ctx || !mpp_dec_ctx->crop) {
        return -1;
    }

    if (sscanf(mpp_dec_ctx->crop,
               "%dx%dx%dx%d",
               &mpp_dec_ctx->crop_xoffset,
               &mpp_dec_ctx->crop_yoffset,
               &mpp_dec_ctx->crop_width,
               &mpp_dec_ctx->crop_height)
        != 4) {
        return -1;
    }

    return 0;
}

static int mpp_decode_get_scale(ESMPPDecContext *mpp_dec_ctx) {
    if (!mpp_dec_ctx || !mpp_dec_ctx->scale) {
        return -1;
    }

    if (strchr(mpp_dec_ctx->scale, ':')) {
        if (sscanf(mpp_dec_ctx->scale, "%d:%d", &mpp_dec_ctx->scale_width, &mpp_dec_ctx->scale_height) != 2) {
            return -1;
        }
    } else {
        mpp_dec_ctx->scale_width = mpp_dec_ctx->scale_height = atoi(mpp_dec_ctx->scale);
    }

    return 0;
}

static int mpp_decode_set_config(AVCodecContext *avctx) {
    ES_S32 ret;
    ES_S32 extra_hw_frames = 4;
    MppDecCfgPtr dec_cfg;
    MppFrameFormat mppfmt;
    ESMPPDecContext *r = avctx->priv_data;

    if ((ret = mpp_dec_cfg_init(&dec_cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init mpp_dec_cfg_ret: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        return ret;
    } else {
        av_log(avctx, AV_LOG_INFO, "success to init mpp_dec_cfg_ret: %d\n", ret);
    }

    ret = esmpp_control(r->mctx, MPP_DEC_GET_CFG, dec_cfg);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "get dec cfg failed ret: %d\n", ret);
        mpp_dec_cfg_deinit(&dec_cfg);
        return ret;
    } else {
        av_log(avctx, AV_LOG_INFO, "get dec cfg success ret: %d\n", ret);
    }

    if (avctx->extra_hw_frames > 0) {
        extra_hw_frames = avctx->extra_hw_frames;
    }
    mpp_dec_cfg_set_s32(dec_cfg, "extra_hw_frames", extra_hw_frames);

    mppfmt = ffmpeg_pixfmt_to_mpp_fmt(r->output_fmt);
    if (mppfmt != MPP_FMT_BUTT) {
        mpp_dec_cfg_set_s32(dec_cfg, "output_fmt", mppfmt);
    }
    if (r->stride_align) {
        mpp_dec_cfg_set_s32(dec_cfg, "stride_align", r->stride_align);
    }

    // crop
    if (!mpp_decode_get_crop(r)) {
        mpp_dec_cfg_set_s32(dec_cfg, "crop_xoffset", r->crop_xoffset);
        mpp_dec_cfg_set_s32(dec_cfg, "crop_yoffset", r->crop_yoffset);
        mpp_dec_cfg_set_s32(dec_cfg, "crop_width", r->crop_width);
        mpp_dec_cfg_set_s32(dec_cfg, "crop_height", r->crop_height);
    }

    // scale
    if (!mpp_decode_get_scale(r)) {
        mpp_dec_cfg_set_s32(dec_cfg, "scale_width", r->scale_width);
        mpp_dec_cfg_set_s32(dec_cfg, "scale_height", r->scale_height);
    }

    ret = esmpp_control(r->mctx, MPP_DEC_SET_CFG, dec_cfg);
    if (ret != MPP_OK) {
        av_log(avctx,
               AV_LOG_ERROR,
               "mpp dec cfg failed extra_hw_frames: %d, mppfmt: %d\n",
               avctx->extra_hw_frames,
               mppfmt);
    } else {
        av_log(avctx,
               AV_LOG_INFO,
               "mpp dec cfg success extra_hw_frames: %d, mppfmt: %d\n",
               avctx->extra_hw_frames,
               mppfmt);
    }
    mpp_dec_cfg_deinit(&dec_cfg);

    return ret;
}

static av_cold int mpp_decode_init(AVCodecContext *avctx) {
    ESMPPDecContext *r = avctx->priv_data;
    MppCodingType coding_type = MPP_VIDEO_CodingUnused;
    const char *opts_env = NULL;
    int ret;

    esmpp_set_log_level();

    opts_env = getenv("FFMPEG_ESMPP_DEC_OPT");
    if (opts_env && av_set_options_string(r, opts_env, "=", " ") <= 0) {
        av_log(avctx, AV_LOG_WARNING, "Unable to set decoder options from env opts_env: %s\n", opts_env);
    }

    av_log(avctx,
           AV_LOG_INFO,
           "opts_env: %s, avctx->pix_fmt: %d, buf_mod: %d, buf_cache_mode: %d\n",
           opts_env,
           avctx->pix_fmt,
           r->buf_mode,
           r->buf_cache_mode);

    if ((coding_type = mpp_decode_get_coding_type(avctx)) == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec id: %d\n", avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    if ((ret = esmpp_create(&r->mctx, MPP_CTX_DEC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_init(r->mctx)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init esmpp ret %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ret = mpp_decode_set_config(avctx);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_WARNING, "mpp_decode_set_config failed\n");
        goto fail;
    }

    if ((ret = esmpp_open(r->mctx)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to esmpp_open: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    } else {
        avctx->sw_pix_fmt = r->output_fmt;
    }

    r->first_packet = 1;
    return 0;

fail:
    mpp_decode_close(avctx);
    return ret;
}

static int mpp_decode_set_buffer_group(AVCodecContext *avctx, enum AVPixelFormat pix_fmt, MppFramePtr mpp_frame) {
    int ret;
    int group_limit;
    ESMPPDecContext *r = avctx->priv_data;
    AVHWFramesContext *hwfc = NULL;
    AVESMPPFramesContext *mppfc;

    if (!r->hwdevice) {
        return AVERROR(ENOMEM);
    }

    av_buffer_unref(&r->hwframe);

    r->hwframe = av_hwframe_ctx_alloc(r->hwdevice);
    if (!r->hwframe) {
        return AVERROR(ENOMEM);
    }

    hwfc = (AVHWFramesContext *)r->hwframe->data;
    hwfc->format = AV_PIX_FMT_DRM_PRIME;
    hwfc->sw_format = pix_fmt;
    hwfc->width = mpp_frame_get_width(mpp_frame);
    hwfc->height = mpp_frame_get_height(mpp_frame);
    group_limit = mpp_frame_get_group_buf_count(mpp_frame);

    mppfc = hwfc->hwctx;
    if (r->buf_mode != ESMPP_DEC_PURE_EXTERNAL) {
        if ((ret = av_hwframe_ctx_init(r->hwframe)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init ESMPP frame pool\n");
            goto fail;
        }
        r->buf_group = mppfc->buf_group;
        goto attach;
    } else {
        mppfc->buf_size = mpp_frame_get_buf_size(mpp_frame);
    }

    hwfc->initial_pool_size = group_limit;
    if ((ret = av_hwframe_ctx_init(r->hwframe)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init ESMPP frame pool\n");
        goto fail;
    }

    if (r->buf_group) {
        if ((ret = mpp_buffer_group_clear(r->buf_group)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to clear external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        if ((ret = mpp_buffer_group_get_external(&r->buf_group, MPP_BUFFER_TYPE_DMA_HEAP)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    for (int i = 0; i < hwfc->initial_pool_size; i++) {
        AVESMPPFramesContext *esmpp_fc = hwfc->hwctx;
        MppBufferInfo buf_info = {
            .index = i,
            .type = MPP_BUFFER_TYPE_DMA_HEAP,
            .ptr = mpp_buffer_get_ptr(esmpp_fc->frames[i].buffers[0]),
            .fd = esmpp_fc->frames[i].drm_desc.objects[0].fd,
            .size = esmpp_fc->frames[i].drm_desc.objects[0].size,
        };

        if ((ret = mpp_buffer_commit(r->buf_group, &buf_info)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to commit external buffer group: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }
    av_log(avctx, AV_LOG_INFO, "buf_mode is ext initial_pool_size: %d\n", hwfc->initial_pool_size);

attach:
    if ((ret = esmpp_control(r->mctx, MPP_DEC_SET_EXT_BUF_GROUP, r->buf_group)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to attach external buffer group: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (r->buf_mode == ESMPP_DEC_HALF_INTERNAL) {
        if ((ret = mpp_buffer_group_limit_config(r->buf_group, 0, group_limit)) != MPP_OK)
            av_log(avctx, AV_LOG_WARNING, "Failed to set buffer group limit: %d\n", ret);
    }

    return 0;

fail:
    if (r->buf_group) {
        mpp_buffer_group_put(r->buf_group);
        r->buf_group = NULL;
    }
    av_buffer_unref(&r->hwframe);
    return ret;
}

static void mpp_decode_free_mpp_buffer(void *opaque, uint8_t *data) {
    MppBufferPtr mpp_buffer = (MppBufferPtr)opaque;
    mpp_buffer_put(mpp_buffer);

    av_log(NULL, AV_LOG_DEBUG, "free_mpp_buffer mpp_buffer: %p\n", mpp_buffer);
}

static void esmpp_free_drm_desc(void *opaque, uint8_t *data) {
    AVESMPPDRMFrameDescriptor *drm_desc = (AVESMPPDRMFrameDescriptor *)opaque;
    av_free(drm_desc);
}

static int frame_create_buf(
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

static bool has_mastering_display_primaries(const MppFrameMasteringDisplayMetadata *mpp_mastering) {
    int i;
    for (i = 0; i < 3; i++) {
        if (mpp_mastering->display_primaries[i][0] || mpp_mastering->display_primaries[i][1]) {
            return true;
        }
    }
    if (mpp_mastering->white_point[0] || mpp_mastering->white_point[1]) {
        return true;
    }

    return false;
}

static bool has_mastering_display_luminance(const MppFrameMasteringDisplayMetadata *mpp_mastering) {
    if (mpp_mastering->max_luminance || mpp_mastering->min_luminance) {
        return true;
    }
    return false;
}

static int esmpp_export_mastering_display(AVCodecContext *avctx,
                                          AVFrame *frame,
                                          MppFrameMasteringDisplayMetadata mpp_mastering) {
    AVMasteringDisplayMetadata *mastering = NULL;
    AVFrameSideData *sd = NULL;
    bool has_primaries = has_mastering_display_primaries(&mpp_mastering);
    bool has_luminance = has_mastering_display_luminance(&mpp_mastering);

    if (!has_primaries || !has_luminance) {
        return 0;
    }

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd)
        mastering = (AVMasteringDisplayMetadata *)sd->data;
    else
        mastering = av_mastering_display_metadata_create_side_data(frame);
    if (!mastering) return AVERROR(ENOMEM);

    if (has_primaries) {
        int chroma_den = 50000;
        int i;
        int mapping[3] = {0, 1, 2};
        switch (avctx->codec_id) {
            case AV_CODEC_ID_HEVC:
                // HEVC uses a g,b,r ordering, which we convert to a more natural r,g,b
                mapping[0] = 2;
                mapping[1] = 0;
                mapping[2] = 1;
                break;
            case AV_CODEC_ID_H264:
                break;
            default:
                return 0;
        }

        for (i = 0; i < 3; i++) {
            const int j = mapping[i];
            mastering->display_primaries[i][0] = av_make_q(mpp_mastering.display_primaries[j][0], chroma_den);
            mastering->display_primaries[i][1] = av_make_q(mpp_mastering.display_primaries[j][1], chroma_den);
        }
        mastering->white_point[0] = av_make_q(mpp_mastering.white_point[0], chroma_den);
        mastering->white_point[1] = av_make_q(mpp_mastering.white_point[1], chroma_den);
        mastering->has_primaries = 1;
    }

    if (has_luminance) {
        int max_luma_den = 10000;
        int min_luma_den = 10000;
        mastering->max_luminance = av_make_q(mpp_mastering.max_luminance, max_luma_den);
        mastering->min_luminance = av_make_q(mpp_mastering.min_luminance, min_luma_den);
        mastering->has_luminance = 1;
    }

    return 0;
}

static int esmpp_export_content_light(AVFrame *frame, MppFrameContentLightMetadata mpp_light) {
    AVContentLightMetadata *light = NULL;
    AVFrameSideData *sd = NULL;

    if (!mpp_light.MaxCLL && !mpp_light.MaxFALL) {
        return 0;
    }
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd)
        light = (AVContentLightMetadata *)sd->data;
    else
        light = av_content_light_metadata_create_side_data(frame);
    if (!light) return AVERROR(ENOMEM);

    light->MaxCLL = mpp_light.MaxCLL;
    light->MaxFALL = mpp_light.MaxFALL;

    return 0;
}

static int mpp_buffer_export_frame(AVFrame *frame, MppFramePtr mpp_frame) {
    int ret;
    int nb_planes, *offset, *stride;
    MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    AVESMPPDRMFrameDescriptor *desc = NULL;
    AVDRMLayerDescriptor *layer = NULL;
    MppBufferPtr mpp_buf = NULL;

    mpp_buf = mpp_frame_get_buffer(mpp_frame);
    if (!mpp_buf) {
        av_log(NULL, AV_LOG_WARNING, "mpp_decode_export_frame mpp_buf is null\n");
        return AVERROR(EAGAIN);
    }

    nb_planes = mpp_frame_get_nb_planes(mpp_frame);
    if (nb_planes <= 0) {
        av_log(NULL, AV_LOG_WARNING, "mpp_decode_export_frame nb_planes <= 0\n");
        return AVERROR(EAGAIN);
    }
    offset = mpp_frame_get_offset(mpp_frame);
    stride = mpp_frame_get_stride(mpp_frame);
    if (!offset || !stride) {
        av_log(NULL, AV_LOG_WARNING, "mpp_decode_export_frame !offset || !stride\n");
        return AVERROR(EAGAIN);
    }

    desc = av_mallocz(sizeof(*desc));
    if (!desc) {
        return AVERROR(ENOMEM);
    }
    desc->drm_desc.nb_objects = 1;
    desc->buffers[0] = mpp_buf;

    mpp_buffer_inc_ref(mpp_buf);

    desc->drm_desc.objects[0].fd = mpp_buffer_get_fd(mpp_buf);
    desc->drm_desc.objects[0].size = mpp_buffer_get_size(mpp_buf);

    desc->drm_desc.nb_layers = 1;
    layer = &desc->drm_desc.layers[0];
    layer->planes[0].object_index = 0;

    mpp_fmt = mpp_frame_get_fmt(mpp_frame);
    layer->format = mpp_fmt_to_drm_format(mpp_fmt);
    layer->nb_planes = nb_planes;
    for (int i = 0; i < nb_planes; i++) {
        layer->planes[i].object_index = 0;
        layer->planes[i].offset = offset[i];
        layer->planes[i].pitch = stride[i];
        frame->linesize[i] = stride[i];
    }

    ret =
        frame_create_buf(frame, mpp_buf, sizeof(mpp_buf), mpp_decode_free_mpp_buffer, mpp_buf, AV_BUFFER_FLAG_READONLY);
    if (ret < 0) {
        return ret;
    }

    ret = frame_create_buf(frame, (uint8_t *)desc, sizeof(*desc), esmpp_free_drm_desc, desc, AV_BUFFER_FLAG_READONLY);
    if (ret < 0) {
        return ret;
    }

    frame->data[0] = (uint8_t *)desc;

    return 0;
}

static int mpp_decode_export_frame(AVCodecContext *avctx, AVFrame *frame, MppFramePtr mpp_frame) {
    int ret = 0;
    ESMPPDecContext *r = avctx->priv_data;
    MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    enum AVPixelFormat actual_pixfmt;

    if (!frame || !mpp_frame) {
        av_log(NULL, AV_LOG_ERROR, "mpp_decode_export_frame !frame || !mpp_frame\n");
        return AVERROR(ENOMEM);
    }

    mpp_fmt = mpp_frame_get_fmt(mpp_frame);
    // for some special stream, the decoder can not convert to target format
    // report error, eg stream is yuv4:0:0, but dec_pixfmt=yuv420p
    actual_pixfmt = mpp_fmt_to_ffmpeg_pixfmt(mpp_fmt);
    if ((avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) && (actual_pixfmt != avctx->pix_fmt)) {
        av_log(avctx,
               AV_LOG_ERROR,
               "stream actual pixfmt: %s can not output pix_fmt: %s, check -dec_pixfmt\n",
               av_get_pix_fmt_name(actual_pixfmt),
               av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR(EINVAL);
    }

    frame->hw_frames_ctx = av_buffer_ref(r->hwframe);
    if (!frame->hw_frames_ctx) {
        return AVERROR(ENOMEM);
    }

    if ((ret = ff_decode_frame_props(avctx, frame)) < 0) {
        av_log(avctx, AV_LOG_WARNING, "ff_decode_frame_props failed\n");
        return ret;
    }

    frame->format = AV_PIX_FMT_DRM_PRIME;
    frame->width = mpp_frame_get_width(mpp_frame);
    frame->height = mpp_frame_get_height(mpp_frame);
    frame->pts = mpp_frame_get_pts(mpp_frame);
    frame->pkt_dts = mpp_frame_get_dts(mpp_frame);
    frame->time_base = avctx->pkt_timebase;
#if 0  // for vlc master
    if (avctx->flags & AV_CODEC_FLAG_COPY_OPAQUE) {
        frame->opaque_ref = (AVBufferRef *)mpp_frame_get_reordered_opaque(mpp_frame);
    }
#endif
    if ((avctx->codec_id == AV_CODEC_ID_HEVC || avctx->codec_id == AV_CODEC_ID_H264)
        && (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
        ret = esmpp_export_mastering_display(avctx, frame, mpp_frame_get_mastering_display(mpp_frame));
        if (ret < 0) return ret;
        ret = esmpp_export_content_light(frame, mpp_frame_get_content_light(mpp_frame));
        if (ret < 0) return ret;
    }

    if (!frame->data[0]) {
        ret = mpp_buffer_export_frame(frame, mpp_frame);
    }

    if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
        if (r->buf_cache_mode == ESMPP_DEC_DMA_CACHE) {
            MppBufferPtr mpp_buf = mpp_frame_get_buffer(mpp_frame);
            if (mpp_buf) {
                mpp_buffer_sync_begin(mpp_buf);
            }
        }
    }

    return ret;
}

static int mpp_decode_get_frame(AVCodecContext *avctx, AVFrame *frame, int timeout) {
    int ret;
    ESMPPDecContext *r = avctx->priv_data;
    MppFramePtr mpp_frame = NULL;
    AVFrame *tmp_frame = NULL;
    enum AVPixelFormat pix_fmts[3] = {AV_PIX_FMT_DRM_PRIME, AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};

    if (r->eof) {
        return AVERROR_EOF;
    }

    ret = esmpp_get_frame(r->mctx, &mpp_frame, timeout);
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get frame: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    if (!mpp_frame) {
        if (timeout != MPP_TIMEOUT_NON_BLOCK) {
            av_log(avctx, AV_LOG_DEBUG, "Timeout getting decoded frame\n");
        }
        return AVERROR(EAGAIN);
    }
    av_log(avctx, AV_LOG_DEBUG, "Received mpp_frame: %p\n", mpp_frame);
    if (mpp_frame_get_eos(mpp_frame)) {
        av_log(avctx, AV_LOG_INFO, "Received a 'EOS' frame\n");
        /* EOS frame may contain valid data */
        if (!mpp_frame_get_buffer(mpp_frame)) {
            r->eof = 1;
            ret = AVERROR_EOF;
            goto exit;
        }
    }

    ret = mpp_frame_get_errinfo(mpp_frame);
    if (ret == MPP_ERR_UNSUPPORT) {
        av_log(avctx, AV_LOG_ERROR, "Video sequence frame size decoder not supported\n");
        ret = AVERROR_EXIT;
        goto exit;
    } else if (ret) {
        av_log(avctx, AV_LOG_DEBUG, "Received a 'errinfo' frame, err_code:%d\n", ret);
        ret = (r->errinfo_cnt++ > MAX_ERRINFO_COUNT) ? AVERROR_EXTERNAL : AVERROR(EAGAIN);
        goto exit;
    }

    if (r->info_change = mpp_frame_get_info_change(mpp_frame)) {
        char *opts = NULL;
        enum AVPixelFormat frame_fmt;
        const MppFrameFormat mpp_fmt = mpp_frame_get_fmt(mpp_frame);

        av_log(avctx, AV_LOG_VERBOSE, "Noticed an info change\n");

        frame_fmt = mpp_fmt_to_ffmpeg_pixfmt(mpp_fmt & MPP_FRAME_FMT_MASK);
        if (frame_fmt == AV_PIX_FMT_NONE) {
            av_log(avctx, AV_LOG_ERROR, "error frame mpp_fmt fmt: %d\n", mpp_fmt);
            goto exit;
        }

        avctx->coded_width = mpp_frame_get_width(mpp_frame);
        avctx->coded_height = mpp_frame_get_height(mpp_frame);
        if (!avctx->width) {
            avctx->width = avctx->coded_width;
        }
        if (!avctx->height) {
            avctx->height = avctx->coded_height;
        }

        if (r->dfd) {
            avctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
        }

        pix_fmts[1] = frame_fmt;
        if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
            if ((ret = ff_get_format(avctx, pix_fmts)) < 0) {
                av_log(avctx,
                       AV_LOG_ERROR,
                       "ff_get_format failed: %d, pixfmt: %s\n",
                       ret,
                       av_get_pix_fmt_name(r->output_fmt));
                return ret;
            }
            avctx->pix_fmt = ret;
        } else {
            avctx->sw_pix_fmt = frame_fmt;
        }
        av_log(avctx,
               AV_LOG_INFO,
               "info change, mpp_fmt: %d, frame_fmt: %d, pix_fmt: %d\n",
               mpp_fmt,
               frame_fmt,
               avctx->pix_fmt);

        if (r->hwdevice) {
            av_buffer_unref(&r->hwdevice);
        }
        if (avctx->hw_device_ctx) {
            r->hwdevice = av_buffer_ref(avctx->hw_device_ctx);
            if (!r->hwdevice) {
                ret = AVERROR(ENOMEM);
                goto exit;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Picked up an existing ESMPP hardware device\n");
        } else {
            int flags = MPP_BUFFER_TYPE_DMA_HEAP;
            if (avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) {
                if (r->buf_cache_mode == ESMPP_DEC_DMA_CACHE) {
                    flags |= MPP_BUFFER_FLAGS_CACHABLE;
                }
            }
            if ((ret = av_hwdevice_ctx_create(&r->hwdevice, AV_HWDEVICE_TYPE_ESMPP, "esmpp", NULL, flags)) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to create a ESMPP hardware device: %d\n", ret);
                goto exit;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Created a ESMPP hardware device\n");
        }

        if (av_opt_serialize(r, 0, 0, &opts, '=', ' ') >= 0) {
            av_log(avctx, AV_LOG_VERBOSE, "Decoder options: %s\n", opts);
            av_free(opts);
        }
        av_opt_free(r);

        av_log(avctx,
               AV_LOG_VERBOSE,
               "Configured with size: %dx%d | pix_fmt: %s | sw_pix_fmt: %s\n",
               avctx->width,
               avctx->height,
               av_get_pix_fmt_name(avctx->pix_fmt),
               av_get_pix_fmt_name(avctx->sw_pix_fmt));

        if ((ret = mpp_decode_set_buffer_group(avctx, frame_fmt, mpp_frame)) < 0) {
            goto exit;
        }

        if ((ret = esmpp_control(r->mctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set info change ready: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto exit;
        }

        // for info change frame only, should notify uplayer eagain.
        ret = AVERROR(EAGAIN);
        goto exit;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Received a frame\n");
        r->errinfo_cnt = 0;
        if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
            if (avctx->get_buffer2 && avctx->get_buffer2 != avcodec_default_get_buffer2) {
                ff_get_buffer(avctx, frame, 0);
                if (frame->data[3]) {
                    AVFrame *tmp = (AVFrame *)frame->data[3];
                    mpp_buffer_export_frame(tmp, mpp_frame);
                } else {
                    av_frame_unref(frame);
                }
            }
            ret = mpp_decode_export_frame(avctx, frame, mpp_frame);
            if (ret < 0) {
                goto exit;
            }
        } else {
            tmp_frame = av_frame_alloc();
            if (!tmp_frame) {
                ret = AVERROR(ENOMEM);
                goto exit;
            }
            if ((ret = mpp_decode_export_frame(avctx, tmp_frame, mpp_frame)) < 0) {
                goto exit;
            }
            frame->format = avctx->pix_fmt;
            frame->width = tmp_frame->width;
            frame->height = tmp_frame->height;
            if (avctx->get_buffer2 == avcodec_default_get_buffer2) {
                ret = av_hwframe_map(frame, tmp_frame, AV_HWFRAME_MAP_READ);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "av_hwframe_map failed: %d\n", ret);
                }
            } else {
                if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed: %d\n", ret);
                    goto exit;
                }
                if ((ret = av_hwframe_transfer_data(frame, tmp_frame, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed: %d\n", ret);
                    goto exit;
                }
                if ((ret = av_frame_copy_props(frame, tmp_frame)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
                    goto exit;
                }
            }
        }
    }

exit:
    if (mpp_frame) {
        mpp_frame_deinit(&mpp_frame);
    }
    if (tmp_frame) {
        av_frame_free(&tmp_frame);
    }
    return ret;
}

static int mpp_decode_send_eos(AVCodecContext *avctx) {
    ESMPPDecContext *r = avctx->priv_data;
    MppPacketPtr mpp_pkt = NULL;
    int ret;

    if ((ret = mpp_packet_init(&mpp_pkt, NULL, 0)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init 'EOS' packet: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    mpp_packet_set_eos(mpp_pkt);

    ret = esmpp_put_packet(r->mctx, mpp_pkt);
    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_WARNING, "send eos failed ret: %d\n", ret);
    } else {
        av_log(avctx, AV_LOG_WARNING, "send eos success\n");
    }
    r->draining = 1;

    mpp_packet_deinit(&mpp_pkt);
    return 0;
}

static int mpp_decode_send_packet(AVCodecContext *avctx, AVPacket *pkt) {
    int ret;
    ESMPPDecContext *r = avctx->priv_data;
    MppPacketPtr mpp_pkt = NULL;

    /* avoid sending new data after EOS */
    if (r->draining) {
        return AVERROR(EOF);
    }

    // first packet, get extradata
    if ((avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) && r->first_packet) {
        if (avctx->extradata_size) {
            if ((ret = mpp_packet_init(&mpp_pkt, avctx->extradata, avctx->extradata_size)) != MPP_OK) {
                av_log(avctx, AV_LOG_ERROR, "Failed to init packet: %d\n", ret);
                return AVERROR_EXTERNAL;
            }
            mpp_packet_set_pts(mpp_pkt, pkt->pts);
            mpp_packet_set_dts(mpp_pkt, pkt->dts);

            if ((ret = esmpp_put_packet(r->mctx, mpp_pkt)) != MPP_OK) {
                av_log(avctx, AV_LOG_DEBUG, "Decoder buffer is full\n");
                ret = AVERROR(EAGAIN);
            } else {
                av_log(avctx, AV_LOG_DEBUG, "Wrote extradata %d bytes to decoder\n", avctx->extradata_size);
            }
        }
        r->first_packet = 0;
    }

    if ((ret = mpp_packet_init(&mpp_pkt, pkt->data, pkt->size)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init packet: %d\n", ret);
        return AVERROR_EXTERNAL;
    }
    mpp_packet_set_pts(mpp_pkt, pkt->pts);
    mpp_packet_set_dts(mpp_pkt, pkt->dts);
#if 0  // for vlc master
    if (avctx->flags & AV_CODEC_FLAG_COPY_OPAQUE) {
        /*tmp_opaque_ref will be released by av_frame_free*/
        AVBufferRef *tmp_opaque_ref = av_buffer_ref(pkt->opaque_ref);
        mpp_packet_set_reordered_opaque(mpp_pkt, (ES_S64)tmp_opaque_ref);
    }
#endif
    if ((ret = esmpp_put_packet(r->mctx, mpp_pkt)) != MPP_OK) {
        av_log(avctx, AV_LOG_DEBUG, "Decoder buffer is full\n");
        ret = AVERROR(EAGAIN);
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Wrote %d bytes to decoder\n", pkt->size);
    }
    mpp_packet_deinit(&mpp_pkt);

    return ret;
}

static int mpp_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    ESMPPDecContext *r = avctx->priv_data;
    AVPacket *pkt = &r->last_pkt;
    int ret = 0;

    if (r->info_change && !r->buf_group) {
        return AVERROR_EOF;
    }

    /* no more frames after EOS */
    if (r->eof) {
        return AVERROR_EOF;
    }

    /* drain remain frames */
    if (r->draining) {
        ret = mpp_decode_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
        goto exit;
    }

    while (1) {
        if (!pkt->size) {
            ret = ff_decode_get_packet(avctx, pkt);
            if (ret == AVERROR_EOF) {
                av_log(avctx, AV_LOG_INFO, "Decoder is at EOF\n");
                /* send EOS and start draining */
                mpp_decode_send_eos(avctx);
                // may return for info change or ffmpeg will close
                do {
                    ret = mpp_decode_get_frame(avctx, frame, MPP_TIMEOUT_BLOCK);
                } while (ret == AVERROR(EAGAIN));
                goto exit;
            } else if (ret == AVERROR(EAGAIN)) {
                /* not blocking so that we can feed data ASAP */
                ret = mpp_decode_get_frame(avctx, frame, MPP_TIMEOUT_NON_BLOCK);
                goto exit;
            } else if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Decoder failed to get packet: %d\n", ret);
                goto exit;
            }
        }

        /* send pending data to decoder */
        ret = mpp_decode_send_packet(avctx, pkt);
        if (ret == AVERROR(EAGAIN)) {
            /* some streams might need more packets to start returning frames */
            ret = mpp_decode_get_frame(avctx, frame, ESMPP_DEC_WAIT_TIME);
            if (ret != AVERROR(EAGAIN)) {
                goto exit;
            }
        } else if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Decoder failed to send packet: %d\n", ret);
            goto exit;
        } else {
            if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
                // it is workaround for jpeg as it must wait for info change
                ret = mpp_decode_get_frame(avctx, frame, ESMPP_DEC_WAIT_TIME);
            } else {
                ret = mpp_decode_get_frame(avctx, frame, MPP_TIMEOUT_NON_BLOCK);
            }

            av_packet_unref(pkt);
            pkt->size = 0;
            goto exit;
        }
    }

exit:
    if (r->draining && ret == AVERROR(EAGAIN)) {
        ret = AVERROR_EOF;
    }
    return ret;
}

static void mpp_decode_flush(AVCodecContext *avctx) {
    ESMPPDecContext *r = avctx->priv_data;
    int ret;

    av_log(avctx, AV_LOG_INFO, "Decoder flushing\n");

    if ((ret = esmpp_reset(r->mctx)) == MPP_OK) {
        r->eof = 0;
        r->draining = 0;
        r->info_change = 0;
        r->errinfo_cnt = 0;
        av_packet_unref(&r->last_pkt);
    } else
        av_log(avctx, AV_LOG_ERROR, "Failed to reset MPP context: %d\n", ret);
}

DEFINE_ESMPP_DECODER(h264, H264, "h264_mp4toannexb")
DEFINE_ESMPP_DECODER(hevc, HEVC, "hevc_mp4toannexb")
DEFINE_ESMPP_DECODER(mjpeg, MJPEG, "mjpeg2jpeg")
