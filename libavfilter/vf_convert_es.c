#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_es.h"
#include "esvfbuffer.h"
#include "esvfcommon.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_GRAYF32LE,
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
    AV_PIX_FMT_B16G16R16I,
    AV_PIX_FMT_B16G16R16I_PLANAR,
    AV_PIX_FMT_B8G8R8_PLANAR,
    AV_PIX_FMT_B8G8R8I,
    AV_PIX_FMT_B8G8R8I_PLANAR,
    AV_PIX_FMT_B16G16R16F,
    AV_PIX_FMT_B16G16R16F_PLANAR,
    AV_PIX_FMT_B32G32R32F,
    AV_PIX_FMT_B32G32R32F_PLANAR,
};

typedef struct _MemoryInfo {
    int64_t fd;
    uint32_t size;
    void *vir_addr;
    ESMemory *memory;  // only be used in output memory
#ifndef MODEL_SIMULATION
    es_dma_buf *dma_buf;
    // uint32_t need_map;
#endif
} MemoryInfo;

typedef struct _VideoRect {
    uint32_t width;
    uint32_t height;
    uint32_t xoffset;
    uint32_t yoffset;
} VideoRect;

typedef struct ConvertContext {
    const AVClass *class;
    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;

    AVBufferRef *hw_device_ref;
    AVBufferRef *in_hw_frame_ref;
    AVBufferRef *out_hw_frame_ref;

    AVVSVDeviceContext *hw_dev_ctx;
    AVHWFramesContext *in_hw_frame_ctx;
    AVHWFramesContext *out_hw_frame_ctx;

    ES2D_ID id;

    MemoryInfo input_mem;

    VideoRect src_video_rect;
    VideoRect dst_video_rect;

    CropInfo crop_info;
    ScaleInfo scale_info;
    uint32_t is_crop;
    uint32_t is_scale;
    VF2DRgbU32 min_value;
    VF2DRgbU32 max_min_reciprocal;
    VF2DRgbU32 std_reciprocal;
    VF2DRgbU32 mean_value;
    uint32_t is_normalization;

    ESOutputPort *port;

    uint32_t stride_align;
    char *scale_set;
    char *crop_set;
    int32_t enter_out_fmt;
    char *dump_path;
    char *mm_r;
    char *min;
    char *mean;
    char *std_r;
    uint32_t step_r;
    int32_t normalization_mode;

    FILE *fp_input;
    FILE *fp_output;

} ConvertContext;

static int query_formats(AVFilterContext *ctx) {
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_NV12,        AV_PIX_FMT_NV21,   AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8,   AV_PIX_FMT_GRAYF32LE,
        AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE, AV_PIX_FMT_YVYU422, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_NV16,        AV_PIX_FMT_RGB24,  AV_PIX_FMT_BGR24,   AV_PIX_FMT_ARGB,    AV_PIX_FMT_ABGR,
        AV_PIX_FMT_BGRA,        AV_PIX_FMT_RGBA,   AV_PIX_FMT_ES,      AV_PIX_FMT_NONE,
    };
    AVFilterFormats *pix_fmts = ff_make_format_list(pixel_formats);

    int ret;
    if ((ret = ff_set_common_formats(ctx, pix_fmts)) < 0) return ret;

    return 0;
}

static int convert_format_is_supported(enum AVPixelFormat pixfmt) {
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == pixfmt) return SUCCESS;
    return FAILURE;
}

static av_cold int init(AVFilterContext *ctx) {
    ConvertContext *s = (ConvertContext *)ctx->priv;
    int ret = SUCCESS;

    if (!s) {
        ret = FAILURE;
        return ret;
    }
    s->hw_device_ref = NULL;
    s->in_hw_frame_ref = NULL;
    s->out_hw_frame_ref = NULL;

    s->hw_dev_ctx = NULL;
    s->in_hw_frame_ctx = NULL;
    s->out_hw_frame_ctx = NULL;

    s->is_scale = 0;
    s->is_crop = 0;
    s->crop_info.crop_height = 0;
    s->crop_info.crop_width = 0;
    s->crop_info.crop_xoffset = 0;
    s->crop_info.crop_yoffset = 0;
    s->scale_info.scale_height = 0;
    s->scale_info.scale_width = 0;

    s->port = NULL;
    s->input_mem.vir_addr = NULL;
    s->input_mem.size = 0;
    s->input_mem.fd = -1;
#ifndef MODEL_SIMULATION
    s->input_mem.dma_buf = NULL;
    // s->input_mem.need_map = 0;

    // init 2D handle
    ret = vf_2D_init(&s->id);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "vf_2D_init failed\n");
        ret = FAILURE;
        return ret;
    }
