#ifndef AVCODEC_ESDEC_API_H__
#define AVCODEC_ESDEC_API_H__

#include <libavutil/opt.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include "avcodec.h"
#include "dectypes.h"
#include "es_common.h"
#include "esdec_common.h"
#include "es_codec_private.h"

struct AVCodecContext;
struct AVBuffer;
struct ESQueue;
struct ESInputPort;
struct ESOutputPort;
struct ReorderPkt;

typedef struct ESVDECContext ESVDECContext;

typedef int (*StoreReorderPktFunction)(ESVDECContext *dec_ctx, struct ReorderPkt *reorder_pkt);
typedef int (*GetReorderPktFunction)(ESVDECContext *dec_ctx, int pic_id, struct ReorderPkt *out_pkt);

struct ESVDECContext {
    const AVClass *class;

    int stride_align;
    int target_pp;
    int drop_frame_interval;
    char *scale;
    int pp_enabled[ES_VID_DEC_MAX_OUT_COUNT];
    char *crop[ES_VID_DEC_MAX_OUT_COUNT];
    int32_t pp_fmt[ES_VID_DEC_MAX_OUT_COUNT];
    int32_t input_buf_num;

    int extra_hw_frames;

    int32_t hw_conceal;
    int32_t disable_slice;

    int packet_dump;
    char *dump_path;
    int packet_dump_time;
    int dump_pkt_count;
    int dump_frame_count[2];
    int pmode;
    int fmode;
    DumpHandle *dump_pkt_handle;
    DumpHandle *dump_frm_handle[ES_VID_DEC_MAX_OUT_COUNT];

    int frame_dump[ES_VID_DEC_MAX_OUT_COUNT];
    int frame_dump_time[ES_VID_DEC_MAX_OUT_COUNT];

    ESDecState state;
    ESVDecInst dec_inst;
    void *dwl_inst;
    struct AVBufferRef *dwl_ref;
    int dev_mem_fd;           // for dev fd split

    ESDecCodec codec;
    int pic_width;
    int pic_height;
    int bit_depth;

    uint32_t got_package_number;
    uint32_t pic_decode_number;
    uint32_t pic_display_number;
    uint32_t pic_output_number;

    int pp_count;
    struct DecInitConfig init_config;
    struct DecConfig dec_config;
    struct DecPicturePpu *picture;

    struct ReorderPkt *reorder_pkt;
    struct ESQueue *reorder_queue;
    StoreReorderPktFunction store_reorder_pkt;
    GetReorderPktFunction get_reorder_pkt_by_pic_id;

    struct ESInputPort *input_port;
    struct ESOutputPort *output_port;
    struct AVBufferRef *input_port_ref;
    struct AVBufferRef *output_port_ref;

    AVPacket pkt;
    AVFrame *frame;
    AVBufferRef *hwdevice;
    AVBufferRef *hwframe;

    int64_t reordered_opaque;

    pthread_t tid;
};

void es_decode_print_version_info(ESVDECContext *dec_ctx);
int es_decode_init(ESVDECContext *dec_ctx);
int es_decode_set_params(ESVDECContext *dec_ctx, ESDecCodec codec);
int es_decode_get_frame(ESVDECContext *dec_ctx, AVFrame *frame, int timeout_ms);
int es_decode_start(ESVDECContext *dec_ctx);
int es_decode_close(ESVDECContext *dec_ctx);
int es_decode_flush(ESVDECContext *dec_ctx);
int es_decode_send_packet(ESVDECContext *dec_ctx, AVPacket *pkt, int timeout);
int es_decode_send_packet_receive_frame(ESVDECContext *dec_ctx, AVPacket *pkt, AVFrame *frame);

#endif