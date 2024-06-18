#define LOG_TAG "esdecapi"
#include <sys/prctl.h>
#include "avcodec.h"
#include <hevcdecapi.h>
#include <h264decapi.h>
#include "vcdecapi.h"
#include "esdecapi.h"
#include "es_common.h"
#include "eslog.h"
#include "esqueue.h"
#include "esdecbuffer.h"
#include "esdec_internal.h"
#include "esdec_wrapper.h"

#ifdef MODEL_SIMULATION
#include <deccfg.h>
extern u32 g_hw_build_id;
extern u32 g_hw_id;
extern u32 g_hw_ver;
#endif

static int esdec_get_next_picture(ESVDECContext *dec_ctx);
static int es_decode_flush_process(ESVDECContext *dec_ctx);

static int esdec_reorder_packet_enqueue(ESVDECContext *dec_ctx, ReorderPkt *pkt) {
    int ret = FAILURE;
    if (!dec_ctx || !pkt) {
        return FAILURE;
    }

    ret = es_reorder_packet_enqueue(dec_ctx->reorder_queue, pkt);
    return ret;
}

static int esdec_reorder_pkt_dequeue(ESVDECContext *dec_ctx, int pic_id, struct ReorderPkt *out_pkt) {
    int ret;
    if (!dec_ctx || !out_pkt) {
        return FAILURE;
    }

    ret = es_reorder_packet_dequeue(dec_ctx->reorder_queue, pic_id, out_pkt);
    return ret;
}

static int esdec_reorder_pkt_store(ESVDECContext *dec_ctx, ReorderPkt *pkt) {
    if (!dec_ctx || !pkt || !dec_ctx->reorder_pkt) {
        log_error(dec_ctx, "dec_ctx or pkt is null dec_ctx; %p, pkt: %p\n", dec_ctx, pkt);
        return FAILURE;
    }

    *dec_ctx->reorder_pkt = *pkt;
    return SUCCESS;
}

static int esdec_get_reorder_pkt(ESVDECContext *dec_ctx, int pic_id, struct ReorderPkt *out_pkt) {
    if (!dec_ctx || !out_pkt || !dec_ctx->reorder_pkt) {
        log_error(dec_ctx, "dec_ctx  or out_pkt is null dec_ctx: %p, out_pkt; %p\n", dec_ctx, out_pkt);
        return FAILURE;
    }

    *out_pkt = *dec_ctx->reorder_pkt;
    return SUCCESS;
}

static void esdec_report_decode_info(ESVDECContext *dec_ctx) {
    struct DecPicturePpu *pic = NULL;
    struct DecPicture *picture;
    if (!dec_ctx || !dec_ctx->picture) {
        log_error(NULL, "esvdec  dec_ctx or picture is null dec_ctx: %p\n", dec_ctx);
        return;
    }

    pic = dec_ctx->picture;
    if (pic) {
        log_warn(NULL,
                 "original resolution: %dx%d, %dbit stream, stride_align: %d, output_num: %u, decode_num: %d\n",
                 dec_ctx->pic_width,
                 dec_ctx->pic_height,
                 dec_ctx->bit_depth,
                 dec_ctx->stride_align,
                 dec_ctx->pic_output_number,
                 dec_ctx->pic_display_number);
        for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
            picture = &pic->pictures[i];
            if (picture->luma.virtual_address) {
                log_warn(NULL,
                         "pp%d resolution: %dx%d, pic_stride: %d, pixpmt: %s, target_pp: %s\n",
                         i,
                         picture->pic_width,
                         picture->pic_height,
                         picture->pic_stride,
                         ff_codec_decfmt_to_char(picture->picture_info.format),
                         esdec_get_ppout_enable(dec_ctx->target_pp, i));
            }
        }
    }
}

static int esdec_set_init_params(ESVDECContext *dec_ctx, ESDecCodec codec) {
    struct DecInitConfig *config;
    struct DWLInitParam dwl_params = {DWL_CLIENT_TYPE_HEVC_DEC};
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return FAILURE;
    }

#ifdef MODEL_SIMULATION
    g_hw_build_id = 0x1FB2;
    g_hw_ver = 19001;
    g_hw_id = 1000;
#endif

    config = &dec_ctx->init_config;
    memset(config, 0, sizeof(struct DecInitConfig));

    dec_ctx->codec = codec;
    if (codec == ES_H264_H10P) {
        config->mvc = 0;
        config->rlc_mode = 0;
        config->mc_cfg.mc_enable = 0;
        if (config->mc_cfg.mc_enable) {
            config->mc_cfg.stream_consumed_callback = esdec_stream_buffer_consumed;
        }
        config->codec = DEC_H264_H10P;
        dwl_params.client_type = DWL_CLIENT_TYPE_H264_DEC;
    } else if (codec == ES_HEVC) {
        config->mc_cfg.mc_enable = 0;
        if (config->mc_cfg.mc_enable) {
            config->mc_cfg.stream_consumed_callback = esdec_stream_buffer_consumed;
        }
        config->codec = DEC_HEVC;
        dwl_params.client_type = DWL_CLIENT_TYPE_HEVC_DEC;
    } else {
        log_error(dec_ctx, "not support codec: %d\n", codec);
        return FAILURE;
    }

    config->full_stream_mode = 0;
    config->disable_picture_reordering = 0;
    config->use_ringbuffer = 0;
    config->use_video_compressor = 0;
    config->decoder_mode = DEC_NORMAL;  // DEC_INTRA_ONLY or DEC_NORMAL;
    config->num_frame_buffers = 0;
    config->auxinfo = 0;


    config->guard_size = dec_ctx->extra_hw_frames;
    config->use_adaptive_buffers = 1;
    config->dwl_inst = DWLInit(&dwl_params);
    if (!config->dwl_inst) {
        log_error(dec_ctx, "DWLInit failed\n");
        return FAILURE;
    }
    dec_ctx->dwl_inst = config->dwl_inst;
    dec_ctx->dwl_ref = av_buffer_create(
        (uint8_t *)dec_ctx->dwl_inst, sizeof(dec_ctx->dwl_inst), esdec_dwl_release, NULL, AV_BUFFER_FLAG_READONLY);
    if (!dec_ctx->dwl_ref) {
        esdec_dwl_release(NULL, dec_ctx->dwl_inst);
        dec_ctx->dwl_inst = NULL;
        log_error(dec_ctx, "av_buffer_create dwl ref failed\n");
        return FAILURE;
    }

    if (config->decoder_mode == DEC_INTRA_ONLY || config->disable_picture_reordering) {
        dec_ctx->reorder_pkt = (ReorderPkt *)av_mallocz(sizeof(*dec_ctx->reorder_pkt));
        if (!dec_ctx->reorder_pkt) {
            log_error(dec_ctx, "reorder_pkt malloc failed\n");
            return FAILURE;
        }
        dec_ctx->store_reorder_pkt = esdec_reorder_pkt_store;
        dec_ctx->get_reorder_pkt_by_pic_id = esdec_get_reorder_pkt;

    } else {
        dec_ctx->reorder_queue = es_queue_create();
        if (!dec_ctx->reorder_queue) {
            log_error(dec_ctx, "reorder_queue create failed\n");
            return FAILURE;
        }
        dec_ctx->store_reorder_pkt = esdec_reorder_packet_enqueue;
        dec_ctx->get_reorder_pkt_by_pic_id = esdec_reorder_pkt_dequeue;
    }

    dec_ctx->frame = av_frame_alloc();
    if (!dec_ctx->frame) {
        log_error(dec_ctx, "av_frame_alloc failed\n");
        return FAILURE;
    }

    dec_ctx->dump_pkt_handle = NULL;
    for (int i = 0; i < 2; i++) {
        dec_ctx->dump_frm_handle[i] = NULL;
    }

    log_info(dec_ctx, "esdec_set_init_params success dwl_inst: %p\n", dec_ctx->dwl_inst);
    return SUCCESS;
}