#endif

    s->fp_input = NULL;
    s->fp_output = NULL;
    if (s->dump_path) {
        s->fp_input = vf_dump_file_open(s->dump_path, "input");
        if (!s->fp_input) {
            av_log(ctx, AV_LOG_ERROR, "create dump input data file failed\n");
        }
        s->fp_output = vf_dump_file_open(s->dump_path, "output");
        if (!s->fp_output) {
            av_log(ctx, AV_LOG_ERROR, "create dump output data file failed\n");
        }
    }

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx) {
    ConvertContext *s = (ConvertContext *)ctx->priv;

    if (s->in_hw_frame_ref) {
        av_buffer_unref(&s->in_hw_frame_ref);
    }
    if (s->out_hw_frame_ref) {
        av_buffer_unref(&s->out_hw_frame_ref);
    }
    if (s->hw_device_ref) {
        av_buffer_unref(&s->hw_device_ref);
    }

    // unref output port
    esvf_output_port_unref(&s->port);

    if (s->input_mem.fd >= 0) {
        if (vf_is_simulation()) {
            if (s->input_mem.vir_addr) av_free(s->input_mem.vir_addr);
        } else {
#ifndef MODEL_SIMULATION
            es_dma_unmap(s->input_mem.dma_buf);
            es_dma_free(s->input_mem.dma_buf);
#endif
        }
    }

#ifndef MODEL_SIMULATION
    int ret = SUCCESS;
    ret = vf_2D_unint(&s->id);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "vf_2D_unint failed.\n");
    }
#endif

    if (s->fp_input) {
        fclose(s->fp_input);
    }
    if (s->fp_output) {
        fclose(s->fp_output);
    }
}

static int convert_para_crop(char *crop_set, CropInfo *crop_info, int width, int height) {
    int ret = SUCCESS;

    if (crop_set) {
        ret = vf_para_crop(crop_set, crop_info);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "vf_para_crop failed\n");
            return FAILURE;
        }

        crop_info->crop_width = FFALIGN(crop_info->crop_width, 2);
        crop_info->crop_height = FFALIGN(crop_info->crop_height, 2);
        crop_info->crop_xoffset = FFALIGN(crop_info->crop_xoffset, 2);
        crop_info->crop_yoffset = FFALIGN(crop_info->crop_yoffset, 2);

        if (crop_info->crop_xoffset + crop_info->crop_width > width
            || crop_info->crop_yoffset + crop_info->crop_height > height) {
            av_log(NULL,
                   AV_LOG_INFO,
                   "crop paras are invaild crop:%d:%d;%d:%d\n",
                   crop_info->crop_xoffset,
                   crop_info->crop_yoffset,
                   crop_info->crop_width,
                   crop_info->crop_height);
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int convert_para_scale(char *scale_set, ScaleInfo *scale_info, int width, int height) {
    int max_w = 0;
    int max_h = 0;
    int min_w = 0;
    int min_h = 0;
    int ret = SUCCESS;

    if (scale_set) {
        ret = vf_para_scale(scale_set, scale_info);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "vf_para_crop failed\n");
            return FAILURE;
        }

        max_w = width * 32;
        max_h = height * 32;
        min_w = width / 32;
        min_h = height / 32;

        scale_info->scale_width = FFALIGN(scale_info->scale_width, 2);
        scale_info->scale_height = FFALIGN(scale_info->scale_height, 2);

        if (scale_info->scale_width < min_w || scale_info->scale_width > max_w || scale_info->scale_height > max_h
            || scale_info->scale_height < min_h) {
            av_log(NULL,
                   AV_LOG_INFO,
                   "scale paras are invaild scale:%d:%d\n",
                   scale_info->scale_width,
                   scale_info->scale_height);
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int convert_get_output_fmt(ConvertContext *s) {
    int ret = SUCCESS;

    if (!s) {
        av_log(NULL, AV_LOG_ERROR, "convert_get_output_fmt invaild paras, s: %p\n", s);
        return FAILURE;
    }

    if (s->enter_out_fmt == -1) {
        s->out_fmt = s->in_fmt;
        return SUCCESS;
    }

    ret = convert_format_is_supported(s->enter_out_fmt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "enter_out_fmt: %d, is not supported\n", s->enter_out_fmt);
        return FAILURE;
    }

    s->out_fmt = (enum AVPixelFormat)s->enter_out_fmt;
    av_log(NULL, AV_LOG_INFO, "output format: %s\n", av_get_pix_fmt_name(s->out_fmt));

    return SUCCESS;
}

static int convert_para_normalization(ConvertContext *s) {
    int ret = SUCCESS;

    if (!s) {
        av_log(NULL, AV_LOG_ERROR, "convert_para_normalization invaild paras, s: %p\n", s);
        return FAILURE;
    }

    s->is_normalization = 0;

    if (s->normalization_mode != 0 && s->normalization_mode != 1) {
        av_log(NULL, AV_LOG_INFO, "do not normalization, normalization_mode: %d\n", s->normalization_mode);
        return SUCCESS;
    }

    if (s->normalization_mode == 0 && (!s->min || !s->mm_r)) {
        av_log(NULL, AV_LOG_ERROR, "normalization_mode is min_max, min: %p, mm_r: %p\n", s->min, s->mm_r);
        return FAILURE;
    }

    if (s->normalization_mode == 1 && (!s->mean || !s->std_r)) {
        av_log(NULL, AV_LOG_ERROR, "normalization_mode is z_score, mean: %p, std_r: %p\n", s->mean, s->std_r);
        return FAILURE;
    }

    if (s->mm_r) {
        ret = vf_para_normalization_rgb(s->mm_r, &s->max_min_reciprocal);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para max_min_reciprocal failed\n");
            return FAILURE;
        }
    }
    if (s->min) {
        ret = vf_para_normalization_rgb(s->min, &s->min_value);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para min_value failed\n");
            return FAILURE;
        }
    }
    if (s->mean) {
        ret = vf_para_normalization_rgb(s->mean, &s->mean_value);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para mean_value failed\n");
            return FAILURE;
        }
    }
    if (s->std_r) {
        ret = vf_para_normalization_rgb(s->std_r, &s->std_reciprocal);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "para std_reciprocal failed\n");
            return FAILURE;
        }
    }

    s->is_normalization = 1;

    av_log(NULL, AV_LOG_DEBUG, "normalizationMode: %d,stepReciprocal: %x\n", s->normalization_mode, s->step_r);
    av_log(NULL,
           AV_LOG_DEBUG,
           "min_value.R: %x, min_value.G: %x, min_value.B: %x\n",
           s->min_value.R,
           s->min_value.G,
           s->min_value.B);
    av_log(NULL,
           AV_LOG_DEBUG,
           "maxMinReciprocal.R: %x, maxMinReciprocal.G: %x, maxMinReciprocal.B: %x\n",
           s->max_min_reciprocal.R,
           s->max_min_reciprocal.G,
           s->max_min_reciprocal.B);
    av_log(NULL,
           AV_LOG_DEBUG,
           "meanValue.R: %x, meanValue.G: %x, meanValue.B: %x\n",
           s->mean_value.R,
           s->mean_value.G,
           s->mean_value.B);
    av_log(NULL,
           AV_LOG_DEBUG,
           "stdReciprocal.R: %x, stdReciprocal.G: %x, stdReciprocal.B: %x\n",
           s->std_reciprocal.R,
           s->std_reciprocal.G,
           s->std_reciprocal.B);

    return SUCCESS;
}

