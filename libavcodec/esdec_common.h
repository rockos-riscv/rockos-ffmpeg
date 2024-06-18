#ifndef AVCODEC_ESDEC_COMMON_H__
#define AVCODEC_ESDEC_COMMON_H__
#include <dectypes.h>
#include <libavutil/pixfmt.h>
#include "es_codec_private.h"
#include "es_common.h"

#define IS_PIC_8BIT_FMT(fmt) ( \
  (fmt) == DEC_OUT_FRM_YUV420SP || \
  (fmt) == DEC_OUT_FRM_YUV420P || \
  (fmt) == DEC_OUT_FRM_YUV400 || \
  (fmt) == DEC_OUT_FRM_NV21SP || \
  (fmt) == DEC_OUT_FRM_RGB888 || \
  (fmt) == DEC_OUT_FRM_BGR888 || \
  (fmt) == DEC_OUT_FRM_ARGB888 || \
  (fmt) == DEC_OUT_FRM_ABGR888 || \
  (fmt) == DEC_OUT_FRM_XRGB888 || \
  (fmt) == DEC_OUT_FRM_XBGR888)

  #define IS_PIC_RGB_FMT(fmt) ( \
  (fmt) == DEC_OUT_FRM_RGB888 || \
  (fmt) == DEC_OUT_FRM_BGR888 || \
  (fmt) == DEC_OUT_FRM_R16G16B16 || \
  (fmt) == DEC_OUT_FRM_B16G16R16 || \
  (fmt) == DEC_OUT_FRM_ARGB888 || \
  (fmt) == DEC_OUT_FRM_ABGR888 || \
  (fmt) == DEC_OUT_FRM_A2R10G10B10 || \
  (fmt) == DEC_OUT_FRM_A2B10G10R10 || \
  (fmt) == DEC_OUT_FRM_XRGB888 || \
  (fmt) == DEC_OUT_FRM_RGB888_P || \
  (fmt) == DEC_OUT_FRM_BGR888_P || \
  (fmt) == DEC_OUT_FRM_R16G16B16_P || \
  (fmt) == DEC_OUT_FRM_B16G16R16_P || \
  (fmt) == DEC_OUT_FRM_XBGR888)

#define NUM_OF_STREAM_BUFFERS (5)
#define ES_DEFAULT_STREAM_BUFFER_SIZE (1024 * 1024)  // 1MB
#define JPEG_DEFAULT_INPUT_MIN_BUFFERS (1)
#define JPEG_DEFAULT_OUTPUT_MIN_BUFFERS (1)
#define JPEG_DEFAULT_INPUT_BUFFERS (5)
#define JPEG_DEFAULT_OUTPUT_BUFFERS (5)
#define JPEG_DEFAULT_INPUT_MAX_BUFFERS (18)
#define JPEG_DEFAULT_OUTPUT_MAX_BUFFERS (18)
#define MAX_BUFFERS 78
#define MAX_STRM_BUFFERS 18

typedef enum _ESDecState
{
    ESDEC_STATE_UNINIT = 0,
    ESDEC_STATE_STARTED,
    ESDEC_STATE_STOPPING,
    ESDEC_STATE_STOPPED,
    ESDEC_STATE_FLUSHING,
    ESDEC_STATE_FLUSHED,
    ESDEC_STATE_CLOSED
} ESDecState;

typedef void *ESVDecInst;
typedef void *ESJDecInst;

enum DecPictureFormat ff_codec_pixfmt_to_decfmt(enum AVPixelFormat pixfmt);
enum AVPixelFormat ff_codec_decfmt_to_pixfmt(enum DecPictureFormat picfmt);
void ff_esdec_set_ppu_output_pixfmt(int is_8bits, enum AVPixelFormat pixfmt, PpUnitConfig *ppu_cfg);
const char *ff_codec_decfmt_to_char(enum DecPictureFormat picfmt);
const char *ff_codec_pixfmt_to_char(enum AVPixelFormat pixfmt);
const char *esdec_get_ppout_enable(int pp_out, int pp_index);

DecPicAlignment esdec_get_align(int stride);
void esdec_fill_planes(OutPutInfo *info, struct DecPicture *picture);
int32_t ff_codec_compute_size(struct DecPicture *pic);
int ff_codec_dump_data_to_file_by_decpicture(struct DecPicture *pic, DumpHandle *dump_handle);
#endif