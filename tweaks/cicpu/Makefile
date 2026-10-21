# cicpu: CoreImage fail-clean tweak for ios-sweet (see ../../research/coreimage-plan.md)
#
# Plain clang cross-compile with the iPhoneOS SDK from Xcode (no theos).
#   make            -> build/cicpu.dylib (ad-hoc signed, arm64, iOS 16.0+)
#   make check      -> print the arch/platform/install name and the CIContext symbol refs
# On the phone, copy to /var/jb/usr/lib/cicpu.dylib (see step list in coreimage-plan.md).
# If you re-sign on the phone instead: ldid -S cicpu.dylib

SDK     ?= $(shell xcrun --sdk iphoneos --show-sdk-path)
CC      = $(shell xcrun --sdk iphoneos -f clang)
MINOS   ?= 16.0
INSTALL ?= /var/jb/usr/lib/cicpu.dylib

CFLAGS  = -arch arm64 -isysroot $(SDK) -miphoneos-version-min=$(MINOS) \
          -O2 -Wall -Wextra -fno-objc-arc
LDFLAGS = -dynamiclib -install_name $(INSTALL) -lobjc

all: build/cicpu.dylib

build/cicpu.dylib: cicpu.m
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ cicpu.m
	codesign -f -s - $@

check: build/cicpu.dylib
	file build/cicpu.dylib
	vtool -show-build build/cicpu.dylib | head -12
	nm -u build/cicpu.dylib | grep -i "objc\|dyld\|dl" || true

clean:
	rm -rf build

.PHONY: all check clean