static void esdec_parse_pp_config(ESVDECContext *dec_ctx, struct DecConfig *config) {
    PpUnitConfig *ppu_cfg = NULL;
    if (!dec_ctx || !config) {
        log_error(dec_ctx, "dec_ctx or config is null dec_ctx: %p, config: %p\n", dec_ctx, config);
        return;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        CropInfo crop = {0};
        ScaleInfo scale = {0};

        ppu_cfg = &config->ppu_cfg[i];
        ppu_cfg->enabled = dec_ctx->pp_enabled[i];
        if (!ppu_cfg->enabled) {
            log_info(dec_ctx, "pp%d disabled\n", i);
            continue;
        } else {
            log_info(dec_ctx, "pp%d enabled\n", i);
        }

        if (dec_ctx->crop[i]) {
            log_info(dec_ctx, "i: %d, crop: %s\n", i, dec_ctx->crop[i]);
        }

        if (es_codec_get_crop(dec_ctx->crop[i], &crop) == 0) {
            if (crop.crop_xoffset >= 0 && crop.crop_yoffset >= 0 && crop.crop_height > 0 && crop.crop_width > 0) {
                ppu_cfg->crop.enabled = 1;
                ppu_cfg->crop.set_by_user = 1;
                ppu_cfg->crop.x = crop.crop_xoffset;
                ppu_cfg->crop.y = crop.crop_yoffset;
                ppu_cfg->crop.width = crop.crop_width;
                ppu_cfg->crop.height = crop.crop_height;
            }
            log_info(dec_ctx,
                     "crop index: %d, enabled: %d, cx: %d, cy: %d, cw: %d, ch: %d\n",
                     i,
                     ppu_cfg->crop.enabled,
                     crop.crop_xoffset,
                     crop.crop_yoffset,
                     crop.crop_width,
                     crop.crop_height);
        }

        if (i == 1 && (es_codec_get_scale(dec_ctx->scale, &scale) == 0)) {
            if ((scale.scale_height == 0 && scale.scale_width == 0)
                || (scale.scale_height == -1 && scale.scale_width == -1)) {
                ppu_cfg->scale.enabled = 0;
            } else {
                ppu_cfg->scale.enabled = 1;
                if (scale.scale_width <= -2 && scale.scale_height <= -2) {
                    ppu_cfg->scale.scale_by_ratio = 1;
                    ppu_cfg->scale.ratio_x = -scale.scale_width;
                    ppu_cfg->scale.ratio_y = -scale.scale_height;
                } else {
                    ppu_cfg->scale.width = scale.scale_width;
                    ppu_cfg->scale.height = scale.scale_height;
                }
            }

            log_info(dec_ctx,
                     "scale index: %d, enabled: %d, scale_width: %d, scale_height: %d\n",
                     i,
                     ppu_cfg->scale.enabled,
                     scale.scale_width,
                     scale.scale_height);
        }
    }
}

static void esdec_set_dec_params(ESVDECContext *dec_ctx) {
    struct DecConfig *dec_config = NULL;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return;
    }
    dec_config = &dec_ctx->dec_config;
    memset(dec_config, 0, sizeof(struct DecConfig));

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        PpUnitConfig *ppu_cfg = &dec_config->ppu_cfg[i];
        ppu_cfg->pp_filter = 1;
        ppu_cfg->video_range = 1;
        ppu_cfg->x_filter_param = 2;
        ppu_cfg->y_filter_param = 2;
    }

    dec_config->hw_conceal = dec_ctx->hw_conceal = 1;
    dec_config->disable_slice = dec_ctx->disable_slice = 0;
    dec_config->align = esdec_get_align(dec_ctx->stride_align);

    log_info(dec_ctx, "dec_config->align: %d\n", dec_config->align);
    // TODO config->delogo_params

    esdec_parse_pp_config(dec_ctx, dec_config);
}

