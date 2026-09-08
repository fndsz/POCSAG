/***************************************************************************
 * ESP8266.ino —— 摩托罗拉寻呼发射器 Web 控制端（4MB Flash 重制版）
 *
 * 适用平台：ESP8266-01S（必须换成 4MB Flash 才能用本固件）
 * 编译工具：Arduino + esp8266 core 2.7.x / 3.x
 * 板卡配置：Generic ESP8266 Module
 *           Flash Size : 4MB (FS:2MB OTA:~1019KB)
 *           SSL Support: All SSL ciphers (most compatible)   ← 天气推送要用到 HTTPS
 *
 * 原始项目：359303267/STM32_POCSAG_Transmit（原作者：小小小日天）
 * 本次改动：
 *   1. 全部页面重做为手机自适应（viewport + flex/grid 响应式布局）
 *   2. 群呼由「20 个复选框」改为「10 行表格」：勾选 + 序号 + 频率 + 地址码 + 速率 + 相位
 *   3. 新增天气推送：Open-Meteo(免Key) / 心知天气 / 和风天气，支持每日定时与手动触发
 *   4. 网页改为 PROGMEM 分片 chunked 发送，发送过程几乎不占用堆内存
 *   5. 配置持久化到 LittleFS，掉电不丢失
 *   6. 发射改为后台队列 + 非阻塞等待，不再卡住 Web 服务
 *
 * 本项目遵循 GPL 协议，个人 DIY 免费，未经授权不得商用。
 *
 * 注意：无线发送前必须接好天线或假负载，否则会烧毁 RF 功放；
 *       发射功率较大，请遵守当地无线电法规。
 ***************************************************************************/

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <time.h>
#include <ctype.h>
#include <stddef.h>      // offsetof（EEPROM 校验要用）

#include "html.h"
#include "weather_gbk.h"

/* ==================== 用户可改区 ==================== */
#define AP_SSID   "POCSAG"        // AP 热点名
#define AP_PSW    ""              // AP 密码，8 位以上或留空
#define STA_SSID  ""              // 家里/单位路由器的名称，留空则只开 AP
#define STA_PSK   ""              // 路由器密码

#define DEBUG 0                   // 置 1 可从串口看到调试信息（注：串口已与 STM32 共用）
/* ==================================================== */

#define FW_VER      "V2.0-4M"
#define PAGER_NUM   10
#define TX_QUEUE    12            // 发送队列深度
#define TX_LINE_MAX 260           // 单条串口命令最大长度（STM32 缓冲区 400 字节）
#define MSG_MAX     120           // 消息最大字符数（汉字按 2 字节算，120 字符 = 240 字节）
#define CFG_FILE    "/pocsag.cfg"
#define FREQ_SETTLE_MS 400        // 切频后等待 RF 模块锁定的时间
#define WX_CHECK_MS   30000       // 定时推送的检查周期
#define WX_SLOTS      3           // 每日推送时间点数量
#define SCAN_KEEP     40          // 最多参与排序的网络数（再多也用不上，还费内存）
#define SCAN_SHOW     12          // 最多返回给网页的网络数（按信号强度取前几名）
#define SCAN_TIMEOUT  20000       // 扫描超时保护，卡住就放弃
#define OTA_FLUSH_MS  2000        // OTA 成功后，等多久再重启（留给 TCP 把响应发完）

#define MYFS LittleFS

#if DEBUG
  #define DBG(...) Serial1.printf(__VA_ARGS__)
#else
  #define DBG(...) do {} while(0)
#endif

IPAddress apIP(192, 168, 4, 1);
IPAddress apGW(192, 168, 4, 1);
IPAddress apMask(255, 255, 255, 0);

ESP8266WebServer server(80);

String ssid     = STA_SSID;
String password = STA_PSK;
String comdata  = "";

/* ---------------- 配置结构 ---------------- */
struct Pager {
  char freq[10];    // 8 字符，如 "152.8250"
  char addr[8];     // 7 位地址码
  char rate;        // L=512 / H=1200 / S=2400
  char phase;       // P=正相位 / N=负相位
  uint8_t sel;      // 是否参与群呼（每行最前面的勾选）
  uint8_t wx;       // 是否接收天气推送（每行「天气」列的勾选）
};

struct Cfg {
  Pager pg[PAGER_NUM];
  char  type;       // N=数字机 / T=汉字机
  char  beep;       // '0'..'3'
  uint16_t gap;     // 固定间隔（自适应关闭时用）
  uint8_t adaptGap; // 1=按消息长度估算等待（快很多） 0=用固定 gap
  uint8_t grpFreq;  // 1=同频的行排在一起发，减少切频次数
  uint8_t wxSrc;    // 0=Open-Meteo 1=心知 2=和风
  char  wxKey[40];
  char  wxCity[32];
  char  wxName[24]; // 天气里显示的城市名（可留空：留空则用接口返回的名字）
  uint8_t wxHour[WX_SLOTS];   // 每日推送时间，最多三组
  uint8_t wxMin[WX_SLOTS];
  uint8_t wxOn[WX_SLOTS];     // 每组是否启用（默认只开第一组）
  uint8_t wxAuto;
  char  staSsid[33];  // 记住路由器账号，重启后自动重连（天气推送要用）
  char  staPsk[65];
} cfg;

/* ---------------- 发送队列 ---------------- */
struct TxJob {
  char freq[10];
  char line[TX_LINE_MAX];
  uint32_t wait;    // 本条发射后要等多久（按消息长度逐条算，不再一刀切）
};

TxJob   txq[TX_QUEUE];
uint8_t qHead = 0, qTail = 0, qCount = 0;
uint8_t txTotal = 0, txDone = 0;
char   txLastFreq[10] = "";       // 上一次切到的频率，同频可跳过 #SET+FREQ
uint8_t txState = 0;              // 0=空闲 1=等待切频 2=等待发射完成
uint32_t txWaitUntil = 0;
volatile bool txAcked = false;    // 收到 STM32 的 #TXOK（需给 STM32 打补丁，见 README）

/* ---------------- 天气 ---------------- */
struct Wx {
  bool    valid = false;
  String  cond;      // 天气现象（UTF-8）
  String  windDir;   // 风向（UTF-8）
  String  city;      // 接口返回的城市名（UTF-8），Open-Meteo 没有这个字段
  int     temp = 0;
  int     hum  = -1;
  int     lvl  = -1; // 蒲福风级
  uint32_t at  = 0;  // 获取时刻
  String  text;      // 上次生成的人类可读预览（UTF-8）
} wx;

uint32_t lastWxDay = 0;           // 上次推送的日期：YYYYMMDD
uint8_t  wxDoneMask = 0;          // 当天各时间点是否已推送（bit i = 第 i 组）
uint32_t lastWxCheck = 0;
bool     ntpDone = false;


/* =====================================================================
 *  通用小工具
 * ===================================================================== */

// 显式前向声明：不依赖 Arduino IDE 自动补声明，换个编译环境也不会翻车
void   staConnect();
void   staDisconnect();
void   staBegin(const String& s, const String& p);
bool   fsBegin();
bool   eeSave();
bool   eeLoad(String& s, String& p);

enum { STA_IDLE = 0, STA_TRYING = 1, STA_OK = 2, STA_FAIL = 3 };
#define STA_TIMEOUT 20000       // 20 秒判超时（连一个存在的网络通常 3~8 秒）
#define STA_RETRY_MS 30000      // 自动重连起始间隔：别太频繁，否则会把热点上的手机踢掉
#define STA_RETRY_MAX 300000UL  // 退避上限 5 分钟

// 连接状态机（/wifistat 与 staTick 都要读，放在这里保证可见）
static uint8_t   staState  = STA_IDLE;
static uint32_t  staTryAt  = 0;
static bool      staManualOff = false;   // 用户手动断开过，就不再自动重连
static bool      fsOk         = false;   // 文件系统是否可用（供网页诊断）
static bool      eeUsed       = false;   // 本次配置的 Wifi 凭据是否来自 EEPROM 兜底
static bool      staSdkHasCred = false;  // SDK 里是否已写入当前这组凭据
static bool      staAuto   = false;      // 本次连接是否由自动重连发起
static uint8_t   staFailCnt = 0;         // 连续失败次数，用于退避
static uint32_t  staBackoff = STA_RETRY_MS;
static String    staWhy;        // 失败原因（中文，直接给网页显示）

bool   fetchWx(String* err);
int    pushWx();
String buildWxText(bool numeric);

String urlEnc(const String& s) {
  String o;
  o.reserve(s.length() * 2);
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); o += buf; }
  }
  return o;
}

String urlDec(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length()) {
      char h[3] = { s[i + 1], s[i + 2], 0 };
      o += (char)strtoul(h, NULL, 16);
      i += 2;
    } else if (s[i] == '+') o += ' ';
    else o += s[i];
  }
  return o;
}

// 在 "&k=v&k=v" 形式的串里取值，避免 f1 与 f10 前缀冲突
String getKV(const String& s, const char* key) {
  String k = String("&") + key + "=";
  int p = s.indexOf(k);
  if (p < 0) return String();
  int e = s.indexOf('&', p + k.length());
  if (e < 0) e = s.length();
  return s.substring(p + k.length(), e);
}

String argVal(const char* name, const String& def = String()) {
  for (int i = 0; i < server.args(); i++)
    if (server.argName(i) == name) return server.arg(i);
  return def;
}

bool argHas(const char* name) {
  for (int i = 0; i < server.args(); i++)
    if (server.argName(i) == name) return true;
  return false;
}

// 频率规整成 STM32 要求的 8 字节定长，如 152.8250
bool normFreq(const String& in, char* out) {
  String s = in;
  s.trim();
  if (!s.length()) return false;
  float f = s.toFloat();
  if (f < 136.0f || f > 174.0f) return false;
  snprintf(out, 10, "%.4f", (double)f);
  return strlen(out) == 8;
}

// 地址码规整成 7 位定长
bool normAddr(const String& in, char* out) {
  String s = in;
  s.trim();
  if (!s.length()) return false;
  unsigned long v = strtoul(s.c_str(), NULL, 10);
  if (v < 1 || v > 2097151UL) return false;
  snprintf(out, 8, "%07lu", v);
  return true;
}

void copyStr(char* dst, const String& src, size_t cap) {
  size_t n = src.length();
  if (n > cap - 1) n = cap - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = 0;
}

