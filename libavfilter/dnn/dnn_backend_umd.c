/*
 * Copyright (c) 2018 Sergey Lavrushkin
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

/**
 * @file
 * DNN eswin umd backend implementation.
 */

#include "dnn_backend_umd.h"
#include "libavformat/avio.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/cpu.h"
#include "libavutil/opt.h"
#include "libavutil/file.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/hwcontext.h"
// #include "libavcodec/defs.h"
#include "../internal.h"
#include "dnn_backend_common.h"
#include "safe_queue.h"
#include "dnn_io_proc.h"
#include "../esvfcommon.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "umd_wrapper.h"
#ifdef __cplusplus
}
#endif

#define ES_2D_ENABLE 1

#define CHECK_PARAMS_INVALID(para)                                                                            \
    if (!para) {                                                                                              \
        av_log(NULL, AV_LOG_ERROR, "%s[%d]:" #para " invalid , Error: NULL params.", __FUNCTION__, __LINE__); \
        return AVERROR(EINVAL);                                                                               \
    }

typedef struct UMDOptions {
    uint8_t async;
    uint32_t nireq;
    char *test_input;
    char *test_output;
} UMDOptions;

typedef struct UMDContext {
    const AVClass *class;
    UMDOptions options;
} UMDContext;

typedef struct TestData {
    uint8_t *bufptr;
    size_t size;
} TestData;

typedef struct UMDModel {
    UMDContext ctx;
    DNNModel *model;
    UMDWarpper *umdWrapper;
    TestData input_data;
    TestData output_data;
    Queue *lltask_queue;
    Queue *task_queue;
    UMDDescription input_desc;
    UMDDescription output_desc;
} UMDModel;

#define OFFSET(x) offsetof(UMDContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM
static const AVOption dnn_umd_options[] = {
    {"test_input", "umd input test", OFFSET(options.test_input), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    {"test_output", "umd test_output", OFFSET(options.test_output), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    DNN_BACKEND_COMMON_OPTIONS{NULL}};
AVFILTER_DEFINE_CLASS(dnn_umd);

static int read_file_bin(char *fname, TestData *test_data) {
    AVIOContext *model_file_context;
    uint8_t *data = NULL;
    int file_size;
    long bytes_read;
    if (avio_open(&model_file_context, fname, AVIO_FLAG_READ) < 0) {
        av_log(NULL, AV_LOG_ERROR, "read io faild \n ");
        return -1;
    }
    file_size = avio_size(model_file_context);
    av_log(NULL, AV_LOG_INFO, "  file_size:%d \n", file_size);
    data = av_malloc(file_size);
    if (!data) {
        avio_closep(&model_file_context);
        av_log(NULL, AV_LOG_ERROR, "read io faild \n ");
        return -1;
    }
    bytes_read = avio_read(model_file_context, data, file_size);
    avio_closep(&model_file_context);
    if (bytes_read != file_size) {
        av_freep(&data);
        av_log(NULL, AV_LOG_ERROR, "read io faild \n ");
        return -1;
    }
    for (size_t i = 0; i < 20; i++) {
        av_log(NULL, AV_LOG_INFO, "%02x ", data[i]);
    }
    av_log(NULL, AV_LOG_INFO, " \n ");
    test_data->bufptr = data;
    test_data->size = bytes_read;
    return 0;
}

static int read_file_txt(char *fname, TestData *test_data) {
    return 0;
}

static int read_file_data(char *file, TestData *test_data) {
    int ret = 0;
    if (av_stristr(file, "bin") != NULL) {
        av_log(NULL, AV_LOG_INFO, "file:%s it is bin file \n", file);
        ret = read_file_bin(file, test_data);
    } else if (av_stristr(file, "txt") != NULL) {
        av_log(NULL, AV_LOG_INFO, "file:%s it is text file \n", file);
        ret = read_file_txt(file, test_data);
    }
    return ret;
}

static int umd_got_mem_fd(const AVFrame *frame, int32_t *mem_fd) {
    AVFrameSideData *sd = NULL;
    if (!frame) return -1;
    if (!frame->nb_side_data) return -1;

    // mem fd
    sd = av_frame_get_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD);
    if (sd) {
        *mem_fd = *((int32_t *)sd->data);
        av_log(NULL, AV_LOG_INFO, "got mem_fd: %x\n", *mem_fd);
        return 0;
    }

    return -1;
}

static int umd_send_mem_fd(AVFrame *frame, int32_t mem_fd) {
    int ret = SUCCESS;
    AVFrameSideData *sd = NULL;

    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "frame is null\n");
        return FAILURE;
    }

    av_log(NULL, AV_LOG_DEBUG, "umd_send_mem_fd: fd = %x\n", mem_fd);
    sd = av_frame_new_side_data(frame, SIDE_DATA_TYPE_MEM_FRAME_FD_RELEASE, sizeof(mem_fd));
    if (sd && sd->data) {
        memcpy(sd->data, &mem_fd, sizeof(mem_fd));
    } else {
        ret = FAILURE;
        av_log(NULL, AV_LOG_ERROR, "av_frame_new_side_data faild sd: %p\n", sd);
    }

    return ret;
}

