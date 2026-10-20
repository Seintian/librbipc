/**
 * @file comp_kernel_ipc.c
 * @brief Linux POSIX mqueue & AF_UNIX DGRAM helper implementations.
 */

#include "comp_common.h"

mqd_t mq_make(const char *name, size_t msgsz, long maxmsg) {
    long real_max = (maxmsg > 0 && maxmsg <= 10) ? maxmsg : 10;
    long real_sz  = (msgsz <= 8192) ? (long)msgsz : 8192;
    struct mq_attr a = {
        .mq_maxmsg = real_max,
        .mq_msgsize = real_sz,
        .mq_flags = 0,
        .mq_curmsgs = 0
    };
    mq_unlink(name);
    return mq_open(name, O_RDWR | O_CREAT | O_EXCL, 0660, &a);
}
