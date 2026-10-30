/* Model-independent snapshot memory-stream contract. Compile as C. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_snapshot_last_byte(void) {
    unsigned char storage[259];
    memset(storage, 0xa5, sizeof(storage));
    unsigned char payload[257];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (unsigned char)(i * 17 + 3);
    payload[sizeof(payload) - 1] = 0xfe;
    FILE *stream = fmemopen(storage, sizeof(payload) + 1, "wb");
    assert(stream && ftello(stream) == 0);
    assert(fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload));
    assert(fflush(stream) == 0 && ftello(stream) == sizeof(payload));
    assert(storage[sizeof(payload)] == 0);
    assert(memcmp(storage, payload, sizeof(payload)) == 0);
    assert(fclose(stream) == 0);
    assert(storage[sizeof(payload) - 1] == 0xfe);
    assert(storage[sizeof(payload)] == 0 && storage[sizeof(payload) + 1] == 0xa5);

    stream = fmemopen(storage, sizeof(payload), "rb");
    assert(stream && fileno(stream) == -1 && errno == EBADF);
    unsigned char restored[257];
    assert(fread(restored, 1, sizeof(restored), stream) == sizeof(restored));
    assert(!feof(stream) && !ferror(stream));
    assert(memcmp(restored, payload, sizeof(payload)) == 0);
    assert(fgetc(stream) == EOF && feof(stream));
    assert(fseeko(stream, -1, SEEK_END) == 0 && !feof(stream));
    assert(fgetc(stream) == 0xfe);
    assert(fclose(stream) == 0);
    assert(storage[sizeof(payload) - 1] == 0xfe); /* Read close never writes NUL. */
}

static void test_modes_seek_and_ownership(void) {
    unsigned char buffer[16];
    memset(buffer, 'x', sizeof(buffer));
    FILE *stream = fmemopen(buffer, sizeof(buffer), "w+b");
    assert(stream && buffer[0] == 0);
    assert(fseeko(stream, 0, SEEK_END) == 0 && ftello(stream) == 0);
    assert(fwrite("abc", 1, 3, stream) == 3);
    assert(fseek(stream, 6, SEEK_SET) == 0);
    assert(fputc('Z', stream) == 'Z');
    assert(fflush(stream) == 0 && ftello(stream) == 7);
    assert(memcmp(buffer, "abc\0\0\0Z\0", 8) == 0);
    fpos_t position;
    assert(fgetpos(stream, &position) == 0);
    rewind(stream);
    assert(ftell(stream) == 0 && !feof(stream) && !ferror(stream));
    assert(fgetc(stream) == 'a' && ungetc('Q', stream) == 'Q');
    assert(fgetc(stream) == 'Q' && buffer[0] == 'a');
    assert(fsetpos(stream, &position) == 0 && ftello(stream) == 7);
    assert(fseeko(stream, LLONG_MAX, SEEK_CUR) == -1 && errno == EINVAL);
    assert(fseeko(stream, LLONG_MIN, SEEK_SET) == -1 && errno == EINVAL);
    assert(fseeko(stream, -1, SEEK_SET) == -1 && errno == EINVAL);
    assert(fclose(stream) == 0);
    buffer[0] = 'u'; /* Borrowed buffer remains caller-owned and writable. */

    memcpy(buffer, "ab\0xxxx", 7);
    stream = fmemopen(buffer, sizeof(buffer), "a+");
    assert(stream && ftello(stream) == 2);
    assert(fseeko(stream, 0, SEEK_SET) == 0);
    assert(fwrite("c", 1, 1, stream) == 1 && fflush(stream) == 0);
    assert(ftello(stream) == 3 && memcmp(buffer, "abc\0", 4) == 0);
    assert(fclose(stream) == 0);

    stream = fmemopen(NULL, 32, "w+");
    assert(stream && fputs("allocated\n", stream) == 0);
    rewind(stream);
    char line[32];
    assert(fgets(line, sizeof(line), stream) == line && strcmp(line, "allocated\n") == 0);
    assert(fclose(stream) == 0); /* Internal buffer freed by close. */
}

static void test_capacity_and_errors(void) {
    unsigned char guarded[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    FILE *stream = fmemopen(guarded + 1, 4, "wb");
    assert(stream);
    assert(fwrite("12345", 1, 5, stream) == 4 && errno == ENOSPC);
    assert(ferror(stream));
    assert(guarded[0] == 0xaa && guarded[5] == 0xff);
    assert(fflush(stream) == 0);
    assert(memcmp(guarded + 1, "123\0", 4) == 0); /* Full buffer reserves trailing NUL on flush. */
    clearerr(stream);
    assert(!ferror(stream));
    assert(fwrite("X", 1, 1, stream) == 0 && errno == ENOSPC);
    assert(fclose(stream) == 0);

    const unsigned char readonly[] = {'r', 0, 'z'};
    stream = fmemopen((void *)readonly, sizeof(readonly), "rb");
    assert(stream && fwrite("x", 1, 1, stream) == 0 && errno == EBADF);
    assert(ferror(stream));
    clearerr(stream);
    unsigned char result[8];
    assert(fread(result, 2, 2, stream) == 1 && feof(stream));
    assert(result[0] == 'r' && result[1] == 0 && result[2] == 'z');
    assert(fclose(stream) == 0 && readonly[2] == 'z');

    stream = fmemopen(NULL, 0, "w+");
    assert(stream && fwrite("a", 1, 1, stream) == 0 && errno == ENOSPC);
    assert(fclose(stream) == 0);
    stream = fmemopen(guarded, sizeof(guarded), "wb");
    assert(stream && fread(result, 1, 1, stream) == 0 && errno == EBADF);
    assert(fwrite(result, SIZE_MAX, 2, stream) == 0 && errno == EOVERFLOW);
    assert(fclose(stream) == 0);
    assert(fmemopen(guarded, sizeof(guarded), "invalid") == NULL && errno == EINVAL);
}

int main(void) {
    test_snapshot_last_byte();
    test_modes_seek_and_ownership();
    test_capacity_and_errors();
    puts("Win32 direct memory stream tests passed");
    return 0;
}
