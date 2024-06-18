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

#ifndef AVCODEC_ES_JPEG_DECODE_H
#define AVCODEC_ES_JPEG_DECODE_H

#include "deccfg.h"
#include "dectypes.h"
#include "avcodec.h"
#include "hwconfig.h"
#include "internal.h"
#include "decode.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "es_codec_private.h"
#include "es_common.h"
#include <stdlib.h>
#include <linux/limits.h>
#include "esdec_common.h"

#define EXTRA_BUFFERS_BASE 17

#define MAX_BUFFERS 17
#define MAX_WAIT_FOR_CONSUME_BUFFERS 100
#define MAX_SEG_NUM 4

#ifndef NEXT_MULTIPLE
#define NEXT_MULTIPLE(value, n) (((value) + (n) - 1) & ~((n) - 1))
#endif

#ifndef ALIGN
#define ALIGN(a) (1 << (a))
#endif

#ifdef SW_PERFORMANCE
#define INIT_SW_PERFORMANCE   \
  double dec_cpu_time = 0;    \
  clock_t dec_start_time = 0; \
  clock_t dec_end_time = 0;
#else
#define INIT_SW_PERFORMANCE
#endif

#ifdef SW_PERFORMANCE
#define START_SW_PERFORMANCE dec_start_time = clock();
#else
#define START_SW_PERFORMANCE
#endif

#ifdef SW_PERFORMANCE
#define END_SW_PERFORMANCE \
  dec_end_time = clock();  \
  dec_cpu_time += ((double)(dec_end_time - dec_start_time)) / CLOCKS_PER_SEC;
#else
#define END_SW_PERFORMANCE
#endif

#ifdef SW_PERFORMANCE
#define FINALIZE_SW_PERFORMANCE printf("SW_PERFORMANCE %0.5f\n", dec_cpu_time);
#else
#define FINALIZE_SW_PERFORMANCE
#endif

#ifdef SW_PERFORMANCE
#define FINALIZE_SW_PERFORMANCE_PP \
  printf("SW_PERFORMANCE_PP %0.5f\n", dec_cpu_time);
#else
#define FINALIZE_SW_PERFORMANCE_PP
#endif

#ifdef FB_SYSLOG_ENABLE
#include "syslog_sink.h"
#define VSV_DEC_INFO_PRINT(fmt, ...) FB_SYSLOG((const void *)&dec_ctx->log_header, SYSLOG_SINK_LEV_INFO, (char *)fmt, ## __VA_ARGS__)
#endif

typedef struct {
    int x;
    int y;
    int cw;
    int ch;
    int sw;
    int sh;
} Scale;


typedef struct {
    struct DecPicturePpu *pic;
    uint8_t wait_for_consume;
}VSVDecPicWaitForConsume;