static int es_decode_set_pkt_dump_params(ESVDECContext *dec_ctx) {
    DumpParas paras;
    if (dec_ctx->packet_dump && !dec_ctx->dump_pkt_handle) {
        paras.width = dec_ctx->pic_width;
        paras.height = dec_ctx->pic_height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "vdec";
        if (dec_ctx->codec == ES_HEVC)
            paras.suffix_name = "hevc";
        else if (dec_ctx->codec == ES_H264_H10P)
            paras.suffix_name = "h264_h10p";
        else
            paras.suffix_name = "h264";
        paras.fmt = NULL;
        dec_ctx->dump_pkt_handle = ff_codec_dump_file_open(dec_ctx->dump_path, dec_ctx->packet_dump_time, &paras);
    } else {
        log_info(dec_ctx, "packet_dump disable\n");
        return -1;
    }

    return 0;
}

static int es_decode_set_frame_dump_params(ESVDECContext *dec_ctx, struct DecPicturePpu *pic) {
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_ctx->pp_enabled[i] && dec_ctx->frame_dump[i] && !dec_ctx->dump_frm_handle[i]) {
            DumpParas paras;
            paras.width = pic->pictures[i].pic_width;
            paras.height = pic->pictures[i].pic_height;
            paras.pic_stride = pic->pictures[i].pic_stride;
            paras.pic_stride_ch = pic->pictures[i].pic_stride_ch;
            paras.prefix_name = "vdec";

            if (i == 0)
                paras.ppu_channel = "pp0";
            else
                paras.ppu_channel = "pp01";

            if (IS_PIC_RGB_FMT(pic->pictures[i].picture_info.format))
                paras.suffix_name = "rgb";
            else
                paras.suffix_name = "yuv";

            paras.fmt = ff_codec_decfmt_to_char(pic->pictures[i].picture_info.format);

            dec_ctx->dump_frm_handle[i] =
                ff_codec_dump_file_open(dec_ctx->dump_path, dec_ctx->frame_dump_time[i], &paras);
        }
    }

    return 0;
}

static int es_decode_frame_dump(ESVDECContext *dec_ctx, struct DecPicturePpu *pic) {
    int ret = 0;
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_ctx->pp_enabled[i] && dec_ctx->frame_dump[i] && dec_ctx->dump_frm_handle[i]) {
            if (dec_ctx->dump_frm_handle[i]->fp) {
                ret = ff_codec_dump_data_to_file_by_decpicture(&(pic->pictures[i]), dec_ctx->dump_frm_handle[i]);
                if (ret == ERR_TIMEOUT) {
                    av_log(NULL, AV_LOG_INFO, "frame dump timeout\n");
                    ff_codec_dump_file_close(&dec_ctx->dump_frm_handle[i]);
                    dec_ctx->frame_dump[i] = 0;
                    return 0;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "write file error\n");
                    return -1;
                }
            } else {
                av_log(NULL, AV_LOG_ERROR, "fp is not inited\n");
                return -1;
            }
        }
    }

    return 0;
}

static int es_decode_pkt_dump(ESVDECContext *dec_ctx, void *data, int size) {
    int ret = 0;
    if (dec_ctx->packet_dump) {
        if (dec_ctx->dump_pkt_handle) {
            ret = ff_codec_dump_bytes_to_file(data, size, dec_ctx->dump_pkt_handle);
            if (ret == ERR_TIMEOUT) {
                av_log(NULL, AV_LOG_INFO, "pkt dump timeout\n");
                ff_codec_dump_file_close(&dec_ctx->dump_pkt_handle);
                dec_ctx->packet_dump = 0;
                return 0;
            } else if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "write frame into file failed\n");
                return -1;
            }
        }
    }

    return 0;
}

int es_decode_set_params(ESVDECContext *dec_ctx, ESDecCodec codec) {
    int ret = FAILURE;
    struct DecInitConfig *config;
    if (!dec_ctx) {
        log_error(dec_ctx, "avctx is null\n");
        return FAILURE;
    }

    config = &dec_ctx->init_config;
    memset(config, 0, sizeof(struct DecInitConfig));

    ret = esdec_set_init_params(dec_ctx, codec);
    if (ret == FAILURE) {
        return ret;
    }
    esdec_set_dec_params(dec_ctx);

    es_decode_set_pkt_dump_params(dec_ctx);

    log_info(dec_ctx, "codec: %d set_decoder_params success\n", codec);
    return SUCCESS;
}

int es_decode_init(ESVDECContext *dec_ctx) {
    enum DecRet rv;
    int ret = FAILURE;
    ESInputPort *port;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return FAILURE;
    }

    rv = VCDecInit((const void **)&dec_ctx->dec_inst, &dec_ctx->init_config);
    if (rv == DEC_OK) {
        log_info(dec_ctx, "VCDecInit success\n");
    } else {
        log_error(dec_ctx, "VCDecInit failed\n");
        return ret;
    }

    if (ESDecInitDevMemFd(&dec_ctx->dev_mem_fd)) {
        log_error(dec_ctx, "init dev mem fd failed\n");
        return FAILURE;
    }

    port = esdec_allocate_input_port(dec_ctx->codec, dec_ctx->dwl_ref, NULL, dec_ctx->input_buf_num);
    if (port) {
        dec_ctx->input_port = port;
        ret = SUCCESS;
    } else {
        log_error(dec_ctx, "es_decode_allocate_input_port failed\n");
    }

    if (ret == SUCCESS) {
        pthread_condattr_t attr;
        dec_ctx->inited = TRUE;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&dec_ctx->cond, &attr);
        pthread_condattr_destroy(&attr);
        pthread_mutex_init(&dec_ctx->mutex, NULL);
    }

    return ret;
}

