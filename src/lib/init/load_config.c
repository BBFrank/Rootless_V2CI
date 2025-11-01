#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils/utils.h"
#include "init/load_config.h"
#include <yaml.h>

/*
    This module is responsible for loading and parsing the configuration file for the V2CI project.
    It reads the YAML configuration file through libyaml, extracts project settings, and populates the Config structure.
    Libyaml provides a robust way to handle YAML files in C, allowing for efficient parsing and error handling.
    In particular, every time the libyaml engine reads something from the file, it generates an event (YAML_EVENT):
    - YAML_STREAM_START_EVENT: Indicates the start of the YAML stream (it happens at the beginning of the file)
    - YAML_DOCUMENT_START_EVENT: Indicates the start of a document (it happens at the beginning of the YAML content)
    - YAML_SCALAR_EVENT: Represents a scalar value (string, number, etc. e.g., "build_dir", "projects", "name", etc.)
    - YAML_MAPPING_START_EVENT: Indicates the start of a mapping (it happens every time a new section with ":" and a subsequent indentation is found. The subsequent indented block could also start with a "-"; in that case before the YAML_MAPPING_START_EVENT there will be a YAML_SEQUENCE_START_EVENT)
    - YAML_MAPPING_END_EVENT: Indicates the end of a mapping (it happens every time a section ends)
    - YAML_SEQUENCE_START_EVENT: Indicates the start of a sequence (it happens every time a list starts with "-")
    - YAML_SEQUENCE_END_EVENT: Indicates the end of a sequence (it happens every time a list ends)
    - YAML_DOCUMENT_END_EVENT: Indicates the end of a document (it happens at the end of the YAML content)
    - YAML_STREAM_END_EVENT: Indicates the end of the YAML stream (it happens at the end of the file)
*/

#define DEFAULT_BUILD_MODE "full"
#define DEFAULT_POLL_INTERVAL 180
#define DEFAULT_DAILY_MEM_LIMIT 10000       // 10 MB
#define DEFAULT_WEEKLY_MEM_LIMIT 50000      // 50 MB
#define DEFAULT_MONTHLY_MEM_LIMIT 200000    // 200 MB
#define DEFAULT_YEARLY_MEM_LIMIT 1000000    // 1 GB
#define DEFAULT_WEEKLY_INTERVAL 1440
#define DEFAULT_MONTHLY_INTERVAL 10080
#define DEFAULT_YEARLY_INTERVAL 43200

static void set_default_binaries_limits(binaries_limits_for_project_t *limits) {
    if (!limits) return;
    limits->daily_mem_limit = DEFAULT_DAILY_MEM_LIMIT;
    limits->weekly_mem_limit = DEFAULT_WEEKLY_MEM_LIMIT;
    limits->monthly_mem_limit = DEFAULT_MONTHLY_MEM_LIMIT;
    limits->yearly_mem_limit = DEFAULT_YEARLY_MEM_LIMIT;
    limits->weekly_interval = DEFAULT_WEEKLY_INTERVAL;
    limits->monthly_interval = DEFAULT_MONTHLY_INTERVAL;
    limits->yearly_interval = DEFAULT_YEARLY_INTERVAL;
}

static int ensure_default_binaries_limits(project_t *prj) {
    if (!prj) return 1;
    if (!prj->binaries_limits) {
        prj->binaries_limits = calloc(1, sizeof(binaries_limits_for_project_t));
        if (!prj->binaries_limits) {
            fprintf(stderr, "Error: Memory allocation failed (binaries_limits)\n");
            return 1;
        }
        set_default_binaries_limits(prj->binaries_limits);
    }
    return 0;
}

static int add_string(char *array[], int *count, int max, const char *val) {
    if (*count >= max) return 1;
    array[*count] = strdup(val);
    if (array[*count]) {
        (*count)++;
        return 0;
    }
    return 1;
}

static int ensure_default_architectures(project_t *prj) {
    if (!prj || prj->arch_count > 0) return 0;
    const char *defaults[] = {"amd64", "arm64", "armhf", "riscv64"};
    int add_result = 0;
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        add_result = add_string(prj->architectures, &prj->arch_count, MAX_ARCHITECTURES, defaults[i]);
        if (add_result != 0) {
            fprintf(stderr, "Error: Failed to add default architecture '%s'\n", defaults[i]);
            return 1;
        }
    }
    return 0;
}