static int calculate_frame_data_size(enum AVPixelFormat in_fmt, AVFrame *in) {
    const AVPixFmtDescriptor *desc;
    int height = 0;
    int total_size = 0;

    desc = av_pix_fmt_desc_get(in_fmt);
    if (!desc) {
        av_log(NULL,
               AV_LOG_ERROR,
               "convert_get_frame_data_size get fmt: %s AVPixFmtDescriptor failed.\n",
               av_get_pix_fmt_name(in_fmt));
        return FAILURE;
    }

    for (int i = 0; i < FF_ARRAY_ELEMS(in->data) && in->data[i]; i++) {
        height = in->height;
        if (i == 1 || i == 2) {
            height = AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
        }
        total_size += in->linesize[i] * height;
    }

    return total_size;
}

static int execute_model_umd(Queue *lltask_queue) {
    UMDModel *umd_model = NULL;
    UMDContext *ctx = NULL;
    DNNData input, output;
    UMD_MEMORY input_memory;
    int32_t mem_fd;
    uint64_t input_buf_size;
    AVFrame *in_frame = NULL;
    AVBufferRef *hw_frames_ctx = NULL;
    AVHWFramesContext *in_hw_frames_ctx = NULL;

    uint8_t *output_data = NULL;
    UMD_MEMORY output_memory;
    UMDDescription *output_desc = NULL;
    UMDDescription *input_desc = NULL;

    LastLevelTaskItem *lltask = NULL;
    TaskItem *task = NULL;
    int ret = 0;

    UMD_MEMORY *input_umd_mem = NULL;

    lltask = ff_queue_pop_front(lltask_queue);
    if (!lltask) {
        av_log(NULL, AV_LOG_ERROR, "Failed to get LastLevelTaskItem\n");
        ret = AVERROR(EINVAL);
        goto err;
    }
    task = lltask->task;
    umd_model = task->model;
    ctx = &umd_model->ctx;
    in_frame = task->in_frame;

    output_desc = &umd_model->output_desc;
    input_desc = &umd_model->input_desc;

#ifdef ES_2D_ENABLE
    hw_frames_ctx = in_frame->hw_frames_ctx;
    in_hw_frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
    input_buf_size = calculate_frame_data_size(in_hw_frames_ctx->sw_format, in_frame);
    av_log(ctx,
           AV_LOG_INFO,
           "ES hwaccel mode. in_frame's fmt:%s, line_size:%d, height:%d, width:%d,bufsize:%ld\n",
           av_get_pix_fmt_name(in_hw_frames_ctx->sw_format),
           in_frame->linesize[0],
           in_frame->height,
           in_frame->width,
           input_buf_size);

    // check data is valid
    if (input_buf_size != input_desc->bufferSize) {
        av_log(NULL,
               AV_LOG_ERROR,
               "failed to compare data actual size:%ld, buffersize:%ld\n",
               input_buf_size,
               input_desc->bufferSize);
        ret = AVERROR(EINVAL);
        goto err;
    }

    input.height = input_desc->dims.h;
    input.width = input_desc->dims.w;
    input.channels = input_desc->dims.c;
    input.data = in_frame->data[0];
    input.dt = input_desc->dataType;

    ret = umd_got_mem_fd(in_frame, &mem_fd);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to  umd_got_mem_fd\n");
        goto err;
    }

    input_memory.fd = mem_fd;
    input_memory.buffer = in_frame->data[0];
    input_memory.size = input_buf_size;
    input_memory.bindId = 0;
    av_log(NULL,
           AV_LOG_INFO,
           "2d is enabled, data from 2d,input fd:%d, bufptr:%p, size:%ld \n",
           input_memory.fd,
           input_memory.buffer,
           input_memory.size);
