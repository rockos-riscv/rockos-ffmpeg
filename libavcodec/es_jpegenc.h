
/*
 * Copyright (C) 2022 Eswin
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

#ifndef AVCODEC_ES_JPEGENC_H
#define AVCODEC_ES_JPEGENC_H

#include <stdio.h>         //printf
#include "esenc_common.h"  //thread unit

/* Maximum lenght of the file path */
#ifndef MAX_PATH
#define MAX_PATH 512
#endif

#define MB_SIZE_16 16
#define MB_SIZE_8 8

/* Quantization tables for luminance, levels 0-9 */
static const u8 NonRoiFilterLuminance[10][64] = {

    {255, 255, 255, 63, 0,  0,  0, 0, 255, 255, 255, 63, 0, 0, 0, 0, 255, 255, 255, 63, 0, 0,
     0,   0,   63,  63, 63, 63, 0, 0, 0,   0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0,  0, 0,
     0,   0,   0,   0,  0,  0,  0, 0, 0,   0,   0,   0,  0, 0, 0, 0, 0,   0,   0,   0},

    {255, 255, 255, 127, 0,   0,   0, 0, 255, 255, 255, 127, 0, 0, 0, 0, 255, 255, 255, 127, 0, 0,
     0,   0,   127, 127, 127, 127, 0, 0, 0,   0,   0,   0,   0, 0, 0, 0, 0,   0,   0,   0,   0, 0,
     0,   0,   0,   0,   0,   0,   0, 0, 0,   0,   0,   0,   0, 0, 0, 0, 0,   0,   0,   0},

    {255, 255, 255, 255, 7,   7,   7, 7, 255, 255, 255, 127, 7, 7, 7, 7, 255, 255, 255, 127, 7, 7,
     7,   7,   255, 127, 127, 127, 7, 7, 7,   7,   7,   7,   7, 7, 7, 7, 7,   7,   7,   7,   7, 7,
     7,   7,   7,   7,   7,   7,   7, 7, 7,   7,   7,   7,   7, 7, 7, 7, 7,   7,   7,   7},

    {255, 255, 255, 255, 63,  15,  15, 15, 255, 255, 255, 255, 63, 15, 15, 15, 255, 255, 255, 255, 31, 15,
     15,  15,  255, 255, 255, 255, 31, 15, 15,  15,  63,  63,  63, 31, 31, 15, 15,  15,  15,  15,  15, 15,
     15,  15,  15,  15,  15,  15,  15, 15, 15,  15,  15,  15,  15, 15, 15, 15, 15,  15,  15,  15},

    {255, 255, 255, 255, 127, 15,  15,  15, 255, 255, 255, 255, 127, 15,  15, 15, 255, 255, 255, 255, 127, 15,
     15,  15,  255, 255, 255, 255, 127, 15, 15,  15,  127, 127, 127, 127, 63, 15, 15,  15,  15,  15,  15,  15,
     15,  15,  15,  15,  15,  15,  15,  15, 15,  15,  15,  15,  15,  15,  15, 15, 15,  15,  15,  15},

    {255, 255, 255, 255, 255, 63,  31,  31, 255, 255, 255, 255, 255, 63,  31,  31, 255, 255, 255, 255, 255, 31,
     31,  31,  255, 255, 255, 255, 255, 31, 31,  31,  255, 255, 255, 255, 255, 31, 31,  31,  63,  63,  31,  31,
     31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31},

    {255, 255, 255, 255, 255, 255, 31,  31,  255, 255, 255, 255, 255, 255, 31,  31, 255, 255, 255, 255, 255, 255,
     31,  31,  255, 255, 255, 255, 255, 127, 31,  31,  255, 255, 255, 255, 127, 63, 31,  31,  255, 255, 255, 127,
     63,  63,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31},

    {255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255, 255, 255,
     31,  63,  255, 255, 255, 255, 255, 255, 31,  63,  255, 255, 255, 255, 255, 255, 31,  63,  255, 255, 255, 255,
     255, 255, 31,  63,  127, 127, 31,  31,  31,  31,  31,  63,  63,  63,  63,  63,  63,  63,  63,  63},

    {255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255, 255, 255,
     127, 63,  255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255, 255, 255, 127, 63,  255, 255, 255, 255,
     255, 255, 63,  63,  127, 127, 127, 127, 127, 63,  63,  63,  63,  63,  63,  63,  63,  63,  63,  63},

    {255, 255, 255, 255, 255, 255, 255, 127, 255, 255, 255, 255, 255, 255, 255, 127, 255, 255, 255, 255, 255, 255,
     255, 127, 255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255,
     255, 255, 255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  127, 127, 127, 63,  63,  63,  63,  63}};

