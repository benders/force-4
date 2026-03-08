#!/usr/bin/env bash
set -e
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4 idf.py build "$@"
