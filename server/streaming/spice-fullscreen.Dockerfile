FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config patch xz-utils libspice-protocol-dev libdrm-dev \
    libx11-dev libxfixes-dev libxrandr-dev libjpeg-dev libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY .build/downloads/spice-streaming-agent-0.3.tar.xz .
COPY server/patches/spice-streaming-agent-fullscreen.patch .
RUN tar -xf spice-streaming-agent-0.3.tar.xz
WORKDIR /build/spice-streaming-agent-0.3
RUN patch -p1 < ../spice-streaming-agent-fullscreen.patch
RUN ./configure --prefix=/opt/spice-fullscreen --enable-gst-plugin=yes --enable-tests=no && make -j8 && make install

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 libx11-6 libxfixes3 libxrandr2 libdrm2 libjpeg62-turbo \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-ugly gstreamer1.0-tools \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /opt/spice-fullscreen /opt/spice-fullscreen
ENV DISPLAY=:0 XAUTHORITY=/run/spice-xauthority GST_REGISTRY_1_0=/tmp/gst-registry.bin
ENTRYPOINT ["/opt/spice-fullscreen/bin/spice-streaming-agent"]
CMD ["-c","framerate=60","-c","gst.codec=h264","-c","gst.encoder=x264enc","-c","gst.prop=tune=zerolatency","-c","gst.prop=speed-preset=ultrafast","-c","gst.prop=bitrate=20000","-c","gst.prop=key-int-max=120"]
