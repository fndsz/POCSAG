/***************************************************************************
 * html.h
 * 适用平台：ESP8266（4MB Flash / FS:2MB / OTA:~1019KB）
 * 编译工具：Arduino（esp8266 core 2.7.x / 3.x 均可）
 *
 * 原始项目：359303303267/STM32_POCSAG_Transmit
 * 本文件在原版基础上重做：手机自适应、10 路群呼、天气推送、gzip 预压缩
 * 原作者：小小小日天；本项目遵循 GPL 协议，仅供个人 DIY 研究，商用需授权
 ***************************************************************************/
#ifndef _HTML_H_
#define _HTML_H_

#include <pgmspace.h>
#include <stdint.h>

// 页面描述：预压缩数据(存 Flash) + 长度 + 是否 gzip。
// 结构体本身很小，放 RAM 即可，省去 PROGMEM 结构体读写的坑。
typedef struct {
  const uint8_t* data;
  uint32_t       len;
  bool           gz;
} PageGz;

extern const PageGz PAGE_INDEX;   // 单呼/直接发送页
extern const PageGz PAGE_GROUP;   // 10 路群呼 + 天气推送页
extern const PageGz PAGE_WIFI;   // Wifi 配置页
extern const PageGz PAGE_UPDATE;   // 固件升级页

#endif
