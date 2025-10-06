#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

chroot_dir=$1
thread_chroot_log_file=$2
project_name=$3
thread_arch=$4
shift 4

if [ "$#" -eq 1 ]; then
    read -r -a packages <<< "$1"
else
    packages=("$@")
fi

if [ -z "$chroot_dir" ] || [ -z "$thread_chroot_log_file" ]; then
    exit 1
fi

$chroot_dir/_enter <<EOF
    if [ ! -f "$thread_chroot_log_file" ]; then
        touch "$thread_chroot_log_file"
        if [ $? -ne 0 ]; then
            exit 1
        fi
    fi

    exec >> "$thread_chroot_log_file" 2>&1
    . /opt/v2ci/logging.sh
    # First try to install curl (needed even for logging)
    apt-get install -y curl
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "$thread_arch" "[From install_packages_in_chroot.sh in $chroot_dir] Installing packages: ${packages[@]}"
    apt-get update
    apt-get install -y git gcc g++ libc-dev make dpkg-dev autoconf automake libtool cmake meson ninja-build pkg-config file ${packages[@]}
    if [ \$? -ne 0 ]; then
        formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$thread_arch" "Failed to install packages: ${packages[@]}"
        exit 1
    fi
EOF
if [ $? -ne 0 ]; then
    formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$thread_arch" "Failed to enter chroot or install packages"
    exit 1
fi
