#!/usr/bin/env bash
# Run the C++ benchmark cases in fresh processes and record each attempt.
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'EOF'
Usage: run_suite.sh PROGRAM OUTPUT.csv [CASE ...] [options]
  --repeats N       Repetitions per case (default 5)
  --timeout SEC     Wall time limit in seconds (default 600)
  --memory-mib N    Virtual memory limit per process in MiB (default 2048)
  --cache POLICY    reuse, reset, or both (default both)

Omit CASE to run the program's whole catalogue. Measurements go to OUTPUT.csv
and process status, peak memory and hashes go to OUTPUT.runs.csv. Redirect
standard output to save the Markdown run table.
EOF
}

die() { echo "run_suite: $*" >&2; exit 1; }
digest() { sha256sum < "$1" | cut -d ' ' -f 1; }

repeats=5
seconds=600
memory_mib=2048
cache=both
args=()
while (($#)); do
    case $1 in
        -h|--help) usage; exit 0 ;;
        --) shift; args+=("$@"); break ;;
        --*=*) set -- "${1%%=*}" "${1#*=}" "${@:2}"; continue ;;
        --repeats|--timeout|--memory-mib|--cache)
            (($# >= 2)) || die "Missing value for $1"
            case $1 in
                --repeats) repeats=$2 ;; --timeout) seconds=$2 ;;
                --memory-mib) memory_mib=$2 ;; --cache) cache=$2 ;;
            esac
            shift 2 ;;
        -*) die "Unknown option: $1" ;;
        *) args+=("$1"); shift ;;
    esac
