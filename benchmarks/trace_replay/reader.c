
/**
 *  a reader for reading requests from trace
 */

#include "reader.h"
#include "bench_storage.h"

#include <cc_array.h>
#include <cc_debug.h>
#include <cc_define.h>
#include <cc_log.h>
#include <cc_mm.h>
#include <time/cc_timer.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sysexits.h>


//static const char *const key_array = "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz_"
//                                     "1234567890abcdefghijklmnopqrstuvwxyz";

static char val_array[MAX_VAL_LEN] = {'A'};


/**
 * default ttl is an array of 100 elements, if single ttl then the array is
 * repeat of single element, if multiple TTLs with different weight, it is
 * reflected in the array
 *
 * @param trace_path
 * @param default_ttls
 * @return
 */
struct reader *
open_trace(const char *trace_path, const int32_t * default_ttls)
{
    int fd;
    struct stat st;
    struct reader *reader = cc_zalloc(sizeof(struct reader));

    /* init reader module */
    for (int i=0; i<MAX_VAL_LEN; i++)
        val_array[i] = (char)('A' + i % 26);

    reader->reader_id = 0;
    reader->default_ttls = default_ttls;
    reader->default_ttl_idx = 0;
    strcpy(reader->trace_path, trace_path);

    /* get trace file info */
    if ((fd = open(trace_path, O_RDONLY)) < 0) {
        log_stderr("Unable to open '%s', %s\n", trace_path, strerror(errno));
        exit(EX_CONFIG);
    }

    if ((fstat(fd, &st)) < 0) {
        close(fd);
        log_stderr("Unable to fstat '%s', %s\n", trace_path, strerror(errno));
        exit(EX_CONFIG);
    }
    reader->file_size = st.st_size;

    /* set up mmap region */
    reader->mmap = (char *)mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

    if ((reader->mmap) == MAP_FAILED) {
        close(fd);
        log_stderr("Unable to allocate %zu bytes of memory, %s\n", st.st_size,
                strerror(errno));
        exit(EX_CONFIG);
    }

#ifdef __linux__
    /* USE_HUGEPAGE */
    madvise(reader->mmap, st.st_size, MADV_HUGEPAGE | MADV_SEQUENTIAL);
#endif

    uint32_t ts = *(uint32_t *) (reader->mmap);
    reader->start_ts = ts;

    /* size of one request, hard-coded for the trace type */
    size_t item_size = 20;

    if (reader->file_size % item_size != 0) {
        log_warn("trace file size %zu is not multiple of item size %zu\n",
                reader->file_size, item_size);
    }

    reader->n_total_req = reader->file_size / item_size;
    reader->e =
            (struct benchmark_entry *)cc_zalloc(sizeof(struct benchmark_entry));
    reader->e->val = val_array;
    memset(reader->e->key, 0, MAX_KEY_LEN);

    reader->update_time = true;

    close(fd);
    return reader;
}


/*
 * read one request from trace and store in benchmark_entry
 * this func is thread-safe
 *
 * current trace format:
 * 20 byte for each request,
 * first 4-byte is time stamp
 * next 8-byte is key encoded using increasing integer sequence
 * next 4-byte is key and val size,
 *      the left 10-bit is key size, right 22-bit is val size
 * next 4-byte is op and ttl,
 *      the left 8-bit is op and right 24-bit is ttl
 *      op is the index (start from 1) in the following array:
 *      get, gets, set, add,
 *      cas, replace, append, prepend, delete, incr, decr
 *
 * return 1 on trace EOF, otherwise 0
 *
 */
int
read_trace(struct reader *reader)
{
    size_t offset = __atomic_fetch_add(&reader->offset, 20, __ATOMIC_RELAXED);
    if (offset >= reader->file_size) {
        return 1;
    }

    char *mmap = reader->mmap + offset;
    uint32_t ts = *(uint32_t *)mmap - reader->start_ts + 1;
    reader->curr_ts = ts;
    if (reader->update_time) {
        __atomic_store_n(&proc_sec, reader->curr_ts, __ATOMIC_RELAXED);
    }
    mmap += 4;

    uint64_t key = *(uint64_t *)mmap;
    mmap += 8;
    uint32_t kv_len = *(uint32_t *)mmap;
    mmap += 4;
    uint32_t op_ttl = *(uint32_t *)mmap;

    uint32_t key_len = (kv_len >> 22) & (0x00000400 - 1);
    uint32_t val_len = kv_len & (0x00400000 - 1);

    if (key_len == 0) {
        printf("trace contains request of key size 0, object id %" PRIu64 "\n",
                key);
        return read_trace(reader);
    }

    if (key_len < 8) {
        key_len = 8;
    }

    uint32_t op = (op_ttl >> 24u) & (0x00000100 - 1);
    uint32_t ttl = op_ttl & (0x01000000 - 1);
//    if (ttl == 0) {
        ttl = reader->default_ttls[reader->default_ttl_idx];
        reader->default_ttl_idx = (reader->default_ttl_idx + 1) % 100;
//    }

    ASSERT(ttl != 0);

    if (op <= 0 || op >= 12) {
        printf("unknown op %d\n", op);
        op = 1;
    }

    *(uint64_t *) (reader->e->key) = key + reader->reader_id * 10000000000;

//    /* it is possible we have overflow here, but it should be rare */
//    snprintf(reader->e->key, key_len, "%.*lu", key_len-1, (unsigned long)key);

    reader->e->key_len = key_len;
    reader->e->val_len = val_len;
    reader->e->op = op - 1;
    reader->e->ttl = ttl;
    reader->e->expire_at = ts + ttl;

    return 0;
}


void
close_trace(struct reader *reader)
{
    munmap(reader->mmap, reader->file_size);

    cc_free(reader->e);
    cc_free(reader);
}
