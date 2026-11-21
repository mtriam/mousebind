#!/usr/bin/env bash
set -eu

cd "$(dirname "$0")"
${CC:-cc} -std=c11 -Wall -Wextra -Wformat=2 -O2 \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
    ${CFLAGS:-} -o "${1:-mbtn}" mbtn.c
