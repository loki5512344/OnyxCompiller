#ifndef CC_EMIT_H
#define CC_EMIT_H
#include "cc.h"
int onx_emit(const char *path,
             const cc_buf_t *text,
             const cc_buf_t *rodata,
             const cc_buf_t *data,
             uint64_t bss_size,
             uint64_t entry,
             bool ring1);
#endif
