/* Synthetic GGUF consumer regression: no model download or GPU required. */
#ifndef DS4_NO_GPU
#define DS4_NO_GPU
#endif
#include <assert.h>
#include <sys/mman.h>

static void *test_model_mmap(void *, size_t, int, int, int, off_t);
#define mmap test_model_mmap
#include "../ds4.c"
#undef mmap
#ifdef _WIN32
#include <windows.h>
#endif

static void *first_mapping;
static uint64_t tail_start;

static void *test_model_mmap(void *addr, size_t size, int prot, int flags,
                             int fd, off_t offset) {
    void *result = mmap(addr, size, prot, flags, fd, offset);
    if (!first_mapping && result != MAP_FAILED) first_mapping = result;
    return result;
}


static void u32(FILE *f, uint32_t n) { assert(fwrite(&n, 4, 1, f) == 1); }
static void u64(FILE *f, uint64_t n) { assert(fwrite(&n, 8, 1, f) == 1); }
static void str(FILE *f, const char *s) {
    size_t len = strlen(s);
    u64(f, len);
    assert(fwrite(s, 1, len, f) == len);
}
static void string_kv(FILE *f, const char *key, const char *value) {
    str(f, key); u32(f, GGUF_VALUE_STRING); str(f, value);
}
static void tensor(FILE *f, const char *name, uint64_t width, uint64_t rows,
                   uint32_t type, uint64_t offset) {
    str(f, name); u32(f, 2); u64(f, width); u64(f, rows);
    u32(f, type); u64(f, offset);
}

/* The gap is deliberately unwritten: the fixture stays small while exercising
 * a page-aligned disk-only tail and the native full-view remapping contract. */
static uint64_t fixture(const char *path, bool engram) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite("GGUF", 4, 1, f) == 1);
    u32(f, 3); u64(f, engram ? 3 : 2); u64(f, engram ? 6 : 5);
    string_kv(f, "general.architecture", engram ? "deepseek41" : "qwen4exp");
    string_kv(f, "general.name", "disk-tail borrowed metadata");
    str(f, "general.alignment"); u32(f, GGUF_VALUE_UINT32); u32(f, 32);
    str(f, "test.tokens"); u32(f, GGUF_VALUE_ARRAY);
    u32(f, GGUF_VALUE_STRING); u64(f, 3);
    str(f, "first"); str(f, ""); str(f, "last");
    str(f, "test.flag"); u32(f, GGUF_VALUE_BOOL); assert(fputc(1, f) == 1);
    if (engram)
        string_kv(f, "deepseek41.engram.encoding", "e4m3_e8m0_32_row264");
    tensor(f, "token_embd.weight", 1, 1, DS4_TENSOR_F32, 0);
    long directory = ftell(f);
    assert(directory > 0);
    if (engram) {
        tensor(f, "blk.1.engram_embd.weight", 264, 2, DS4_TENSOR_I8, 0);
        tensor(f, "blk.14.engram_embd.weight", 264, 2, DS4_TENSOR_I8, 0);
    } else {
        tensor(f, "per_layer_token_embd.weight", 160, 3, DS4_TENSOR_BF16, 0);
    }
    long end = ftell(f);
    assert(end > directory && (uint64_t)end < tail_start);
    uint64_t data_start = align_up((uint64_t)end, 32);
    assert(fseek(f, directory, SEEK_SET) == 0);
    if (engram) {
        tensor(f, "blk.1.engram_embd.weight", 264, 2, DS4_TENSOR_I8, tail_start - data_start);
        tensor(f, "blk.14.engram_embd.weight", 264, 2, DS4_TENSOR_I8, tail_start + 528 - data_start);
    } else {
        tensor(f, "per_layer_token_embd.weight", 160, 3, DS4_TENSOR_BF16, tail_start - data_start);
    }
    assert(fseek(f, (long)data_start, SEEK_SET) == 0);
    u32(f, 0x3f800000);
    assert(fseek(f, (long)tail_start, SEEK_SET) == 0);
    if (engram) {
        for (unsigned i = 0; i < 1056; i++) assert(fputc((int)(i % 251), f) != EOF);
    } else {
        for (unsigned row = 0; row < 3; row++) {
            uint16_t bf16 = (uint16_t)(0x3f80 + row * 0x80); /* 1, 2, 4 */
            for (unsigned col = 0; col < 160; col++)
                assert(fwrite(&bf16, 2, 1, f) == 1);
        }
    }
    assert(fclose(f) == 0);
    return tail_start + (engram ? 1056 : 960);
}

static void check_borrowed(ds4_str s, const ds4_model *m) {
    uintptr_t start = (uintptr_t)m->map, ptr = (uintptr_t)s.ptr;
    assert(ptr >= start && ptr - start <= m->tensor_data_pos);
    assert(s.len <= m->tensor_data_pos - (ptr - start));
}

