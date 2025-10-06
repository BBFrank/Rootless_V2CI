#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include "utils/utils.h"
#include "utils/project_worker_utils.h"
#include "project_worker.h"
#include "build_thread.h"
#include "utils/scripts_runner.h"

volatile sig_atomic_t terminate_worker_flag = 0;

static void sigterm_handler(int signum) {
    if (signum == SIGTERM) {
        terminate_worker_flag = 1;
    }
}

static void update_worker_state(const char *state, char *STATE_FILE) {
    FILE *fp = fopen(STATE_FILE, "w");
    if (fp) {
        fprintf(fp, "%s", state);
        fclose(fp);
    } else {
        perror("Failed to update daemon state file");
    }
}

static void sleep_and_handle_interrupts(int poll_interval, char *STATE_FILE, FILE *log_fp, const char *project_name) {
    // Sleep cycle and handling of interrupt signals
    update_worker_state(PROJECT_WORKER_STATE_SLEEPING, STATE_FILE);
    unsigned int time_left = poll_interval;
    while(time_left > 0) {
        time_left = sleep(time_left);
        if (terminate_worker_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, project_name, NULL, "Sleep interrupted by termination signal.");
            break; 
        }
        if (time_left > 0) {
            // If I had time left to sleep and I didn't receive a stop signal, then I was disturbed by someone else and so I ignore and sleep again
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, project_name, NULL, "Sleep interrupted, %u seconds remaining, continuing to wait...", time_left);
        }
    }
    update_worker_state(PROJECT_WORKER_STATE_WORKING, STATE_FILE);
}

