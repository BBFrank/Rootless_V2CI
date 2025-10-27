#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include "utils/scripts_runner.h"
#include "utils/utils.h"

static int lock_package_manager_in_chroot(const char *chroot_dir, FILE *log_fp, const char *project_name, const char *thread_arch) {
    char lock_file_path[MAX_CONFIG_ATTR_LEN];
    snprintf(lock_file_path, sizeof(lock_file_path), "%s/lock", chroot_dir);
    int fd = open(lock_file_path, O_CREAT | O_RDWR, 0644);
    if (fd == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "Unable to open lock file %s: %s", lock_file_path, strerror(errno));
        return 1;
    }
    if (flock(fd, LOCK_EX) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "Unable to acquire lock on %s: %s", lock_file_path, strerror(errno));
        close(fd);
        return 1;
    }
    return fd;
}

static int unlock_package_manager_in_chroot(int fd, FILE *log_fp, const char *project_name, const char *thread_arch) {
    if (flock(fd, LOCK_UN) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "Unable to release lock: %s", strerror(errno));
        return 1;
    }
    close(fd);
    return 0;
}
    
// This function is the entry point for each build thread.
// Its roles include:
// - install all dependencies packages in the chroot
// - clone or pull the sources of the main project and all its manual dependencies
// - build all the manual dependencies and the main project itself