static av_cold int convert_config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    ConvertContext *s = (ConvertContext *)ctx->priv;

    AVVSVDeviceContext *hw_device_ctx = NULL;
    AVHWFramesContext *in_hw_frames_ctx = NULL;
    AVHWFramesContext *out_hw_frames_ctx = NULL;

    MemInfo *mem_info = NULL;
    int buffer_size = 0;
    int height = 0;
    int buf_num;
    int ret = FAILURE;

    s->src_video_rect.width = inlink->w;
    s->src_video_rect.height = inlink->h;
    s->src_video_rect.xoffset = 0;
    s->src_video_rect.yoffset = 0;

    if (s->crop_set) {
        ret = convert_para_crop(s->crop_set, &s->crop_info, inlink->w, inlink->h);
        if (ret == SUCCESS) {
            s->is_crop = 1;
        } else {
            av_log(ctx, AV_LOG_ERROR, "crop paras are invaild.\n");
            return FAILURE;
        }
    } else {
        s->crop_info.crop_height = inlink->h;
        s->crop_info.crop_width = inlink->w;
        s->crop_info.crop_xoffset = 0;
        s->crop_info.crop_yoffset = 0;
    }

    if (s->scale_set) {
        if (s->is_crop) {
            ret = convert_para_scale(s->scale_set, &s->scale_info, s->crop_info.crop_width, s->crop_info.crop_height);
        } else {
            ret = convert_para_scale(s->scale_set, &s->scale_info, inlink->w, inlink->h);
        }
        if (ret == SUCCESS) {
            s->is_scale = 1;
        } else {
            av_log(ctx, AV_LOG_ERROR, "scale paras are invaild.\n");
            return FAILURE;
        }
    } else {
        if (s->is_crop) {
            s->scale_info.scale_height = s->crop_info.crop_height;
            s->scale_info.scale_width = s->crop_info.crop_width;
        } else {
            s->scale_info.scale_height = inlink->h;
            s->scale_info.scale_width = inlink->w;
        }
    }

    if (s->is_crop && !s->is_scale) {
        s->dst_video_rect.width = s->crop_info.crop_width;
        s->dst_video_rect.height = s->crop_info.crop_height;
    } else if (s->is_scale) {
        s->dst_video_rect.width = s->scale_info.scale_width;
        s->dst_video_rect.height = s->scale_info.scale_height;
    } else {
        s->dst_video_rect.width = inlink->w;
        s->dst_video_rect.height = inlink->h;
    }

    outlink->w = s->dst_video_rect.width;
    outlink->h = s->dst_video_rect.height;

    ret = convert_para_normalization(s);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "normalization param is invaild\n");
        return FAILURE;
    }

    if (inlink->format == AV_PIX_FMT_ES && inlink->hw_frames_ctx) {
        in_hw_frames_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
        hw_device_ctx = in_hw_frames_ctx->device_ctx->hwctx;

        if (!inlink->hw_frames_ctx) {
            av_log(ctx, AV_LOG_ERROR, "Input must have a hwframe reference. \n");
            if (!s->in_hw_frame_ref) av_buffer_unref(&s->in_hw_frame_ref);
            if (!s->out_hw_frame_ref) av_buffer_unref(&s->out_hw_frame_ref);
            if (!s->hw_device_ref) av_buffer_unref(&s->hw_device_ref);
            return AVERROR(EINVAL);
        }

        s->in_hw_frame_ref = av_buffer_ref(inlink->hw_frames_ctx);
        s->hw_device_ref = av_buffer_ref(in_hw_frames_ctx->device_ref);

        s->hw_dev_ctx = hw_device_ctx;
        s->in_hw_frame_ctx = in_hw_frames_ctx;

        s->in_fmt = in_hw_frames_ctx->sw_format;
        ret = convert_get_output_fmt(s);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "convert_get_output_fmt failed \n");
            return FAILURE;
        }

        outlink->format = inlink->format;

        av_buffer_unref(&s->out_hw_frame_ref);
        s->out_hw_frame_ref = av_hwframe_ctx_alloc(s->hw_device_ref);
        if (!s->out_hw_frame_ref) {
            av_log(ctx, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed \n");
            if (!s->in_hw_frame_ref) av_buffer_unref(&s->in_hw_frame_ref);
            if (!s->out_hw_frame_ref) av_buffer_unref(&s->out_hw_frame_ref);
            if (!s->hw_device_ref) av_buffer_unref(&s->hw_device_ref);
            return AVERROR(EINVAL);
        }
        out_hw_frames_ctx = (AVHWFramesContext *)s->out_hw_frame_ref->data;
        s->out_hw_frame_ctx = out_hw_frames_ctx;
        out_hw_frames_ctx->format = AV_PIX_FMT_ES;
        out_hw_frames_ctx->sw_format = s->out_fmt;
        out_hw_frames_ctx->width = FFALIGN(outlink->w, s->stride_align);
        out_hw_frames_ctx->height = FFALIGN(outlink->h, 1);
        outlink->hw_frames_ctx = av_buffer_ref(s->out_hw_frame_ref);
    } else {
        s->in_fmt = inlink->format;
        ret = convert_get_output_fmt(s);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "convert_get_output_fmt failed \n");
            return FAILURE;
        }
        outlink->format = s->out_fmt;
    }

    if (s->port) {
        esvf_output_port_unref(&s->port);
    }
    if (inlink->format == AV_PIX_FMT_ES) {
        if (ctx->extra_hw_frames > 0) {
            buf_num = ctx->extra_hw_frames;
        } else {
            buf_num = 5;
        }
        av_log(ctx, AV_LOG_INFO, "hwaccel mode, allocate buffer num: %d\n", buf_num);
        s->port = esvf_allocate_output_port(
            buf_num, s->out_fmt, s->dst_video_rect.width, s->dst_video_rect.height, s->stride_align);
    } else {
        av_log(ctx, AV_LOG_INFO, "no hwaccel mode, allocate buffer num: 1\n");
        s->port = esvf_allocate_output_port(
            1, s->out_fmt, s->dst_video_rect.width, s->dst_video_rect.height, s->stride_align);
    }
    if (!s->port) {
        av_log(ctx, AV_LOG_ERROR, "esvf_allocate_output_port faild\n");
        ret = FAILURE;
    }

    return SUCCESS;
}