int project_worker(project_t *prj, char *main_build_dir) {
    // Create log file for the process
    int worker_log_file_result = recursive_mkdir_or_file(prj->worker_log_file, 0755, 1);
    if (worker_log_file_result != 0) {
        return 1;
    }

    // Initialize logging
    FILE *log_fp = fopen(prj->worker_log_file, "a");
    setvbuf(log_fp, NULL, _IOLBF, 0);
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "v2ci process started.");

    // Initialize pid and state file paths
    char PID_FILE[256];
    char STATE_FILE[256];
    snprintf(PID_FILE, sizeof(PID_FILE), "/tmp/%s-worker.pid", prj->name);
    snprintf(STATE_FILE, sizeof(STATE_FILE), "/tmp/%s-worker.state", prj->name);

    // Check if another instance of the process is already running
    FILE* pid_fp_check = fopen(PID_FILE, "r");
    if (pid_fp_check) {
        pid_t old_pid;
        if (fscanf(pid_fp_check, "%d", &old_pid) == 1) {
            if (kill(old_pid, 0) == 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "v2ci is already running with PID %d.", old_pid);
                fclose(pid_fp_check);
                return 1;
            }
        }
        fclose(pid_fp_check);
    }

    // Set the pid file for the worker
    FILE *pid_fp = fopen(PID_FILE, "w");
    if (pid_fp) {
        fprintf(pid_fp, "%d\n", getpid());
        fclose(pid_fp);
    } else {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Error: Unable to create PID file for project %s at %s", prj->name, PID_FILE);
        fclose(log_fp);
        return 1;
    }

    // Set up signal handler for graceful termination
    signal(SIGTERM, sigterm_handler);

    // Create necessary directories and files for the worker
    int main_worker_build_dir_result = recursive_mkdir_or_file(prj->main_project_build_dir, 0755, 0);
    if (main_worker_build_dir_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to create main project build directory at %s: %s", prj->main_project_build_dir, strerror(errno));
        fclose(log_fp);
        return 1;
    }
    int worker_target_dir_result = recursive_mkdir_or_file(prj->target_dir, 0755, 0);
    if (worker_target_dir_result != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to create target directory at %s: %s", prj->target_dir, strerror(errno));
        fclose(log_fp);
        return 1;
    }

    // Declare chroot dir (<main_build_dir>/<architecture>-chroot) and chroot build dir (/home/<prj->name>) strings
    char chroot_dir[MAX_CONFIG_ATTR_LEN];
    char chroot_build_dir[MAX_CONFIG_ATTR_LEN];
    char worker_tmp_chroot_log_file[MAX_CONFIG_ATTR_LEN+20];

    // Update state to working
    update_worker_state(PROJECT_WORKER_STATE_WORKING, STATE_FILE);

    // Main loop
    while (1) {
        if (terminate_worker_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received before starting operations, exiting...");
            break;
        }
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Starting build operations...");

        // Depending on build mode (main or dependency), perform the update check (obviously on the first iteration the check will return the need to clone all the repos)
        int need2update = 0;
        // Initialize chroot paths for the update_check (use the first architecture for the check, it doesn't matter which one)
        snprintf(chroot_dir, sizeof(chroot_dir), "%s/%s-chroot", main_build_dir, prj->architectures[0]);
        snprintf(chroot_build_dir, sizeof(chroot_build_dir), "/home/%s", prj->name);
        snprintf(worker_tmp_chroot_log_file, sizeof(worker_tmp_chroot_log_file), "%s/logs/worker.log", chroot_build_dir);
        
        if (strcmp(prj->build_mode, "main") == 0) {
            // Check for updates in main repo
            char *main_repo_name = NULL;
            int extract_result = extract_repo_name(prj->repo_url, &main_repo_name);
            if (extract_result != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to extract repository name from URL %s", prj->repo_url);
                sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
                if (terminate_worker_flag) {
                    formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                    break;
                }
                continue;
            }
            int update_result = check_for_updates_inside_chroot(chroot_dir, chroot_build_dir, main_repo_name, worker_tmp_chroot_log_file, log_fp, &need2update, prj->name, prj->architectures[0]);
            if (update_result != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to check for updates in main repository.");
                sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
                if (terminate_worker_flag) {
                    formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                    break;
                }
                continue;
            }
        } else if (strcmp(prj->build_mode, "dep") == 0) {
            // For each manual dependency, check for updates
            manual_dependency_t *cur_manual = prj->manual_dependencies;
            while (cur_manual) {
                char *dependency_repo_name = NULL;
                int extract_result = extract_repo_name(cur_manual->git_url, &dependency_repo_name);
                if (extract_result != 0) {
                    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to extract repository name from URL %s", cur_manual->git_url);
                    sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
                    if (terminate_worker_flag) {
                        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                        break;
                    }
                    continue;
                }
                int update_result = check_for_updates_inside_chroot(chroot_dir, chroot_build_dir, dependency_repo_name, worker_tmp_chroot_log_file, log_fp, &need2update, prj->name, prj->architectures[0]);
                if (update_result != 0) {
                    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to check for updates in manual dependency %s", dependency_repo_name);
                    sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
                    if (terminate_worker_flag) {
                        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                        break;
                    }
                    continue;
                }
                if (need2update) {
                    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Update detected in manual dependency %s.", cur_manual->git_url);
                    break;
                }
                cur_manual = cur_manual->next;
            }
            if (terminate_worker_flag) {
                formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during manual dependency update checks, exiting...");
                break;
            }
        }

        // If no updates were found, sleep for the poll interval and restart the loop
        if (!need2update) {
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "No updates found for project %s. Sleeping for %d seconds.", prj->name, prj->poll_interval);
            sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
            continue;
        }

        // Otherwise, setup threads for each architecture
        pthread_t threads[prj->arch_count];
        thread_arg_t args[prj->arch_count];
        for (int i = 0; i < prj->arch_count; i++) {
            args[i].project = prj;
            snprintf(args[i].arch, sizeof(args[i].arch), "%s", prj->architectures[i]);

            snprintf(args[i].thread_log_file, sizeof(args[i].thread_log_file), "%s/logs/%s-worker.log", prj->main_project_build_dir, prj->architectures[i]);
            snprintf(args[i].thread_chroot_dir, sizeof(args[i].thread_chroot_dir), "%s/%s-chroot", main_build_dir, prj->architectures[i]);
            snprintf(args[i].thread_chroot_build_dir, sizeof(args[i].thread_chroot_build_dir), "/home/%s", prj->name);
            snprintf(args[i].thread_chroot_log_file, sizeof(args[i].thread_chroot_log_file), "/home/%s/logs/worker.log", prj->name);
            snprintf(args[i].thread_chroot_target_dir, sizeof(args[i].thread_chroot_target_dir), "/home/%s/binaries", prj->name);

            args[i].terminate_flag = &terminate_worker_flag;
        }

        // For each architecture, start a build thread
        int i = 0;
        while (i < prj->arch_count) {
            if (pthread_create(&threads[i], NULL, build_thread, &args[i]) != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to create thread for architecture %s: %s. Retrying after poll interval.", prj->architectures[i], strerror(errno));
                sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
                if (terminate_worker_flag) {
                    formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                    break;
                }
                continue;
            } else {
                formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Thread created successfully for architecture %s.", args[i].arch);
                i++;
            }
        }

        if (terminate_worker_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during thread creation retry (after failure): only %d out of %d threads were created. Joining launched threads...", i, prj->arch_count);
        }
        // Wait only for the threads that were successfully created
        for (int j = 0; j < i; j++) {
            void *thread_return_value = NULL;
            int successful_join = pthread_join(threads[j], &thread_return_value);
            if (successful_join != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to join thread for architecture %s: %s", args[j].arch, strerror(errno));
            } else {
                if (thread_return_value != NULL) {
                    thread_result_t *thread_result = (thread_result_t *)thread_return_value;
                    if (thread_result->status != 0) {
                        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Thread for architecture %s terminated with errors (code %d): %s", args[j].arch, thread_result->status, (thread_result->error_message ? thread_result->error_message : "Unknown error"));
                    } else {
                        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Thread for architecture %s terminated successfully. Here the stats: %s", args[j].arch, (thread_result->stats ? thread_result->stats : "No stats available"));
                    }
                } else {
                    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Thread for architecture %s terminated without a specific return value.", args[j].arch);
                }
            }
            // Free the thread return value (allocated in build_thread)
            free(thread_return_value);
        }
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "All launched build threads (%d out of %d) joined successfully for project %s.", i, prj->arch_count, prj->name);
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Your final binaries (if the build was successful) are located in %s for each architecture.", prj->target_dir);

        // After all threads complete, if termination flag is set, exit
        if (terminate_worker_flag) {
            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal detected for project %s, exiting...", prj->name);
            break;
        }

        // Sleep for the poll interval before the next iteration
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Sleeping for %d seconds before the next check.", prj->poll_interval);
        sleep_and_handle_interrupts(prj->poll_interval, STATE_FILE, log_fp, prj->name);
    }

    // Cleanup
    remove(PID_FILE);
    remove(STATE_FILE);
    // Free the project structure and its manual dependencies (allocated in load_config)
    manual_dependency_t *cur_manual = prj->manual_dependencies;
    while (cur_manual) {
        manual_dependency_t *next_manual = cur_manual->next;
        free(cur_manual);
        cur_manual = next_manual;
    }
    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "v2ci process for project %s exiting.", prj->name);
    fclose(log_fp);
    free(prj);
    return 0;
}