static int esdec_modify_config_by_sequence_info(ESVDECContext *dec_ctx) {
    int ret = FAILURE;
    enum DecRet rv;
    int dec_crop_enable = 0;
    struct DecConfig *config;
    struct DecSequenceInfo sequence_info = {0};
    if (!dec_ctx) {
        log_error(dec_ctx, "dectx is null\n");
        return FAILURE;
    }

    rv = VCDecGetInfo(dec_ctx->dec_inst, &sequence_info);
    if (rv != DEC_OK) {
        log_error(dec_ctx, "VCDecGetInfo failed\n");
        return FAILURE;
    }
    if (sequence_info.h264_base_mode == 1) {
        dec_ctx->init_config.use_ringbuffer = 0;
        dec_ctx->init_config.mc_cfg.mc_enable = 0;
        dec_ctx->init_config.mc_cfg.stream_consumed_callback = NULL;
    }

    dec_ctx->bit_depth = sequence_info.bit_depth_luma;

    if (sequence_info.crop_params.crop_left_offset != 0 || sequence_info.crop_params.crop_top_offset != 0
        || (sequence_info.crop_params.crop_out_width != sequence_info.pic_width
            && sequence_info.crop_params.crop_out_width != 0)
        || (sequence_info.crop_params.crop_out_height != sequence_info.pic_height
            && sequence_info.crop_params.crop_out_height != 0)) {
        dec_crop_enable = 1;
    }

    log_info(dec_ctx,
             "pic_width: %d, pic_height: %d, dec_crop_enable: %d, bit_depth: %d\n",
             sequence_info.pic_width,
             sequence_info.pic_height,
             dec_crop_enable,
             dec_ctx->bit_depth);

    config = &dec_ctx->dec_config;
    dec_ctx->pp_count = 0;
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        PpUnitConfig *ppu_cfg = &config->ppu_cfg[i];
        uint32_t original_width = sequence_info.pic_width;
        uint32_t original_height = sequence_info.pic_height;

        if (!ppu_cfg->enabled) {
            continue;
        }
        dec_ctx->pp_count++;

        if (dec_crop_enable) {
            original_width = (sequence_info.crop_params.crop_out_width + 1) & ~0x1;
            original_height = (sequence_info.crop_params.crop_out_height + 1) & ~0x1;
            if (!ppu_cfg->crop.enabled) {
                ppu_cfg->crop.x = sequence_info.crop_params.crop_left_offset;
                ppu_cfg->crop.y = sequence_info.crop_params.crop_top_offset;
                ppu_cfg->crop.width = original_width;
                ppu_cfg->crop.height = original_height;
            } else {
                ppu_cfg->crop.x += sequence_info.crop_params.crop_left_offset;
                ppu_cfg->crop.y += sequence_info.crop_params.crop_top_offset;
            }
            ppu_cfg->crop.enabled = 1;

            log_info(dec_ctx,
                     "crop index: %d, enabled: %d, cx: %d, cy: %d, cw: %d, ch: %d\n",
                     i,
                     ppu_cfg->crop.enabled,
                     ppu_cfg->crop.x,
                     ppu_cfg->crop.y,
                     ppu_cfg->crop.width,
                     ppu_cfg->crop.height);
        }

        dec_ctx->pic_width = original_width;
        dec_ctx->pic_height = original_height;

        if (ppu_cfg->crop.enabled) {
            if (ppu_cfg->crop.width > original_width || ppu_cfg->crop.width < 48
                || ppu_cfg->crop.height > original_height || ppu_cfg->crop.height < 48) {
                ppu_cfg->crop.enabled = 0;
            } else {
                original_width = ppu_cfg->crop.width;
                original_height = ppu_cfg->crop.height;
            }
        }

        if (ppu_cfg->scale.enabled) {
            if (ppu_cfg->scale.width == -1 && ppu_cfg->scale.height > 0) {
                ppu_cfg->scale.width = (original_width * ppu_cfg->crop.height / original_height) & ~0x1;
            }
            if (ppu_cfg->scale.height == -1 && ppu_cfg->scale.width > 0) {
                ppu_cfg->scale.height = (original_height * ppu_cfg->crop.width / original_width) & ~0x1;
            }

            if (ppu_cfg->scale.width > original_width || ppu_cfg->scale.height > original_height) {
                ppu_cfg->scale.enabled = 0;
            }
        }
        log_info(dec_ctx,
                 "i: %d, crop enable: %d, scale enable: %d, width: %d, height: %d\n",
                 i,
                 ppu_cfg->crop.enabled,
                 ppu_cfg->scale.enabled,
                 ppu_cfg->scale.width,
                 ppu_cfg->scale.height);

        ff_esdec_set_ppu_output_pixfmt(dec_ctx->bit_depth == 8, dec_ctx->pp_fmt[i], ppu_cfg);

        // es_decode_set_pkt_dump_params(dec_ctx);
    }

    rv = VCDecSetInfo(dec_ctx->dec_inst, config);
    if (rv != DEC_OK) {
        ret = FAILURE;
        log_error(dec_ctx, "VCDecSetInfo failed\n");
    } else {
        ret = SUCCESS;
        log_info(dec_ctx, "VCDecSetInfo success ppu_count: %d\n", dec_ctx->pp_count);
    }

    return ret;
}