#else
    av_log(NULL, AV_LOG_INFO, "2d is disabled, data from test_input\n");
    input.height = 321;
    input.width = 481;
    input.channels = 3;
    input.data = in_frame->data[0];
    input.dt = DNN_FLOAT;

    input_umd_mem = umd_wrapper_alloc_buffer(umd_model->umdWrapper, umd_model->input_data.size);
    if (!input_umd_mem) {
        ret = AVERROR(ENOSYS);
        av_log(NULL, AV_LOG_ERROR, "failed to cumd_wrapper_alloc_buffer\n");
        goto err;
    }
    input_memory.fd = input_umd_mem->fd;
    input_memory.buffer = input_umd_mem->buffer;
    input_memory.size = input_umd_mem->size;
    input_memory.bindId = 0;
    if (input_memory.size != umd_model->input_data.size) {
        ret = AVERROR(ENOSYS);
        av_log(NULL, AV_LOG_ERROR, "input buffer invalid\n");
        goto err;
    }
    memcpy(input_memory.buffer, umd_model->input_data.bufptr, input_memory.size);
#endif

    if (task->do_ioproc) {
        if (umd_model->model->frame_pre_proc != NULL) {
            av_log(NULL, AV_LOG_INFO, "frame_pre_proc is enabled\n");
            umd_model->model->frame_pre_proc(in_frame, &input, umd_model->model->filter_ctx);
        } else {
            av_log(NULL, AV_LOG_INFO, "frame_pre_proc is do nothing\n");
            // ff_proc_from_frame_to_dnn(in_frame, &input, ctx);
        }
    }

    if (task->nb_output != 1) {
        // currently, the filter does not need multiple outputs,
        // so we just pending the support until we really need it.
        avpriv_report_missing_feature(ctx, "multiple outputs");
        ret = AVERROR(ENOSYS);
        goto err;
    }

    ret = umd_wrapper_send_data(umd_model->umdWrapper, &input_memory);
    if (ret != 0) {
        ret = AVERROR(ENOSYS);
        av_log(NULL, AV_LOG_ERROR, "Failed to  umd_wrapper_send_data\n");
        goto err;
    }
    ret = umd_wrapper_get_result(umd_model->umdWrapper, &output_memory);
    if (ret != 0) {
        ret = AVERROR(ENOSYS);
        av_log(NULL, AV_LOG_ERROR, "Failed to  umd_wrapper_get_result\n");
        goto err;
    }

    av_log(ctx,
           AV_LOG_INFO,
           " output_memory buffer:%p, bindId:%d, fd:%d, size:%ld\n",
           output_memory.buffer,
           output_memory.bindId,
           output_memory.fd,
           output_memory.size);

    output_data = output_memory.buffer;
    for (size_t i = 0; i < 20; i++) {
        av_log(ctx, AV_LOG_INFO, "%02x ", output_data[i]);
    }
    av_log(ctx, AV_LOG_INFO, " \n ");

#define DUMP_UMD_OUTPUT 1
#ifdef DUMP_UMD_OUTPUT
    // dump output to file
    FILE *fp = fopen("output.raw", "wb+");
    fwrite(output_memory.buffer, output_memory.size, 1, fp);
    fflush(fp);
    fclose(fp);
#endif

#ifdef ES_2D_ENABLE
    output.height = output_desc->dims.h;
    output.width = output_desc->dims.w;
    output.channels = output_desc->dims.c;
    output.dt = output_desc->dataType;
#else
    output.height = 321;
    output.width = 481;
    output.channels = 3;
    output.dt = DNN_FLOAT;