static int load_project(project_t *prj, yaml_parser_t *parser) {

    typedef enum { SEC_NONE, SEC_BINARIES_CFG, SEC_BIN_INTERVAL, SEC_BIN_MEM, SEC_SOURCE, SEC_MAIN_REPO, SEC_DEP_REPO_ITEM, SEC_BUILD_CFG } Section;
    typedef enum { SEQ_NONE, SEQ_DEPS, SEQ_DEP_REPOS, SEQ_ARCH } ActiveSeq;

    Section section = SEC_NONE;
    ActiveSeq seq = SEQ_NONE;
    char last_key[128] = {0};

    manual_dependency_t *cur_manual = NULL;

    int depth = 1;
    yaml_event_t ev;

    // Set defaults values
    if (ensure_default_architectures(prj) != 0) return 1;
    if (ensure_default_binaries_limits(prj) != 0) return 1;
    snprintf(prj->build_mode, sizeof(prj->build_mode), "%s", DEFAULT_BUILD_MODE);
    prj->poll_interval = DEFAULT_POLL_INTERVAL;

    int add_result = 0;

    while (depth > 0) {
        if (!yaml_parser_parse(parser, &ev)) {
            fprintf(stderr, "Error: Failed to parse YAML\n");
            return 1;
        }
        switch (ev.type) {
            case YAML_SCALAR_EVENT:
                // Handle scalar events: they can be either keys or values (always strings)
                const char *val = (const char*)ev.data.scalar.value;
                // Always read the key of a scalar event before processing (the first time we read something it must be a key)
                if (!last_key[0]) {
                    snprintf(last_key, sizeof(last_key), "%s", val);
                }
                // General case: we are now reading the value corresponding to last_key 
                else {
                    // First events: name and target dir
                    if (strcmp(last_key, "name") == 0) {
                        snprintf(prj->name, sizeof(prj->name), "%s", val);
                        last_key[0] = '\0';
                    } else if (strcmp(last_key, "target_dir") == 0) {
                        snprintf(prj->target_dir, sizeof(prj->target_dir), "%s", val);
                        last_key[0] = '\0';
                    }
                    // General case 1: we are in a section and we're reading a key-value pair (we always need to reset the last key here)
                    else if (section == SEC_BIN_INTERVAL) {
                        if (strcmp(last_key, "weekly") == 0) prj->binaries_limits->weekly_interval = atoi(val);
                        else if (strcmp(last_key, "monthly") == 0) prj->binaries_limits->monthly_interval = atoi(val);
                        else if (strcmp(last_key, "yearly") == 0) prj->binaries_limits->yearly_interval = atoi(val);
                        last_key[0] = '\0';
                    } else if (section == SEC_BIN_MEM) {
                        if (strcmp(last_key, "daily") == 0) prj->binaries_limits->daily_mem_limit = atoi(val);
                        else if (strcmp(last_key, "weekly") == 0) prj->binaries_limits->weekly_mem_limit = atoi(val);
                        else if (strcmp(last_key, "monthly") == 0) prj->binaries_limits->monthly_mem_limit = atoi(val);
                        else if (strcmp(last_key, "yearly") == 0) prj->binaries_limits->yearly_mem_limit = atoi(val);
                        last_key[0] = '\0';
                    } else if (section == SEC_MAIN_REPO) {
                        if (strcmp(last_key, "git_url") == 0) snprintf(prj->repo_url, sizeof(prj->repo_url), "%s", val);
                        else if (strcmp(last_key, "build_system") == 0) snprintf(prj->main_repo_build_system, sizeof(prj->main_repo_build_system), "%s", val);
                        last_key[0] = '\0';
                    // Here I have to specify that I'm in a dependency_repos sequence since it can also happen that I'm in SEC_DEP_REPO_ITEM but I've just started the inner SEQ_DEPS (packages of a manual dependency). In that case
                    // I only have to process the scalar events adding the dependencies to the current manual dependency.
                    // So this control ensures that I'm processing the key-value pairs of the dependency_repos items (no inner lists, active sequence corresponds to the outer list of SEQ_DEP_REPOS)
                    } else if (section == SEC_DEP_REPO_ITEM && seq == SEQ_DEP_REPOS) {
                        if (strcmp(last_key, "git_url") == 0 && cur_manual) snprintf(cur_manual->git_url, sizeof(cur_manual->git_url), "%s", val);
                        else if (strcmp(last_key, "build_system") == 0 && cur_manual) snprintf(cur_manual->build_system, sizeof(cur_manual->build_system), "%s", val);
                        last_key[0] = '\0';
                    } else if (section == SEC_BUILD_CFG) {
                        if (strcmp(last_key, "build_mode") == 0) snprintf(prj->build_mode, sizeof(prj->build_mode), "%s", val);
                        else if (strcmp(last_key, "poll_interval") == 0) prj->poll_interval = atoi(val);
                        last_key[0] = '\0';
                    }
                    // General case 2: we received a scalar event due to a string-only list entry of a sequence (so we must be in a sequence). Here we mustn't reset last_key because the next scalar event will be a new value (if I reset it here, I will lose the context and read it as a key instead of a value)
                    else if (seq == SEQ_DEPS)  {
                        // If we are in source.dependencies add to project, if we are in dependency_repos[i].dependencies add to current manual dependency
                        if (section == SEC_SOURCE) {
                            add_result = add_string(prj->dependency_packages, &prj->dep_count, MAX_DEPENDENCIES, val);
                        } else if (section == SEC_DEP_REPO_ITEM && cur_manual) {
                            add_result = add_string(cur_manual->dependencies, &cur_manual->dep_count, MAX_DEPENDENCIES, val);
                        }
                        if (add_result != 0) {
                            fprintf(stderr, "Error: Failed to add dependency '%s'\n", val);
                            yaml_event_delete(&ev);
                            return 1;
                        }
                    } else if (seq == SEQ_ARCH) {
                        add_result = add_string(prj->architectures, &prj->arch_count, MAX_ARCHITECTURES, val);
                        if (add_result != 0) {
                            fprintf(stderr, "Error: Failed to add architecture '%s'\n", val);
                            yaml_event_delete(&ev);
                            return 1;
                        }
                    }
                }
                break;
            case YAML_MAPPING_START_EVENT:
                // Handle start of mapping events: increase depth and change section
                depth++;
                if (strcmp(last_key, "binaries-config") == 0 && section == SEC_NONE) {
                    section = SEC_BINARIES_CFG;
                } else if (strcmp(last_key, "interval") == 0 && section == SEC_BINARIES_CFG) {
                    section = SEC_BIN_INTERVAL;
                } else if (strcmp(last_key, "mem-limit") == 0 && section == SEC_BINARIES_CFG) {
                    section = SEC_BIN_MEM;
                } else if (strcmp(last_key, "source") == 0 && section == SEC_NONE) {
                    section = SEC_SOURCE;
                } else if (strcmp(last_key, "main_repo") == 0 && section == SEC_SOURCE) {
                    section = SEC_MAIN_REPO;
                } else if (seq == SEQ_DEP_REPOS && section == SEC_SOURCE) {
                    // If I receive a mapping start event due to a new item in dependency_repos list, I also have to create a new manual_dependency_t node and insert it in the linked list
                    section = SEC_DEP_REPO_ITEM;
                    manual_dependency_t *node = (manual_dependency_t*)calloc(1, sizeof(manual_dependency_t));
                    if (!node) {
                        fprintf(stderr, "Error: Memory allocation failed (manual_dependency)\n");
                        yaml_event_delete(&ev);
                        return 1;
                    }
                    if (!prj->manual_dependencies) {
                        prj->manual_dependencies = node;
                    } else {
                        if (!cur_manual) {
                            cur_manual = prj->manual_dependencies;
                            while (cur_manual->next) cur_manual = cur_manual->next;
                        }
                        cur_manual->next = node;
                    }
                    cur_manual = node;
                    prj->manual_dep_count++;
                } else if (strcmp(last_key, "build-config") == 0 && section == SEC_NONE) {
                    section = SEC_BUILD_CFG;
                }
                // Reset last_key: this operation is necessary because after a mapping start event we always expect a key next and we probably just read a key before
                last_key[0] = '\0';
                break;
            case YAML_MAPPING_END_EVENT:
                // Handle end of mapping events: restore the previous section and decrease depth
                depth--;
                if (section == SEC_BINARIES_CFG) section = SEC_NONE;
                else if (section == SEC_BIN_INTERVAL) section = SEC_BINARIES_CFG;
                else if (section == SEC_BIN_MEM) section = SEC_BINARIES_CFG;
                else if (section == SEC_MAIN_REPO) section = SEC_SOURCE;
                else if (section == SEC_DEP_REPO_ITEM) section = SEC_SOURCE;
                else if (section == SEC_BUILD_CFG) section = SEC_NONE;
                break;
            case YAML_SEQUENCE_START_EVENT:
                // Handle start of sequence events: increase depth and set sequence type
                depth++;
                // Support source.dependencies and dependency_repos[i].dependencies
                if (strcmp(last_key, "dependencies") == 0 && (section == SEC_SOURCE || section == SEC_DEP_REPO_ITEM)) {
                    // Here I don't have to reset last_key because the next scalar event will be a value in the list
                    seq = SEQ_DEPS;
                } else if (strcmp(last_key, "dependency_repos") == 0 && section == SEC_SOURCE) {
                    // Here I don't have to reset last_key because the next event will be a mapping start event for the first item in the list, and the mapping start case already resets last_key
                    seq = SEQ_DEP_REPOS;
                }
                break;
            case YAML_SEQUENCE_END_EVENT:
                // Handle end of sequence events: restore the previous sequence type and decrease depth
                depth--;
                if (seq == SEQ_DEP_REPOS) cur_manual = NULL;
                seq = SEQ_NONE;
                // Reset last_key: after a sequence ends we always expect a new key next
                // Note: if we came from the sequence end of SEQ_DEP_REPOS, the last_key was already reset in the mapping end event of the last item; if we came from SEQ_DEPS instead, we need to reset it here
                last_key[0] = '\0';
                break;
            default:
                break;
        }
        yaml_event_delete(&ev);
    }
    return 0;
}

