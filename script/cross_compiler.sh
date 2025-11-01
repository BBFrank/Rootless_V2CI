#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

debian_arch=$1
thread_chroot_dir=$2
thread_chroot_build_dir=$3
repo_name=$4
main_repo_build_system=$5
thread_log_file=$6
thread_chroot_log_file=$7
project_name=$8
thread_chroot_target_dir=$9
project_target_dir=${10}
mem_limit=${11}

if [ -z "$project_name" ]
	then
		exit 1
fi

if [ ! -f "$thread_log_file" ]; then
	touch "$thread_log_file"
	if [ $? -ne 0 ]; then
		exit 1
	fi
fi

# Determine if the repo is the main project or a dependency
main_project="yes"
if [ -z "$thread_chroot_target_dir" ] || [ -z "$project_target_dir" ] || [ -z "$mem_limit" ]; then
	main_project="no"
fi

exec >> "$thread_log_file" 2>&1

formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Starting cross-compilation for $repo_name in $debian_arch chroot at $thread_chroot_dir$thread_chroot_build_dir"

formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Entering rootfs at $thread_chroot_dir$thread_chroot_build_dir; logs will be available in $thread_chroot_dir$thread_chroot_log_file"
$thread_chroot_dir/_enter <<EOF
    exec >> "$thread_chroot_log_file" 2>&1
    . /opt/v2ci/logging.sh
    REPO_ROOT="$thread_chroot_build_dir/$repo_name"
    cd "\$REPO_ROOT" || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: [From cross_compiler.sh for $debian_arch arch] Cannot change directory to \$REPO_ROOT"; exit 1; }

    # Build: main project -> build directory, no install; dependencies -> install
    if [ "$main_repo_build_system" = "cmake" ]; then
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch for repo $repo_name] Building with CMake"
        mkdir -p build && cd build
        cmake .. || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: CMake configuration failed"; exit 1; }
        if [ "$main_project" = "yes" ]; then
            make -j\$(nproc) || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: CMake build failed"; exit 1; }
        else
            make -j\$(nproc) install || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: CMake install failed"; exit 1; }
        fi
        cd ..

    elif [ "$main_repo_build_system" = "autotools" ]; then
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Building with Autotools"
        if [ -f "configure.ac" ] || [ -f "configure.in" ]; then
            autoreconf -fiv || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: autoreconf failed"; exit 1; }
        fi
        if [ ! -f "configure" ]; then
            formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: configure script not found"; exit 1;
        fi
        ./configure --prefix=/usr || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: configure failed"; exit 1; }
        if [ "$main_project" = "yes" ]; then
            make -j\$(nproc) || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: make failed"; exit 1; }
        else
            make -j\$(nproc) install || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: make install failed"; exit 1; }
        fi

    elif [ "$main_repo_build_system" = "meson" ]; then
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Building with Meson"
        meson setup build . --default-library=both || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Meson configuration failed"; exit 1; }
        if [ "$main_project" = "yes" ]; then
            meson compile -C build || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Meson build failed"; exit 1; }
        else
            meson install -C build || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Meson install failed"; exit 1; }
        fi

    elif [ "$main_repo_build_system" = "makefile" ]; then
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Building with Makefile"
        if [ "$main_project" = "yes" ]; then
            make -j\$(nproc) || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: make failed"; exit 1; }
        else
            make -j\$(nproc) install || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: make install failed"; exit 1; }
        fi
    else
        formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Unsupported build system: $main_repo_build_system"
        exit 1
    fi

    # Only for main project, search and copy the built binary to the target directory
    if [ "$main_project" = "yes" ]; then
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Build completed, selecting binaries"

        # 1) Priority to executables in the common build directories
        binary_candidates=()
        while IFS= read -r -d '' f; do
            if file "\$f" | grep -q "ELF.*executable"; then
                binary_candidates+=("\$f")
            fi
        done < <(
            find . \( -path "./build/*" -o -path "./builddir/*" -o -path "./target/release/*" -o -path "./dist/*" \) \
                -type f -executable -not -path "*/.*" -print0 | sort -z
        )

        if [ "\${#binary_candidates[@]}" -eq 0 ]; then
            formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] No binaries found in known build directories"
            exit 1
        fi

        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Found \${#binary_candidates[@]} executable candidates: \${binary_candidates[*]}"

		# 2) Prefer those that match the repo name
		named_candidates=()
		for f in "${binary_candidates[@]}"; do
			base=$(basename "$f")
			case "$base" in
				${repo_name}|${repo_name}-*|${repo_name}_*|${repo_name}.*)
					named_candidates+=("$f")
				;;
			esac
		done
		if [ "\${#named_candidates[@]}" -gt 0 ]; then
			binary_candidates=( "\${named_candidates[@]}" )
            formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Found \${#binary_candidates[@]} named candidates: \${binary_candidates[*]}"
		fi

        # 3) Prefer statically linked binaries if multiple candidates remain
        selected_binary=""
        for f in "\${binary_candidates[@]}"; do
            if file "\$f" 2>/dev/null | grep -q "statically linked"; then
                selected_binary="\$f"
                formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Found statically linked binary: \$selected_binary"
                break
            fi
        done
        if [ -z "\$selected_binary" ]; then
            selected_binary="\${binary_candidates[0]}"
        fi

        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Selected binary: \$selected_binary"
        cp -f "\$selected_binary" "$thread_chroot_target_dir/$repo_name-$debian_arch" || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Failed to copy final binary"; exit 1; }
    else
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Dependency installation completed"
    fi
EOF

if [ $? -ne 0 ]; then
    formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Build process failed in chroot"
    exit 1
fi

exec >> "$thread_log_file" 2>&1

# Only for main project, move the binary to the final target directory with simple versioning
if [ "$main_project" = "yes" ]; then
	cd "$thread_chroot_dir$thread_chroot_build_dir/$repo_name"
	current_tag=$(git describe --tags --abbrev=0 2>/dev/null)
	if [ -z "$current_tag" ]; then
		release_version="unstable"
	else
		release_version="$current_tag"
	fi

	formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Creating daily directory: $project_target_dir/daily"
	mkdir -p "$project_target_dir/daily"

	# Copy in the daily directory without unnecessary checks (exists and is readable)
	if [ -f "$thread_chroot_dir$thread_chroot_target_dir/$repo_name-$debian_arch" ]; then
		formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Moving $repo_name-$debian_arch to $project_target_dir/daily/$repo_name-$release_version-$debian_arch"
		install -m 0755 "$thread_chroot_dir$thread_chroot_target_dir/$repo_name-$debian_arch" "$project_target_dir/daily/$repo_name-$release_version-$debian_arch" || { formatted_log "ERROR" "$0" "$LINENO" "$project_name" "$debian_arch" "Error: Failed to copy final binary"; exit 1; }

        # If we exceeded the mem_limit for the daily builds, remove the oldest files until we are under the limit (note: here we ignore the rotation since this will be handled by the cronjob - trade-off: it could happen that multiple builds exceed the limit due to old files that are still in the daily dir even if they were created more than 24h ago, because of low frequency of the cronjob;
        # possible solutions: either increase cronjob frequency or implement a more complex logic here to also consider file ages. Anyway, this is a rare edge case, especially for academic projects, so we keep it simple for now)
        total_daily_size=$(du -s "$project_target_dir/daily" | cut -f1)
        while [ "$total_daily_size" -gt "$mem_limit" ]; do
            oldest_file=$(ls -t "$project_target_dir/daily" | tail -n 1)
            if [ "$oldest_file" = "$repo_name-$release_version-$debian_arch" ]; then
                formatted_log "WARNING" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Only the just added file remains but daily memory limit exceeded; cannot remove it"
                break
            fi
            oldest_file_size=$(du -s "$project_target_dir/daily/$oldest_file" | cut -f1)
            rm -f "$project_target_dir/daily/$oldest_file"
            total_daily_size=$((total_daily_size - oldest_file_size))
            formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Removed oldest file $oldest_file to respect daily memory limit"
        done
    fi

	formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Cross-compilation completed successfully"
else
	formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Dependency installation completed successfully"
fi

exit 0