# 与麒麟桌面 V10 SP1 的 GCC 9 / glibc 2.31 / Qt 5.12 系列接近的构建基线。
# 这是兼容性验证环境，不能代替麒麟 UKUI 实机验收。
FROM ubuntu:20.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config python3 python3-pil \
    qtbase5-dev libqt5svg5-dev qt5-image-formats-plugins \
    libx11-dev libxrandr-dev libdbus-1-dev zlib1g-dev \
    libwayland-dev wayland-protocols \
    xvfb xauth xdotool openbox dbus-x11 fonts-noto-cjk \
    desktop-file-utils librsvg2-bin ca-certificates libssl1.1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
