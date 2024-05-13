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

#ifndef AVCODEC_ESENC_VIDEO_H
#define AVCODEC_ESENC_VIDEO_H

#include <stdio.h>

#ifndef ESW_FF_ENHANCEMENT
#include "dectypes.h"
#endif

#include "instance.h"
#include "hevcencapi.h"
#include "enccommon.h"
#include "es_common.h"
#ifdef SUPPORT_TCACHE
#include "dtrc_api.h"
#endif
#include "libavutil/fifo.h"
#include "libavutil/timestamp.h"
#include "hevcencapi.h"
#include "enccommon.h"
#include "ewl.h"
#include "encinputlinebuffer.h"
#include "esqueue.h"
#include "esenc_common.h"

#ifndef NEXT_MULTIPLE
#define NEXT_MULTIPLE(value, n) (((value) + (n)-1) & ~((n)-1))
#endif

#define DEFAULT -255
#define MAX_BPS_ADJUST 20
#define MAX_STREAMS 16
#define MAX_SCENE_CHANGE 20

#define DEFAULT_VALUE -255

#define MAX_FIFO_DEPTH 16
#define MAX_WAIT_DEPTH 78  // 34
#define MAX_ENC_NUM 4
#define MAX_GOP_LEN 300

#define DEFAULT_VALUE -255

// roi region number
#define MAX_ROI_NUM (8)

// ipcm number
#define MAX_IPCM_NUM (2)

#define RESOLUTION_TO_CTB(var, ctu) ((var) / (ctu))

#define STRIDE_BY_ALIGN(var, ctu) (((var) + (ctu)-1) & ~((ctu)-1))

#ifndef MAX_GOP_SIZE
#define MAX_GOP_SIZE 8
#endif
#ifndef MAX_DELAY_NUM
#define MAX_DELAY_NUM (MAX_CORE_NUM + MAX_GOP_SIZE)
#endif

#ifndef MOVING_AVERAGE_FRAMES
#define MOVING_AVERAGE_FRAMES 120
#endif

typedef struct {
    uint32_t stream_pos;
    uint32_t multislice_encoding;
    uint32_t output_byte_stream;
    FILE *out_stream_file;
} SliceCtl;

typedef struct {
    uint32_t stream_rd_counter;
    uint32_t stream_multi_seg_en;
    uint8_t *stream_base;
    uint32_t segment_size;
    uint32_t segment_amount;
    FILE *out_stream_file;
    uint8_t start_code_done;
    int32_t output_byte_stream;
} SegmentCtl;

typedef struct {
    int32_t frame[MOVING_AVERAGE_FRAMES];
    int32_t length;
    int32_t count;
    int32_t pos;
    int32_t frame_rate_numer;
    int32_t frame_rate_denom;
} Ma;

typedef struct {
    int gop_frm_num;
    double sum_intra_vs_interskip;
    double sum_skip_vs_interskip;
    double sum_intra_vs_interskipP;
    double sum_intra_vs_interskipB;
    int sum_costP;
    int sum_costB;
    int last_gop_size;
} AdapGopCtr;

