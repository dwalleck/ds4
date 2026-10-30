#include "platform.h"
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    HANDLE handles[2];
    DWORD modes[2];
    UINT code_page;
    unsigned count;
    volatile LONG active;
} console_output;

static void restore_console_output(void) {
    if (!InterlockedExchange(&console_output.active, 0)) return;
    for (unsigned i = 0; i < console_output.count; ++i)
        SetConsoleMode(console_output.handles[i], console_output.modes[i]);
    SetConsoleOutputCP(console_output.code_page);
}

static void finish_console_output(void) {
    /* CRT stream flushing follows atexit callbacks; flush UTF-8 while its
     * console code page is still active. Never do this on a control thread. */
    fflush(NULL);
    restore_console_output();
}

static BOOL WINAPI console_output_control(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        restore_console_output();
        break;
    }
    /* Register before application/CRT signal handlers. Handled cancellation
     * stops before this fallback; default termination restores the console. */
    return FALSE;
}

int ds4_win32_console_init(void) {
    const DWORD ids[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    if (InterlockedCompareExchange(&console_output.active, 0, 0)) return 0;
    console_output.count = 0;
    /* Capture both modes before changing either: stdout/stderr may refer to
     * the same screen buffer through different native handles. */
    for (unsigned i = 0; i < 2; ++i) {
        HANDLE handle = GetStdHandle(ids[i]);
        DWORD mode;
        if (GetConsoleMode(handle, &mode)) {
            unsigned n = console_output.count++;
            console_output.handles[n] = handle;
            console_output.modes[n] = mode;
        }
    }
    if (!console_output.count) return 0; /* Redirected byte streams stay unchanged. */
    console_output.code_page = GetConsoleOutputCP();
    if (!console_output.code_page ||
        !SetConsoleCtrlHandler(console_output_control, TRUE)) {
        errno = EIO;
        return -1;
    }
    if (atexit(finish_console_output)) {
        SetConsoleCtrlHandler(console_output_control, FALSE);
        errno = ENOMEM;
        return -1;
    }
    InterlockedExchange(&console_output.active, 1);
    if (!SetConsoleOutputCP(CP_UTF8)) goto failed;
    for (unsigned i = 0; i < console_output.count; ++i) {
        if (!SetConsoleMode(console_output.handles[i], console_output.modes[i] |
                            ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            goto failed;
    }
    return 0;
failed:
    restore_console_output();
    SetConsoleCtrlHandler(console_output_control, FALSE);
    errno = EIO;
    return -1;
}

int ds4_win32_temp_path(char *path, size_t capacity, const char *name) {
    if (!path || !capacity || capacity > INT_MAX || !name ||
        strchr(name, '/') || strchr(name, '\\')) { errno = EINVAL; return -1; }
    DWORD needed = GetTempPathW(0, NULL);
    if (!needed) { errno = EIO; return -1; }
    wchar_t *wide = malloc((size_t)needed * sizeof(*wide));
    if (!wide) { errno = ENOMEM; return -1; }
    DWORD length = GetTempPathW(needed, wide);
    if (!length || length >= needed) { free(wide); errno = EIO; return -1; }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                    path, (int)capacity, NULL, NULL);
    free(wide);
    if (!bytes) { errno = ENAMETOOLONG; return -1; }
    size_t n = strlen(name);
    if (n > capacity - (size_t)bytes) { errno = ENAMETOOLONG; return -1; }
    memcpy(path + bytes - 1, name, n + 1);
    return 0;
}
