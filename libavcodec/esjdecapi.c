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

#include "esjdecapi.h"
#include "jpegdecapi.h"
#include "libavutil/hwcontext_es.h"
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

static void report_dec_pic_info( JDECContext *dec_ctx,  struct DecPicture *picture)
{
    char info_string[2048];
    static const char* pic_types[] = {"        IDR", "Non-IDR (P)", "Non-IDR (B)"};

    log_debug(dec_ctx, "PIC %2d/%2d, type %s, ",
                              dec_ctx->pic_display_number,
                              picture->picture_info.pic_id,
                              picture->picture_info.pic_coding_type);
    if (picture->picture_info.cycles_per_mb) {
        log_debug(dec_ctx,
                              " %4d cycles / mb,",
                              picture->picture_info.cycles_per_mb);
    }

    log_debug(dec_ctx,
            " %d x %d, Crop: (%d, %d), %d x %d %s",
            picture->sequence_info.pic_width,
            picture->sequence_info.pic_height,
            picture->sequence_info.crop_params.crop_left_offset,
            picture->sequence_info.crop_params.crop_top_offset,
            picture->sequence_info.crop_params.crop_out_width,
            picture->sequence_info.crop_params.crop_out_height,
            picture->picture_info.is_corrupted ? "CORRUPT" : "");

    log_debug(dec_ctx, "%s\n", info_string);
}

static void esjdec_print_decode_return(enum DecRet jpeg_ret) {
    switch (jpeg_ret) {
    case DEC_PIC_RDY:
        log_debug(NULL, "JpegDecDecode API returned : DEC_PIC_RDY\n");
        break;
    case DEC_OK:
        log_debug(NULL, "JpegDecDecode API returned : DEC_OK\n");
        break;
    case DEC_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_ERROR\n");
        break;
    case DEC_HW_TIMEOUT:
        log_error(NULL, "JpegDecDecode API returned : JPEGDEC_HW_TIMEOUT\n");
        break;
    case DEC_UNSUPPORTED:
        log_error(NULL, "JpegDecDecode API returned : DEC_UNSUPPORTED\n");
        break;
    case DEC_PARAM_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_PARAM_ERROR\n");
        break;
    case DEC_MEMFAIL:
        log_error(NULL, "JpegDecDecode API returned : DEC_MEMFAIL\n");
        break;
    case DEC_INITFAIL:
        log_error(NULL, "JpegDecDecode API returned : DEC_INITFAIL\n");
        break;
    case DEC_HW_BUS_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_HW_BUS_ERROR\n");
        break;
    case DEC_SYSTEM_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_SYSTEM_ERROR\n");
        break;
    case DEC_DWL_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_DWL_ERROR\n");
        break;
    case DEC_INVALID_STREAM_LENGTH:
        log_error(NULL,
                "JpegDecDecode API returned : DEC_INVALID_STREAM_LENGTH\n");
        break;
    case DEC_STRM_ERROR:
        log_error(NULL, "JpegDecDecode API returned : DEC_STRM_ERROR\n");
        break;
    case DEC_INVALID_INPUT_BUFFER_SIZE:
        log_error(NULL,
                "JpegDecDecode API returned : DEC_INVALID_INPUT_BUFFER_SIZE\n");
        break;
    case DEC_INCREASE_INPUT_BUFFER:
        log_debug(NULL,
                "JpegDecDecode API returned : DEC_INCREASE_INPUT_BUFFER\n");
        break;
    case DEC_SLICE_MODE_UNSUPPORTED:
        log_error(NULL,
                "JpegDecDecode API returned : DEC_SLICE_MODE_UNSUPPORTED\n");
        break;
    case DEC_NO_DECODING_BUFFER:
        log_debug(NULL,
                "JpegDecDecode API returned : DEC_NO_DECODING_BUFFER\n");
        break;
    case DEC_WAITING_FOR_BUFFER:
        log_debug(NULL,
                "JpegDecDecode API returned : DEC_WAITING_FOR_BUFFER\n");
        break;
    case DEC_FORMAT_NOT_SUPPORTED:
        log_error(NULL,
                "JpegDecDecode API returned : DEC_FORMAT_NOT_SUPPORTED\n");
        break;
    case DEC_STRM_PROCESSED:
        log_error(NULL,
                "JpegDecDecode API returned : DEC_STRM_PROCESSED\n");
        break;
    default:
        log_error(NULL, "JpegDecDecode API returned unknown status\n");
        break;
    }
}

static void esjdec_print_image_info(struct DecSequenceInfo * image_info) {
  assert(image_info);

  /* Select if Thumbnail or full resolution image will be decoded */
  if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
    /* decode thumbnail */
    log_debug(NULL, "\t-JPEG THUMBNAIL IN STREAM\n");
    log_debug(NULL, "\t-JPEG THUMBNAIL INFO\n");
    log_debug(NULL, "\t\t-JPEG thumbnail display resolution(W x H): %d x %d\n",
            image_info->scaled_width_thumb, image_info->scaled_height_thumb);
    log_debug(NULL, "\t\t-JPEG thumbnail HW decoded RESOLUTION(W x H): %d x %d\n",
            NEXT_MULTIPLE(image_info->scaled_width_thumb, 16),
            NEXT_MULTIPLE(image_info->scaled_height_thumb, 8));
    log_debug(NULL, "\t\t-JPEG thumbnail OUTPUT SIZE(Stride x H): %d x %d\n",
            image_info->pic_width_thumb, image_info->pic_height_thumb);

    /* stream type */
    switch (image_info->coding_mode_thumb) {
    case JPEG_BASELINE:
      log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_BASELINE\n");
      break;
    case JPEG_PROGRESSIVE:
      log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_PROGRESSIVE\n");
      break;
    case JPEG_NONINTERLEAVED:
      log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_NONINTERLEAVED\n");
      break;
    }

    if(image_info->output_format_thumb) {
      switch (image_info->output_format_thumb) {
      case DEC_OUT_FRM_YUV400:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV400\n");
        break;
      case DEC_OUT_FRM_YUV420SP:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV420SP\n");
        break;
      case DEC_OUT_FRM_YUV422SP:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV422SP\n");
        break;
      case DEC_OUT_FRM_YUV440:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV440\n");
        break;
      case DEC_OUT_FRM_YUV411SP:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV411SP\n");
        break;
      case DEC_OUT_FRM_YUV444SP:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: DEC_OUT_FRM_YUV444SP\n");
        break;
      default:
        log_debug(NULL,
                "\t\t-JPEG: THUMBNAIL OUTPUT: NOT SUPPORT\n");
        break;
      }
    }
  } else if(image_info->thumbnail_type == JPEGDEC_NO_THUMBNAIL) {
    /* decode full image */
    log_debug(NULL,
            "\t-NO THUMBNAIL IN STREAM ==> Decode full resolution image\n");
  } else if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT) {
    /* decode full image */
    log_debug(NULL,
            "\tNot SUPPORTED THUMBNAIL IN STREAM ==> Decode full resolution image\n");
  }

  log_debug(NULL, "\t-JPEG FULL RESOLUTION INFO\n");
  log_debug(NULL, "\t\t-JPEG display resolution(W x H): %d x %d\n",
          image_info->scaled_width, image_info->scaled_height);
  log_debug(NULL, "\t\t-JPEG HW decoded RESOLUTION(W x H): %d x %d\n",
          NEXT_MULTIPLE(image_info->scaled_width, 8),
          NEXT_MULTIPLE(image_info->scaled_height, 8));
  log_debug(NULL, "\t\t-JPEG OUTPUT SIZE(Stride x H): %d x %d\n",
          image_info->pic_width, image_info->pic_height);
  if(image_info->output_format) {
    switch (image_info->output_format) {
    case DEC_OUT_FRM_YUV400:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV400\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_0_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV420SP:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV420SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_2_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV422SP:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV422SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_2_2 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV440:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV440\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_4_0 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV411SP:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV411SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_1_1 = 1;
#endif
      break;
    case DEC_OUT_FRM_YUV444SP:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: DEC_OUT_FRM_YUV444SP\n");
#ifdef ASIC_TRACE_SUPPORT
      decoding_tools.sampling_4_4_4 = 1;
#endif
      break;
    default:
      log_debug(NULL,
              "\t\t-JPEG: FULL RESOLUTION OUTPUT: NOT SUPPORT\n");
      break;
    }
  }

  /* stream type */
  switch (image_info->coding_mode) {
        case JPEG_BASELINE:
            log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_BASELINE\n");
            break;
        case JPEG_PROGRESSIVE:
            log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_PROGRESSIVE\n");
#ifdef ASIC_TRACE_SUPPORT
        decoding_tools.progressive = 1;
#endif
             break;
        case JPEG_NONINTERLEAVED:
            log_debug(NULL, "\t\t-JPEG: STREAM TYPE: JPEG_NONINTERLEAVED\n");
            break;
   }

  if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
    log_debug(NULL, "\t-JPEG ThumbnailType: JPEG\n");
#ifdef ASIC_TRACE_SUPPORT
    decoding_tools.thumbnail = 1;
#endif
  } else if(image_info->thumbnail_type == JPEGDEC_NO_THUMBNAIL)
    log_debug(NULL, "\t-JPEG ThumbnailType: NO THUMBNAIL\n");
  else if(image_info->thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT)
    log_debug(NULL, "\t-JPEG ThumbnailType: NOT SUPPORTED THUMBNAIL\n");
}

static bool check_scale_value( uint32_t v) {
    if (v == -1 || v == -2 || v == -4 || v == -8) {
        return TRUE;
    }
    return FALSE;
}

