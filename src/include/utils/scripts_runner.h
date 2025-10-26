#include <stdio.h>
#include "types/types.h"

int chroot_setup(const char *debian_arch, const char *chroot_dir, const char* main_log_file, FILE *log_fp);

int check_for_updates_inside_chroot(const char *chroot_dir, const char *chroot_build_dir, const char *repo_name, const char *worker_tmp_chroot_log_file, FILE *log_fp, int *need2update, const char *project_name, const char *tmp_arch);

int install_packages_list_in_chroot(char *package[], const char *chroot_dir, FILE *log_fp, const char *thread_log_file, const char *project_name, const char *thread_arch);

int clone_or_pull_sources_inside_chroot(thread_arg_t *targ, FILE *log_fp);

int build_in_chroot(thread_arg_t *targ, FILE *log_fp);