/* Quantization tables for chrominance, levels 0-9 */
static const u8 NonRoiFilterChrominance[10][64] = {
    {255, 255, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0,   0,   0, 0, 0, 0, 0, 0, 0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},

    {255, 255, 31, 0, 0, 0, 0, 0, 255, 255, 31, 0, 0, 0, 0, 0, 31, 31, 31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0,   0,   0,  0, 0, 0, 0, 0, 0,   0,   0,  0, 0, 0, 0, 0, 0,  0,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},

    {255, 255, 255, 31, 7,  7,  7, 7, 255, 255, 255, 31, 7, 7, 7, 7, 255, 255, 255, 31, 7, 7,
     7,   7,   31,  31, 31, 31, 7, 7, 7,   7,   7,   7,  7, 7, 7, 7, 7,   7,   7,   7,  7, 7,
     7,   7,   7,   7,  7,  7,  7, 7, 7,   7,   7,   7,  7, 7, 7, 7, 7,   7,   7,   7},

    {255, 255, 255, 255, 31,  15,  15, 15, 255, 255, 255, 255, 31, 15, 15, 15, 255, 255, 255, 255, 31, 15,
     15,  15,  255, 255, 255, 255, 31, 15, 15,  15,  31,  31,  31, 31, 31, 15, 15,  15,  15,  15,  15, 15,
     15,  15,  15,  15,  15,  15,  15, 15, 15,  15,  15,  15,  15, 15, 15, 15, 15,  15,  15,  15},

    {255, 255, 255, 255, 127, 15,  15, 15, 255, 255, 255, 255, 63, 15, 15, 15, 255, 255, 255, 255, 63, 15,
     15,  15,  255, 255, 255, 255, 63, 15, 15,  15,  127, 63,  63, 63, 63, 15, 15,  15,  15,  15,  15, 15,
     15,  15,  15,  15,  15,  15,  15, 15, 15,  15,  15,  15,  15, 15, 15, 15, 15,  15,  15,  15},

    {255, 255, 255, 255, 255, 63,  31,  31, 255, 255, 255, 255, 255, 63,  31,  31, 255, 255, 255, 255, 255, 31,
     31,  31,  255, 255, 255, 255, 255, 31, 31,  31,  255, 255, 255, 255, 255, 31, 31,  31,  63,  63,  31,  31,
     31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31},

    {255, 255, 255, 255, 255, 255, 31,  31, 255, 255, 255, 255, 255, 255, 31,  31, 255, 255, 255, 255, 255, 255,
     31,  31,  255, 255, 255, 255, 255, 63, 31,  31,  255, 255, 255, 255, 255, 63, 31,  31,  255, 255, 255, 63,
     63,  63,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31,  31,  31,  31,  31, 31,  31,  31,  31},

    {255, 255, 255, 255, 255, 255, 31,  31,  255, 255, 255, 255, 255, 255, 31,  31,  255, 255, 255, 255, 255, 255,
     31,  31,  255, 255, 255, 255, 255, 255, 31,  31,  255, 255, 255, 255, 255, 255, 31,  31,  255, 255, 255, 255,
     255, 255, 31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31,  31},

    {255, 255, 255, 255, 255, 255, 127, 35,  255, 255, 255, 255, 255, 255, 127, 35,  255, 255, 255, 255, 255, 255,
     127, 35,  255, 255, 255, 255, 255, 255, 127, 35,  255, 255, 255, 255, 255, 255, 63,  35,  255, 255, 255, 255,
     255, 255, 63,  35,  127, 127, 127, 127, 63,  63,  63,  35,  35,  35,  35,  35,  35,  35,  35,  35},

    {255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255, 255, 255,
     255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  255, 255, 255, 255,
     255, 255, 255, 63,  255, 255, 255, 255, 255, 255, 255, 63,  63,  63,  63,  63,  63,  63,  63,  63}};

