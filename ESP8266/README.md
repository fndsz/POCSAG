# POCSAG ESP8266 WebUpdate 版

摩托罗拉寻呼发射器 Web 控制端，已启用 OTA 网页固件升级功能。

## Arduino IDE 编译设置

| 选项 | 设置值 |
|------|--------|
| 开发板 | Generic ESP8266 Module |
| Flash Size | **4MB (FS:2MB OTA:~1019KB)** ← 必须选这项 |
| Flash Mode | QIO / DIO 均可 |
| CPU Frequency | 80 MHz |
| Flash Frequency | 40 MHz |
| Upload Speed | 115200 |
| Reset Method | dtr |
| Debug Port | Disabled |
| Debug Level | None |
| lwIP Variant | v2 Lower Memory |
| VTABLE | Flash |
| Exceptions | Legacy |
| Builtin LED | 2 |
| Erase Flash | Only Sketch |
| SSL Support | All SSL ciphers |

## 依赖库

- ESP8266 core for Arduino (2.7.x / 3.x)
  - 安装方式：文件 → 首选项 → 附加开发板管理器网址，添加：
    ```
    http://arduino.esp8266.com/stable/package_esp8266com_index.json
    ```
  - 然后：工具 → 开发板 → 开发板管理器 → 搜索 `esp8266` → 安装

## 使用说明

1. 打开 `ESP8266.ino`
2. 按上表设置编译选项
3. 编译上传（首次用 USB 串口）
4. 设备启动后，连接热点 `POCSAG`（或已配置的路由器）
5. 浏览器访问设备 IP，所有页面底部导航栏都有「升级」入口
6. 进入升级页面，选择 `.bin` 文件上传即可完成 OTA 升级

## OTA 升级注意事项

- 只能传 Arduino「项目 → 导出已编译的二进制文件」得到的程序 bin
- 不能传打包脚本合成的 4MB 整片镜像
- Flash Size 必须是 4MB (FS:2MB OTA:~1019KB)
- 上传前页面会自动校验文件合法性

## 升级页面说明

`/webupdate` 页面（`html.c` 中的 `update_gz`）已重构，主要变化：

- CSS 去重：删除了原始段与 v3 段重复的 4 套主题变量定义
- 新增「升级前准备」4 步结构化流程卡片（`.steps` 有序列表）
- 品牌头版本号同步：`<span class="tag">` 加载固件信息后自动显示版本号
- 进度显示增强：同时显示百分比与已传/总量绝对数值
- 信息卡扩展：新增「已运行」「连接」两个运行时状态字段，并检测升级挂起状态

## 修改升级页面

页面源文件是 `webupdate.html`（GB18030 编码），**不要直接改 `html.c`**。
改完后按以下步骤重新生成压缩数据：

```bash
# 1. 转成 GB18030 字节流
python3 -c "open('/tmp/w.gb','wb').write(open('webupdate.html',encoding='gb18030').read().encode('gb18030'))"

# 2. gzip 压缩
gzip -9 -c /tmp/w.gb > /tmp/w.gz

# 3. 转成 C 字节数组，覆盖 html.c 中的 update_gz
python3 -c "
import sys
b=open('/tmp/w.gz','rb').read()
out='const uint8_t update_gz[] PROGMEM = {\n'
out+='\n'.join('  '+', '.join('0x%02X'%x for x in b[i:i+12])+',' for i in range(0,len(b),12))
out+='\n};\n'
open('update_gz.inc','w').write(out)
print('长度',len(b))
"
```

校验 DOM 引用是否完整：

```bash
python3 tools_check_refs.py
```

## 项目结构

```
ESP8266.ino        — 主程序
html.h             — 页面声明头文件
html.c             — 预压缩的 HTML 页面数据（gzip，存 Flash）
weather_gbk.h      — GBK 转码表（天气用）
webupdate.html     — 升级页面源文件（GB18030），改页面改这个
tools_check_refs.py — 校验页面 DOM 引用完整性
```