static int convert_transfer_data_from(AVFrame *dst, ConvertContext *s, MemoryInfo *out_mem) {
    AVPixFmtDescriptor *desc;
    int height;

    if (!s || !out_mem || !dst) {
        av_log(NULL, AV_LOG_INFO, "invaild paras, s: %p, out_mem: %p, dst: %p\n", s, out_mem, dst);
        return FAILURE;
    }

    desc = av_pix_fmt_desc_get(s->out_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_transfer_data_from get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(s->out_fmt));
        return FAILURE;
    }

// #ifndef MODEL_SIMULATION
//     es_dma_sync_start(out_mem->dma_buf);
// #endif
    for (int i = 0; i < 4 && s->port->mem_info->datasize[i]; i++) {
        height = s->port->mem_info->h;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        if (s->port->mem_info->linesize[i] == dst->linesize[i]) {
            esvf_memcpy_block(out_mem->vir_addr + s->port->mem_info->offset[i],
                              dst->data[i],
                              s->port->mem_info->linesize[i] * height);
        } else {
            esvf_memcpy_by_line(out_mem->vir_addr + s->port->mem_info->offset[i],
                                dst->data[i],
                                s->port->mem_info->linesize[i],
                                dst->linesize[i],
                                height);
        }
    }
// #ifndef MODEL_SIMULATION
//     es_dma_sync_end(out_mem->dma_buf);
// #endif

    return 0;
}

static AVFrame *convert_allocate_and_fill_output_frame(AVFilterLink *link, AVFrame *in, MemoryInfo *out_mem) {
    AVFilterContext *ctx = link->dst;
    ConvertContext *s = (ConvertContext *)ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    ESMemory *memory;
    ESBuffer *buffer;
    int ret = FAILURE;

    if (!out_mem) {
        av_log(NULL, AV_LOG_ERROR, "out_mem: %p is null\n", out_mem);
    }
    memory = out_mem->memory;

    if (link->format == AV_PIX_FMT_ES) {
        out = av_frame_alloc();
        if (!out) {
            return NULL;
        }
        out->width = outlink->w;
        out->height = outlink->h;
        out->format = AV_PIX_FMT_ES;
        out->key_frame = in->key_frame;
        out->pts = in->pts;
        out->reordered_opaque = in->reordered_opaque;
        out->hw_frames_ctx = av_buffer_ref(s->out_hw_frame_ref);
        buffer = &memory->buffer;
        buffer->port_ref = av_buffer_ref(memory->port_ref);
        buffer->buffer_ref = av_buffer_ref(memory->buffer_ref);
        vf_add_fd_to_side_data(out, out_mem->fd);
        for (int i = 0; i < 4 && s->port->mem_info->linesize[i]; i++) {
            out->data[i] = out_mem->vir_addr + s->port->mem_info->offset[i];
            out->linesize[i] = s->port->mem_info->linesize[i];
        }
        out->buf[0] =
            av_buffer_create((uint8_t *)memory, sizeof(*memory), esvf_buffer_consume, s->port, AV_BUFFER_FLAG_READONLY);
        if (!out->buf[0]) {
            av_log(NULL, AV_LOG_ERROR, "av_buffer_create frame[0] failed\n");
            av_free(out);
            return NULL;
        }
    } else {
        out = ff_get_video_buffer(outlink, FFALIGN(outlink->w, s->stride_align), outlink->h);
        av_frame_copy_props(out, in);
        out->width = outlink->w;
        out->height = outlink->h;
        out->format = s->out_fmt;
        if (!out) {
            return NULL;
        }

        ret = convert_transfer_data_from(out, s, out_mem);
        if (ret == FAILURE) {
            av_log(NULL, AV_LOG_ERROR, "convert_transfer_data_from failed\n");
            av_frame_free(&out);
            return NULL;
        }
    }

    return out;
}