typedef struct EsJpegEncParams {
    /**Parameters affecting input frame and encoded frame resolutions and cropping:*/
    i32 width;                   /**Width of output image.*/
    i32 height;                  /**Height of output image.*/
    i32 lumWidthSrc;             /**Width of source image. */
    i32 lumHeightSrc;            /**Height of source image.*/
    i32 horOffsetSrc;            /**Output image horizontal offset.*/
    i32 verOffsetSrc;            /**Output image vertical offset.*/
    i32 restartInterval;         /**Restart interval in MCU rows.*/
    JpegEncFrameType frameType;  //@see JpegEncFrameType: JPEGENC_YUV420_PLANAR

    /**Parameters for RoiMap*/
    i32 roi_enable;
    char *roimapFile;   /**Input file for roimap region. [jpeg_roimap.roi]  */
    char *nonRoiFilter; /**Input file for nonroimap region filter. [filter.txt]*/
    i32 nonRoiLevel;    // non_roi filter level (0 - 9)

    i32 colorConversion; /**RGB to YCbCr color conversion type.
                           0 - ITU-R BT.601, RGB limited [16...235] (BT601_l.mat)
                           1 - ITU-R BT.709, RGB limited [16...235] (BT709_l.mat)
                           2 - User defined, coefficients defined in test bench.
                           3 - ITU-R BT.2020
                           4 - ITU-R BT.601, RGB full [0...255] (BT601_f.mat)
                           5 - ITU-R BT.601, RGB limited [0...219] (BT601_219.mat)
                           6 - ITU-R BT.709, RGB full [0...255] (BT709_f.mat) */

    JpegEncPictureRotation rotation; /**Rotate input image.1:90 degrees right,
                                      2:90 degrees left, 3:180 degrees*/

    JpegEncCodingType partialCoding; /**0=whole frame, 1=partial frame encoding. */

    i32 codingMode; /**@see JpegEncCodingMode: JPEGENC_420_MODE,0=YUV420, 1=YUV422, 2=Monochrome [0]
                     */
    i32 markerType; /**@see JpegEncTableMarkerType: JPEGENC_SINGLE_MARKER, Quantization/Huffman
                       table markers.0 = Single marker 1 = Multiple markers*/

    i32 qLevel;        // Quantization level (0 - 9)
    char *qTablePath;  // User defined qtable path

    i32 unitsType;  //@JpegEncAppUnitsType :JPEGENC_DOTS_PER_CM
    i32 xdensity;   // Horizontal pixel density @JpegEncAppUnitsType
    i32 ydensity;   // Vertical pixel density @JpegEncAppUnitsType

    /** \brief  thumbnail info */
    char *inputThumb;     // thumbnail path
    i32 thumbnailFormat;  // thumbnail format 0:disable 1:JPEG 2:RGB8 3:RGB24
    i32 widthThumb;
    i32 heightThumb;

    /** \brief Comment header file path,It was add to file header
     *  Ex: Eswin Jpeg encoder
     */
    i32 comLength;  // Length of COM header
    char com[MAX_PATH];

    /* low latency */
    i32 inputLineBufMode;
    i32 inputLineBufDepth;
    u32 amountPerLoopBack;
    u32 hashtype;

    /** \brief horizontal mirror 0:disable mirror 1:enable mirror*
     *   Ex: When using the camera to take a selfie, set mirror =1
     */
    i32 mirror;

    /**
     *-1..4 Convert YUV420 to customer private format. [-1]
      -1 - No conversion to customer private format
       0 - customer private tile format for HEVC
       1 - customer private tile format for H.264
       2 - customer private YUV422_888
       3 - common data 8-bit tile 4x4
       4 - common data 10-bit tile 4x4
       5 - customer private tile format for JPEG
     */
    i32 formatCustomizedType;

    /** Enable/Disable set chroma to a constant pixel value.*/
    i32 constChromaEn;
    u32 constCb;  // 0..255. The constant pixel value for Cb. [128]
    u32 constCr;  // 0..255. The constant pixel value for Cr. [128]

    /** Losless setup(It doesn't work after modification)*/
    i32 losslessEnable;  // UNUSED
    i32 predictMode;     // 1~7 - Enalbe lossless with prediction select mode n
    i32 ptransValue;     //  0..7 Point transform value for lossless encoding. [0]

    /** \brief those params Motion JPEG*/
    /* jpeg Rate control*/
    u32 bitPerSecond;   /**Target bit per second. 0 - RC OFF, none zero - RC ON */
    u32 frameRateNum;   /**1..1048575 Output picture rate numerator.*/
    u32 frameRateDenom; /**1..1048575 Output picture rate denominator. */
    i32 rcMode;         /**0..2, JPEG/MJPEG RC mode. 0 = single frame RC mode.1 = video RC with CBR.
                        2 = video RC with VBR.*/
    i32 picQpDeltaMin;  /**Qp Delta range in picture-level rate control.
                        Min: -10..-1 Minimum Qp_Delta in picture RC. [-2]
                        Max:  1..10  Maximum Qp_Delta in picture RC. [3]
                        This range only applies to two neighboring frames. */
    i32 picQpDeltaMax;
    u32 qpmin;   // 0..51, Minimum frame qp. [0]
    u32 qpmax;   // 0..51, Maxmum frame qp. [51]
    i32 fixedQP; /**-1..51, Fixed qp for every frame. [-1], -1   = disable fixed qp mode*/

    u32 exp_of_input_alignment; /** Alignment value of input frame buffer. 0 = Disable
                                   alignment  4..12 = Base address of input frame buffer and each
                                   line are aligned to 2^inputAlignmentExp*/
    u32 streamBufChain;         /**Enable two output stream buffers. 0 - Single output stream buffer 1 - Two
                                   output stream buffers chained together.*/

    /**Parameters affecting stream multi-segment output**/
    u32 streamMultiSegmentMode;   /**0..2 Stream multi-segment mode control.0 = Disable stream
                                     multi-segment.1 = Enable. No SW handshaking. Loop-back enabled.
                                     2 = Enable. SW handshaking. Loop-back enabled.*/
    u32 streamMultiSegmentAmount; /** 2..16. the total amount of segments to control
                                     loopback/sw-handshake/IRQ. */

    char dec400CompTableinput[MAX_PATH];
    u64 dec400FrameTableSize;

    /* AXI alignment */
    u32 AXIAlignment;

    /* irq Type mask */
    u32 irqTypeMask;

    /**Parameters for OSD overlay controls*/
    char olInput[MAX_OVERLAY_NUM][MAX_PATH];
    u32 overlayEnables;
    u32 olFormat[MAX_OVERLAY_NUM];
    u32 olAlpha[MAX_OVERLAY_NUM];
    u32 olWidth[MAX_OVERLAY_NUM];
    u32 olCropWidth[MAX_OVERLAY_NUM];
    u32 olHeight[MAX_OVERLAY_NUM];
    u32 olCropHeight[MAX_OVERLAY_NUM];
    u32 olXoffset[MAX_OVERLAY_NUM];
    u32 olCropXoffset[MAX_OVERLAY_NUM];
    u32 olYoffset[MAX_OVERLAY_NUM];
    u32 olCropYoffset[MAX_OVERLAY_NUM];
    u32 olYStride[MAX_OVERLAY_NUM];
    u32 olUVStride[MAX_OVERLAY_NUM];
    u32 olBitmapY[MAX_OVERLAY_NUM];
    u32 olBitmapU[MAX_OVERLAY_NUM];
    u32 olBitmapV[MAX_OVERLAY_NUM];
    u32 olSuperTile[MAX_OVERLAY_NUM];
    u32 olScaleWidth[MAX_OVERLAY_NUM];
    u32 olScaleHeight[MAX_OVERLAY_NUM];
    char osdDec400CompTableInput[MAX_PATH];

    // Mosaic area
    u32 mosaicEnables;
    u32 mosXoffset[MAX_MOSAIC_NUM];
    u32 mosYoffset[MAX_MOSAIC_NUM];
    u32 mosWidth[MAX_MOSAIC_NUM];
    u32 mosHeight[MAX_MOSAIC_NUM];

    /* SRAM power down mode disable */
    u32 sramPowerdownDisable;

    i32 useVcmd;
    i32 useDec400;
    i32 useL2Cache;
    i32 useAXIFE;

    /*AXI max burst length */
    u32 burstMaxLength;
} EsJpegEncParams;