#endif
    // now output data is equal input frame data:
    output.data = in_frame->data[0];
    // memcpy(output.data, input_memory.buffer, input_memory.size);

    if (task->do_ioproc) {
        if (umd_model->model->frame_post_proc != NULL) {
            av_log(NULL, AV_LOG_INFO, "frame_post_proc is enabled\n");
            umd_model->model->frame_post_proc(task->out_frame, &output, umd_model->model->filter_ctx);
        } else {
            av_log(NULL, AV_LOG_INFO, "output data is input data \n");
            // ff_proc_from_dnn_to_frame(task->out_frame, &output, ctx);
            // task->out_frame->data[0] = output.data;
            // task->out_frame->linesize[0] = input_memory.size;
            task->out_frame = av_frame_clone(task->in_frame);
        }
    } else {
        task->out_frame->width = output.width;
        task->out_frame->height = output.height;
    }
    task->inference_done++;
#ifdef ES_2D_ENABLE
    umd_send_mem_fd(task->out_frame, mem_fd);
#else
    umd_wrapper_free_buffer(umd_model->umdWrapper, input_umd_mem);
#endif
err:
    av_freep(&lltask);
    return ret;
}

static int extract_lltask_from_task(TaskItem *task, Queue *lltask_queue) {
    UMDModel *umd_model = task->model;
    UMDContext *ctx = &umd_model->ctx;
    LastLevelTaskItem *lltask = av_malloc(sizeof(*lltask));

    if (!lltask) {
        av_log(ctx, AV_LOG_ERROR, "Unable to allocate space for LastLevelTaskItem\n");
        return AVERROR(ENOMEM);
    }
    task->inference_todo = 1;
    task->inference_done = 0;
    lltask->task = task;

    if (ff_queue_push_back(lltask_queue, lltask) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to push back lltask_queue.\n");
        av_freep(&lltask);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int get_input_umd(void *model, DNNData *input, const char *input_name) {
    UMDModel *umd_model = model;
    UMDContext *ctx = &umd_model->ctx;
    UMDDescription *input_desc = &umd_model->input_desc;
    UMDDescription *output_desc = &umd_model->output_desc;
    // TF_Status *status;

    av_log(ctx, AV_LOG_INFO, " %s, %d, input_name:%s\n", __FUNCTION__, __LINE__, input_name);

#ifdef ES_2D_ENABLE
    umd_wrapper_get_input_description(umd_model->umdWrapper, input_desc);
    umd_wrapper_get_output_description(umd_model->umdWrapper, output_desc);
    // currently only NCHW is supported
    av_log(
        ctx,
        AV_LOG_INFO,
        "input_desc.name:%s, dims[n:%d,c:%d,h:%d,w:%d], dataType[%d], dataFormat[%d],bufferSize[%ld],pixelFormat[%d]\n",
        input_desc->name,
        input_desc->dims.n,
        input_desc->dims.c,
        input_desc->dims.h,
        input_desc->dims.w,
        input_desc->dataType,
        input_desc->dataFormat,
        input_desc->bufferSize,
        input_desc->pixelFormat);
    av_log(ctx,
           AV_LOG_INFO,
           "output_desc.name:%s, dims[n:%d,c:%d,h:%d,w:%d], dataType[%d], "
           "dataFormat[%d],bufferSize[%ld],pixelFormat[%d]\n",
           output_desc->name,
           output_desc->dims.n,
           output_desc->dims.c,
           output_desc->dims.h,
           output_desc->dims.w,
           output_desc->dataType,
           output_desc->dataFormat,
           output_desc->bufferSize,
           output_desc->pixelFormat);

    // #define TENSOR_DATA_TYPE_UNKNOWN 0U
    // #define TENSOR_DATA_TYPE_FLOAT   1U
    // #define TENSOR_DATA_TYPE_HALF    2U
    // #define TENSOR_DATA_TYPE_INT16   3U
    // #define TENSOR_DATA_TYPE_INT8    4U
    if (output_desc->dataType == 1U) {
        input->dt = DNN_FLOAT;
    } else if (output_desc->dataType == 4U) {
        input->dt = DNN_UINT8;
    } else {
        input->dt = DNN_FLOAT;
        av_log(ctx, AV_LOG_WARNING, "dataType:%d ffmpeg unsupport\n", output_desc->dataType);
    }
    input->dt = DNN_FLOAT;  // ffmpeg only support float
    input->order = DCO_RGB;
    input->height = input_desc->dims.h;
    input->width = input_desc->dims.w;
    input->channels = input_desc->dims.c;
#else
    int64_t dims[4];
    input->dt = DNN_FLOAT;
    input->order = DCO_RGB;
    // currently only NCHW is supported
    dims[0] = 1;
    dims[1] = 321;
    dims[2] = 481;
    dims[3] = 3;
    av_assert0(dims[0] == 1 || dims[0] == -1);
    input->height = dims[1];
    input->width = dims[2];
    input->channels = dims[3];
#endif

    return 0;
}

static int get_output_umd(void *model,
                          const char *input_name,
                          int input_width,
                          int input_height,
                          const char *output_name,
                          int *output_width,
                          int *output_height) {
    int ret = 0;
    UMDModel *umd_model = model;
    // UMDContext *ctx = &umd_model->ctx;
    // TaskItem task;
    // DNNExecBaseParams exec_params = {
    //     .input_name = input_name,
    //     .output_names = &output_name,
    //     .nb_output = 1,
    //     .in_frame = NULL,
    //     .out_frame = NULL,
    // };
    UMDDescription *output_desc = &umd_model->output_desc;
    av_log(NULL, AV_LOG_INFO, "input_name:%s output_name:%s\n", input_name, output_name);
    av_log(NULL, AV_LOG_INFO, "%s,input_width:%d input_height:%d\n", __FUNCTION__, input_width, input_height);
    // ret = ff_dnn_fill_gettingoutput_task(&task, &exec_params, umd_model, input_height, input_width, ctx);
    // if (ret != 0) {
    //     goto err;
    // }
    // ret = extract_lltask_from_task(&task, umd_model->lltask_queue);
    // if (ret != 0) {
    //     av_log(ctx, AV_LOG_ERROR, "unable to extract last level task from task.\n");
    //     goto err;
    // }

    // ret = execute_model_umd(umd_model->lltask_queue);
    // av_log(ctx,
    //        AV_LOG_INFO,
    //        " %s, %d w:%d, h:%d\n",
    //        __FUNCTION__,
    //        __LINE__,
    //        task.out_frame->width,
    //        task.out_frame->height);

    *output_width = input_width;
    *output_height = input_height;

    return ret;
}

DNNModel *ff_dnn_load_model_umd(const char *model_filename,
                                DNNFunctionType func_type,
                                const char *options,
                                AVFilterContext *filter_ctx) {
    DNNModel *model = NULL;
    UMDModel *umd_model = NULL;
    UMDContext *ctx = NULL;
    int ret = 0;

    if (!model_filename || !filter_ctx) {
        av_log(NULL, AV_LOG_ERROR, "params invalid\n");
        return NULL;
    }

    av_log(ctx, AV_LOG_INFO, "%s, %d IN\n", __FUNCTION__, __LINE__);

    model = av_mallocz(sizeof(DNNModel));
    if (!model) {
        return NULL;
    }

    umd_model = av_mallocz(sizeof(UMDModel));
    if (!umd_model) {
        av_freep(&model);
        return NULL;
    }
    umd_model->model = model;
    ctx = &umd_model->ctx;
    ctx->class = &dnn_umd_class;

    // parse options
    av_opt_set_defaults(ctx);
    if (av_opt_set_from_string(ctx, options, NULL, "=", "&") < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse options \"%s\"\n", options);
        goto err;
    }

#ifdef ES_2D_ENABLE
#else
    ret = read_file_data(ctx->options.test_input, &umd_model->input_data);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to read_file_data\n");
        goto err;
    }
    av_log(ctx, AV_LOG_INFO, "input[buf:%p, size:%ld]\n", umd_model->input_data.bufptr, umd_model->input_data.size);
#endif

    umd_model->umdWrapper = umd_wrapper_creat();
    if (!umd_model->umdWrapper) {
        av_log(ctx, AV_LOG_ERROR, "Failed to umd_wrapper_creat\n");
        return NULL;
    }

    if (umd_wrapper_load_model(umd_model->umdWrapper, model_filename) != 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to umd_wrapper_load_model\n");
        return NULL;
    }

    if (ctx->options.nireq <= 0) {
        ctx->options.nireq = av_cpu_count() / 2 + 1;
    }

#if !HAVE_PTHREAD_CANCEL
    if (ctx->options.async) {
        ctx->options.async = 0;
        av_log(filter_ctx, AV_LOG_WARNING, "pthread is not supported, roll back to sync.\n");
    }
#endif

    umd_model->lltask_queue = ff_queue_create();
    if (!umd_model->lltask_queue) {
        goto err;
    }

    umd_model->task_queue = ff_queue_create();
    if (!umd_model->task_queue) {
        goto err;
    }

    model->model = umd_model;
    model->get_input = &get_input_umd;
    model->get_output = &get_output_umd;
    model->options = options;
    model->filter_ctx = filter_ctx;
    model->func_type = func_type;

    return model;
err:
    ff_dnn_free_model_umd(&model);
    return NULL;
}

