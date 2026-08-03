# obsbot-cli only.
#
# Deliberately excludes the GUI: it needs a display socket, the host DBus
# session bus for the tray, and GPU passthrough for the GLSL filters, none of
# which containerize well. The CLI needs only usbfs and V4L2 ioctls.

FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config \
        qt6-base-dev qt6-multimedia-dev libgl1-mesa-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The GUI target is part of the same CMake project, so build only what we ship.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel --target obsbot-cli


FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# lsof backs the "camera in use by another process" check. It only reports
# usefully with --pid=host; see docs/DOCKER.md.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 lsof \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/bin/obsbot-cli /usr/bin/obsbot-cli
COPY --from=build /src/sdk/lib/libdev.so.1.0.2 /usr/lib/libdev.so.1.0.2
RUN ln -s libdev.so.1.0.2 /usr/lib/libdev.so.1 \
    && ln -s libdev.so.1.0.2 /usr/lib/libdev.so \
    && ldconfig \
    # CMake sets INSTALL_RPATH=/usr/lib, but the binary is copied from the
    # build tree where BUILD_RPATH points at /src/sdk/lib. Fail the image build
    # now rather than shipping something that cannot resolve the SDK.
    && ldd /usr/bin/obsbot-cli && ! ldd /usr/bin/obsbot-cli | grep 'not found'

ENTRYPOINT ["/usr/bin/obsbot-cli"]
