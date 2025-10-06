#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <execs.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "utils/scripts_runner.h"
#include "utils/utils.h"

int chroot_setup(const char *debian_arch, const char *chroot_dir, const char* main_log_file, FILE *log_fp) {
    char command[MAX_COMMAND_LEN];
    const char *chroot_setup_expanded_path = expand_tilde(CHROOT_SETUP_SCRIPT_PATH);
    if (chmod((char *)chroot_setup_expanded_path, 0755) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, debian_arch, "Error: Unable to set execute permissions on %s: %s", chroot_setup_expanded_path, strerror(errno));
        return 1;
    }
    snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\"", chroot_setup_expanded_path, debian_arch, chroot_dir, main_log_file);
    int status = system_safe(command);
    if (status == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, debian_arch, "system_safe() call during the execution of %s failed for architecture %s", chroot_setup_expanded_path, debian_arch);
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, NULL, debian_arch, "script %s for architecture %s did not terminate normally; status: %d", chroot_setup_expanded_path, debian_arch, status);
    return 1;
}

int check_for_updates_inside_chroot(const char *chroot_dir, const char *chroot_build_dir, const char *repo_name, const char *worker_tmp_chroot_log_file, FILE *log_fp, int *need2update, const char *project_name, const char *tmp_arch) {
    char command[MAX_COMMAND_LEN];
    const char *check_updates_expanded_path = expand_tilde(CHECK_UPDATES_SCRIPT_PATH);
    if (chmod((char *)check_updates_expanded_path, 0755) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, NULL, "Error: Unable to set execute permissions on %s: %s", check_updates_expanded_path, strerror(errno));
        return 1;
    }
    snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", check_updates_expanded_path, chroot_dir, chroot_build_dir, repo_name, worker_tmp_chroot_log_file, project_name, tmp_arch);
    int status = system_safe(command);
    if (status == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, tmp_arch, "system_safe() call during the execution of %s failed for repository %s, in the tmp chroot arch %s", check_updates_expanded_path, repo_name, tmp_arch);
        return 1;
    }
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            *need2update = 0; // No updates
        } else if (exit_code == 2) {
            *need2update = 1; // Updates found
        } else {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, tmp_arch, "script %s for repository %s , in the tmp chroot arch %s, failed with code %d", check_updates_expanded_path, repo_name, tmp_arch, exit_code);
            return 1;
        }
        return 0;
    }
    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, tmp_arch, "script %s for repository %s , in the tmp chroot arch %s, did not terminate normally; status: %d", check_updates_expanded_path, repo_name, tmp_arch, status);
    return 1;
}

int install_packages_list_in_chroot(char *packages[], const char *chroot_dir, FILE *log_fp, const char *thread_log_file, const char *project_name, const char *thread_arch) {
    char command[MAX_COMMAND_LEN];
    const char *install_packages_expanded_path = expand_tilde(INSTALL_PACKAGES_SCRIPT_PATH);
    if (chmod((char *)install_packages_expanded_path, 0755) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "Error: Unable to set execute permissions on %s: %s", install_packages_expanded_path, strerror(errno));
        return 1;
    }
    snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\"", install_packages_expanded_path, chroot_dir, thread_log_file, project_name, thread_arch);
    for (int i = 0; packages[i] != NULL; i++) {
        strcat(command, " \"");
        strcat(command, packages[i]);
        strcat(command, "\"");
    }
    int status = system_safe(command);
    if (status == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "system_safe() call during the execution of %s failed for packages in chroot", install_packages_expanded_path);
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    formatted_log(log_fp, "ERROR", __FILE__, __LINE__, project_name, thread_arch, "script %s for packages installation did not terminate normally; status: %d", install_packages_expanded_path, status);
    return 1;
}

int clone_or_pull_sources_inside_chroot(thread_arg_t *targ, FILE *log_fp) {
    // Extract repository names from URLs (manual dependencies and main project) - (useful for checking if the repos were already cloned)
    char *repo_names[targ->project->manual_dep_count + 1];
    manual_dependency_t *cur_manual = targ->project->manual_dependencies;
    int i = 0;
    while (cur_manual) {
        if (extract_repo_name(cur_manual->git_url, &repo_names[i]) != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Failed to extract repository name from URL %s for project %s", cur_manual->git_url, targ->project->name);
            for (int j = 0; j < i; j++) free(repo_names[j]);
            return 1;
        }
        cur_manual = cur_manual->next;
        i++;
    }
    if (extract_repo_name(targ->project->repo_url, &repo_names[i]) != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Failed to extract main repository name from URL %s for project %s", targ->project->repo_url, targ->project->name);
        for (int j = 0; j < i; j++) free(repo_names[j]);
        return 1;
    }

    // First clone or pull all the manual dependencies
    char command[MAX_COMMAND_LEN];
    cur_manual = targ->project->manual_dependencies;
    const char *clone_or_pull_expanded_path = expand_tilde(CLONE_OR_PULL_SCRIPT_PATH);
    if (chmod((char *)clone_or_pull_expanded_path, 0755) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Error: Unable to set execute permissions on %s: %s", clone_or_pull_expanded_path, strerror(errno));
        for (int j = 0; j < i; j++) free(repo_names[j]);
        return 1;
    }
    i = 0;
    while (cur_manual) {
        snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", 
            clone_or_pull_expanded_path, 
            targ->thread_chroot_dir, 
            targ->thread_chroot_build_dir, 
            repo_names[i], 
            cur_manual->git_url,
            targ->thread_log_file,
            targ->project->name,
            targ->arch
        );
        int status = system_safe(command);
        if (status == -1) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "system_safe() call during the execution of %s failed for project %s during the clone of the dependency %s", clone_or_pull_expanded_path, targ->project->name, repo_names[i]);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        } else if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the clone of the dependency %s exited with failure code %d", clone_or_pull_expanded_path, targ->project->name, repo_names[i], exit_code);
                for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                    if (repo_names[k]) free(repo_names[k]);
                }
                return 1;
            }
        } else {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the clone of the dependency %s did not terminate normally; status: %d", clone_or_pull_expanded_path, targ->project->name, repo_names[i], status);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        }
        cur_manual = cur_manual->next;
        i++;
    }

    // Now clone or pull the main project repository
    snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", 
        clone_or_pull_expanded_path, 
        targ->thread_chroot_dir,
        targ->thread_chroot_build_dir,
        repo_names[i], 
        targ->project->repo_url,
        targ->thread_log_file,
        targ->project->name,
        targ->arch
    );
    int status = system_safe(command);
    if (status == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "system_safe() call during the execution of %s failed for project %s during the clone of the main repository", clone_or_pull_expanded_path, targ->project->name);
        for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
            if (repo_names[k]) free(repo_names[k]);
        }
        return 1;
    } else if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the clone of the main repository exited with code %d", clone_or_pull_expanded_path, targ->project->name, exit_code);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        }
    } else {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the clone of the main repository did not terminate normally; status: %d", clone_or_pull_expanded_path, targ->project->name, status);
        for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
            if (repo_names[k]) free(repo_names[k]);
        }
        return 1;
    }
    for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
        if (repo_names[k]) free(repo_names[k]);
    }
    return 0;
}

