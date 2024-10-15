#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"
#include "adp_amr.h"
#include "adp_itut_gxx.h"
#include "adp_aac.h"
#include "codec_api.h"
#include "CodecTypes.h"
#include "get_bits.h"
#include "put_bits.h"
#include "mpeg4audio.h"
#include "mpeg4audio_copy_pce.h"
#include "codec_par.h"

#define ADTS_HEADER_SIZE 7
#define ADTS_MAX_FRAME_BYTES ((1 << 14) - 1)
#define ADTS_MAX_PCE_SIZE 320

#define DEFAULT_G722_BIT_RATE       64000
#define DEFAULT_G726_BIT_RATE       32000
#define MAX_BUFFER_SIZE             32768
#define MAX_STREAM_LEN              2048

typedef struct ESADECContext {
    const AVClass *class;
    int chan_id;
    int bit_rate;
    char *buffer;
    int buffer_size;
    uint8_t stream[MAX_STREAM_LEN * 2];
    uint32_t offset;
    AVChannelLayout downmix_layout;
} ESADECContext;

typedef struct ADTSContext {
    AVClass *class;
    int write_adts;
    int objecttype;
    int sample_rate_index;
    int channel_conf;
    int pce_size;
    int apetag;
    int id3v2tag;
    int mpeg_id;
    uint8_t pce_data[ADTS_MAX_PCE_SIZE];
} ADTSContext;

static es_codec_type convert_codec_id_to_type(enum AVCodecID codec_id)
{
    switch (codec_id) {
        case AV_CODEC_ID_AAC:
            return AAC;
        case AV_CODEC_ID_MP3:
        case AV_CODEC_ID_MP2:
            return MP3;
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_MULAW:
            return G711;
        case AV_CODEC_ID_ADPCM_G722:
            return G722;
        case AV_CODEC_ID_ADPCM_G726:
            return G726;
        case AV_CODEC_ID_AMR_NB:
        case AV_CODEC_ID_AMR_WB:
            return AMR;
        default:
            return UNKNOW;
    }
}

static aac_profile convert_aot_to_esprofile(int aot)
{
    switch(aot) {
        case FF_PROFILE_AAC_LOW:
            return AAC_TYPE_AACLC;
        case FF_PROFILE_AAC_MAIN:
            return AAC_TYPE_AACMAIN;
        case FF_PROFILE_AAC_HE:
        case FF_PROFILE_AAC_HE_V2:
            return AAC_TYPE_AACHE;
        case FF_PROFILE_AAC_LD:
            return  AAC_TYPE_AACLD;
        default:
            return AAC_TYPE_AACLC;
    }
}

static void *get_audio_decoder_attr(AVCodecContext *avctx, enum AVCodecID codec_id)
{
    void *attr = NULL;

    if (AV_CODEC_ID_AMR_NB == codec_id || AV_CODEC_ID_AMR_WB == codec_id) {
        audio_amr_decoder_attr *amr_attr = av_mallocz(sizeof(audio_amr_decoder_attr));
        if (amr_attr) {
            amr_attr->is_wb = (AV_CODEC_ID_AMR_WB == codec_id) ? 1 : 0;
            attr = amr_attr;
        }
    } else if (AV_CODEC_ID_PCM_ALAW == codec_id || AV_CODEC_ID_PCM_MULAW == codec_id) {
        audio_g711_attr *g711_attr = av_mallocz(sizeof(audio_g711_attr));
        if (g711_attr) {
            g711_attr->type = (AV_CODEC_ID_PCM_ALAW == codec_id) ? ALAW : ULAW;
            attr = g711_attr;
        }
    } else if (AV_CODEC_ID_ADPCM_G722 == codec_id) {
        audio_g722_attr *g722_attr = av_mallocz(sizeof(audio_g722_attr));
        if (g722_attr) {
            g722_attr->bit_rate = (avctx->bit_rate == 0) ? DEFAULT_G722_BIT_RATE : avctx->bit_rate;
            attr = g722_attr;
        }
    } else if (AV_CODEC_ID_ADPCM_G726 == codec_id) {
        audio_g726_attr *g726_attr = av_mallocz(sizeof(audio_g726_attr));
        if (g726_attr) {
            g726_attr->bit_rate = (avctx->bit_rate == 0) ? DEFAULT_G726_BIT_RATE : avctx->bit_rate;
            attr = g726_attr;
        }
    } else if (AV_CODEC_ID_AAC == codec_id) {
        audio_aacdecoder_attr *aac_attr = av_mallocz(sizeof(audio_aacdecoder_attr));
        if (aac_attr) {
            aac_attr->output_format = 1;
            aac_attr->profile = convert_aot_to_esprofile(avctx->profile);
            aac_attr->trans_type = AAC_TRANS_TYPE_ADTS;
            attr = aac_attr;
        }
    }
    return attr;
}

