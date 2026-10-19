#!/usr/bin/env bash
# 「J2」把工作区源码/脚本同步进验证容器
#
# 为什么需要它：
#   Docker Desktop for macOS 的 bind mount 在**原子替换**（写临时文件再 rename，
#   很多编辑器与工具链都这么做）后，容器内可能仍看到旧 inode 的内容。
#   实测症状：容器里 grep 到的是旧代码，构建产物与宿主编辑不一致，
#   "改了没生效 / 构建通过但行为是旧的"，极易把排查带偏。
#   构建前用 docker cp 强制同步一次，代价很小。
#
# 用法：bash scripts/sync-to-container.sh [容器名]
set -eu

CONTAINER="${1:-jie2-verify}"

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "容器 $CONTAINER 未运行，正在启动…"
    docker start "$CONTAINER" >/dev/null
    sleep 2
fi

echo "== 同步到 $CONTAINER =="
for dir in src scripts tests packaging third_party; do
    [ -e "$dir" ] || continue
    docker cp "$dir/." "$CONTAINER:/work/$dir/" >/dev/null
    echo "  synced: $dir"
done
for file in CMakeLists.txt README.md; do
    [ -e "$file" ] || continue
    docker cp "$file" "$CONTAINER:/work/$file" >/dev/null
    echo "  synced: $file"
done

# 校验关键文件一致（不一致时明确报错，而不是让构建悄悄用旧代码）
fail=0
check() {
    local path="$1"
    [ -e "$path" ] || return 0
    local host_md5 container_md5
    host_md5="$(md5 -q "$path")"
    container_md5="$(docker exec "$CONTAINER" md5sum "/work/$path" | awk '{print $1}')"
    if [ "$host_md5" != "$container_md5" ]; then
        echo "  MISMATCH: $path ($host_md5 vs $container_md5)"
        fail=1
    fi
}
check "src/ui/main.cpp"
check "src/ui/tray.cpp"
check "src/ui/clothesline.cpp"
check "src/core/image_ops.cpp"

if [ "$fail" -ne 0 ]; then
    echo "同步校验失败 —— 容器里仍是旧内容，请重试或重启容器。"
    exit 1
fi
echo "同步完成且校验一致。"
