#!/usr/bin/env bash

# Usage:
#   SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
#   . "$SCRIPT_DIR/logging.sh"
#   formatted_log "INFO" "/path/file.c" 42 "project_name" "thread_arch" "Log message"

formatted_log() {
  local log_level="$1"
  local source_file="$2"
  local line_number="$3"
  local project_name="$4"
  local thread_arch="$5"
  shift 5
  local message="$*"

  # Timestamp format: "[YYYY-MM-DD HH:MM:SS] "
  local ts
  ts="$(date '+%Y-%m-%d %H:%M:%S')"

  local ip os arch agent

  ip="$(curl -s --max-time 3 https://api.ipify.org 2>/dev/null || true)"
  [[ -n "$ip" ]] || ip="Unknown IP"

  os="$(uname -o 2>/dev/null || true)"
  [[ -n "$os" ]] || os="Unknown OS"

  arch="$(uname -m 2>/dev/null || true)"
  [[ -n "$arch" ]] || arch="Unknown Arch"

  agent="$(hostnamectl 2>/dev/null | grep -F 'Hardware Model' | cut -d ':' -f2 | sed 's/^[[:space:]]*//' || true)"
  [[ -n "$agent" ]] || agent="Unknown Agent"

  printf '[%s] [%s] source: { client: { ip: %s, os: %s, arch: %s, agent: %s }, location: { file: %s, line: %s } }, project: %s, thread_arch: %s, message: %s\n' \
    "$ts" "$log_level" \
    "$ip" "$os" "$arch" "$agent" \
    "$source_file" "$line_number" \
    "${project_name:-N/A}" "${thread_arch:-N/A}" \
    "$message"
}