// JSON 取字段，支持 "key":"str" 与 "key":number，可指定起始搜索位置
String jval(const String& s, const char* key, int from = 0) {
  String k = String("\"") + key + "\":";
  int p = s.indexOf(k, from);
  if (p < 0) return String();
  p += k.length();
  while (p < (int)s.length() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
  String out;
  if (p < (int)s.length() && s[p] == '"') {
    p++;
    while (p < (int)s.length() && s[p] != '"') {
      if (s[p] == '\\') {
        p++;
        if (p >= (int)s.length()) break;
        char c = s[p++];
        if (c == 'n') out += '\n';
        else if (c == 't') out += '\t';
        else if (c == 'r') out += '\r';
        else if (c == 'u') {
          if (p + 4 <= (int)s.length()) {
            char h[5] = { s[p], s[p + 1], s[p + 2], s[p + 3], 0 };
            unsigned long cp = strtoul(h, NULL, 16);
            p += 4;
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
          }
        } else out += c;
      } else out += s[p++];
    }
  } else {
    while (p < (int)s.length() && (isdigit((unsigned char)s[p]) || s[p] == '-' || s[p] == '.' || s[p] == '+'))
      out += s[p++];
  }
  return out;
}

// JSON 字符串转义（用于往 /cfg 里塞中文预览）
// JSON 里的字符串必须是合法 UTF-8，否则浏览器端 JSON.parse() 会整体失败。
// 路由器广播的 SSID 是原始字节流，个别老路由会用 GBK 而不是 UTF-8，
// 这种非法序列会让整个扫描结果解析不出来。这里把非法字节换成 U+FFFD，
// 名字虽显示成问号，但界面和连接流程都不受影响。
#define UTF8_REPL "\357\277\275"      // U+FFFD 替换字符

String utf8Safe(const String& in) {
  String o;
  o.reserve(in.length() + 8);
  const char* s = in.c_str();
  size_t i = 0, n = in.length();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { o += (char)c; i++; continue; }

    size_t need;
    if      ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if ((c & 0xF8) == 0xF0) need = 3;
    else { o += UTF8_REPL; i++; continue; }     // 不可能作为首字节的，单个替换

    // 数一下首字节后面紧跟着几个合法的 continuation，这就是「最大合法子块」。
    // 截断或断了的部分按标准只换成一个 U+FFFD，而不是每个字节换一个 ——
    // 否则末尾被截的序列会吐出一串问号，长度也对不上。
    size_t have = 0;
    while (have < need && (i + 1 + have) < n &&
           ((unsigned char)s[i + 1 + have] & 0xC0) == 0x80) have++;

    if (have == need) { o.concat(s + i, need + 1); i += need + 1; }   // 完整合法，原样保留
    else { o += UTF8_REPL; i += 1 + have; }                           // 一个子块只换一个
  }
  return o;
}

String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04X", c); o += b; }
    else o += (char)c;
  }
  return o;
}


/* =====================================================================
 *  UTF-8 -> GB18030（只转天气词表里收录的词汇，其余丢弃）
 * ===================================================================== */
size_t utf8ToGbk(const String& in, char* out, size_t maxOut) {
  const char* s = in.c_str();
  size_t n = in.length(), i = 0, o = 0;
  while (i < n) {
    if (o + 8 >= maxOut) break;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { out[o++] = (char)c; i++; continue; }

    bool hit = false;
    const char* p = WX_GBK_BLOB;
    while (true) {
      if (!pgm_read_byte(p)) break;                 // 读到空 key，表结束
      size_t kl = 0;
      while (pgm_read_byte(p + kl)) kl++;            // key 长度
      const char* v = p + kl + 1;
      size_t vl = 0;
      while (pgm_read_byte(v + vl)) vl++;            // value 长度

      bool eq = (i + kl <= n);
      for (size_t q = 0; eq && q < kl; q++)
        if ((unsigned char)s[i + q] != (unsigned char)pgm_read_byte(p + q)) eq = false;

      if (eq) {
        for (size_t q = 0; q < vl && o + 8 < maxOut; q++) out[o++] = (char)pgm_read_byte(v + q);
        i += kl;
        hit = true;
        break;
      }
      p = v + vl + 1;
    }

    if (!hit) {                                      // 未收录：整个 UTF-8 字符跳过
      size_t adv = 1;
      if (c >= 0xF0) adv = 4;
      else if (c >= 0xE0) adv = 3;
      else if (c >= 0xC0) adv = 2;
      if (i + adv > n) adv = 1;
      i += adv;
    }
  }
  out[o] = 0;
  return o;
}


/* =====================================================================
 *  配置存取（LittleFS）
 * ===================================================================== */
void cfgDefault() {
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    snprintf(cfg.pg[i].freq, sizeof(cfg.pg[i].freq), "152.8250");
    snprintf(cfg.pg[i].addr, sizeof(cfg.pg[i].addr), "%07lu", 1UL + i);
    cfg.pg[i].rate  = 'H';
    cfg.pg[i].phase = 'P';
    cfg.pg[i].sel   = (i == 0) ? 1 : 0;
    cfg.pg[i].wx    = (i == 0) ? 1 : 0;   // 默认第 1 行收天气，其余关掉，避免误推
  }
  cfg.type  = 'T';
  cfg.beep  = '0';
  cfg.gap   = 8000;
  cfg.adaptGap = 1;         // 默认自适应：短消息不再空等 8 秒
  cfg.grpFreq  = 1;         // 默认同频归组：切频次数从「每行一次」降到「每个频点一次」
  cfg.wxSrc = 0;
  cfg.wxKey[0]  = 0;
  snprintf(cfg.wxCity, sizeof(cfg.wxCity), "22.8170,108.3665");   // 广西南宁
  // 城市名默认填上，而不是留空：三个数据源里只有心知天气会回传城市名，
  // Open-Meteo（默认源，免 Key）和和风都不返回，留空的话城市就永远不显示，
  // 看上去像功能没生效。默认位置是南宁，这里跟着填南宁，用户改位置时一并改掉。
  copyStr(cfg.wxName, "\xe5\x8d\x97\xe5\xae\x81", sizeof(cfg.wxName));   // 南宁
  // 三个时间点：默认只启用第一个（07:30），后两个给了常用值但默认关闭，
  // 免得用户还没设置就一天推三次。
  const uint8_t dh[WX_SLOTS] = {7, 12, 18};
  const uint8_t dm[WX_SLOTS] = {30, 0, 0};
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    cfg.wxHour[i] = dh[i];
    cfg.wxMin[i]  = dm[i];
    cfg.wxOn[i]   = (i == 0) ? 1 : 0;
  }
  cfg.wxAuto = 0;
  copyStr(cfg.staSsid, String(STA_SSID), sizeof(cfg.staSsid));
  copyStr(cfg.staPsk,  String(STA_PSK),  sizeof(cfg.staPsk));
}

// 返回值：凭据是否落盘成功（EEPROM 与文件系统任一成功即算成功）。
// 全局 fsOk 另外记录文件系统这一路的结果，网页据此提示「只有 Wifi 存住了」。
bool cfgSave() {
  // EEPROM 不依赖文件系统，先写它 —— 保证 Wifi 凭据在任何布局下都留得住
  bool okEe = eeSave();

  fsOk = fsBegin();
  if (!fsOk) { DBG("[cfg] fs unavailable, wifi kept in EEPROM only\n"); return okEe; }
  File f = MYFS.open(CFG_FILE, "w");
  if (!f) { DBG("[cfg] open for write failed\n"); fsOk = false; return okEe; }
  f.print("ver=2");
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    f.printf("&f%d=%s&a%d=%s&r%d=%c&p%d=%c&s%d=%d&w%d=%d",
             i, cfg.pg[i].freq, i, cfg.pg[i].addr,
             i, cfg.pg[i].rate, i, cfg.pg[i].phase,
             i, cfg.pg[i].sel ? 1 : 0, i, cfg.pg[i].wx ? 1 : 0);
  }
  f.printf("&type=%c&beep=%c&gap=%u&adg=%u&gfr=%u",
           cfg.type, cfg.beep, cfg.gap, cfg.adaptGap, cfg.grpFreq);
  f.printf("&wsrc=%u&wkey=%s&wcity=%s&wname=%s&wauto=%u",
           cfg.wxSrc, urlEnc(String(cfg.wxKey)).c_str(), urlEnc(String(cfg.wxCity)).c_str(),
           urlEnc(String(cfg.wxName)).c_str(), cfg.wxAuto);
  for (uint8_t i = 0; i < WX_SLOTS; i++)
    f.printf("&whh%u=%u&wmm%u=%u&won%u=%u",
             i, cfg.wxHour[i], i, cfg.wxMin[i], i, cfg.wxOn[i] ? 1 : 0);
  f.printf("&ssid=%s&psk=%s", urlEnc(String(cfg.staSsid)).c_str(), urlEnc(String(cfg.staPsk)).c_str());
  f.close();
  DBG("[cfg] saved\n");
  return true;                 // 文件系统写成功
}

/* 挂载文件系统，挂不上就格式化一次再试。
 *
 * 这一步原来没有，是「Wifi 重启后丢失」的根因：
 * 串口烧录整片镜像时若不带文件系统镜像（默认就不带），FS 区域是一片 0xFF，
 * LittleFS 挂载必然失败。原来 cfgLoad() 挂不上就直接 return（配置全丢），
 * cfgSave() 更坑 —— open 失败后一路静默走完，网页上照样提示「已提交」，
 * 实际一个字节都没写进去。于是每次重启都是出厂设置。 */
bool fsTried = false;    // 本次运行是否已尝试过格式化（重启自然归零）

bool fsBegin() {
  if (MYFS.begin()) return true;
  // 只试一次格式化：Flash 真坏了的话，每次启动都格式化一遍只会让开机更慢。
  if (fsTried) return false;
  fsTried = true;
  DBG("[fs] mount failed, formatting\n");
  if (!MYFS.format()) { DBG("[fs] format failed\n"); return false; }
  delay(50);
  if (MYFS.begin()) { DBG("[fs] formatted ok\n"); return true; }
  DBG("[fs] still cannot mount\n");
  return false;
}

