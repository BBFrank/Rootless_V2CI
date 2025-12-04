#!/bin/bash

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$SCRIPT_DIR/logging.sh"

DAY_MINUTES=1440 
WEEK_MINUTES=10080
MONTH_MINUTES=43200
YEAR_MINUTES=525600

project_name=$1
project_target_dir=$2
log_file=$3
weekly_mem_limit_kb=$4
monthly_mem_limit_kb=$5
yearly_mem_limit_kb=$6
weekly_interval_min=$7
monthly_interval_min=$8
yearly_interval_min=$9

if [ -z "$project_name" ] || [ -z "$project_target_dir" ] || [ -z "$log_file" ] || [ -z "$weekly_mem_limit_kb" ] || [ -z "$monthly_mem_limit_kb" ] || [ -z "$yearly_mem_limit_kb" ] || [ -z "$weekly_interval_min" ] || [ -z "$monthly_interval_min" ] || [ -z "$yearly_interval_min" ]; then
    exit 1
fi

minutes_distance_calculator() {
    local mtime1=$1
    local mtime2=$2

    diff_min=$(( (mtime1 > mtime2 ? mtime1 - mtime2 : mtime2 - mtime1) / 60 ))
    echo "$diff_min"
}

minutes_age_calculator() {
    local mtime=$1
    local current_time=$(date +%s)

    age_min=$(( (current_time - mtime) / 60 ))
    echo "$age_min"
}

rotation_engine() {
    local rotation_type=$1
    local current_dir=$2
    local later_dir=$3
    local oldest_current_file=$(ls -t "$current_dir" | tail -n 1)

    if [ -n "$oldest_current_file" ]; then
        while true; do
            oldest_current_file=$(ls -t "$current_dir" | tail -n 1)
            if [ -z "$oldest_current_file" ]; then
                formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] No more files in $rotation_type directory to process. Exiting $rotation_type rotation loop."
                break
            fi

            # Obtain the memory and interval constraints and the minimum required age for the rotation of the file, based on rotation type
            MEM_LIMIT=0
            INTERVAL_LIMIT=0
            MIN_AGE=0
            if [ $rotation_type == "daily" ]; then
                MEM_LIMIT=$weekly_mem_limit_kb
                INTERVAL_LIMIT=$weekly_interval_min
                MIN_AGE=$DAY_MINUTES
            elif [ $rotation_type == "weekly" ]; then
                MEM_LIMIT=$monthly_mem_limit_kb
                INTERVAL_LIMIT=$monthly_interval_min
                MIN_AGE=$WEEK_MINUTES
            elif [ $rotation_type == "monthly" ]; then
                MEM_LIMIT=$yearly_mem_limit_kb
                INTERVAL_LIMIT=$yearly_interval_min
                MIN_AGE=$MONTH_MINUTES
            fi

            # Check if the oldest current file is old enough for rotation
            mtime_oldest_current=$(stat -c %Y "$current_dir/$oldest_current_file")
            oldest_current_age=$(minutes_age_calculator "$mtime_oldest_current")
            if [ "$oldest_current_age" -lt "$MIN_AGE" ]; then
                formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Oldest $rotation_type file $oldest_current_file is not old enough for rotation. Exiting $rotation_type rotation loop."
                break
            fi

            # Get the most recent later_file modification time in order to check time interval constraint
            recent_later_file=$(ls -t "$later_dir" | head -n 1)
            if [ -n "$recent_later_file" ]; then
                mtime_recent_later=$(stat -c %Y "$later_dir/$recent_later_file")
                minutes_diff=$(minutes_distance_calculator "$mtime_oldest_current" "$mtime_recent_later")
                if [ "$minutes_diff" -lt "$INTERVAL_LIMIT" ]; then
                    formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Time interval constraint not satisfied between oldest $rotation_type file $oldest_current_file and most recent later file $recent_later_file. Deleting the oldest $rotation_type file."
                    rm -f "$current_dir/$oldest_current_file"
                    continue
                fi
            fi

            # Compute the dimension of the oldest current file, and the total dimension of the later directory in order to check memory limit constraint
            oldest_current_mem_kb=$(du -k "$current_dir/$oldest_current_file" | cut -f1)
            later_dir_mem_kb=$(du -sk "$later_dir" | cut -f1)
            forcasted_later_dir_mem_kb=$((later_dir_mem_kb + oldest_current_mem_kb))
            while [ "$forcasted_later_dir_mem_kb" -gt "$MEM_LIMIT" ]; do
                oldest_later_file=$(ls -t "$later_dir" | tail -n 1)
                if [ -z "$oldest_later_file" ]; then
                    break
                fi
                oldest_later_file_size=$(du -s "$later_dir/$oldest_later_file" | cut -f1)
                rm -f "$later_dir/$oldest_later_file"
                forcasted_later_dir_mem_kb=$((forcasted_later_dir_mem_kb - oldest_later_file_size))
                formatted_log "INFO" "$0" "$LINENO" "$project_name" "$debian_arch" "[From cross_compiler.sh for $debian_arch arch] Removed oldest file $oldest_later_file to respect $rotation_type memory limit"
            done

            # Move the oldest current file to the later directory
            mv "$current_dir/$oldest_current_file" "$later_dir/"
            formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Moved oldest $rotation_type file $oldest_current_file from $current_dir to $later_dir."
        done
    else
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] No files found in $rotation_type directory. Skipping $rotation_type rotation."
    fi
}

