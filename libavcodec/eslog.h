
#ifndef AVCODEC_ES_LOG_H__
#define AVCODEC_ES_LOG_H__
#include <unistd.h>
#include <sys/syscall.h>
#include <libavutil/log.h>

#define es_gettid() syscall(__NR_gettid)
#ifndef LOG_TAG
#define LOG_TAG "escodec"
#endif

#define log_info(avcl, fmt, ...) \
    av_log(avcl, AV_LOG_INFO, "%s [tid:%ld][%s][%d]" fmt, LOG_TAG, es_gettid(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define log_error(avcl, fmt, ...)                 \
    av_log(avcl,                                  \
           AV_LOG_ERROR,                          \
           "%s [tid:%ld][%s][%d] error !!! " fmt, \
           LOG_TAG,                               \
           es_gettid(),                           \
           __FUNCTION__,                          \
           __LINE__,                              \
           ##__VA_ARGS__)
#define log_debug(avcl, fmt, ...) \
    av_log(avcl, AV_LOG_DEBUG, "%s [tid:%ld][%s][%d]" fmt, LOG_TAG, es_gettid(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define log_warn(avcl, fmt, ...) \
    av_log(                      \
        avcl, AV_LOG_WARNING, "%s [tid:%ld][%s][%d]" fmt, LOG_TAG, es_gettid(), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif