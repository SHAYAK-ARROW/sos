#!/bin/bash
set -e
echo "Building SOSBasic OS..."
make clean
make
echo "Build Successful! Output: sosbasic.bin"