static int convert_memcpy_host2device(ConvertContext *s, AVFrame *in, MemoryInfo *input_mem) {
    AVPixFmtDescriptor *desc;
    int height = 0;
    int cp_size = 0;
    int totol_size = 0;

    desc = av_pix_fmt_desc_get(s->in_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_memcpy_host2device get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(s->out_fmt));
        return FAILURE;
    }

// #ifndef MODEL_SIMULATION
//     es_dma_sync_start(input_mem->dma_buf);
// #endif
    for (int i = 0; i < FF_ARRAY_ELEMS(in->data) && in->data[i]; i++) {
        height = in->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        cp_size = in->linesize[i] * height;
        if (totol_size + cp_size > input_mem->size) {
            av_log(NULL, AV_LOG_ERROR, "convert_memcpy_host2device input_mem has no enough space\n");
            return FAILURE;
        }
        memcpy(input_mem->vir_addr + totol_size, in->data[i], cp_size);
        totol_size += cp_size;
    }
// #ifndef MODEL_SIMULATION
//     es_dma_sync_end(input_mem->dma_buf);
// #endif

    return SUCCESS;
}

static int convert_allocate_input_mem(ConvertContext *s, MemoryInfo *input_mem) {
    if (!input_mem) {
        av_log(NULL, AV_LOG_ERROR, "input_mem is invalid\n");
        return FAILURE;
    }

    if (input_mem->vir_addr) {
        return SUCCESS;
    }

    if (vf_is_simulation()) {
        if (input_mem->size > 0) {
            input_mem->vir_addr = av_malloc(FFALIGN(input_mem->size, s->stride_align));
            if (!input_mem->vir_addr) {
                av_log(NULL, AV_LOG_ERROR, "allocate mem failed\n");
                return FAILURE;
            }
            input_mem->fd = (int64_t)input_mem->vir_addr;
        } else {
            av_log(NULL, AV_LOG_ERROR, "invaild mem size: %d0\n", input_mem->size);
            return FAILURE;
        }
    } else {
#ifndef MODEL_SIMULATION
        input_mem->dma_buf = es_dma_alloc(DMA_TYPE_MMZ_0, FFALIGN(input_mem->size, s->stride_align), UNCACHED_BUF, 0);
        if (!input_mem->dma_buf) {
            av_log(NULL, AV_LOG_ERROR, "allocate input_mem dma_buf failed\n");
            return FAILURE;
        }
        es_dma_map(input_mem->dma_buf, UNCACHED_BUF);
        input_mem->fd = input_mem->dma_buf->dmabuf_fd;
        input_mem->vir_addr = input_mem->dma_buf->vir_addr;
#endif
    }

    return SUCCESS;
}

static int convert_get_frame_data_size(ConvertContext *s, AVFrame *in) {
    AVPixFmtDescriptor *desc;
    int height = 0;
    int total_size = 0;

    if (!in) {
        av_log(NULL, AV_LOG_ERROR, "convert_get_frame_data_size invaild paras, in: %p\n", in);
        return FAILURE;
    }

    desc = av_pix_fmt_desc_get(s->in_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_get_frame_data_size get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(s->out_fmt));
        return FAILURE;
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(in->data) && in->data[i]; i++) {
        height = in->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        total_size += in->linesize[i] * height;
    }

    return total_size;
}

#ifndef MODEL_SIMULATION
static int convert_init_input_mem_from_fd(MemoryInfo *input_mem) {
    es_dma_buf *dma_buf = NULL;

    if (!input_mem) {
        av_log(NULL, AV_LOG_ERROR, "convert_init_input_mem_from_fd invalid paras\n");
        return FAILURE;
    }

    if (input_mem->dma_buf) {
        dma_buf = input_mem->dma_buf;
        es_dma_unwrap(dma_buf);
        dma_buf = NULL;
    }

    dma_buf = es_dma_wrap(input_mem->fd, input_mem->vir_addr, input_mem->size);
    if (!dma_buf) {
        av_log(NULL, AV_LOG_ERROR, "convert_init_input_mem_from_fd es_dma_wrap failed\n");
        return FAILURE;
    }
    // input_mem->need_map = !(input_mem->vir_addr);
    // av_log(NULL, AV_LOG_ERROR,
    //             "input_mem->vir_addr: %p,  dma_buf->vir_addr: %p, need_map:%d\n",
    //             input_mem->vir_addr, dma_buf->vir_addr, input_mem->need_map);
    // if (input_mem->need_map) {
    //     es_dma_map(dma_buf);
    //     input_mem->dma_buf = dma_buf;
    //     input_mem->vir_addr = dma_buf->vir_addr;
    // }

    es_dma_map(dma_buf, UNCACHED_BUF);
    input_mem->dma_buf = dma_buf;
    input_mem->vir_addr = dma_buf->vir_addr;

    return SUCCESS;
}