static av_cold int ff_es_adec_init(AVCodecContext *avctx)
{
    static int32_t chan = 1;
    void *attr = NULL;
    ESADECContext *s = NULL;
    int ret = 0;

    /* get codec type */
    enum AVCodecID codec_id = avctx->codec_id;
    es_codec_type codec_type = convert_codec_id_to_type(codec_id);
    if (codec_type == UNKNOW) {
        av_log(avctx, AV_LOG_ERROR, "UNKNOW codec_id:0x%x\n", codec_id);
        return AVERROR_DECODER_NOT_FOUND;
    }

    /* init audio dec */
    if(chan == 1) {
        es_adec_init();
    }

    /* get codec attr */
    attr = get_audio_decoder_attr(avctx, codec_id);
    s = (ESADECContext *)avctx->priv_data;

    ret = es_adec_create(chan, codec_type, attr);
    /* free attr, NULL is safe */
    av_free(attr);

    if(0 != ret) {
        av_log(avctx, AV_LOG_ERROR, "es_adec_create() failed:%d\n", ret);
        return AVERROR_UNKNOWN;
    }

    s->chan_id = chan;
    chan++;
    s->buffer_size = MAX_BUFFER_SIZE;
    s->buffer = av_malloc(s->buffer_size);
    s->offset = 0;

    return 0;
}

static av_cold int ff_es_adec_close(AVCodecContext *avctx)
{
    ESADECContext *s = avctx->priv_data;
    es_adec_destroy(s->chan_id);
    av_free(s->buffer);
    return 0;
}

static int get_stream_info(AVCodecContext *avctx, uint8_t *data, uint32_t size, es_frame_info *frame_info)
{
    ESADECContext *s = avctx->priv_data;
    int ret = es_adec_parse_packets(s->chan_id, data, size, frame_info);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "es_adec_parse_packets failed:%d\n", ret);
        return AVERROR_UNKNOWN;
    }
    // av_log(avctx, AV_LOG_INFO, "sample_rate=%d, frame_size=%d, channels=%d, bit_depth=%d, decoded_size=%d\n",
    //        frame_info->sample_rate, frame_info->frame_size, frame_info->channels,
    //        frame_info->bit_depth, frame_info->decoded_size);
    avctx->sample_rate = frame_info->sample_rate;
    avctx->frame_size  = frame_info->decoded_size / frame_info->channels / (frame_info->bit_depth / 8);

    av_channel_layout_uninit(&avctx->ch_layout);
    if (frame_info->channels == 1) {
        avctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    } else {
        avctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    }

    switch (frame_info->bit_depth) {
    case 8:
        avctx->sample_fmt = AV_SAMPLE_FMT_U8;
        break;
    case 16:
        avctx->sample_fmt = AV_SAMPLE_FMT_S16;
        break;
    case 32:
        avctx->sample_fmt = AV_SAMPLE_FMT_S32;
        break;
    default:
        avctx->sample_fmt = AV_SAMPLE_FMT_S16;
        break;
    }
    return 0;
}