typedef struct {
#ifdef FB_SYSLOG_ENABLE
    LOG_INFO_HEADER log_header;
#endif
    FILE *out;
    int32_t width;
    int32_t height;
    int32_t output_rate_numer; /* Output frame rate numerator */
    int32_t output_rate_denom; /* Output frame rate denominator */
    int32_t first_pic;
    int32_t last_pic;
    int32_t input_pic_cnt;
    int32_t picture_enc_cnt;      // count that had send to encoder api
    int32_t picture_encoded_cnt;  // count that encoder had encoded frame
    int32_t idr_interval;
    int32_t last_idr_picture_cnt;
    int32_t byte_stream;
    uint8_t *lum;
    uint8_t *cb;
    uint8_t *cr;
    uint32_t src_img_size_ds;
    int32_t interlaced_frame;
    uint32_t valid_encoded_frame_number;
    uint32_t input_alignment;
    uint32_t ref_alignment;
    uint32_t ref_ch_alignment;
    int32_t format_customized_type;
    uint32_t transformed_size;
    uint32_t luma_size_lookahead;
    uint32_t chroma_size_lookahead;

    /* SW/HW shared memories for input/output buffers */
    EWLLinearMem_t *picture_mem;
    EWLLinearMem_t *outbuf_mem[MAX_STRM_BUF_NUM];
    EWLLinearMem_t *roi_map_delta_qp_mem;

    EWLLinearMem_t picture_mem_factory[MAX_DELAY_NUM];
    uint8_t picture_mem_status[MAX_DELAY_NUM];  // maintain the status of picture_mem_factory buffer list, 0: can reuse,
                                                // 1: using by encoder
    EWLLinearMem_t outbuf_mem_factory[MAX_CORE_NUM][MAX_STRM_BUF_NUM]; /* [coreIdx][bufIdx] */
    EWLLinearMem_t roi_map_delta_qp_mem_factory[MAX_DELAY_NUM];
    uint8_t roi_qp_map_mem_status[MAX_DELAY_NUM];

    float sum_square_of_error;
    float average_square_of_error;
    int32_t max_error_over_target;
    int32_t max_error_under_target;
    long number_square_of_error;

    uint32_t gop_size;
    int32_t next_gop_size;
    VCEncIn enc_in;

    inputLineBufferCfg input_ctb_line_buf;

    uint32_t parallel_core_num;
    SliceCtl slice_ctl_factory[MAX_DELAY_NUM];
    SliceCtl *slice_ctl;
    SliceCtl *slice_ctl_out;

    const void *ewl;
    const void *two_pass_ewl;

    int enc_index;
    double ssim_acc;
    long long hwcycle_acc;

#if defined(SUPPORT_DEC400) || defined(SUPPORT_TCACHE)
    uint32_t tbl_luma_size;
    uint32_t tbl_chroma_size;
#endif
#ifdef DRV_NEW_ARCH
    int priority;
    char *device;
    int mem_id;
#endif
    int32_t stream_buf_num;
    uint32_t frame_delay;
    uint32_t buffer_cnt;
    SegmentCtl stream_seg_ctl;
    uint32_t roi_map_version;
    char *cu_map_buf;
    u_int32_t cu_map_buf_len;
    ESQueue *in_mem_queue;
    pthread_mutex_t in_mem_queue_mutex;
    int8_t share_fd_buf;
    ESQueue *dts_queue;
    pthread_mutex_t dts_queue_mutex;
    int compress_rate;  // for allocate output buffer as compress rate
    int picture_size;
    int picture_buffer_allocated;  // a flag presents if the piture buffer had alloced
} ESEncVidInternalContext;

typedef struct {
    unsigned int slice_size;
} SideDataSliceSize;

typedef struct {
    unsigned int force_idr;
} SideDataForeceIdr;

typedef struct {
    unsigned int insert_sps_pps;
} SideDataInsertSpsPps;

typedef struct {
    unsigned int rc_mode;
    unsigned int rc_window;
    unsigned int bitrate;
    unsigned int min_qp;
    unsigned int max_qp;
    unsigned int fixed_qp_i;
    unsigned int fixed_qp_p;
    unsigned int fixed_qp_b;
} SideDataRc;

typedef enum {
    CU_BLK_64x64,
    CU_BLK_32x32,
    CU_BLK_16x16,
    CU_BLK_8x8,  // H264 not support
} CuBlockUint;

typedef struct {
    CuBlockUint blk_unit;  // block unit
    char skip;             // 1: skip cu
    char is_abs_qp;        // is absolute qp value
    char qp_value;         // qp value. is_abs=1: [-51, 51], is_abs=0: [-30, 0]
} SideDataCuMap;

typedef enum {
    VSV_PRESET_NONE,
    VSV_PRESET_SUPERFAST,
    VSV_PRESET_FAST,
    VSV_PRESET_MEDIUM,
    VSV_PRESET_SLOW,
    VSV_PRESET_SUPERSLOW,
    VSV_PRESET_NUM
} VSVPreset;

struct statistic {
    uint32_t frame_count;
    uint32_t cycle_mb_avg;
    uint32_t cycle_mb_avg_p1;
    uint32_t cycle_mb_avg_total;
    double ssim_avg;
    uint32_t bitrate_avg;
    uint32_t hw_real_time_avg;
    uint32_t hw_real_time_avg_remove_overlap;
    int32_t total_usage;
    int32_t core_usage_counts[4];
    struct timeval last_frame_encoded_timestamp;
};

typedef struct {
    unsigned char index;     // index, coresponding with 0 ~ (MAX_ROI_NUM-1)/roi1~roi8 area
    unsigned char enable;    // 1: enable, 0: disable
    unsigned char is_absQp;  // 1: absolute QP, 0: relative QP
    int qp;                  // QP value, if is_absQp == 1, [0, 51]; else [-30, 0]
    unsigned int x;          // x
    unsigned int y;          // y
    unsigned int width;      // width
    unsigned int height;     // height
} RoiAttr;

