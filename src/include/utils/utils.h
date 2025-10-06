#ifndef UTILS_H
#define UTILS_H

#include "types/types.h"
#include <stdio.h>
#include <sys/types.h>

FILE *fopen_expanding_tilde(const char *path, const char *mode);

char *expand_tilde(const char *path);

void log_time(FILE *log_file);

void formatted_log(FILE *log_file, const char *log_level, const char *source_file, int line_number, const char *project_name, const char *thread_arch, const char *format, ...);

int recursive_mkdir_or_file(const char *path, mode_t mode, int file_mode);

int extract_repo_name(const char *git_url, char **repo_name);

#endif // UTILS_H