static int adts_decode_extradata(AVCodecContext *s, ADTSContext *adts, const uint8_t *buf, int size)
{
    GetBitContext gb;
    PutBitContext pb;
    MPEG4AudioConfig m4ac;
    int off, ret;

    ret = init_get_bits8(&gb, buf, size);
    if (ret < 0)
        return ret;
    off = avpriv_mpeg4audio_get_config2(&m4ac, buf, size, 1, s);
    if (off < 0)
        return off;
    skip_bits_long(&gb, off);
    adts->objecttype        = m4ac.object_type - 1;
    adts->sample_rate_index = m4ac.sampling_index;
    adts->channel_conf      = m4ac.chan_config;

    if (adts->objecttype > 3U) {
        av_log(s, AV_LOG_ERROR, "MPEG-4 AOT %d is not allowed in ADTS\n", adts->objecttype+1);
        return AVERROR_INVALIDDATA;
    }
    if (adts->sample_rate_index == 15) {
        av_log(s, AV_LOG_ERROR, "Escape sample rate index illegal in ADTS\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "960/120 MDCT window is not allowed in ADTS\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "Scalable configurations are not allowed in ADTS\n");
        return AVERROR_INVALIDDATA;
    }
    if (get_bits(&gb, 1)) {
        av_log(s, AV_LOG_ERROR, "Extension flag is not allowed in ADTS\n");
        return AVERROR_INVALIDDATA;
    }
    if (!adts->channel_conf) {
        init_put_bits(&pb, adts->pce_data, MAX_PCE_SIZE);

        put_bits(&pb, 3, 5); //ID_PCE
        adts->pce_size = (ff_copy_pce_data(&pb, &gb) + 3) / 8;
        flush_put_bits(&pb);
    }

    adts->write_adts = 1;

    return 0;
}

static int adts_write_frame_header(AVCodecContext *s, ADTSContext *ctx,
                                   uint8_t *buf, int size, int pce_size)
{
    PutBitContext pb;

    unsigned full_frame_size = (unsigned)ADTS_HEADER_SIZE + size + pce_size;
    if (full_frame_size > ADTS_MAX_FRAME_BYTES) {
        av_log(s, AV_LOG_ERROR, "frame size too large: %u (max %d)\n",
               full_frame_size, ADTS_MAX_FRAME_BYTES);
        return AVERROR_INVALIDDATA;
    }

    init_put_bits(&pb, buf, ADTS_HEADER_SIZE);

    /* adts_fixed_header */
    put_bits(&pb, 12, 0xfff);   /* syncword */
    put_bits(&pb, 1, ctx->mpeg_id); /* ID */
    put_bits(&pb, 2, 0);        /* layer */
    put_bits(&pb, 1, 1);        /* protection_absent */
    put_bits(&pb, 2, ctx->objecttype); /* profile_objecttype */
    put_bits(&pb, 4, ctx->sample_rate_index);
    put_bits(&pb, 1, 0);        /* private_bit */
    put_bits(&pb, 3, ctx->channel_conf); /* channel_configuration */
    put_bits(&pb, 1, 0);        /* original_copy */
    put_bits(&pb, 1, 0);        /* home */

    /* adts_variable_header */
    put_bits(&pb, 1, 0);        /* copyright_identification_bit */
    put_bits(&pb, 1, 0);        /* copyright_identification_start */
    put_bits(&pb, 13, full_frame_size); /* aac_frame_length */
    put_bits(&pb, 11, 0x7ff);   /* adts_buffer_fullness */
    put_bits(&pb, 2, 0);        /* number_of_raw_data_blocks_in_frame */

    flush_put_bits(&pb);

    return 0;
}