static int esdec_decode(ESVDECContext *dec_ctx, InputBuffer *input_buffer, int *peek_frame) {
    int ret = SUCCESS;
    enum DecRet rv;
    int new_hdr = FALSE;
    ReorderPkt reorder_pkt;
    struct DWLLinearMem *buffer = NULL;
    struct DecInputParameters input_param;
    struct DecOutput dec_out = {0};

    if (!dec_ctx || !input_buffer || !peek_frame) {
        log_error(dec_ctx, "error dec_ctx: %p, buffer: %p\n", dec_ctx, input_buffer);
        return FAILURE;
    }
    *peek_frame = 0;
    log_debug(dec_ctx, "input_buffer->size: %d\n", input_buffer->size);

    reorder_pkt.pts = input_buffer->pts;
    reorder_pkt.reordered_opaque = input_buffer->reordered_opaque;

    dec_out.buff_size = input_buffer->max_size;
    dec_out.data_left = input_buffer->size;
    dec_out.strm_curr_pos = dec_out.strm_buff = (uint8_t *)input_buffer->vir_addr;
    dec_out.strm_buff_bus_address = input_buffer->bus_address;

    DWLmemset(&input_param, 0, sizeof(struct DecInputParameters));
    buffer = &input_param.stream_buffer;

    // dump packet
    es_decode_pkt_dump(dec_ctx, (void *)input_buffer->vir_addr, input_buffer->size);

    do {
        buffer->virtual_address = (uint32_t *)dec_out.strm_buff;
        buffer->bus_address = dec_out.strm_buff_bus_address;
        buffer->size = dec_out.buff_size;
        input_param.p_user_data = (void *)dec_ctx->input_port;
        input_param.stream = dec_out.strm_curr_pos;
        input_param.strm_len = dec_out.data_left;
        input_param.pic_id = dec_ctx->pic_decode_number;
        // input_param.sei_buffer = dec.sei_buffer; TODO

        rv = VCDecDecode(dec_ctx->dec_inst, &dec_out, &input_param);
        log_info(dec_ctx, "VCDecDecode ret: %d\n", rv);
        if (rv == DEC_HDRS_RDY) {
            new_hdr = TRUE;
            ret = esdec_modify_config_by_sequence_info(dec_ctx);
            if (ret < 0) {
                break;
            }
            if (dec_out.data_left <= 0) {
                break;
            }
        } else if (rv == DEC_WAITING_FOR_BUFFER) {
            if (!dec_ctx->output_port) {
                dec_ctx->output_port = esdec_allocate_output_port(
                    dec_ctx->codec, dec_ctx->dec_inst, dec_ctx->dev_mem_fd, dec_ctx->dwl_ref, dec_ctx->pp_count);

            } else {
                ret = esdec_output_port_change(dec_ctx->codec,
                                               dec_ctx->output_port,
                                               dec_ctx->dec_inst,
                                               dec_ctx->dev_mem_fd,
                                               dec_ctx->pp_count,
                                               new_hdr);
            }
            if (new_hdr) {
                new_hdr = FALSE;
            }
        } else if (rv == DEC_NO_DECODING_BUFFER) {
            ret = esdec_wait_all_pictures_consumed_unitl_timeout(
                dec_ctx->codec, dec_ctx->dec_inst, dec_ctx->output_port, 20 /*ms*/);
            if (ret < 0 && dec_ctx->state == ESDEC_STATE_FLUSHING) {
                ret = AVERROR(EAGAIN);
                *peek_frame = 0;
                break;
            } else if (ret == AVERROR_EXIT) {
                break;
            }
        } else if (rv == DEC_ADVANCED_TOOLS && dec_ctx->init_config.mc_cfg.mc_enable) {
            log_error(dec_ctx, "detected and not supported in multicore mode\n");
            break;
        } else if (rv == DEC_PIC_DECODED) {
            reorder_pkt.pic_id = dec_ctx->pic_decode_number;
            if (dec_ctx->store_reorder_pkt) {
                dec_ctx->store_reorder_pkt(dec_ctx, &reorder_pkt);
            }
            dec_ctx->pic_decode_number += 1;
            *peek_frame = 1;
            log_debug(dec_ctx, "pic_decode_number: %u\n", dec_ctx->pic_decode_number);
        } else if (rv == DEC_PENDING_FLUSH) {
            log_info(dec_ctx, "dec get all next pictures\n");
            do {
                ret = esdec_get_next_picture(dec_ctx);
                log_info(dec_ctx, "esdec_get_next_picture ret: %d\n", ret);
            } while (ret != FAILURE);
            log_info(NULL, "pending flush end\n");
        } else if (rv == DEC_ABORTED) {
            rv = VCDecAbortAfter(dec_ctx->dec_inst);
            log_info(dec_ctx, "VCDecAbortAfter rv: %d\n", rv);
            esdec_reset_output_memorys(dec_ctx->output_port);
            if (new_hdr) {
                new_hdr = FALSE;
            }
        } else if (rv < 0) {
            break;
        }

        esdec_timed_printf_memory_state(dec_ctx->output_port);

    } while (dec_out.data_left > 0);

    return ret;
}

static int ff_esdec_fill_frame_prop(ESVDECContext *dec_ctx, OutputBuffer *buffer, AVFrame *frame) {
    int ret = SUCCESS;
    DecPicturePri *pri_pic;
    OutPutInfo *info;
    uint64_t dma_fd;
    ESOutputMemory *memory;
    if (!dec_ctx || !buffer || !buffer->memory || !frame) {
        return FAILURE;
    }

    memory = buffer->memory;
    pri_pic = &memory->pic_pri;
    info = pri_pic->default_pic;

    frame->format = info->format;
    frame->width = info->width;
    frame->height = info->height;
    frame->key_frame = info->key_frame;
    frame->pts = buffer->pts;
    frame->reordered_opaque = buffer->reordered_opaque;
    for (int i = 0; i < info->n_planes; i++) {
        frame->data[i] = (uint8_t *)info->virtual_address + info->offset[i];
        frame->linesize[i] = info->stride[i];
    }
    if (ESDecIsSimulation()) {
        dma_fd = (uint64_t)info->virtual_address;
    } else {
        dma_fd = (uint64_t)info->fd;
    }
    ff_es_codec_add_fd_to_side_data(frame, dma_fd);

    frame->buf[0] = av_buffer_create(
        (uint8_t *)pri_pic, sizeof(*pri_pic), esdec_picture_consume, dec_ctx->output_port, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        ret = FAILURE;
        log_error(dec_ctx, "av_buffer_create frame[0] failed\n");
    }

    return ret;
}

static int esdec_drop_frame(ESVDECContext *dec_ctx, OutputBuffer *buffer, AVFrame *frame) {
    int ret = FAILURE;
    ESOutputPort *port;
    if (!dec_ctx || !dec_ctx->output_port || !frame || !buffer) {
        log_error(dec_ctx, "dec_ctx or frame or ppu is null\n");
        return FAILURE;
    }
    port = dec_ctx->output_port;

    frame->width = dec_ctx->pic_width;
    frame->height = dec_ctx->pic_height;
    frame->pts = buffer->pts;
    frame->reordered_opaque = buffer->reordered_opaque;
    frame->buf[0] = av_buffer_create(NULL, 0, NULL, dec_ctx, AV_BUFFER_FLAG_READONLY);
    frame->flags |= AV_FRAME_FLAG_DISCARD;

    esdec_release_buffer_to_consume_queue(port->consumed_queue, buffer);

    return ret;
}

static int esdec_check_discard_frame(uint32_t decode_num, int32_t drop_frame_interval) {
    int ret = FALSE;
    if (drop_frame_interval > 0) {
        ret = decode_num % drop_frame_interval ? TRUE : FALSE;
    }
    return ret;
}

