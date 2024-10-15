#include "esmppenc.h"
#include <es_venc_def.h>
#include <es_mpp_video.h>

#include "libavutil/imgutils.h"
#include "libavutil/mastering_display_metadata.h"

static MppCodingType esmpp_get_coding_type(AVCodecContext *avctx) {
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

static MppFrameFormat esmpp_get_mpp_fmt(enum AVPixelFormat pix_fmt) {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            return MPP_FMT_I420;
        case AV_PIX_FMT_NV12:
            return MPP_FMT_NV12;
        case AV_PIX_FMT_NV21:
            return MPP_FMT_NV21;
        case AV_PIX_FMT_YUYV422:
            return MPP_FMT_YUY2;
        case AV_PIX_FMT_UYVY422:
            return MPP_FMT_UYVY;
        case AV_PIX_FMT_YUV420P10LE:
            return MPP_FMT_I010;
        case AV_PIX_FMT_P010LE:
            return MPP_FMT_P010;
        // MPP_FMT_YV12 TODO: how about this one?
        default:
            return MPP_FMT_BUTT;
    }
}

static unsigned get_used_frame_count(ESMPPEncFrame *list) {
    unsigned count = 0;

    while (list) {
        if (list->queued == 1 && (list->frame || list->mpp_frame)) {
            ++count;
        }
        list = list->next;
    }

    return count;
}

static void clear_unused_frames(ESMPPEncFrame *list) {
    while (list) {
        if (list->queued == 1) {
            MppFramePtr mpp_frame = list->mpp_frame;
            MppBufferPtr mpp_buf = NULL;

            if (mpp_frame) {
                mpp_buf = mpp_frame_get_buffer(mpp_frame);
            }

            if (mpp_buf && mpp_buffer_get_index(mpp_buf) < 0) {
                mpp_buffer_put(mpp_buf);

                mpp_frame_deinit(&list->mpp_frame);
                list->mpp_frame = NULL;

                av_frame_free(&list->frame);
                list->queued = 0;
            }
        }
        list = list->next;
    }
}

static void clear_frame_list(ESMPPEncFrame **list) {
    while (*list) {
        ESMPPEncFrame *frame = NULL;
        MppFramePtr mpp_frame = NULL;
        MppBufferPtr mpp_buf = NULL;

        frame = *list;
        *list = (*list)->next;

        mpp_frame = frame->mpp_frame;
        if (mpp_frame) {
            mpp_buf = mpp_frame_get_buffer(mpp_frame);
            if (mpp_buf && mpp_buffer_get_index(mpp_buf) >= 0) {
                mpp_buffer_put(mpp_buf);
            }

            mpp_frame_deinit(&frame->mpp_frame);
            frame->mpp_frame = NULL;
        }

        av_frame_free(&frame->frame);
        av_freep(&frame);
    }
}

static ESMPPEncFrame *get_free_frame(ESMPPEncFrame **list) {
    ESMPPEncFrame *out = *list;

    for (; out; out = out->next) {
        if (!out->queued) {
            out->queued = 1;
            break;
        }
    }

    if (!out) {
        out = av_mallocz(sizeof(*out));
        if (!out) {
            av_log(NULL, AV_LOG_ERROR, "Cannot alloc new output frame\n");
            return NULL;
        }
        out->queued = 1;
        out->next = *list;
        *list = out;
    }

    return out;
}

#define CFG_SET_U16(cfg, cfgstr, value)                                            \
    do {                                                                           \
        if (mpp_enc_cfg_set_u16(cfg, cfgstr, value)) {                             \
            av_log(NULL, AV_LOG_ERROR, "%s is set to %u failed\n", cfgstr, value); \
            return -1;                                                             \
        }                                                                          \
        av_log(NULL, AV_LOG_INFO, "%s is set to %u\n", cfgstr, value);             \
    } while (0)

#define CFG_SET_S32(cfg, cfgstr, value)                                            \
    do {                                                                           \
        if (mpp_enc_cfg_set_s32(cfg, cfgstr, value)) {                             \
            av_log(NULL, AV_LOG_ERROR, "%s is set to %d failed\n", cfgstr, value); \
            return -1;                                                             \
        }                                                                          \
        av_log(NULL, AV_LOG_INFO, "%s is set to %d\n", cfgstr, value);             \
    } while (0)

#define CFG_SET_U32(cfg, cfgstr, value)                                            \
    do {                                                                           \
        if (mpp_enc_cfg_set_u32(cfg, cfgstr, value)) {                             \
            av_log(NULL, AV_LOG_ERROR, "%s is set to %u failed\n", cfgstr, value); \
            return -1;                                                             \
        }                                                                          \
        av_log(NULL, AV_LOG_INFO, "%s is set to %u\n", cfgstr, value);             \
    } while (0)

#define CFG_SET_S32_IF_USER_SET(cfg, cfgstr, value, unset_value)                       \
    do {                                                                               \
        if (value != unset_value) {                                                    \
            if (mpp_enc_cfg_set_s32(cfg, cfgstr, value)) {                             \
                av_log(NULL, AV_LOG_ERROR, "%s is set to %d failed\n", cfgstr, value); \
                return -1;                                                             \
            }                                                                          \
            av_log(NULL, AV_LOG_INFO, "%s is set to %d\n", cfgstr, value);             \
        }                                                                              \
    } while (0)

#define CFG_SET_U32_IF_USER_SET(cfg, cfgstr, value, unset_value)                       \
    do {                                                                               \
        if (value != unset_value) {                                                    \
            if (mpp_enc_cfg_set_u32(cfg, cfgstr, value)) {                             \
                av_log(NULL, AV_LOG_ERROR, "%s is set to %u failed\n", cfgstr, value); \
                return -1;                                                             \
            }                                                                          \
            av_log(NULL, AV_LOG_INFO, "%s is set to %u\n", cfgstr, value);             \
        }                                                                              \
    } while (0)

