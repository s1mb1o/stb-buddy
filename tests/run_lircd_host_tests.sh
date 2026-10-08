#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
remotes_dir=${1:-"$project_dir/tests/fixtures"}
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM

if [ ! -d "$remotes_dir" ]; then
    echo "remote fixture directory does not exist: $remotes_dir" >&2
    exit 2
fi
if ! find "$remotes_dir" -name lircd.conf -type f -print -quit | grep -q .; then
    echo "no lircd.conf fixtures found in: $remotes_dir" >&2
    exit 2
fi

c++ -std=c++20 -Wall -Wextra -Werror -Wconversion -Wshadow \
    -I"$project_dir/main" \
    "$project_dir/main/lircd.cpp" \
    "$project_dir/main/ir_waveform.cpp" \
    "$project_dir/tests/lircd_host_test.cpp" \
    -o "$test_dir/lircd_host_test"

find "$remotes_dir" -name lircd.conf -type f -print0 | \
    sort -z | xargs -0 -r "$test_dir/lircd_host_test"