typedef struct EsJpegEncodeContext {
    const AVClass *class;
    // common fields of ThreadContext
    void *tc;

#ifdef FB_SYSLOG_ENABLE
    LOG_INFO_HEADER log_header;
#endif
    char module_name[20];

    JpegEncCfg enc_config;
    JpegEncInst enc_inst;
    JpegEncIn enc_input;
    JpegEncOut enc_output;

    EsJpegEncParams jpeg_enc_params;    /* params set to codec*/
    EsJpegEncParams jpeg_option_params; /* params set by user @AVOption*/

    /* SW/HW shared memories for input/output buffers */
    EWLLinearMem_t input_buf_mem;
    EWLLinearMem_t outbufMem[MAX_STRM_BUF_NUM];

    /* ROI */
    EWLLinearMem_t roimapMem;
    bool roi_buffer_alloced;
    u32 roi_memory_size;
    u32 roi_enable;

    bool encoder_is_open;

    /* low latency (input line buffer) */
    int32_t input_line_buf_mode;
    inputLineBufferCfg lineBufCfg;

    /*  Sliced Frame Coding index*/
    i32 sliceIdx;

    /*  Thumbnail info*/
    bool thumbnail_enable;
    JpegEncThumb jpeg_thumb;
    u8 *thumb_data; /* thumbnail data buffer */

    /*  when quantization qLevel=9 user defined qTable saved*/
    u8 qtable_luma[64];
    u8 qtable_chroma[64];

    /*  Crop info*/
    char *crop_str;

    // other fields
    AVCodecContext *avctx;

    u32 out_stream_size; /**< \brief Size of output stream in bytes */

    uint32_t stream_multi_segment_mode; /* VS 9000EJ not support */
    uint32_t stream_multi_segment_amount;

    struct timeval time_frame_start;
    struct timeval time_frame_end;

    JpegEncRateCtrl rate_ctrl;

    AVHWFramesContext *hwdevice;

    i32 mb_width;  /**Width of source image of mb. */
    i32 mb_height; /**Height of source image of mb.*/
    int compress_rate; // for allocate output buffer as compress rate

    AVFrame *frame;
} EsJpegEncodeContext;