/* ---------------------------------------------------------------------
 *  Wifi 凭据的 EEPROM 兜底通道
 *
 *  为什么需要它：LittleFS 的分区完全由 Arduino 的「Flash Size」选项决定 ——
 *  选了 4MB(FS:none)、1MB 之类，FS 分区要么不存在、要么位置跟实际芯片对不上，
 *  LittleFS 挂载必然失败，配置就一个字节都存不住，表现为「掉电丢账号密码」。
 *
 *  EEPROM 不一样：它由链接脚本的 _EEPROM_start 决定，
 *  而且在【所有】布局里都有定义（包括那几个 FS:none 的布局），
 *  地址永远跟编译时一致。所以用它兜底存 Wifi 凭据，不管 Flash Size 怎么选都能存住。
 *
 *  只在保存配置时写一次，不是高频操作，4KB 扇区的擦写寿命完全够。
 * ------------------------------------------------------------------- */
#define EE_MAGIC 0x5041          // 'P','A' —— 判断这块数据是不是我们写的
#define EE_VER   1

struct EEWifi {
  uint16_t magic;
  uint8_t  ver;
  char     ssid[33];             // 32 字符 + 结尾 0
  char     psk[65];              // 64 字符 + 结尾 0
  uint8_t  xor8;                 // 前面所有字节的异或校验
};

bool eeSave() {
  EEWifi e;
  memset(&e, 0, sizeof(e));
  e.magic = EE_MAGIC;
  e.ver   = EE_VER;
  snprintf(e.ssid, sizeof(e.ssid), "%s", cfg.staSsid);
  snprintf(e.psk,  sizeof(e.psk),  "%s", cfg.staPsk);
  e.ssid[sizeof(e.ssid) - 1] = 0;
  e.psk[sizeof(e.psk) - 1]   = 0;

  const uint8_t* p = (const uint8_t*)&e;
  uint8_t x = 0;
  for (size_t i = 0; i < offsetof(EEWifi, xor8); i++) x ^= p[i];
  e.xor8 = x;

  EEPROM.begin(sizeof(e) + 4);
  for (size_t i = 0; i < sizeof(e); i++) EEPROM.write((int)i, p[i]);
  bool ok = EEPROM.commit();
  EEPROM.end();
  DBG("[ee] save %s\n", ok ? "ok" : "failed");
  return ok;
}

bool eeLoad(String& s, String& p) {
  EEWifi e;
  EEPROM.begin(sizeof(e) + 4);
  uint8_t* q = (uint8_t*)&e;
  for (size_t i = 0; i < sizeof(e); i++) q[i] = EEPROM.read((int)i);
  EEPROM.end();

  if (e.magic != EE_MAGIC || e.ver != EE_VER) return false;
  const uint8_t* pp = (const uint8_t*)&e;
  uint8_t x = 0;
  for (size_t i = 0; i < offsetof(EEWifi, xor8); i++) x ^= pp[i];
  if (x != e.xor8) return false;                 // 数据坏了，别拿去用

  e.ssid[sizeof(e.ssid) - 1] = 0;
  e.psk[sizeof(e.psk) - 1]   = 0;
  s = String(e.ssid);
  p = String(e.psk);
  return true;
}

// 从 EEPROM 兜底恢复 Wifi 凭据。放在所有读取路径的末尾统一做一次，
// 这样不管文件系统是挂不上、文件不存在、还是文件里恰好没写凭据，都能兜住。
void cfgLoadWifiFallback() {
  if (strlen(cfg.staSsid)) return;              // 文件系统里已经有了，不用兜底
  String es, ep;
  if (!eeLoad(es, ep)) return;
  copyStr(cfg.staSsid, es, sizeof(cfg.staSsid));
  copyStr(cfg.staPsk,  ep, sizeof(cfg.staPsk));
  eeUsed = true;
  DBG("[cfg] wifi restored from EEPROM\n");
}

void cfgLoad() {
  cfgDefault();
  fsOk = fsBegin();
  if (!fsOk) {
    // 文件系统用不了（Flash Size 选了没有 FS 分区的布局，或分区位置跟芯片对不上）。
    // 至少把 Wifi 凭据从 EEPROM 捞回来，保证上电还能自动重连。
    DBG("[cfg] LittleFS unavailable, try EEPROM\n");
    cfgLoadWifiFallback();
    if (strlen(cfg.staSsid)) { ssid = String(cfg.staSsid); password = String(cfg.staPsk); }
    return;
  }
  File f = MYFS.open(CFG_FILE, "r");
  if (!f) {
    DBG("[cfg] no config file, use defaults\n");
    cfgLoadWifiFallback();                      // 文件不存在也要兜底
    if (strlen(cfg.staSsid)) { ssid = String(cfg.staSsid); password = String(cfg.staPsk); }
    return;
  }
  String s = f.readString();
  f.close();
  if (!s.length()) {
    cfgLoadWifiFallback();                      // 文件是空的也要兜底
    if (strlen(cfg.staSsid)) { ssid = String(cfg.staSsid); password = String(cfg.staPsk); }
    return;
  }

  char tmpFreq[16], tmpAddr[16];
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    char kf[8], ka[8], kr[8], kp[8], ks[8], kw[8];
    snprintf(kf, sizeof(kf), "f%d", i);
    snprintf(ka, sizeof(ka), "a%d", i);
    snprintf(kr, sizeof(kr), "r%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    snprintf(ks, sizeof(ks), "s%d", i);
    snprintf(kw, sizeof(kw), "w%d", i);
    String v;
    v = getKV(s, kf); if (v.length() && normFreq(v, tmpFreq)) copyStr(cfg.pg[i].freq, tmpFreq, sizeof(cfg.pg[i].freq));
    v = getKV(s, ka); if (v.length() && normAddr(v, tmpAddr)) copyStr(cfg.pg[i].addr, tmpAddr, sizeof(cfg.pg[i].addr));
    v = getKV(s, kr); if (v.length()) { char c = v[0]; if (c == 'L' || c == 'H' || c == 'S') cfg.pg[i].rate = c; }
    v = getKV(s, kp); if (v.length()) { char c = v[0]; if (c == 'P' || c == 'N') cfg.pg[i].phase = c; }
    v = getKV(s, ks); cfg.pg[i].sel = (v == "1") ? 1 : 0;
    v = getKV(s, kw); cfg.pg[i].wx  = (v == "1") ? 1 : 0;
  }
  String v;
  v = getKV(s, "type"); if (v.length()) { char c = v[0]; if (c == 'N' || c == 'T') cfg.type = c; }
  v = getKV(s, "beep"); if (v.length()) { char c = v[0]; if (c >= '0' && c <= '3') cfg.beep = c; }
  v = getKV(s, "gap");  if (v.length()) { uint32_t g = v.toInt(); if (g >= 1000 && g <= 60000) cfg.gap = (uint16_t)g; }
  v = getKV(s, "adg");  cfg.adaptGap = (v == "0") ? 0 : 1;
  v = getKV(s, "gfr");  cfg.grpFreq  = (v == "0") ? 0 : 1;
  v = getKV(s, "wsrc"); if (v.length()) cfg.wxSrc = (uint8_t)constrain(v.toInt(), 0, 2);
  v = getKV(s, "wkey"); if (v.length()) copyStr(cfg.wxKey, urlDec(v), sizeof(cfg.wxKey));
  v = getKV(s, "wcity"); if (v.length()) { String d = urlDec(v); if (d.length()) copyStr(cfg.wxCity, d, sizeof(cfg.wxCity)); }
  v = getKV(s, "wname"); copyStr(cfg.wxName, urlDec(v), sizeof(cfg.wxName));
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    char kh[8], km[8], ko[8];
    snprintf(kh, sizeof(kh), "whh%u", i);
    snprintf(km, sizeof(km), "wmm%u", i);
    snprintf(ko, sizeof(ko), "won%u", i);
    v = getKV(s, kh); if (v.length()) cfg.wxHour[i] = (uint8_t)constrain(v.toInt(), 0, 23);
    v = getKV(s, km); if (v.length()) cfg.wxMin[i]  = (uint8_t)constrain(v.toInt(), 0, 59);
    v = getKV(s, ko); if (v.length()) cfg.wxOn[i]   = (v == "1") ? 1 : 0;
  }
  // 兼容早期的单时间配置（whh/wmm）：老固件存的旧配置不会因此丢掉推送时间
  v = getKV(s, "whh"); if (v.length()) { cfg.wxHour[0] = (uint8_t)constrain(v.toInt(), 0, 23); cfg.wxOn[0] = 1; }
  v = getKV(s, "wmm"); if (v.length()) cfg.wxMin[0]  = (uint8_t)constrain(v.toInt(), 0, 59);
  v = getKV(s, "wauto"); cfg.wxAuto = (v == "1") ? 1 : 0;
  v = getKV(s, "ssid");  if (v.length()) copyStr(cfg.staSsid, urlDec(v), sizeof(cfg.staSsid));
  v = getKV(s, "psk");   copyStr(cfg.staPsk, urlDec(v), sizeof(cfg.staPsk));
  // 配置文件里没有凭据（比如改过 Flash Size 重新烧录，FS 分区挪了位置），回退到 EEPROM
  cfgLoadWifiFallback();
  // 配置文件里的路由器账号优先，这样重启后也能自动联网推天气
  if (strlen(cfg.staSsid)) { ssid = String(cfg.staSsid); password = String(cfg.staPsk); }
  DBG("[cfg] loaded\n");
}


/* =====================================================================
 *  发送队列（后台非阻塞）
 * ===================================================================== */
bool txEnqueue(const char* freq, const char* line, uint32_t wait) {
  if (qCount >= TX_QUEUE) return false;
  strncpy(txq[qTail].freq, freq, sizeof(txq[qTail].freq) - 1);
  txq[qTail].freq[sizeof(txq[qTail].freq) - 1] = 0;
  strncpy(txq[qTail].line, line, sizeof(txq[qTail].line) - 1);
  txq[qTail].line[sizeof(txq[qTail].line) - 1] = 0;
  txq[qTail].wait = wait;
  qTail = (qTail + 1) % TX_QUEUE;
  qCount++;
  return true;
}

void txClear() {
  qHead = qTail = qCount = 0;
  txState = 0;
  txTotal = txDone = 0;
  txLastFreq[0] = 0;
}

