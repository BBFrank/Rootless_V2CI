#ifndef TYPES_H
#define TYPES_H

#define TEST_ENABLED 1                                          // Set to 1 to enable testing, 0 to disable (only for sshlirp)

#include <pthread.h>
#include <signal.h>

#define DEFAULT_CONFIG_PATH "~/.config/v2ci/config.yml"         // Substitute with the actual absolute path of the config file (path/to/config.yml)
#define SCRIPTS_DIR_PATH "/usr/lib/v2ci/scripts"                // Substitute with the actual absolute path of the scripts directory
#define CHROOT_SETUP_SCRIPT_PATH SCRIPTS_DIR_PATH "/chroot_setup.sh"
#define CHECK_UPDATES_SCRIPT_PATH SCRIPTS_DIR_PATH "/check_updates.sh"
#define INSTALL_PACKAGES_SCRIPT_PATH SCRIPTS_DIR_PATH "/install_packages_in_chroot.sh"
#define CLONE_OR_PULL_SCRIPT_PATH SCRIPTS_DIR_PATH "/clone_or_pull_for_project.sh"
#define BUILD_SCRIPT_PATH SCRIPTS_DIR_PATH "/cross_compiler.sh"

#define MAX_ARCHITECTURES 9
#define MAX_DEPENDENCIES 16
#define MIN_CONFIG_ATTR_LEN 128
#define CONFIG_ATTR_LEN 256
#define MAX_CONFIG_ATTR_LEN 512
#define MAX_COMMAND_LEN 4096

struct project;

typedef struct thread_arg {
    struct project *project;
    char arch[64];

    char thread_log_file[MAX_CONFIG_ATTR_LEN];          // Absolute path of the log file for this thread (e.g. <project->main_project_build_dir>/logs/<arch>-worker.log)
    char thread_chroot_dir[MAX_CONFIG_ATTR_LEN];        // /<cfg.build_dir>/<arch-chroot>/
    char thread_chroot_build_dir[MAX_CONFIG_ATTR_LEN];  // /home/<project.name>/ (absolute w.r.t chroot -> <cfg.build_dir>/<arch-chroot>/home/<project.name>/)
    char thread_chroot_log_file[MAX_CONFIG_ATTR_LEN];   // /home/<project.name>/logs/worker.log (relative to chroot)
    char thread_chroot_target_dir[MAX_CONFIG_ATTR_LEN]; // /home/<project.name>/binaries (relative to chroot)

    volatile sig_atomic_t *terminate_flag;
} thread_arg_t;

typedef struct thread_result {
    int status;
    char *error_message;
    char *stats;
} thread_result_t;

typedef struct manual_dependency {
    char git_url[MAX_CONFIG_ATTR_LEN];
    char build_system[MIN_CONFIG_ATTR_LEN];
    char *dependencies[MAX_DEPENDENCIES];
    int dep_count;
    struct manual_dependency *next;
} manual_dependency_t;

typedef struct binaries_limits_for_project {
    int daily_mem_limit;
    int weekly_mem_limit;
    int monthly_mem_limit;
    int yearly_mem_limit;

    // Note: the daily interval correspond to poll_interval of the project
    int weekly_interval;
    int monthly_interval;
    int yearly_interval;
} binaries_limits_for_project_t;

typedef struct project {
    char name[64];
    char main_project_build_dir[CONFIG_ATTR_LEN];       // <cfg.build_dir>/<project.name>
    char worker_log_file[MAX_CONFIG_ATTR_LEN];              // <main_project_build_dir>/logs/worker.log
    char target_dir[CONFIG_ATTR_LEN];                   // Absolute path got from config file

    char repo_url[MAX_CONFIG_ATTR_LEN];
    char main_repo_build_system[CONFIG_ATTR_LEN];

    char build_mode[MIN_CONFIG_ATTR_LEN];
    int  poll_interval;

    char *architectures[MAX_ARCHITECTURES];
    int arch_count;

    char *dependency_packages[MAX_DEPENDENCIES];
    int dep_count;

    manual_dependency_t *manual_dependencies;
    int manual_dep_count;

    binaries_limits_for_project_t *binaries_limits;

    struct project *next;
} project_t;

typedef struct {
    char build_dir[MIN_CONFIG_ATTR_LEN];
    char main_log_file[CONFIG_ATTR_LEN];
    project_t *projects;
    int project_count;
} Config;

#endif // TYPES_H