static int convert_init_input_mem_from_fd_copy(ConvertContext *s, AVFrame *in, int64_t fd) {
    es_dma_buf *dma_buf = NULL;
    es_dma_buf *src_dma_buf = NULL;
    size_t dma_buf_size = 0;
    int ret = SUCCESS;

    if (!s) {
        av_log(NULL, AV_LOG_ERROR, "convert_init_input_mem_from_fd invalid paras\n");
        return FAILURE;
    }

    if (!s->input_mem.dma_buf) {
        dma_buf_size = FFALIGN(s->input_mem.size, s->stride_align);
        dma_buf = es_dma_alloc(DMA_TYPE_MMZ_0, dma_buf_size, UNCACHED_BUF, 0);
        if (!dma_buf) {
            av_log(NULL, AV_LOG_ERROR, "convert_init_input_mem_from_fd_copy allocate dma_buf failed\n");
            return FAILURE;
        }
        av_log(NULL,
               AV_LOG_INFO,
               "convert_init_input_mem_from_fd_copy allocate dma_buf size: %ld data size: %d\n",
               dma_buf->size,
               s->input_mem.size);
        es_dma_map(dma_buf, UNCACHED_BUF);
        s->input_mem.dma_buf = dma_buf;
        s->input_mem.vir_addr = dma_buf->vir_addr;
        s->input_mem.fd = dma_buf->dmabuf_fd;
    }

    src_dma_buf = es_dma_wrap(fd, in->data[0], s->input_mem.size);
    if (!src_dma_buf) {
        av_log(NULL, AV_LOG_ERROR, "convert_init_input_mem_from_fd_copy es_dma_wrap failed\n");
        return FAILURE;
    }

    // es_dma_sync_start(s->input_mem.dma_buf);
    ret = read_from_es_dma(src_dma_buf, s->input_mem.vir_addr, s->input_mem.size);
    // es_dma_sync_end(s->input_mem.dma_buf);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "read_from_es_dma failed\n");
        ret = FAILURE;
    }

    es_dma_unwrap(src_dma_buf);

    return ret;
}

#endif

static int convert_filter_frame(AVFilterLink *link, AVFrame *in) {
    AVFilterContext *ctx = link->dst;
    ConvertContext *s = (ConvertContext *)ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ESBuffer buffer;
    ESMemory *memory = NULL;
    MemoryInfo out_mem;
    AVFrame *out = NULL;
    int64_t fd;
    int ret = SUCCESS;

    out_mem.fd = -1;
    out_mem.vir_addr = NULL;
    out_mem.size = 0;
    out_mem.memory = NULL;

    if (link->format == AV_PIX_FMT_ES && !in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Input frames must have hardware context.\n");
        av_frame_free(&in);
        return FAILURE;
    }

    ret = esvf_get_buffer_unitl_timeout(s->port->frame_queue, &buffer, -1);
    if (ret == FAILURE) {
        av_log(ctx, AV_LOG_ERROR, "esvf_get_video_buffer failed\n");
        av_frame_free(&in);
        return FAILURE;
    }

    memory = buffer.memory;
    if (!memory) {
        av_log(ctx, AV_LOG_ERROR, "memory is invaild\n");
        av_frame_free(&in);
        return FAILURE;
    }

    if (s->input_mem.fd < 0) {
        s->input_mem.size = convert_get_frame_data_size(s, in);
    }

    if (link->format == AV_PIX_FMT_ES) {
        ret = esvf_get_fd(in, &fd);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "esvf_get_fd faild\n");
            av_frame_free(&in);
            return FAILURE;
        }

        if (vf_is_simulation()) {
            s->input_mem.fd = fd;
            s->input_mem.vir_addr = (void *)s->input_mem.fd;
        } else {
#ifndef MODEL_SIMULATION
            // s->input_mem.fd = fd;
            // s->input_mem.vir_addr = in->data[0];
            // ret = convert_init_input_mem_from_fd(&s->input_mem);
            ret = convert_init_input_mem_from_fd_copy(s, in, fd);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "convert_init_input_mem_from_fd_copy faild\n");
                av_frame_free(&in);
                return FAILURE;
            }
#endif
        }
    } else {
        ret = convert_allocate_input_mem(s, &s->input_mem);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "convert_allocate_input_mem failed\n");
            esvf_buffer_consume((void *)s->port, (void *)memory);
            av_frame_free(&in);
            return FAILURE;
        }

        ret = convert_memcpy_host2device(s, in, &s->input_mem);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "convert_memcpy_host2device faild\n");
            esvf_buffer_consume((void *)s->port, (void *)memory);
            av_frame_free(&in);
            return FAILURE;
        }
    }

    out_mem.fd = memory->fd;
    out_mem.vir_addr = memory->vir_addr;
    out_mem.size = memory->size;
    out_mem.memory = memory;