void txPump() {
  switch (txState) {
    case 0:                                     // 空闲：取一条出来，必要时先切频
      if (qCount == 0) { txTotal = 0; txDone = 0; txLastFreq[0] = 0; return; }
      if (strcmp(txq[qHead].freq, txLastFreq) != 0) {
        Serial.print("#SET+FREQ=");
        Serial.print(txq[qHead].freq);
        strncpy(txLastFreq, txq[qHead].freq, sizeof(txLastFreq) - 1);
        txWaitUntil = millis() + FREQ_SETTLE_MS;
      } else {
        txWaitUntil = millis();                 // 与上一行同频，省掉一次锁相等待
      }
      txState = 1;
      break;

    case 1:                                     // 等待 RF 模块锁定，再下发发射命令
      if ((int32_t)(millis() - txWaitUntil) >= 0) {
        Serial.print(txq[qHead].line);
        txWaitUntil = millis() + txq[qHead].wait;   // 用本条自己算好的等待
        txAcked = false;
        txState = 2;
      }
      break;

    case 2:                                     // 等待本条发完：收到 #TXOK 立即继续，否则等满间隔
      if (txAcked || (int32_t)(millis() - txWaitUntil) >= 0) {
        qHead = (qHead + 1) % TX_QUEUE;
        qCount--;
        txDone++;
        txState = 0;
      }
      break;
  }
}

/* ---------------------------------------------------------------------
 * 发射耗时估算
 *
 * POCSAG 一帧的实际构成：
 *   前导码 576 bit  +  若干「批」(batch)，每批 = 1 个同步码字 + 8 帧 × 2 码字
 *   每个码字固定 32 bit，故每批 = 32 + 8×64 = 544 bit
 *   一条消息占：1 个地址码字 + N 个消息码字 + 1 个结束码字
 *     汉字机：20 bit 信息位装 2 字节（GB18030 双字节）
 *     数字机：20 bit 信息位装 5 个 BCD 字符
 *
 * 原版逻辑是「每条都干等固定间隔」，短消息也要等满 8 秒，10 行群呼 80 多秒。
 * 这里按上面公式算出真实耗时，再加安全系数和 STM32 编码开销。
 * 收到 STM32 的 #TXOK 应答时仍然立即发下一条，那是最准的。
 * ------------------------------------------------------------------- */
#define TX_SAFETY_NUM   14      // 安全系数 1.4（分子）
#define TX_SAFETY_DEN   10
#define TX_MCU_OVERHEAD 600     // STM32 解析串口、编码、装载发射的开销
#define TX_MIN_WAIT     800     // 等待下限，太短会打断上一条

uint32_t txEstimateMs(uint8_t i, const String& msg) {
  uint32_t rate = (cfg.pg[i].rate == 'L') ? 512UL
                : (cfg.pg[i].rate == 'H') ? 1200UL : 2400UL;
  size_t nb = msg.length();                       // 字节数（汉字按 GB18030 已是 2 字节）
  uint32_t msgCw = (cfg.type == 'T') ? ((nb + 1) / 2) : ((nb + 4) / 5);
  uint32_t totalCw = 1 + msgCw + 1;               // 地址 + 消息 + 结束
  uint32_t batches = (totalCw + 15) / 16;         // 每批 16 个码字
  uint32_t bits = 576UL + batches * 544UL + 64UL; // +2 码字补齐帧对齐
  return (uint32_t)((uint64_t)bits * 1000UL / rate);
}

// 本条发完要等多久：自适应开启时按估算值，否则用固定间隔
uint32_t txWaitFor(uint8_t i, const String& msg) {
  if (!cfg.adaptGap) return cfg.gap;
  uint32_t w = txEstimateMs(i, msg) * TX_SAFETY_NUM / TX_SAFETY_DEN + TX_MCU_OVERHEAD;
  if (w < TX_MIN_WAIT) w = TX_MIN_WAIT;
  if (w > 60000UL) w = 60000UL;
  return w;
}

// 组装发给 STM32 的发射指令：#<相位><地址码7><响声><速率><类型><消息>
// 消息原样透传（网页已按 GB18030 提交），这里只补 GB18030 的结尾 0
void buildTxLine(char* out, size_t cap, uint8_t i, const String& msg) {
  snprintf(out, cap, "#%c%s%c%c%c", cfg.pg[i].phase, cfg.pg[i].addr, cfg.beep, cfg.pg[i].rate, cfg.type);
  size_t used = strlen(out);
  size_t room = cap - used - 1;
  size_t n = msg.length();
  if (n > room) n = room;
  memcpy(out + used, msg.c_str(), n);
  out[used + n] = 0;
}


/* =====================================================================
 *  网页发送：PROGMEM 分片 chunked，几乎不占堆
 * ===================================================================== */
// 页面发送：预压缩数据直接透传，浏览器负责解压。
// chunk 取 1460（以太网 MSS），太小会让每个分片各触发一次 TCP 发送，明显拖慢首屏。
#define HTTP_CHUNK 1460

void sendPage(const PageGz& p) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  if (p.gz) server.sendHeader("Content-Encoding", "gzip");
  server.send(200, "text/html; charset=GB18030", "");
  size_t off = 0;
  while (off < p.len) {
    size_t n = p.len - off;
    if (n > HTTP_CHUNK) n = HTTP_CHUNK;
    server.sendContent_P(reinterpret_cast<PGM_P>(p.data) + off, n);
    off += n;
    yield();
  }
}

// POST 结果回填到隐藏 iframe，并回调父页面的 txDone()
void sendIframeReply(int n) {
  char buf[160];
  snprintf(buf, sizeof(buf),
    "<!DOCTYPE html><html><head><meta charset=\"GB18030\"></head><body><script>"
    "if(parent&&parent.txDone)parent.txDone(%d);"
    "</script>OK %d</body></html>", n, n);
  server.send(200, "text/html", buf);
}


/* =====================================================================
 *  Web 处理函数
 * ===================================================================== */
void index_page_server() {
  if (server.method() != HTTP_POST) { sendPage(PAGE_INDEX); return; }

  char freq[10], addr[8];
  if (!normFreq(argVal("r_freq"), freq) || !normAddr(argVal("r_addr"), addr)) {
    sendIframeReply(-1);
    return;
  }
  String r = argVal("r_rate",  "H");
  String p = argVal("r_phase", "P");
  String t = argVal("r_type",  "T");
  String b = argVal("r_beep",  "0");
  String m = argVal("r_message", "");
  if (m.length() > MSG_MAX) m = m.substring(0, MSG_MAX);

  // 顺手把这一页的参数回写进第 1 行，方便群呼页直接复用
  copyStr(cfg.pg[0].freq, String(freq), sizeof(cfg.pg[0].freq));
  copyStr(cfg.pg[0].addr, String(addr), sizeof(cfg.pg[0].addr));
  cfg.pg[0].rate  = r[0];
  cfg.pg[0].phase = p[0];
  cfg.type = (t[0] == 'N' || t[0] == 'T') ? t[0] : cfg.type;
  cfg.beep = (b[0] >= '0' && b[0] <= '3') ? b[0] : cfg.beep;
  cfgSave();

  char line[TX_LINE_MAX];
  snprintf(line, sizeof(line), "#%c%s%c%c%c", p[0], addr, b[0], r[0], cfg.type);
  size_t used = strlen(line);
  size_t room = sizeof(line) - used - 1;
  if (m.length() > room) m = m.substring(0, room);
  memcpy(line + used, m.c_str(), m.length());
  line[used + m.length()] = 0;

  int ok = txEnqueue(freq, line, txWaitFor(0, m)) ? 1 : -1;
  if (ok == 1) txTotal += 1;
  sendIframeReply(ok);
}

void group_page_server() {
  if (server.method() != HTTP_POST) { sendPage(PAGE_GROUP); return; }

  // 1) 收集勾选状态：sel=参与群呼，wx=接收天气推送，两者互相独立
  bool sel[PAGER_NUM], wxsel[PAGER_NUM];
  for (uint8_t i = 0; i < PAGER_NUM; i++) { sel[i] = false; wxsel[i] = false; }
  for (int i = 0; i < server.args(); i++) {
    const String& an = server.argName(i);
    if (an != "sel" && an != "wx") continue;
    int v = server.arg(i).toInt();
    if (v < 0 || v >= PAGER_NUM) continue;
    if (an == "sel") sel[v]   = true;
    else             wxsel[v] = true;
  }

  // 2) 逐行读取并规整
  char tmpF[16], tmpA[16];
  uint8_t bad = 0;
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    char kf[8], ka[8], kr[8], kp[8];
    snprintf(kf, sizeof(kf), "f%d", i);
    snprintf(ka, sizeof(ka), "a%d", i);
    snprintf(kr, sizeof(kr), "r%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    String vf = argVal(kf), va = argVal(ka);
    if (vf.length() && normFreq(vf, tmpF)) copyStr(cfg.pg[i].freq, String(tmpF), sizeof(cfg.pg[i].freq));
    if (va.length() && normAddr(va, tmpA)) copyStr(cfg.pg[i].addr, String(tmpA), sizeof(cfg.pg[i].addr));
    String vr = argVal(kr); if (vr.length()) { char c = vr[0]; if (c == 'L' || c == 'H' || c == 'S') cfg.pg[i].rate = c; }
    String vp = argVal(kp); if (vp.length()) { char c = vp[0]; if (c == 'P' || c == 'N') cfg.pg[i].phase = c; }
    cfg.pg[i].sel = sel[i]   ? 1 : 0;
    cfg.pg[i].wx  = wxsel[i] ? 1 : 0;
    if (!sel[i]) continue;
    if (strlen(cfg.pg[i].freq) != 8 || strlen(cfg.pg[i].addr) != 7) bad++;
  }

  // 3) 全局参数
  String t = argVal("r_type"); if (t.length()) { char c = t[0]; if (c == 'N' || c == 'T') cfg.type = c; }
  String b = argVal("r_beep"); if (b.length()) { char c = b[0]; if (c >= '0' && c <= '3') cfg.beep = c; }
  String g = argVal("r_gap");  if (g.length()) { uint32_t v = g.toInt(); if (v >= 1000 && v <= 60000) cfg.gap = (uint16_t)v; }
  cfg.adaptGap = argHas("adg") ? 1 : 0;      // 自适应间隔
  cfg.grpFreq  = argHas("gfr") ? 1 : 0;      // 同频归组
  String m = argVal("r_message", "");
  if (m.length() > MSG_MAX) m = m.substring(0, MSG_MAX);

  cfgSave();

  // 4) 入队。
  //    默认按频率归组（稳定排序，同频内仍按序号升序）：切频一次要等 400ms 锁相，
  //    10 行若是 3 个频点，就从 10 次切频降到 3 次，省 2.8 秒。
  //    想要严格按序号发，把「同频归组」关掉即可。
  uint8_t ord[PAGER_NUM], on = 0;
  for (uint8_t i = 0; i < PAGER_NUM; i++)
    if (cfg.pg[i].sel) ord[on++] = i;

  if (cfg.grpFreq && on > 1) {                 // 插入排序，元素少足够快且稳定
    for (uint8_t a = 1; a < on; a++) {
      uint8_t key = ord[a];
      int16_t b = (int16_t)a - 1;
      while (b >= 0 && strcmp(cfg.pg[ord[b]].freq, cfg.pg[key].freq) > 0) {
        ord[b + 1] = ord[b];
        b--;
      }
      ord[b + 1] = key;
    }
  }

  int n = 0;
  char line[TX_LINE_MAX];
  for (uint8_t k = 0; k < on; k++) {
    uint8_t i = ord[k];
    buildTxLine(line, sizeof(line), i, m);
    if (txEnqueue(cfg.pg[i].freq, line, txWaitFor(i, m))) n++;
  }
  txTotal += n;
  sendIframeReply(n);
}

