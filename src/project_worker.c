#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <execs.h>
#include <fcntl.h>
#include <sys/file.h>
#include "utils/utils.h"
#include "project_worker.h"
#include "build_thread.h"
#include "utils/scripts_runner.h"

volatile sig_atomic_t terminate_worker_flag = 0;

static void sigterm_handler(int signum) {
    if (signum == SIGTERM) {
        terminate_worker_flag = 1;
    }
}

static void sleep_and_handle_interrupts(int poll_interval, FILE *log_fp, const char *project_name) {
    // Sleep cycle and handling of interrupt signals
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
}

static int set_binaries_rotation_cronjob(project_t *prj, FILE *log_fp) {
    // 0. First lock the cronjob setting process globally (on /tmp) to avoid race conditions (every project worker might try to set cronjobs simultaneously, modifying the same crontab of the same user, leaving it in an inconsistent state)
    char cronjob_lock_file[MAX_CONFIG_ATTR_LEN];
    snprintf(cronjob_lock_file, sizeof(cronjob_lock_file), "/tmp/cronjob_lock.lock");
    int lock_fd = open(cronjob_lock_file, O_CREAT | O_RDWR, 0644);
    if (lock_fd == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to open cronjob lock file: %s", strerror(errno));
        return 1;
    }
    if (flock(lock_fd, LOCK_EX) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to acquire lock on cronjob setting lock file: %s", strerror(errno));
        close(lock_fd);
        return 1;
    }

    // 1. Prepare the cronjob entry to set. Here we set the rotation cronjob (with all the necessary parameters) script to run at the end of each day (i.e., at 00:00)
    /* Crontab entries have the following format:
    * * * * * command to be executed
    - - - - -
    | | | | |
    | | | | +----- Day of week (0 - 7) (Sunday=0 or 7)
    | | | +------- Month (1 - 12)
    | | +--------- Day of month (1 - 31)
    | +----------- Hour (0 - 23)
    +------------- Minute (0 - 59)
    */
    char cronjob_entry[MAX_COMMAND_LEN];
    char *cronjob_script_expanded_path = expand_tilde(CRONJOB_SCRIPT_PATH);
    snprintf(cronjob_entry, sizeof(cronjob_entry), "0 0 * * * %s %s %s %s %d %d %d %d %d %d\n", cronjob_script_expanded_path,
        prj->name,
        prj->target_dir,
        prj->cronjob_log_file,
        prj->binaries_limits->weekly_mem_limit,
        prj->binaries_limits->monthly_mem_limit,
        prj->binaries_limits->yearly_mem_limit,
        prj->binaries_limits->weekly_interval,
        prj->binaries_limits->monthly_interval,
        prj->binaries_limits->yearly_interval
    );
    free(cronjob_script_expanded_path);

    // 2. Get existing cron jobs and copy them to a temporary file only if their entries differ from the one we want to add (to avoid duplicates)
    char command_get_cron[MAX_COMMAND_LEN];
    snprintf(command_get_cron, sizeof(command_get_cron), "/usr/bin/crontab -u %s -l", getenv("USER"));
    FILE *cron_pipe = popen(command_get_cron, "r");
    if (!cron_pipe) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to get existing crontab entries: %s", strerror(errno));
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 1;
    }
    
    char temporary_crontab_file[MAX_CONFIG_ATTR_LEN];
    snprintf(temporary_crontab_file, sizeof(temporary_crontab_file), "%s/%s-crontab", prj->main_project_build_dir, prj->name);
    FILE *cron_fp = fopen(temporary_crontab_file, "w");
    if (!cron_fp) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to create temporary cron file %s: %s", temporary_crontab_file, strerror(errno));
        pclose(cron_pipe);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 1;
    }
    char line[MAX_COMMAND_LEN];
    while (fgets(line, sizeof(line), cron_pipe)) {
        if (strcmp(line, cronjob_entry) == 0) {
            // Skip this line since it is identical to the one we want to add
            continue;
        }
        fputs(line, cron_fp);
    }
    fclose(cron_fp);
    pclose(cron_pipe);

    // 3. Now append the new cron job for binaries rotation to the temporary crontab file
    cron_fp = fopen(temporary_crontab_file, "a");
    if (!cron_fp) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Unable to open temporary cron file for appending the new entry: %s", strerror(errno));
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 1;
    }
    fputs(cronjob_entry, cron_fp);
    fclose(cron_fp);

    // 4. Set the new cron tab
    char command[MAX_COMMAND_LEN];
    snprintf(command, sizeof(command), "/usr/bin/crontab -u %s %s", getenv("USER"), temporary_crontab_file);
    int status = system_safe(command);
    if (status != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to set new crontab from %s, system_safe() returned status %d", temporary_crontab_file, status);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        remove(temporary_crontab_file);
        return 1;
    }

    // 5. Release the lock and clean up
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    remove(temporary_crontab_file);

    return 0;
}