typedef struct _ESJpegOutputMemory {
    int fd[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t *virtual_address;
    struct DWLLinearMem mem;
} ESJpegOutputMemory;

typedef struct _ESJpegOutputMemoryList {
    ESJpegOutputMemory output_memory[MAX_BUFFERS];
    int size;
} ESJpegOutputMemoryList;

enum ThumbMode {
    Only_Decode_Pic = 0,
    Only_Decode_Thumb,
    Decode_Pic_Thumb
};

enum ESJpegCode {
    ES_ERROR = -1,
    ES_OK = 0,
    ES_MORE_BUFFER,
    ES_EXIT
};

typedef const void *ESJDecInst;

/* thread for low latency feature */
typedef void* task_handle;
typedef void* (*task_func)(void*);

typedef struct {
    const AVClass *class;

    char *pp_setting[ES_VID_DEC_MAX_OUT_COUNT];
    char *scale_set;
    char * crop_set[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t packet_dump;
    char *dump_path;
    uint32_t packet_dump_time;
    uint32_t dump_pkt_count;
    uint32_t dump_frame_count[2];
    time_t start_dump_time;
    char dirpath[PATH_MAX];
    uint32_t pmode;
    uint32_t fmode;

    DumpHandle *frame_dump_handle[ES_VID_DEC_MAX_OUT_COUNT];
    DumpHandle *pkt_dump_handle;


    uint32_t frame_dump[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t frame_dump_time[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t out_stride;
    uint32_t fdump;
    uint32_t force_8bit;
    char *filename;
    uint32_t drop_frame_interval;
    // uint32_t thumbdone;
    uint32_t thum_exist;

    AVCodecContext *avctx;
    AVBufferRef *hwdevice;
    AVBufferRef *hwframe;
    int extra_hw_frames;

#ifdef FB_SYSLOG_ENABLE
    LOG_INFO_HEADER log_header;
    char module_name[16];
#endif

    uint32_t disable_dtrc;
    uint8_t pp_units_params_from_cmd_valid;
    uint8_t *stream_stop ;

    uint32_t enable_mc;
    uint32_t hdrs_rdy;
    DecPicAlignment align;  /* default: 64 bytes alignment */
    uint32_t prev_width;
    uint32_t prev_height;

    uint32_t retry;

    uint32_t clock_gating;
    uint32_t data_discard;
    uint32_t latency_comp;
    uint32_t output_picture_endian;
    uint32_t bus_burst_length;
    uint32_t asic_service_priority;
    uint32_t output_format[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t service_merge_disable;

    uint32_t tiled_output;
    uint32_t dpb_mode;
    uint32_t pp_enabled;
    int32_t pp_fmt[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t cfg_pp_enabled[ES_VID_DEC_MAX_OUT_COUNT];
    uint32_t pp_out;//control output PP0 or PP

    ESJDecInst dec_inst;
    void *dwl_inst;
    uint32_t thumb_mode;

    uint32_t use_extra_buffers_num;
    uint32_t buffer_size;
    uint32_t num_buffers;    /* external buffers allocated yet. */
    uint32_t min_buffer_num;
    uint32_t add_buffer_thread_run;
    pthread_mutex_t ext_buffer_control;
    uint32_t buffer_consumed[MAX_BUFFERS];
    uint32_t buffer_release_flag;

    uint32_t pic_display_number;
    uint32_t pic_decode_number;
    uint32_t got_package_number;
    uint32_t last_pic_flag;
    uint32_t add_extra_flag;
    uint32_t pic_size;
    uint32_t decode_mode;
    uint32_t low_latency;
    uint32_t low_latency_sim;

    struct DWLInitParam dwl_init;
    ESJpegOutputMemoryList output_memory_list;

    enum DecRet rv;
    /* one extra stream buffer so that we can decode ahead,
     * and be ready when core has finished
     */
    #define MAX_STRM_BUFFERS    (MAX_ASIC_CORES + 1)

    struct DWLLinearMem stream_mem[MAX_STRM_BUFFERS];
    long int max_strm_len;
    uint32_t allocated_buffers;
    uint32_t stream_mem_index;

    struct DecPicturePpu pic;
    struct DecPicturePpu pic_out;
    uint32_t hw_ppu_initialized;
    uint32_t cycle_count; /* Sum of average cycles/mb counts */
    uint32_t initialized;
    uint32_t closed;
    VSVDecPicWaitForConsume wait_for_consume_list[MAX_WAIT_FOR_CONSUME_BUFFERS];
    uint32_t wait_consume_num;
    pthread_mutex_t consume_mutex;
    void (*vsv_decode_picture_consume)(void *opaque, uint8_t *data);
    void (*vsv_decode_pri_picture_info_free)(void *opaque, uint8_t *data);
    void (*data_free)(void *opaque, uint8_t *data);
    struct DecConfig vsv_dec_config;
    struct DecSequenceInfo  sequence_info;
    Scale scales[4];
    uint32_t scales_num;
    uint32_t extra_buffer_num; //record extra buffer count
    uint32_t thum_out;//whether to output thumbnails 0-output only original image 1-output only thumbnails

    uint32_t pic_decoded;
    sem_t send_sem;
    sem_t frame_sem;
    uint32_t decode_end_flag;
    uint32_t strm_len;
    uint32_t sw_hw_bound;
    uint32_t tmp_len;
    struct strmInfo send_strm_info;
    task_handle task;  // for low-latency thread
    uint32_t task_existed; // for low-latency thread
    AVPacket avpkt;
    AVFrame *frame;

}VSVDECContext;

int ff_es_jpeg_dec_init_hwctx(AVCodecContext *avctx);

uint32_t ff_es_jpeg_dec_del_pic_wait_consume_list(VSVDECContext *dec_ctx, uint8_t *data);

int ff_es_jpeg_dec_parse_scale(AVCodecContext *avctx);

int ff_es_jpeg_dec_output_frame(AVCodecContext *avctx,
                                AVFrame *out,
                                struct DecPicturePpu *decoded_pic);

int ff_es_jpeg_dec_output_thum(AVCodecContext *avctx,
                               AVFrame *out,
                               struct DecPicturePpu *decoded_pic);

int ff_es_jpeg_dec_send_avpkt_to_decode_buffer(AVCodecContext *avctx,
                                              AVPacket *avpkt,
                                              struct DWLLinearMem stream_buffer);

void ff_es_jpeg_dec_print_return(AVCodecContext *avctx, enum DecRet jpeg_ret);

void ff_es_jpeg_dec_disable_all_pp_shaper(struct DecConfig *config);

void ff_es_jpeg_dec_release_ext_buffers(VSVDECContext *dec_ctx);

uint32_t ff_es_jpeg_dec_find_empty_index(VSVDECContext *dec_ctx);

void ff_es_jpeg_dec_performance_report(AVCodecContext *avctx);

int ff_es_jpeg_dec_set_buffer_number_for_trans(AVCodecContext *avctx);

void jpeg_dec_set_default_dec_config(AVCodecContext *avctx);

enum DecRet jpegdec_consumed(void* inst, struct DecPicturePpu *pic);

int ff_esdec_get_next_picture(AVCodecContext *avctx, AVFrame *frame);

enum  ESJpegCode ff_es_jpeg_decoder(AVCodecContext *avctx, struct DecInputParameters *jpeg_in, AVFrame *frame);

int jpeg_init(VSVDECContext *dec_ctx);

enum DecRet jpeg_dec_get_info(VSVDECContext *dec_ctx);

enum DecRet jpeg_dec_set_info(const void *inst, struct DecConfig config);

enum DecRet jpeg_get_buffer_info(void *inst, struct DecBufferInfo *buf_info);

enum DecRet jpeg_decode(AVCodecContext *avctx,void* inst, struct DecInputParameters* jpeg_in);

enum DecRet jpeg_next_picture(AVCodecContext *avctx,const void *inst, struct DecPicturePpu *pic);

void ff_es_jpeg_dwl_output_mempry_free(void *opaque, uint8_t *data);

int ff_es_jpeg_output_buffer_fd_split(void *dwl_inst,
                                      void *dec_inst,
                                      ESJpegOutputMemory *memory,
                                      struct DecConfig *dec_config);

int ff_es_jpeg_allocate_output_buffer(VSVDECContext *dec_ctx,
                                      struct DecBufferInfo *buf_info);

int ff_es_jpeg_allocate_input_buffer(VSVDECContext *dec_ctx);

int ff_es_jpeg_dec_modify_thum_config_by_sequence_info(AVCodecContext *avctx);

int ff_es_jpeg_dec_modify_config_by_sequence_info(AVCodecContext *avctx);

void ff_es_jpeg_dec_init_log_header(AVCodecContext *avctx);

int ff_es_jpeg_dec_init_ppu_cfg(AVCodecContext *avctx,struct DecConfig *config);

void ff_es_jpeg_dec_init_dec_input_paras(AVPacket *avpkt,
                                         VSVDECContext *dec_ctx,
                                         struct DecInputParameters *jpeg_in);

void ff_es_jpeg_dec_update_dec_input_paras(VSVDECContext *dec_ctx,
                                           struct DecInputParameters *jpeg_in);

void ff_es_jpeg_dec_print_image_info(AVCodecContext *avctx, struct DecSequenceInfo * image_info);

void ff_es_jpeg_ppu_print(struct DecConfig *config);

void ff_es_pri_picture_info_free(void *opaque, uint8_t *data);

void ff_es_data_free(void *opaque, uint8_t *data);

void ff_es_jpeg_set_ppu_output_format(VSVDECContext *dec_ctx, struct DecConfig *config);

int ff_es_jpeg_parse_ppset(VSVDECContext *dec_ctx, CropInfo *crop_info, ScaleInfo *scale_info);

int ff_es_jpeg_set_ppu_crop_and_scale(VSVDECContext *dec_ctx, struct DecConfig *config);

bool check_scale_value( uint32_t v);

int ff_es_jpeg_get_align(uint32_t stride);

int ff_es_jpeg_dec_paras_check(AVCodecContext *avctx);

int ff_es_check_pixfmt(enum AVPixelFormat pixfmt);

int ff_es_jpeg_init_frame_dump_handle(VSVDECContext *dec_ctx);

int ff_es_jpeg_init_pkt_dump_handle(VSVDECContext *dec_ctx);

int ff_es_jpeg_frame_dump(VSVDECContext *dec_ctx);

int ff_es_jpeg_pkt_dump(VSVDECContext *dec_ctx);

int ff_codec_dump_data_to_file_by_decpicture(struct DecPicture *pic, DumpHandle *dump_handle);

int ff_es_dec_drop_pkt(AVCodecContext *avctx, AVPacket *avpkt);

/* func for low latency feature */
void ff_es_jpeg_wait_for_task_completion(task_handle task);

uint32_t ff_es_jpeg_low_latency_task_init(VSVDECContext *dec_ctx);

task_handle ff_es_jpeg_run_task(task_func func, void* param);

void* ff_es_jpeg_send_bytestrm_task(void* param);

uint32_t ff_es_jpeg_find_imagedata(u8 * p_stream, u32 stream_length);

#endif