void wifi_page_server() {
  if (server.method() != HTTP_POST) { sendPage(PAGE_WIFI); return; }
  String r_ssid = argVal("r_ssid");
  String r_psk  = argVal("r_psk");
  if (!r_ssid.length()) { sendIframeReply(-1); return; }
  ssid = r_ssid;
  password = r_psk;
  // 存一份到 ESP8266 自己的文件系统，重启后能自动重连（天气推送依赖它）
  copyStr(cfg.staSsid, r_ssid, sizeof(cfg.staSsid));
  copyStr(cfg.staPsk,  r_psk,  sizeof(cfg.staPsk));
  bool ok = cfgSave();
  if (!ok) {
    // 写不进去必须当场说清楚。原来这里静默失败，
    // 用户看到「已提交」以为存好了，重启后却是出厂设置。
    Serial.println("#WIFI+SAVEFAIL");
    sendIframeReply(-2);
    return;
  }
  // 同时交给 STM32 存一份，掉电不丢
  Serial.print("#WIFI+SAVESSID=" + r_ssid);
  delay(50);
  Serial.print("#WIFI+SAVESSID=" + r_ssid);
  delay(300);
  Serial.print("#WIFI+SAVEPSK=" + r_psk);
  delay(300);
  // 后台连，不阻塞 Web：结果由前端轮询 /wifistat 拿
  staBegin(ssid, password);
  sendIframeReply(1);
}

// GET /wifistat —— 连接状态机当前结果，供 Wifi 页轮询
void wifistat_server() {
  bool on = (WiFi.status() == WL_CONNECTED);
  String j = "{\"st\":" + String(staState) +
             ",\"on\":" + (on ? "1" : "0") +
             ",\"ssid\":\"" + jsonEsc(utf8Safe(WiFi.SSID())) + "\"" +
             ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
             ",\"saved\":" + String(strlen(cfg.staSsid) ? 1 : 0) +
             ",\"fsok\":" + String(fsOk ? 1 : 0) +
             ",\"eefrom\":" + String(eeUsed ? 1 : 0) +
             ",\"why\":\"" + jsonEsc(staWhy) + "\"";
  if (on) j += ",\"rssi\":" + String(WiFi.RSSI());
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

/* =====================================================================
 *  Wifi 扫描
 *
 *  用异步扫描（scanNetworks(true)），不用同步的：
 *  同步扫描会把整个 CPU 阻塞 1~2 秒，期间 Web 服务不响应，
 *  手机上的页面会「卡住」甚至把请求判超时。异步方式下 loop 照常跑。
 *  ==================================================================== */
static bool     scanRun  = false;
static uint32_t scanAt   = 0;
static int      scanN    = 0;       // 最近一次结果：>=0 数量，WIFI_SCAN_FAILED 失败
static bool     scanData = false;   // 结果是否还在内存里（未 scanDelete）

void scanStart() {
  if (scanRun) return;
  if (scanData) { WiFi.scanDelete(); scanData = false; }
  WiFi.scanNetworks(true, true);      // async=true, show_hidden=true
  scanRun = true;
  scanAt  = millis();
}

void scanTick() {
  if (!scanRun) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {                       // -1：还在扫
    if (millis() - scanAt > SCAN_TIMEOUT) {           // 卡住就放弃，别一直占着
      scanRun = false;
      scanN   = WIFI_SCAN_FAILED;
    }
    return;
  }
  scanRun  = false;
  scanN    = n;                                       // -2 失败，或 >=0 数量
  scanData = (n > 0);
}

static const char* encName(uint8_t e) {
  switch (e) {
    case 0:  return "开放";
    case 1:  return "WEP";
    case 2:  return "WPA";
    case 3:  return "WPA2";
    case 4:  return "WPA/WPA2";
    case 5:  return "WPA2 企业";
    case 6:  return "WPA3";
    case 7:  return "WPA2/WPA3";
    case 8:  return "WAPI";
    default: return "加密";
  }
}

// GET /scan?start=1 发起扫描；GET /scan 取结果（扫描中返回 run=1）
void scan_server() {
  if (argHas("start")) scanStart();

  bool connected = (WiFi.status() == WL_CONNECTED);
  String curSsid = connected ? WiFi.SSID() : String();

  // 分片输出，避免为了拼一个大 JSON 去申请大块堆内存
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  server.sendContent(String("{\"run\":") + (scanRun ? "1" : "0") +
                     ",\"n\":" + String(scanRun ? -1 : scanN) +
                     ",\"cur\":" + (connected ? "1" : "0") +
                     ",\"curssid\":\"" + jsonEsc(utf8Safe(curSsid)) + "\"" +
                     ",\"list\":[");

  if (!scanRun && scanData && scanN > 0) {
    int total = scanN;
    if (total > SCAN_KEEP) total = SCAN_KEEP;

    // 按信号强度降序排（插入排序，元素少且稳定，同强度时保持原有顺序）
    uint8_t ord[SCAN_KEEP];
    for (int i = 0; i < total; i++) ord[i] = (uint8_t)i;
    for (int a = 1; a < total; a++) {
      uint8_t key = ord[a];
      int16_t b = (int16_t)a - 1;
      while (b >= 0 && WiFi.RSSI(ord[b]) < WiFi.RSSI(key)) { ord[b + 1] = ord[b]; b--; }
      ord[b + 1] = key;
    }

    int show = total < SCAN_SHOW ? total : SCAN_SHOW;
    for (int k = 0; k < show; k++) {
      int i = ord[k];
      String s = jsonEsc(utf8Safe(WiFi.SSID(i)));
      bool isCur = connected && (WiFi.SSID(i) == curSsid);
      server.sendContent(
        String(k ? ",{\"s\":\"" : "{\"s\":\"") + s +
        "\",\"r\":" + String(WiFi.RSSI(i)) +
        ",\"c\":" + String(WiFi.channel(i)) +
        ",\"e\":" + String((int)WiFi.encryptionType(i)) +
        ",\"a\":\"" + encName(WiFi.encryptionType(i)) + "\"" +
        ",\"h\":" + (WiFi.isHidden(i) ? "1" : "0") +
        ",\"cur\":" + (isCur ? "1" : "0") +
        "}");
      yield();
    }
  }
  server.sendContent(String("]}"));
}

void wxcfg_server() {
  String v;
  v = argVal("wsrc"); if (v.length()) cfg.wxSrc = (uint8_t)constrain(v.toInt(), 0, 2);
  v = argVal("wkey");  copyStr(cfg.wxKey,  v, sizeof(cfg.wxKey));
  v = argVal("wcity"); if (v.length()) copyStr(cfg.wxCity, v, sizeof(cfg.wxCity));
  v = argVal("wname"); copyStr(cfg.wxName, v, sizeof(cfg.wxName));   // 可留空 = 用接口返回的城市名
  // 三个时间点：wt0/wt1/wt2 = "HH:MM"，won0/won1/won2 = 是否启用
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    char kt[8], ko[8];
    snprintf(kt, sizeof(kt), "wt%u", i);
    snprintf(ko, sizeof(ko), "won%u", i);
    v = argVal(kt);
    if (v.length() >= 3 && v.indexOf(':') > 0) {
      int c = v.indexOf(':');
      int h = v.substring(0, c).toInt();
      int m = v.substring(c + 1).toInt();
      if (h >= 0 && h <= 23 && m >= 0 && m <= 59) { cfg.wxHour[i] = (uint8_t)h; cfg.wxMin[i] = (uint8_t)m; }
    }
    cfg.wxOn[i] = argHas(ko) ? 1 : 0;
  }
  cfg.wxAuto = argHas("wauto") ? 1 : 0;
  wxDoneMask = 0;                                       // 改配置后允许立刻再推一次
  cfgSave();
  sendIframeReply(1);
}

