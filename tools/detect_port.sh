#!/usr/bin/env bash
# 自动探测 ESP32-S3 串口设备（macOS / Linux）
# 被 Makefile 调用，输出第一个匹配的串口路径；未找到则静默退出（exit 0，无输出）。
set -u

found=""

# ---- Linux: 常见 USB 转串口 ----
if [[ -d /dev ]]; then
    # 按稳定性排序：ttyACM（USB CDC，如原生 USB）优先于 ttyUSB（CH340/CP2102）
    for dev in /dev/ttyACM[0-9] /dev/ttyUSB[0-9]; do
        if [[ -e "$dev" ]]; then
            found="$dev"
            break
        fi
    done
fi

# ---- macOS: cu.* 优先于 tty.*（cu 可被独占打开，推荐用于烧录） ----
if [[ -z "$found" && "$(uname -s 2>/dev/null)" == "Darwin" ]]; then
    # SLAB_USBtoUART（CP210x）、usbmodem（原生 USB / CH340）、usbserial（FTDI）
    for dev in /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem* /dev/cu.usbserial*; do
        if [[ -e "$dev" ]]; then
            found="$dev"
            break
        fi
    done
fi

if [[ -n "$found" ]]; then
    printf '%s' "$found"
fi
exit 0
