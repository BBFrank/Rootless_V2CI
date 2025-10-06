#include <stdio.h>
#include <stdlib.h>
#include <execs.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "types/types.h"

FILE *fopen_expanding_tilde(const char *path, const char *mode) {
    if (!path) return NULL;
    if (path[0] != '~') return fopen(path, mode);
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s", home, path + 1);
    return fopen(buf, mode);
}

char *expand_tilde(const char *path) {
    if (!path) return NULL;
    if (path[0] != '~') return strdup(path);
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s", home, path + 1);
    return strdup(buf);
}

void log_time(FILE *log_file) {
    time_t now;
    struct tm *local_time;
    char time_buffer[80];

    now = time(NULL);
    local_time = localtime(&now);

    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", local_time);

    fprintf(log_file, "[%s] ", time_buffer);
}

static int get_client_stats(const char *command, char *buffer, size_t buffer_size) {
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        return 1;
    }
    if (fgets(buffer, buffer_size, fp) != NULL) {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        pclose(fp);
        return 0;
    } else {
        pclose(fp);
        return 1;
    }
}

void formatted_log(FILE *log_file,
    const char *log_level,
    const char *source_file,
    int line_number,
    const char *project_name,
    const char *thread_arch,
    const char *format, ...) {

    char ip_buffer[128];
    if (get_client_stats("curl -s --max-time 3 https://api.ipify.org", ip_buffer, sizeof(ip_buffer)) != 0) {
        snprintf(ip_buffer, sizeof(ip_buffer), "Unknown IP");
    }
    char os_buffer[128];
    if (get_client_stats("uname -o", os_buffer, sizeof(os_buffer)) != 0) {
        snprintf(os_buffer, sizeof(os_buffer), "Unknown OS");
    }
    char arch_buffer[128];
    if (get_client_stats("uname -m", arch_buffer, sizeof(arch_buffer)) != 0) {
        snprintf(arch_buffer, sizeof(arch_buffer), "Unknown Arch");
    }
    char agent_buffer[128];
    if (get_client_stats("hostnamectl | grep -F 'Hardware Model' | cut -d ':' -f2 | sed 's/^[[:space:]]*//'", agent_buffer, sizeof(agent_buffer)) != 0) {
        snprintf(agent_buffer, sizeof(agent_buffer), "Unknown Agent");
    }

    char message_buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message_buffer, sizeof(message_buffer), format, args);
    va_end(args);

    log_time(log_file);
    fprintf(log_file, "[%s] source: { client: { ip: %s, os: %s, arch: %s, agent: %s }, location: { file: %s, line: %d } }, project: %s, thread_arch: %s, message: %s\n",
        log_level,
        ip_buffer,
        os_buffer,
        arch_buffer,
        agent_buffer,
        source_file,
        line_number,
        project_name ? project_name : "N/A",
        thread_arch ? thread_arch : "N/A",
        message_buffer
    );
}

int recursive_mkdir_or_file(const char *path, mode_t mode, int file_mode) {
    char *temp_path = strdup(path);
    if (!temp_path) return -1;

    char *p = temp_path;
    if (p[0] == '/') p++;

    // Reset errno before starting
    errno = 0;

    while (*p) {
        if (*p == '/') {
            *p = '\0';
            if (strlen(temp_path) > 0) {
                struct stat st;
                int stat_result = stat(temp_path, &st);
                // Create only if temp_path does not exist or if it exists but is not a directory
                if ((stat_result == 0 && !S_ISDIR(st.st_mode)) || errno == ENOENT) {
                    if (mkdir(temp_path, mode) != 0) {
                        free(temp_path);
                        return -1;
                    }
                }
            }
            *p = '/';
        }
        p++;
    }

    if (strlen(temp_path) > 0) {
        if (file_mode) {
            FILE *file = fopen(temp_path, "w");
            if (!file) {
                free(temp_path);
                return -1;
            }
            fclose(file);
            chmod(temp_path, mode);
        } else {
            struct stat st;
            int stat_result = stat(temp_path, &st);
            if ((stat_result == 0 && !S_ISDIR(st.st_mode)) || errno == ENOENT) {
                if (mkdir(temp_path, mode) != 0) {
                    free(temp_path);
                    return -1;
                }
            }
        }
    }

    free(temp_path);
    return 0;
}

int extract_repo_name(const char *git_url, char **repo_name) {
    if (!git_url || !repo_name) return 1;

    const char *last_slash = strrchr(git_url, '/');
    if (!last_slash || *(last_slash + 1) == '\0') return -1;

    const char *name_start = last_slash + 1;
    const char *dot_git = strstr(name_start, ".git");
    size_t name_length = dot_git ? (size_t)(dot_git - name_start) : strlen(name_start);

    *repo_name = (char *)malloc(name_length + 1);
    if (!*repo_name) return 1;

    strncpy(*repo_name, name_start, name_length);
    (*repo_name)[name_length] = '\0';

    return 0;
}