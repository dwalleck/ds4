/* Native console regression: run in a private, hidden console so the user's
 * shell modes/code pages and input queue are never changed by this test. */
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../linenoise.h"
#include "../compat/win32/platform.h"

static HANDLE diagnostic_output;

static void diagnostic(const char *text) {
    DWORD written;
    if (diagnostic_output)
        WriteFile(diagnostic_output, text, (DWORD)strlen(text), &written, NULL);
    else
        fputs(text, stderr);
}

static void dump_bytes(const char *label, const char *bytes, size_t len) {
    char text[1024];
    int used = snprintf(text, sizeof(text), "%s (%zu bytes):", label, len);
    for (size_t i = 0; i < len && used + 4 < (int)sizeof(text); i++)
        used += snprintf(text + used, sizeof(text) - (size_t)used,
                         " %02x", (unsigned char)bytes[i]);
    text[used++] = '\n';
    text[used] = '\0';
    diagnostic(text);
}

static int failure(int line, const char *condition) {
    char text[512];
    snprintf(text, sizeof(text), "line %d: %s (errno %d, Win32 %lu)\n",
             line, condition, errno, (unsigned long)GetLastError());
    diagnostic(text);
    return line;
}

#define CHECK(condition) do { if (!(condition)) return failure(__LINE__, #condition); } while (0)

static INPUT_RECORD key(WORD vk, WCHAR wc, WORD repeats) {
    INPUT_RECORD record = {0};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = repeats;
    record.Event.KeyEvent.wVirtualKeyCode = vk;
    record.Event.KeyEvent.uChar.UnicodeChar = wc;
    return record;
}