#ifndef MODEL_SIMULATION
    out_mem.dma_buf = memory->dma_buf;
#endif

    if (s->fp_input) {
        vf_dump_bytes_to_file(s->input_mem.vir_addr, s->input_mem.size, s->fp_input);
    }

    if (vf_is_simulation()) {
        /*todo convert, test code*/
        memcpy(out_mem.vir_addr,
               s->input_mem.vir_addr,
               s->port->mem_info->size < s->input_mem.size ? s->port->mem_info->size : s->input_mem.size);
    } else {
#ifndef MODEL_SIMULATION
        VF2DSurface input_paras;
        VF2DSurface output_paras;
        VF2DRect input_rect;
        VF2DRect output_rect;
        VF2DNormalizationParams normalization_paras;

        av_log(ctx, AV_LOG_DEBUG, "2D input memory: %p, size: %ld\n", s->input_mem.dma_buf, s->input_mem.dma_buf->size);

        av_log(ctx, AV_LOG_DEBUG, "2D output memory: %p, size: %ld\n", out_mem.dma_buf, out_mem.dma_buf->size);

#if 0
        // read test file to input mem for umd test
        FILE *fp = fopen("ILSVRC2012_val_00000043_224x224x3_R8G8B8_0317.raw", "rb+");
        if(fp == NULL){
            av_log(ctx, AV_LOG_ERROR, "test fopen faild\n");
        }
        av_log(ctx, AV_LOG_INFO, "vir_addr:%p, size:%d\n",s->input_mem.vir_addr, s->input_mem.size);
        es_dma_sync_start(s->input_mem.dma_buf);
        fread(s->input_mem.vir_addr, s->input_mem.size, 1, fp);
        fflush(fp);
        fclose(fp);
        es_dma_sync_end(s->input_mem.dma_buf);
#endif

        input_paras.height = s->src_video_rect.height;
        input_paras.width = FFALIGN(s->src_video_rect.width, s->stride_align);
        input_paras.pixfmt = s->in_fmt;
        input_paras.dma_buf = s->input_mem.dma_buf;
        input_paras.offset = 0;

        input_rect.left = s->crop_info.crop_xoffset;
        input_rect.top = s->crop_info.crop_yoffset;
        input_rect.bottom = s->crop_info.crop_yoffset + s->crop_info.crop_height;
        input_rect.right = s->crop_info.crop_xoffset + s->crop_info.crop_width;

        output_paras.height = s->dst_video_rect.height;
        output_paras.width = FFALIGN(s->dst_video_rect.width, s->stride_align);
        output_paras.pixfmt = s->out_fmt;
        output_paras.dma_buf = out_mem.dma_buf;
        output_paras.offset = 0;

        output_rect.left = 0;
        output_rect.top = 0;
        output_rect.bottom = s->scale_info.scale_height;
        output_rect.right = s->scale_info.scale_width;

        if (s->is_normalization) {
            normalization_paras.normalizationMode = s->normalization_mode;
            // for min-max
            normalization_paras.minValue.R = s->min_value.R;
            normalization_paras.minValue.G = s->min_value.G;
            normalization_paras.minValue.B = s->min_value.B;
            normalization_paras.maxMinReciprocal.R = s->max_min_reciprocal.R;
            normalization_paras.maxMinReciprocal.G = s->max_min_reciprocal.G;
            normalization_paras.maxMinReciprocal.B = s->max_min_reciprocal.B;
            // for z-score
            normalization_paras.meanValue.R = s->mean_value.R;
            normalization_paras.meanValue.G = s->mean_value.G;
            normalization_paras.meanValue.B = s->mean_value.B;
            normalization_paras.stdReciprocal.R = s->std_reciprocal.R;
            normalization_paras.stdReciprocal.G = s->std_reciprocal.G;
            normalization_paras.stdReciprocal.B = s->std_reciprocal.B;
            // for stepReciprocal
            normalization_paras.stepReciprocal = s->step_r;
        }

        ret = vf_2D_work(&s->id,
                         &input_paras,
                         &output_paras,
                         s->is_normalization ? &normalization_paras : NULL,
                         &input_rect,
                         &output_rect);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "vf_2D_work error\n");
            esvf_buffer_consume((void *)s->port, (void *)memory);
            av_frame_free(&in);
            return FAILURE;
        }
