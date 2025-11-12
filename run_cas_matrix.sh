#!/usr/bin/env bash
set -euo pipefail

BIN=${BIN:-./ccbench}
REPS=${REPS:-10000}
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

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found or not executable" >&2
  exit 1
fi

echo "source_core,target_core,reported_core_a,avg_cycles_a,reported_core_b,avg_cycles_b"

for ((src=0; src<CORE_COUNT; src++)); do
  for ((dst=0; dst<CORE_COUNT; dst++)); do
    if [[ $src -eq $dst ]]; then
      continue
    fi
    cores_arg="[$src,$dst]"
    #echo "Running CAS on cores $src and $dst..." >&2

    if output=$(timeout "$TIMEOUT" "$BIN" --test CAS --cores 2 --cores_array "$cores_arg" --repetitions "$REPS" 2>&1); then
      status=0
    else
      status=$?
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
      {
        if [[ -n $reason ]]; then
          echo "Run ${reason} for $src,$dst"
        else
          echo "Unexpected output format for cores $src,$dst"
        fi
        echo "  Command: $BIN --test CAS --cores 2 --cores_array $cores_arg --repetitions $REPS"
        echo "  Output:"
        printf '%s\n' "$output" | sed 's/^/    /'
      } >&2
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
        echo "  Output:"
        printf '%s\n' "$output" | sed 's/^/    /'
      } >&2
    fi

    echo "${src},${dst},${first_core},${first_avg},${second_core},${second_avg}"
  done
done