void *build_thread(void *arg) {
    // Extract arguments
    thread_arg_t *targ = (thread_arg_t *)arg;
    project_t *prj = targ->project;
    char *arch = targ->arch;
    volatile sig_atomic_t *terminate_flag = targ->terminate_flag;

    // Prepare return value
    thread_result_t *result = malloc(sizeof(thread_result_t));
    if (!result) {
        return NULL;
    }
    result->status = 1;
    result->error_message = NULL;
    result->stats = "Progress: 0%";

    // Create log file for the thread (couple project-architecture)
    int thread_log_file_result = recursive_mkdir_or_file(targ->thread_log_file, 0755, 1);
    if (thread_log_file_result != 0) {
        return (void *)result;
    }

    // Initialize logging
    FILE *log_fp = fopen(targ->thread_log_file, "a");
    setvbuf(log_fp, NULL, _IOLBF, 0);
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Build thread started for project %s, architecture %s.", prj->name, arch);

    // Create all necessary directories and files in the chroot:
    char expanded_chroot_build_dir[MAX_CONFIG_ATTR_LEN*2];
    snprintf(expanded_chroot_build_dir, sizeof(expanded_chroot_build_dir), "%s%s", targ->thread_chroot_dir, targ->thread_chroot_build_dir);
    char expanded_chroot_log_file[MAX_CONFIG_ATTR_LEN*2];
    snprintf(expanded_chroot_log_file, sizeof(expanded_chroot_log_file), "%s%s", targ->thread_chroot_dir, targ->thread_chroot_log_file);
    char expanded_chroot_target_dir[MAX_CONFIG_ATTR_LEN*2];
    snprintf(expanded_chroot_target_dir, sizeof(expanded_chroot_target_dir), "%s%s", targ->thread_chroot_dir, targ->thread_chroot_target_dir);
    // Note: the chroot dir itself (<cfg.build_dir>/<arch>-chroot) is assumed to be already created (during chroot setup in main)
    int chroot_build_dir_result = recursive_mkdir_or_file(expanded_chroot_build_dir, 0755, 0);
    if (chroot_build_dir_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Unable to create chroot build directory at %s: %s", expanded_chroot_build_dir, strerror(errno));
        result->error_message = "Unable to create chroot build directory";
        return (void *)result;
    }
    int chroot_log_file_result = recursive_mkdir_or_file(expanded_chroot_log_file, 0755, 1);
    if (chroot_log_file_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Unable to create chroot log file at %s: %s", expanded_chroot_log_file, strerror(errno));
        result->error_message = "Unable to create chroot log file";
        return (void *)result;
    }
    int chroot_target_dir_result = recursive_mkdir_or_file(expanded_chroot_target_dir, 0755, 0);
    if (chroot_target_dir_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Unable to create chroot target directory at %s: %s", expanded_chroot_target_dir, strerror(errno));
        result->error_message = "Unable to create chroot target directory";
        return (void *)result;
    }

    // Install all dependency packages in the chroot
    if (*terminate_flag) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Termination signal received before starting build for architecture %s for project %s, exiting...", arch, prj->name);
        result->error_message = "Termination signal received before starting build";
        return (void *)result;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Starting installation of dependencies packages in chroot for architecture %s for project %s...", arch, prj->name);
    // First lock this phase to avoid different forked workers accessing apt/dnf/ in the same time in same chroot
    // Note: the different threads won't interfere as they work in different chroots, but the forked processes could do it (they operate on different projects but maybe sharing some chroots)
    int lock_fd = lock_package_manager_in_chroot(targ->thread_chroot_dir, log_fp, prj->name, arch);
    if (lock_fd == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Failed to acquire package manager lock");
        result->error_message = "Failed to acquire package manager lock";
        return (void *)result;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Acquired package manager lock for architecture %s for project %s.", arch, prj->name);
    result->stats = "Progress: 10%";
    // Install all main dependency packages in the chroot
    int install_result = install_packages_list_in_chroot(prj->dependency_packages, targ->thread_chroot_dir, log_fp, targ->thread_chroot_log_file, prj->name, arch);
    if (install_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Failed to install main dependencies packages in chroot for architecture %s for project %s.", arch, prj->name);
        result->error_message = "Failed to install main dependencies packages";
        return (void *)result;
    }
    result->stats = "Progress: 30%";
    // Install all secondary (manual) dependency packages in the chroot (for each manual dependency, install its dependencies)
    manual_dependency_t *cur_manual = prj->manual_dependencies;
    while (cur_manual) {
        int install_result = install_packages_list_in_chroot(cur_manual->dependencies, targ->thread_chroot_dir, log_fp, targ->thread_chroot_log_file, prj->name, arch);
        if (install_result != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Failed to install dependencies packages for manual dependency %s in chroot for architecture %s for project %s. Retrying after poll interval.", cur_manual->git_url, arch, prj->name);
            result->error_message = "Failed to install manual dependencies packages";
            return (void *)result;
        }
        cur_manual = cur_manual->next;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "All dependencies installed in chroot for architecture %s for project %s.", arch, prj->name);
    result->stats = "Progress: 50%";
    // Release the lock on package manager
    int unlock_result = unlock_package_manager_in_chroot(lock_fd, log_fp, prj->name, arch);
    if (unlock_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Failed to release package manager lock");
        result->error_message = "Failed to release package manager lock";
        return (void *)result;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Released package manager lock for architecture %s for project %s.", arch, prj->name);

    // Clone or pull the sources of the main project and all its manual dependencies
    if (*terminate_flag) {
        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, arch, "Termination signal received before cloning sources for architecture %s for project %s, exiting...", arch, prj->name);
        result->error_message = "Termination signal received before cloning sources";
        return (void *)result;
    }
    int clone_result = clone_or_pull_sources_inside_chroot(targ, log_fp);
    if (clone_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Failed to clone or pull sources inside chroot for architecture %s for project %s.", arch, prj->name);
        result->error_message = "Failed to clone or pull sources inside chroot";
        return (void *)result;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "All sources cloned or pulled inside chroot for architecture %s for project %s.", arch, prj->name);
    result->stats = "Progress: 70%";

    // Start the build process in the chroot (compilation of manual dependencies and main project)
    if (*terminate_flag) {
        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, arch, "Termination signal received before starting build for architecture %s for project %s, exiting...", arch, prj->name);
        result->error_message = "Termination signal received before starting build";
        return (void *)result;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Starting build process for architecture %s for project %s...", arch, prj->name);
    int build_result = build_in_chroot(targ, log_fp);
    if (build_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, arch, "Build failed for architecture %s for project %s.", arch, prj->name);
        result->error_message = "Build failed";
        return (void *)result;
    }

    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, arch, "Build completed successfully for architecture %s for project %s.", arch, prj->name);
    result->stats = "Progress: 100%";
    result->status = 0;
    return (void *)result;
}
    