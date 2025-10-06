#!/bin/bash

thread_chroot_dir=$1
thread_chroot_build_dir=$2
repo_name=$3
git_url=$4
thread_log_file=$5
project_name=$6
thread_arch=$7

if [ -z "$thread_chroot_dir" ] || [ -z "$thread_chroot_build_dir" ] || [ -z "$repo_name" ] || [ -z "$git_url" ] || [ -z "$thread_log_file" ] || [ -z "$project_name" ] || [ -z "$thread_arch" ]; then
    exit 1
fi

if [ ! -f "$thread_log_file" ]; then
    touch "$thread_log_file"
    if [ $? -ne 0 ]; then
        exit 1
    fi
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

exec >> "$thread_log_file" 2>&1

# check if the repo was already cloned
if [ ! -d "$thread_chroot_dir$thread_chroot_build_dir/$repo_name" ]; then
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "$thread_arch" "[From clone_or_pull_for_project.sh for $repo_name] Cloning repository $git_url into $thread_chroot_dir$thread_chroot_build_dir/$repo_name"
    git clone "$git_url" "$thread_chroot_dir$thread_chroot_build_dir/$repo_name"
    if [ $? -ne 0 ]; then
        formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$thread_arch" "Error: [From clone_or_pull_for_project.sh for $repo_name] Failed to clone repository $git_url into $thread_chroot_dir$thread_chroot_build_dir/$repo_name"
        exit 1
    fi
else
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "$thread_arch" "[From clone_or_pull_for_project.sh for $repo_name] Pulling latest changes for repository $git_url in $thread_chroot_dir$thread_chroot_build_dir/$repo_name"
    cd "$thread_chroot_dir$thread_chroot_build_dir/$repo_name" || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$thread_arch" "Error: [From clone_or_pull_for_project.sh for $repo_name] Cannot change directory to $thread_chroot_dir$thread_chroot_build_dir/$repo_name"; exit 1; }
    git pull
    if [ $? -ne 0 ]; then
        formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$thread_arch" "Error: [From clone_or_pull_for_project.sh for $repo_name] Failed to pull latest changes for repository $git_url in $thread_chroot_dir$thread_chroot_build_dir/$repo_name"
        exit 1
    fi
fi