#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "init/load_config.h"

int main() {
    // 0. Try to kil the main process if it still exists (/tmp/rootless_v2ci.pid)
    const char *MAIN_PID_FILE = "/tmp/rootless_v2ci.pid";
    if (access(MAIN_PID_FILE, F_OK) == 0) {
        FILE *fp = fopen(MAIN_PID_FILE, "r");
        if (fp) {
            pid_t main_pid;
            if (fscanf(fp, "%d", &main_pid) == 1) {
                if (kill(main_pid, SIGTERM) == 0) {
                    printf("Sent termination signal to main v2ci process (PID: %d).\n", main_pid);
                } else {
                    fprintf(stderr, "Failed to stop main v2ci process (PID: %d). Error: %s\n", main_pid, strerror(errno));
                }
            }
            fclose(fp);
        }
    }

    // 1. Load the config file variables (in order to build the projects' pid files paths)
    Config cfg;
    if(load_config(&cfg) != 0) {
        fprintf(stderr, "Failed to load configuration variables during stop process. Exiting.\n");
        return 1;
    }

    // 2. Iterate over all projects and send SIGTERM to their worker processes (if running)
    project_t *current = cfg.projects;
    int stopped_projects = 0;
    for (int i = 0; i < cfg.project_count; i++) {
        if (!current) {
            break;
        }

        // 3. Check if the PID file exists
        char PID_FILE[256];
        snprintf(PID_FILE, sizeof(PID_FILE), "/tmp/%s-worker.pid", current->name);
        if (access(PID_FILE, F_OK) == 0) {
            // 4. Read the PID from the file
            FILE *fp = fopen(PID_FILE, "r");
            if (fp) {
                pid_t pid;
                if (fscanf(fp, "%d", &pid) == 1) {
                    // 5. Send SIGTERM to the process
                    if (kill(pid, SIGTERM) == 0) {
                        stopped_projects++;
                        printf("Sent termination signal to project %s (PID: %d).\n", current->name, pid);
                    } else {
                        fprintf(stderr, "Failed to stop project %s (PID: %d). Error: %s\n", current->name, pid, strerror(errno));
                    }
                }
                fclose(fp);
            }
        }
        current = current->next;
    }

    // 6. Print the summary
    if (stopped_projects > 0) {
        printf("Successfully stopped %d project(s).\n", stopped_projects);
    } else {
        printf("No projects were stopped.\n");
    }

    // 7. Free allocated memory for projects (for each project, free its manual dependencies and the project itself)
    current = cfg.projects;
    while (current) {
        manual_dependency_t *cur_manual = current->manual_dependencies;
        while (cur_manual) {
            manual_dependency_t *next_manual = cur_manual->next;
            free(cur_manual);
            cur_manual = next_manual;
        }
        project_t *next_project = current->next;
        free(current);
        current = next_project;
    }

    return 0;
}