static int esdec_fill_output_info(ESVDECContext *dec_ctx, struct DecPicturePpu *pic) {
    int ret = SUCCESS;
    int index = FAILURE;
    OutputBuffer *buffer = NULL;
    ESOutputMemory *memory;
    DecPicturePri *pri_pic = NULL;
    ESOutputPort *port;
    if (!dec_ctx || !pic || !dec_ctx->output_port) {
        log_error(dec_ctx, "error !!! dec_ctx: %p, pic: %p\n", dec_ctx, pic);
        return FAILURE;
    }
    port = dec_ctx->output_port;

    // dump frame
    es_decode_frame_dump(dec_ctx, pic);

    memory = esdec_find_memory_by_picture(port, pic);
    if (!memory) {
        log_error(dec_ctx, "esdec_find_memory_index_by_picture failed\n");
        return FAILURE;
    } else {
        log_debug(
            dec_ctx, "find memory vir_addr: %p, state: %s\n", memory->vir_addr, esdec_str_output_state(memory->state));
    }

    buffer = &memory->buffer;
    buffer->memory = memory;
    buffer->vir_addr = memory->vir_addr;
    buffer->buffer_ref = av_buffer_ref(memory->buffer_ref);
    buffer->port_ref = av_buffer_ref(memory->port_ref);

    memory->picture = *pic;
    pri_pic = &memory->pic_pri;
    pri_pic->hwpic = (void *)buffer;

    pri_pic->stride_align = dec_ctx->stride_align;
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (pic->pictures[i].luma.virtual_address != NULL) {
            pri_pic->pic_count++;
            pri_pic->pictures[i].fd = memory->fd[i];
            esdec_fill_planes(&pri_pic->pictures[i], &pic->pictures[i]);
            if (i == dec_ctx->target_pp) {
                pri_pic->default_index = i;
                pri_pic->default_pic = &pri_pic->pictures[i];
            } else if (index == -1) {
                index = i;
            }
            log_debug(dec_ctx, "index: %d, vir_addr: %p\n", i, pic->pictures[i].luma.virtual_address);
        } else if (pri_pic->pictures[i].enabled) {
            pri_pic->pictures[i].enabled = FALSE;
        }
    }

    if (pri_pic->pic_count > 0) {
        uint32_t pic_id;
        ReorderPkt pkt;
        if (!pri_pic->default_pic) {
            pri_pic->default_index = index;
            pri_pic->default_pic = &pri_pic->pictures[index];
            log_info(dec_ctx, "DEFAULT_INDEX: %d, real pic_index: %d\n", dec_ctx->target_pp, index);
            dec_ctx->target_pp = index;
        }

        pic_id = pic->pictures[pri_pic->default_index].picture_info.pic_id;
        if (dec_ctx->get_reorder_pkt_by_pic_id) {
            int ret = dec_ctx->get_reorder_pkt_by_pic_id(dec_ctx, pic_id, &pkt);
            if (ret == SUCCESS) {
                buffer->pts = pkt.pts;
                buffer->reordered_opaque = pkt.reordered_opaque;
            } else {
                log_error(dec_ctx, "get reorder pkt failed pic_id: %d\n", pic_id);
            }
        }

        if (!dec_ctx->picture) {
            dec_ctx->picture = av_mallocz(sizeof(*dec_ctx->picture));
            if (!dec_ctx->picture) {
                log_error(dec_ctx, "av_malloc picture fialed\n");
            } else {
                *dec_ctx->picture = *pic;
            }
        }

        esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_FRAME_QUEUE);
        ret = esdec_push_frame_output_buffer(port->frame_queue, buffer);
        if (ret == FAILURE) {
            esdec_set_output_buffer_state(memory, OUTPUT_MEMORY_STATE_ERROR);
            log_error(dec_ctx, "esdec_push_frame_output_buffer failed vir_addr: %p\n", buffer->vir_addr);
        } else {
            log_debug(dec_ctx, "esdec_push_frame_output_buffer vir_addr: %p\n", memory->vir_addr);
        }
    } else {
        log_error(dec_ctx, "no picture\n");
        return FAILURE;
    }

    if (ret == FAILURE) {
        av_buffer_unref(&buffer->port_ref);
        av_buffer_unref(&buffer->buffer_ref);
    }

    return ret;
}

static int esdec_get_next_picture(ESVDECContext *dec_ctx) {
    int ret;
    enum DecRet rv;
    struct DecPicturePpu pic;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return FAILURE;
    }

    rv = VCDecNextPicture(dec_ctx->dec_inst, &pic);
    if (rv == DEC_PIC_RDY) {
        es_decode_set_frame_dump_params(dec_ctx, &pic);
        ret = esdec_fill_output_info(dec_ctx, &pic);
        if (ret == FAILURE) {
            ESOutputMemory *memory;
            memory = esdec_find_memory_by_picture(dec_ctx->output_port, &pic);
            if (memory) {
                esdec_release_buffer_to_consume_queue(dec_ctx->output_port->consumed_queue, &memory->buffer);
            } else {
                VCDecPictureConsumed(dec_ctx->dec_inst, &pic);
            }
            log_error(dec_ctx, "fill output info failed\n");
        }
    } else {
        log_debug(dec_ctx, "VCDecNextPicture rv: %d\n", rv);
        ret = FAILURE;
    }

    return ret;
}

void es_decode_print_version_info(ESVDECContext *dec_ctx) {
    uint32_t n_cores;
    struct DecApiVersion dec_api;
    struct DecSwHwBuild dec_build;
#ifdef MODEL_SIMULATION
    // TODO solve compilation problem
    if (dec_ctx) {
        dec_build = HevcDecGetBuild();
    } else {
        dec_build = H264DecGetBuild();
    }
#else
    dec_build = VCDecGetBuild(DWL_CLIENT_TYPE_HEVC_DEC);
#endif

    dec_api = VCDecGetAPIVersion();
    dec_build = VCDecGetBuild(DWL_CLIENT_TYPE_HEVC_DEC);
    n_cores = VCDecMCGetCoreCount();

    log_info(dec_ctx,
             "Decoder API v%d.%d - SW build: %d.%d - HW build: 0x%x, %d cores\n",
             dec_api.major,
             dec_api.minor,
             dec_build.sw_build >> 16,
             dec_build.sw_build & 0xFFFF,
             dec_build.hw_build,
             n_cores);
}

int es_decode_send_packet(ESVDECContext *dec_ctx, AVPacket *pkt, int timeout) {
    int ret;
    int data_size = 0;
    InputBuffer buffer;
    ESInputPort *port;

    if (!dec_ctx || !dec_ctx->input_port) {
        return AVERROR(EINVAL);
    }
    port = dec_ctx->input_port;

    if (pkt) {
        data_size = pkt->size;
    }

    ret = esdec_get_input_buffer_unitl_timeout(port->release_queue, &buffer, timeout);
    if (ret == SUCCESS) {
        if (buffer.max_size < data_size) {
            ret = es_decode_realloc_input_memory(port, data_size + ES_DEFAULT_STREAM_BUFFER_SIZE, &buffer);
        }

        if (ret == SUCCESS) {
            buffer.size = data_size;
            if (pkt) {
                buffer.pts = pkt->pts;
                buffer.reordered_opaque = dec_ctx->reordered_opaque;
                memcpy((void *)buffer.vir_addr, pkt->data, data_size);
            }
            esdec_push_input_packet_buffer(port->packet_queue, &buffer);
        }
    }

    return ret;
}

int es_decode_send_packet_receive_frame(ESVDECContext *dec_ctx, AVPacket *pkt, AVFrame *frame) {
    int ret;
    if (!dec_ctx || !pkt || !frame) {
        log_error(dec_ctx, "error !!! dec_ctx or pkt or frame is null pkt: %p, frame: %p\n", pkt, frame);
        return FAILURE;
    }

    for (;;) {
        ret = es_decode_send_packet(dec_ctx, pkt, 20 /*20ms*/);
        if (ret == SUCCESS) {
            ret = es_decode_get_frame(dec_ctx, frame, 0);
            av_packet_unref(pkt);
            break;
        } else {
            ret = es_decode_get_frame(dec_ctx, frame, 0);
            if (ret == SUCCESS) {
                log_info(dec_ctx, "get frame data_left: %d\n", pkt->size);
                break;
            }
        }
    }

    if (frame->buf[0]) {
        ret = SUCCESS;
    } else {
        ret = AVERROR(EAGAIN);
    }

    return ret;
}

int es_decode_get_frame(ESVDECContext *dec_ctx, AVFrame *frame, int timeout_ms) {
    int ret;
    int is_discard;
    OutputBuffer buffer;
    ESOutputPort *port;
    if (!dec_ctx || !frame) {
        return AVERROR(EINVAL);
    }
    port = dec_ctx->output_port;
    if (!port) {
        return AVERROR(EAGAIN);
    }

    ret = esdec_get_output_frame_buffer(port->frame_queue, &buffer, timeout_ms);
    if (ret == SUCCESS) {
        is_discard = esdec_check_discard_frame(dec_ctx->pic_display_number, dec_ctx->drop_frame_interval);
        if (is_discard) {
            esdec_drop_frame(dec_ctx, &buffer, frame);
        } else {
            ret = ff_esdec_fill_frame_prop(dec_ctx, &buffer, frame);
            if (ret < 0) {
                esdec_release_buffer_to_consume_queue(port->consumed_queue, &buffer);
            } else {
                dec_ctx->pic_output_number++;
                esdec_set_output_buffer_state(buffer.memory, OUTPUT_MEMORY_STATE_FFMPEG);
            }
        }
        dec_ctx->pic_display_number++;
    }

    if (frame->buf[0]) {
        log_info(dec_ctx, "buffer vir_addr: %p\n", buffer.vir_addr);
        ret = SUCCESS;
    }

    return ret;
}

static void esdec_eos_process(ESVDECContext *dec_ctx) {
    int ret;
    if (!dec_ctx || !dec_ctx->output_port) {
        return;
    }
    log_info(dec_ctx, "VCDecEndOfStream start\n");
    VCDecEndOfStream(dec_ctx->dec_inst);

    for (;;) {
        ret = esdec_get_next_picture(dec_ctx);
        if (ret == FAILURE) {
            OutputBuffer buffer = {0};
            buffer.flags = OUTPUT_BUFFERFLAG_EOS;
            esdec_push_frame_output_buffer(dec_ctx->output_port->frame_queue, &buffer);
            log_info(dec_ctx, "push eos frames\n");
            break;
        }
    }
    log_info(dec_ctx, "end\n");
}