done
((${#args[@]} >= 2)) || { usage >&2; exit 1; }
((BASH_VERSINFO[0] >= 5)) || die "Use Bash 5 or newer for process timing"
for tool in gawk jq sha256sum timeout /usr/bin/time; do
    command -v "$tool" >/dev/null || die "Required tool not found: $tool"
done
positive_integer() {
    [[ $1 =~ ^[+]?0*([1-9][0-9]*)$ ]] || return 1
    local digits=${BASH_REMATCH[1]}
    [[ $((10#$digits)) == "$digits" ]] || return 1
    echo "$digits"
}
repeats=$(positive_integer "$repeats") || die "Repetitions must be a positive integer"
memory_mib=$(positive_integer "$memory_mib") || die "Memory must be a positive integer in MiB"
((memory_mib * 1024 / 1024 == memory_mib)) || die "Memory exceeds the shell's addressable range"
[[ $seconds =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$ ]] &&
    gawk -v sec="$seconds" 'BEGIN {exit !(sec > 0)}' || die "Timeout must be positive seconds"
case $cache in reuse|reset|both) ;; *) die "Unknown cache policy: $cache" ;; esac

program=$(realpath -- "${args[0]}")
[[ -f $program && -x $program ]] || die "Expected a benchmark executable: $program"
output=${args[1]}
output_dir=$(dirname -- "$output")
stem=$(basename -- "$output")
if [[ $stem == ?*.* && $stem != *. ]]; then stem=${stem%.*}; fi
runs=$output_dir/$stem.runs.csv
for path in "$output" "$runs"; do
    [[ ! -e $path && ! -L $path ]] || die "Result file already exists: $path"
done
directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

# The C++ catalogue supplies paths and seed defaults. Check each selected
# input before measuring so both cache policies use the same verified data.
"$program" --list > "$tmp/catalogue.csv"
gawk -v mode=list -f "$directory/suite_csv.awk" "$tmp/catalogue.csv" > "$tmp/catalogue"
mapfile -d '' -t catalogue < "$tmp/catalogue"
jq -er '.presentations[] | [.file, .sha256] | @tsv' "$directory/data/sources.json" > "$tmp/sources"
declare -A recorded hashes presentations slices seeds requested
while IFS=$'\t' read -r filename sha; do recorded["$filename"]=$sha; done < "$tmp/sources"
for name in "${args[@]:2}"; do
    [[ $name =~ ^[[:alnum:]_-]+$ ]] || die "Unknown case: $name"
    requested["$name"]=1
done
cases=()
for ((i=0; i<${#catalogue[@]}; i+=4)); do
    name=${catalogue[i]}
    if ((${#args[@]} > 2)) && [[ ! -v 'requested[$name]' ]]; then continue; fi
    cases+=("$name")
    presentations[$name]=${catalogue[i+1]}
    slices[$name]=${catalogue[i+2]}
    seeds[$name]=${catalogue[i+3]}
    for path in "${presentations[$name]}" "${slices[$name]}"; do
        [[ -n $path ]] || continue
        if [[ -z ${hashes[$path]+set} ]]; then
            hashes[$path]=$(digest "$path")
            filename=${path##*/}
            if [[ -v 'recorded[$filename]' && ${hashes[$path]} != "${recorded[$filename]}" ]]; then
                die "Dataset differs from its recorded source: $path"
            fi
        fi
    done
done
for name in "${args[@]:2}"; do [[ -v 'seeds[$name]' ]] || die "Unknown case: $name"; done
binary_hash=$(digest "$program")
recipes_hash=$(digest "$directory/workloads.hpp")

mkdir -p -- "$output_dir"
(set -C; : > "$output") || die "Could not create $output"
if ! (set -C; : > "$runs"); then rm -- "$output"; die "Could not create $runs"; fi
echo 'workload,repeat,cache,seed,status,returncode,elapsed_s,peak_rss_kib,timeout_s,memory_mib,presentation_sha256,slices_sha256,program_sha256,recipes_sha256,error' > "$runs"
echo '| Workload | Repeat | Cache | Status | Elapsed (s) | Peak RSS (KiB) |'
echo '| --- | ---: | --- | --- | ---: | ---: |'

run_case() {
    local name=$1 repeat=$2 policy=$3 seed=$4 presentation_hash=$5 slice_hash=$6
    local code=0 status=ok start elapsed rss error header
    local command=("$program" "$name" --cache "$policy")
    if [[ -n $seed ]]; then command+=(--seed "$seed"); fi

    : > "$tmp/rss"
    start=$EPOCHREALTIME
    (ulimit -v "$((memory_mib * 1024))" &&
        exec /usr/bin/time -q -f '%M' -o "$tmp/rss" \
            timeout --verbose --kill-after=1s "$seconds" "${command[@]}") \
        > "$tmp/raw.csv" 2> "$tmp/error" || code=$?
    elapsed=$(gawk -v start="$start" -v end="$EPOCHREALTIME" 'BEGIN {printf "%.6f", end - start}')
    rss=$(<"$tmp/rss")

    if ((code != 0)); then
        if ((code == 124)) || grep -q '^timeout: sending signal' "$tmp/error"; then status=timeout
        elif grep -q 'bad_alloc' "$tmp/error"; then status=memory_limit
        else status=error; fi
    elif ! gawk -v mode=measure -v expected="$name" -v repeat="$repeat" -v sums="$tmp/sums" \
            -f "$directory/suite_csv.awk" "$tmp/raw.csv" > "$tmp/rows" 2>> "$tmp/error"; then
        status=invalid_output
    elif [[ -n $previous && $(<"$tmp/sums") != "$previous" ]]; then
        status=result_mismatch
        echo "Checksums differ between cache policies" >> "$tmp/error"
    else
        IFS= read -r header < "$tmp/rows"
        if [[ -z $measurement_header ]]; then
            measurement_header=$header
            echo "$header" >> "$output"
        fi
        if [[ $header != "$measurement_header" ]]; then
            status=invalid_output
            echo "Measurement columns changed during the study" >> "$tmp/error"
        else
            tail -n +2 "$tmp/rows" >> "$output"
            previous=$(<"$tmp/sums")
        fi
    fi
    # Text from the child can itself contain CSV quotes and newlines.
    error=$(tail -c 2000 "$tmp/error")
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' \
        "$name" "$repeat" "$policy" "$seed" "$status" "$code" "$elapsed" "$rss" \
        "$seconds" "$memory_mib" "$presentation_hash" "$slice_hash" "$binary_hash" "$recipes_hash" \
        "${error//\"/\"\"}" >> "$runs"
    printf '| %s | %s | %s | %s | %s | %s |\n' "$name" "$repeat" "$policy" "$status" "$elapsed" "$rss"
    if [[ $status != ok ]]; then failed=1; fi
}

measurement_header=''
failed=0
for name in "${cases[@]}"; do
    presentation=${presentations[$name]}
    slice_file=${slices[$name]}
    presentation_hash=''
    slice_hash=''
    if [[ -n $presentation ]]; then presentation_hash=${hashes[$presentation]}; fi
    if [[ -n $slice_file ]]; then slice_hash=${hashes[$slice_file]}; fi
    for ((repeat=1; repeat<=repeats; repeat++)); do
        seed=${seeds[$name]}
        if [[ -n $seed ]]; then seed=$(((seed + repeat - 1) % 4294967296)); fi
        policies=("$cache")
        if [[ $cache == both ]]; then
            if ((repeat % 2)); then policies=(reuse reset); else policies=(reset reuse); fi
        fi
        previous=''
        for policy in "${policies[@]}"; do
            run_case "$name" "$repeat" "$policy" "$seed" "$presentation_hash" "$slice_hash"
        done
    done
done
exit "$failed"
