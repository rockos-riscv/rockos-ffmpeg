#ifndef AVUTIL_ESMPP_UTILS_H
#define AVUTIL_ESMPP_UTILS_H
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* When type of function is void and expr is false, return. */
#define ES_RETURN_IF_FAIL(expr)                                                                                   \
    do {                                                                                                          \
        if (!(expr)) {                                                                                            \
            av_log(NULL, AV_LOG_ERROR, "Func:%s, Line:%d, expr \"%s\" failed.\n", __FUNCTION__, __LINE__, #expr); \
            return;                                                                                               \
        }                                                                                                         \
    } while (0)

/* When expr is false, return error info. */
#define ES_RETURN_VAL_IF_FAIL(expr, ret)                                                                          \
    do {                                                                                                          \
        if (!(expr)) {                                                                                            \
            av_log(NULL, AV_LOG_ERROR, "Func:%s, Line:%d, expr \"%s\" failed.\n", __FUNCTION__, __LINE__, #expr); \
            return ret;                                                                                           \
        }                                                                                                         \
    } while (0)

/* When func return is false, return ret. */
#define ES_RETURN_FUNC_FAIL(func)                                       \
    do {                                                                \
        ret = func;                                                     \
        if (ret) {                                                      \
            av_log(NULL,                                                \
                   AV_LOG_ERROR,                                        \
                   "Func:%s, Line:%d, call %s failed, ret = %d(%#x)\n", \
                   __FUNCTION__,                                        \
                   __LINE__,                                            \
                   #func,                                               \
                   ret,                                                 \
                   ret);                                                \
            return ret;                                                 \
        }                                                               \
    } while (0)

int esmpp_hwframe_transfer(AVFrame *dst_frame, const AVFrame *src_frame);
int esmpp_hwframe_transfer_bysize(AVFrame *dst_frame, const AVFrame *src_frame, size_t size);

#ifdef __cplusplus
}
#endif

#endif