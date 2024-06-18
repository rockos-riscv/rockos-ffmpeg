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

// #include "vsv_decode.h"
#include "es_jpeg_decode.h"
#include "jpegdecapi.h"
#include "libavutil/hwcontext_es.h"

#define NEXT_MULTIPLE(value, n) (((value) + (n) - 1) & ~((n) - 1))
#define ALIGN(a) (1 << (a))

static void jpeg_picture_consume(void *opaque, uint8_t *data)
{
    VSVDECContext *dec_ctx = opaque;
    struct DecPicturePpu pic = *((struct DecPicturePpu *)data);
    JpegDecOutput jpic;
    int i;

    memset(&jpic, 0, sizeof(JpegDecOutput));
    /* TODO update chroma luma/chroma base */
    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++){
        jpic.pictures[i].output_picture_y = pic.pictures[i].luma;
    }

    ff_es_jpeg_dec_del_pic_wait_consume_list(dec_ctx, data);
    JpegDecPictureConsumed((void*)dec_ctx->dec_inst, &jpic);

    av_free(data);
}

static av_cold int ff_es_jpeg_decode_close(AVCodecContext *avctx)
{
    VSVDECContext *dec_ctx = avctx->priv_data;
    int i;

    av_log(avctx, AV_LOG_DEBUG, "Es jpeg decode close.....\n");

    if(dec_ctx->low_latency && (dec_ctx->decode_end_flag == 0)) {
        dec_ctx->decode_end_flag = 1;
        sem_post(&dec_ctx->frame_sem);
        ff_es_jpeg_wait_for_task_completion(dec_ctx->task);
        dec_ctx->task_existed = 0;
        av_log(avctx, AV_LOG_DEBUG, "low_latency thread exited\n");
    }

    if (dec_ctx->pkt_dump_handle)
        ff_codec_dump_file_close(&dec_ctx->pkt_dump_handle);

    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->frame_dump_handle[i]) {
            ff_codec_dump_file_close(&dec_ctx->frame_dump_handle[i]);
        }
    }

    if(dec_ctx->dec_inst) {
        JpegDecEndOfStream(dec_ctx->dec_inst);
    }

    av_packet_unref(&dec_ctx->avpkt);
    dec_ctx->last_pic_flag = 1;
    dec_ctx->closed = 1;

    av_log(avctx, AV_LOG_DEBUG, "release input buffer\n");
    for (i = 0; i < dec_ctx->allocated_buffers; i++) {
        if (dec_ctx->stream_mem[i].virtual_address != NULL) {
            if (dec_ctx->dec_inst)
                DWLFreeLinear(dec_ctx->dwl_inst, &dec_ctx->stream_mem[i]);
        }
    }

    av_log(avctx, AV_LOG_DEBUG, "release dec_inst\n");
    if (dec_ctx->dec_inst)
        JpegDecRelease(dec_ctx->dec_inst);

    av_log(avctx, AV_LOG_DEBUG, "release output buffer\n");
    ff_es_jpeg_dec_release_ext_buffers(dec_ctx);

     av_log(avctx, AV_LOG_DEBUG, "release dwl_inst\n");
    if (dec_ctx->dwl_inst)
        DWLRelease(dec_ctx->dwl_inst);

    if (dec_ctx->frame) {
        av_frame_free(&dec_ctx->frame);
    }

    av_buffer_unref(&dec_ctx->hwframe);
    av_buffer_unref(&dec_ctx->hwdevice);
    return 0;
}

