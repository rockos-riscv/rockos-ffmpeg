/*
 * Copyright (C) 2019  VeriSilicon
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "es_common.h"
#include "avcodec.h"

/* parse gstreamer cjson para about crop*/
int ff_codec_get_crop(char *str, CropInfo *crop_info) {
    char *p;

    if (!str || !crop_info) {
        return -1;
    }
    if (((p = strstr(str, "cx")) == NULL) && ((p = strstr(str, "cy")) == NULL) && ((p = strstr(str, "cw")) == NULL)
        && ((p = strstr(str, "ch")) == NULL)) {
        return 0;
    }

    if ((p = strstr(str, "cx")) != NULL) {
        crop_info->crop_xoffset = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "cy")) != NULL) {
        crop_info->crop_yoffset = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "cw")) != NULL) {
        crop_info->crop_width = atoi(p + 3);
    } else {
        return -1;
    }

    if ((p = strstr(str, "ch")) != NULL) {
        crop_info->crop_height = atoi(p + 3);
    } else {
        return -1;
    }

    return 0;
}

int es_codec_get_crop(char *str, CropInfo *crop_info) {
    if (!str || !crop_info) {
        return -1;
    }

    if (sscanf(str,
               "%dx%dx%dx%d",
               &crop_info->crop_xoffset,
               &crop_info->crop_yoffset,
               &crop_info->crop_width,
               &crop_info->crop_height)
        != 4) {
        return -1;
    }

    return 0;
}

int es_codec_get_scale(char *str, ScaleInfo *scale_info) {
    if (!str || !scale_info) {
        return -1;
    }

    if (strchr(str, ':')) {
        if (sscanf(str, "%d:%d", &scale_info->scale_width, &scale_info->scale_height) != 2) {
            return -1;
        }
    } else {
        scale_info->scale_width = scale_info->scale_height = atoi(str);
    }

    return 0;
}

static int ff_dec_scale_is_valid(int value) {
    if ((value == -1) || (value == -2) || (value == -4) || (value == -8) || (value >= 0)) {
        return 1;
    } else {
        return 0;
    }
}
/* parse gstreamer cjson para about scale*/
int ff_dec_get_scale(char *str, ScaleInfo *scale_info, int pp_idx) {
    char *p;

    if (!str || !scale_info) {
        return -1;
    }

    if (((p = strstr(str, "sw")) == NULL) && ((p = strstr(str, "sh")) == NULL)) {
        return 0;
    }

    if (pp_idx == 0) {
        av_log(NULL, AV_LOG_ERROR, "pp0 not support scale!");
        return -2;
    }

    if ((p = strstr(str, "sw")) != NULL) {
        scale_info->scale_width = atoi(p + 3);
    } else {
        return -1;
    }

    if (!ff_dec_scale_is_valid(scale_info->scale_width)) {
        av_log(NULL, AV_LOG_ERROR, "pp1 only support scale width config 1/2/4/8!");
        return -1;
    }

    if ((p = strstr(str, "sh")) != NULL) {
        scale_info->scale_height = atoi(p + 3);
    } else {
        return -1;
    }

    if (!ff_dec_scale_is_valid(scale_info->scale_height)) {
        av_log(NULL, AV_LOG_ERROR, "pp1 only support scale height config 1/2/4/8!");
        return -1;
    }

    return 0;
}

DumpHandle *ff_codec_dump_file_open(const char *dump_path, int duration, DumpParas *paras) {
    DumpHandle *dump_handle = NULL;
    char file_path[PATH_MAX];
    struct timeb sys_time;
    char time_char[128];
    struct tm *tm;
    int ret;

    if (!dump_path) {
        av_log(NULL, AV_LOG_ERROR, "error !!! dump path is null\n");
        return NULL;
    }

    if (!paras) {
        av_log(NULL, AV_LOG_ERROR, "error !!! paras is null\n");
        return NULL;
    }

    ret = access(dump_path, 0);
    if (ret == -1) {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s does not exist\n", dump_path);
        if (mkdir(dump_path, 0731) == -1) {
            av_log(NULL, AV_LOG_ERROR, "create dump_path: %s failed errno: %d\n", dump_path, errno);
            return NULL;
        }
    } else {
        av_log(NULL, AV_LOG_INFO, "dump_path: %s exist\n", dump_path);
    }

    if (duration <= 0) {
        av_log(NULL, AV_LOG_ERROR, "invalid dump_time: %d\n", duration);
        return NULL;
    }

    // get local time
    ftime(&sys_time);
    tm = localtime(&sys_time);
    strftime(time_char, sizeof(time_char), "%y%m%d%H%M%S", tm);

    dump_handle = (DumpHandle *)malloc(sizeof(DumpHandle));
    if (!dump_handle) {
        av_log(NULL, AV_LOG_ERROR, "dump_handle malloc failed\n");
        return NULL;
    }

    dump_handle->stop_dump_time = av_gettime_relative() + duration * 1000;

    if (!paras->pic_stride && !paras->pic_stride_ch && !paras->fmt) {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%dms_%dx%d.%s",
                 dump_path,
                 paras->prefix_name,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->suffix_name);
    } else if (!paras->pic_stride && !paras->pic_stride_ch) {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%dms_%dx%d_%s.%s",
                 dump_path,
                 paras->prefix_name,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->fmt,
                 paras->suffix_name);
    } else {
        snprintf(file_path,
                 sizeof(file_path),
                 "%s/%s_%s_%s_%dms_%dx%d_%dx%d(stride)_%s.%s",
                 dump_path,
                 paras->prefix_name,
                 paras->ppu_channel,
                 time_char,
                 duration,
                 paras->width,
                 paras->height,
                 paras->pic_stride,
                 paras->pic_stride_ch,
                 paras->fmt,
                 paras->suffix_name);
    }

    dump_handle->fp = fopen(file_path, "ab+");
    if (dump_handle->fp) {
        av_log(NULL, AV_LOG_INFO, "open %s success\n", file_path);
    } else {
        dump_handle->fp = NULL;
        free(dump_handle);
        av_log(NULL, AV_LOG_ERROR, "open %s failed\n", file_path);
        return NULL;
    }

    return dump_handle;
}

