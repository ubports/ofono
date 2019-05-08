#!/bin/sh

set -e

# Filelist:
# grep -oE 'ell/.*\.(h|c)( |$)' ../Makefile.am

filelist="
ell/ell.h
ell/util.h
ell/test.h
ell/strv.h
ell/utf8.h
ell/queue.h
ell/main.h
ell/idle.h
ell/signal.h
ell/timeout.h
ell/io.h
ell/log.h
ell/checksum.h
ell/random.h
ell/uuid.h
ell/file.h
ell/uintset.h
ell/string.h
ell/private.h
ell/missing.h
ell/util.c
ell/test.c
ell/strv.c
ell/utf8.c
ell/queue.c
ell/main.c
ell/idle.c
ell/signal.c
ell/timeout.c
ell/io.c
ell/log.c
ell/checksum.c
ell/random.c
ell/uuid.c
ell/file.c
ell/uintset.c
ell/string.c
"

mkdir -p ../ell

cd ../ell
touch internal

git clone --depth 1 https://git.kernel.org/pub/scm/libs/ell/ell.git

cd ell
cp -v $filelist ../

cd ..
rm -rf ell