int ff_dnn_execute_model_umd(const DNNModel *model, DNNExecBaseParams *exec_params) {
    UMDModel *umd_model = model->model;
    UMDContext *ctx = &umd_model->ctx;
    TaskItem *task;
    int ret = 0;
    CHECK_PARAMS_INVALID(model)
    CHECK_PARAMS_INVALID(exec_params)
    av_log(NULL, AV_LOG_INFO, "ff_dnn_execute_model_umd \n");

    ret = ff_check_exec_params(ctx, DNN_UMD, model->func_type, exec_params);
    if (ret != 0) {
        return ret;
    }

    task = av_malloc(sizeof(*task));
    if (!task) {
        av_log(ctx, AV_LOG_ERROR, "unable to alloc memory for task item.\n");
        return AVERROR(ENOMEM);
    }

    ret = ff_dnn_fill_task(task, exec_params, umd_model, ctx->options.async, 1);
    if (ret != 0) {
        av_freep(&task);
        return ret;
    }

    if (ff_queue_push_back(umd_model->task_queue, task) < 0) {
        av_freep(&task);
        av_log(ctx, AV_LOG_ERROR, "unable to push back task_queue.\n");
        return AVERROR(ENOMEM);
    }

    ret = extract_lltask_from_task(task, umd_model->lltask_queue);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "unable to extract last level task from task.\n");
        return ret;
    }

    return execute_model_umd(umd_model->lltask_queue);
}