static int encoder_set_venc(MppEncCfgPtr cfg, AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;
    uint32_t plane = 0, offset[3] = {0}, stride[3] = {0};

    CFG_SET_U32(cfg, "venc:pixel_format", esmpp_get_mpp_fmt(cxt->pix_fmt));
    CFG_SET_S32(cfg, "venc:width", avctx->width);
    CFG_SET_S32(cfg, "venc:height", avctx->height);
    CFG_SET_S32_IF_USER_SET(cfg, "venc:align", cxt->stride_align, -1);
    esmpp_get_picbufinfo(cxt->pix_fmt,
                         avctx->width,
                         avctx->height,
                         cxt->stride_align > 0 ? cxt->stride_align : 1,
                         cxt->v_stride_align > 0 ? cxt->stride_align : 1,
                         stride,
                         offset,
                         &plane);

    CFG_SET_S32(cfg, "venc:hor_stride", stride[0]);
    CFG_SET_S32(cfg, "venc:ver_stride", FFALIGN(avctx->height, cxt->v_stride_align > 0 ? cxt->v_stride_align : 1));

    if (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) {
        avctx->profile = cxt->profile;
        avctx->level = cxt->level;
        CFG_SET_S32(cfg, "venc:profile", avctx->profile);
        CFG_SET_S32(cfg, "venc:level", avctx->level);
        if (avctx->codec_id == AV_CODEC_ID_HEVC) {
            CFG_SET_S32_IF_USER_SET(cfg, "venc:tier", cxt->tier, -1);
        }
        if (cxt->bitdepth == -1) {
            if (cxt->pix_fmt == AV_PIX_FMT_YUV420P10LE || cxt->pix_fmt == AV_PIX_FMT_P010LE) {
                cxt->bitdepth = BIT_DEPTH_10BIT;
            } else {
                cxt->bitdepth = BIT_DEPTH_8BIT;
            }
        }
        CFG_SET_S32(cfg, "venc:bit_depth", cxt->bitdepth);
    }

    return 0;
}

// only for h264/hevc
static int encoder_set_venc_gop(MppEncCfgPtr cfg, AVCodecContext *avctx) {
    if (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) {
        ESMPPEncContext *cxt = avctx->priv_data;

        CFG_SET_S32_IF_USER_SET(cfg, "gop:gop_mode", cxt->gop_mode, VENC_GOPMODE_BUTT);
        switch (cxt->gop_mode) {
            case VENC_GOPMODE_NORMALP: {
                CFG_SET_S32(cfg, "normalp:ip_qp_delta", cxt->ip_qp_delta);
                break;
            }
            case VENC_GOPMODE_DUALREF: {
                CFG_SET_S32(cfg, "dualp:sb_interval", cxt->sb_interval);
                CFG_SET_S32(cfg, "dualp:sp_qp_delta", cxt->sp_qp_delta);
                CFG_SET_S32(cfg, "dualp:ip_qp_delta", cxt->ip_qp_delta);
                break;
            }
            case VENC_GOPMODE_SMARTREF: {
                CFG_SET_S32(
                    cfg, "smart:bg_interval", (cxt->bg_interval > 1) ? cxt->bg_interval : FFMAX(avctx->gop_size, 1));
                CFG_SET_S32(cfg, "smart:bg_qp_delta", cxt->bg_qp_delta);
                CFG_SET_S32(cfg, "smart:vi_qp_delta", cxt->vi_qp_delta);
                break;
            }
            case VENC_GOPMODE_ADVSMARTREF: {
                CFG_SET_S32(cfg,
                            "advance:bg_interval",
                            (cxt->bg_interval > 1) ? cxt->bg_interval : FFMAX(avctx->gop_size, 1) + 1);
                CFG_SET_S32(cfg, "advance:bg_qp_delta", cxt->bg_qp_delta);
                CFG_SET_S32(cfg, "advance:vi_qp_delta", cxt->vi_qp_delta);
                break;
            }
            case VENC_GOPMODE_BIPREDB: {
                CFG_SET_S32(cfg, "bipredb:b_frm_num", cxt->b_frm_num);
                CFG_SET_S32(cfg, "bipredb:b_qp_delta", cxt->b_qp_delta);
                CFG_SET_S32(cfg, "bipredb:ip_qp_delta", cxt->ip_qp_delta);
                break;
            }
            case VENC_GOPMODE_LOWDELAYB: {
                CFG_SET_S32(cfg, "lowdelayb:b_frm_num", cxt->b_frm_num);
                CFG_SET_S32(cfg, "lowdelayb:i_qp_delta", cxt->i_qp_delta);
                break;
            }
            default:
                av_log(avctx, AV_LOG_ERROR, "gop_mode is set to %d\n", cxt->gop_mode);
                break;
        }
    }

    return 0;
}

static VENC_RC_MODE_E encoder_get_venc_rc_mode(int rc_mode, enum AVCodecID codec_id) {
    VENC_RC_MODE_E mode = VENC_RC_MODE_BUTT;
    switch (rc_mode) {
        case 0: {
            if (codec_id == AV_CODEC_ID_H264) {
                mode = VENC_RC_MODE_H264CBR;
            } else if (codec_id == AV_CODEC_ID_H265) {
                mode = VENC_RC_MODE_H265CBR;
            } else if (codec_id == AV_CODEC_ID_MJPEG) {
                mode = VENC_RC_MODE_MJPEGCBR;
            }
            break;
        }
        case 1: {
            if (codec_id == AV_CODEC_ID_H264) {
                mode = VENC_RC_MODE_H264VBR;
            } else if (codec_id == AV_CODEC_ID_H265) {
                mode = VENC_RC_MODE_H265VBR;
            } else if (codec_id == AV_CODEC_ID_MJPEG) {
                mode = VENC_RC_MODE_MJPEGVBR;
            }
            break;
        }
        case 2: {
            if (codec_id == AV_CODEC_ID_H264) {
                mode = VENC_RC_MODE_H264FIXQP;
            } else if (codec_id == AV_CODEC_ID_H265) {
                mode = VENC_RC_MODE_H265FIXQP;
            } else if (codec_id == AV_CODEC_ID_MJPEG) {
                mode = VENC_RC_MODE_MJPEGFIXQP;
            }
            break;
        }
        case 3: {
            if (codec_id == AV_CODEC_ID_H264) {
                mode = VENC_RC_MODE_H264QPMAP;
            } else if (codec_id == AV_CODEC_ID_H265) {
                mode = VENC_RC_MODE_H265QPMAP;
            }
            break;
        }
        default:
            break;
    }

    return mode;
}

