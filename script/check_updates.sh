#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

chroot_dir=$1
chroot_build_dir=$2
repo_name=$3
worker_chroot_log_file=$4
project_name=$5
tmp_arch=$6

if [ -z "$chroot_dir" ] || [ -z "$chroot_build_dir" ] || [ -z "$repo_name" ] || [ -z "$worker_chroot_log_file" ] || [ -z "$project_name" ] || [ -z "$tmp_arch" ]; then
    exit 1
fi

if [ ! -d "$chroot_dir/home" ]; then
    # The chroot is not set up, we need recovery operations
    exit 1
fi

if [ ! -d "$chroot_dir$chroot_build_dir/$repo_name" ]; then
    # The repo directory does not exist, so it must be the first run - we need to perform the clone
    exit 2
fi

# Enter the chroot to check for real updates
$chroot_dir/_enter <<EOF
    if [ ! -f "$worker_chroot_log_file" ]; then
        touch "$worker_chroot_log_file"
        if [ $? -ne 0 ]; then
            exit 1
        fi
    fi
    exec >> "$worker_chroot_log_file" 2>&1
    . /opt/v2ci/logging.sh
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "$tmp_arch" "Checking for updates in repository: $repo_name"
    apt-get update
    apt-get upgrade -y
    apt-get install -y git

    cd "$chroot_build_dir/$repo_name" || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$tmp_arch" "Failed to enter directory: $chroot_build_dir/$repo_name"; exit 1; }
    before_pull_hash=\$(git rev-parse HEAD)
    if [ \$? -ne 0 ]; then
        exit 1
    fi
    git_pull_output=\$(git pull 2>&1)
    pull_status=\$?
    if [ "\$pull_status" -ne 0 ]; then
        exit 1
    fi
    after_pull_hash=\$(git rev-parse HEAD)
    if [ \$? -ne 0 ]; then
        exit 1
    fi
    if [ "\$before_pull_hash" != "\$after_pull_hash" ]; then
        exit 2
    else
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$tmp_arch" "[From check_updates.sh in $chroot_dir] No updates found for repository: $repo_name"
        exit 0
    fi
EOF

exit $?