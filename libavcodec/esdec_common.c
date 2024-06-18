#define LOG_TAG "esdec_common"
#include "esdec_common.h"
#include "eslog.h"
#include "es_codec_private.h"

typedef struct {
    enum AVPixelFormat pixfmt;
    enum DecPictureFormat picfmt;
    const char *pixfmt_name;
} AvFmtTopicfmt;

static const AvFmtTopicfmt fmttopicfmttable[] = {
    {AV_PIX_FMT_NV12, DEC_OUT_FRM_YUV420SP, "nv12"},
    {AV_PIX_FMT_NV21, DEC_OUT_FRM_NV21SP, "nv21"},
    {AV_PIX_FMT_YUV420P, DEC_OUT_FRM_YUV420P, "yuv420p"},
    {AV_PIX_FMT_GRAY8, DEC_OUT_FRM_YUV400, "gray"},
    {AV_PIX_FMT_YUV420P10LE, DEC_OUT_FRM_YUV420P_I010, "yuv420p10le"},
    {AV_PIX_FMT_P010LE, DEC_OUT_FRM_YUV420SP_P010, "p010le"},
    {AV_PIX_FMT_RGB24, DEC_OUT_FRM_RGB888, "rgb24"},
    {AV_PIX_FMT_BGR24, DEC_OUT_FRM_BGR888, "bgr24"},
    {AV_PIX_FMT_ARGB, DEC_OUT_FRM_ARGB888, "argb24"},
    {AV_PIX_FMT_ABGR, DEC_OUT_FRM_ABGR888, "abgr24"},
    {AV_PIX_FMT_0RGB, DEC_OUT_FRM_XRGB888, "0rgb24"},
    {AV_PIX_FMT_0BGR, DEC_OUT_FRM_XBGR888, "0bgr24"},
    {AV_PIX_FMT_RGB48LE, DEC_OUT_FRM_R16G16B16, "rgb48le"},
    {AV_PIX_FMT_BGR48LE, DEC_OUT_FRM_B16G16R16, "bgr48le"},
    {AV_PIX_FMT_RGBA64LE, DEC_OUT_FRM_A2R10G10B10, "rgba64le"},
    {AV_PIX_FMT_BGRA64LE, DEC_OUT_FRM_A2B10G10R10, "bgra64le"},
};

enum AVPixelFormat ff_codec_decfmt_to_pixfmt(enum DecPictureFormat picfmt) {
    int i;

    for (i = 0; i < sizeof(fmttopicfmttable) / sizeof(AvFmtTopicfmt); i++)
        if (fmttopicfmttable[i].picfmt == picfmt) return fmttopicfmttable[i].pixfmt;

    return AV_PIX_FMT_NONE;
}

enum DecPictureFormat ff_codec_pixfmt_to_decfmt(enum AVPixelFormat pixfmt) {
    int i;

    for (i = 0; i < sizeof(fmttopicfmttable) / sizeof(AvFmtTopicfmt); i++)
        if (fmttopicfmttable[i].pixfmt == pixfmt) return fmttopicfmttable[i].picfmt;

    return -1;
}

const char *ff_codec_decfmt_to_char(enum DecPictureFormat picfmt) {
    for (int i = 0; i < sizeof(fmttopicfmttable) / sizeof(AvFmtTopicfmt); i++)
        if (fmttopicfmttable[i].picfmt == picfmt) return fmttopicfmttable[i].pixfmt_name;

    return NULL;
}

const char *ff_codec_pixfmt_to_char(enum AVPixelFormat pixfmt) {
    for (int i = 0; i < sizeof(fmttopicfmttable) / sizeof(AvFmtTopicfmt); i++)
        if (fmttopicfmttable[i].pixfmt == pixfmt) return fmttopicfmttable[i].pixfmt_name;

    return NULL;
}

const char *esdec_get_ppout_enable(int pp_out, int pp_index) {
    if (pp_out == pp_index) {
        return "enabled";
    } else {
        return "disabled";
    }
}

DecPicAlignment esdec_get_align(int stride) {
    switch (stride) {
        case 1:
            return DEC_ALIGN_1B;
        case 8:
            return DEC_ALIGN_8B;
        case 16:
            return DEC_ALIGN_16B;
        case 32:
            return DEC_ALIGN_32B;
        case 64:
            return DEC_ALIGN_64B;
        case 128:
            return DEC_ALIGN_128B;
        case 256:
            return DEC_ALIGN_256B;
        case 512:
            return DEC_ALIGN_512B;
        case 1024:
            return DEC_ALIGN_1024B;
        case 2048:
            return DEC_ALIGN_2048B;
        default:
            log_error(NULL, "invaild stride: %d\n", stride);
    }
    return DEC_ALIGN_128B;
}

