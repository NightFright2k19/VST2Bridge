# Build and test image of the CI (check.yml and build.yml): mingw-w64, Wine (64 + 32 bit) with an initialised prefix,
# Xvfb and the tools of the patcher checks. Built by .github/workflows/ci-image.yml; the tag is a hash of this file,
# so changing the file (or the base image digest you pin here) makes the next run build a new image.
FROM ubuntu:24.04

LABEL org.opencontainers.image.source="https://github.com/NightFright2k19/VST2Bridge"
LABEL org.opencontainers.image.description="VST2Bridge CI: mingw-w64, Wine, Xvfb"

ENV DEBIAN_FRONTEND=noninteractive \
    WINEDEBUG=-all \
    WINEPREFIX=/opt/wineprefix

# no recommends: they pull in hundreds of MB (fonts, GStreamer ...) the tests do not need
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git make gcc libc6-dev libxcb1-dev gcc-mingw-w64 file zip python3 python3-tk \
      wine wine64 wine32:i386 xvfb xauth \
 && rm -rf /var/lib/apt/lists/*

# initialise the Wine prefix once, here, instead of in every run
RUN xvfb-run -a wine wineboot -i; wineserver -w; wineserver -k || true