static void *esdec_decode_thread_run(void *ctx) {
    int ret = FAILURE;
    int end_stream = FALSE;
    int peek_frame;
    int abort_request = FALSE;
    int flushed = FALSE;
    InputBuffer buffer;
    ESInputPort *input_port;
    ESVDECContext *dec_ctx = (ESVDECContext *)ctx;
    if (!dec_ctx || !dec_ctx->input_port) {
        log_error(dec_ctx, "error !!! dec_ctx or input_port is null\n");
        return NULL;
    }

    prctl(PR_SET_NAME, "esvdec");
    input_port = dec_ctx->input_port;

    while (!abort_request) {
        if (dec_ctx->state == ESDEC_STATE_FLUSHING) {
            if (!flushed) {
                flushed = TRUE;
                es_decode_flush_process(dec_ctx);
            } else {
                usleep(10000);
            }
            continue;
        }
        if (flushed) {
            flushed = FALSE;
        }
        ret = esdec_get_input_packet_buffer(input_port->packet_queue, &buffer);
        if (ret < 0 && dec_ctx->state == ESDEC_STATE_FLUSHING) {
            continue;
        } else if (ret == AVERROR_EXIT) {
            log_info(dec_ctx, "decode thread will be exit\n");
            abort_request = TRUE;
            continue;
        } else if (ret < 0) {
            continue;
        }

        ret = esdec_wait_all_pictures_consumed_unitl_timeout(
            dec_ctx->codec, dec_ctx->dec_inst, dec_ctx->output_port, 0 /*without waiting*/);
        if (ret < 0 && dec_ctx->state == ESDEC_STATE_FLUSHING) {
            continue;
        } else if (ret == AVERROR_EXIT) {
            abort_request = TRUE;
            continue;
        }

        if (buffer.size <= 0) {
            esdec_eos_process(dec_ctx);
            end_stream = TRUE;
            esdec_release_input_buffer(input_port->release_queue, &buffer);
            continue;
        }

        ret = esdec_decode(dec_ctx, &buffer, &peek_frame);
        if (ret == AVERROR_EXIT) {
            abort_request = TRUE;
            continue;
        } else if (peek_frame) {
            do {
                ret = esdec_get_next_picture(dec_ctx);
                log_debug(dec_ctx, "esdec_get_next_picture ret: %d\n", ret);
            } while (ret != FAILURE);
        }

        if (dec_ctx->init_config.mc_cfg.stream_consumed_callback == NULL) {
            log_debug(dec_ctx, "release input buffer\n");
            esdec_release_input_buffer(input_port->release_queue, &buffer);
        }
    }

    esdec_output_port_clear(dec_ctx->output_port, esdec_picture_consume);
    if (dec_ctx->reorder_queue) {
        es_reorder_queue_clear(dec_ctx->reorder_queue);
        av_freep(&dec_ctx->reorder_queue);
    }

    if (dec_ctx->reorder_pkt) {
        av_freep(&dec_ctx->reorder_pkt);
    }

    if (!end_stream) {
        log_info(dec_ctx, "dec end stream\n");
        VCDecEndOfStream(dec_ctx->dec_inst);
    }

    if (dec_ctx->dec_inst) {
        if (dec_ctx->codec == ES_HEVC) {
            VCDecAbort(dec_ctx->dec_inst);
        }
    }

    if (dec_ctx->picture) {
        esdec_report_decode_info(dec_ctx);
        av_freep(&dec_ctx->picture);
    }

    if (dec_ctx->frame) {
        av_frame_free(&dec_ctx->frame);
    }
    av_buffer_unref(&dec_ctx->hwframe);
    av_buffer_unref(&dec_ctx->hwdevice);

    return NULL;
}

int es_decode_start(ESVDECContext *dec_ctx) {
    if (!dec_ctx) {
        log_error(dec_ctx, "error !!! dec_ctx is null\n");
        return FAILURE;
    }

    if (pthread_create(&dec_ctx->tid, NULL, esdec_decode_thread_run, dec_ctx)) {
        log_info(dec_ctx, "esdec_decode_thread_run create failed\n");
        return FAILURE;
    }

    log_info(dec_ctx, "esdec_decode_thread_run create success\n");

    return SUCCESS;
}

int es_decode_close(ESVDECContext *dec_ctx) {
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return FAILURE;
    }

    esdec_input_port_stop(dec_ctx->input_port);
    esdec_output_port_stop(dec_ctx->output_port);

    pthread_join(dec_ctx->tid, NULL);

    if (dec_ctx->dec_inst) {
        VCDecRelease(dec_ctx->dec_inst);
        dec_ctx->dec_inst = NULL;
        log_info(NULL, "dec_inst release success\n");
    }

    ESDecDeinitDevMemFd(&dec_ctx->dev_mem_fd);

    esdec_input_port_unref(&dec_ctx->input_port);
    esdec_output_port_unref(&dec_ctx->output_port);

    av_buffer_unref(&dec_ctx->dwl_ref);

    if (dec_ctx->inited) {
        dec_ctx->inited = FALSE;
        pthread_mutex_destroy(&dec_ctx->mutex);
        pthread_cond_destroy(&dec_ctx->cond);
    }

    log_info(dec_ctx, "es_decode_close success\n");

    return SUCCESS;
}

static int es_decode_flush_process(ESVDECContext *dec_ctx) {
    if (!dec_ctx || !dec_ctx->dec_inst) {
        log_error(dec_ctx, "dec_ctx: %p is null\n", dec_ctx);
        return FAILURE;
    }
    log_info(dec_ctx, "flush process\n");
    VCDecAbort(dec_ctx->dec_inst);
    VCDecAbortAfter(dec_ctx->dec_inst);
    log_info(dec_ctx, "VCDecAbortAfter\n");
    esdec_clear_input_packets(dec_ctx->input_port);
    esdec_clear_output_frames(dec_ctx->output_port);
    esdec_reset_output_memorys(dec_ctx->output_port);
    pthread_mutex_lock(&dec_ctx->mutex);
    pthread_cond_signal(&dec_ctx->cond);
    pthread_mutex_unlock(&dec_ctx->mutex);

    return 0;
}

int es_decode_flush(ESVDECContext *dec_ctx) {
    int ret;
    struct timespec ts;
    if (!dec_ctx || !dec_ctx->dec_inst || !dec_ctx->inited) {
        log_error(dec_ctx, "dec_ctx: %p is null\n", dec_ctx);
        return FAILURE;
    }
    if (!dec_ctx->input_port || !dec_ctx->output_port) {
        log_info(dec_ctx, "decode flush port input: %p, output: %p\n", dec_ctx->input_port, dec_ctx->output_port);
        return FAILURE;
    }

    pthread_mutex_lock(&dec_ctx->mutex);
    dec_ctx->state = ESDEC_STATE_FLUSHING;
    es_fifo_queue_abort(dec_ctx->output_port->consumed_queue);
    es_fifo_queue_abort(dec_ctx->input_port->packet_queue);

    get_clock_time_by_timeout(&ts, 3000);
    ret = pthread_cond_timedwait(&dec_ctx->cond, &dec_ctx->mutex, &ts);
    es_fifo_queue_start(dec_ctx->output_port->consumed_queue);
    es_fifo_queue_start(dec_ctx->input_port->packet_queue);
    dec_ctx->state = ESDEC_STATE_FLUSHED;
    pthread_mutex_unlock(&dec_ctx->mutex);

    if (ret) {
        log_warn(dec_ctx, "es_decode_flush timeout\n");
    } else {
        log_warn(dec_ctx, "es_decode_flush success\n");
    }

    return 0;
}
