#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "internal.h"
#include "adp_amr.h"
#include "adp_itut_gxx.h"
#include "adp_aac.h"
#include "codec_api.h"

#define AMR_NB_SAMPLES_PER_FRAME     160
#define AMR_WB_SAMPLES_PER_FRAME     320
#define GXX_SAMPLES_PER_FRAME        160
#define AAC_LC_SAMPLES_PER_FRAME     1024

#define MAX_PACKET_SIZE              16384

#define DEBUG_DUMP 0

typedef struct ESAENCContext {
    const AVClass *class;
    int chan_id;
    int bit_rate;
    int afterburner;
    int vbr;
    int eld_sbr;
} ESAENCContext;

static const AVOption aac_options[] = {
    { "afterburner", "Afterburner (improved quality)", offsetof(ESAENCContext, afterburner), AV_OPT_TYPE_INT,
      { .i64 = 1 }, 0, 1, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { "vbr", "VBR mode (1-5)", offsetof(ESAENCContext, vbr), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 5,
      AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { "eld_sbr", "Enable SBR for ELD (for SBR in other configurations, use the -profile parameter)",
      offsetof(ESAENCContext, eld_sbr), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1,
      AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVOption amrnb_options[] = { { NULL } };

static const AVOption amrwb_options[] = { { NULL } };

static const AVOption g711alaw_options[] = { { NULL } };

static const AVOption g711mulaw_options[] = { { NULL } };

static const AVOption g722_options[] = { { NULL } };

static const AVOption g726_options[] = { { NULL } };

static es_codec_type convert_codec_id_to_type(enum AVCodecID codec_id);
static void *get_audio_encoder_attr(AVCodecContext *avctx, enum AVCodecID codec_id);

static es_codec_type convert_codec_id_to_type(enum AVCodecID codec_id)
{
    switch (codec_id) {
        case AV_CODEC_ID_AAC:
            return AAC;
        case AV_CODEC_ID_MP3:
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

static void *get_audio_encoder_attr(AVCodecContext *avctx, enum AVCodecID codec_id)
{
    ESAENCContext *s = (ESAENCContext *)avctx->priv_data;
    void *attr = NULL;

    if (AV_CODEC_ID_AMR_NB == codec_id || AV_CODEC_ID_AMR_WB == codec_id) {
        audio_amr_encoder_attr *amr_attr = av_mallocz(sizeof(audio_amr_encoder_attr));
        if (amr_attr) {
            amr_attr->is_wb = (AV_CODEC_ID_AMR_WB == codec_id) ? 1 : 0;
            amr_attr->bit_rate = avctx->bit_rate;
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
            g722_attr->bit_rate = avctx->bit_rate;
            attr = g722_attr;
        }
    } else if (AV_CODEC_ID_ADPCM_G726 == codec_id) {
        audio_g726_attr *g726_attr = av_mallocz(sizeof(audio_g726_attr));
        if (g726_attr) {
            g726_attr->bit_rate = avctx->bit_rate;
            attr = g726_attr;
        }
    } else if (AV_CODEC_ID_AAC == codec_id) {
        audio_aacenc_attr *aac_attr = av_mallocz(sizeof(audio_aacenc_attr));
        if (aac_attr) {
            aac_attr->bit_rate = avctx->bit_rate;
            aac_attr->aot = avctx->profile + 1;
            aac_attr->sbr = s->eld_sbr;
            aac_attr->channels = avctx->ch_layout.nb_channels;
            aac_attr->vbr = s->vbr;
            aac_attr->sample_rate = avctx->sample_rate;
            aac_attr->afterburner = s->afterburner;
            attr = aac_attr;
        }
    }
    return attr;
}

static int set_frame_size(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
        case AV_CODEC_ID_AMR_WB:
            avctx->frame_size = AMR_WB_SAMPLES_PER_FRAME;
            break;
        case AV_CODEC_ID_AMR_NB:
            avctx->frame_size = AMR_NB_SAMPLES_PER_FRAME;
            break;
        case AV_CODEC_ID_AAC:{
            if (FF_PROFILE_AAC_LOW == avctx->profile) {
                avctx->frame_size = AAC_LC_SAMPLES_PER_FRAME;
            } else {
                av_log(avctx, AV_LOG_ERROR, "Not support AAC profile: %d\n", avctx->frame_size);
            }
            break;
        }
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_ADPCM_G722:
        case AV_CODEC_ID_ADPCM_G726:
            avctx->frame_size = GXX_SAMPLES_PER_FRAME;
            break;
        default:
            // Handle other codec IDs
            break;
    }
    return 0;
}

static av_cold int ff_es_aenc_init(AVCodecContext *avctx)
{
    static int32_t chan = 1;

    /* get codec type */
    enum AVCodecID codec_id = avctx->codec_id;
    es_codec_type codec_type = convert_codec_id_to_type(codec_id);
    if (codec_type == UNKNOW) {
        av_log(avctx, AV_LOG_ERROR, "UNKNOW codec_id:0x%x\n", codec_id);
        return AVERROR_ENCODER_NOT_FOUND;
    }

    /* init audio dec */
    if(chan == 1) {
        es_aenc_init();
    }

    if (AV_CODEC_ID_AAC == codec_id) {
        if (FF_PROFILE_AAC_LOW != avctx->profile) {
            av_log(avctx, AV_LOG_DEBUG, "Only support AAC-LC, use AAC_LOW auto\n");
            avctx->profile = FF_PROFILE_AAC_LOW;
        }
    }

    /* get codec attr */
    void *attr = get_audio_encoder_attr(avctx, codec_id);
    ESAENCContext *s = (ESAENCContext *)avctx->priv_data;

    int ret = es_aenc_create(chan, codec_type, attr);
    /* free attr, NULL is safe */
    av_free(attr);

    if(0 != ret) {
        av_log(avctx, AV_LOG_ERROR, "es_aenc_create() failed:%d\n", ret);
        return AVERROR_UNKNOWN;
    }

    s->chan_id = chan;
    chan++;

    set_frame_size(avctx);

    return 0;
}

static av_cold int ff_es_aenc_close(AVCodecContext *avctx)
{
    ESAENCContext *s = avctx->priv_data;
    es_aenc_destroy(s->chan_id);
    return 0;
}

#if DEBUG_DUMP
static int dump_data(const char *path, const void *buf, size_t bytes)
{
    if (!path) {
        return 0;
    }

    FILE *fp = fopen(path, "a+");
    if (fp) {
        fwrite((char *)buf, 1, bytes, fp);
        fclose(fp);
    }

    return 0;
}

static const char* get_codec_suffix(AVCodecContext *avctx)
{
    const char *suffix = "";

    switch (avctx->codec_id) {
        case AV_CODEC_ID_AAC:
            suffix = ".aac";
            break;
        case AV_CODEC_ID_MP3:
            suffix = ".mp3";
            break;
        case AV_CODEC_ID_AMR_NB:
        case AV_CODEC_ID_AMR_WB:
            suffix = ".amr";
            break;
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_PCM_MULAW:
            suffix = ".wav";
            break;
        case AV_CODEC_ID_ADPCM_G722:
            suffix = ".g722";
            break;
        case AV_CODEC_ID_ADPCM_G726:
            suffix = ".g726";
            break;
        default:
            break;
    }
    return suffix;
}
#endif

static int ff_es_aenc_frame(AVCodecContext *avctx, AVPacket *avpkt,
                               const AVFrame *frame, int *got_packet_ptr)
{
    int ret;
    int32_t bytes_per_sample;
    int32_t channels;
    ESAENCContext *s = avctx->priv_data;
    int32_t size = 0;

    if ((ret = ff_alloc_packet(avctx, avpkt, MAX_PACKET_SIZE)) < 0) {
        return ret;
    }

    bytes_per_sample = av_get_bytes_per_sample(frame->format);
    channels = frame->ch_layout.nb_channels;

    ret = es_aenc_encode_frame(s->chan_id, frame->data[0], frame->nb_samples * bytes_per_sample * channels, avpkt->data, &size);
    if (0 != ret){
        av_log(s, AV_LOG_ERROR, "es_aenc_encode_frame failed:%d\n", ret);
        return AVERROR_UNKNOWN;
    }
    // av_log(avctx, AV_LOG_INFO, "nb_samples:%d, bytes_per_sample:%d, channels:%d, linesize:%d, avctx->frame_size:%d, size=%d\n",
    //        frame->nb_samples, bytes_per_sample, channels, frame->linesize[0], avctx->frame_size, size);
#if DEBUG_DUMP
    dump_data("./dump_before_encode.raw", frame->data[0], frame->nb_samples * bytes_per_sample * channels);
    char output_filename[256];
    snprintf(output_filename, sizeof(output_filename), "dump_after_encode%s", get_codec_suffix(avctx));
    dump_data(output_filename, avpkt->data, size);
#endif

    avpkt->size = size;
    *got_packet_ptr = 1;
    return 0;
}

static const int amrnb_sample_rates[] = { 8000, 0 };

static const int amrwb_sample_rates[] = { 16000, 0 };

static const int g711alaw_sample_rates[] = { 8000, 0 };

static const int g711mulaw_sample_rates[] = { 8000, 0 };

static const int g722_sample_rates[] = { 16000, 0 };

static const int g726_sample_rates[] = { 8000, 0 };

static const int aac_sample_rates[] = { 48000, 44100, 32000, 24000, 22050, 16000, 8000, 0 };

static const AVChannelLayout amrnb_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout amrwb_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout g711alaw_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout g711mulaw_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout g722_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout g726_ch_layouts[2] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };

static const AVChannelLayout aac_ch_layouts[3] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    { 0 },
};

#define ES_AUDIO_ENC(ctype, CTYPE)                                                                  \
    static const AVClass es_##ctype##_encoder_class = {                                             \
        .class_name = #ctype "_esaenc",                                                             \
        .item_name = av_default_item_name,                                                          \
        .option = ctype##_options,                                                                  \
        .version = LIBAVUTIL_VERSION_INT,                                                           \
    };                                                                                              \
    const FFCodec ff_es_##ctype##_encoder = {                                                       \
        .p.name = "es_" #ctype,                                                                     \
        .p.long_name = NULL_IF_CONFIG_SMALL("Es " #ctype " encoder"),                               \
        .p.type = AVMEDIA_TYPE_AUDIO,                                                               \
        .p.id = AV_CODEC_ID_##CTYPE,                                                                \
        .priv_data_size = sizeof(ESAENCContext),                                                    \
        .p.priv_class = &es_##ctype##_encoder_class,                                                \
        .init = ff_es_aenc_init,                                                                    \
        .close = ff_es_aenc_close,                                                                  \
        FF_CODEC_ENCODE_CB(ff_es_aenc_frame),                                                       \
        .p.wrapper_name = "esaenc",                                                                 \
        .p.sample_fmts = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE },    \
        .p.ch_layouts = ctype##_ch_layouts,                                                         \
        .p.supported_samplerates = ctype##_sample_rates,                                            \
    };

ES_AUDIO_ENC(amrnb, AMR_NB)
ES_AUDIO_ENC(amrwb, AMR_WB)
ES_AUDIO_ENC(g711alaw, PCM_ALAW)
ES_AUDIO_ENC(g711mulaw, PCM_MULAW)
ES_AUDIO_ENC(g722, ADPCM_G722)
ES_AUDIO_ENC(g726, ADPCM_G726)
ES_AUDIO_ENC(aac, AAC)