int build_in_chroot(thread_arg_t *targ, FILE *log_fp) {
    // Extract repository names from URLs (manual dependencies and main project) - (useful for cd for each repo)
    char *repo_names[targ->project->manual_dep_count + 1];
    manual_dependency_t *cur_manual = targ->project->manual_dependencies;
    int i = 0;
    while (cur_manual) {
        if (extract_repo_name(cur_manual->git_url, &repo_names[i]) != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Failed to extract repository name from URL %s for project %s", cur_manual->git_url, targ->project->name);
            for (int j = 0; j < i; j++) free(repo_names[j]);
            return 1;
        }
        cur_manual = cur_manual->next;
        i++;
    }
    if (extract_repo_name(targ->project->repo_url, &repo_names[i]) != 0) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Failed to extract main repository name from URL %s for project %s", targ->project->repo_url, targ->project->name);
        for (int j = 0; j < i; j++) free(repo_names[j]);
        return 1;
    }

    // First build all the dependencies
    char command[MAX_COMMAND_LEN];
    cur_manual = targ->project->manual_dependencies;
    const char *build_script_expanded_path = expand_tilde(BUILD_SCRIPT_PATH);
    if (chmod((char *)build_script_expanded_path, 0755) == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "Unable to set execute permissions on %s: %s", build_script_expanded_path, strerror(errno));
        for (int j = 0; j < i; j++) free(repo_names[j]);
        return 1;
    }
    i = 0;
    while (cur_manual) {
        snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", 
            build_script_expanded_path, 
            targ->arch,
            targ->thread_chroot_dir, 
            targ->thread_chroot_build_dir, 
            repo_names[i],
            cur_manual->build_system,
            targ->thread_log_file, 
            targ->thread_chroot_log_file,
            targ->project->name
        );
        int status = system_safe(command);
        if (status == -1) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "system_safe() call during the execution of %s failed for project %s during the build of the dependency %s", build_script_expanded_path, targ->project->name, repo_names[i]);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        } else if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code != 0) {
                formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the build of the dependency %s exited with failure code %d", build_script_expanded_path, targ->project->name, repo_names[i], exit_code);
                for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                    if (repo_names[k]) free(repo_names[k]);
                }
                return 1;
            }
        } else {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the build of the dependency %s did not terminate normally; status: %d", build_script_expanded_path, targ->project->name, repo_names[i], status);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        }
        cur_manual = cur_manual->next;
        i++;
    }

    // Now build the main project repository
    snprintf(command, sizeof(command), "%s \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"", 
        build_script_expanded_path, 
        targ->arch,
        targ->thread_chroot_dir,
        targ->thread_chroot_build_dir,
        repo_names[i],
        targ->project->main_repo_build_system,
        targ->thread_log_file,
        targ->thread_chroot_log_file,
        targ->project->name,
        targ->thread_chroot_target_dir,
        targ->project->target_dir
    );
    int status = system_safe(command);
    if (status == -1) {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "system_safe() call during the execution of %s failed for project %s during the build of the main repository", build_script_expanded_path, targ->project->name);
        for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
            if (repo_names[k]) free(repo_names[k]);
        }
        return 1;
    } else if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the build of the main repository exited with code %d", build_script_expanded_path, targ->project->name, exit_code);
            for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
                if (repo_names[k]) free(repo_names[k]);
            }
            return 1;
        }
    } else {
        formatted_log(log_fp, "ERROR", __FILE__, __LINE__, targ->project->name, targ->arch, "script %s for project %s during the build of the main repository did not terminate normally; status: %d", build_script_expanded_path, targ->project->name, status);
        for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
            if (repo_names[k]) free(repo_names[k]);
        }
        return 1;
    }
    for (int k = 0; k < (targ->project->manual_dep_count + 1); k++) {
        if (repo_names[k]) free(repo_names[k]);
    }
    return 0;
}