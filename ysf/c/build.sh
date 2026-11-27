#!/bin/sh
# builds ysf/c/ysf_test.exe from the firmware's sources (gcc: msys2 ucrt64)
cd "$(dirname "$0")/../.."
export PATH=/c/msys64/ucrt64/bin:$PATH
gcc -O2 -Wall -Wextra -Isrc/services -Ivocoder/c/ambe -Ip25/c/p25 -Ip25/c/imbe -o ysf/c/ysf_test.exe ysf/c/ysf_test.c \
    src/services/ysf_core.c src/services/p25_demod.c src/services/fsk4.c vocoder/c/ambe/ambe_frame.c \
    vocoder/c/ambe/ambe_tables.c p25/c/p25/p25_fec.c p25/c/imbe/imbe_frame.c -lm
