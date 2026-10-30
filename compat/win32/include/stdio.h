/* Keep CRT stream ownership visible to the compatibility descriptor table. */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <ds4_ucrt/stdio.h>
#else
#include_next <stdio.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
int gufo_fclose(FILE* stream);
FILE* gufo_fopen(const char* path, const char* mode);
FILE* fmemopen(void* buffer, size_t size, const char* mode);
size_t gufo_fread(void* buffer, size_t size, size_t count, FILE* stream);
size_t gufo_fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
int gufo_fflush(FILE* stream);
int gufo_fseek(FILE* stream, long offset, int origin);
long gufo_ftell(FILE* stream);
int gufo_fseeko(FILE* stream, long long offset, int origin);
long long gufo_ftello(FILE* stream);
int gufo_fgetpos(FILE* stream, fpos_t* position);
int gufo_fsetpos(FILE* stream, const fpos_t* position);
int gufo_feof(FILE* stream);
int gufo_ferror(FILE* stream);
void gufo_clearerr(FILE* stream);
void gufo_rewind(FILE* stream);
int gufo_fileno(FILE* stream);
int gufo_fgetc(FILE* stream);
int gufo_fputc(int byte, FILE* stream);
int gufo_ungetc(int byte, FILE* stream);
char* gufo_fgets(char* buffer, int size, FILE* stream);
int gufo_fputs(const char* text, FILE* stream);
int gufo_setvbuf(FILE* stream, char* buffer, int mode, size_t size);
#ifdef __cplusplus
}
#endif
#ifndef GUFO_STDIO_IMPLEMENTATION
#define fdopen _fdopen
#define ftello gufo_ftello
#define fseeko gufo_fseeko
#define fclose gufo_fclose
#define fopen gufo_fopen
#define fread gufo_fread
#define fwrite gufo_fwrite
#define fflush gufo_fflush
#define fseek gufo_fseek
#define ftell gufo_ftell
#define fgetpos gufo_fgetpos
#define fsetpos gufo_fsetpos
#define feof gufo_feof
#define ferror gufo_ferror
#define clearerr gufo_clearerr
#define rewind gufo_rewind
#define fileno gufo_fileno
#define fgetc gufo_fgetc
#define fputc gufo_fputc
#undef getc
#undef putc
#define getc gufo_fgetc
#define putc gufo_fputc
#define ungetc gufo_ungetc
#define fgets gufo_fgets
#define fputs gufo_fputs
#define setvbuf gufo_setvbuf
#endif
