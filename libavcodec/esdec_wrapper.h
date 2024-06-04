#ifndef AVCODEC_ESDEC_WRAPPER_H__
#define AVCODEC_ESDEC_WRAPPER_H__
#include <dwl.h>
typedef struct ESDecoderWrapper {
    enum DecCodec codec;
    void *inst;
} ESDecoderWrapper;

int ESDecIsSimulation(void);
int ESDecGetDmaBufFd(struct DWLLinearMem *mem);
int ESDecGetDmaFdSplit(void *dwl_inst, void *dec_inst, int dmabuf_fd, int *split_fds, int pp_count);

#endif