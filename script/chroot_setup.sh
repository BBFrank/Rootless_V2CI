#!/bin/bash

export PATH=/usr/sbin:$PATH

debian_arch=$1
chroot_dir=$2
main_log_file=$3

if [ -z "$main_log_file" ]; then
    exit 1
fi

if [ ! -f "$main_log_file" ]; then
    touch "$main_log_file"
    if [ $? -ne 0 ]; then
		exit 1
	fi
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

exec >> "$main_log_file" 2>&1

if [ $debian_arch == "arm64" ]; then
	suite="bookworm"
else
	suite="trixie"
fi

formatted_log "INFO" "$0" "$LINENO" "" "$debian_arch" "[From chroot_setup.sh for $debian_arch arch] Starting chroot setup for $debian_arch; using $suite"

WRAPPER="$SCRIPT_DIR/rootless-debootstrap-wrapper.sh"

if [ ! -x "$WRAPPER" ]; then
    formatted_log "ERROR" "$0" "$LINENO" "" "$debian_arch" "Error: wrapper not found or not executable: $WRAPPER"
    exit 1
fi

if [ ! -d "$chroot_dir/home" ]; then
    formatted_log "INFO" "$0" "$LINENO" "" "$debian_arch" "[From chroot_setup.sh for $debian_arch arch] Creating rootfs at $chroot_dir"
    "$WRAPPER" --target-dir="$chroot_dir" --arch="$debian_arch" --suite "$suite" --include=build-essential
    if [ $? -ne 0 ]; then
        formatted_log "ERROR" "$0" "$LINENO" "" "$debian_arch" "Error: [From chroot_setup.sh for $debian_arch arch] Failed to create rootfs at $chroot_dir"
        exit 1
    fi
    # Deploy logging.sh inside the chroot so here-docs can source it
    mkdir -p "$chroot_dir/opt/v2ci" && install -m 0644 "$SCRIPT_DIR/logging.sh" "$chroot_dir/opt/v2ci/logging.sh"
    if [ $? -ne 0 ]; then
        formatted_log "ERROR" "$0" "$LINENO" "" "$debian_arch" "Error: not able to copy logging.sh to $chroot_dir/opt/v2ci/logging.sh"
        exit 1
    fi
fi