typedef enum
{
    ESJENC_THUMBNAIL_FORMAT_INVALID = 0,   // invalid
    ESJENC_THUMBNAIL_FORMAT_JPEG = 0x10,   // jpeg
    ESJENC_THUMBNAIL_FORMAT_RGB8 = 0x11,   // rgb 8
    ESJENC_THUMBNAIL_FORMAT_RGB24 = 0x13,  // rgb 24
} JencThumbnailFormat;

typedef struct {
    JencThumbnailFormat format; /**< \brief Format of the thumbnail */
    unsigned int width;         /**< \brief Width in pixels of thumbnail */
    unsigned int height;        /**< \brief Height in pixels of thumbnail */
    void *data;                 /**< \brief Thumbnail data */
    unsigned int data_length;   /**< \brief Data amount in bytes */
} SideDataThumbnail;

/**REGIONS OF INTEREST**/
typedef struct {
    int x;
    int y;
    int width;
    int height;
} SideDataRoiArea;

typedef struct {
    unsigned int non_roi_qp_level;
    int non_roi_filter_map[128];
} SideDataJencNonRoiFilter;

/** Params Function*/
/**initialization the codec’s params*/
JpegEncRet es_jenc_init_params(AVCodecContext *avctx);

/**Private function ,print the params info*/
static void es_jenc_print_params(AVCodecContext *avctx, EsJpegEncParams *params);

/**Get Current the codec’s params, */
JpegEncRet es_jenc_get_params(AVCodecContext *avctx, EsJpegEncParams *params);

/**Set the codec’s params, If some params you want to fix , call es_jenc_get_params first*/
JpegEncRet es_jenc_set_params(AVCodecContext *avctx, EsJpegEncParams *params);

av_cold int ff_es_jpeg_encode_init(AVCodecContext *avctx);

av_cold int ff_es_jpeg_encode_close(AVCodecContext *avctx);

int ff_es_jpeg_encode_send_frame(AVCodecContext *avctx, const AVFrame *frame);

int ff_es_jpeg_encode_receive_packet(AVCodecContext *avctx, AVPacket *avpkt);

int ff_es_jpeg_encode_encode2(AVCodecContext *avctx, AVPacket *avpkt, const AVFrame *frame, int *got_packet);

void es_get_aligned_pic_size_by_format(
    JpegEncFrameType type, u32 width, u32 height, u32 alignment, u64 *luma_Size, u64 *chroma_Size, u64 *picture_Size);
#endif
