# sys-antinag - builds the sysmodule and the overlay and assembles out/ (copy out/* to the SD card root)
# needs: devkitPro with libnx (export DEVKITPRO=/opt/devkitpro)
# version = git tag (vX.Y.Z) on release builds, otherwise tag-commits-sha, e.g. v0.1.0-3-gabc1234 (+ -dirty)
export VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
export CUSTOM_DEFINES := -DANTINAG_VERSION=\"$(VERSION)\"

all:
	$(MAKE) -C sysmod
	$(MAKE) -C overlay
	@rm -rf out && mkdir -p out
	@cp -R sysmod/out/* out/
	@cp -R overlay/out/* out/

# host-side checks: unit test of the patch logic (+ optional real nifm images: make test IMGS="a/img.bin b/img.bin")
test:
	gcc -std=c11 -Wall -Wextra -fsanitize=address,undefined -Isysmod/include test/test_antinag.c -o /tmp/test_antinag && /tmp/test_antinag $(IMGS)

clean:
	$(MAKE) -C sysmod clean
	$(MAKE) -C overlay clean
	@rm -rf out

dist: all
	@rm -f sys-antinag.zip && cd out && zip -r ../sys-antinag.zip ./*

.PHONY: all test clean dist