static int redirected_console_test(void) {
    int input = _fileno(stdin), output = _fileno(stdout);
    HANDLE ih = (HANDLE)_get_osfhandle(input), reader, writer;
    DWORD original_mode, mode, count;
    UINT original_cp = GetConsoleCP();
    CHECK(GetConsoleMode(ih, &original_mode));
    CHECK(SetConsoleMode(ih, original_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    CHECK(SetConsoleCP(437)); /* Deliberately not UTF-8. */
    CHECK(!_putenv_s("LINENOISE_ASSUME_TTY", ""));
    CHECK(FlushConsoleInputBuffer(ih));
    CHECK(CreatePipe(&reader, &writer, NULL, 0));
    CHECK(fflush(stdout) == 0);
    int saved_output = _dup(output);
    int pipe_output = _open_osfhandle((intptr_t)writer, _O_WRONLY | _O_BINARY);
    CHECK(saved_output >= 0 && pipe_output >= 0);
    CHECK(_dup2(pipe_output, output) == 0);
    INPUT_RECORD records[] = {
        key(0, 0x4e2d, 1), key(0, 0xd83d, 1), key(0, 0xde00, 1),
        key(VK_RETURN, L'\r', 1)
    };
    CHECK(WriteConsoleInputW(ih, records, 4, &count) && count == 4);
    char *line = linenoise("this prompt must not reach redirected output");
    CHECK(line && !strcmp(line, "\xe4\xb8\xad\xf0\x9f\x98\x80"));
    linenoiseFree(line);
    CHECK(GetConsoleCP() == 437);
    CHECK(GetConsoleMode(ih, &mode) &&
          mode == (original_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    CHECK(fflush(stdout) == 0);
    CHECK(_dup2(saved_output, output) == 0);
    CHECK(_close(saved_output) == 0);
    CHECK(PeekNamedPipe(reader, NULL, 0, NULL, &count, NULL) && count == 0);
    CHECK(_close(pipe_output) == 0);
    CHECK(CloseHandle(reader));
    CHECK(SetConsoleCP(original_cp));
    CHECK(SetConsoleMode(ih, original_mode));
    return 0;
}

static int check_private_history(WCHAR *path) {
    HANDLE token;
    union {
        TOKEN_USER user;
        BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } identity;
    DWORD count, revision;
    PSID owner;
    PACL acl;
    PSECURITY_DESCRIPTOR security;
    SECURITY_DESCRIPTOR_CONTROL control;
    ACCESS_ALLOWED_ACE *ace;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    CHECK(GetTokenInformation(token, TokenUser, &identity, sizeof(identity), &count));
    CHECK(CloseHandle(token));
    CHECK(GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          &owner, NULL, &acl, NULL, &security) == ERROR_SUCCESS);
    CHECK(owner && EqualSid(owner, identity.user.User.Sid));
    CHECK(GetSecurityDescriptorControl(security, &control, &revision));
    CHECK(control & SE_DACL_PROTECTED);
    CHECK(acl && acl->AceCount == 1);
    CHECK(GetAce(acl, 0, (void **)&ace));
    CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(ace->Header.AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS);
    CHECK(EqualSid(&ace->SidStart, identity.user.User.Sid));
    LocalFree(security);
    return 0;
}

static int history_test(void) {
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    char utf8[MAX_PATH * 3], content[128];
    DWORD count;
    CHECK(GetTempPathW(MAX_PATH, directory));
    CHECK(GetTempFileNameW(directory, L"lnh", 0, path));
    CHECK(DeleteFileW(path)); /* Exercise creation, not just an existing file. */
    CHECK(WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL));
    CHECK(linenoiseHistorySetMaxLen(1));
    CHECK(linenoiseHistoryAdd("private \xe4\xb8\xad history"));
    CHECK(linenoiseHistorySave(utf8) == 0);
    int error = check_private_history(path);
    if (error) return error;

    /* Start with an explicitly world-accessible existing file. Saving must
     * replace its DACL, protect it from inheritance, and truncate old bytes. */
    BYTE world[SECURITY_MAX_SID_SIZE];
    DWORD world_len = sizeof(world);
    union {
        DWORD alignment;
        BYTE storage[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    } acl_buffer;
    PACL acl = (PACL)acl_buffer.storage;
    CHECK(CreateWellKnownSid(WinWorldSid, NULL, world, &world_len));
    CHECK(InitializeAcl(acl, sizeof(acl_buffer), ACL_REVISION));
    CHECK(AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, world));
    CHECK(SetNamedSecurityInfoW(path, SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          NULL, NULL, acl, NULL) == ERROR_SUCCESS);
    CHECK(linenoiseHistoryAdd("x"));
    CHECK(linenoiseHistorySave(utf8) == 0);
    error = check_private_history(path);
    if (error) return error;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(ReadFile(file, content, sizeof(content), &count, NULL));
    CHECK(count == 3 && !memcmp(content, "x\r\n", 3));
    /* An existing reader denies exclusive writing. Failure must be reported
     * and leave the previous contents intact, not fake a successful save. */
    CHECK(linenoiseHistoryAdd("must not replace existing data"));
    CHECK(linenoiseHistorySave(utf8) == -1);
    CHECK(errno == EACCES);
    LARGE_INTEGER zero = {0};
    CHECK(SetFilePointerEx(file, zero, NULL, FILE_BEGIN));
    CHECK(ReadFile(file, content, sizeof(content), &count, NULL));
    CHECK(count == 3 && !memcmp(content, "x\r\n", 3));
    CHECK(CloseHandle(file));
    CHECK(DeleteFileW(path));
    return 0;
}

static int frontend_output_child(void) {
    FILE *stream;
    CHECK(_wfreopen_s(&stream, L"CONOUT$", L"w", stdout) == 0);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout))));
    CHECK(ds4_win32_console_init() == 0);
    /* No explicit flush: atexit must flush before restoring the legacy CP. */
    printf("\x1b[H\x1b[32m\xce\xbb!\x1b[0m");
    return 0;
}

static int frontend_output_test(void) {
    HANDLE output = (HANDLE)_get_osfhandle(_fileno(stdout));
    DWORD original_mode, code, count;
    UINT original_cp = GetConsoleOutputCP();
    CHECK(GetConsoleMode(output, &original_mode));
    const DWORD legacy_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    CHECK(SetConsoleMode(output, legacy_mode) && SetConsoleOutputCP(437));
    WCHAR executable[MAX_PATH], command[MAX_PATH + 32];
    CHECK(GetModuleFileNameW(NULL, executable, MAX_PATH));
    CHECK(swprintf(command, sizeof(command) / sizeof(command[0]),
                   L"\"%ls\" --frontend-output", executable) > 0);
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    startup.cb = sizeof(startup);
    CHECK(CreateProcessW(executable, command, NULL, NULL, FALSE, 0,
                         NULL, NULL, &startup, &process));
    DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    CHECK(GetExitCodeProcess(process.hProcess, &code));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CHECK(wait == WAIT_OBJECT_0 && code == 0);
    DWORD restored;
    CHECK(GetConsoleMode(output, &restored) && restored == legacy_mode);
    CHECK(GetConsoleOutputCP() == 437);
    CONSOLE_SCREEN_BUFFER_INFO info;
    CHECK(GetConsoleScreenBufferInfo(output, &info));
    COORD home = {info.srWindow.Left, info.srWindow.Top};
    WCHAR visible[2];
    WORD attributes[2];
    CHECK(ReadConsoleOutputCharacterW(output, visible, 2, home, &count) && count == 2);
    CHECK(visible[0] == 0x03bb && visible[1] == L'!');
    CHECK(ReadConsoleOutputAttribute(output, attributes, 2, home, &count) && count == 2);
    CHECK((attributes[0] & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) ==
          FOREGROUND_GREEN);
    CHECK(SetConsoleMode(output, original_mode) && SetConsoleOutputCP(original_cp));
    return 0;
}