void wxnow_server() {
  String err;
  bool ok = fetchWx(&err);
  String j = "{\"ok\":";
  j += ok ? "1" : "0";
  j += ",\"text\":\"" + jsonEsc(wx.text) + "\"";
  if (!ok) j += ",\"err\":\"" + jsonEsc(err) + "\"";
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

void wxpush_server() {
  String err;
  if (!fetchWx(&err)) {
    server.send(200, "application/json; charset=utf-8", "{\"ok\":0,\"err\":\"" + jsonEsc(err) + "\"}");
    return;
  }
  int n = pushWx();
  if (!n) {
    server.send(200, "application/json; charset=utf-8",
                "{\"ok\":0,\"err\":\"没有勾选任何一行的天气推送，请先在列表「天气」列勾选\",\"text\":\""
                + jsonEsc(wx.text) + "\"}");
    return;
  }
  server.send(200, "application/json; charset=utf-8",
              "{\"ok\":1,\"n\":" + String(n) + ",\"text\":\"" + jsonEsc(wx.text) + "\"}");
}

void cancel_server() {
  txClear();
  server.send(200, "application/json; charset=utf-8", "{\"ok\":1}");
}

void status_server() {
  // rem：当前这一条还要等多少毫秒，前端拿它画进度条
  uint32_t rem = 0;
  if (qCount && txState == 2) {
    uint32_t now = millis();
    rem = ((int32_t)(txWaitUntil - now) > 0) ? (txWaitUntil - now) : 0;
  }
  String j = "{\"q\":" + String(qCount) +
             ",\"done\":" + String(txDone) +
             ",\"total\":" + String(txTotal) +
             ",\"rem\":" + String(rem) +
             ",\"heap\":" + String(ESP.getFreeHeap()) + "}";
  server.send(200, "application/json; charset=utf-8", j);
}

// OTA 状态（升级页 /cfg 与 /update 都要读，所以放在这里）
static bool     otaRejected = false;  // 开写前就被拒（没动过 Flash）
static bool     otaStarted  = false;  // Update.begin() 是否已成功调用
static String   otaMsg;               // 给网页的原因说明
static bool     otaGotData  = false;  // 是否真的收到过数据
static uint32_t otaRebootAt = 0;      // 计划重启的时间戳（0=不重启）
static uint32_t otaMaxSpace = 0;      // 本次允许写入的最大字节数

// Updater 的错误码（esp8266 core 的 Updater.h），转成中文给网页显示
static const char* otaErrText(int e) {
  switch (e) {
    case 0:  return "没有错误";
    case 1:  return "写入 Flash 失败";
    case 2:  return "擦除 Flash 失败";
    case 3:  return "读取 Flash 失败";
    case 4:  return "空间不足";
    case 5:  return "固件大小超过可用空间";
    case 6:  return "数据流中断";
    case 7:  return "校验和不匹配（文件传坏了）";
    case 8:  return "Flash 配置不符（编译时的 Flash Size 不对）";
    case 9:  return "新的 Flash 配置无法应用";
    case 10: return "不是合法的固件（缺少 0xE9 魔术字节）";
    case 11: return "无法进入升级模式";
    case 12: return "签名校验失败";
    case 13: return "没有收到数据";
    default: return "未知错误";
  }
}


/* 推迟到「真的收到第一个数据包」时才 Update.begin()，而不是在 UPLOAD_FILE_START 就开写。
 *
 * 为什么要这样：一旦 begin() 成功，Updater 就进入了占用状态；如果之后没数据了
 * （用户选了个空文件、或中途取消），就得把它复位。core 3.x 有 Update.abort()，
 * 但 2.7.x 没有 —— 直接调会在 2.7.4 上报
 *   'class UpdaterClass' has no member named 'abort'
 *
 * 与其写 #if 分支去兼容两个版本，不如从根上不让它进入这个状态：
 * 空文件时压根不会触发 WRITE，begin() 从未调用，自然也不需要清理。
 * 顺带的好处是 LittleFS 也不会被无谓地卸载。
 */
bool otaBeginWrite() {
  if (otaStarted) return true;

  MYFS.end();                          // OTA 与文件系统不能同时操作 Flash
  if (!Update.begin(otaMaxSpace)) {
    otaRejected = true;
    otaMsg = String("无法开始升级：") + otaErrText(Update.getError());
    MYFS.begin();
    return false;
  }
  otaStarted = true;
  return true;
}

void cfg_server() {
  String j = "{\"pg\":[";
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    if (i) j += ",";
    j += "{\"f\":\"" + String(cfg.pg[i].freq) +
         "\",\"a\":\"" + String(cfg.pg[i].addr) +
         "\",\"r\":\"" + String((char)cfg.pg[i].rate) +
         "\",\"p\":\"" + String((char)cfg.pg[i].phase) +
         "\",\"s\":" + String(cfg.pg[i].sel) +
         ",\"w\":" + String(cfg.pg[i].wx) + "}";
  }
  j += "],\"type\":\"" + String((char)cfg.type) + "\"";
  j += ",\"beep\":\"" + String((char)cfg.beep) + "\"";
  j += ",\"gap\":" + String(cfg.gap);
  j += ",\"adg\":" + String(cfg.adaptGap);
  j += ",\"gfr\":" + String(cfg.grpFreq);

  char t0[8];
  snprintf(t0, sizeof(t0), "%02u:%02u", cfg.wxHour[0], cfg.wxMin[0]);
  j += ",\"wx\":{\"src\":" + String(cfg.wxSrc) +
       ",\"key\":\"" + jsonEsc(String(cfg.wxKey)) +
       "\",\"city\":\"" + jsonEsc(String(cfg.wxCity)) +
       "\",\"name\":\"" + jsonEsc(String(cfg.wxName)) +
       "\",\"gotcity\":\"" + jsonEsc(wx.city) +
       "\",\"t\":\"" + String(t0) +                       // 兼容：第一组时间
       "\",\"auto\":" + String(cfg.wxAuto) +
       ",\"ts\":[";
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    char t[8];
    snprintf(t, sizeof(t), "%02u:%02u", cfg.wxHour[i], cfg.wxMin[i]);
    if (i) j += ",";
    j += "\"" + String(t) + "\"";
  }
  j += "],\"on\":[";
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    if (i) j += ",";
    j += String(cfg.wxOn[i] ? 1 : 0);
  }
  j += "],\"pv\":\"" + jsonEsc(wx.text) + "\"}";

  j += ",\"ap\":\"" + String(AP_SSID) + "\"";
  j += ",\"apip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"sta\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0);
  j += ",\"saved\":" + String(strlen(cfg.staSsid) ? 1 : 0);
  j += ",\"savedssid\":\"" + jsonEsc(utf8Safe(String(cfg.staSsid))) + "\"";
  j += ",\"fsok\":" + String(fsOk ? 1 : 0);
  j += ",\"eefrom\":" + String(eeUsed ? 1 : 0);
  j += ",\"ssid\":\"" + jsonEsc(WiFi.SSID()) + "\"";
  j += ",\"staip\":\"" + WiFi.localIP().toString() + "\"";
  if (WiFi.status() == WL_CONNECTED) j += ",\"rssi\":" + String(WiFi.RSSI());

  char now[24] = "-";
  time_t ts = time(nullptr);
  if (ts > 1700000000L) {
    struct tm* tmv = localtime(&ts);
    snprintf(now, sizeof(now), "%02d-%02d %02d:%02d",
             tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min);
  }
  j += ",\"time\":\"" + String(now) + "\"";

  uint32_t real = ESP.getFlashChipRealSize();
  uint32_t sz = ESP.getSketchSize();
  uint32_t freeSp = ESP.getFreeSketchSpace();
  FSInfo fsi;
  bool hasFs = MYFS.info(fsi);
  j += ",\"fw\":\"" FW_VER "\"";
  j += ",\"build\":\"" + String(__DATE__) + " " + String(__TIME__) + "\"";
  j += ",\"sketch\":\"" + String(sz / 1024) + " KB\"";
  j += ",\"free\":\"" + String(freeSp / 1024) + " KB\"";
  j += ",\"flash\":\"" + String(real / 1024 / 1024) + " MB\"";
  // 数值版，供升级页在上传前精确判断固件能否放下（而不是只看字符串）
  j += ",\"freesp\":" + String((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
  j += ",\"flashb\":" + String(real);
  j += ",\"otapend\":" + String(otaRebootAt ? 1 : 0);
  j += ",\"fs\":\"" + (hasFs ? String(fsi.usedBytes / 1024) + " KB / " + String(fsi.totalBytes / 1024) + " KB" : "-") + "\"";
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

/* =====================================================================
 *  OTA 升级
 *
 *  原版这里有两个坑，都会表现为网页上「升级失败：网络错误」：
 *
 *  ① 不限大小就往 Flash 里写。Update.begin() 传的是可用空间上限，
 *     但从不校验上传的文件是否超限。一旦传进来的东西比 OTA 分区大
 *     （比如把打包脚本合成的 4MB 整片镜像当固件传），写越界会直接崩，
 *     设备复位 → TCP 连接断开 → 浏览器只看到「网络错误」，看不到真正原因。
 *
 *  ② 在响应函数里 delay(100) 就 ESP.restore()。100ms 常常不够 lwIP 把
 *     响应真正发出去，设备先断电重启了，浏览器同样只报「网络错误」——
 *     其实固件已经刷好了。这个最坑：明明成功了却显示失败。
 *
 *  对应改法：
 *     ① 开写之前先用 Content-Length 卡一次大小，超限直接拒，并说明原因；
 *     ② 响应先发，重启挂到 loop() 里延后执行，中间继续跑 handleClient()
 *        让 TCP 把数据发完。
 *  ==================================================================== */

// 上传结束后的响应。单独拎出来，便于测试直接调用。
void otaReply() {
  server.sendHeader("Connection", "close");
  bool ok = !Update.hasError() && !otaRejected && otaMsg.length() == 0;
  // 响应正文带上原因（成功时是 OK），前端直接显示
  server.send(200, "text/plain; charset=utf-8",
              ok ? String("OK")
                 : (otaMsg.length() ? otaMsg : String(otaErrText(Update.getError()))));
  // 关键：不在这里重启。留给 loop() 延后执行，好让 lwIP 有时间把响应发出去。
  if (ok) otaRebootAt = millis() + OTA_FLUSH_MS;
}

void UpdateProcess() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaRejected = false;
    otaGotData  = false;
    otaStarted  = false;
    otaMsg      = "";
    otaRebootAt = 0;

    WiFiUDP::stopAll();

    otaMaxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

    // 关键①：写之前先卡大小。Content-Length 是整包，含 multipart 头部开销，
    // 留 2KB 余量估算，宁可误拦也不要写越界。
    String cl = server.header("Content-Length");
    if (cl.length()) {
      long total = atol(cl.c_str());
      if (total > 0 && (uint32_t)total > otaMaxSpace) {
        otaRejected = true;
        otaMsg = "固件太大：收到 " + String((uint32_t)total / 1024) + " KB，"
                 "可用空间只有 " + String(otaMaxSpace / 1024) + " KB。"
                 "OTA 只能传 Arduino「导出已编译的二进制文件」得到的程序 bin，"
                 "不能传打包脚本合成的整片 Flash 镜像（那个必须用串口烧录）";
        DBG("[ota] rejected: %ld > %u\n", total, otaMaxSpace);
      }
    }
    // 注意：这里【不】调用 Update.begin()。
    // 见下面的 otaBeginWrite() —— 推迟到真的收到数据时才开写。
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaRejected) return;                       // 已拒，把剩下的数据吃掉但不写
    if (!otaBeginWrite()) return;                  // 第一次收到数据时才真正开始
    otaGotData = true;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (otaRejected) return;        // 被拒的都是开写前就拒的，没动过 Flash，无需收尾
    if (!otaStarted) {              // 空文件 / 还没开写就结束：Updater 没起过，不必清理
      otaMsg = "没有收到固件数据";
      return;
    }
    if (Update.end(true)) {
      DBG("[ota] ok %u bytes\n", upload.totalSize);
      otaMsg = "";
    } else {
      otaMsg = String("写入失败：") + otaErrText(Update.getError());
      MYFS.begin();                 // 失败后把文件系统挂回去，配置读写仍可用
    }
  }
  yield();
}