static int ff_es_adec_frame(AVCodecContext *avctx, AVFrame *frame,
                                int *got_frame_ptr, AVPacket *avpkt)
{
    int ret;
    ESADECContext *s = avctx->priv_data;
    int32_t size = s->buffer_size;
    es_frame_info frame_info;
    uint8_t *pkt_data = NULL;
    uint32_t pkt_size = 0;

    AVCodecParameters *par = NULL;
    ADTSContext adts;
    par = avcodec_parameters_alloc();
    memset(&adts, 0 , sizeof(adts));

    if (AV_CODEC_ID_MP3 == avctx->codec_id) {
        memcpy(s->stream + s->offset, avpkt->data, avpkt->size);
        pkt_size = s->offset;
        pkt_data = s->stream;
        s->offset = avpkt->size;
    } else {
        pkt_data = avpkt->data;
        pkt_size = avpkt->size;
    }

    if(AV_CODEC_ID_AAC == avctx->codec_id && avctx->extradata && par) {
        avcodec_parameters_from_context(par, avctx);
        if (par->extradata_size > 0) {
            adts_decode_extradata(avctx, &adts, par->extradata, par->extradata_size);
        }

        if (adts.write_adts) {
            adts_write_frame_header(avctx, &adts, s->stream, ADTS_HEADER_SIZE, adts.pce_size);
            memcpy(s->stream + ADTS_HEADER_SIZE, avpkt->data, avpkt->size);
            pkt_data = s->stream;
            pkt_size = avpkt->size + ADTS_HEADER_SIZE;
        }
    }
    if(par)
        avcodec_parameters_free(&par);

    if (pkt_size == 0) {
        return avpkt->size;
    }

    if ((ret = get_stream_info(avctx, pkt_data, pkt_size, &frame_info)) < 0) {
        av_log(s, AV_LOG_ERROR, "get_stream_info failed:%d\n", ret);
        return ret;
    }

    ret = es_adec_decode_stream(s->chan_id, pkt_data, pkt_size, s->buffer, &size);
    if (0 != ret){
        av_log(s, AV_LOG_ERROR, "es_adec_decode_stream failed: %d\n", ret);
        return AVERROR_UNKNOWN;
    }

    if (AV_CODEC_ID_MP3 == avctx->codec_id) {
        memmove(s->stream, s->stream + pkt_size, avpkt->size);
    }

    if (size == 0) {
        return avpkt->size;
    }

    frame->nb_samples = size / frame_info.channels / (frame_info.bit_depth / 8);

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
        av_log(s, AV_LOG_ERROR, "cannot get buffer for decode, ret: %d\n", ret);
        return ret;
    }

    memcpy(frame->extended_data[0], s->buffer, size);

    *got_frame_ptr = 1;
    return avpkt->size;
}

static av_cold void ff_es_adec_flush(AVCodecContext *avctx)
{

}

#define ES_AUDIO_DEC(ctype, CTYPE)                                                                  \
    static const AVClass es_##ctype##_decoder_class = {                                             \
        .class_name = #ctype "_esadec",                                                             \
        .item_name = av_default_item_name,                                                          \
        .version = LIBAVUTIL_VERSION_INT,                                                           \
    };                                                                                              \
    const FFCodec ff_es_##ctype##_decoder = {                                                       \
        .p.name = "es_" #ctype,                                                                     \
        .p.long_name = NULL_IF_CONFIG_SMALL("Es " #ctype " decoder"),                               \
        .p.type = AVMEDIA_TYPE_AUDIO,                                                               \
        .p.id = AV_CODEC_ID_##CTYPE,                                                                \
        .priv_data_size = sizeof(ESADECContext),                                                    \
        .p.priv_class = &es_##ctype##_decoder_class,                                                \
        .init = ff_es_adec_init,                                                                    \
        .close = ff_es_adec_close,                                                                  \
        FF_CODEC_DECODE_CB(ff_es_adec_frame),                                                       \
        .flush = ff_es_adec_flush,                                                                  \
        .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,                             \
        .p.wrapper_name = "esadec",                                                                 \
    };

ES_AUDIO_DEC(aac, AAC)
ES_AUDIO_DEC(mp3, MP3)
ES_AUDIO_DEC(amrnb, AMR_NB)
ES_AUDIO_DEC(amrwb, AMR_WB)
ES_AUDIO_DEC(g711alaw, PCM_ALAW)
ES_AUDIO_DEC(g711mulaw, PCM_MULAW)
ES_AUDIO_DEC(g722, ADPCM_G722)
ES_AUDIO_DEC(g726, ADPCM_G726)