static int console_test(void) {
    int input = _fileno(stdin), output = _fileno(stdout);
    HANDLE ih = (HANDLE)_get_osfhandle(input);
    HANDLE oh = (HANDLE)_get_osfhandle(output);
    DWORD input_mode, output_mode, mode, count;
    UINT input_cp = GetConsoleCP(), output_cp = GetConsoleOutputCP();
    CONSOLE_SCREEN_BUFFER_INFO info;
    struct linenoiseState l;
    char line[256], bytes[256];
    CHECK(!_putenv_s("LINENOISE_ASSUME_TTY", ""));
    CHECK(!_putenv_s("LINENOISE_COLS", ""));
    CHECK(GetConsoleMode(ih, &input_mode));
    CHECK(GetConsoleMode(oh, &output_mode));
    CHECK(GetConsoleScreenBufferInfo(oh, &info));
    CHECK(FlushConsoleInputBuffer(ih));
    CHECK(linenoiseEditStart(&l, input, output, line, sizeof(line), "test> ") == 0);
    CHECK(l.cols == (size_t)(info.srWindow.Right - info.srWindow.Left + 1));
    CHECK(GetConsoleMode(ih, &mode));
    CHECK(!(mode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)));
    CHECK(GetConsoleMode(oh, &mode));
    CHECK(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    CHECK(GetConsoleCP() == CP_UTF8 && GetConsoleOutputCP() == CP_UTF8);

    /* A refresh must observe the live console window, not a startup snapshot
     * or a missing Unix ioctl. Restore geometry before the input checks. */
    SMALL_RECT resized = info.srWindow;
    COORD buffer_size = info.dwSize;
    int original_columns = resized.Right - resized.Left + 1;
    int width_delta = original_columns > 40 ? -1 : 1;
    resized.Right += (SHORT)width_delta;
    if (resized.Right >= buffer_size.X) {
        CHECK(buffer_size.X < 32767);
        buffer_size.X++;
        CHECK(SetConsoleScreenBufferSize(oh, buffer_size));
    }
    CHECK(SetConsoleWindowInfo(oh, TRUE, &resized));
    linenoiseShow(&l);
    CHECK(l.cols == (size_t)(original_columns + width_delta));
    CHECK(SetConsoleWindowInfo(oh, TRUE, &info.srWindow));
    if (buffer_size.X != info.dwSize.X)
        CHECK(SetConsoleScreenBufferSize(oh, info.dwSize));
    linenoiseShow(&l);
    CHECK(l.cols == (size_t)original_columns);

    /* Non-key records must drain without turning a ready console into a
     * blocking read. Repeats and surrogate pairs remain ordinary UTF-8. */
    INPUT_RECORD records[10] = {0};
    records[0].EventType = FOCUS_EVENT;
    records[1] = key('A', 'a', 2);
    records[2] = key(0, 0x4e2d, 1);
    records[3] = key(0, 0xd83d, 1);
    records[4] = key(0, 0xde00, 1);
    records[5] = key(VK_LEFT, 0, 1);
    records[6] = key(VK_BACK, 8, 1);
    records[7] = key(VK_HOME, 0, 1);
    records[8] = key('1', '!', 1);
    CHECK(WriteConsoleInputW(ih, records, 9, &count) && count == 9);
    int n = linenoiseWindowsReadInput(input, bytes, sizeof(bytes), 1);
    CHECK(n > 0);
    for (int i = 0; i < n; i++)
        CHECK(linenoiseEditFeedByte(&l, bytes[i]) == linenoiseEditMore);
    if (strcmp(l.buf, "!aa\xf0\x9f\x98\x80")) {
        dump_bytes("decoded", bytes, (size_t)n);
        dump_bytes("edited", l.buf, l.len);
        dump_bytes("pending key", l.pending_key, l.pending_key_len);
        char state[128];
        snprintf(state, sizeof(state), "editor cursor %zu, length %zu\n", l.pos, l.len);
        diagnostic(state);
    }
    CHECK(!strcmp(l.buf, "!aa\xf0\x9f\x98\x80"));
    CHECK(l.pos == 1);
    errno = 0;
    CHECK(linenoiseWindowsReadInput(input, bytes, sizeof(bytes), 1) == -1);
    CHECK(errno == EAGAIN);

    /* Assemble repeated surrogate records before expanding their repetitions,
     * even when the consumer requests only one UTF-8 byte at a time. */
    linenoiseEditClear(&l);
    records[0] = key(0, 0xd83d, 2);
    records[1] = key(0, 0xde00, 2);
    CHECK(WriteConsoleInputW(ih, records, 2, &count) && count == 2);
    const char repeated[] = "\xf0\x9f\x98\x80\xf0\x9f\x98\x80";
    for (size_t i = 0; i < sizeof(repeated) - 1; i++) {
        CHECK(linenoiseWindowsReadInput(input, bytes, 1, 1) == 1);
        CHECK(bytes[0] == repeated[i]);
        CHECK(linenoiseEditFeedByte(&l, bytes[0]) == linenoiseEditMore);
    }
    CHECK(!strcmp(l.buf, repeated));
    errno = 0;
    CHECK(linenoiseWindowsReadInput(input, bytes, 1, 1) == -1 && errno == EAGAIN);

    /* Reapplying raw mode must never replace the original snapshot with a
     * child's cooked state; stop must restore the original exact values. */
    CHECK(SetConsoleMode(ih, input_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    CHECK(SetConsoleMode(oh, output_mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING));
    linenoiseRestoreRawMode();
    CHECK(GetConsoleMode(ih, &mode));
    CHECK(!(mode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)));
    records[0] = key('C', 3, 1);
    CHECK(WriteConsoleInputW(ih, records, 1, &count));
    errno = 0;
    CHECK(linenoiseEditFeed(&l) == NULL && errno == EAGAIN);
    linenoiseEditStop(&l);
    CHECK(GetConsoleMode(ih, &mode) && mode == input_mode);
    CHECK(GetConsoleMode(oh, &mode) && mode == output_mode);
    CHECK(GetConsoleCP() == input_cp && GetConsoleOutputCP() == output_cp);

    /* A utility write outside raw mode interprets ANSI, without permanently
     * changing the output mode or code page. */
    const char ansi[] = "\x1b[2J\x1b[HOK";
    CHECK(linenoiseWrite(output, ansi, sizeof(ansi) - 1) == 0);
    WCHAR visible[2];
    /* VT home is relative to the visible window, not scrollback buffer (0,0). */
    CONSOLE_SCREEN_BUFFER_INFO output_info;
    CHECK(GetConsoleScreenBufferInfo(oh, &output_info));
    COORD origin = {output_info.srWindow.Left, output_info.srWindow.Top};
    CHECK(ReadConsoleOutputCharacterW(oh, visible, 2, origin, &count));
    if (count != 2 || visible[0] != 'O' || visible[1] != 'K') {
        char geometry[256];
        snprintf(geometry, sizeof(geometry),
                 "ANSI output count %lu chars %04x %04x cursor %d,%d "
                 "window %d,%d..%d,%d buffer %d,%d\n",
                 (unsigned long)count, (unsigned int)visible[0], (unsigned int)visible[1],
                 output_info.dwCursorPosition.X, output_info.dwCursorPosition.Y,
                 output_info.srWindow.Left, output_info.srWindow.Top,
                 output_info.srWindow.Right, output_info.srWindow.Bottom,
                 output_info.dwSize.X, output_info.dwSize.Y);
        diagnostic(geometry);
    }
    CHECK(count == 2 && visible[0] == 'O' && visible[1] == 'K');
    CHECK(GetConsoleMode(oh, &mode) && mode == output_mode);
    CHECK(GetConsoleOutputCP() == output_cp);

    /* Pipe reads stay byte-exact, report drained separately from EOF, and
     * LINENOISE_ASSUME_TTY still permits editor tests without a console. */
    HANDLE reader, writer, sink_reader, sink_writer;
    CHECK(CreatePipe(&reader, &writer, NULL, 0));
    CHECK(CreatePipe(&sink_reader, &sink_writer, NULL, 0));
    int rfd = _open_osfhandle((intptr_t)reader, _O_RDONLY | _O_BINARY);
    int wfd = _open_osfhandle((intptr_t)sink_writer, _O_WRONLY | _O_BINARY);
    CHECK(rfd >= 0 && wfd >= 0);
    errno = 0;
    CHECK(linenoiseWindowsReadInput(rfd, bytes, sizeof(bytes), 1) == -1);
    CHECK(errno == EAGAIN);
    const char sample[] = "p\xe4\xb8\xad\r";
    CHECK(WriteFile(writer, sample, sizeof(sample) - 1, &count, NULL));
    CHECK(!_putenv_s("LINENOISE_ASSUME_TTY", "1"));
    CHECK(!_putenv_s("LINENOISE_COLS", "80"));
    CHECK(linenoiseEditStart(&l, rfd, wfd, line, sizeof(line), "> ") == 0);
    char *result;
    do { result = linenoiseEditFeed(&l); } while (result == linenoiseEditMore);
    CHECK(result && !strcmp(result, "p\xe4\xb8\xad"));
    linenoiseFree(result);
    linenoiseEditStop(&l);
    CHECK(CloseHandle(writer));
    CHECK(linenoiseWindowsReadInput(rfd, bytes, sizeof(bytes), 1) == 0);
    CHECK(_close(rfd) == 0 && _close(wfd) == 0);
    CHECK(CloseHandle(sink_reader));
    CHECK(GetConsoleMode(ih, &mode) && mode == input_mode);
    CHECK(GetConsoleMode(oh, &mode) && mode == output_mode);
    int frontend_error = frontend_output_test();
    if (frontend_error) return frontend_error;
    int history_error = history_test();
    if (history_error) return history_error;
    return redirected_console_test();
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--frontend-output"))
        return frontend_output_child();
    if (argc == 3 && !strcmp(argv[1], "--console-child")) {
        diagnostic_output = (HANDLE)(uintptr_t)_strtoui64(argv[2], NULL, 10);
        /* Inherited diagnostic pipe must not make the CRT use the parent's
         * redirected streams instead of this child's private console. */
        int input = _wopen(L"CONIN$", _O_RDWR | _O_TEXT);
        int output = _wopen(L"CONOUT$", _O_RDWR | _O_TEXT);
        CHECK(input >= 0 && output >= 0);
        CHECK(_dup2(input, _fileno(stdin)) == 0);
        CHECK(_dup2(output, _fileno(stdout)) == 0);
        CHECK(_close(input) == 0 && _close(output) == 0);
        return console_test();
    }
    WCHAR executable[MAX_PATH], command[MAX_PATH + 64];
    HANDLE diagnostics_reader, diagnostics_writer;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    CHECK(CreatePipe(&diagnostics_reader, &diagnostics_writer, &attributes, 0));
    CHECK(SetHandleInformation(diagnostics_reader, HANDLE_FLAG_INHERIT, 0));
    CHECK(GetModuleFileNameW(NULL, executable, MAX_PATH));
    CHECK(swprintf(command, sizeof(command) / sizeof(command[0]),
                   L"\"%ls\" --console-child %llu", executable,
                   (unsigned long long)(uintptr_t)diagnostics_writer) > 0);
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    CHECK(CreateProcessW(executable, command, NULL, NULL, TRUE,
                         CREATE_NEW_CONSOLE, NULL, NULL, &startup, &process));
    CHECK(CloseHandle(diagnostics_writer));
    DWORD wait = WaitForSingleObject(process.hProcess, 15000), code = 0;
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        fprintf(stderr, "linenoise console child timed out\n");
        code = 1;
    } else if (!GetExitCodeProcess(process.hProcess, &code)) {
        code = 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    char message[1024];
    DWORD received;
    while (ReadFile(diagnostics_reader, message, sizeof(message) - 1, &received, NULL) &&
           received) {
        message[received] = '\0';
        fputs(message, stderr);
    }
    CHECK(CloseHandle(diagnostics_reader));
    if (code) fprintf(stderr, "linenoise console regression failed at child line %lu\n",
                      (unsigned long)code);
    else puts("linenoise Windows console regression passed");
    return code ? 1 : 0;
}