static void esjdec_ppu_print(struct DecConfig *config){

    for(int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled) {
            log_debug(NULL, "ppu_cfg[%d].tiled_e = %d\n", i,
               config->ppu_cfg[i].tiled_e);
            log_debug(NULL, "ppu_cfg[%d].scale.enabled = %d\n",
                i, config->ppu_cfg[i].scale.enabled);
            log_debug(NULL, "ppu_cfg[%d].scale.scale_by_ratio = %d\n",
                i, config->ppu_cfg[i].scale.scale_by_ratio);
            log_debug(NULL, "ppu_cfg[%d].scale.ratio_x = %d\n",
                i, config->ppu_cfg[i].scale.ratio_x);
            log_debug(NULL, "ppu_cfg[%d].scale.ratio_y = %d\n",
                i, config->ppu_cfg[i].scale.ratio_y);
            log_debug(NULL, "ppu_cfg[%d].scale.width = %d\n",
                i, config->ppu_cfg[i].scale.width);
            log_debug(NULL, "ppu_cfg[%d].scale.height = %d\n",
                i, config->ppu_cfg[i].scale.height);
            log_debug(NULL, "ppu_cfg[%d].crop.enabled = %d\n",
                i, config->ppu_cfg[i].crop.enabled);
            log_debug(NULL, "ppu_cfg[%d].crop.x = %d\n",
                i, config->ppu_cfg[i].crop.x);
            log_debug(NULL, "ppu_cfg[%d].crop.y = %d\n",
                i, config->ppu_cfg[i].crop.y);
            log_debug(NULL, "ppu_cfg[%d].crop.width = %d\n",
                i, config->ppu_cfg[i].crop.width);
            log_debug(NULL, "ppu_cfg[%d].crop.height = %d\n",
                i, config->ppu_cfg[i].crop.height);
            log_debug(NULL, "ppu_cfg[%d].out_p010 = %d\n",
                i, config->ppu_cfg[i].out_p010);
            log_debug(NULL, "ppu_cfg[%d].out_I010 = %d\n",
                i, config->ppu_cfg[i].out_I010);
            log_debug(NULL, "ppu_cfg[%d].align = %d\n",
                i, config->ppu_cfg[i].align);
            log_debug(NULL, "ppu_cfg[%d].shaper_enabled = %d\n",
                i, config->ppu_cfg[i].shaper_enabled);
            log_debug(NULL, "ppu_cfg[%d].cr_first = %d\n",
                i, config->ppu_cfg[i].cr_first);
            log_debug(NULL, "ppu_cfg[%d].rgb = %d\n",
                i, config->ppu_cfg[i].rgb);
            log_debug(NULL, "ppu_cfg[%d].rgb_format = %d\n",
                i, config->ppu_cfg[i].rgb_format);
        }
    }
}

