/* POSIX fmemopen for the UCRT: opaque FILE tokens dispatched by stdio shadows.
 * No payload-sized staging buffer, temporary file, or Win32 file handle exists.
 * The registry's shared lock protects token lifetime; each stream has its own
 * lock so large independent snapshot writes do not serialize with each other. */
#define GUFO_STDIO_IMPLEMENTATION
#include "memory_stream.h"
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct memory_stream {
    struct memory_stream *next;
    SRWLOCK lock;
    unsigned char *buffer;
    size_t capacity, length, position;
    int readable, writable, append, owned, eof, error, dirty, pushed;
};
static SRWLOCK registry_lock = SRWLOCK_INIT;
static struct memory_stream *streams;

static struct memory_stream *acquire(FILE *token) {
    AcquireSRWLockShared(&registry_lock);
    for (struct memory_stream *s = streams; s; s = s->next) {
        if ((FILE *)s == token) {
            AcquireSRWLockExclusive(&s->lock);
            return s;
        }
    }
    ReleaseSRWLockShared(&registry_lock);
    return NULL;
}
static void release(struct memory_stream *s) {
    ReleaseSRWLockExclusive(&s->lock);
    ReleaseSRWLockShared(&registry_lock);
}
static int failure(struct memory_stream *s, int error) {
    s->error = 1;
    errno = error;
    return EOF;
}
static void flush_memory(struct memory_stream *s) {
    if (s->dirty && s->capacity) {
        s->buffer[s->position < s->capacity ? s->position : s->capacity - 1] = 0;
        s->dirty = 0;
    }
}
FILE *fmemopen(void *buffer, size_t size, const char *mode) {
    if (!mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') ||
        size > LLONG_MAX) { errno = EINVAL; return NULL; }
    int plus = 0, binary = 0;
    for (const char *p = mode + 1; *p; ++p) {
        if (*p == '+' && !plus) plus = 1;
        else if (*p == 'b' && !binary) binary = 1;
        else { errno = EINVAL; return NULL; }
    }
    struct memory_stream *s = calloc(1, sizeof(*s));
    if (!s) { errno = ENOMEM; return NULL; }
    if (!buffer) {
        buffer = calloc(size ? size : 1, 1);
        if (!buffer) { free(s); errno = ENOMEM; return NULL; }
        s->owned = 1;
    }
    s->buffer = buffer;
    s->capacity = size;
    s->readable = mode[0] == 'r' || plus;
    s->writable = mode[0] != 'r' || plus;
    s->append = mode[0] == 'a';
    s->pushed = -1;
    InitializeSRWLock(&s->lock);
    if (mode[0] == 'r') s->length = size;
    else if (mode[0] == 'a') s->position = s->length = strnlen(buffer, size);
    else if (size) s->buffer[0] = 0;
    AcquireSRWLockExclusive(&registry_lock);
    s->next = streams;
    streams = s;
    ReleaseSRWLockExclusive(&registry_lock);
    return (FILE *)s;
}
int gufo_memory_close(FILE *token, int *result) {
    AcquireSRWLockExclusive(&registry_lock);
    struct memory_stream **link = &streams;
    while (*link && (FILE *)*link != token) link = &(*link)->next;
    struct memory_stream *s = *link;
    if (!s) { ReleaseSRWLockExclusive(&registry_lock); return 0; }
    *link = s->next;
    flush_memory(s);
    ReleaseSRWLockExclusive(&registry_lock);
    if (s->owned) free(s->buffer);
    free(s);
    *result = 0;
    return 1;
}
size_t gufo_fread(void *buffer, size_t size, size_t count, FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return fread(buffer, size, count, token);
    if (!size || !count) { release(s); return 0; }
    if (!s->readable) { failure(s, EBADF); release(s); return 0; }
    if (count > SIZE_MAX / size) { failure(s, EOVERFLOW); release(s); return 0; }
    const size_t requested = size * count;
    size_t copied = 0;
    if (s->pushed >= 0) {
        ((unsigned char *)buffer)[copied++] = (unsigned char)s->pushed;
        s->pushed = -1;
        ++s->position;
    }
    const size_t available = s->position < s->length ? s->length - s->position : 0;
    const size_t n = requested - copied < available ? requested - copied : available;
    if (n) memcpy((unsigned char *)buffer + copied, s->buffer + s->position, n);
    s->position += n;
    copied += n;
    if (copied < requested) s->eof = 1;
    release(s);
    return copied / size;
}
size_t gufo_fwrite(const void *buffer, size_t size, size_t count, FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return fwrite(buffer, size, count, token);
    if (!size || !count) { release(s); return 0; }
    if (!s->writable) { failure(s, EBADF); release(s); return 0; }
    if (count > SIZE_MAX / size) { failure(s, EOVERFLOW); release(s); return 0; }
    if (s->append) s->position = s->length;
    s->pushed = -1;
    const size_t requested = size * count;
    const size_t available = s->capacity - s->position;
    const size_t copied = requested < available ? requested : available;
    if (copied) {
        if (s->position > s->length)
            memset(s->buffer + s->length, 0, s->position - s->length);
        memcpy(s->buffer + s->position, buffer, copied);
        s->position += copied;
        if (s->position > s->length) s->length = s->position;
        s->dirty = 1;
    }
    if (copied < requested) failure(s, ENOSPC);
    release(s);
    return copied / size;
}
int gufo_fflush(FILE *token) {
    if (!token) {
        AcquireSRWLockShared(&registry_lock);
        for (struct memory_stream *s = streams; s; s = s->next) {
            AcquireSRWLockExclusive(&s->lock);
            flush_memory(s);
            ReleaseSRWLockExclusive(&s->lock);
        }
        ReleaseSRWLockShared(&registry_lock);
        return fflush(NULL);
    }
    struct memory_stream *s = acquire(token);
    if (!s) return fflush(token);
    flush_memory(s);
    release(s);
    return 0;
}
int gufo_fseeko(FILE *token, long long offset, int origin) {
    struct memory_stream *s = acquire(token);
    if (!s) return _fseeki64(token, offset, origin);
    long long base;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_CUR) base = (long long)s->position;
    else if (origin == SEEK_END) base = (long long)s->length;
    else { errno = EINVAL; release(s); return -1; }
    if (offset < -base || offset > (long long)s->capacity - base) {
        errno = EINVAL; release(s); return -1;
    }
    flush_memory(s);
    s->position = (size_t)(base + offset);
    s->eof = 0;
    s->pushed = -1;
    release(s);
    return 0;
}
long long gufo_ftello(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return _ftelli64(token);
    const long long result = (long long)s->position;
    release(s);
    return result;
}
int gufo_fseek(FILE *token, long offset, int origin) {
    return gufo_fseeko(token, offset, origin);
}
long gufo_ftell(FILE *token) {
    const long long position = gufo_ftello(token);
    if (position > LONG_MAX) { errno = EOVERFLOW; return -1; }
    return (long)position;
}
int gufo_fgetpos(FILE *token, fpos_t *position) {
    struct memory_stream *s = acquire(token);
    if (!s) return fgetpos(token, position);
    *position = (fpos_t)s->position;
    release(s);
    return 0;
}
int gufo_fsetpos(FILE *token, const fpos_t *position) {
    struct memory_stream *s = acquire(token);
    if (!s) return fsetpos(token, position);
    if (*position < 0 || (unsigned long long)*position > s->capacity) {
        errno = EINVAL; release(s); return -1;
    }
    flush_memory(s);
    s->position = (size_t)*position;
    s->eof = 0;
    s->pushed = -1;
    release(s);
    return 0;
}
int gufo_feof(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return feof(token);
    const int result = s->eof;
    release(s);
    return result;
}
int gufo_ferror(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return ferror(token);
    const int result = s->error;
    release(s);
    return result;
}
void gufo_clearerr(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) { clearerr(token); return; }
    s->error = s->eof = 0;
    release(s);
}
void gufo_rewind(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) { rewind(token); return; }
    flush_memory(s);
    s->position = 0;
    s->eof = s->error = 0;
    s->pushed = -1;
    release(s);
}
int gufo_fileno(FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return _fileno(token);
    release(s);
    errno = EBADF;
    return -1;
}
int gufo_fgetc(FILE *token) {
    unsigned char byte;
    return gufo_fread(&byte, 1, 1, token) == 1 ? byte : EOF;
}
int gufo_fputc(int byte, FILE *token) {
    unsigned char value = (unsigned char)byte;
    return gufo_fwrite(&value, 1, 1, token) == 1 ? value : EOF;
}
int gufo_ungetc(int byte, FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return ungetc(byte, token);
    if (byte == EOF || !s->readable || s->pushed >= 0 || !s->position) {
        release(s); return EOF;
    }
    s->pushed = (unsigned char)byte;
    --s->position;
    s->eof = 0;
    release(s);
    return (unsigned char)byte;
}
char *gufo_fgets(char *buffer, int size, FILE *token) {
    struct memory_stream *s = acquire(token);
    if (!s) return fgets(buffer, size, token);
    if (size <= 0) { errno = EINVAL; release(s); return NULL; }
    if (!s->readable) { failure(s, EBADF); release(s); return NULL; }
    int n = 0;
    while (n < size - 1) {
        int byte;
        if (s->pushed >= 0) {
            byte = s->pushed;
            s->pushed = -1;
            ++s->position;
        } else if (s->position < s->length) byte = s->buffer[s->position++];
        else { s->eof = 1; break; }
        buffer[n++] = (char)byte;
        if (byte == '\n') break;
    }
    buffer[n] = 0;
    release(s);
    return n || size == 1 ? buffer : NULL;
}
int gufo_fputs(const char *text, FILE *token) {
    const size_t length = strlen(text);
    return gufo_fwrite(text, 1, length, token) == length ? 0 : EOF;
}
int gufo_setvbuf(FILE *token, char *buffer, int mode, size_t size) {
    struct memory_stream *s = acquire(token);
    if (!s) return setvbuf(token, buffer, mode, size);
    if (mode != _IONBF) { release(s); errno = ENOTSUP; return -1; }
    release(s);
    return 0;
}