/* =====================================================================
 *  Wifi
 * ===================================================================== */
/* =====================================================================
 *  连接路由器
 *
 *  原版 staConnect() 里有一个 while 循环死等 10 秒。那 10 秒里 CPU 被占满，
 *  Web 服务完全不响应 —— 所以原版只能让用户在提交后去盯 OLED 屏看结果，
 *  网页上什么也显示不出来。
 *
 *  这里改成状态机：staBegin() 只负责发起，staTick() 在 loop() 里查结果。
 *  连接期间网页照常刷新，前端轮询 /wifistat 就能看到「正在连接 → 成功/失败」。
 *  ==================================================================== */


// 把 SSID/密码里的危险字符清掉：串口命令以 # 开头、\r\n 结尾，
// 名称里若带这些字符会破坏命令帧；GB18030 双字节里确实可能撞上 0x0D、0x23。
String serialSafe(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == '#')  o += '?';   // 控制字符与命令起始符
    else if (c < 0x80)         o += (char)c;
    else                       o += '?';   // 非 ASCII：串口侧按 GB18030 显示，
    // 这里没有完整的 UTF-8→GB18030 表，降级成 ? 至少不会让命令帧错乱
  }
  return o;
}

void staBegin(const String& s, const String& p) {
  // 凭据有没有变。这一步很关键：WiFi.begin() 会把凭据写进 SDK 的 Flash 区，
  // 反复调用既损耗 Flash，还可能触发库的竞态把 WiFi 子系统搞坏。
  // 所以只有「凭据变了 / SDK 里还没有」时才 begin()，其余用 reconnect()。
  bool needBegin = (!staSdkHasCred) || (s != ssid) || (p != password) || (WiFi.SSID() != s);

  ssid = s;
  password = p;

  // 扫描结果占着堆内存，连接前先清掉，避免和 TLS/HTTP 抢内存
  if (scanData) { WiFi.scanDelete(); scanData = false; }

  // 只在模式不对时才切：反复调用 WiFi.mode() 会重置 softAP，把连着热点的手机踢掉
  if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);

  if (needBegin) {
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();   // 换网络才需要先断开
    WiFi.begin(ssid.c_str(), password.c_str());
    staSdkHasCred = true;
  } else {
    WiFi.reconnect();                                        // 配置没变，让 SDK 重连
  }

  staState     = STA_TRYING;
  staTryAt     = millis();
  staWhy       = "";
  staManualOff = false;
}

// 掉线自动重连。原版完全没有这个：setup 里只试一次，
// 路由器晚一点起来、模块先上电、信号一时抖动，都会永久掉线，
// 天气推送也就此失效，只能手动再连一次。
//
// 注意间隔要退避，不能固定短间隔：ESP8266 的 STA 和 softAP 共用同一个射频，
// STA 每次尝试连接都会跳信道，softAP 跟着跳，连在热点上的手机就会被踢下来。
// 所以失败次数越多，等得越久。
void staAutoTick() {
  if (staManualOff) return;                    // 用户主动断开，尊重用户
  if (!strlen(cfg.staSsid)) return;            // 从没配过
  if (staState == STA_TRYING) return;          // 正在连
  if (WiFi.status() == WL_CONNECTED) { staState = STA_OK; staFailCnt = 0; staBackoff = STA_RETRY_MS; return; }
  if (millis() - staTryAt < staBackoff) return;
  if (staFailCnt < 250) staFailCnt++;
  staBackoff = STA_RETRY_MS;
  for (uint8_t i = 0; i < staFailCnt && staBackoff < STA_RETRY_MAX; i++) staBackoff *= 2;
  if (staBackoff > STA_RETRY_MAX) staBackoff = STA_RETRY_MAX;
  staAuto = true;                              // 标记：这次是自动重连，别刷屏
  DBG("[wifi] auto reconnect (fail %d, next %u ms)\n", staFailCnt, staBackoff);
  staBegin(String(cfg.staSsid), String(cfg.staPsk));
}

void staTick() {
  if (staState != STA_TRYING) return;
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    staState = STA_OK;
    staFailCnt = 0;
    staBackoff = STA_RETRY_MS;
    staAuto = false;
    Serial.println("#WIFI+OK");
    Serial.println("#WIFI+SHOWIP=" + WiFi.localIP().toString());
    int r = WiFi.RSSI();
    int lvl = 0;
    if (r > -50) lvl = 4; else if (r > -70) lvl = 3;
    else if (r > -80) lvl = 2; else if (r > -100) lvl = 1;
    Serial.println("#WIFI+SHOWRSSI=" + String(lvl));
    configTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
    ntpDone = false;
    return;
  }

  if (st == WL_NO_SSID_AVAIL) {
    staWhy = "搜不到这个网络，可能不在信号范围内，或名称输错了";
  } else if (st == WL_CONNECT_FAILED) {
    staWhy = "密码错误，或被路由器拒绝";
  } else if (millis() - staTryAt > STA_TIMEOUT) {
    staWhy = "连接超时，请检查名称密码，或靠近路由器再试";
  } else {
    return;                                 // 还在协商中，继续等
  }

  staState = STA_FAIL;
  // 自动重连的失败不上报：否则路由器不在时 STM32 屏幕会每半分钟弹一次失败，
  // 用户根本分不清是真连不上还是设备坏了。真连上时会正常报 #WIFI+OK。
  if (!staAuto) Serial.println("#WIFI+FAILE");
  staAuto = false;
  // 这里刻意【不】调用 WiFi.disconnect()：它会把 SDK 里记住的 SSID 清掉，
  // 下次重连就不得不再来一次 begin()（写 Flash）。放着不管，
  // SDK 自己也在后台重试，等它连上了 staAutoTick() 会认出来并上报。
}

// 供开机自动重连用（阻塞版，setup 里没有网页要响应）
void staConnect() {
  if (!ssid.length()) return;
  staBegin(ssid, password);
  uint32_t t0 = millis();
  while (staState == STA_TRYING && millis() - t0 < STA_TIMEOUT) {
    delay(100);
    staTick();
    yield();
  }
}

void staDisconnect() {
  WiFi.disconnect();
  staState       = STA_IDLE;
  staManualOff   = true;      // 主动断开就别再自己连回来
  staSdkHasCred  = false;     // SDK 里的凭据已清空，下次要重新 begin()
  staFailCnt     = 0;
  staBackoff     = STA_RETRY_MS;
  staWhy         = "";
  Serial.println("#WIFI+FAILE");
}

void apStart() {
  // 凭据由我们自己存在 LittleFS 里，不让 SDK 每次 begin() 都往 Flash 写一遍 ——
  // 那片区域没有磨损均衡，反复写既费 Flash，还可能撞上库的竞态把 WiFi 搞坏。
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGW, apMask);
  WiFi.softAP(AP_SSID, AP_PSW);
  DBG("[ap] %s %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void serverRoutes() {
  server.on("/",           index_page_server);
  server.on("/index.html", index_page_server);
  server.on("/group_call", group_page_server);
  server.on("/wifi",       wifi_page_server);
  server.on("/scan",       scan_server);
  server.on("/wifistat",   wifistat_server);
  server.on("/wxcfg",      HTTP_POST, wxcfg_server);
  server.on("/wxnow",      wxnow_server);
  server.on("/wxpush",     HTTP_POST, wxpush_server);
  server.on("/cancel",     HTTP_POST, cancel_server);
  server.on("/cfg",        cfg_server);
  server.on("/status",     status_server);
  server.on("/webupdate", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    sendPage(PAGE_UPDATE);
  });
  server.on("/update", HTTP_POST, otaReply, UpdateProcess);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}


/* =====================================================================
 *  串口桥接（与 STM32 通讯）
 * ===================================================================== */
void receiveString() {
  while (Serial.available() > 0) {
    comdata += char(Serial.read());
    delay(2);
  }
  if (!comdata.length()) return;

  // 发射完成应答（需给 STM32 的 start_tx() 加一行 USART2_SendStr("#TXOK\r\n");）
  int k;
  while ((k = comdata.indexOf("#TXOK")) >= 0) {
    txAcked = true;
    comdata.remove(0, k + 5);
  }
  comdata.trim();
  if (!comdata.length()) { comdata = ""; return; }

  if      (comdata.startsWith("#WIFI+STA=ON"))    staConnect();
  else if (comdata.startsWith("#WIFI+STA=OFF"))   staDisconnect();
  else if (comdata.startsWith("#WIFI+SET+SSID=")) {
    ssid = comdata.substring(15);
    ssid.trim();
    copyStr(cfg.staSsid, ssid, sizeof(cfg.staSsid));
  }
  else if (comdata.startsWith("#WIFI+SET+PSK="))  password = comdata.substring(14);
  else if (comdata.startsWith("#WIFI+GETIP"))     Serial.println("#WIFI+SHOWIP=" + WiFi.localIP().toString());
  else if (comdata.startsWith("#WIFI+GETRSSI"))   Serial.println("#WIFI+SHOWRSSI=" + String(WiFi.RSSI()));
  comdata = "";
}


/* =====================================================================
 *  天气
 * ===================================================================== */
static const char* wmoText(int c) {
  switch (c) {
    case 0:  return "晴";
    case 1:  return "晴间多云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: case 56: case 57: return "毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: return "小雨";
    case 81: return "中雨";
    case 82: return "大雨";
    case 85: return "小雪";
    case 86: return "大雪";
    case 95: return "雷阵雨";
    case 96: case 99: return "雷阵雨伴有冰雹";
    default: return "";
  }
}

static int beaufort(float kmh) {
  if (kmh < 1)   return 0;
  if (kmh < 6)   return 1;
  if (kmh < 12)  return 2;
  if (kmh < 20)  return 3;
  if (kmh < 29)  return 4;
  if (kmh < 39)  return 5;
  if (kmh < 50)  return 6;
  if (kmh < 62)  return 7;
  if (kmh < 75)  return 8;
  if (kmh < 89)  return 9;
  if (kmh < 103) return 10;
  if (kmh < 118) return 11;
  return 12;
}