#endif
    }

    if (s->fp_output) {
        vf_dump_bytes_to_file(out_mem.vir_addr, s->port->mem_info->size, s->fp_output);
    }

    out = convert_allocate_and_fill_output_frame(link, in, &out_mem);
    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "convert_allocate_and_fill_output_frame failed\n");
        esvf_buffer_consume((void *)s->port, (void *)memory);
        av_frame_free(&in);
        return FAILURE;
    }

    if (link->format == AV_PIX_FMT_ES) {
        if (vf_is_simulation()) {
            s->input_mem.fd = -1;
            s->input_mem.vir_addr = NULL;
            s->input_mem.size = 0;
        }
#ifndef MODEL_SIMULATION
        // if (s->input_mem.need_map) {
        //     es_dma_unmap(s->input_mem.dma_buf);
        // }
        // es_dma_unwrap(s->input_mem.dma_buf);
        // s->input_mem.dma_buf = NULL;
#endif
    } else {
        esvf_buffer_consume((void *)s->port, (void *)memory);
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(ConvertContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption options[] = {
    {"stride_align", "set buffer stride", OFFSET(stride_align), AV_OPT_TYPE_INT, {.i64 = 64}, 0, 2048, FLAGS},
    {"scale", "widthxheight", OFFSET(scale_set), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    {"crop",
     "crop(xoffset)x(yoffset))x(width)x(height)",
     OFFSET(crop_set),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"o_fmt", "output pixfmt", OFFSET(enter_out_fmt), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, FLAGS, "fmt"},
    {"nv12", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV12}, 0, INT_MAX, FLAGS, "fmt"},
    {"nv21", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV21}, 0, INT_MAX, FLAGS, "fmt"},
    {"yuv420p", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P}, 0, INT_MAX, FLAGS, "fmt"},
    {"gray", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAY8}, 0, INT_MAX, FLAGS, "fmt"},
    {"gray32f", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_GRAYF32LE}, 0, INT_MAX, FLAGS, "fmt"},
    {"yuv420p10le", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUV420P10LE}, 0, INT_MAX, FLAGS, "fmt"},
    {"p010le", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_P010LE}, 0, INT_MAX, FLAGS, "fmt"},
    {"yvyu422", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YVYU422}, 0, INT_MAX, FLAGS, "fmt"},
    {"yuyv422", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_YUYV422}, 0, INT_MAX, FLAGS, "fmt"},
    {"uyvy422", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_UYVY422}, 0, INT_MAX, FLAGS, "fmt"},
    {"nv16", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_NV16}, 0, INT_MAX, FLAGS, "fmt"},
    {"rgb24", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGB24}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgr24", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGR24}, 0, INT_MAX, FLAGS, "fmt"},
    {"argb", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ARGB}, 0, INT_MAX, FLAGS, "fmt"},
    {"abgr", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_ABGR}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgra", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_BGRA}, 0, INT_MAX, FLAGS, "fmt"},
    {"rgba", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_RGBA}, 0, INT_MAX, FLAGS, "fmt"},
    {"b16g16r16i", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B16G16R16I}, 0, INT_MAX, FLAGS, "fmt"},
    {"b16g16r16i_p",
     "output pixfmt",
     0,
     AV_OPT_TYPE_CONST,
     {.i64 = AV_PIX_FMT_B16G16R16I_PLANAR},
     0,
     INT_MAX,
     FLAGS,
     "fmt"},
    {"bgr24_p", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B8G8R8_PLANAR}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgr24i", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B8G8R8I}, 0, INT_MAX, FLAGS, "fmt"},
    {"bgr24i_p", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B8G8R8I_PLANAR}, 0, INT_MAX, FLAGS, "fmt"},
    {"b16g16r16f", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B16G16R16F}, 0, INT_MAX, FLAGS, "fmt"},
    {"b16g16r16f_p",
     "output pixfmt",
     0,
     AV_OPT_TYPE_CONST,
     {.i64 = AV_PIX_FMT_B16G16R16F_PLANAR},
     0,
     INT_MAX,
     FLAGS,
     "fmt"},
    {"b32g32r32f", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = AV_PIX_FMT_B32G32R32F}, 0, INT_MAX, FLAGS, "fmt"},
    {"b32g32r32f_p",
     "output pixfmt",
     0,
     AV_OPT_TYPE_CONST,
     {.i64 = AV_PIX_FMT_B32G32R32F_PLANAR},
     0,
     INT_MAX,
     FLAGS,
     "fmt"},
    {"n_mode",
     "set normalization mode",
     OFFSET(normalization_mode),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     FLAGS,
     "n_mode"},
    {"min_max", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, INT_MAX, FLAGS, "n_mode"},
    {"z_score", "output pixfmt", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, INT_MAX, FLAGS, "n_mode"},
    {"mm_r",
     "set normalization maxMinReciprocal, such as:R/G/B",
     OFFSET(mm_r),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"min", "set normalization minValue, such as:R/G/B", OFFSET(min), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    {"mean",
     "set normalization meanValue, such as:R/G/B",
     OFFSET(mean),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"std_r",
     "set normalization stdReciprocal, such as:R/G/B",
     OFFSET(std_r),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     0,
     0,
     FLAGS},
    {"step_r", "set normalization stepReciprocal", OFFSET(step_r), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    {"dump_path",
     "set file path to dump 2D input data and output data",
     OFFSET(dump_path),
     AV_OPT_TYPE_STRING,
     {.i64 = NULL},
     0,
     0,
     FLAGS},
    {NULL}};

static const AVClass convert_class = {
    .class_name = "convert",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad convert_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = convert_filter_frame,
    },
};

static const AVFilterPad convert_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = convert_config_props,
    },
};

AVFilter ff_vf_convert_es = {
    .name = "convert_es",
    .description = NULL_IF_CONFIG_SMALL("es 2D convert"),

    .init = init,
    .uninit = uninit,

    .priv_size = sizeof(ConvertContext),
    .priv_class = &convert_class,

    FILTER_INPUTS(convert_inputs),
    FILTER_OUTPUTS(convert_outputs),
    FILTER_QUERY_FUNC(query_formats),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
