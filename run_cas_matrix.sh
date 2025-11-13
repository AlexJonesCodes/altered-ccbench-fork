#!/usr/bin/env bash
set -euo pipefail

BIN=${BIN:-./ccbench}
REPS=${REPS:-10000}
LOG_DIR=${LOG_DIR:-cas_logs}
if [[ -z ${CORE_COUNT:-} ]]; then
  if command -v nproc >/dev/null 2>&1; then
    CORE_COUNT=$(nproc)
  else
    CORE_COUNT=8
  fi
fi
if (( CORE_COUNT < 2 )); then
  echo "error: CORE_COUNT must be at least 2 (got ${CORE_COUNT})" >&2
  exit 1
fi
TIMEOUT=${TIMEOUT:-60}

# Automatically rebuild the default benchmark binary when it is out of date.
# This avoids confusing situations where a stale executable is used after the
# sources have been updated (for example after a `git pull`).  The behaviour can
# be disabled by setting SKIP_AUTO_BUILD=1.
if [[ ${SKIP_AUTO_BUILD:-0} -eq 0 ]] \
   && [[ "$BIN" == "./ccbench" || "$BIN" == "ccbench" ]] \
   && [[ -f Makefile ]] \
   && command -v make >/dev/null 2>&1; then
  if ! make -q >/dev/null 2>&1; then
    status=$?
    if [[ $status -eq 1 ]]; then
      echo "* info: rebuilding ccbench before running benchmarks..." >&2
      make >&2
    elif [[ $status -ne 0 ]]; then
      echo "* warning: unable to determine build status (make -q exited with $status); attempting rebuild." >&2
      make >&2
    fi
  fi
fi

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found or not executable" >&2
  exit 1
fi

mkdir -p "$LOG_DIR"

echo "source_core,target_core,reported_core_a,avg_cycles_a,reported_core_b,avg_cycles_b"

for ((src=0; src<CORE_COUNT; src++)); do
  for ((dst=0; dst<CORE_COUNT; dst++)); do
    if [[ $src -eq $dst ]]; then
      continue
    fi
    cores_arg="[$src,$dst]"
    #echo "Running CAS on cores $src and $dst..." >&2

    log_file="$LOG_DIR/src_${src}_dst_${dst}.log"

    if output=$(timeout "$TIMEOUT" "$BIN" --test CAS --cores 2 --cores_array "$cores_arg" --repetitions "$REPS" 2>&1); then
      status=0
    else
      status=$?
    fi

    printf '%s\n' "$output" >"$log_file"

    line_count=$(printf '%s\n' "$output" | wc -l)
    byte_count=$(printf '%s' "$output" | wc -c)

    excerpt="--- log metadata ---\\n"
    excerpt+="total_lines=${line_count}\\n"
    excerpt+="total_bytes=${byte_count}\\n"
    excerpt+="--- first 200 lines ---\\n"
    excerpt+="$(printf '%s\n' "$output" | head -n 200)"

    if (( line_count > 200 )); then
      excerpt+="\\n--- last 200 lines ---\\n"
      excerpt+="$(printf '%s\n' "$output" | tail -n 200)"
    fi

    reason=""
    if [[ $status -eq 124 ]]; then
      reason="timed out"
    elif [[ $status -ne 0 ]]; then
      reason="failed (exit status $status)"
      if (( status > 128 )); then
        signal_num=$((status - 128))
        if signal_name=$(kill -l "$signal_num" 2>/dev/null); then
          reason+=" (signal ${signal_num}/${signal_name})"
        fi
      fi
    fi

    mapfile -t stats < <(printf '%s\n' "$output" | awk '/Core [0-9]+ : avg/ {printf "%s %s\n", $3, $6}')

    if [[ ${#stats[@]} -lt 2 ]]; then
      if [[ -n $reason ]]; then
        {
          echo "Run ${reason} for $src,$dst"
          echo "  Command: $BIN --test CAS --cores 2 --cores_array $cores_arg --repetitions $REPS"
          echo "  Log file: $log_file"
          echo "  Output excerpt:"
          printf '%s\n' "$excerpt" | sed 's/^/    /'
        } >&2
      else
        {
          echo "Unexpected output format for cores $src,$dst"
          echo "  Command: $BIN --test CAS --cores 2 --cores_array $cores_arg --repetitions $REPS"
          echo "  Log file: $log_file"
          echo "  Output excerpt:"
          printf '%s\n' "$excerpt" | sed 's/^/    /'
        } >&2
      fi
      echo "${src},${dst},,,,"
      continue
    fi

    first_core=$(awk '{print $1}' <<<"${stats[0]}")
    first_avg=$(awk '{print $2}' <<<"${stats[0]}")
    second_core=$(awk '{print $1}' <<<"${stats[1]}")
    second_avg=$(awk '{print $2}' <<<"${stats[1]}")

    if [[ -n $reason ]]; then
      {
        echo "Run ${reason} for $src,$dst, but statistics were parsed"
        echo "  Command: $BIN --test CAS --cores 2 --cores_array $cores_arg --repetitions $REPS"
        echo "  Log file: $log_file"
        echo "  Output excerpt:"
        printf '%s\n' "$excerpt" | sed 's/^/    /'
      } >&2
    fi

    echo "${src},${dst},${first_core},${first_avg},${second_core},${second_avg}"
  done
done