void ff_esdec_set_ppu_output_pixfmt(int is_8bits, enum AVPixelFormat pixfmt, PpUnitConfig *ppu_cfg) {
    if (!ppu_cfg) {
        log_error(NULL, "ppu_cfg is null\n");
        return;
    }

    if (!ppu_cfg->enabled) {
        log_warn(NULL, "pput disenabled\n");
    } else {
        enum DecPictureFormat dstpicfmt = ff_codec_pixfmt_to_decfmt(pixfmt);
        if (!is_8bits) {
            if (IS_PIC_8BIT_FMT(dstpicfmt)) {
                ppu_cfg->out_cut_8bits = 1;
            }
        }
        log_info(NULL,
                 "pixfmt: %d, dstpicfmt: %d, is_8bits: %d, out_cut_8bits: %d\n",
                 pixfmt,
                 dstpicfmt,
                 is_8bits,
                 ppu_cfg->out_cut_8bits);

        switch (dstpicfmt) {
            case DEC_OUT_FRM_NV21SP:
                ppu_cfg->cr_first = 1;
                break;
            case DEC_OUT_FRM_YUV420SP:
                // TODO
                break;
            case DEC_OUT_FRM_YUV420P:
                ppu_cfg->planar = 1;
                break;
            case DEC_OUT_FRM_YUV400:
                ppu_cfg->monochrome = 1;
                break;
            case DEC_OUT_FRM_YUV420P_I010:
                ppu_cfg->planar = 1;
                ppu_cfg->out_I010 = 1;
                break;
            case DEC_OUT_FRM_YUV420SP_P010:
                ppu_cfg->out_p010 = 1;
                break;
            case DEC_OUT_FRM_RGB888:
            case DEC_OUT_FRM_BGR888:
            case DEC_OUT_FRM_XRGB888:
            case DEC_OUT_FRM_XBGR888:
                ppu_cfg->rgb = 1;
                ppu_cfg->rgb_format = dstpicfmt;
                break;
            case DEC_OUT_FRM_ARGB888:
            case DEC_OUT_FRM_ABGR888:
                ppu_cfg->rgb = 1;
                ppu_cfg->rgb_stan = BT709;
                ppu_cfg->rgb_alpha = 255;
                ppu_cfg->rgb_format = dstpicfmt;
                break;
            default:
                log_error(NULL, "not support pixfmt\n");
                break;
        }
    }
}

void esdec_fill_planes(OutPutInfo *info, struct DecPicture *picture) {
    if (!info || !picture) {
        log_info(NULL, "info  or picture is null out: %p\n", info);
        return;
    }

    if (!info->enabled) {
        info->enabled = TRUE;
    }
    info->key_frame = (picture->picture_info.pic_coding_type == DEC_PIC_TYPE_I);
    info->width = picture->pic_width;
    info->height = picture->pic_height;
    info->bus_address = picture->luma.bus_address;
    info->virtual_address = picture->luma.virtual_address;
    info->format = ff_codec_decfmt_to_pixfmt(picture->picture_info.format);
    switch (info->format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_P010LE:
            info->n_planes = 2;
            info->stride[0] = picture->pic_stride;
            info->stride[1] = picture->pic_stride_ch;
            info->offset[0] = 0;
            info->offset[1] = picture->pic_stride * picture->pic_height;
            info->size = info->offset[1] * 3 / 2;
            log_debug(NULL,
                      "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                      info->format,
                      info->width,
                      info->height,
                      info->stride[0],
                      info->size);
            break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE:
            info->n_planes = 3;
            info->offset[0] = 0;
            info->stride[0] = picture->pic_stride;
            info->offset[1] = picture->pic_stride * picture->pic_height;
            info->stride[1] = picture->pic_stride_ch;
            info->offset[2] = info->offset[1] + picture->pic_stride_ch * picture->pic_height / 2;
            info->stride[2] = info->stride[1];
            info->size = info->offset[1] + picture->pic_stride_ch * picture->pic_height;
            log_debug(NULL,
                      "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                      info->format,
                      info->width,
                      info->height,
                      info->stride[0],
                      info->size);
            break;
        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR:
        case AV_PIX_FMT_RGB565:
        case AV_PIX_FMT_BGR565:
            info->n_planes = 1;
            info->stride[0] = picture->pic_stride;
            info->offset[0] = 0;
            info->size = picture->pic_stride * picture->pic_height;
            log_debug(NULL,
                      "format: %d width: %d, height: %d, stride: %d, size: %zu\n",
                      info->format,
                      info->width,
                      info->height,
                      info->stride[0],
                      info->size);
            break;
        default: {
            log_error(NULL, "not support dec format: %d\n", info->format);
        }
    }
}

int32_t ff_codec_compute_size(struct DecPicture *pic) {
    int32_t size = 0;
    enum AVPixelFormat format = ff_codec_decfmt_to_pixfmt(pic->picture_info.format);
    switch (format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
        case AV_PIX_FMT_P010LE: {
            size = pic->pic_stride * pic->pic_height * 3 / 2;
            break;
        }
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10LE: {
            size = pic->pic_stride * pic->pic_height + pic->pic_stride_ch * pic->pic_height;
            break;
        }
        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_0RGB:
        case AV_PIX_FMT_0BGR:
            size = pic->pic_stride * pic->pic_height;
            break;

        default: {
        }
    }
    return size;
}

int ff_codec_dump_data_to_file_by_decpicture(struct DecPicture *pic, DumpHandle *dump_handle) {
    int len = 0;
    if (!dump_handle) {
        return FAILURE;
    }

    if (ff_codec_compara_timeb(dump_handle->stop_dump_time) > 0) {
        av_log(NULL, AV_LOG_ERROR, "packe dump need stop\n");
        return ERR_TIMEOUT;
    } else {
        uint8_t *data;
        int32_t size;
        data = (uint8_t *)pic->luma.virtual_address;
        size = pic->luma.size + pic->chroma.size + pic->chroma_cr.size;
        len = fwrite(data, 1, size, dump_handle->fp);
        fflush(dump_handle->fp);
        if (len != size) {
            av_log(NULL, AV_LOG_ERROR, "write packet error !!! len: %d, size: %d\n", len, size);
        }
    }
    return len;
}