static av_cold int ff_es_jpeg_decode_init(AVCodecContext *avctx)
{
    int ret = 0;
    VSVDECContext *dec_ctx = avctx->priv_data;
    JpegDecApiVersion dec_api;
    enum DecRet rv;
    int  i;

    avctx->apply_cropping = 0;//for RGB format can output
    dec_api = JpegGetAPIVersion();
    av_log(avctx, AV_LOG_DEBUG, "Jpeg Decoder API v%d.%d\n", dec_api.major, dec_api.minor);

    ff_es_jpeg_dec_paras_check(avctx);

    ret = ff_es_jpeg_dec_init_hwctx(avctx);
    if (ret < 0)
        return ret;

    dec_ctx->vsv_decode_picture_consume = jpeg_picture_consume;
    dec_ctx->vsv_decode_pri_picture_info_free = ff_es_pri_picture_info_free;
    dec_ctx->data_free = ff_es_data_free;

#ifdef FB_SYSLOG_ENABLE
    sprintf(dec_ctx->module_name, "JPEGDEC");
#endif

    // ff_es_jpeg_create_dump_document(dec_ctx);

    jpeg_dec_set_default_dec_config(avctx);

    // ret = ff_es_jpeg_dec_set_buffer_number_for_trans(avctx);
    // av_log(avctx, AV_LOG_DEBUG, "ff_es_jpeg_dec_set_buffer_number_for_trans %d\n", ret);

    ret = ff_es_jpeg_dec_init_ppu_cfg(avctx,&dec_ctx->vsv_dec_config);
    if (ret < 0){
        av_log(avctx, AV_LOG_ERROR, "ff_es_jpeg_init_ppu_cfg failed\n");
        goto error;
    }

    rv = jpeg_init(dec_ctx);
    if (rv != DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Decoder initialization failed!\n");
        goto error;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "JpegDecInit Init OK!\n");
    }

    ret = ff_es_jpeg_allocate_input_buffer(dec_ctx);
    if (ret < 0)
        goto error;

    return ret;
error:
    ff_es_jpeg_decode_close(avctx);
    return ret;
}

static int ff_es_jpeg_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    VSVDECContext *dec_ctx = avctx->priv_data;
    // AVPacket avpkt = { 0 };
    AVFrame *out = frame;
    enum DecRet ret;
    struct DWLLinearMem mem;
    struct DecBufferInfo buf_info;
    struct DecInputParameters jpeg_in;
    AVFrame *tmp_frame;

    tmp_frame = dec_ctx->frame;
    u32 tmp = 0;
    int rv = 0;

    av_packet_unref(&dec_ctx->avpkt);
    rv = ff_decode_get_packet(avctx, &dec_ctx->avpkt);

    if (rv < 0 && rv != AVERROR_EOF) {
        return rv;
    } else if (rv == AVERROR_EOF) {

        av_log(avctx, AV_LOG_DEBUG,
               "in ff_es_jpeg_decode_receive_frame JpegDecEndOfStream  EOS... \n");

        JpegDecEndOfStream(dec_ctx->dec_inst);
        ret = ff_esdec_get_next_picture(avctx, frame);
        if (ret < 0) {
            return ret;
        }
    }

    dec_ctx->got_package_number++;

    if(dec_ctx->drop_frame_interval > 0 ){
        rv = ff_es_dec_drop_pkt(avctx, &dec_ctx->avpkt);
        if (rv < 0)
            goto err_exit;
    }

    // ff_es_jpeg_pkt_dump(dec_ctx, &dec_ctx->avpkt);

    if(dec_ctx->low_latency) {
        if (!dec_ctx->task_existed) {
            ff_es_jpeg_low_latency_task_init(dec_ctx);
        }
        else {
            if (!dec_ctx->avpkt.data) {
                av_log(NULL, AV_LOG_ERROR, "pkt.data is invaild, exit low_latency pthread\n");
                goto err_exit;
            }
            dec_ctx->send_strm_info.strm_bus_addr =
                        dec_ctx->send_strm_info.strm_bus_start_addr =
                        dec_ctx->stream_mem[dec_ctx->stream_mem_index].bus_address;
            dec_ctx->send_strm_info.strm_vir_addr =
                        dec_ctx->send_strm_info.strm_vir_start_addr =
                        (u8 *)dec_ctx->stream_mem[dec_ctx->stream_mem_index].virtual_address;
            sem_post(&dec_ctx->frame_sem);
        }
    }  else {
        ff_es_jpeg_dec_send_avpkt_to_decode_buffer(avctx,
                                                   &dec_ctx->avpkt,
                                                   dec_ctx->stream_mem[dec_ctx->stream_mem_index]);
    }

    if(dec_ctx->low_latency) {
        sem_wait(&dec_ctx->send_sem);
        jpeg_in.strm_len = dec_ctx->sw_hw_bound;
    }

    // init jpeg_in
    ff_es_jpeg_dec_init_dec_input_paras(&dec_ctx->avpkt, dec_ctx, &jpeg_in);

