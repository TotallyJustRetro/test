#pragma once
/* Small portability helpers: UTF-8 paths on Windows, directory creation, native file pickers. */
#include <stdio.h>
#include <stddef.h>
FILE *fopen_u(const char *path, const char *mode);
int   mkdir_u(const char *path);                         /* 0 ok (or already there) */
int   file_exists(const char *path);
const char *path_base(const char *path);                 /* file name part */
#ifdef _WIN32
#define fopen fopen_u
#endif

enum { DLG_ROM, DLG_PATCH, DLG_IMAGE, DLG_AUDIO, DLG_FOLDER };
/* Blocking native dialog. Returns 1 and fills `out` (UTF-8) if the user picked something. */
int dlg_pick(int kind, const char *title, char *out, size_t n);
void open_folder(const char *path);                      /* show in the file manager */