typedef struct {
    unsigned int num_of_roi;
    RoiAttr roi_attr[MAX_ROI_NUM];
} RoiParas;

typedef struct {
    unsigned char hdr10_display_enable;  // 1: enable, 0: disable
    unsigned int hdr10_dx0;      // Component 0 normalized x chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_dy0;      // Component 0 normalized y chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_dx1;      // Component 1 normalized x chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_dy1;      // Component 1 normalized y chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_dx2;      // Component 2 normalized x chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_dy2;      // Component 2 normalized y chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_wx;       // White piont normalized x chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_wy;       // White piont normalized y chromaticity coordinates range:[0, 50000]; Default:0
    unsigned int hdr10_maxluma;  // Nominal maximum display luminance Default:0
    unsigned int hdr10_minluma;  // Nominal minimum display luminance Defaul:0
} Hdr10DisplayAttr;

typedef struct {
    unsigned char hdr10_lightlevel_enable;  // 1: enable, 0: disable
    unsigned int hdr10_maxlight;            // Max content light level
    unsigned int hdr10_avglight;            // Max picture average light level
} Hdr10LightAttr;

typedef struct {
    unsigned char hdr10_color_enable;  // 1: enable, 0: disable
    unsigned int hdr10_primary;        // primary - Index of chromaticity coordinates range:[0, 9]; Default:9
    unsigned int hdr10_matrix;         // Index of matrix coefficients range:[0, 9]; Default:9
    unsigned int hdr10_transfer;       // 0-ITU-R BT.2020; 1-SMPTE ST 2084 2-ARIB STD-B67
} Hdr10ColorAttr;

typedef struct {
    unsigned char index;   // index, coresponding with 0 ~ (MAX_ROI_NUM-1)/roi1~roi8 area
    unsigned char enable;  // 1: enable, 0: disable
    unsigned int x;        // x-coordinate of the upper-left
    unsigned int y;        // y-coordinate of the upper-left
    unsigned int width;    // x-coordinate of the lower-right
    unsigned int height;   // y-coordinate of the lower-right
} IpcmAttr;

typedef struct {
    unsigned int num_of_ipcm;
    IpcmAttr ipcm_attr[MAX_IPCM_NUM];
} IpcmParas;