decode:

    dec_ctx->sequence_info.jpeg_input_info = jpeg_in;

    ret = jpeg_dec_get_info(dec_ctx);
    if(ret != DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "jpeg_dec_get_info return: %d\n", ret);
        goto err_exit;
    } else {
        av_log(avctx, AV_LOG_DEBUG, "[%s:%d] get image info successful\n", __func__, __LINE__);
        ff_es_jpeg_dec_print_image_info(avctx, &dec_ctx->sequence_info);
    }

    // create pkt dump file
    if (dec_ctx->packet_dump && !dec_ctx->pkt_dump_handle)
        ff_es_jpeg_init_pkt_dump_handle(dec_ctx);
    ff_es_jpeg_pkt_dump(dec_ctx);

    // update jpeg_in after jpeg_dec_get_info
    ff_es_jpeg_dec_update_dec_input_paras(dec_ctx, &jpeg_in);
    if (dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thum_exist && dec_ctx->thum_out) {
        jpeg_in.dec_image_type = JPEGDEC_THUMBNAIL;
    }

    dec_ctx->vsv_dec_config.dec_image_type = jpeg_in.dec_image_type;

    rv = ff_es_jpeg_dec_modify_config_by_sequence_info(avctx);
    if (rv < 0)
        goto err_exit;

    //modify dec_ctx->vsv_dec_config according to dec_ctx->thum_out.
    ff_es_jpeg_dec_modify_thum_config_by_sequence_info(avctx);

    tmp = jpeg_dec_set_info((void* )dec_ctx->dec_inst, dec_ctx->vsv_dec_config);
    av_log(avctx, AV_LOG_DEBUG, "jpeg_dec_set_info return: %d\n", tmp);
    if (tmp != DEC_OK)
        goto err_exit;

allocate_buffer:

    rv = jpeg_get_buffer_info((void* )dec_ctx->dec_inst, &buf_info);
    av_log(avctx, AV_LOG_DEBUG, "jpeg_get_buffer_info return: %d\n", rv);
    if(rv != DEC_WAITING_FOR_BUFFER && rv != DEC_OK)
        goto err_exit;

    av_log(avctx, AV_LOG_DEBUG, "buf_to_free %p, next_buf_size %d, buf_num %d\n",
            (void *)buf_info.buf_to_free.virtual_address, buf_info.next_buf_size, buf_info.buf_num);

    rv = ff_es_jpeg_allocate_output_buffer(dec_ctx, &buf_info);

    if (rv < 0)
        goto err_exit;

    rv = ff_es_jpeg_decoder(avctx, &jpeg_in, frame);

    if (rv == ES_MORE_BUFFER) {
        goto allocate_buffer;
    } else if (rv == ES_ERROR) {
        goto err_exit;
    }

    if (dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thum_exist && !dec_ctx->thum_out) {
        dec_ctx->thum_out = 1;
        goto decode;
    }

    if (ret == ES_OK) {
        if (avctx->width != frame->width || avctx->height != frame->height) {
            avctx->width = frame->width;
            avctx->height = frame->height;
        }

        if (avctx->pix_fmt == AV_PIX_FMT_ES) {
            frame->format = AV_PIX_FMT_ES;
            frame->hw_frames_ctx = av_buffer_ref(dec_ctx->hwframe);
        } else if (frame->flags & AV_FRAME_FLAG_DISCARD){
            frame->format = avctx->sw_pix_fmt;
        } else {
            av_frame_move_ref(tmp_frame, frame);
            ret = ff_get_buffer(avctx, frame, 0);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed\n");
            } else {
                frame->pts = tmp_frame->pts;
                tmp_frame->hw_frames_ctx = av_buffer_ref(dec_ctx->hwframe);
                ret = av_hwframe_transfer_data(frame, tmp_frame, 0);
                if (ret) {
                    av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed\n");
                }
            }
            av_frame_unref(tmp_frame);
        }
    }

err_exit:
    av_log(avctx, AV_LOG_DEBUG, "exit of jpeg_decode_frame...\n");
    if(dec_ctx->low_latency) {
        dec_ctx->pic_decoded = 1;
        av_log(avctx, AV_LOG_DEBUG, "send_bytestrm_task sleep\n");
    }
    return AVERROR_EOF;
}

static void ff_es_jpeg_decode_flush(AVCodecContext *avctx)
{
}