int ff_codec_dump_file_close(DumpHandle **dump_handle) {
    if (!dump_handle || !(*dump_handle)) {
        return 0;
    }
    fflush((*dump_handle)->fp);
    fclose((*dump_handle)->fp);
    (*dump_handle)->fp = NULL;
    free(*dump_handle);
    *dump_handle = NULL;
    return 0;
}

// if now_time > end_time, return 1
int ff_codec_compara_timeb(int64_t end_time) {
    int64_t now_time;
    now_time = av_gettime_relative();

    if ((now_time - end_time) > 0) {
        return 1;
    } else {
        return 0;
    }
}

int ff_codec_dump_bytes_to_file(void *data, int size, DumpHandle *dump_handle) {
    int len = 0;

    if (!dump_handle || !dump_handle->fp) {
        av_log(NULL, AV_LOG_ERROR, " invalid dump_handle\n");
        return FAILURE;
    }

    if (!data || size <= 0) {
        return FAILURE;
    }

    if (ff_codec_compara_timeb(dump_handle->stop_dump_time) > 0) {
        av_log(NULL, AV_LOG_INFO, "data dump stop\n");
        return ERR_TIMEOUT;
    } else {
        len = fwrite(data, 1, size, dump_handle->fp);
        fflush(dump_handle->fp);
        if (len != size) {
            av_log(NULL, AV_LOG_ERROR, "write data to file error !!! len: %d, data size: %d\n", len, size);
        }
    }

    return len;
}

const char *ff_vsv_encode_get_fmt_char(enum AVPixelFormat pix_fmt) {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            return "yuv420p";
        case AV_PIX_FMT_NV12:
            return "nv12";
        case AV_PIX_FMT_NV21:
            return "nv21";
        case AV_PIX_FMT_UYVY422:
            return "uyvy422";
        case AV_PIX_FMT_YUYV422:
            return "yuyv422";
        case AV_PIX_FMT_YUV420P10LE:
            return "I010";
        case AV_PIX_FMT_P010LE:
            return "p010";
        default:
            return "unknown";
    }
}

int ff_es_codec_add_fd_to_side_data(AVFrame *frame, uint64_t fd) {
    int ret = SUCCESS;
    AVFrameSideData *sd = NULL;

    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "frame is null\n");
        return FAILURE;
    }

    av_log(NULL, AV_LOG_DEBUG, "add side data: fd = %lx\n", fd);
    sd = av_frame_new_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD, sizeof(fd));
    if (sd && sd->data) {
        memcpy(sd->data, &fd, sizeof(fd));
    } else {
        ret = FAILURE;
        av_log(NULL, AV_LOG_ERROR, "av_frame_new_side_data faild sd: %p\n", sd);
    }

    return ret;
}

int ff_es_codec_memcpy_block(void *src, void *dst, size_t data_size) {
    if (!src || !dst) {
        return AVERROR(EINVAL);
    }
    memcpy(dst, src, data_size);

    return 0;
}

int ff_es_codec_memcpy_by_line(uint8_t *src, uint8_t *dst, int src_linesize, int dst_linesize, int linecount) {
    int copy_size;
    if (!src || !dst) {
        return AVERROR(EINVAL);
    }

    copy_size = FFMIN(src_linesize, dst_linesize);
    for (int i = 0; i < linecount; i++) {
        memcpy(dst + i * dst_linesize, src + i * src_linesize, copy_size);
    }

    return 0;
}