static unsigned int encoder_get_framerate(AVRational av_framerate) {
    unsigned int framerate_den, framerate_num, framerate;
    if (av_framerate.den <= 0 || av_framerate.num <= 0) {
        return -1;
    }
    framerate_den = (unsigned int)av_framerate.den;  // high 16 bits
    framerate_num = (unsigned int)av_framerate.num;  // low 16 bits

    if (framerate_den > 1) {
        framerate_den = framerate_den << 16;
        framerate = framerate_den | framerate_num;
    } else {
        framerate = framerate_num / framerate_den;
    }

    av_log(NULL, AV_LOG_DEBUG, "get_framerate num:%d den:%d rate:%u\n", av_framerate.num, av_framerate.den, framerate);
    return framerate;
}

static int encoder_set_venc_rc(MppEncCfgPtr cfg, AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;
    VENC_RC_MODE_E rc_mode;
    unsigned int bitrate;

    rc_mode = encoder_get_venc_rc_mode(cxt->rc_mode, avctx->codec_id);
    if (rc_mode < VENC_RC_MODE_H264CBR || rc_mode >= VENC_RC_MODE_BUTT) {
        av_log(avctx, AV_LOG_WARNING, "unsupported rc:mode %d\n", rc_mode);
        return -1;
    }
    CFG_SET_S32(cfg, "rc:mode", rc_mode);
    // for get rc releted params
    if (esmpp_control(cxt->mctx, MPP_ENC_SET_CFG, cfg)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config\n");
        return -1;
    }
    if (esmpp_control(cxt->mctx, MPP_ENC_GET_CFG, cfg)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get config\n");
        return -1;
    }

    CFG_SET_U32(cfg, "rc:dst_frame_rate", encoder_get_framerate(avctx->framerate));

    if (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) {
        CFG_SET_U32(cfg, "rc:gop", FFMAX(avctx->gop_size, 1));
    }

    switch (rc_mode) {
        case VENC_RC_MODE_H264CBR:
        case VENC_RC_MODE_H265CBR: {
            bitrate = (unsigned int)(avctx->bit_rate / 1000);
            CFG_SET_U32(cfg, "cbr:bitrate", bitrate);
            if (cxt->cpb_size == -1) {
                cxt->cpb_size = bitrate * 1.25;
            }
            CFG_SET_U32(cfg, "cbr:cpb_size", (unsigned int)cxt->cpb_size);
            CFG_SET_U32(cfg, "rc:stat_time", cxt->stat_time);

            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:iprop", cxt->iprop, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:max_qp", cxt->qp_max, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:min_qp", cxt->qp_min, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:max_iqp", cxt->qp_max_i, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:min_iqp", cxt->qp_min_i, -1);
            break;
        }
        case VENC_RC_MODE_H264VBR:
        case VENC_RC_MODE_H265VBR: {
            bitrate = (unsigned int)(avctx->bit_rate / 1000);
            CFG_SET_U32(cfg, "vbr:max_bitrate", bitrate);
            CFG_SET_U32(cfg, "rc:stat_time", cxt->stat_time);

            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:iprop", cxt->iprop, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:max_qp", cxt->qp_max, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:min_qp", cxt->qp_min, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:max_iqp", cxt->qp_max_i, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:min_iqp", cxt->qp_min_i, -1);
            break;
        }
        case VENC_RC_MODE_H264FIXQP:
        case VENC_RC_MODE_H265FIXQP: {
            CFG_SET_U32(cfg, "fixqp:iqp", cxt->iqp);
            CFG_SET_U32(cfg, "fixqp:pqp", cxt->pqp);
            CFG_SET_U32(cfg, "fixqp:bqp", cxt->bqp);
            break;
        }
        case VENC_RC_MODE_MJPEGCBR: {
            bitrate = (unsigned int)(avctx->bit_rate / 1000);
            CFG_SET_U32(cfg, "cbr:bitrate", bitrate);
            CFG_SET_U32(cfg, "rc:stat_time", cxt->stat_time);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:max_qfactor", cxt->qfactor_max, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "cbr_adv:min_qfactor", cxt->qfactor_min, -1);
            break;
        }
        case VENC_RC_MODE_MJPEGVBR: {
            bitrate = (unsigned int)(avctx->bit_rate / 1000);
            CFG_SET_U32(cfg, "vbr:max_bitrate", bitrate);
            CFG_SET_U32(cfg, "rc:stat_time", cxt->stat_time);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:max_qfactor", cxt->qfactor_max, -1);
            CFG_SET_U32_IF_USER_SET(cfg, "vbr_adv:min_qfactor", cxt->qfactor_min, -1);
            break;
        }
        case VENC_RC_MODE_MJPEGFIXQP: {
            CFG_SET_U32_IF_USER_SET(cfg, "fixqp:qfactor", cxt->qfactor, -1);
            break;
        }

        default:
            av_log(avctx, AV_LOG_ERROR, "rc_mode is set to %d\n", cxt->rc_mode);
            return -1;
    }

    return 0;
}

/* parse crop str*/
static int encoder_get_crop(char *str, RECT_S *rect) {
    char *p;

    if (!str || !rect) {
        return -1;
    }
    if (((p = strstr(str, "cx")) == NULL) && ((p = strstr(str, "cy")) == NULL) && ((p = strstr(str, "cw")) == NULL)
        && ((p = strstr(str, "ch")) == NULL)) {
        return 0;
    }

    if ((p = strstr(str, "cx")) != NULL) {
        rect->x = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "cy")) != NULL) {
        rect->y = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "cw")) != NULL) {
        rect->width = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "ch")) != NULL) {
        rect->height = atoi(p + 3);
    } else {
        return -1;
    }

    return 0;
}

static ROTATION_E encoder_get_rotation(int ratation) {
    switch (ratation) {
        case 0:
            return ROTATION_0;
        case 1:
            return ROTATION_90;
        case 2:
            return ROTATION_180;
        case 3:
            return ROTATION_270;
    }
    return ROTATION_BUTT;
}

static int encoder_set_venc_prp(MppEncCfgPtr cfg, AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;

    if (cxt->rotation >= 0) {
        CFG_SET_S32(cfg, "pp:rotation", encoder_get_rotation(cxt->rotation));
    }
    if (cxt->crop_str) {
        RECT_S rect;
        if (encoder_get_crop(cxt->crop_str, &rect)) {
            av_log(avctx, AV_LOG_ERROR, "Crop params error %s\n", cxt->crop_str);
        } else {
            mpp_enc_cfg_set_s32(cfg, "pp:enable", 1);
            if (mpp_enc_cfg_set_st(cfg, "pp:rect", (void *)&rect)) {
                av_log(NULL, AV_LOG_ERROR, "Crop rect is set to %s failed\n", cxt->crop_str);
                return -1;
            }
            av_log(avctx, AV_LOG_INFO, "Crop rect is set to %s\n", cxt->crop_str);
        }
    }

    return 0;
}