#define OFFSET(x) offsetof(VSVDECContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "stride_align", "set out_stride", OFFSET(out_stride),
        AV_OPT_TYPE_INT,  { .i64 = 64 }, 1, 2048, VD },
    { "pp0_enabled", "set pp0 output enable", OFFSET(cfg_pp_enabled[0]),
        AV_OPT_TYPE_INT,  { .i64 = 1 }, 0, 1, VD },
    { "pp0_format", "set ppu0 output format", OFFSET(output_format[0]),
        AV_OPT_TYPE_INT,  { .i64 = 23 }, 0, INT_MAX, VD },
    { "pp0_set", "set pp configure", OFFSET(pp_setting[0]),
        AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VD }, //pp0 not support scale
    { "pp1_enabled", "set pp0 output enable", OFFSET(cfg_pp_enabled[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "pp1_format", "set ppu1 output format", OFFSET(output_format[1]),
        AV_OPT_TYPE_INT,  { .i64 = 23 }, 0, INT_MAX, VD },
    { "pp1_set", "set pp configure", OFFSET(pp_setting[1]),
        AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VD },
    { "packet_dump", "set dump packet", OFFSET(packet_dump),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "dump_path", "set dump packet path", OFFSET(dump_path),
        AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD },
    { "packet_dump_time", "set dump packet time", OFFSET(packet_dump_time),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "pp0_frame_dump", "set pp0 frame dump", OFFSET(frame_dump[0]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 60, VD },
    { "pp1_frame_dump", "set pp1 frame dump", OFFSET(frame_dump[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 60, VD },
    { "pp0_frame_dump_time", "set dump pp0 frame time", OFFSET(frame_dump_time[0]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "pp1_frame_dump_time", "set dump pp1 frame time", OFFSET(frame_dump_time[1]),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, INT_MAX, VD },
    { "pmode", "set pkt dump mode", OFFSET(pmode),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "fmode", "set frame dump mode", OFFSET(fmode),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "drop_frame_interval", "set drop frame interval", OFFSET(drop_frame_interval),
        AV_OPT_TYPE_INT,  {.i64 = 0 }, 0, INT_MAX, VD },
    { "thumb_mode", "0: only decode picture, 1: only decode thumbnail, 2: decode thumbnail and picture.",
        OFFSET(thumb_mode), AV_OPT_TYPE_INT,  {.i64 = Decode_Pic_Thumb }, 0, 2, VD, "thumbmode"},
    { "decode_pic", "0: only decode picture",  0,
        AV_OPT_TYPE_CONST, { .i64 = Only_Decode_Pic } , 0, 2, VD, "thumbmode"},
    { "decode_thumb", "1: only decode thumbnail",  0,
        AV_OPT_TYPE_CONST, { .i64 = Only_Decode_Thumb } , 0, 2, VD, "thumbmode"},
    { "decode_both", "2: decode thumbnail and picture",  0,
        AV_OPT_TYPE_CONST, { .i64 = Decode_Pic_Thumb } , 0, 2, VD, "thumbmode"},
    { "decode_mode", "decode mode: dec_normal, dec_low_latency",  OFFSET(decode_mode),
        AV_OPT_TYPE_INT, { .i64 =  DEC_NORMAL} , 0, 8, VD, "decode_mode"},
    { "dec_normal", "decode mode: dec_normal",  0,
        AV_OPT_TYPE_CONST, { .i64 =  DEC_NORMAL} , 0, 8, VD, "decode_mode"},
    { "dec_low_latency", "decode mode: dec_low_latency",  0,
        AV_OPT_TYPE_CONST, { .i64 =  DEC_LOW_LATENCY} , 0, 8, VD, "decode_mode"},

    // ffmpeg set opts
    { "down_scale", "width:height or ratio_x:ratio_y", OFFSET(scale_set),
        AV_OPT_TYPE_STRING, { .str="0:0" }, 0, 0, VD },
    { "pp0_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop_set[0]),
        AV_OPT_TYPE_STRING, { .str=NULL }, 0, 0, VD },
    { "pp1_crop", "crop (top)x(bottom)x(left)x(right)", OFFSET(crop_set[1]),
        AV_OPT_TYPE_STRING, { .str=NULL }, 0, 0, VD },
    { "frame_dump", "frame dump enable, next set filename better", OFFSET(fdump),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "filename", " frame dump filename", OFFSET(filename),
        AV_OPT_TYPE_STRING,  { .str="dump_file" }, 0, 0, VD },
    { "force_8bit", "force output 8 bits data", OFFSET(force_8bit),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "pp0", "PP0 output enable",  OFFSET(cfg_pp_enabled[0]),
        AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, VD, "enable pp"},
    { "pp1", "PP1 output enable",  OFFSET(cfg_pp_enabled[1]),
        AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD, "enable pp"},
    { "pp_out", "set pp output enable", OFFSET(pp_out),
        AV_OPT_TYPE_INT,  { .i64 = 0 }, 0, 1, VD },
    { "enable", "enable pp",  0,
        AV_OPT_TYPE_CONST, { .i64 = 1 } , 0, 1, VD, "enable pp"},
    { "disable", "disable pp",  0,
        AV_OPT_TYPE_CONST, { .i64 = 0 } , 0, 0, VD, "enable pp"},
    { "pp0_fmt", "set pp0 output format",  OFFSET(output_format[0]),
        AV_OPT_TYPE_INT, { .i64 = 23 } , 0, 194, VD, "pp0_fmt"},
    { "pp1_fmt", "set pp1 output format",  OFFSET(output_format[1]),
        AV_OPT_TYPE_INT, { .i64 = 23 } , 0, 194, VD, "pp1_fmt"},

    // set pp0 format
    { "nv12", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV12 } , 0, 194, VD, "pp0_fmt"},
    { "nv21", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV21 } , 0, 194, VD, "pp0_fmt"},
    { "yuv420p", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_YUV420P } , 0, 194, VD, "pp0_fmt"},
    { "gray", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_GRAY8 } , 0, 194, VD, "pp0_fmt"},
    // set pp1 format
    { "nv12", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV12 } , 0, 194, VD, "pp1_fmt"},
    { "nv21", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV21 } , 0, 194, VD, "pp1_fmt"},
    { "yuv420p", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_YUV420P } , 0, 194, VD, "pp1_fmt"},
    { "gray", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_GRAY8 } , 0, 194, VD, "pp1_fmt"},
    { "rgb24", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_RGB24 } , 0, 194, VD, "pp1_fmt"},
    { "bgr24", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_BGR24 } , 0, 194, VD, "pp1_fmt"},
    { "argb", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_ARGB } , 0, 194, VD, "pp1_fmt"},
    { "abgr", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_ABGR } , 0, 194, VD, "pp1_fmt"},
    { "0rgb", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_0RGB } , 0, 194, VD, "pp1_fmt"},
    { "0bgr", "output format",  0,
        AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_0BGR } , 0, 194, VD, "pp1_fmt"},
    { NULL },
};

