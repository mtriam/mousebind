#!/usr/bin/env bash
set -eu

cd "$(dirname "$0")"
${CC:-cc} -std=c11 -Wall -Wextra -O2 -o "${1:-mbtn}" mbtn.c