/* parse and set mastering_display */
static int encoder_set_venc_mastering_display(MppEncCfgPtr cfg, const char *str) {
    char *p;
    uint32_t x = 0, y = 0;

    if (!str) {
        return 0;
    }
    // R(x,y)G(x,y)B(x,y)WP(x,y)L(x,y)
    if ((p = strstr(str, "R(")) != NULL) {
        if (sscanf(p, "R(%u,%u)", &x, &y) == 2) {
            CFG_SET_U16(cfg, "display:dx0", (uint16_t)x);
            CFG_SET_U16(cfg, "display:dy0", (uint16_t)y);
        }
    }

    if ((p = strstr(str, "G(")) != NULL) {
        if (sscanf(p, "G(%u,%u)", &x, &y) == 2) {
            CFG_SET_U16(cfg, "display:dx1", (uint16_t)x);
            CFG_SET_U16(cfg, "display:dy1", (uint16_t)y);
        }
    }

    if ((p = strstr(str, "B(")) != NULL) {
        if (sscanf(p, "B(%u,%u)", &x, &y) == 2) {
            CFG_SET_U16(cfg, "display:dx2", (uint16_t)x);
            CFG_SET_U16(cfg, "display:dy2", (uint16_t)y);
        }
    }

    if ((p = strstr(str, "WP(")) != NULL) {
        if (sscanf(p, "WP(%u,%u)", &x, &y) == 2) {
            CFG_SET_U16(cfg, "display:white_x", (uint16_t)x);
            CFG_SET_U16(cfg, "display:white_y", (uint16_t)y);
        }
    }
    if ((p = strstr(str, "L(")) != NULL) {
        if (sscanf(p, "L(%u,%u)", &x, &y) == 2) {
            CFG_SET_U32(cfg, "display:min_luminance", x);
            CFG_SET_U32(cfg, "display:max_luminance", y);
        }
    }

    return 0;
}

/* parse and set content_light */
static int encoder_set_venc_content_light(MppEncCfgPtr cfg, const char *str) {
    char *p;

    if (!str) {
        return 0;
    }

    if ((p = strstr(str, "maxcll")) != NULL) {
        CFG_SET_U16(cfg, "display:maxcll", (uint16_t)atoi(p + 7));
    }
    if ((p = strstr(str, "maxfall")) != NULL) {
        CFG_SET_U16(cfg, "display:maxfall", (uint16_t)atoi(p + 8));
    }

    return 0;
}

static int encoder_set_venc_protocal(MppEncCfgPtr cfg, AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;

    if (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) {
        if (cxt->enable_deblocking > -1) {
            CFG_SET_U32(cfg, "dblk:dblk_disable", (cxt->enable_deblocking == 0) ? 1 : 0);
        }

        if (avctx->color_range > AVCOL_RANGE_UNSPECIFIED && avctx->color_range < AVCOL_RANGE_NB) {
            mpp_enc_cfg_set_s32(
                cfg,
                "vui:colorrange",
                (avctx->color_range == AVCOL_RANGE_MPEG) ? MPP_FRAME_RANGE_LIMITED : MPP_FRAME_RANGE_FULL);
        }
        mpp_enc_cfg_set_s32(cfg, "vui:colorspace", avctx->colorspace);
        mpp_enc_cfg_set_s32(cfg, "vui:colorprim", avctx->color_primaries);
        mpp_enc_cfg_set_s32(cfg, "vui:colortrc", avctx->color_trc);

        encoder_set_venc_mastering_display(cfg, cxt->mastering_display);
        encoder_set_venc_content_light(cfg, cxt->content_light);

        if (avctx->codec_id == AV_CODEC_ID_H264) {
            CFG_SET_U32_IF_USER_SET(cfg, "h264:cabac", cxt->enable_cabac, -1);
        }
    }

    return 0;
}