static const AVClass es_jpeg_decode_class = {
    .class_name = "es_jpeg_decoder",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault es_jpeg_decode_defaults[] = {
    { NULL },
};

static const enum AVPixelFormat es_jdec_support_pixfmts[] = {AV_PIX_FMT_ES,
                                                             AV_PIX_FMT_YUV420P,
                                                             AV_PIX_FMT_NV12,
                                                             AV_PIX_FMT_NV21,
                                                             AV_PIX_FMT_GRAY8,
                                                             AV_PIX_FMT_NONE};

static const AVCodecHWConfigInternal *es_hw_configs[] = {
    &(const AVCodecHWConfigInternal) {
        .public = {
            .pix_fmt     = AV_PIX_FMT_ES,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX
                           | AV_CODEC_HW_CONFIG_METHOD_INTERNAL,
            .device_type = AV_HWDEVICE_TYPE_ES,
        },
        .hwaccel = NULL,
    },
    NULL
};

AVCodec ff_jpeg_es_decoder = {
    .name           = "jpeg_es_decoder",
    .long_name      = NULL_IF_CONFIG_SMALL("Eswin JPEG decoder, on VeriSilicon & GStreamer & FFmpeg."),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MJPEG,
    .priv_data_size = sizeof(VSVDECContext),
    .init           = &ff_es_jpeg_decode_init,
    .close          = &ff_es_jpeg_decode_close,
    .receive_frame  = &ff_es_jpeg_decode_receive_frame,
    .flush          = &ff_es_jpeg_decode_flush,
    .priv_class     = &es_jpeg_decode_class,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_AVOID_PROBING,
    .defaults       = es_jpeg_decode_defaults,
    .pix_fmts       = es_jdec_support_pixfmts,
    .hw_configs     = es_hw_configs,
    .wrapper_name   = "es",
    //.bsfs           = "mjpeg2jpeg",
};
