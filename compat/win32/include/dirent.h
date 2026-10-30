/* ds4-owned native Windows directory subset; no third-party implementation. */
#ifndef DS4_WIN32_DIRENT_H
#define DS4_WIN32_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ds4_win32_directory DIR;

struct dirent {
    /* A Windows component has at most 255 UTF-16 units, each requiring at
     * most three UTF-8 bytes. Supplementary characters require fewer per unit. */
    char d_name[255 * 3 + 1];
};

/* Paths and names are strict UTF-8. The returned entry belongs to the stream
 * and is overwritten by readdir; closedir releases it and the native handle.
 * readdir leaves errno unchanged at EOF and sets it on an error. */
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);

#ifdef __cplusplus
}
#endif
#endif