static int recovery(project_t *prj, FILE **log_fp, char *main_build_dir) {
    // 1. Create the foundamental directories and files if they don't exist
    int main_build_dir_result = recursive_mkdir_or_file(main_build_dir, 0755, 0);
    if (main_build_dir_result != 0) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to create main build directory at %s: %s", main_build_dir, strerror(errno));
        return 1;
    }
    int worker_build_dir_result = recursive_mkdir_or_file(prj->main_project_build_dir, 0755, 0);
    if (worker_build_dir_result != 0) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to create worker build directory at %s: %s", prj->main_project_build_dir, strerror(errno));
        return 1;
    }
    int worker_log_file_result = recursive_mkdir_or_file(prj->worker_log_file, 0755, 1);
    if (worker_log_file_result != 0) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to create worker log file at %s: %s", prj->worker_log_file, strerror(errno));
        return 1;
    }
    // Substitute created log file pointer (if the whole build directory was missing, the log file pointer was invalid too; maybe all the logs were lost)
    FILE *new_log_fp = fopen(prj->worker_log_file, "a");
    if (!new_log_fp) {
        fprintf(stderr, "Unable to open log file at %s: %s\n", prj->worker_log_file, strerror(errno));
        return 1;
    }
    if (*log_fp) {
        fclose(*log_fp);
    }
    *log_fp = new_log_fp;
    setvbuf(*log_fp, NULL, _IOLBF, 0);
    formatted_log(*log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Created fundamental directories and files for project %s.", prj->name);

    // 2. For each architecture, perform the chroot setup if the chroot is missing
    for (int i = 0; i < prj->arch_count; i++) {
        if (terminate_worker_flag) {
            formatted_log(*log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Termination signal received before starting chroot setup, exiting...");
            break;
        }
        char chroot_dir[MAX_CONFIG_ATTR_LEN];
        snprintf(chroot_dir, sizeof(chroot_dir), "%s/%s-chroot", main_build_dir, prj->architectures[i]);
        formatted_log(*log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Setting up chroot at %s for architecture %s if missing...", chroot_dir, prj->architectures[i]);
        if (chroot_setup(prj->architectures[i], chroot_dir, prj->worker_log_file, *log_fp) != 0) {
            formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Failed to set up chroot for architecture %s.", prj->architectures[i]);
            return 1;
        }
    }

    if (terminate_worker_flag) {
        return 2;
    } else {
        formatted_log(*log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Recovery operations completed successfully for project %s.", prj->name);
    }

    return 0;
}

static int handle_recovery(FILE **log_fp, project_t *prj, char *main_build_dir) {
    // lock a recovery state file globally (on /tmp) to avoid multiple recoveries at the same time (each project could attempt to setup the same chroot at the same time)
    char recovery_state_file_path[MAX_CONFIG_ATTR_LEN];
    snprintf(recovery_state_file_path, sizeof(recovery_state_file_path), "/tmp/v2ci_worker_recovery_state.lock");
    int fd = open(recovery_state_file_path, O_CREAT | O_RDWR, 0644);
    if (fd == -1) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to open recovery state file: %s", strerror(errno));
        return 1;
    }
    if (flock(fd, LOCK_EX) == -1) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to acquire lock on recovery state file: %s", strerror(errno));
        close(fd);
        return 1;
    }

    // Start recovery operations
    formatted_log(*log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Starting recovery operations...");
    int recovery_result = recovery(prj, log_fp, main_build_dir);
    if (recovery_result == 1) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Recovery operations failed for project %s.", prj->name);
        if (flock(fd, LOCK_UN) == -1) {
            formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to release lock on recovery state file: %s", strerror(errno));
        }
        close(fd);
        return 1;
    } else if (recovery_result == 2) {
        formatted_log(*log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Termination signal received during recovery operations for project %s, exiting...", prj->name);
    } else {
        formatted_log(*log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Recovery operations completed successfully for project %s.", prj->name);
    }

    // Release the lock and clean up
    if (flock(fd, LOCK_UN) == -1) {
        formatted_log(*log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "[Recovery] Unable to release lock on recovery state file: %s", strerror(errno));
    }
    close(fd);
    return 0;
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

    // Initialize pid file path
    char PID_FILE[256];
    snprintf(PID_FILE, sizeof(PID_FILE), "/tmp/%s-worker.pid", prj->name);

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

    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Initial directories setup completed successfully for project %s.", prj->name);

    // Set the final binaries rotation cronjob with crontab
    if (set_binaries_rotation_cronjob(prj, log_fp) != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to set the binaries rotation cronjob for project %s.", prj->name);
        fclose(log_fp);
        return 1;
    }

    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Binaries rotation cronjob set successfully for project %s.", prj->name);

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
        
        if (strcmp(prj->build_mode, "main") == 0 || strcmp(prj->build_mode, "full") == 0) {
            // Check for updates in main repo
            char *main_repo_name = NULL;
            int extract_result = extract_repo_name(prj->repo_url, &main_repo_name);
            if (extract_result != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to extract repository name from URL %s", prj->repo_url);
                sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
                if (terminate_worker_flag) {
                    formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                    break;
                }
                continue;
            }
            while (check_for_updates_inside_chroot(chroot_dir, chroot_build_dir, main_repo_name, worker_tmp_chroot_log_file, log_fp, &need2update, prj->name, prj->architectures[0]) != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to check for updates in main repository; trying recover operations... ");
                while (handle_recovery(&log_fp, prj, main_build_dir) == 1) {
                    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Recovery operations failed; will retry update check after poll interval.");
                    sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
                    if (terminate_worker_flag) {
                        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                        break;
                    }
                }
                if (terminate_worker_flag) {
                    break;
                }
            }
            if (terminate_worker_flag) {
                formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during main repository update check, exiting...");
                break;
            }
            free(main_repo_name);
        }
        if (strcmp(prj->build_mode, "dep") == 0 || (strcmp(prj->build_mode, "full") == 0 && !need2update)) {
            // For each manual dependency, check for updates
            manual_dependency_t *cur_manual = prj->manual_dependencies;
            while (cur_manual) {
                char *dependency_repo_name = NULL;
                int extract_result = extract_repo_name(cur_manual->git_url, &dependency_repo_name);
                if (extract_result != 0) {
                    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to extract repository name from URL %s", cur_manual->git_url);
                    sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
                    if (terminate_worker_flag) {
                        formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                        break;
                    }
                    continue;
                }
                while (check_for_updates_inside_chroot(chroot_dir, chroot_build_dir, dependency_repo_name, worker_tmp_chroot_log_file, log_fp, &need2update, prj->name, prj->architectures[0]) != 0) {
                    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Failed to check for updates in manual dependency %s; trying recover operations... ", cur_manual->git_url);
                    while (handle_recovery(&log_fp, prj, main_build_dir) == 1) {
                        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Recovery operations failed; will retry update check after poll interval.");
                        sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
                        if (terminate_worker_flag) {
                            formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                            break;
                        }
                    }
                    if (terminate_worker_flag) {
                        break;
                    }
                }
                if (terminate_worker_flag) {
                    break;
                }
                if (need2update) {
                    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Update detected in manual dependency %s.", cur_manual->git_url);
                    break;
                }
                cur_manual = cur_manual->next;
                free(dependency_repo_name);
            }
            if (terminate_worker_flag) {
                formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during manual dependency update checks, exiting...");
                break;
            }
        }

        // If no updates were found, sleep for the poll interval and restart the loop
        if (!need2update) {
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "No updates found for project %s. Sleeping for %d seconds.", prj->name, prj->poll_interval);
            sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
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
                sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
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
        int failed_builds = 0;
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
                        failed_builds++;
                    } else {
                        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Thread for architecture %s terminated successfully. Here the stats: %s", args[j].arch, (thread_result->stats ? thread_result->stats : "No stats available"));
                    }
                } else {
                    formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Thread for architecture %s terminated without a specific return value.", args[j].arch);
                }
            }
            free(thread_return_value);
        }
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "All launched build threads (%d out of %d) joined successfully for project %s.", i, prj->arch_count, prj->name);

        // If there were failed builds, attempt recovery and retry
        if (failed_builds > 0) {
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "%d builds failed for project %s. Retrying with recovery...", failed_builds, prj->name);
            while (handle_recovery(&log_fp, prj, main_build_dir) == 1) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, prj->name, NULL, "Recovery operations failed; will retry update check after poll interval.");
                sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
                if (terminate_worker_flag) {
                    formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during wait after error, exiting...");
                    break;
                }
            }
            if (terminate_worker_flag) {
                formatted_log(log_fp, "INTERRUPT", __FILE__, __LINE__, prj->name, NULL, "Termination signal received during recovery handling after failed builds, exiting...");
                break;
            }
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Recovery operations completed successfully for project %s. Restarting builds...", prj->name);
            continue;
        } else {
            formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "All builds completed successfully for project %s.", prj->name);
        }
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Your final binaries (for the successful builds) are located in %s for each architecture.", prj->target_dir);

        // Sleep for the poll interval before the next iteration
        formatted_log(log_fp, "INFO", __FILE__, __LINE__, prj->name, NULL, "Sleeping for %d seconds before the next check.", prj->poll_interval);
        sleep_and_handle_interrupts(prj->poll_interval, log_fp, prj->name);
    }

    // Cleanup
    remove(PID_FILE);
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