static const char* degToDir(float d) {
  static const char* d8[] = { "北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风" };
  if (d < 0) return "";
  int i = (int)((d + 22.5f) / 45.0f) & 7;
  return d8[i];
}

bool fetchWx(String* err) {
  if (WiFi.status() != WL_CONNECTED) { if (err) *err = "Wifi 未连接，请先在“配置 Wifi”里连上可上网的路由器"; return false; }

  String url;
  if (cfg.wxSrc == 0) {                                    // Open-Meteo（免 Key）
    String city = String(cfg.wxCity);
    int c = city.indexOf(',');
    if (c < 0) { if (err) *err = "免 Key 源需要填“纬度,经度”，例如 22.8170,108.3665"; return false; }
    url = "https://api.open-meteo.com/v1/forecast?latitude=" + urlEnc(city.substring(0, c)) +
          "&longitude=" + urlEnc(city.substring(c + 1)) +
          "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m" +
          "&timezone=Asia%2FShanghai";
  } else if (cfg.wxSrc == 1) {                             // 心知天气
    if (!strlen(cfg.wxKey)) { if (err) *err = "心知天气需要填 API Key"; return false; }
    url = "https://api.seniverse.com/v3/weather/now.json?key=" + urlEnc(String(cfg.wxKey)) +
          "&location=" + urlEnc(String(cfg.wxCity)) + "&language=zh-Hans&unit=c";
  } else {                                                 // 和风天气
    if (!strlen(cfg.wxKey)) { if (err) *err = "和风天气需要填 API Key"; return false; }
    url = "https://devapi.qweather.com/v7/weather/now?key=" + urlEnc(String(cfg.wxKey)) +
          "&location=" + urlEnc(String(cfg.wxCity));
  }

  WiFiClientSecure client;
  client.setInsecure();                                    // 跳过证书校验，省内存也省事
  client.setBufferSizes(1024, 512);
  HTTPClient http;
  http.setTimeout(12000);
  http.setUserAgent("POCSAG-ESP8266/" FW_VER);
  if (!http.begin(client, url)) { if (err) *err = "URL 解析失败"; return false; }

  int code = http.GET();
  if (code != 200) {
    if (err) *err = "HTTP " + String(code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  wx = Wx();
  if (cfg.wxSrc == 0) {
    int p = body.indexOf("\"current\":");                  // 跳过 current_units 里的同名字段
    if (p < 0) p = 0;
    String t = jval(body, "temperature_2m", p);
    String h = jval(body, "relative_humidity_2m", p);
    String w = jval(body, "weather_code", p);
    String s = jval(body, "wind_speed_10m", p);
    String d = jval(body, "wind_direction_10m", p);
    if (!t.length()) { if (err) *err = "返回数据里没有温度字段"; return false; }
    wx.temp = (int)lroundf(t.toFloat());
    if (h.length()) wx.hum  = (int)h.toInt();
    wx.cond = String(wmoText(w.length() ? w.toInt() : -1));
    if (s.length()) wx.lvl = beaufort(s.toFloat());
    if (d.length()) wx.windDir = String(degToDir(d.toFloat()));
  } else if (cfg.wxSrc == 1) {
    String t = jval(body, "temperature");
    String x = jval(body, "text");
    if (!t.length() && !x.length()) { if (err) *err = "心知天气返回异常（Key 或城市名有误？）"; return false; }
    if (t.length()) wx.temp = (int)lroundf(t.toFloat());
    wx.cond = x;
    // 心知的返回里 location 排在 now 前面，结构为
    //   {"results":[{"location":{"id":"...","name":"南宁",...},"now":{...}}]}
    // 所以用 jval 取第一个 "name" 就是城市名。
    wx.city = jval(body, "name");
  } else {
    String c  = jval(body, "code");
    if (c.length() && c != "200") { if (err) *err = "和风返回 code=" + c; return false; }
    String t  = jval(body, "temp");
    String x  = jval(body, "text");
    String h  = jval(body, "humidity");
    String wd = jval(body, "windDir");
    String ws = jval(body, "windScale");
    if (!t.length() && !x.length()) { if (err) *err = "和风天气返回异常（Key 或 LocationID 有误？）"; return false; }
    if (t.length()) wx.temp = (int)lroundf(t.toFloat());
    if (h.length()) wx.hum = (int)h.toInt();
    if (ws.length()) wx.lvl = (int)ws.toInt();
    wx.cond = x;
    wx.windDir = wd;
  }

  wx.valid = true;
  wx.at = millis();
  wx.text = buildWxText(false);
  DBG("[wx] %s\n", wx.text.c_str());
  return true;
}

// 生成天气文本。numeric=true 时只输出数字机认得的字符
// 天气文本里显示的城市名：用户在页面填的优先，没填就用接口返回的
String wxCityLabel() {
  String s = String(cfg.wxName);
  s.trim();
  if (s.length()) return s;
  return wx.city;
}

String buildWxText(bool numeric) {
  String s;
  time_t ts = time(nullptr);
  bool hasTime = (ts > 1700000000L);
  struct tm* tmv = localtime(&ts);

  if (numeric) {
    if (hasTime) {
      char d[12];
      snprintf(d, sizeof(d), "%02d%02d-%02d%02d", tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min);
      s += d;
      s += "-";
    }
    s += String(wx.temp);
    if (wx.hum >= 0) { s += "-"; s += String(wx.hum); }
    if (wx.lvl > 0)  { s += "-"; s += String(wx.lvl); }
    return s;
  }

  if (hasTime) {
    char d[16];
    snprintf(d, sizeof(d), "%02d-%02d %02d:%02d", tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_hour, tmv->tm_min);
    s += d;
    s += " ";
  }
  // 城市名（只在汉字机上加：数字机只认 0-9 和 A-F，塞汉字也没有意义）
  String city = wxCityLabel();
  if (city.length()) { s += city; s += " "; }
  if (wx.cond.length()) { s += wx.cond; s += " "; }
  s += String(wx.temp);
  s += "℃";
  if (wx.hum >= 0) { s += " "; s += "湿度"; s += String(wx.hum); s += "%"; }
  if (wx.lvl >= 0) {
    s += " ";
    if (wx.windDir.length()) s += wx.windDir;
    s += String(wx.lvl);
    s += "级";
  }
  return s;
}

// 把当前天气发给「天气」列被勾选的行，返回入队条数
int pushWx() {
  bool numeric = (cfg.type == 'N');
  String txt = buildWxText(numeric);
  char gbk[220];
  gbk[0] = 0;
  if (!numeric) utf8ToGbk(txt, gbk, sizeof(gbk));
  else if (txt.length() + 8 < sizeof(gbk)) strncpy(gbk, txt.c_str(), sizeof(gbk) - 1);

  int n = 0;
  char line[TX_LINE_MAX];
  for (uint8_t i = 0; i < PAGER_NUM; i++) {
    if (!cfg.pg[i].wx) continue;          // 只发给「天气」列勾了的行
    snprintf(line, sizeof(line), "#%c%s%c%c%c%s",
             cfg.pg[i].phase, cfg.pg[i].addr, cfg.beep, cfg.pg[i].rate, cfg.type, gbk);
    if (txEnqueue(cfg.pg[i].freq, line, txWaitFor(i, String(gbk)))) n++;
  }
  txTotal += n;
  return n;
}

void wxTick() {
  if (!cfg.wxAuto) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastWxCheck < WX_CHECK_MS) return;
  lastWxCheck = millis();

  time_t ts = time(nullptr);
  if (ts < 1700000000L) return;                    // NTP 还没同步
  struct tm* tmv = localtime(&ts);

  uint32_t today = (uint32_t)(tmv->tm_year + 1900) * 10000UL +
                   (uint32_t)(tmv->tm_mon + 1) * 100UL +
                   (uint32_t)tmv->tm_mday;
  // 跨天就把「已推送」标记清空 —— 这个标记是按【时间点】分别记的，
  // 不能像原来那样只记一个「今天推过了」，否则第一个时间点一推，
  // 后面两个当天就再也不触发了。
  if (today != lastWxDay) { lastWxDay = today; wxDoneMask = 0; }

  // 找出当前这一分钟该推的那个时间点（一次只推一个，避免同一分钟连发）
  int slot = -1;
  for (uint8_t i = 0; i < WX_SLOTS; i++) {
    if (!cfg.wxOn[i]) continue;                    // 这一组没启用
    if (wxDoneMask & (1 << i)) continue;           // 今天这个点已经推过
    if (tmv->tm_hour == cfg.wxHour[i] && tmv->tm_min == cfg.wxMin[i]) { slot = (int)i; break; }
  }
  if (slot < 0) return;

  wxDoneMask |= (1 << slot);
  DBG("[wx] auto push slot %d at %02u:%02u\n", slot, cfg.wxHour[slot], cfg.wxMin[slot]);
  if (fetchWx(nullptr)) pushWx();
}


/* =====================================================================
 *  主流程
 * ===================================================================== */
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(200);

  cfgLoad();
  apStart();
  MDNS.begin("esp8266");
  serverRoutes();
  MDNS.addService("http", "tcp", 80);

  // 后台连，不阻塞：这样 Web 服务立刻可用，
  // 连不上还有 staAutoTick() 每 30 秒重试一次。
  if (ssid.length()) staBegin(ssid, password);
  configTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
}

void loop() {
  server.handleClient();
  MDNS.update();
  receiveString();
  txPump();
  scanTick();
  staTick();        // 推进连接状态机（原版漏了它，导致状态永远停在「连接中」）
  staAutoTick();    // 掉线自动重连

  // OTA 成功后的延后重启：这段等待里继续跑 handleClient()，
  // 让 lwIP 把「OK」响应真正推到空中，别让浏览器只收到一个断连。
  if (otaRebootAt && (int32_t)(millis() - otaRebootAt) >= 0) {
    otaRebootAt = 0;
    DBG("[ota] rebooting\n");
    ESP.restart();
  }

  wxTick();

  static uint32_t lastNtp = 0;
  if (!ntpDone && WiFi.status() == WL_CONNECTED && millis() - lastNtp > 5000) {
    lastNtp = millis();
    if (time(nullptr) > 1700000000L) ntpDone = true;
  }
  yield();
}
