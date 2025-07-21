#include "esmpp_transfer.h"
#include "pixdesc.h"
#include "mpp_buffer.h"

int esmpp_hwframe_transfer(AVFrame *dst_frame, const AVFrame *src_frame) {
    int ret = 0;

    if (!dst_frame || !src_frame || !src_frame->data[0] || !dst_frame->data[0]) {
        av_log(NULL, AV_LOG_ERROR, "Invalid frame pointers\n");
        return AVERROR(EINVAL);
    }

    if (dst_frame->format != src_frame->format) {
        av_log(NULL, AV_LOG_ERROR, "Format mismatch: dst=%d vs src=%d\n", dst_frame->format, src_frame->format);
        return AVERROR(ENOSYS);
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(src_frame->format);
    if (!desc) {
        av_log(NULL, AV_LOG_ERROR, "Unsupported pixel format: %d\n", src_frame->format);
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < desc->nb_components; i++) {
        if (!src_frame->data[i] || !dst_frame->data[i]) break;

        int v_shift = desc->comp[i].plane > 0 ? desc->log2_chroma_h : 0;
        size_t plane_height = src_frame->height >> v_shift;

        size_t plane_size = src_frame->linesize[i] * plane_height;
        // av_log(NULL,
        //        AV_LOG_WARNING,
        //        "mpp_buffer_memcpy i %d dst_frame lsize %d height:%d\n",
        //        i,
        //        dst_frame->linesize[i],
        //        dst_frame->height);
        // av_log(NULL,
        //        AV_LOG_WARNING,
        //        "mpp_buffer_memcpy i %d src_frame lsize %d\n, height:%d pheight:%ld\n",
        //        i,
        //        src_frame->linesize[i],
        //        src_frame->height,
        //        plane_height);
        ret = mpp_buffer_memcpy(dst_frame->data[i], src_frame->data[i], plane_size);

        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "mpp_buffer_memcpy failed on plane %d: %d\n", i, ret);
            goto error_cleanup;
        }

        dst_frame->linesize[i] = src_frame->linesize[i];
    }

    if ((ret = av_frame_copy_props(dst_frame, src_frame)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
        goto error_cleanup;
    }
    return 0;

error_cleanup:
    av_frame_unref(dst_frame);
    return ret;
}

int esmpp_hwframe_transfer_bysize(AVFrame *dst_frame, const AVFrame *src_frame, size_t size) {
    int ret = 0;

    if (!dst_frame || !src_frame || !src_frame->data[0] || !dst_frame->data[0]) {
        av_log(NULL, AV_LOG_ERROR, "Invalid frame pointers\n");
        return AVERROR(EINVAL);
    }

    if (dst_frame->format != src_frame->format) {
        av_log(NULL, AV_LOG_ERROR, "Format mismatch: dst=%d vs src=%d\n", dst_frame->format, src_frame->format);
        return AVERROR(ENOSYS);
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(src_frame->format);
    if (!desc) {
        av_log(NULL, AV_LOG_ERROR, "Unsupported pixel format: %d\n", src_frame->format);
        return AVERROR(EINVAL);
    }

    ret = mpp_buffer_memcpy(dst_frame->data[0], src_frame->data[0], size);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "mpp_buffer_memcpy failed %d\n", ret);
        goto error_cleanup;
    } else {
        av_log(NULL, AV_LOG_DEBUG, "mpp_buffer_memcpy size:%ld\n", size);
    }

    if ((ret = av_frame_copy_props(dst_frame, src_frame)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
        goto error_cleanup;
    }

    return 0;

error_cleanup:
    av_frame_unref(dst_frame);
    return ret;
}
