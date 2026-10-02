#!/bin/sh
# ESP-IDF v6.1 ships FatFs with f_expand() disabled, which the production log
# needs. Enables it in $IDF_PATH; safe to run more than once. Remove once the
# ESP-IDF release in use contains upstream commit 74a7a4b.
set -e
ffconf="${IDF_PATH:?IDF_PATH is not set}/components/fatfs/src/ffconf.h"
sed -i 's/^#define FF_USE_EXPAND[[:space:]]*0$/#define FF_USE_EXPAND\t1/' "$ffconf"
if ! grep -q '^#define FF_USE_EXPAND[[:space:]]*1$' "$ffconf"; then
    echo "$ffconf: could not enable FF_USE_EXPAND" >&2
    exit 1
fi