static void check_model(const char *path, bool engram) {
    uint64_t file_size = fixture(path, engram);
    first_mapping = NULL;
    ds4_model m;
    model_open(&m, path, false, false);
    assert(m.size == tail_start && m.file_size == file_size);
    assert(m.max_tensor_bytes == 4);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(m.map, &info, sizeof(info)) == sizeof(info));
    assert(info.AllocationBase == m.map && info.Type == MEM_MAPPED);
    assert(info.State == MEM_COMMIT && info.Protect == PAGE_READONLY);
    assert(info.RegionSize == m.size);
    assert(VirtualQuery(m.map + m.size, &info, sizeof(info)) == sizeof(info));
    assert(info.AllocationBase != m.map); /* Table is outside this view. */
    if (first_mapping != m.map) {
        assert(VirtualQuery(first_mapping, &info, sizeof(info)) == sizeof(info));
        assert(info.State == MEM_FREE); /* Replaced full view did not leak. */
    }
#endif
    for (uint64_t i = 0; i < m.n_kv; i++) check_borrowed(m.kv[i].key, &m);
    for (uint64_t i = 0; i < m.n_tensors; i++) check_borrowed(m.tensors[i].name, &m);
    ds4_str name, arch;
    assert(model_get_string(&m, "general.name", &name));
    assert(ds4_streq(name, "disk-tail borrowed metadata"));
    check_borrowed(name, &m);
    assert(model_get_string(&m, "general.architecture", &arch));
    assert(ds4_streq(arch, engram ? "deepseek41" : "qwen4exp"));
    uint32_t alignment;
    bool flag;
    assert(model_get_u32(&m, "general.alignment", &alignment) && alignment == 32);
    assert(model_get_bool(&m, "test.flag", &flag) && flag);
    ds4_array_ref tokens;
    assert(model_get_array(&m, "test.tokens", &tokens));
    assert(tokens.type == GGUF_VALUE_STRING && tokens.len == 3);
    ds4_cursor c = cursor_at(&m, tokens.data_pos);
    const char *expected[] = {"first", "", "last"};
    for (unsigned i = 0; i < 3; i++) {
        ds4_str token;
        assert(cursor_string(&c, &token) && ds4_streq(token, expected[i]));
        check_borrowed(token, &m);
    }
    const ds4_tensor *weight = model_find_tensor(&m, "token_embd.weight");
    assert(weight && *(const float *)tensor_data(&m, weight) == 1.0f);
    if (engram) {
        assert(m.ngram_fd == -1 && !m.ngram_tensor);
        const char *names[] = {"blk.1.engram_embd.weight", "blk.14.engram_embd.weight"};
        for (unsigned i = 0; i < 2; i++) {
            const ds4_tensor *table = model_find_tensor(&m, names[i]);
            assert(table && table->abs_offset == tail_start + i * 528);
            unsigned char raw[528];
            assert(pread(m.fd, raw, sizeof(raw), (off_t)table->abs_offset) == sizeof(raw));
            for (unsigned j = 0; j < sizeof(raw); j++)
                assert(raw[j] == (i * 528 + j) % 251);
        }
    } else {
        assert(m.ngram_fd >= 0 && m.ngram_fd != m.fd);
        assert(m.ngram_tensor == model_find_tensor(&m, "per_layer_token_embd.weight"));
        assert(m.ngram_tensor->abs_offset == m.size && m.ngram_tensor->bytes == 960);
        uint32_t rows[] = {2, 0, 1, 2};
        float out[4 * 160];
        assert(qwen4_ngram_read(&m, rows, 4, out));
        for (unsigned row = 0; row < 4; row++) {
            uint32_t expected_bits = (0x3f80u + rows[row] * 0x80u) << 16;
            for (unsigned col = 0; col < 160; col++) {
                uint32_t bits;
                memcpy(&bits, &out[row * 160 + col], sizeof(bits));
                assert(bits == expected_bits);
            }
        }
    }
    const uint8_t *prefix = m.map;
    int model_fd = m.fd, disk_fd = m.ngram_fd;
    model_close(&m);
    assert(!m.map && !m.kv && !m.tensors && m.fd == -1 && m.ngram_fd == -1);
    assert(fcntl(model_fd, F_GETFD) == -1 && errno == EBADF);
    if (disk_fd >= 0) assert(fcntl(disk_fd, F_GETFD) == -1 && errno == EBADF);
#ifdef _WIN32
    assert(VirtualQuery(prefix, &info, sizeof(info)) == sizeof(info));
    assert(info.State == MEM_FREE);
#endif
}

int main(void) {
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    /* Exercise a page boundary that is not a Windows 64 KiB boundary. */
    tail_start = (uint64_t)page * 17;
    char path[] = "ds4-model-disk-tail-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);
    check_model(path, false);
    check_model(path, true);
    assert(unlink(path) == 0);
    puts("Model disk-only tails: borrowed metadata, prefix views, disk reads and cleanup OK");
    return 0;
}