typedef struct {
    const AVClass *class;
    // common fields of ThreadContext
    void *tc;
    // other fields
    AVCodecContext *avctx;
    ESEncVidInternalContext in_ctx;
    char module_name[20];
    int internal_enc_index;

    VCEncInst encoder;
    VCEncOut encOut;

    VCEncGopPicConfig gop_pic_cfg[MAX_GOP_PIC_CONFIG_NUM];
    VCEncGopPicConfig gop_pic_cfg_pass2[MAX_GOP_PIC_CONFIG_NUM];
    VCEncGopPicSpecialConfig gop_pic_special_cfg[MAX_GOP_SPIC_CONFIG_NUM];

    VCEncRateCtrl rc;

    bool encoder_is_open;
    int encoder_is_start;
    bool encoder_is_end;
    // bool EncoderFlushPic;

    VCEncIn enc_in_bk;
    int picture_cnt_bk;

    AdapGopCtr agop;
    bool adaptive_gop;
    uint8_t *p_user_data;
    int next_poc;
    VCEncPictureCodingType next_coding_type;
    int32_t next_gop_size;
    AVVSVDeviceContext *hwdevice;
    uint8_t *dev_name;

    int64_t frame_numbers;
    int32_t bframe_delay_time;
    int32_t bstart_output;
    int32_t out_pkt_numbers;
    AVFifoBuffer *output_pkt_queue;
    int32_t poc;
    /* param for performance */
    Ma ma;
    uint64_t total_bits;
    uint32_t frame_cnt_total;
    uint32_t frame_cnt_output;

    /* param pass from ffmpeg */
    int32_t pic_channel;
    char *preset;
    int bitdepth;
    // User options.
    int32_t output_rate_numer; /* output frame rate numerator */
    int32_t output_rate_denom; /* output frame rate denominator */
    int32_t width;
    int32_t height;
    int32_t lum_width_src;
    int32_t lum_height_src;

    int32_t input_format;
    int32_t input_format_lookahead;
    int32_t format_customized_type; /*change general format to customized one*/
    int32_t byte_stream;

    int32_t max_cu_size;                /* max coding unit size in pixels */
    int32_t min_cu_size;                /* min coding unit size in pixels */
    int32_t max_tr_size;                /* max transform size in pixels */
    int32_t min_tr_size;                /* min transform size in pixels */
    int32_t tr_depth_intra;             /* max transform hierarchy depth */
    int32_t tr_depth_inter;             /* max transform hierarchy depth */
    VCEncVideoCodecFormat codec_format; /* video codec format: hevc/h264/av1 */

    int32_t min_qp_size;

    int32_t enable_cabac; /* [0,1] h.264 entropy coding mode, 0 for cavlc, 1 for cabac */
    int32_t cabac_init_flag;

    // intra setup
    uint32_t strong_intra_smoothing_enabled_flag;

    int32_t cir_start;
    int32_t cir_interval;

    int32_t intra_area_enable;
    int32_t intra_area_left;
    int32_t intra_area_top;
    int32_t intra_area_right;
    int32_t intra_area_bottom;

    int32_t pcm_loop_filter_disabled_flag;

    int32_t ipcm1_area_left;
    int32_t ipcm1_area_top;
    int32_t ipcm1_area_right;
    int32_t ipcm1_area_bottom;

    int32_t ipcm2_area_left;
    int32_t ipcm2_area_top;
    int32_t ipcm2_area_right;
    int32_t ipcm2_area_bottom;

    int32_t ipcm3_area_left;
    int32_t ipcm3_area_top;
    int32_t ipcm4_area_right;
    int32_t ipcm4_area_bottom;

    int32_t ipcm5_area_left;
    int32_t ipcm5_area_top;
    int32_t ipcm5_area_right;
    int32_t ipcm5_area_bottom;

    int32_t ipcm6_area_left;
    int32_t ipcm6_area_top;
    int32_t ipcm6_area_right;
    int32_t ipcm6_area_bottom;

    int32_t ipcm7_area_left;
    int32_t ipcm7_area_top;
    int32_t ipcm7_area_right;
    int32_t ipcm7_area_bottom;

    int32_t ipcm8_area_left;
    int32_t ipcm8_area_top;
    int32_t ipcm8_area_right;
    int32_t ipcm8_area_bottom;

    int32_t ipcm_map_enable;
    char *ipcm_map_file;
    // ipcm option config
    char *ipcm_str;
    // save roi config
    IpcmParas ipcm_tbl;

    char *skip_map_file;
    int32_t skip_map_enable;
    int32_t skip_map_block_unit;
    // roi option config
    char *roi_str;
    // save roi config
    RoiParas roi_tbl;
    /* rate control parameters */
    rcMode_e rc_mode;
    int32_t hrd_conformance;
    int32_t cpb_size;
    int32_t intra_pic_rate; /* idr interval */

    int32_t vbr; /* variable bit rate control by qp_min */
    int32_t qp_hdr;
    int32_t qp_min;
    int32_t qp_max;
    int32_t qp_min_i;
    int32_t qp_max_i;
    int32_t bit_per_second;
    int32_t crf; /*crf constant*/

    int32_t bit_var_range_i;

    int32_t bit_var_range_p;

    int32_t bit_var_range_b;
    uint32_t u32_static_scene_ibit_percent;

    int32_t tol_moving_bit_rate; /*tolerance of max moving bit rate */
    int32_t monitor_frames;      /*monitor frame length for moving bit rate*/
    int32_t pic_rc;
    int32_t ctb_rc;
    int32_t block_rc_size;
    uint32_t rc_qp_delta_range;
    uint32_t rc_base_mb_complexity;
    int32_t pic_skip;
    int32_t pic_qp_delta_min;
    int32_t pic_qp_delta_max;
    int32_t ctb_rc_row_qp_step;

    float tol_ctb_rc_inter;
    float tol_ctb_rc_intra;

    int32_t bitrate_window;
    int32_t intra_qp_delta;
    int32_t fixed_qp_I;
    int32_t fixed_qp_P;
    int32_t fixed_qp_B;
    int32_t b_frame_qp_delta;

    int32_t enable_deblocking;
    int32_t enable_sao;
    int32_t max_TLayers;  // SVC-T max layers
    int32_t b_nums;       // B frame numbers within GOP
    int32_t num_refP;
    int32_t force_IDR;
    int32_t insert_AUD;
    int32_t insert_SPS_PPS;

    int32_t tc_offset;
    int32_t beta_offset;

    int32_t chroma_qp_offset;

    int32_t profile; /*main profile or main still picture profile*/
    int32_t tier;    /*main tier or high tier*/
    int32_t level;   /*main profile level*/

    int32_t bps_adjust_frame[MAX_BPS_ADJUST];
    int32_t bps_adjust_bitrate[MAX_BPS_ADJUST];
    int32_t smooth_psnr_in_gop;

    int32_t slice_size;

    int32_t test_id;

    int32_t rotation;
    int32_t mirror;
    int32_t hor_offset_src;
    int32_t ver_offset_src;
    int32_t color_conversion;
    /*  Crop info*/
    char *crop_str;

    int32_t enable_deblock_override;
    int32_t deblock_override;

    int32_t enable_scaling_list;

    uint32_t compressor;

    int32_t interlaced_frame;
    int32_t field_order;
    int32_t video_range;
    int32_t ssim;
    int32_t enable_sei;
    char *user_data;
    uint32_t gop_size;
    char *gop_cfg;
    uint32_t gop_lowdelay;
    int32_t out_recon_frame;
    uint32_t long_term_gap;
    uint32_t long_term_gap_offset;
    uint32_t ltr_interval;
    int32_t long_term_qp_delta;

    int32_t gdr_duration;
    uint32_t roi_map_delta_qp_block_unit;
    uint32_t roi_map_delta_qp_enable;
    char *roi_map_delta_qp_file;
    char *roi_map_delta_qp_bin_file;
    char *roi_map_info_bin_file;
    char *roimap_cu_ctrl_info_bin_file;
    char *roimap_cu_ctrl_index_bin_file;
    uint32_t roi_cu_ctrl_ver;
    uint32_t roi_qp_delta_ver;
    int32_t out_buf_size_max;
    int32_t multimode;  // multi-stream mode, 0--disable, 1--mult-thread, 2--multi-process
    char *streamcfg[MAX_STREAMS];
    int32_t outfile_format;  // 0->hevc, 1->h264, 2->vp9
    uint32_t enable_output_cu_info;

    uint32_t rdo_level;
    /* low latency */
    int32_t input_line_buf_mode;
    int32_t input_line_buf_depth;
    int32_t amount_per_loop_back;

    uint32_t hashtype;
    uint32_t verbose;

    /* constant chroma control */
    int32_t const_chroma_en;
    uint32_t const_cb;
    uint32_t const_cr;

    int32_t scene_change[MAX_SCENE_CHANGE];

    /* for tile*/
    int32_t tiles_enabled_flag;
    int32_t num_tile_columns;
    int32_t num_tile_rows;
    int32_t loop_filter_across_tiles_enabled_flag;

    /*for skip frame encoding ctr*/
    int32_t skip_frame_enabled_flag;
    int32_t skip_frame_poc;

    /*stride*/
    uint32_t exp_of_input_alignment;
    uint32_t exp_of_ref_alignment;
    uint32_t exp_of_ref_ch_alignment;

    uint32_t rps_in_slice_header;
    uint32_t p010_ref_enable;
    uint32_t enable_vui;

    uint32_t pic_order_cnt_type;
    uint32_t log2_max_pic_order_cnt_lsb;
    uint32_t log2_max_frame_num;

    uint32_t cutree_blkratio;
    int16_t gmv[2][2];
    char *gmv_file_name[2];
    char *half_ds_input;

    uint32_t parallel_core_num;

    uint32_t dump_register;
    uint32_t rasterscan;

    uint32_t stream_buf_chain;
    uint32_t lookahead_depth;
    uint32_t stream_multi_segment_mode;
    uint32_t stream_multi_segment_amount;
    // add for new driver
#ifdef DRV_NEW_ARCH
    int priority;
    char *device;
    int mem_id;
#endif
    /*hdr10*/
    char *hdr_display_str;
    char *hdr_light_str;
    char *hdr_color_str;

    /*save hdr10 cfg*/
    Hdr10DisplayAttr hdr10_display;
    Hdr10LightAttr hdr10_light;
    Hdr10ColorAttr hdr10_color;

    // dump streaming and frame
    char *dump_path;

    int32_t dump_frame_enable;
    int32_t dump_frame_time;
    DumpHandle *dump_frame_hnd;
    int32_t dump_frame_count;

    int32_t dump_pkt_enable;
    int32_t dump_pkt_time;
    DumpHandle *dump_pkt_hnd;
    int32_t dump_pkt_count;

    time_t dump_start_time;

    AVFrame *frame;
} ESEncVidContext;

av_cold int ff_vsv_h26x_encode_init(AVCodecContext *avctx);

av_cold int ff_vsv_h26x_encode_close(AVCodecContext *avctx);

int ff_vsv_h26x_encode_send_frame(AVCodecContext *avctx, const AVFrame *frame);

int ff_vsv_h26x_encode_receive_packet(AVCodecContext *avctx, AVPacket *avpkt);

int ff_vsv_h26x_encode_encode2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet);

#endif
