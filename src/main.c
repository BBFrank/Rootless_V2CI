#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include "init/load_config.h"
#include "project_worker.h"
#include "utils/utils.h"
#include "utils/scripts_runner.h"

volatile sig_atomic_t terminate_main_flag = 0;

static void main_sigterm_handler(int signum) {
    if (signum == SIGTERM) {
        terminate_main_flag = 1;
    }
}

static void daemonize() {
    pid_t pid;

    // Fork off: the parent terminates and the child will be responsible for starting the daemon
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Create a new session for the daemon
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    // Fork again so I'll be a child of the session leader and I'm sure I won't have access to the terminal
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Try to change the working directory to /
    chdir("/");

    // Set the signal handler for SIGTERM
    signal(SIGTERM, main_sigterm_handler);
}

static int remove_failed_archs_from_project(project_t *prj, char *failed_chroots[], int num_failed_chroots) {
    int i = 0;
    while (i < prj->arch_count) {
        int found = 0;
        for (int j = 0; j < num_failed_chroots; j++) {
            if (strcmp(prj->architectures[i], failed_chroots[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (found) {
            // Shift left the remaining architectures
            for (int k = i; k < prj->arch_count - 1; k++) {
                prj->architectures[k] = prj->architectures[k + 1];
            }
            prj->arch_count--;
            prj->architectures[prj->arch_count] = NULL;
        } else {
            i++;
        }
    }
    if (prj->arch_count == 0) {
        return 1; // All architectures failed
    }
    return 0;
}

int main() {
    printf("Starting rootless_v2ci...\n");

    // 0. Load variables from the configuration file
    Config cfg;
    printf("Loading configuration variables...\n");
    if(load_config(&cfg) != 0) {
        fprintf(stderr, "Failed to load configuration variables. Exiting.\n");
        return 1;
    }
    printf("Configuration loaded successfully.\n");

    // 1. Create main dirs and files (if they don't exist):
    int build_dir_result = recursive_mkdir_or_file(cfg.build_dir, 0755, 0);
    if (build_dir_result != 0) {
        fprintf(stderr, "Error: Unable to create build directory at %s: %s\n", cfg.build_dir, strerror(errno));
        return 1;
    }
    int logs_dir_result = recursive_mkdir_or_file(cfg.main_log_file, 0755, 1);
    if (logs_dir_result != 0) {
        fprintf(stderr, "Error: Unable to create main log file at %s: %s\n", cfg.main_log_file, strerror(errno));
        return 1;
    }
    printf("Main directories and files are set up.\n");

    // 2. Daemonize the process
    printf("Daemonizing the process...\n");
    printf("The main log file is located at: %s\n", cfg.main_log_file);
    daemonize();

    // 2.1. Create and open the main log file
    FILE *log_fp = fopen(cfg.main_log_file, "a");
    if (!log_fp) {
        return 1;
    }
    setvbuf(log_fp, NULL, _IOLBF, 0);

    // 2.2. Setup PID file path and check if another instance is running (useful for interrupting the chroot setups)
    char PID_FILE[MAX_CONFIG_ATTR_LEN];
    snprintf(PID_FILE, sizeof(PID_FILE), "/tmp/rootless_v2ci.pid");
    FILE* pid_fp_check = fopen(PID_FILE, "r");
    if (pid_fp_check) {
        pid_t old_pid;
        if (fscanf(pid_fp_check, "%d", &old_pid) == 1) {
            if (kill(old_pid, 0) == 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, NULL, "v2ci is already running (probably chroot setups did not finish) with PID %d.", old_pid);
                fclose(pid_fp_check);
                return 1;
            }
        }
        fclose(pid_fp_check);
    }

    // 2.3. Set the temporary pid file for the main
    FILE *pid_fp = fopen(PID_FILE, "w");
    if (pid_fp) {
        fprintf(pid_fp, "%d\n", getpid());
        fclose(pid_fp);
    } else {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, NULL, "Unable to create PID file at %s", PID_FILE);
        return 1;
    }

    // 3. Merge the needed architectures from all projects into a single list of unique architectures
    char *archs_list[MAX_ARCHITECTURES];
    int num_archs = 0;
    project_t *current = cfg.projects;
    for (int i = 0; i < cfg.project_count; i++) {
        for (int j = 0; j < current->arch_count; j++) {
            char *arch = current->architectures[j];
            int found = 0;
            for (int k = 0; k < num_archs; k++) {
                if (strcmp(archs_list[k], arch) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found && num_archs < MAX_ARCHITECTURES) {
                archs_list[num_archs] = arch;
                num_archs++;
            }
        }
        current = current->next;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, NULL, NULL, "Unique architectures to be built across all projects:");
    for (int i = 0; i < num_archs; i++) {
        fprintf(log_fp, "%s ", archs_list[i]);
    }
    fprintf(log_fp, "\n");

    // 4. Iterative chroot_setup for each architecture
    char *failed_chroots[MAX_ARCHITECTURES];
    int num_failed_chroots = 0;
    for (int i = 0; i < num_archs; i++) {
        // Chroot setup are the most time-consuming operations, so if a termination signal is received, exit immediately
        if (terminate_main_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, NULL, NULL, "Termination signal received during chroot setups, exiting...");
            remove(PID_FILE);
            fclose(log_fp);
            return 1;
        }
        char chroot_dir[MAX_CONFIG_ATTR_LEN + 32];
        snprintf(chroot_dir, sizeof(chroot_dir), "%s/%s-chroot", cfg.build_dir, archs_list[i]);
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, NULL, NULL, "Setting up chroot at %s for architecture %s...", chroot_dir, archs_list[i]);
        if (chroot_setup(archs_list[i], chroot_dir, cfg.main_log_file, log_fp) != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, NULL, "Failed to set up chroot for architecture %s.", archs_list[i]);
            failed_chroots[num_failed_chroots] = archs_list[i];
            num_failed_chroots++;
        }
    }
    if (num_failed_chroots == num_archs) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, NULL, "All chroot setups failed. Exiting...");
        fclose(log_fp);
        remove(PID_FILE);
        return 1;
    }

    // 5. Iterative project builders launch through fork (new process for each project)
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, NULL, NULL, "Launching project build processes...");
    current = cfg.projects;
    int launched_projects = 0;
    for (int i = 0; i < cfg.project_count; i++) {
        // Before launching a new project worker, check if a termination signal was received
        if (terminate_main_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, NULL, NULL, "Termination signal received before launching project %s, exiting...", current->name);
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, NULL, "Failed to fork for project %s. Exiting...", current->name);
            break;
        } else if (pid == 0) {
            // Child process: launch the project worker only with architectures that succeeded in chroot setup
            int failed_removal = remove_failed_archs_from_project(current, failed_chroots, num_failed_chroots);
            if (failed_removal == 1) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, current->name, NULL, "Unable to remove failed architectures from project %s.", current->name);
            }
            int result = project_worker(current, cfg.build_dir);
            exit(result);
        } else {
            // Parent process: continue to the next project
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, current->name, NULL, "Launched project %s with PID %d.", current->name, pid);
            current = current->next;
            launched_projects++;
        }
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, NULL, NULL, "Logs will be available in the various project log files. In particular:");
    current = cfg.projects;
    for (int i = 0; i < launched_projects; i++) {
        project_t *proj = current;
        fprintf(log_fp, "- Project '%s' log file: %s\n", proj->name, proj->worker_log_file);
        current = current->next;
    }
    fprintf(log_fp, "To terminate the entire process, run: ./v2ci_stop\n");

    fclose(log_fp);
    remove(PID_FILE);
    return 0;
}