static int esjdec_init_pkt_dump_handle(JDECContext *dec_ctx, struct DecSequenceInfo *info) {
    DumpParas paras;

    if(dec_ctx->packet_dump && !dec_ctx->pkt_dump_handle) {
        paras.width = info->pic_width;
        paras.height = info->pic_height;
        paras.pic_stride = 0;
        paras.pic_stride_ch = 0;
        paras.prefix_name = "jdec";
        paras.suffix_name = "jpeg";
        paras.fmt = NULL;
        dec_ctx->pkt_dump_handle = ff_codec_dump_file_open(dec_ctx->dump_path, dec_ctx->packet_dump_time, &paras);
        if (!dec_ctx->pkt_dump_handle) {
            log_error(NULL, "init pkt_dump_handle failed\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int esjdec_init_frame_dump_handle(JDECContext *dec_ctx, struct DecPicturePpu *pic) {
    int ret = 0;

    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->cfg_pp_enabled[i] == 1 && dec_ctx->frame_dump[i] && !dec_ctx->frame_dump_handle[i]) {
            DumpParas paras;
            paras.width = pic->pictures[i].pic_width;
            paras.height = pic->pictures[i].pic_height;
            paras.pic_stride = pic->pictures[i].pic_stride;
            paras.pic_stride_ch = pic->pictures[i].pic_stride_ch;
            paras.prefix_name = "jdec";

            if (i == 0)
                paras.ppu_channel = "pp0";
            else
                paras.ppu_channel = "pp01";

            if (IS_PIC_RGB_FMT(pic->pictures[i].picture_info.format))
                paras.suffix_name = "rgb";
            else
                paras.suffix_name = "yuv";

            paras.fmt = ff_codec_decfmt_to_char(pic->pictures[i].picture_info.format);

            dec_ctx->frame_dump_handle[i] = ff_codec_dump_file_open(dec_ctx->dump_path,
                                                                    dec_ctx->frame_dump_time[i],
                                                                    &paras);

        }
    }

    return ret;
}

static int esjdec_frame_dump(JDECContext *dec_ctx, struct DecPicturePpu *pic) {
    int ret = 0;
    for ( int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(dec_ctx->cfg_pp_enabled[i] == 1 && dec_ctx->frame_dump[i]) {
            if (dec_ctx->frame_dump_handle[i]) {
                ret = ff_codec_dump_data_to_file_by_decpicture(&pic->pictures[i], dec_ctx->frame_dump_handle[i]);
                if (ret == ERR_TIMEOUT) {
                    log_info(NULL, "frame dump timeout\n");
                    ff_codec_dump_file_close(&dec_ctx->frame_dump_handle[i]);
                    dec_ctx->frame_dump[i] = 0;
                    return 0;
                } else if (ret < 0) {
                    log_error(NULL, "write file error\n");
                    return -1;
                }
            } else {
                log_error(NULL,"fp is not inited\n");
                return -1;
            }
        }
    }

    return 0;
}

static int esjdec_pkt_dump(JDECContext *dec_ctx, InputBuffer *buffer)
{
    int ret = 0;
    if(dec_ctx->packet_dump) {
        if (dec_ctx->pkt_dump_handle) {
            ret = ff_codec_dump_bytes_to_file(buffer->vir_addr, buffer->size,  dec_ctx->pkt_dump_handle);
            if (ret == ERR_TIMEOUT) {
                log_info(NULL, "pkt dump timeout\n");
                ff_codec_dump_file_close(&dec_ctx->pkt_dump_handle);
                dec_ctx->packet_dump = 0;
            } else if (ret < 0) {
                log_error(NULL, "write frame into file failed\n");
                return FAILURE;
            }
        } else {
            log_error(NULL, "fp is not inited\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static bool esjdec_drop_pkt(JDECContext *dec_ctx)
{
    bool ret = FALSE;

    if (!dec_ctx) {
        log_error(NULL, "dec_ctx is null, dex_ctx: %p\n", dec_ctx);
        return ret;
    }

    if (dec_ctx->drop_frame_interval <= 0) {
        return ret;
    }

    if (dec_ctx->got_package_number % (dec_ctx->drop_frame_interval + 1) == 0) {
        log_debug(dec_ctx, "drop pkt number: %d\n", dec_ctx->got_package_number);
        ret = TRUE;
    } else {
        ret = FALSE;
    }

    return ret;
}

static int esjdec_get_image_info(void *inst, struct DecSequenceInfo *info, struct DecConfig *config)
{
    JpegDecInput jpeg_input;
    enum DecRet rv;
    JpegDecImageInfo image_info;

    jpeg_input.stream_buffer = info->jpeg_input_info.stream_buffer;
    jpeg_input.stream_length = info->jpeg_input_info.strm_len;
    jpeg_input.buffer_size = info->jpeg_input_info.buffer_size;
    jpeg_input.stream = info->jpeg_input_info.stream;

    DWLmemset(&image_info, 0, sizeof(image_info));

    rv = JpegDecGetImageInfo(inst, &jpeg_input, &image_info);

    info->scaled_width = image_info.display_width;
    info->scaled_height = image_info.display_height;
    info->pic_width = image_info.output_width;
    info->pic_height = image_info.output_height;
    info->scaled_width_thumb = image_info.display_width_thumb;
    info->scaled_height_thumb = image_info.display_height_thumb;
    info->pic_width_thumb = image_info.output_width_thumb;
    info->pic_height_thumb = image_info.output_height_thumb;
    info->output_format = image_info.output_format;
    info->output_format_thumb = image_info.output_format_thumb;
    info->coding_mode = image_info.coding_mode;
    info->coding_mode_thumb = image_info.coding_mode_thumb;
    info->thumbnail_type = image_info.thumbnail_type;
    info->img_max_dec_width = image_info.img_max_dec_width;
    info->img_max_dec_height = image_info.img_max_dec_height;
    /* update the alignment setting in "image_info" data structure and output picture width */
    info->pic_width = NEXT_MULTIPLE(info->pic_width, ALIGN(config->align));
    info->pic_width_thumb = NEXT_MULTIPLE(info->pic_width_thumb, ALIGN(config->align));

    esjdec_print_image_info(&info);

    if(rv != DEC_OK) {
        log_error(NULL, "get SequenceInfo failed, rv: %d\n", rv);
        return FAILURE;
    }

    return SUCCESS;
}
static int esjdec_modify_config_by_image_info(JDECContext *dec_ctx,
                                                 struct DecSequenceInfo *sequence_info)
{
    enum DecRet rv_info;
    int i;
    /* process pp size -1/-2/-4/-8. */
    struct DecConfig *config = &dec_ctx->jdec_config;
    uint32_t alignh = sequence_info->is_interlaced ? 4 : 2;
    uint32_t alignw = 2;

    uint32_t original_width = sequence_info->pic_width;
    uint32_t original_height = sequence_info->pic_height;

    //crop
    for (i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled && config->ppu_cfg[i].crop.enabled) {
            if (config->ppu_cfg[i].crop.x > original_width ||
                config->ppu_cfg[i].crop.y > original_height ||
                ((config->ppu_cfg[i].crop.x + config->ppu_cfg[i].crop.width) > original_width) ||
                ((config->ppu_cfg[i].crop.x + config->ppu_cfg[i].crop.height) > original_height)) {
                log_error(dec_ctx,
                       "invalid crop config, original_width: %d original_height: %d\n",
                       original_width, original_height);
                return FAILURE;
            }

            // check value
            if(config->ppu_cfg[i].crop.width < 48 || config->ppu_cfg[i].crop.height < 48) {
                log_error(dec_ctx,
                       "pp%d invalid crop config, crop.width: %d crop.height: %d, "
                        "request values equal to or more than 48\n",
                        i,
                        config->ppu_cfg[i].crop.width,
                        config->ppu_cfg[i].crop.height);
                return FAILURE;
            }

            if ((config->ppu_cfg[i].crop.width % 2) || (config->ppu_cfg[i].crop.height % 2)) {
                log_error(dec_ctx,
                      "pp%d invalid crop config, crop.width: %d crop.height: %d, request values is even\n",
                       i,
                       config->ppu_cfg[i].crop.width,
                       config->ppu_cfg[i].crop.height);
                config->ppu_cfg[i].crop.width = NEXT_MULTIPLE(config->ppu_cfg[i].crop.width, alignw);
                config->ppu_cfg[i].crop.height = NEXT_MULTIPLE(config->ppu_cfg[i].crop.height, alignh);
            }
        }
    }

    // scale
    for (i = 1; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (config->ppu_cfg[i].enabled && config->ppu_cfg[i].scale.enabled) {
            if (check_scale_value(config->ppu_cfg[i].scale.width) ||
                check_scale_value(config->ppu_cfg[i].scale.height)) {

                if (config->ppu_cfg[i].scale.width == -1
                    && config->ppu_cfg[i].scale.height == -1) {
                    rv_info = DEC_INFOPARAM_ERROR;
                    break;
                }

                if (config->ppu_cfg[i].crop.enabled) {
                    if (config->ppu_cfg[i].crop.width != original_width) {
                        original_width = config->ppu_cfg[i].crop.width;
                    }
                    if (config->ppu_cfg[i].crop.height != original_height) {
                        original_height = config->ppu_cfg[i].crop.height;
                    }
                }

                log_debug(dec_ctx, "original_width = %d, original_height = %d\n",
                    original_width, original_height);

                if (config->ppu_cfg[i].scale.width == -1 &&
                    !check_scale_value(config->ppu_cfg[i].scale.height) &&
                    config->ppu_cfg[i].scale.height > 0) {
                    config->ppu_cfg[i].scale.width =
                        NEXT_MULTIPLE((original_width
                                    * config->ppu_cfg[i].scale.height)/original_height, alignw);
                    config->ppu_cfg[i].scale.height =
                        NEXT_MULTIPLE(config->ppu_cfg[i].scale.height, alignh);
                } else if (config->ppu_cfg[i].scale.height == -1 &&
                        !check_scale_value(config->ppu_cfg[i].scale.width) &&
                        config->ppu_cfg[i].scale.width > 0) {
                    config->ppu_cfg[i].scale.width =
                        NEXT_MULTIPLE(config->ppu_cfg[i].scale.width, alignw);
                    config->ppu_cfg[i].scale.height =
                        NEXT_MULTIPLE((original_height
                                    * config->ppu_cfg[i].scale.width)/original_width, alignh);
                } else if (check_scale_value(config->ppu_cfg[i].scale.width) &&
                        check_scale_value(config->ppu_cfg[i].scale.height)) {
                    config->ppu_cfg[i].scale.scale_by_ratio = 1;
                    config->ppu_cfg[i].scale.ratio_x = -config->ppu_cfg[i].scale.width;
                    config->ppu_cfg[i].scale.ratio_y = -config->ppu_cfg[i].scale.height;
                    config->ppu_cfg[i].scale.width = 0;
                    config->ppu_cfg[i].scale.height = 0;
                } else {
                    rv_info = DEC_INFOPARAM_ERROR;
                    log_error(dec_ctx,
                           "invalid scale config, scale.width: %d scale.height: %d\n",
                           config->ppu_cfg[i].scale.width,
                           config->ppu_cfg[i].scale.height);
                    break;
                }
            } else if (config->ppu_cfg[i].scale.width > 0 && config->ppu_cfg[i].scale.width > 0) {
                if(config->ppu_cfg[i].scale.width > original_width ||
                    config->ppu_cfg[i].scale.height > original_height) {
                    log_error(dec_ctx,
                           "invalid scale config, scale.width: %d scale.height: %d\n",
                           config->ppu_cfg[i].scale.width,
                           config->ppu_cfg[i].scale.height);
                    return FAILURE;
                }
            } else if (config->ppu_cfg[i].scale.width != 0 && config->ppu_cfg[i].scale.height != 0){
                log_error(dec_ctx,
                       "invalid scale config, scale.width: %d scale.height: %d\n",
                       config->ppu_cfg[i].scale.width,
                       config->ppu_cfg[i].scale.height);
                return FAILURE;
            }
        }
    }

    dec_ctx->jdec_config.dec_image_type = sequence_info->jpeg_input_info.dec_image_type;

    esjdec_ppu_print(config);

    return SUCCESS;
}

static void esjdec_modify_thum_config_by_image_info(struct DecSequenceInfo *image_info, struct DecConfig *config, uint32_t thumb_out)
{
    u32 display_width = (image_info->scaled_width + 1) & ~0x1;
    u32 display_height = (image_info->scaled_height + 1) & ~0x1;
    u32 display_width_thumb = (image_info->scaled_width_thumb + 1) & ~0x1;
    u32 display_height_thumb = (image_info->scaled_height_thumb + 1) & ~0x1;
    u32 crop_width_thumb = 0;
    u32 crop_height_thumb = 0;

    if (!config->ppu_cfg[0].crop.set_by_user) {
      config->ppu_cfg[0].crop.width = thumb_out ? display_width_thumb: display_width;
      config->ppu_cfg[0].crop.height = thumb_out ? display_height_thumb: display_height;
      config->ppu_cfg[0].crop.enabled = 1;
    }
    u32 crop_w = config->ppu_cfg[0].crop.width;
    u32 crop_h = config->ppu_cfg[0].crop.height;

    crop_width_thumb = NEXT_MULTIPLE(crop_w - 1, 2);
    crop_height_thumb = NEXT_MULTIPLE(crop_h - 1, 2);
    image_info->pic_width = NEXT_MULTIPLE(crop_width_thumb, ALIGN(config->align));
    image_info->pic_width_thumb = NEXT_MULTIPLE(crop_width_thumb, ALIGN(config->align));
    image_info->pic_height = crop_height_thumb;
    image_info->pic_height_thumb = crop_height_thumb;

    if (thumb_out == 1) {
        // set image type
        config->dec_image_type = JPEGDEC_THUMBNAIL;

        //set ppu config
        for (int i = 0; i < DEC_MAX_PPU_COUNT; i++) {
            if (!config->ppu_cfg[i].enabled)
                    continue;
            if (config->ppu_cfg[i].scale.enabled == 1) {
                config->ppu_cfg[i].scale.scale_by_ratio = 0;
                config->ppu_cfg[i].scale.width = image_info->scaled_width_thumb;
                config->ppu_cfg[i].scale.height = image_info->scaled_height_thumb;
            }
            if (config->ppu_cfg[i].crop.enabled == 1) {
                config->ppu_cfg[i].crop.enabled = 0;
                config->ppu_cfg[i].crop.set_by_user = 0;
            }
        }
    }
}

static int esjdec_enlarge_input_port(JDECContext *dec_ctx, int buf_num) {
    int ret = SUCCESS;
    int output_buffer_num;

    if (!dec_ctx) {
        log_error(dec_ctx, "error dec_ctx: %p, buf_num: %d\n", dec_ctx, buf_num);
        return FAILURE;
    }

    if (buf_num > 0) {
        output_buffer_num = buf_num < JPEG_DEFAULT_INPUT_MAX_BUFFERS ? buf_num : JPEG_DEFAULT_INPUT_MAX_BUFFERS; 
    } else {
        output_buffer_num = JPEG_DEFAULT_INPUT_BUFFERS;
    }

    ret = esdec_enlarge_input_port(dec_ctx->codec,
                                      dec_ctx->input_port,
                                      dec_ctx->dwl_ref,
                                      output_buffer_num);
    if (ret < 0) {
        log_error(dec_ctx, "es_decode_enlarge_input_port failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

static int esjdec_enlarge_out_port(JDECContext *dec_ctx, int buf_num) {
    int ret = SUCCESS;
    int output_buffer_num;

    if (!dec_ctx) {
        log_error(dec_ctx, "error dec_ctx: %p, buf_num: %d\n", dec_ctx, buf_num);
        return FAILURE;
    }

    if (buf_num > 0) {
        output_buffer_num = buf_num < JPEG_DEFAULT_OUTPUT_MAX_BUFFERS ? buf_num : JPEG_DEFAULT_OUTPUT_MAX_BUFFERS;
    } else {
        output_buffer_num = JPEG_DEFAULT_OUTPUT_BUFFERS;
    }

    ret = esdec_enlarge_output_port(dec_ctx->codec,
                                    dec_ctx->output_port,
                                    dec_ctx->dec_inst,
                                    dec_ctx->dwl_ref,
                                    output_buffer_num);
    if (ret == FAILURE) {
        log_error(dec_ctx, "esdec_enlarge_output_port error\n");
        ret = FAILURE;
    }

    return ret;
}

int ff_jdec_send_packet(JDECContext *dec_ctx, AVPacket *pkt, int timeout) {
    int ret;
    int data_size = 0;
    bool is_discard = 0;
    InputBuffer buffer;
    ESInputPort *port;

    if (!dec_ctx || !dec_ctx->input_port) {
        return AVERROR(EINVAL);
    }
    port = dec_ctx->input_port;

    if (pkt) {
        data_size = pkt->size;
    }

    if (dec_ctx->got_package_number == 2 && pkt->size > 0) {
        esjdec_enlarge_input_port(dec_ctx, dec_ctx->input_buf_num);
    }

    is_discard = esjdec_drop_pkt(dec_ctx);
    if (is_discard && pkt && pkt->size > 0) {
        dec_ctx->drop_pkt_number++;
        log_debug(dec_ctx,
                  "drop pkt number: %d, has dropped pkt number: %d\n",
                  dec_ctx->got_package_number,
                  dec_ctx->drop_pkt_number);
        return SUCCESS;
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
                log_debug(dec_ctx,
                              "get input buffer virtual_address: %p",
                              buffer.vir_addr);
            }
            esdec_push_input_packet_buffer(port->packet_queue, &buffer);
        }
    }

    return ret;
}

int ff_jdec_send_packet_receive_frame(JDECContext *dec_ctx, AVPacket *pkt, AVFrame *frame) {
    int ret;
    if (!dec_ctx || !pkt || !frame) {
        log_error(dec_ctx, "error !!! dec_ctx or pkt or frame is null pkt: %p, frame: %p\n", pkt, frame);
        return FAILURE;
    }

    for (;;) {
        ret = ff_jdec_send_packet(dec_ctx, pkt, 20 /*20ms*/);
        if (ret == SUCCESS) {
            ret = ff_jdec_get_frame(dec_ctx, frame, 0);
            av_packet_unref(pkt);
            break;
        } else {
            ret = ff_jdec_get_frame(dec_ctx, frame, 0);
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

static int esjdec_fill_frame_prop(JDECContext *dec_ctx, OutputBuffer *buffer, AVFrame *frame) {
    int ret = SUCCESS;
    DecPicturePri *pri_pic;
    OutPutInfo *info;
    ESOutputMemory *memory;
    uint64_t dma_fd;
    if (!dec_ctx || !buffer || !frame) {
        return FAILURE;
    }

    ret = ff_decode_frame_props(dec_ctx->avctx, frame);
    if (ret < 0) {
        av_log(dec_ctx, AV_LOG_ERROR, "ff_decode_frame_props failed\n");
        return ret;
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

int ff_jdec_get_frame(JDECContext *dec_ctx, AVFrame *frame, int timeout_ms) {
    int ret;
    int is_discard = 0;
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
        ret = esjdec_fill_frame_prop(dec_ctx, &buffer, frame);
        if (ret < 0) {
            log_error(dec_ctx, "esjdec_fill_frame_prop failed\n");
            esdec_release_output_buffer(port->consumed_queue, &buffer);
        } else {
            dec_ctx->pic_output_number++;
            log_info(dec_ctx,
                "%d got frame :size=%dx%d,data[0]=%p,data[1]=%p,buf[0]=%p,pts=%lld\n",
                dec_ctx->pic_output_number,
                frame->width,
                frame->height,
                frame->data[0],
                frame->data[1],
                frame->buf[0],
                frame->pts);
        }
    }

    if (frame->buf[0]) {
        log_info(dec_ctx, "buffer picture: %p\n", buffer.memory->vir_addr);
        ret = SUCCESS;
    }

    return ret;
}

static int esjdec_fill_thumb_mem(struct DecPicturePpu *pic, ESThumbOutputMemory *thumb_mem) {
    int ret = FAILURE;
    uint32_t *virtual_address;

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        virtual_address = pic->pictures[i].luma.virtual_address;
        if (virtual_address != NULL && virtual_address == thumb_mem->virtual_address) {
            ret = SUCCESS;
            thumb_mem->picture = *pic;
            thumb_mem->state = Using;
            log_debug(NULL, "this is a thumbnail pic, virtual_address: %p\n", virtual_address);
            break;
        }
    }

    return ret;
}

static int esjdec_fill_thumb_mem_into_output_buffer(OutputBuffer *buffer, ESThumbOutputMemory *thumb_mem, int index) {
    int ret = FAILURE;
    DecPicturePri *pri_pic = NULL;
    struct DecPicturePpu *pic;
    ESOutputMemory *memory;

    if (!buffer || !thumb_mem) {
        log_error(NULL, "error !!! buffer: %p, thumb_mem: %p\n", buffer, thumb_mem);
        return FAILURE;
    }

    memory = buffer->memory;
    pri_pic = &memory->pic_pri;
    pic = &thumb_mem->picture;

    if (thumb_mem->state != Using) {
        log_error(NULL, "thumbnail memory has no valid data\n");
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (pic->pictures[i].luma.virtual_address != NULL && index == i) {
            pri_pic->pic_count++;
            pri_pic->pictures[2].fd = thumb_mem->fd[i];
            esdec_fill_planes(&pri_pic->pictures[2], &pic->pictures[i]);
            ret = SUCCESS;
            log_info(NULL, "index: %d, virtual_address: %p\n", i, pic->pictures[i].luma.virtual_address);
            break;
        }
    }
    thumb_mem->state = Used;

    return ret;
}

static int esjdec_fill_output_buffer(JDECContext *dec_ctx, struct DecPicturePpu *pic)
{
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
            if (i == dec_ctx->pp_out) {
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
            log_info(dec_ctx, "DEFAULT_INDEX: %d, real pic_index: %d\n", dec_ctx->pp_out, index);
            dec_ctx->pp_out = index;
        }

        pic_id = dec_ctx->pic_id;
        if (dec_ctx->get_reorder_pkt_by_pic_id) {
            int ret = dec_ctx->get_reorder_pkt_by_pic_id(dec_ctx, pic_id, &pkt);
            if (ret == SUCCESS) {
                buffer->pts = pkt.pts;
                buffer->reordered_opaque = pkt.reordered_opaque;
            } else {
                log_error(dec_ctx, "get reorder pkt failed pic_id: %d\n", pic_id);
            }
        }

        log_info(dec_ctx, ": %d\n", dec_ctx->pp_out, index);

        if (!dec_ctx->picture) {
            dec_ctx->picture = av_mallocz(sizeof(*dec_ctx->picture));
            if (!dec_ctx->picture) {
                log_error(dec_ctx, "av_malloc picture fialed\n");
            } else {
                *dec_ctx->picture = *pic;
            }
        }

        if (dec_ctx->thumb_mem.state == Using) {
            ret = esjdec_fill_thumb_mem_into_output_buffer(buffer, &dec_ctx->thumb_mem, dec_ctx->pp_out);
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
        log_error(dec_ctx, "error !!! no picture\n");
        return FAILURE;
    }

    return ret;
}

static enum DecRet esjdec_jpeg_next_picture(const void *inst, struct DecPicturePpu *pic,  uint32_t thumb_out)
{
    enum DecRet rv;
    JpegDecOutput jpic;
    JpegDecImageInfo info;
    u32 stride, stride_ch, i;

    memset(&jpic, 0, sizeof(JpegDecOutput));
    memset(pic, 0, sizeof(struct DecPicturePpu));
    rv = JpegDecNextPicture(inst, &jpic, &info);

    if (rv != DEC_PIC_RDY)
        return rv;
    for (i = 0; i < DEC_MAX_OUT_COUNT; i++)
    {
        stride = jpic.pictures[i].pic_stride;
        stride_ch = jpic.pictures[i].pic_stride_ch;

        pic->pictures[i].picture_info.cycles_per_mb = jpic.cycles_per_mb;
        pic->pictures[i].luma = jpic.pictures[i].output_picture_y;
        pic->pictures[i].chroma = jpic.pictures[i].output_picture_cb_cr;
        pic->pictures[i].chroma_cr = jpic.pictures[i].output_picture_cr;
        pic->pictures[i].sequence_info.pic_width = jpic.pictures[i].output_width;
        pic->pictures[i].sequence_info.pic_height = jpic.pictures[i].output_height;
        pic->pictures[i].sequence_info.scaled_width = jpic.pictures[i].display_width;
        pic->pictures[i].sequence_info.scaled_height = jpic.pictures[i].display_height;
        pic->pictures[i].sequence_info.pic_width_thumb = jpic.pictures[i].output_width_thumb;
        pic->pictures[i].sequence_info.pic_height_thumb = jpic.pictures[i].output_height_thumb;
        pic->pictures[i].sequence_info.scaled_width_thumb = jpic.pictures[i].display_width_thumb;
        pic->pictures[i].sequence_info.scaled_height_thumb = jpic.pictures[i].display_height_thumb;
        pic->pictures[i].sequence_info.bit_depth_luma = jpic.bit_depth;
        pic->pictures[i].sequence_info.bit_depth_chroma = jpic.bit_depth;

        if(thumb_out == 0){
            pic->pictures[i].pic_width = jpic.pictures[i].display_width;
            pic->pictures[i].pic_height = jpic.pictures[i].display_height;
        }else{
            pic->pictures[i].pic_width = jpic.pictures[i].display_width_thumb;
            pic->pictures[i].pic_height = jpic.pictures[i].display_height_thumb;
        }

        if (IS_PIC_TILE(jpic.pictures[i].output_format)) {
            pic->pictures[i].luma.size = stride * (NEXT_MULTIPLE(pic->pictures[i].pic_height, 4) / 4);
            pic->pictures[i].chroma.size = stride_ch * (NEXT_MULTIPLE(pic->pictures[i].pic_height / 2, 4) / 4);
        } else if (IS_PIC_PLANAR(jpic.pictures[i].output_format)) {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height;
            pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height;
        } else if (jpic.pictures[i].output_format == DEC_OUT_FRM_RFC) {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height / 4;
            pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height / 8;
        } else {
            pic->pictures[i].luma.size = stride * pic->pictures[i].pic_height;
            if (!IS_PIC_RGB(jpic.pictures[i].output_format))
                pic->pictures[i].chroma.size = stride_ch * pic->pictures[i].pic_height / 2;
        }

        pic->pictures[i].pic_stride = jpic.pictures[i].pic_stride;
        pic->pictures[i].pic_stride_ch = jpic.pictures[i].pic_stride_ch;
        pic->pictures[i].picture_info.format = jpic.pictures[i].output_format;
#ifdef SUPPORT_DEC400
        pic->pictures[i].dec400_luma_table = jpic.pictures[i].dec400_luma_table;
        pic->pictures[i].dec400_chroma_table = jpic.pictures[i].dec400_chroma_table;

#endif
    }
    pic->pictures[0].sequence_info.output_format = info.output_format;
    pic->pictures[0].sequence_info.output_format_thumb = info.output_format_thumb;
    pic->pictures[0].sequence_info.coding_mode = info.coding_mode;
    pic->pictures[0].sequence_info.coding_mode_thumb = info.coding_mode_thumb;
    pic->pictures[0].sequence_info.thumbnail_type = info.thumbnail_type;

    return rv;
}

static int esjdec_get_next_picture(JDECContext *dec_ctx) {
    enum DecRet ret;
    int rv = SUCCESS;
    struct DecPicturePpu pic;

    if (!dec_ctx) {
        log_error(NULL, "error !!! dec_ctx: %p\n", dec_ctx);
        return FAILURE;
    }

    ret = esjdec_jpeg_next_picture(dec_ctx->dec_inst, &pic, dec_ctx->thumb_out);
    log_info(dec_ctx, "JpegNextPicture return: %d\n", ret);

    if (ret == DEC_PIC_RDY) {
        dec_ctx->pic_display_number++;
        for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
            if(pic.pictures[i].luma.virtual_address != NULL) {
                log_debug(dec_ctx, "Dec pic rdy, %d -> %d x %d -luma.virtual_address=%p"
                                            "-luma.bus_address=%ld -luma.size=%d stride=%d"
                                            "-sequence_info.pic_height=%d -sequence_info.pic_width=%d \n",
                                            i,
                                            pic.pictures[i].pic_width,
                                            pic.pictures[i].pic_height,
                                            pic.pictures[i].luma.virtual_address,
                                            pic.pictures[i].luma.bus_address,
                                            pic.pictures[i].luma.size,
                                            pic.pictures[i].sequence_info.pic_stride,
                                            pic.pictures[i].sequence_info.pic_height,
                                            pic.pictures[i].sequence_info.pic_width);
            }
        }

        // create frame file
        for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++)
            if(!dec_ctx->frame_dump_handle[i])
                esjdec_init_frame_dump_handle(dec_ctx, &pic);
        esjdec_frame_dump(dec_ctx, &pic);

        ret = esjdec_fill_thumb_mem(&pic, &dec_ctx->thumb_mem);
        if (ret == SUCCESS) {
            log_info(dec_ctx, "this is a thumb pic, skip fill output buffer\n");
            return ret;
        }

        rv = esjdec_fill_output_buffer(dec_ctx, &pic);
        if (rv < 0) {
            log_error(dec_ctx, "esjdec_fill_output_buffer failed,\n");
        }
    } else if (ret == DEC_END_OF_STREAM) {
        log_debug(dec_ctx, "End of stream!\n");
        return FAILURE;
    }
    return SUCCESS;
}

static void esjdec_eos_process(JDECContext *dec_ctx) {
    int ret;
    if (!dec_ctx || !dec_ctx->output_port) {
        return;
    }
    log_info(dec_ctx, "JpegDecEndOfStream start\n");
    JpegDecEndOfStream(dec_ctx->dec_inst);

    for (;;) {
        ret = esjdec_get_next_picture(dec_ctx);
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

static int esjdec_set_ppu_output_format(JDECContext *dec_ctx){
    if (!dec_ctx) {
        log_error(dec_ctx, "esjdec_set_ppu_output_format dec_ctx invaild\n");
        return FAILURE;
    }

    struct DecConfig *config = &dec_ctx->jdec_config;

    for(int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if(config->ppu_cfg[i].enabled == 1) {

            enum DecPictureFormat dstpicfmt = ff_codec_pixfmt_to_decfmt(dec_ctx->output_format[i]);

            log_info(NULL,
                   "dec_ctx->output_format[%d]; %d  dstpicfmt: %d\n",
                    i,
                    dec_ctx->output_format[i],dstpicfmt);

            switch (dstpicfmt) {
                case DEC_OUT_FRM_NV21SP:
                    config->ppu_cfg[i].cr_first = 1;
                    break;

                case DEC_OUT_FRM_YUV420SP:
                    // todo
                    break;

                case DEC_OUT_FRM_YUV420P:
                    config->ppu_cfg[i].planar = 1;
                    break;

                case DEC_OUT_FRM_YUV400:
                    config->ppu_cfg[i].monochrome = 1;
                    break;

                case DEC_OUT_FRM_RGB888:
                case DEC_OUT_FRM_BGR888:
                case DEC_OUT_FRM_XRGB888:
                case DEC_OUT_FRM_XBGR888:
                    config->ppu_cfg[i].rgb = 1;
                    config->ppu_cfg[i].rgb_format = dstpicfmt;
                    break;

                case DEC_OUT_FRM_ARGB888:
                case DEC_OUT_FRM_ABGR888:
                    config->ppu_cfg[i].rgb = 1;
                    config->ppu_cfg[i].rgb_stan = BT709;
                    config->ppu_cfg[i].rgb_alpha = 255;
                    config->ppu_cfg[i].rgb_format = dstpicfmt;
                    break;

                default:
                    log_error(NULL,"[%s:%d] set ppu[%d] output_format failed,"
                                             "avctx->output_format: %d dstpicfmt: %d\n",
                                              __func__, __LINE__, i,
                                              dec_ctx->output_format[i], dstpicfmt);
                    return FAILURE;
            }
        }
    }

    return SUCCESS;
}

static int esjdec_parse_ppset(JDECContext *dec_ctx, CropInfo *crop_info, ScaleInfo *scale_info) {
    if (!dec_ctx) {
        log_error(dec_ctx, "esjdec_parse_ppset dec_ctx invaild\n");
        return FAILURE;
    }
    if (!crop_info) {
        log_error(dec_ctx, "esjdec_parse_ppset crop_info invaild\n");
        return FAILURE;
    }
    if (!scale_info) {
        log_error(dec_ctx, "esjdec_parse_ppset scale_info invaild\n");
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        log_debug(dec_ctx, "dec_ctx->pp_setting[%d]: %s\n", i, dec_ctx->pp_setting[i]);

        if (dec_ctx->pp_setting[i] || dec_ctx->crop_set[i] || (i == 1 && dec_ctx->scale_set)) {
            int ret = 0;

            if (dec_ctx->pp_setting[i]) {
                ret = ff_codec_get_crop(dec_ctx->pp_setting[i], crop_info + i);
            } else if (dec_ctx->crop_set[i]) {
                ret = es_codec_get_crop(dec_ctx->crop_set[i], crop_info + i);
            }
            if (ret < 0) {
                log_error(dec_ctx, "picture_%d crop config error, please check!\n", i);
                return FAILURE;
            }

            if( i == 0)
                continue;

            ret = 0;
            if (dec_ctx->pp_setting[i]) {
                ret = ff_dec_get_scale(dec_ctx->pp_setting[i], scale_info + i, i);
            } else if (dec_ctx->scale_set) {
                ret = es_codec_get_scale(dec_ctx->scale_set, scale_info + i);
            }
            if (ret < 0) {
                log_error(dec_ctx, "picture_%d scale config error, please check!\n", i);
                return FAILURE;
            }
        }
    }

    return SUCCESS;
}

static int esjdec_set_ppu_crop_and_scale(JDECContext *dec_ctx)
{
    struct DecConfig *config = &dec_ctx->jdec_config;
    CropInfo crop_paras[ES_VID_DEC_MAX_OUT_COUNT];
    ScaleInfo scale_paras[ES_VID_DEC_MAX_OUT_COUNT];
    int pp_enabled = 0;

    memset(crop_paras, 0, sizeof(CropInfo) * ES_VID_DEC_MAX_OUT_COUNT);
    memset(scale_paras, 0, sizeof(ScaleInfo) * ES_VID_DEC_MAX_OUT_COUNT);

    int ret = esjdec_parse_ppset(dec_ctx, &crop_paras, &scale_paras);
    if (ret < 0) {
        log_error(dec_ctx, "parse ppset error\n");
        return FAILURE;
    }

    //set crop paras
    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        // if config->ppu_cfg[i].enabled==0, crop is not assigned
        if (!config->ppu_cfg[i].enabled ||
            (crop_paras[i].crop_xoffset == 0 && crop_paras[i].crop_yoffset == 0
            && crop_paras[i].crop_width == 0 && crop_paras[i].crop_height == 0)) {
            continue;
        }

        pp_enabled = 1;
        config->ppu_cfg[i].crop.enabled = 1;
        config->ppu_cfg[i].crop.set_by_user = 1;
        config->ppu_cfg[i].crop.x = crop_paras[i].crop_xoffset;
        config->ppu_cfg[i].crop.y = crop_paras[i].crop_yoffset;
        config->ppu_cfg[i].crop.width = crop_paras[i].crop_width;
        config->ppu_cfg[i].crop.height = crop_paras[i].crop_height;

        log_debug(dec_ctx,
               "[%s:%d]  ppu_cfg[%d].crop.x: %d  "
                "ppu_cfg[%d].crop.y: %d  "
                "ppu_cfg[%d].crop.width: %d  "
                "ppu_cfg[%d].crop.height: %d\n",
                __func__,__LINE__,
                i, config->ppu_cfg[i].crop.x,
                i, config->ppu_cfg[i].crop.y,
                i, config->ppu_cfg[i].crop.width,
                i, config->ppu_cfg[i].crop.height);
    }

    //set scale paras,pp0 not support scale
    for (int i = 1; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        // if config->ppu_cfg[i].enabled==0, scale is not assigned
        if (!config->ppu_cfg[i].enabled ||
            scale_paras[i].scale_width == 0 && scale_paras[i].scale_height == 0) {
            continue;
        }
        pp_enabled = 1;

        if (!(scale_paras[i].scale_width == 0 && scale_paras[i].scale_height == 0)) {
            config->ppu_cfg[i].scale.enabled = 1;
            config->ppu_cfg[i].scale.width = scale_paras[i].scale_width;
            config->ppu_cfg[i].scale.height = scale_paras[i].scale_height;
            log_debug(dec_ctx,
                      "[%s:%d]  ppu_cfg[%d].scale.width: %d   "
                      "ppu_cfg[%d].scale.height: %d",
                      __func__, __LINE__,
                      i, config->ppu_cfg[i].scale.width,
                      i, config->ppu_cfg[i].scale.height);
        }
    }

    dec_ctx->pp_enabled = pp_enabled;

    return SUCCESS;
}

static int esjdec_dec_init_ppu_cfg(JDECContext *dec_ctx)
{
    struct DecConfig *config = &dec_ctx->jdec_config;
    enum DecPictureFormat dstpicfmt;
    int ret = 0;

    if (dec_ctx->scale_set == NULL)
        log_error(dec_ctx, "scale_set is null\n");

    dstpicfmt = ff_codec_pixfmt_to_decfmt(dec_ctx->output_format[0]);
    if(IS_PIC_RGB(dstpicfmt) && dec_ctx->cfg_pp_enabled[0] == 1) {
        log_error(dec_ctx, "cannot set pp0 output format as rgb.\n",dec_ctx->pp_out);
        return FAILURE;
    }

    if (dec_ctx->cfg_pp_enabled[0] == 1 && dec_ctx->cfg_pp_enabled[1] == 0) {
        if(dec_ctx->pp_out == 1) {
            log_error(dec_ctx, "pp_out=1, pp1 disable.\n",dec_ctx->pp_out);
            return FAILURE;
        }
    } else if (dec_ctx->cfg_pp_enabled[0] == 0 && dec_ctx->cfg_pp_enabled[1] == 1) {
        if(dec_ctx->pp_out == 0) {
            log_error(dec_ctx, "pp_out=0, but pp0 disable.\n",dec_ctx->pp_out);
            return FAILURE;
        }
    } else if (dec_ctx->cfg_pp_enabled[0] == 0 && dec_ctx->cfg_pp_enabled[1] == 0) {
        log_error(dec_ctx, "pp0,pp1 are both disable.\n",dec_ctx->pp_out);
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_ctx->cfg_pp_enabled[i] == 1) {
            dec_ctx->pp_count++;
        }
        config->ppu_cfg[i].enabled = dec_ctx->cfg_pp_enabled[i];
    }

    //set crop and scale
    ret = esjdec_set_ppu_crop_and_scale(dec_ctx);
    if (ret < 0) {
        log_error(dec_ctx, "pp0,pp1 are both disable.\n",dec_ctx->pp_out);
        return FAILURE;
    }

    //set output_format
    ret = esjdec_set_ppu_output_format(dec_ctx);
    if (ret < 0) {
        log_error(dec_ctx, "set ppu output format failed.\n",dec_ctx->pp_out);
        return FAILURE;
    }

    dec_ctx->pp_enabled = 1;

    return SUCCESS;
}

static int esjdec_reorder_packet_enqueue(JDECContext *dec_ctx, ReorderPkt *pkt) {
    int ret = FAILURE;
    if (!dec_ctx || !pkt) {
        return FAILURE;
    }

    ret = es_reorder_packet_enqueue(dec_ctx->reorder_queue, pkt);
    return ret;
}

static int esjdec_reorder_pkt_dequeue(JDECContext *dec_ctx, int pic_id, struct ReorderPkt *out_pkt) {
    int ret;
    if (!dec_ctx || !out_pkt) {
        return FAILURE;
    }

    ret = es_reorder_packet_dequeue(dec_ctx->reorder_queue, pic_id, out_pkt);
    return ret;
}

static int esjdec_set_default_dec_config(JDECContext *dec_ctx)
{
    int ret = 0;

    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is invaild\n");
        return FAILURE;
    }

    dec_ctx->align = esdec_get_align(dec_ctx->stride_align);
    dec_ctx->dwl_init.client_type = DWL_CLIENT_TYPE_JPEG_DEC;

    dec_ctx->pic_display_number = 0;
    dec_ctx->got_package_number = 0;
    dec_ctx->pic_decode_number = 0;
    dec_ctx->pic_output_number = 0;
    dec_ctx->got_inputbuf_number = 0;
    dec_ctx->drop_pkt_number = 0;
    dec_ctx->prev_width = 0;
    dec_ctx->prev_height = 0;
    dec_ctx->frame_dump_handle[0] = NULL;
    dec_ctx->frame_dump_handle[1] = NULL;
    dec_ctx->pkt_dump_handle = NULL;
    dec_ctx->thumb_exist = 0;
    dec_ctx->thumb_out = 0;
    dec_ctx->thumb_done = 0;
    dec_ctx->need_out_buf_for_thumb = 0;
    dec_ctx->codec = ES_JPEG;
    dec_ctx->pp_count = 0;

    dec_ctx->jdec_config.align = dec_ctx->align;
    dec_ctx->jdec_config.dec_image_type = JPEGDEC_IMAGE;
    memset(dec_ctx->jdec_config.ppu_cfg, 0, sizeof(dec_ctx->jdec_config.ppu_cfg));
    memset(dec_ctx->jdec_config.delogo_params, 0, sizeof(dec_ctx->jdec_config.delogo_params));

#ifdef MODEL_SIMULATION
    g_hw_build_id = 0x1FB1;
    g_hw_ver = 19001;
    g_hw_id = 1000;
#endif

    dec_ctx->reorder_queue = es_queue_create();
    if (!dec_ctx->reorder_queue) {
        log_error(dec_ctx, "reorder_queue create failed\n");
        return FAILURE;
    }
    dec_ctx->store_reorder_pkt = esjdec_reorder_packet_enqueue;
    dec_ctx->get_reorder_pkt_by_pic_id = esjdec_reorder_pkt_dequeue;

    dec_ctx->frame = av_frame_alloc();
    if (!dec_ctx->frame) {
        log_error(dec_ctx, "av_frame_alloc failed\n");
        return FAILURE;
    }

    if (dec_ctx->fdump) {
        dec_ctx->frame_dump[0] = dec_ctx->cfg_pp_enabled[0];
        dec_ctx->frame_dump[1] = dec_ctx->cfg_pp_enabled[1];
    }

    return ret;
}

int ff_jdec_set_dec_config(JDECContext *dec_ctx) {
    int ret = SUCCESS;
    if (!dec_ctx) {
        log_error(NULL, "dec_ctx is invaild\n");
        return FAILURE;
    }

    // set default paras
    ret = esjdec_set_default_dec_config(dec_ctx);
    if (ret < 0){
        log_error(dec_ctx, "esjdec_set_default_dec_config failed\n");
        return FAILURE;
    } else {
        log_info(dec_ctx, "esjdec_set_default_dec_config success\n");
    }

    // set ppu config
    ret = esjdec_dec_init_ppu_cfg(dec_ctx);
    if (ret < 0){
        log_error(dec_ctx, "esjdec_dec_init_ppu_cfg failed\n");
        return FAILURE;
    } else {
        log_info(dec_ctx, "esjdec_dec_init_ppu_cfg success\n");
    }

    return ret;
}

static enum DecRet esjdec_set_jpeg_info(const void *inst, struct DecConfig config)
{
    struct JpegDecConfig dec_cfg;
    dec_cfg.dec_image_type = config.dec_image_type;
    dec_cfg.align = config.align;

    DWLmemcpy(dec_cfg.ppu_config, config.ppu_cfg, sizeof(config.ppu_cfg));
    DWLmemcpy(dec_cfg.delogo_params, config.delogo_params, sizeof(config.delogo_params));

    return JpegDecSetInfo(inst, &dec_cfg);
}

static int esdec_thumb_mem_fd_split(void *dwl_inst,
                                        void *dec_inst,
                                        ESThumbOutputMemory *memory,
                                        struct DecConfig *dec_config) {
    int ret = SUCCESS;
    int dma_fd;
    int pp_count = 0;

    if (!dwl_inst || !dec_inst || !memory || !dec_config) {
        log_error(NULL,
                  "error !!! dwl_inst: %p, dec_inst: %p, memory: %p, dec_config: %p\n",
                  dwl_inst,
                  dec_inst,
                  memory,
                  dec_config);

        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        memory->fd[i] = -1;
    }

    if (ESDecIsSimulation()) {
        return SUCCESS;
    }

    dma_fd = ESDecGetDmaBufFd(&memory->mem);
    if (dma_fd < 0) {
        log_error(NULL, "dma fd is error dma_fd: %d\n", dma_fd);
        return FAILURE;
    }

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (dec_config->ppu_cfg[i].enabled) {
            memory->fd[i] = dma_fd;
            pp_count++;
        }
    }
    log_info(NULL, "pp_count: %d\n", pp_count);

    if (pp_count == ES_VID_DEC_MAX_OUT_COUNT) {
        ret = ESDecGetDmaFdSplit(dwl_inst, dec_inst, dma_fd, memory->fd, ES_VID_DEC_MAX_OUT_COUNT);
    }

    return ret;
}

static int esjdec_allocate_thumbnail_memory(ESThumbOutputMemory *thumb_mem,
                                            void* dec_inst,
                                            struct AVBufferRef *dwl_ref,
                                            struct DecConfig *dec_config) {
    struct DWLLinearMem *mem;
    struct DecBufferInfo info;
    AVBufferRef *buffer_ref;
    void *dwl_inst;
    enum DecRet rv;

    if (!thumb_mem || !dec_inst || !dwl_ref || !dwl_ref->data || !dec_config) {
        log_error(NULL, "dec_inst or dwl_inst is null dec_inst: %p, dec_config: %p\n", dec_inst, dec_config);
        return FAILURE;
    }

    rv = JpegDecGetBufferInfo(dec_inst, &info);
    if (rv != DEC_OK) {
        log_error(NULL, "JpegDecGetBufferInfo failed, rv: %d\n", rv);
        return FAILURE;
    }

    dwl_inst = dwl_ref->data;

    if (info.next_buf_size != 0) {
        mem = &thumb_mem->mem;
        mem->mem_type = DWL_MEM_TYPE_DPB;
        if (DWLMallocLinear(dwl_inst, info.next_buf_size, mem) != DWL_OK) {
            log_error(NULL, "DWLMallocLinear failed size: %d\n", info.next_buf_size);
            return FAILURE;
        }
        if (mem->virtual_address == NULL) {
            log_error(NULL, "virtual_address is null\n");
            return FAILURE;
        }

        struct ESDecoderWrapper esdec;
        esdec.codec = DEC_JPEG;
        esdec.inst = dec_inst;
        esdec_thumb_mem_fd_split(dwl_inst, &esdec, &thumb_mem, dec_config);

        thumb_mem->virtual_address = mem->virtual_address;

        rv = JpegDecAddBuffer(dec_inst, mem);
        log_info(NULL, "thumbnail output virtual_addres: %p, rv: %d\n", mem->virtual_address, rv);
        if (rv == DEC_OK) {
            log_debug(NULL, "DecAddBuffer return DEC_OK\n");
            thumb_mem->state = No_Use;
        } else {
            log_error(NULL, "DecAddBuffer failed, rv: %d\n", rv);
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int esjdec_release_thumbnail_memory(ESThumbOutputMemory *thumb_mem, struct AVBufferRef *dwl_ref)
{
    int dma_fd = -1;
    void *dwl_inst;

    if (!thumb_mem || !dwl_ref || !dwl_ref->data) {
        log_error(NULL, "thumb_mem or dwl_inst is null thumb_mem: %p, dwl_ref: %p\n", thumb_mem, dwl_ref);
        return FAILURE;
    }

    if (thumb_mem->state == No_Use) {
        log_info(NULL, "thumbnail memory is not allocated, do not need to release\n");
        return SUCCESS;
    }

    dwl_inst = dwl_ref->data;

    dma_fd = ESDecGetDmaBufFd(&thumb_mem->mem);
    log_info(NULL,
             "thumbnail memory dma_fd: %d, size: %d, virtual_address: %p\n",
             dma_fd,
             thumb_mem->mem.size,
             thumb_mem->mem.virtual_address);

    for (int i = 0; i < ES_VID_DEC_MAX_OUT_COUNT; i++) {
        if (thumb_mem->fd[i] >= 0) {
            if (dma_fd != thumb_mem->fd[i]) {
                log_info(NULL, "output buffer close  pp_fd[%d]: %d\n", i, thumb_mem->fd[i]);
                close(thumb_mem->fd[i]);
            }

            thumb_mem->fd[i] = -1;
        }
    }

    DWLFreeLinear(dwl_inst, &thumb_mem->mem);

    return SUCCESS;
}

static enum DecRet esjdec_decode_one_pic(void* inst, struct DecInputParameters* jpeg_in)
{
    enum DecRet rv;
    JpegDecInput jpeg_input;
    JpegDecOutput jpeg_out;
    DWLmemset(&jpeg_input, 0, sizeof(jpeg_input));
    DWLmemset(&jpeg_out, 0, sizeof(jpeg_out));

    jpeg_input.stream_buffer =jpeg_in->stream_buffer;
    jpeg_input.stream_length = jpeg_in->strm_len;
    jpeg_input.buffer_size = jpeg_in->buffer_size;
    jpeg_input.dec_image_type = jpeg_in->dec_image_type;
    jpeg_input.slice_mb_set = jpeg_in->slice_mb_set;
    jpeg_input.ri_count = jpeg_in->ri_count;
    jpeg_input.ri_array = jpeg_in->ri_array;
    jpeg_input.picture_buffer_y = jpeg_in->picture_buffer_y;
    jpeg_input.picture_buffer_cb_cr = jpeg_in->picture_buffer_cb_cr;
    jpeg_input.picture_buffer_cr = jpeg_in->picture_buffer_cr;
    jpeg_input.p_user_data = jpeg_in->p_user_data;
    jpeg_input.stream = jpeg_in->stream;

    rv = JpegDecDecode(inst, &jpeg_input, &jpeg_out);

    esjdec_print_decode_return(rv);

    return rv;
}

static int esjdec_decode(JDECContext *dec_ctx,
                         InputBuffer *input_buffer,
                         struct DecSequenceInfo *sequence_info,
                         int *peek_frame) {
    int ret = SUCCESS;
    enum DecRet rv;
    ReorderPkt reorder_pkt;
    struct DWLLinearMem *buffer = NULL;

    if (!dec_ctx || !input_buffer || !peek_frame) {
        log_error(dec_ctx, "error dec_ctx: %p, buffer: %p\n", dec_ctx, input_buffer);
        return FAILURE;
    }
    *peek_frame = 0;
    log_debug(dec_ctx, "input_buffer->size: %d\n", input_buffer->size);

    reorder_pkt.pts = input_buffer->pts;
    reorder_pkt.reordered_opaque = input_buffer->reordered_opaque;

    sequence_info->jpeg_input_info.pic_id = dec_ctx->pic_decode_number;
    dec_ctx->pic_id = dec_ctx->pic_decode_number;

    do {
        ret = esjdec_decode_one_pic((void* )dec_ctx->dec_inst, &sequence_info->jpeg_input_info);

        switch (ret) {
            case DEC_PIC_RDY:
                reorder_pkt.pic_id = dec_ctx->pic_decode_number;
                if (dec_ctx->store_reorder_pkt) {
                    dec_ctx->store_reorder_pkt(dec_ctx, &reorder_pkt);
                }
                dec_ctx->pic_decode_number += 1;
                *peek_frame = 1;
                log_info(dec_ctx, "\t-JPEG: DEC_PIC_RDY\n");
                break;
            case DEC_SCAN_PROCESSED:
                log_info(dec_ctx, "\t-JPEG: DEC_PIC_RDY\n");
                break;
            case DEC_SLICE_RDY:
                log_info(dec_ctx, "\t-JPEG: DEC_SLICE_RDY\n");
                break;
            case DEC_NO_DECODING_BUFFER:
                if (!dec_ctx->output_port) {
                    dec_ctx->output_port =
                    esdec_allocate_output_port(dec_ctx->codec, dec_ctx->dec_inst, dec_ctx->dwl_ref, dec_ctx->pp_count);
                    if (!dec_ctx->output_port) {
                        break;
                    }
                }
                if (dec_ctx->got_inputbuf_number == 2) {
                    esjdec_enlarge_out_port(dec_ctx, dec_ctx->output_buf_num);
                }
                ret = esdec_wait_picture_consumed_until_timeout(dec_ctx->codec,
                                                                dec_ctx->dec_inst,
                                                                dec_ctx->output_port,
                                                                20 /*ms*/);
                if (ret == AVERROR_EXIT) {
                   return ret;
                }
            case DEC_WAITING_FOR_BUFFER:
                if (!dec_ctx->output_port) {
                    dec_ctx->output_port =
                    esdec_allocate_output_port(dec_ctx->codec, dec_ctx->dec_inst, dec_ctx->dwl_ref, dec_ctx->pp_count);
                    if (!dec_ctx->output_port) {
                        break;
                    }
                }
            case DEC_STRM_PROCESSED:
                log_debug(dec_ctx, "\t-JPEG: DEC_STRM_PROCESSED\n");
                break;
            case DEC_STRM_ERROR:
                log_debug(dec_ctx, "\t-JPEG: DEC_STRM_ERROR\n");
                return FAILURE;
            default:
                log_error(dec_ctx, "jpegdecode return: %d\n", ret);
                return FAILURE;
        }
    } while (ret != DEC_PIC_RDY);

    return SUCCESS;
}

static void esjdec_init_input_paras(InputBuffer *buffer, struct DecInputParameters *jpeg_in) {
    jpeg_in->stream_buffer.virtual_address = (u32 *) buffer->vir_addr;
    jpeg_in->stream_buffer.bus_address =  (addr_t)buffer->bus_address;
    jpeg_in->stream_buffer.logical_size =(u32) buffer->logical_size;
    jpeg_in->stream = (u8 *) buffer->vir_addr;
    jpeg_in->strm_len = buffer->size;
    jpeg_in->buffer_size = 0;
    jpeg_in->dec_image_type = JPEGDEC_IMAGE;
}

static int esjdec_set_image_type(JDECContext *dec_ctx,
                                  struct DecSequenceInfo *sequence_info)
{
    int ret = SUCCESS;

    if(sequence_info->thumbnail_type == JPEGDEC_THUMBNAIL_JPEG) {
        if (dec_ctx->thumb_mode == Only_Decode_Thumb) {
            sequence_info->jpeg_input_info.dec_image_type = JPEGDEC_THUMBNAIL;
            dec_ctx->thumb_out = 1;
        } else if (dec_ctx->thumb_mode == Only_Decode_Pic) {
            sequence_info->jpeg_input_info.dec_image_type = JPEGDEC_IMAGE;
        } else {
            sequence_info->jpeg_input_info.dec_image_type = JPEGDEC_THUMBNAIL;
            dec_ctx->thumb_out = 1;
            dec_ctx->need_out_buf_for_thumb = 1;
        }
        log_debug(NULL, "thumbnail exits\n");
    } else if((sequence_info->thumbnail_type == JPEGDEC_NO_THUMBNAIL)
                ||(sequence_info->thumbnail_type == JPEGDEC_THUMBNAIL_NOT_SUPPORTED_FORMAT)) {
        if (dec_ctx->thumb_mode == Only_Decode_Thumb) {
            ret = FAILURE;
            log_error(NULL, "no thumbnail exits\n");
        } else {
            sequence_info->jpeg_input_info.dec_image_type = JPEGDEC_IMAGE;
        }
    }

    // if thumbnail and pic both need to decode, 
    // and thumbnail has decoded, next we will decode pic
    if (dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thumb_done) {
        sequence_info->jpeg_input_info.dec_image_type = JPEGDEC_IMAGE;
        dec_ctx->thumb_out = 0;
        dec_ctx->need_out_buf_for_thumb = 0;
    }

    return ret;
}

static int esjdec_decode_and_get_next_picture(JDECContext *dec_ctx,
                                              InputBuffer *input_buffer) {
    int *peek_frame;
    int ret = SUCCESS;
    enum DecRet rv;
    struct DecSequenceInfo *sequence_info = &dec_ctx->sequence_info;

    // set input info para according to input buffer
    esjdec_init_input_paras(input_buffer, &sequence_info->jpeg_input_info);

    do {
        ret = esjdec_get_image_info((void *)dec_ctx->dec_inst, sequence_info, &dec_ctx->jdec_config);
        if(ret < 0) {
            log_error(dec_ctx, "esjdec_get_image_info failed\n");
            return FAILURE;
        }

        ret = esjdec_set_image_type(dec_ctx, sequence_info);
        if (ret < 0) {
            log_error(dec_ctx, "esjdec_set_image_type failed\n");
            return FAILURE;
        }

        ret = esjdec_modify_config_by_image_info(dec_ctx, sequence_info);
        if (ret < 0) {
            log_error(dec_ctx, "esjdec_modify_config_by_image_info failed\n");
            return FAILURE;
        }

        // init pkt dump handle
        esjdec_init_pkt_dump_handle(dec_ctx, sequence_info);

        esjdec_modify_thum_config_by_image_info(sequence_info, &dec_ctx->jdec_config, dec_ctx->thumb_out);

        rv = esjdec_set_jpeg_info((void* )dec_ctx->dec_inst, dec_ctx->jdec_config);
         if(rv != DEC_OK) {
            log_error(NULL, "esjdec_set_jpeg_info failed, rv: %d\n", rv);
            return FAILURE;
        }

        if (dec_ctx->need_out_buf_for_thumb) {
            ret = esjdec_allocate_thumbnail_memory(&dec_ctx->thumb_mem,
                                                   dec_ctx->dec_inst,
                                                   dec_ctx->dwl_ref,
                                                   &dec_ctx->jdec_config);
            if (ret == FAILURE) {
                log_error(dec_ctx, "esjdec_allocate_thumbnail_memory failed\n");
                return FAILURE;
            }
            dec_ctx->need_out_buf_for_thumb = 0;
        }

        ret = esjdec_decode(dec_ctx, input_buffer, sequence_info, &peek_frame);
        if (ret == AVERROR_EXIT) {
            break;
        } else if (peek_frame) {
            ret = esjdec_get_next_picture(dec_ctx);
            if (ret == FAILURE) {
                log_error(dec_ctx,
                        "esjdec_get_next_picture failed, pic_decode_number: %d\n",
                        dec_ctx->pic_decode_number);
                break;
            }
        }

        if(dec_ctx->thumb_mode == Decode_Pic_Thumb && dec_ctx->thumb_out) {
            dec_ctx->thumb_out = 0;
            dec_ctx->thumb_done = 1;
        } else {
            break;
        }
    }while(1);

    return ret;
}

static void *esjdec_decode_thread_run(void *ctx) {
    int ret = FAILURE;
    int end_stream = FALSE;
    int peek_frame;
    int abort_request = FALSE;
    InputBuffer buffer;
    ESInputPort *input_port;
    JDECContext *dec_ctx = (JDECContext *)ctx;
    if (!dec_ctx || !dec_ctx->input_port) {
        log_error(dec_ctx, "error !!! dec_ctx or input_port is null\n");
        return NULL;
    }

    input_port = dec_ctx->input_port;

    while (!abort_request) {
        ret = esdec_get_input_packet_buffer(input_port->packet_queue, &buffer);
        if (ret == AVERROR_EXIT) {
            log_info(dec_ctx, "decode thread will be exit\n");
            abort_request = TRUE;
            continue;
        } else if (ret < 0) {
            continue;
        } else {
            dec_ctx->got_inputbuf_number++;
            log_info(dec_ctx, "got_inputbuf_number: %d\n", dec_ctx->got_inputbuf_number);
        }

        ret = esdec_wait_picture_consumed_until_timeout(dec_ctx->codec,
                                                        dec_ctx->dec_inst,
                                                        dec_ctx->output_port,
                                                        0 /*without waiting*/);
        if (ret == AVERROR_EXIT) {
            abort_request = TRUE;
            continue;
        }

        if (buffer.size <= 0) {
            esjdec_eos_process(dec_ctx);
            end_stream = TRUE;
            continue;
        }

        ret = esjdec_decode_and_get_next_picture(dec_ctx, &buffer);
        if (ret == AVERROR_EXIT) {
            abort_request = TRUE;
            continue;
        }

        //  dump packet
        ret = esjdec_pkt_dump(dec_ctx, &buffer);

        log_info(dec_ctx, "release input buffer\n");
        esdec_release_input_buffer(input_port->release_queue, &buffer);
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
        JpegDecEndOfStream(dec_ctx->dec_inst);
    }

    if (dec_ctx->picture) {
        av_freep(&dec_ctx->picture);
    }

    if (dec_ctx->frame) {
        av_frame_free(&dec_ctx->frame);
    }
    av_buffer_unref(&dec_ctx->hwframe);
    av_buffer_unref(&dec_ctx->hwdevice);

    return NULL;
}

int ff_jdec_decode_start(JDECContext *dec_ctx)
{
    if (!dec_ctx) {
        log_error(dec_ctx, "error !!! dec_ctx is null\n");
        return FAILURE;
    }

    if (pthread_create(&dec_ctx->tid, NULL, esjdec_decode_thread_run, dec_ctx)) {
        log_info(dec_ctx, "esdec_decode_thread_run create failed\n");
        return FAILURE;
    }

    log_info(dec_ctx, "esdec_decode_thread_run create success\n");

    return SUCCESS;
}

static int esjdec_init_jpeg_config(JDECContext *dec_ctx, struct JpegDecConfig *dec_cfg)
{
    if (!dec_ctx || !dec_cfg) {
        log_error(dec_ctx, "esjdec_init_jpeg_config input paras invalid\n");
        return FAILURE;
    }

    struct DecConfig *config = &dec_ctx->jdec_config;

    dec_ctx->dwl_inst = (void *)DWLInit(&dec_ctx->dwl_init);
    if (dec_ctx->dwl_inst == NULL) {
        log_error(dec_ctx, "DWL Init failed\n");
        return FAILURE;
    } else {
        log_debug(dec_ctx, "DWL Init success\n");
    }

    dec_ctx->dwl_ref = av_buffer_create(
        (uint8_t *)dec_ctx->dwl_inst, sizeof(dec_ctx->dwl_inst), esdec_dwl_release, NULL, AV_BUFFER_FLAG_READONLY);

    if ((dec_ctx->decode_mode & DEC_LOW_LATENCY) != 0) {
        dec_ctx->low_latency = 1;
    }

    dec_cfg->decoder_mode = DEC_NORMAL;
    if (dec_ctx->low_latency)
        dec_cfg->decoder_mode = DEC_LOW_LATENCY;

    dec_cfg->align = config->align;
    dec_cfg->mcinit_cfg.mc_enable = 0;
    dec_cfg->mcinit_cfg.stream_consumed_callback = NULL;
    memcpy(dec_cfg->ppu_config, config->ppu_cfg, sizeof(config->ppu_cfg));
    memcpy(dec_cfg->delogo_params, config->delogo_params, sizeof(config->delogo_params));

    return SUCCESS;
}

int ff_jdec_jpegdec_init(JDECContext *dec_ctx)
{
    struct JpegDecConfig dec_cfg;
    ESInputPort *input_port;
    int ret = SUCCESS;

    ret = esjdec_init_jpeg_config(dec_ctx, &dec_cfg);
    if (ret) {
        log_error(dec_ctx, "init jpeg config failed\n");
        return FAILURE;
    }

    input_port = esdec_allocate_input_port(dec_ctx->codec, dec_ctx->dwl_ref, &dec_ctx->dwl_init, 1);
    if (input_port) {
        dec_ctx->input_port = input_port;
    } else {
        log_info(dec_ctx, "es_decode_allocate_input_port failed\n");
        return FAILURE;
    }

    ret = JpegDecInit(&dec_ctx->dec_inst, dec_ctx->dwl_inst, &dec_cfg);

    if(ret) {
        log_error(dec_ctx, "JpegDecInit failed, ret: %d\n", ret);
        return FAILURE;
    } else {
        log_info(dec_ctx, "JpegDecInit success\n");
    }

    return ret;
}

static enum AVPixelFormat esjdec_get_format(JDECContext *dec_ctx) {
    enum AVPixelFormat format = AV_PIX_FMT_NV12;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return AV_PIX_FMT_NONE;
    }

    for (int i = 0; i < DEC_MAX_PPU_COUNT; i++) {
        if (dec_ctx->cfg_pp_enabled[i] && dec_ctx->pp_out == i) {
            format = dec_ctx->output_format[i];
            log_debug(dec_ctx, "target pp: %d, format: %d\n", i, format);
            break;
        }
    }

    return format;
}

int ff_jdec_decode_close(JDECContext *dec_ctx) {
    int ret = SUCCESS;
    if (!dec_ctx) {
        log_error(dec_ctx, "dec_ctx is null\n");
        return FAILURE;
    }

    esdec_input_port_stop(dec_ctx->input_port);
    esdec_output_port_stop(dec_ctx->output_port);

    pthread_join(dec_ctx->tid, NULL);

    ret = esjdec_release_thumbnail_memory(&dec_ctx->thumb_mem, dec_ctx->dwl_ref);
    if (ret == FAILURE) {
        log_info(dec_ctx, "release thumbnail memory failed\n");
    }

    if (dec_ctx->dec_inst) {
        JpegDecRelease(dec_ctx->dec_inst);
        dec_ctx->dec_inst = NULL;
        log_info(NULL, "dec_inst release success\n");
    }

    esdec_input_port_unref(&dec_ctx->input_port);
    esdec_output_port_unref(&dec_ctx->output_port);

    av_buffer_unref(&dec_ctx->dwl_ref);

    log_info(dec_ctx, "es_decode_close success\n");

    return SUCCESS;
}

int ff_jdec_init_hwctx(AVCodecContext *avctx)
{
    int ret = 0;
    AVHWFramesContext *hw_frames_ctx;
    JDECContext *dec_ctx = avctx->priv_data;

    avctx->sw_pix_fmt = esjdec_get_format(dec_ctx);
    enum AVPixelFormat pix_fmts[3] = {AV_PIX_FMT_ES, avctx->sw_pix_fmt, AV_PIX_FMT_NONE};
    avctx->pix_fmt = ff_get_format(avctx, pix_fmts);
    log_info(dec_ctx,
           "avctx sw_pix_fmt: %s, pix_fmt: %s\n",
            av_get_pix_fmt_name(avctx->sw_pix_fmt),
            av_get_pix_fmt_name(avctx->pix_fmt));

    if (avctx->hw_frames_ctx) {
        dec_ctx->hwframe = av_buffer_ref(avctx->hw_frames_ctx);
        if (!dec_ctx->hwframe) {
            log_error(dec_ctx, "init hwframe failed\n");
            return AVERROR(ENOMEM);
        }
    } else {
        if (avctx->hw_device_ctx) {
            log_info(avctx, "avctx->hw_device_ctx = %p\n", avctx->hw_device_ctx);
            dec_ctx->hwdevice = av_buffer_ref(avctx->hw_device_ctx);
            if (!dec_ctx->hwdevice) {
                log_error(dec_ctx, "init hwdevice failed\n");
                return AVERROR(ENOMEM);
            }
            log_info(avctx, "dec_ctx->hwdevice = %p\n", dec_ctx->hwdevice);
        } else {
            ret = av_hwdevice_ctx_create(&dec_ctx->hwdevice,
                                         AV_HWDEVICE_TYPE_ES,
                                         "es",
                                         NULL,
                                         0);
            if (ret < 0) {
                log_error(dec_ctx, "av_hwdevice_ctx_create failed\n");
                return FAILURE;
            }
        }

        dec_ctx->hwframe = av_hwframe_ctx_alloc(dec_ctx->hwdevice);
        if (!dec_ctx->hwframe) {
            log_error(dec_ctx, "av_hwframe_ctx_alloc failed\n");
            return AVERROR(ENOMEM);
        }

        hw_frames_ctx = (AVHWFramesContext *)dec_ctx->hwframe->data;
        hw_frames_ctx->format = AV_PIX_FMT_ES;
        hw_frames_ctx->sw_format = avctx->sw_pix_fmt;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_ES) {
        dec_ctx->extra_hw_frames = avctx->extra_hw_frames > 1 ? avctx->extra_hw_frames : 1;
    }

    log_info(avctx,
            "dec_ctx extra_hw_frames: %d, avctx extra_hw_frames: %d\n",
            dec_ctx->extra_hw_frames,
            avctx->extra_hw_frames);

    return SUCCESS;
}