static int esmpp_set_enc_cfg(AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;
    MppEncCfgPtr cfg = cxt->mcfg;
    int ret = MPP_OK;

    if ((ret = esmpp_control(cxt->mctx, MPP_ENC_GET_CFG, cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get encoder config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        return -1;
    }

    // common attrs
    if (encoder_set_venc(cfg, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set_venc config\n");
        return -1;
    }

    // prp attrs
    if (encoder_set_venc_prp(cfg, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set_venc_prp config\n");
        return -1;
    }

    // rc attrs
    if (encoder_set_venc_rc(cfg, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set_venc_rc config\n");
        return -1;
    }

    // gop attrs
    if (encoder_set_venc_gop(cfg, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set_venc_gop config\n");
        return -1;
    }

    // video protocal
    if (encoder_set_venc_protocal(cfg, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set_venc_rc config\n");
        return -1;
    }

    if (esmpp_control(cxt->mctx, MPP_ENC_SET_CFG, cfg)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config\n");
        return -1;
    }
    return 0;
}

static int esmpp_send_frame(AVCodecContext *avctx, ESMPPEncFrame *mpp_enc_frame) {
    ESMPPEncContext *cxt = avctx->priv_data;
    MppFramePtr mpp_frame = NULL;
    int ret;

    if (mpp_enc_frame) {
        mpp_frame = mpp_enc_frame->mpp_frame;
    }

    if ((ret = esmpp_put_frame(cxt->mctx, mpp_frame)) != MPP_OK) {
        int log_level = (ret == MPP_ERR_INPUT_FULL) ? AV_LOG_DEBUG : AV_LOG_ERROR;
        ret = (ret == MPP_ERR_INPUT_FULL) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
        av_log(avctx, log_level, "Failed to put frame to encoder input queue: %d\n", ret);
        goto exit;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "Wrote %ld bytes to encoder\n", mpp_frame ? mpp_frame_get_buf_size(mpp_frame) : 0);
    }

exit:
    return ret;
}

static void esmpp_free_packet_buf(void *opaque, uint8_t *data) {
    MppPacketPtr mpp_pkt = opaque;
    mpp_packet_deinit(&mpp_pkt);
}

static int esmpp_get_packet_internal(AVCodecContext *avctx, AVPacket *packet, int timeout) {
    ESMPPEncContext *cxt = avctx->priv_data;
    MppPacketPtr mpp_pkt = NULL;
    MppMetaPtr mpp_meta = NULL;
    MppFramePtr mpp_frame = NULL;
    MppBufferPtr mpp_buf = NULL;
    int ret;

    if ((ret = esmpp_get_packet(cxt->mctx, &mpp_pkt, timeout)) != MPP_OK) {
        int log_level = (ret == MPP_ERR_TIMEOUT) ? AV_LOG_DEBUG : AV_LOG_ERROR;
        ret = (ret == MPP_ERR_TIMEOUT) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
        av_log(avctx,
               log_level,
               "Failed to get packet from encoder output queue:%s, timeout:%d\n",
               ret == AVERROR(EAGAIN) ? "EAGAIN" : "AVERROR_EXTERNAL",
               timeout);
        return ret;
    }
    if (!mpp_pkt) return AVERROR(ENOMEM);

    if (mpp_packet_get_eos(mpp_pkt)) {
        av_log(avctx, AV_LOG_INFO, "Received an EOS packet\n");
        ret = AVERROR_EOF;
        goto exit;
    }

    packet->data = mpp_packet_get_data(mpp_pkt);
    packet->size = mpp_packet_get_length(mpp_pkt);
    packet->buf = av_buffer_create(packet->data, packet->size, esmpp_free_packet_buf, mpp_pkt, AV_BUFFER_FLAG_READONLY);
    if (!packet->buf) {
        ret = AVERROR(ENOMEM);
        av_log(avctx, AV_LOG_ERROR, "Failed to create av buf, no mem\n");
        goto exit;
    }

    packet->time_base.num = avctx->time_base.num;
    packet->time_base.den = avctx->time_base.den;
    packet->pts = MPP_PTS_TO_PTS(mpp_packet_get_pts(mpp_pkt), avctx->time_base);
    packet->dts = mpp_packet_get_dts(mpp_pkt);

    if (mpp_packet_has_meta(mpp_pkt)) {
        mpp_meta = mpp_packet_get_meta(mpp_pkt);
    }

    if (!mpp_meta) {
        av_log(avctx, AV_LOG_WARNING, "Failed to get packet meta\n");
    } else {
        int key_frame = 0;
        mpp_meta_get_s32(mpp_meta, KEY_OUTPUT_INTRA, &key_frame);
        if (key_frame) {
            packet->flags |= AV_PKT_FLAG_KEY;
        }

        if ((ret = mpp_meta_get_frame(mpp_meta, KEY_INPUT_FRAME, &mpp_frame)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get key input frame from packet meta: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto exit;
        }

        mpp_buf = mpp_frame_get_buffer(mpp_frame);
        if (!mpp_buf) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get buf frame mpp frame: %d\n", ret);
            return AVERROR(ENOMEM);
        }
        /* mark buffer as unused (idx < 0) */
        mpp_buffer_set_index(mpp_buf, -1);
        clear_unused_frames(cxt->frame_list);
    }

    // dump packet
    if (cxt->dump_pkt_enable) {
        int ret = 0;
        ret = esmpp_codec_dump_bytes_to_file(packet->data, packet->size, cxt->dump_pkt_hnd);
        if (ret == ERR_TIMEOUT) {
            av_log(NULL, AV_LOG_INFO, "pkt dump timeout\n");
            esmpp_codec_dump_file_close(&cxt->dump_pkt_hnd);
            cxt->dump_pkt_enable = 0;
            av_log(NULL, AV_LOG_INFO, "closed dump packet handle\n");
        } else if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "write packet into file failed\n");
        }
        // av_log(NULL, AV_LOG_INFO, "dump packet: data %p size %d\n", packet->data, packet->size);
    }

    return 0;
exit:
    if (mpp_pkt) {
        mpp_packet_deinit(&mpp_pkt);
    }
    return ret;
}

static int is_hevc_still_pic_profile(enum AVCodecID codec_id, int profile) {
    return codec_id == AV_CODEC_ID_HEVC && profile == PROFILE_H265_MAIN_STILL_PICTURE;
}

static ESMPPEncFrame *esmpp_submit_frame(AVCodecContext *avctx, AVFrame *frame) {
    ESMPPEncContext *cxt = avctx->priv_data;
    AVFrame *es_frame = NULL;
    MppFramePtr mpp_frame = NULL;
    MppBufferPtr mpp_buf = NULL;
    ESMPPEncFrame *mpp_enc_frame = NULL;
    int ret = 0;
    int hshift, vshift, planes;
    int stride[4] = {0}, offset[4] = {0}, offset_sum = 0;
    uint32_t luma_size = 0, chroma_size = 0, pic_size = 0, i = 0;

    clear_unused_frames(cxt->frame_list);

    if (frame) {
        enum AVPixelFormat pix_fmt = avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME ? avctx->sw_pix_fmt : avctx->pix_fmt;
        av_pix_fmt_get_chroma_sub_sample(pix_fmt, &hshift, &vshift);
        planes = av_pix_fmt_count_planes(pix_fmt);
        for (i = 0; i < planes; i++) {
            pic_size += frame->linesize[i] * (frame->height >> (i ? vshift : 0));
            stride[i] = frame->linesize[i];
            offset[i] = offset_sum;
            offset_sum = pic_size;
        }

        luma_size = frame->linesize[0] * frame->height;
        chroma_size = pic_size - luma_size;
        av_log(avctx,
               AV_LOG_DEBUG,
               "linesize:%d-%d-%d, offset:%d-%d-%d, lumasize %u chromasize %u, pic_size %u, planes:%d\n",
               frame->linesize[0],
               frame->linesize[1],
               frame->linesize[2],
               offset[0],
               offset[1],
               offset[2],
               luma_size,
               chroma_size,
               pic_size,
               planes);
    }

    mpp_enc_frame = get_free_frame(&cxt->frame_list);
    if (!mpp_enc_frame) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get free frame\n");
        return NULL;
    }

    if (!frame
        || (is_hevc_still_pic_profile(avctx->codec_id, cxt->profile)
            && cxt->sent_frm_cnt >= 1)) {  // eos send null data
        av_log(avctx, AV_LOG_DEBUG, "End of stream\n");
        mpp_enc_frame->mpp_frame = mpp_frame;
        return mpp_enc_frame;
    }

    if ((ret = mpp_frame_init(&mpp_frame)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init mpp frame: %d\n", ret);
        goto exit;
    }
    mpp_enc_frame->mpp_frame = mpp_frame;

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        es_frame = frame;
        mpp_enc_frame->frame = av_frame_clone(es_frame);
        if (es_frame->buf[0]) {
            mpp_buf = (MppBufferPtr)es_frame->buf[0]->data;
            if (!mpp_buf) {
                av_log(avctx, AV_LOG_ERROR, "mpp buf is NULL\n");
                return NULL;
            }
            mpp_buffer_inc_ref(mpp_buf);
        } else {
            av_log(avctx, AV_LOG_WARNING, "frame buf is NULL\n");
            return NULL;
        }
    } else {
        void *vir_addr = NULL;
        es_frame = frame;
        mpp_enc_frame->frame = av_frame_clone(es_frame);

        ret = mpp_buffer_get(cxt->frame_grp, &mpp_buf, pic_size);
        if (MPP_OK != ret) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get buf\n");
            return NULL;
        }

        vir_addr = mpp_buffer_get_ptr(mpp_buf);
        if (!vir_addr) {
            mpp_buffer_put(mpp_buf);
            ret = 1;
            av_log(avctx, AV_LOG_ERROR, "Failed to get buf ptr\n");
            return NULL;
        }

        // cp luma
        memcpy(vir_addr, frame->data[0], luma_size);
        if (frame->linesize[1] && frame->linesize[2]) {
            memcpy((uint8_t *)vir_addr + luma_size, frame->data[1], chroma_size / 2);
            memcpy((uint8_t *)vir_addr + luma_size + chroma_size / 2, frame->data[2], chroma_size / 2);
        } else if (frame->linesize[1]) {
            memcpy((uint8_t *)vir_addr + luma_size, frame->data[1], chroma_size);
        }
    }

    mpp_frame_set_stride(mpp_frame, stride);
    mpp_frame_set_offset(mpp_frame, offset);
    mpp_frame_set_pts(mpp_frame, PTS_TO_MPP_PTS(es_frame->pts, avctx->time_base));
    mpp_frame_set_width(mpp_frame, es_frame->width);
    mpp_frame_set_height(mpp_frame, es_frame->height);

    mpp_frame_set_colorspace(mpp_frame, avctx->colorspace);
    mpp_frame_set_color_primaries(mpp_frame, avctx->color_primaries);
    mpp_frame_set_color_trc(mpp_frame, avctx->color_trc);
    if (avctx->color_range > AVCOL_RANGE_UNSPECIFIED && avctx->color_range < AVCOL_RANGE_NB) {
        mpp_frame_set_color_range(
            mpp_frame, (avctx->color_range == AVCOL_RANGE_MPEG) ? MPP_FRAME_RANGE_LIMITED : MPP_FRAME_RANGE_FULL);
    }

    mpp_buffer_set_index(mpp_buf, mpp_buffer_get_fd(mpp_buf));
    mpp_frame_set_buffer(mpp_frame, mpp_buf);

    // dump yuv
    if (cxt->dump_frame_enable) {
        int ret = 0;
        uint32_t luma_size = 0, chroma_size = 0, pic_size = 0;
        esmpp_get_picsize((avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) ? avctx->sw_pix_fmt : avctx->pix_fmt,
                          avctx->width,
                          avctx->height,
                          (cxt->stride_align > 0) ? cxt->stride_align : 1,
                          (cxt->v_stride_align > 0) ? cxt->v_stride_align : 1,
                          &luma_size,
                          &chroma_size,
                          &pic_size);
        if (cxt->dump_frame_hnd) {
            void *paddr = mpp_buffer_get_ptr(mpp_buf);
            ret = esmpp_codec_dump_bytes_to_file(paddr, pic_size, cxt->dump_frame_hnd);
            if (ret == ERR_TIMEOUT) {
                av_log(NULL, AV_LOG_INFO, "frame dump timeout\n");
                esmpp_codec_dump_file_close(&cxt->dump_frame_hnd);
                cxt->dump_frame_enable = 0;
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "write file error\n");
            }
            // av_log(NULL, AV_LOG_INFO, "dump frame: data %p size %d\n", paddr, pic_size);
        } else {
            av_log(NULL, AV_LOG_ERROR, "fp is not inited\n");
        }
    }

    cxt->sent_frm_cnt++;
    return mpp_enc_frame;

exit:
    if (es_frame && avctx->pix_fmt != AV_PIX_FMT_DRM_PRIME) av_frame_free(&es_frame);

    return NULL;
}

static int esmpp_encode_frame(AVCodecContext *avctx, AVPacket *packet, const AVFrame *frame, int *got_packet) {
    ESMPPEncContext *cxt = avctx->priv_data;
    ESMPPEncFrame *mpp_enc_frame = NULL;
    int ret = 0;

    int timeout = (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC
                   || avctx->codec_id == AV_CODEC_ID_MJPEG)
                          && !(avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
                      ? MPP_TIMEOUT_NON_BLOCK
                      : MPP_TIMEOUT_BLOCK;
    if (get_used_frame_count(cxt->frame_list) > cxt->async_frames) {
        av_log(avctx,
               AV_LOG_DEBUG,
               "get_used_frame_count(cxt->frame_list) %d > cxt->async_frames %d\n",
               get_used_frame_count(cxt->frame_list),
               cxt->async_frames);
        goto get_pkt;
    }

    mpp_enc_frame = esmpp_submit_frame(avctx, (AVFrame *)frame);
    if (!mpp_enc_frame) {
        av_log(avctx, AV_LOG_ERROR, "Failed to submit input frame\n");
        return AVERROR(ENOMEM);
    }

send_frm:
    ret = esmpp_send_frame(avctx, mpp_enc_frame);
    av_log(avctx, AV_LOG_INFO, "send_frame ret:%d, sent frm cnt:%d\n", ret, cxt->sent_frm_cnt);
    if (ret == AVERROR(EAGAIN)) {
        av_usleep(10 * 1000);
        goto send_frm;
    } else if (ret) {
        return ret;
    }

get_pkt:
    ret = esmpp_get_packet_internal(avctx, packet, timeout);
    if (ret == AVERROR(EAGAIN)) {
        if (is_hevc_still_pic_profile(avctx->codec_id, cxt->profile)) {
            if (cxt->sent_frm_cnt == 1 && cxt->got_pkt_cnt < 1) {
                av_log(avctx, AV_LOG_INFO, "still pic frame is %p, ret %d\n", frame, ret);
                av_usleep(10 * 1000);
                goto get_pkt;
            }
        } else if (!frame) {  // sent all the frame we must get left packet
            av_log(avctx, AV_LOG_INFO, "frame is %p, ret %d\n", frame, ret);
            av_usleep(10 * 1000);
            goto get_pkt;
        } else if (cxt->sent_frm_cnt - cxt->got_pkt_cnt >= cxt->async_frames) {
            av_log(avctx,
                   AV_LOG_DEBUG,
                   "cxt->sent_frm_cnt %d, cxt->got_pkt_cnt %d\n",
                   cxt->sent_frm_cnt,
                   cxt->got_pkt_cnt);
            // to ensure that not too much bufs are occupied
            av_usleep(10 * 1000);
            goto get_pkt;
        }
    }

    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        *got_packet = 0;
        av_log(avctx,
               AV_LOG_INFO,
               "get_packet len %d ret:%d %s\n",
               packet->size,
               ret,
               ret == AVERROR(EAGAIN) ? "EAGAIN" : "AVERROR_EOF");
    } else if (ret) {
        av_log(avctx, AV_LOG_INFO, "get_packet len %d ret:%d\n", packet->size, ret);
        return ret;
    } else {
        av_log(avctx,
               AV_LOG_INFO,
               "get_packet len %d ret:%d pts:%ld dts:%ld, got pktcnt:%d\n",
               packet->size,
               ret,
               packet->pts,
               packet->dts,
               ++cxt->got_pkt_cnt);
        *got_packet = 1;
    }

    return 0;
}

static int esmpp_encode_close(AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;

    cxt->cfg_init = 0;
    cxt->async_frames = 0;
    if (cxt->mcfg) {
        mpp_enc_cfg_deinit(cxt->mcfg);
    }
    if (cxt->frame_grp) {
        mpp_buffer_group_put(cxt->frame_grp);
    }
    if (cxt->mctx) {
        esmpp_close(cxt->mctx);
        esmpp_deinit(cxt->mctx);
        esmpp_destroy(cxt->mctx);
        cxt->mctx = NULL;
    }

    if (cxt->dump_frame_hnd) esmpp_codec_dump_file_close(&cxt->dump_frame_hnd);
    if (cxt->dump_pkt_hnd) esmpp_codec_dump_file_close(&cxt->dump_pkt_hnd);

    clear_frame_list(&cxt->frame_list);

    return 0;
}

static int esmpp_encode_get_header_packet(AVCodecContext *avctx, MppPacketPtr *mpp_pkt) {
    int ret = 0;
    if (avctx->codec_id == AV_CODEC_ID_H264 || avctx->codec_id == AV_CODEC_ID_HEVC) {
        ESMPPEncContext *cxt = avctx->priv_data;
        MppPacketPtr tmp_pkt;
        ES_U8 enc_hdr_buf[H26X_HEADER_SIZE];
        size_t pkt_len = 0;
        void *pkt_pos = NULL;

        memset(enc_hdr_buf, 0, H26X_HEADER_SIZE);

        ret = mpp_packet_init(&tmp_pkt, (void *)enc_hdr_buf, H26X_HEADER_SIZE);
        if (ret != MPP_OK || !tmp_pkt) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init header info packet: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            return ret;
        }

        *mpp_pkt = tmp_pkt;
        mpp_packet_set_length(tmp_pkt, 0);
        if ((ret = esmpp_control(cxt->mctx, MPP_ENC_GET_HDR_SYNC, tmp_pkt)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get header sync: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            return ret;
        }

        pkt_pos = mpp_packet_get_pos(tmp_pkt);
        pkt_len = mpp_packet_get_length(tmp_pkt);

        if (avctx->extradata) {
            av_free(avctx->extradata);
            avctx->extradata = NULL;
        }
        avctx->extradata = av_malloc(pkt_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            ret = AVERROR(ENOMEM);
            return ret;
        }
        avctx->extradata_size = pkt_len + AV_INPUT_BUFFER_PADDING_SIZE;
        memcpy(avctx->extradata, pkt_pos, pkt_len);
        memset(avctx->extradata + pkt_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        mpp_packet_deinit(&tmp_pkt);

        if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
            return 0;
        }
    }
    return ret;
}

static int handle_mdcv(AVCodecContext *avctx, const AVMasteringDisplayMetadata *mdcv, MppEncCfgPtr cfg) {
    if (!mdcv->has_primaries && !mdcv->has_luminance) return 0;

    if (mdcv->has_primaries) {
        // hevc GBR order, h264 RGB order
        if (avctx->codec_id == AV_CODEC_ID_HEVC) {
            CFG_SET_U16(
                cfg, "display:dx0", (uint16_t)av_rescale_q(1, mdcv->display_primaries[1][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy0", (uint16_t)av_rescale_q(1, mdcv->display_primaries[1][1], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dx1", (uint16_t)av_rescale_q(1, mdcv->display_primaries[2][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy1", (uint16_t)av_rescale_q(1, mdcv->display_primaries[2][1], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dx2", (uint16_t)av_rescale_q(1, mdcv->display_primaries[0][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy2", (uint16_t)av_rescale_q(1, mdcv->display_primaries[0][1], (AVRational){1, 50000}));
        } else {
            CFG_SET_U16(
                cfg, "display:dx0", (uint16_t)av_rescale_q(1, mdcv->display_primaries[0][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy0", (uint16_t)av_rescale_q(1, mdcv->display_primaries[0][1], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dx1", (uint16_t)av_rescale_q(1, mdcv->display_primaries[1][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy1", (uint16_t)av_rescale_q(1, mdcv->display_primaries[1][1], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dx2", (uint16_t)av_rescale_q(1, mdcv->display_primaries[2][0], (AVRational){1, 50000}));
            CFG_SET_U16(
                cfg, "display:dy2", (uint16_t)av_rescale_q(1, mdcv->display_primaries[2][1], (AVRational){1, 50000}));
        }

        CFG_SET_U16(cfg, "display:white_x", (uint16_t)av_rescale_q(1, mdcv->white_point[0], (AVRational){1, 50000}));
        CFG_SET_U16(cfg, "display:white_y", (uint16_t)av_rescale_q(1, mdcv->white_point[1], (AVRational){1, 50000}));
        av_log(avctx,
               AV_LOG_INFO,
               "display primaries %ld-%ld-%ld-%ld-%ld-%ld white_point %ld-%ld",
               av_rescale_q(1, mdcv->display_primaries[0][0], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->display_primaries[0][1], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->display_primaries[1][0], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->display_primaries[1][1], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->display_primaries[2][0], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->display_primaries[2][1], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->white_point[0], (AVRational){1, 50000}),
               av_rescale_q(1, mdcv->white_point[1], (AVRational){1, 50000}));
    }

    if (mdcv->has_luminance) {
        CFG_SET_U32(
            cfg, "display:max_luminance", (uint32_t)av_rescale_q(1, mdcv->max_luminance, (AVRational){1, 10000}));
        CFG_SET_U32(
            cfg, "display:min_luminance", (uint32_t)av_rescale_q(1, mdcv->min_luminance, (AVRational){1, 10000}));
        av_log(avctx,
               AV_LOG_INFO,
               "display luminance %ld-%ld",
               av_rescale_q(1, mdcv->max_luminance, (AVRational){1, 10000}),
               av_rescale_q(1, mdcv->min_luminance, (AVRational){1, 10000}));
    }
    return 0;
}

static int handle_side_data(AVCodecContext *avctx, ESMPPEncContext *params) {
    const AVFrameSideData *cll_sd = av_frame_side_data_get(
        avctx->decoded_side_data, avctx->nb_decoded_side_data, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    const AVFrameSideData *mdcv_sd = av_frame_side_data_get(
        avctx->decoded_side_data, avctx->nb_decoded_side_data, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);

    // prefer to the setting from user
    if ((cll_sd && !params->content_light) || (mdcv_sd && !params->mastering_display)) {
        ESMPPEncContext *cxt = avctx->priv_data;
        MppEncCfgPtr cfg = cxt->mcfg;
        int ret = MPP_OK;

        if ((ret = esmpp_control(cxt->mctx, MPP_ENC_GET_CFG, cfg)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get encoder config: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            return -1;
        }

        if (cll_sd && !params->content_light) {
            const AVContentLightMetadata *cll = (AVContentLightMetadata *)cll_sd->data;
            CFG_SET_U16(cfg, "display:maxcll", cll->MaxCLL);
            CFG_SET_U16(cfg, "display:maxfall", cll->MaxFALL);
        }

        if (mdcv_sd && !params->mastering_display) {
            handle_mdcv(avctx, (AVMasteringDisplayMetadata *)mdcv_sd->data, cfg);
        }

        if (esmpp_control(cxt->mctx, MPP_ENC_SET_CFG, cfg)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set config\n");
            return -1;
        }
    }
    return 0;
}

static int esmpp_encode_init(AVCodecContext *avctx) {
    ESMPPEncContext *cxt = avctx->priv_data;
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    MppCodingType coding_type = MPP_VIDEO_CodingUnused;
    MppPacketPtr mpp_pkt = NULL;
    int ret = 0;

    esmpp_set_log_level();

    cxt->cfg_init = 0;
    cxt->async_frames = 0;
    cxt->sent_frm_cnt = 0;
    cxt->got_pkt_cnt = 0;

    ret = mpp_buffer_group_get_internal(&cxt->frame_grp, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "get frame group failed %d\n", ret);
        return AVERROR(ENOSYS);
    }

    coding_type = esmpp_get_coding_type(avctx);
    if (coding_type == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec id: %d\n", avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    pix_fmt = (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) ? avctx->sw_pix_fmt : avctx->pix_fmt;
    cxt->pix_fmt = pix_fmt;

    if ((ret = esmpp_create(&cxt->mctx, MPP_CTX_ENC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create mpp context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(avctx, AV_LOG_INFO, "esmpp_create success pix_fmt %d, coding_type %d\n", cxt->pix_fmt, coding_type);

    if ((ret = esmpp_init(cxt->mctx)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init mpp context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_enc_cfg_init(&cxt->mcfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init enc config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_set_enc_cfg(avctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set enc config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = handle_side_data(avctx, cxt)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to handle side data: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = esmpp_open(cxt->mctx)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open esmpp: %d\n", ret);
        goto fail;
    }

    cxt->async_frames = (avctx->codec_id == AV_CODEC_ID_MJPEG) ? MJPEG_ASYNC_FRAMES : H26X_ASYNC_FRAMES;

    ret = esmpp_encode_get_header_packet(avctx, &mpp_pkt);
    if (ret != 0) {
        goto fail;
    }

    // dump
    // init packet dump handle
    if (cxt->dump_pkt_enable && !cxt->dump_pkt_hnd) {
        DumpParas paras;
        paras.width = avctx->width;
        paras.height = avctx->height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "venc";
        if (avctx->codec_id == AV_CODEC_ID_H264) {
            paras.suffix_name = "h264";
        } else if (avctx->codec_id == AV_CODEC_ID_H265) {
            paras.suffix_name = "h265";
        }
        paras.fmt = NULL;
        cxt->dump_pkt_hnd = esmpp_codec_dump_file_open(cxt->dump_path, cxt->dump_pkt_time, &paras);
    }
    if (cxt->dump_frame_enable && !cxt->dump_frame_hnd) {
        DumpParas paras;
        paras.width = avctx->width;
        paras.height = avctx->height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "venc";
        paras.suffix_name = "yuv";
        paras.fmt = esmpp_get_fmt_char(avctx->pix_fmt);
        cxt->dump_frame_hnd = esmpp_codec_dump_file_open(cxt->dump_path, cxt->dump_frame_time, &paras);
    }

    return 0;

fail:
    if (mpp_pkt) {
        mpp_packet_deinit(&mpp_pkt);
    }

    esmpp_encode_close(avctx);
    return ret;
}

DEFINE_ESMPP_ENCODER(h264, H264, h26x)
DEFINE_ESMPP_ENCODER(hevc, HEVC, h26x)
DEFINE_ESMPP_ENCODER(mjpeg, MJPEG, mjpeg)