# Check log file existence and redirect output
if [ ! -f "$log_file" ]; then
    touch "$log_file"
    if [ $? -ne 0 ]; then
        exit 1
    fi
fi
exec >> "$log_file" 2>&1

formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Starting binaries rotation cronjob for project $project_name."

# Check the existance of target daily directory (if not present, a build has probably not yet occurred)
if [ ! -d "$project_target_dir/daily" ]; then
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Daily directory not found in target_dir. Exiting."
    exit 0
fi

daily_dir="$project_target_dir/daily"
weekly_dir="$project_target_dir/weekly"
monthly_dir="$project_target_dir/monthly"
yearly_dir="$project_target_dir/yearly"

# 1. YEARLY ROTATION -> no further directory to move to, so just clean up too old files from yearly directory
formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Performing yearly rotation cleanup."
oldest_yearly_file=$(ls -t "$yearly_dir" | tail -n 1)
if [ -n "$oldest_yearly_file" ]; then
    while true; do
        oldest_yearly_file=$(ls -t "$yearly_dir" | tail -n 1)
        if [ -z "$oldest_yearly_file" ]; then
            formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] No more files in yearly directory to process. Exiting yearly rotation cleanup loop."
            break
        fi
        mtime_oldest_yearly=$(stat -c %Y "$yearly_dir/$oldest_yearly_file")
        oldest_yearly_age=$(minutes_age_calculator "$mtime_oldest_yearly")
        if [ "$oldest_yearly_age" -lt "$YEAR_MINUTES" ]; then
            break
        fi
        formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Removing yearly file $oldest_yearly_file from yearly directory due to age constraint."
        rm -f "$yearly_dir/$oldest_yearly_file"
    done
else
    formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] No files found in yearly directory. Skipping yearly rotation cleanup."
fi

# 2. MONTHLY ROTATION
formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Performing monthly rotation."
mkdir -p "$yearly_dir"
rotation_engine "monthly" "$monthly_dir" "$yearly_dir"

# 3. WEEKLY ROTATION
formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Performing weekly rotation."
mkdir -p "$monthly_dir"
rotation_engine "weekly" "$weekly_dir" "$monthly_dir"

# 4. DAILY ROTATION
formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Performing daily rotation."
mkdir -p "$weekly_dir"
rotation_engine "daily" "$daily_dir" "$weekly_dir"

formatted_log "INFO" "$0" "$LINENO" "$project_name" "" "[From binaries_rotation_cronjob.sh] Binaries rotation cronjob for project $project_name completed."