int load_config(Config *cfg) {
    if (!cfg) return 1;

    memset(cfg, 0, sizeof(Config));

    FILE *config_file = fopen_expanding_tilde(DEFAULT_CONFIG_PATH, "rb");
    if (!config_file) {
        fprintf(stderr, "Error: Unable to open config file at %s\n", DEFAULT_CONFIG_PATH);
        return 1;
    }

    yaml_parser_t parser;
    yaml_event_t ev;
    if (!yaml_parser_initialize(&parser)) {
        fclose(config_file);
        fprintf(stderr, "Error: Failed to initialize YAML parser\n");
        return 1;
    }
    yaml_parser_set_input_file(&parser, config_file);

    int done = 0;
    int project_loaded = 0;
    int in_projects = 0;
    char top_last_key[128] = {0};
    project_t *tail = NULL;

    while (!done) {
        if (!yaml_parser_parse(&parser, &ev)) {
            fprintf(stderr, "Error: Failed to parse YAML\n");
            yaml_parser_delete(&parser);
            fclose(config_file);
            return 1;
        }
        switch (ev.type) {
            case YAML_SCALAR_EVENT:
                const char *val = (const char*)ev.data.scalar.value;
                if (!in_projects) {
                    if (!top_last_key[0]) {
                        snprintf(top_last_key, sizeof(top_last_key), "%s", val);
                    } else {
                        if (strcmp(top_last_key, "build_dir") == 0) {
                            snprintf(cfg->build_dir, sizeof(cfg->build_dir), "%s", val);
                            snprintf(cfg->main_log_file, sizeof(cfg->main_log_file), "%s/logs/main.log", cfg->build_dir);
                        }
                        top_last_key[0] = '\0';
                    }
                }
                break;
            case YAML_SEQUENCE_START_EVENT:
                if (strcmp(top_last_key, "projects") == 0) {
                    in_projects = 1;
                    top_last_key[0] = '\0';
                }
                break;
            case YAML_MAPPING_START_EVENT:
                if (in_projects) {
                    project_t *prj = (project_t*)calloc(1, sizeof(project_t));
                    if (!prj) {
                        fprintf(stderr, "Error: Memory allocation failed\n");
                        yaml_parser_delete(&parser);
                        yaml_event_delete(&ev);
                        fclose(config_file);
                        return 1;
                    }
                    if (!cfg->projects) {
                        cfg->projects = prj;
                    } else {
                        tail->next = prj;
                    }
                    tail = prj;

                    if (load_project(prj, &parser) == 0) {
                        project_loaded++;
                    } else {
                        fprintf(stderr, "Error: Failed to load project configuration for project %d\n", project_loaded + 1);
                        yaml_parser_delete(&parser);
                        yaml_event_delete(&ev);
                        fclose(config_file);
                        return 1;
                    }

                    // Add paths required by the worker responsible for building this project
                    snprintf(prj->main_project_build_dir, sizeof(prj->main_project_build_dir), "%s/%s", cfg->build_dir, prj->name);
                    snprintf(prj->worker_log_file, sizeof(prj->worker_log_file), "%s/logs/worker.log", prj->main_project_build_dir);
                    snprintf(prj->cronjob_log_file, sizeof(prj->cronjob_log_file), "%s/logs/binaries_rotation_cronjob.log", prj->main_project_build_dir);
                }
                break;
            case YAML_SEQUENCE_END_EVENT:
                if (in_projects) in_projects = 0;
                break;
            case YAML_STREAM_END_EVENT:
                done = 1;
                break;
            default:
                break;
        }
        yaml_event_delete(&ev);
    }
    yaml_parser_delete(&parser);
    fclose(config_file);

    if (project_loaded == 0) {
        fprintf(stderr, "Error: No projects found in configuration file\n");
        return 1;
    } else {
        printf("Loaded %d project(s) from configuration file.\n", project_loaded);
        cfg->project_count = project_loaded;
    }
    return 0;
}