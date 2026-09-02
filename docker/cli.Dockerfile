# obsbot-cli only.
#
# Deliberately excludes the GUI: it needs a display socket, the host DBus
# session bus for the tray, and GPU passthrough for the GLSL filters, none of
# which containerize well. The CLI needs only usbfs and V4L2 ioctls.

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# -DBUILD_GUI=OFF drops the project's find_package(Qt6), so this image needs no
# Qt packages at all -- the CLI links only the vendored SDK.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF \
    && cmake --build build --parallel --target obsbot-cli

# Stage the SDK at a fixed path so the runtime stage need not name a version.
# sdk/lib is a symlink into the chosen sdk/<version>/lib, and COPY --from
# resolves a symlinked source directory inconsistently, so flatten it here
# with cp -a, which keeps the relative libdev.so and libdev.so.1 symlinks the
# loader needs rather than dereferencing them into duplicate 26MB copies.
RUN mkdir -p /sdklib && cp -a "$(readlink -f sdk/lib)/." /sdklib/


FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# lsof backs the "camera in use by another process" check. It only reports
# usefully with --pid=host; see docs/DOCKER.md.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 lsof \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/bin/obsbot-cli /usr/bin/obsbot-cli
# Copy the staged SDK rather than a pinned filename: the vendored SDK version
# changes (v1.0.2 shipped libdev.so.1.0.2, v2.1.0_8 ships libdev.so.1.0.0) and
# COPY cannot expand a shell substitution, so a pinned name would break the
# image build on every SDK upgrade.
COPY --from=build /sdklib/ /usr/lib/
RUN ldconfig \
    # CMake sets INSTALL_RPATH=/usr/lib, but the binary is copied from the
    # build tree where BUILD_RPATH points at /src/sdk/lib. Fail the image build
    # now rather than shipping something that cannot resolve the SDK.
    && ldd /usr/bin/obsbot-cli && ! ldd /usr/bin/obsbot-cli | grep 'not found'

ENTRYPOINT ["/usr/bin/obsbot-cli"]
