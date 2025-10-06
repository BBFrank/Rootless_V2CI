#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils/utils.h"
#include "init/load_config.h"
#include <yaml.h>

static void add_string(char *array[], int *count, int max, const char *val) {
    if (*count >= max) return;
    array[*count] = strdup(val);
    if (array[*count]) (*count)++;
}

static int load_project(project_t *prj, yaml_parser_t *parser) {

    typedef enum { SEC_NONE, SEC_GIT, SEC_BUILD_CFG, SEC_SOURCE, SEC_MAIN_REPO, SEC_DEP_REPO_ITEM } Section;
    typedef enum { SEQ_NONE, SEQ_ARCH, SEQ_DEPS, SEQ_DEP_REPOS } ActiveSeq;

    Section section = SEC_NONE;
    ActiveSeq seq = SEQ_NONE;
    char last_key[128] = {0};

    manual_dependency_t *cur_manual = NULL;
    manual_dependency_t *manual_tail = prj->manual_dependencies;

    int depth = 1;
    yaml_event_t ev;

    while (depth > 0) {
        if (!yaml_parser_parse(parser, &ev)) {
            fprintf(stderr, "Error: Failed to parse YAML\n");
            return 1;
        }
        switch (ev.type) {
            case YAML_SCALAR_EVENT:
                const char *val = (const char*)ev.data.scalar.value;
                if (seq == SEQ_ARCH) {
                    add_string(prj->architectures, &prj->arch_count, MAX_ARCHITECTURES, val);
                    break;
                }
                if (seq == SEQ_DEPS)  {
                    // If we are in source.dependencies add to project, if we are in dependency_repos[i].dependencies add to current manual dependency
                    if (section == SEC_SOURCE) {
                        add_string(prj->dependency_packages, &prj->dep_count, MAX_DEPENDENCIES, val);
                    } else if (section == SEC_DEP_REPO_ITEM && cur_manual) {
                        add_string(cur_manual->dependencies, &cur_manual->dep_count, MAX_DEPENDENCIES, val);
                    }
                    break;
                }
                if (section == SEC_DEP_REPO_ITEM) {
                    if (!last_key[0]) {
                        snprintf(last_key, sizeof(last_key), "%s", val);
                    } else {
                        if (cur_manual) {
                            if (strcmp(last_key, "git_url") == 0) snprintf(cur_manual->git_url, sizeof(cur_manual->git_url), "%s", val);
                            else if (strcmp(last_key, "build_system") == 0) snprintf(cur_manual->build_system, sizeof(cur_manual->build_system), "%s", val);
                        }
                        last_key[0] = '\0';
                    }
                    break;
                }
                if (!last_key[0]) {
                    snprintf(last_key, sizeof(last_key), "%s", val);
                }
                else {
                    if (strcmp(last_key, "name") == 0) {
                        snprintf(prj->name, sizeof(prj->name), "%s", val);
                    } else if (section == SEC_NONE && strcmp(last_key, "target_dir") == 0) {
                        snprintf(prj->target_dir, sizeof(prj->target_dir), "%s", val);
                    } else if (section == SEC_GIT) {
                        if (strcmp(last_key, "repo_url") == 0) snprintf(prj->repo_url, sizeof(prj->repo_url), "%s", val);
                    } else if (section == SEC_BUILD_CFG) {
                        if (strcmp(last_key, "build_mode") == 0) snprintf(prj->build_mode, sizeof(prj->build_mode), "%s", val);
                        else if (strcmp(last_key, "poll_interval") == 0) prj->poll_interval = atoi(val);
                    } else if (section == SEC_MAIN_REPO) {
                        if (strcmp(last_key, "git_url") == 0) snprintf(prj->repo_url, sizeof(prj->repo_url), "%s", val);
                        else if (strcmp(last_key, "build_system") == 0) snprintf(prj->main_repo_build_system, sizeof(prj->main_repo_build_system), "%s", val);
                    }
                    last_key[0] = '\0';
                }
                break;
            case YAML_MAPPING_START_EVENT:
                depth++;
                if (strcmp(last_key, "git") == 0) {
                    section = SEC_GIT;
                    last_key[0] = '\0';
                } else if (strcmp(last_key, "build-config") == 0) {
                    section = SEC_BUILD_CFG;
                    last_key[0] = '\0';
                } else if (strcmp(last_key, "source") == 0) {
                    section = SEC_SOURCE;
                    last_key[0] = '\0';
                } else if (strcmp(last_key, "main_repo") == 0 && section == SEC_SOURCE) {
                    section = SEC_MAIN_REPO;
                    last_key[0] = '\0';
                } else if (seq == SEQ_DEP_REPOS) {
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
                        if (!manual_tail) {
                            manual_tail = prj->manual_dependencies;
                            while (manual_tail->next) manual_tail = manual_tail->next;
                        }
                        manual_tail->next = node;
                    }
                    manual_tail = node;
                    cur_manual = node;
                    prj->manual_dep_count++;
                }
                break;
            case YAML_MAPPING_END_EVENT:
                if (section == SEC_DEP_REPO_ITEM) cur_manual = NULL;
                section = (section == SEC_MAIN_REPO || section == SEC_DEP_REPO_ITEM) ? SEC_SOURCE : SEC_NONE;
                depth--;
                break;
            case YAML_SEQUENCE_START_EVENT:
                if (strcmp(last_key, "architectures") == 0) {
                    seq = SEQ_ARCH;
                    last_key[0] = '\0';
                } else if (strcmp(last_key, "dependencies") == 0 && (section == SEC_SOURCE || section == SEC_DEP_REPO_ITEM)) {
                    // Support source.dependencies and dependency_repos[i].dependencies
                    seq = SEQ_DEPS;
                    last_key[0] = '\0';
                } else if (strcmp(last_key, "dependency_repos") == 0 && section == SEC_SOURCE) {
                    seq = SEQ_DEP_REPOS;
                    last_key[0] = '\0';
                }
                break;
            case YAML_SEQUENCE_END_EVENT:
                if (seq == SEQ_DEP_REPOS) section = SEC_SOURCE;
                seq = SEQ_NONE; break;
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