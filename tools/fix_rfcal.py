#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
fix_rfcal.py —— 修复「换过 Flash 后 Wifi 完全搜不到热点」

症状
----
- 模块程序在跑、串口有输出，但手机【完全搜不到 POCSAG 热点】
- 或者能搜到但信号极弱，贴脸才能连上
- STA 模式扫不到任何附近 Wifi

原因
----
ESP8266 的 RF 校准数据（rfcal / phy_init）存在 Flash 倒数第 4 个扇区：
    4MB 模块 -> 0x3FC000
    1MB 模块 -> 0xFC000
    8MB 模块 -> 0x7FC000
新 Flash 芯片出厂整片是 0xFF，而平时用 Arduino/esptool 烧程序只写程序段，
不会碰这一块 —— 于是 phy 初始化拿不到校准值，射频基本不工作。

用法
----
    # 1) 先体检（只读，不动 Flash，随便跑）
    python3 tools/fix_rfcal.py --port COM3
    python3 tools/fix_rfcal.py --port /dev/ttyUSB0

    # 2) 确认有问题后，加 --write 修复
    python3 tools/fix_rfcal.py --port COM3 --write

    # 3) 已知自己芯片不是 4MB，手动指定容量
    python3 tools/fix_rfcal.py --port COM3 --flash-size 8MB

依赖：python3 -m pip install esptool
"""

import argparse
import os
import subprocess
import sys
import tempfile

# 容量名 -> 字节
SIZES = {
    "512KB": 0x80000,
    "1MB": 0x100000,
    "2MB": 0x200000,
    "4MB": 0x400000,
    "8MB": 0x800000,
    "16MB": 0x1000000,
}


def rfcal_addr(size_bytes):
    """rfcal 固定在真实容量 - 0x4000"""
    return size_bytes - 0x4000


def run(cmd, **kw):
    print("  $ " + " ".join(cmd))
    return subprocess.run([sys.executable, "-m", "esptool"] + cmd, **kw)


def read_flash(port, addr, length, out):
    return run(["--port", port, "read_flash", hex(addr), hex(length), out])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="串口号，如 COM3 或 /dev/ttyUSB0")
    ap.add_argument("--flash-size", default="4MB", choices=sorted(SIZES),
                    help="Flash 真实容量，默认 4MB")
    ap.add_argument("--write", action="store_true", help="确认有问题后，加这个参数才真的写入")
    ap.add_argument("--default-bin", default=None,
                    help="esp_init_data_default.bin 的路径（不给就自动找）")
    args = ap.parse_args()

    size = SIZES[args.flash_size]
    addr = rfcal_addr(size)
    print("Flash 容量：%s，rfcal 地址：0x%06X" % (args.flash_size, addr))

    tmp = os.path.join(tempfile.gettempdir(), "rfcal_now.bin")
    print("\n[1/3] 读取当前 rfcal 区...")
    r = read_flash(args.port, addr, 0x1000, tmp)
    if r.returncode != 0:
        print("读取失败。检查：串口号对不对、模块是否进入下载模式、别的串口工具是否占用。")
        return 1

    with open(tmp, "rb") as f:
        data = f.read()
    blank = all(b == 0xFF for b in data[:128])
    print("      前 128 字节：%s" % data[:16].hex())
    if blank:
        print("      >>> 结论：rfcal 是空的，这就是 Wifi 搜不到的原因，需要修复。")
    else:
        print("      >>> 结论：rfcal 有内容，射频校准值在。")
        print("          如果依然搜不到热点，问题在别处（天线、供电、Flash Size 选错等）。")
        if not args.write:
            return 0

    if not args.write:
        print("\n（只读检查，没有修改任何东西。确认要修复请加 --write）")
        return 0

    print("\n[2/3] 备份当前 rfcal 区...")
    bak = "rfcal_backup_0x%06X.bin" % addr
    r = read_flash(args.port, addr, 0x1000, bak)
    if r.returncode == 0:
        print("      已备份到 %s" % bak)

    print("\n[3/3] 写入默认 RF 校准数据...")
    bin_path = args.default_bin
    if not bin_path:
        # 从已安装的 esp8266 支持包里找，找不到再提示手动指定
        import glob
        pats = [
            os.path.expanduser("~/AppData/Local/Arduino15/packages/esp8266/**/esp_init_data_default.bin"),
            os.path.expanduser("~/Library/Arduino15/packages/esp8266/**/esp_init_data_default.bin"),
            os.path.expanduser("~/.arduino15/packages/esp8266/**/esp_init_data_default.bin"),
            "/usr/share/**/esp_init_data_default.bin",
        ]
        for p in pats:
            hit = glob.glob(p, recursive=True)
            if hit:
                bin_path = hit[0]
                break
    if not bin_path or not os.path.exists(bin_path):
        print("找不到 esp_init_data_default.bin。")
        print("它在 Arduino 的 esp8266 支持包里，路径形如：")
        print("  ~/.arduino15/packages/esp8266/hardware/esp8266/<版本>/tools/sdk/")
        print("找到后用 --default-bin 指定路径再跑一次。")
        return 1
    print("      使用：%s" % bin_path)

    r = run(["--port", args.port, "write_flash", hex(addr), bin_path])
    if r.returncode != 0:
        print("写入失败。")
        return 1

    print("\n完成。重新上电后手机应该能搜到 POCSAG 热点了。")
    print("注意：这一段只是【默认值】，信号强度不如芯片原厂校准的那一版。")
    print("      动手前如果读过原厂数据，在 %s 里，可用 --default-bin 指回去恢复。" % bak)
    return 0


if __name__ == "__main__":
    sys.exit(main())
