#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
html2c.py —— 网页源文件 -> ESP8266/html.c

用法：
    python3 tools/html2c.py                 # 默认扫描「原始html/」并生成到 ESP8266/html.c
    python3 tools/html2c.py --no-gzip       # 不压缩（方便串口抓包看明文，体积会大很多）
    python3 tools/html2c.py --src 原始html --out ESP8266/html.c

做了三件事：
  1. UTF-8 源 -> GB18030（与 STM32 串口编码一致），并顺手把 <meta charset> 改成 GB18030；
  2. gzip 预压缩（等级 9、mtime 归零，保证同一份源文件每次生成结果完全一致）；
  3. 输出成 C 数组，全部用 0xNN 数字字面量，规避 \xNN 十六进制转义“吞字符”的坑。

源文件里若有 GB18030 编码不了的字符（比如 emoji），会直接报错并指出行号 ——
这类字符烧进设备只会显示成乱码，宁可早失败。
"""

import argparse
import gzip
import io
import os
import re
import sys

# 源文件名 -> (C 数组名, PageGz 变量名, 页面说明)
PAGES = [
    ("index.html",       "index_gz",    "PAGE_INDEX",  "单呼 / 直接发送页"),
    ("group_call.html",  "groupcall_gz", "PAGE_GROUP", "群呼 + 天气推送页"),
    ("wifi_page.html",   "wifi_gz",     "PAGE_WIFI",   "Wifi 配置页"),
    ("update.html",      "update_gz",   "PAGE_UPDATE", "固件升级页"),
    ("hunt.html",        "hunt_gz",     "PAGE_HUNT",   "追码 / 追频扫描页"),
    ("weather.html",     "weather_gz",  "PAGE_WEATHER","天气推送页"),
]

HEADER = """/***************************************************************************
 * html.c —— 由 tools/html2c.py 自动生成，请勿手工修改
 * 修改网页请改「原始html/」下的 UTF-8 源文件，然后重新运行 html2c.py
 *
 * 编码：内容已转成 GB18030（与 STM32 串口编码一致，中文才能正确下发）
 * 存储：gzip 预压缩 + PROGMEM，只占 Flash 不占 RAM
 * 发送：配合 Content-Encoding: gzip 由浏览器解压，传输量约为原来的 1/4
 * 转义：全部用 0xNN 数字字面量，规避 \\xNN 十六进制转义“吞字符”的坑
 ***************************************************************************/

#include "html.h"

"""


def to_bytes(path, use_gzip):
    src = open(path, encoding="utf-8").read()
    # 页面实际以 GB18030 下发，声明必须跟着改，否则浏览器按 UTF-8 解码会乱码
    src = re.sub(r'<meta\s+charset=["\']?utf-8["\']?\s*>', '<meta charset="GB18030">',
                 src, count=1, flags=re.I)
    try:
        raw = src.encode("gb18030")
    except UnicodeEncodeError as e:
        line = src[:e.start].count("\n") + 1
        ch = src[e.start]
        raise SystemExit("编码失败：%s 第 %d 行有 GB18030 表示不了的字符 %r (U+%04X)，请替换掉再生成"
                         % (path, line, ch, ord(ch)))
    if not use_gzip:
        return raw, raw, False
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(raw)
    packed = buf.getvalue()
    return packed, raw, True


def c_array(name, data, note):
    out = ["// %s —— %s（%d 字节 -> gzip %d 字节）" % (name, note, len(data), len(data)),
           "const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.append("  " + ",".join("0x%02X" % b for b in chunk) + ",")
    if out[-1].endswith(","):
        out[-1] = out[-1][:-1]
    out.append("};")
    return "\n".join(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(root, "原始html"))
    ap.add_argument("--out", default=os.path.join(root, "ESP8266", "html.c"))
    ap.add_argument("--no-gzip", action="store_true")
    args = ap.parse_args()

    blocks, defs = [], []
    total_raw = total_packed = 0
    for fn, arr, page, note in PAGES:
        path = os.path.join(args.src, fn)
        if not os.path.exists(path):
            raise SystemExit("找不到源文件：" + path)
        packed, raw, is_gz = to_bytes(path, not args.no_gzip)
        blocks.append(c_array(arr, packed, note))
        defs.append("const PageGz %s = { %s, %d, %s };" % (page, arr, len(packed), "true" if is_gz else "false"))
        total_raw += len(raw)
        total_packed += len(packed)
        print("%-18s %6d -> %6d 字节  (%.0f%%)" % (fn, len(raw), len(packed), 100.0 * len(packed) / max(len(raw), 1)))

    text = HEADER + "\n\n".join(blocks) + "\n\n" + "\n".join(defs) + "\n"
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print("合计 %d -> %d 字节，已写入 %s" % (total_raw, total_packed, args.out))


if __name__ == "__main__":
    main()