DNNAsyncStatusType ff_dnn_get_result_umd(const DNNModel *model, AVFrame **in, AVFrame **out) {
    UMDModel *umd_model = model->model;
    av_log(NULL, AV_LOG_INFO, "ff_dnn_get_result_umd \n");
    return ff_dnn_get_result_common(umd_model->task_queue, in, out);
}

int ff_dnn_flush_umd(const DNNModel *model) {
    UMDModel *umd_model = model->model;
    CHECK_PARAMS_INVALID(model)
    av_log(NULL, AV_LOG_INFO, "ff_dnn_flush_umd \n");

    if (ff_queue_size(umd_model->lltask_queue) == 0) {
        // no pending task need to flush
        return 0;
    }

    // for now, use sync node with flush operation
    // Switch to async when it is supported
    return execute_model_umd(umd_model->lltask_queue);
}

void ff_dnn_free_model_umd(DNNModel **model) {
    UMDModel *umd_model;
    if (!(*model)) {
        av_log(NULL, AV_LOG_ERROR, "params invalid\n");
        return;
    }
    av_log(NULL, AV_LOG_INFO, "ff_dnn_free_model_umd \n");
    if (*model) {
        if ((*model)->model) {
            umd_model = (*model)->model;
            // free input buffer
            if (umd_model->input_data.bufptr) {
                av_freep(&umd_model->input_data.bufptr);
                umd_model->input_data.bufptr = NULL;
            }
            while (ff_queue_size(umd_model->lltask_queue) != 0) {
                LastLevelTaskItem *item = ff_queue_pop_front(umd_model->lltask_queue);
                av_freep(&item);
            }
            ff_queue_destroy(umd_model->lltask_queue);
            while (ff_queue_size(umd_model->task_queue) != 0) {
                TaskItem *item = ff_queue_pop_front(umd_model->task_queue);
                av_frame_free(&item->in_frame);
                av_frame_free(&item->out_frame);
                av_freep(&item);
            }

            umd_wrapper_destroy(umd_model->umdWrapper);
            ff_queue_destroy(umd_model->task_queue);
            av_freep(&umd_model);
        }
        av_freep(model);
    }
    av_log(NULL, AV_LOG_INFO, "ff_dnn_free_model_umd done\n");
}