#!/bin/bash

arch="arm64"

as -arch $arch hello.s -o hello.o
sdk_path=`xcrun -sdk macosx --show-sdk-path`
ld hello.o -o hello -lSystem -syslibroot "$sdk_path" -e _start -arch $arch

# Sign the binary (ad-hoc for local testing)
codesign --sign - ./hello

./hello
rm -f hello.o
