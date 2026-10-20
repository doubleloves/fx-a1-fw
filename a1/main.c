/*
 * A1 空气净化器 自定义固件
 * 平台: EMW3080B / MK3080B (MiCO)
 *
 * 未联网: 连接热点 A1_xxxx (192.168.4.1), 打开网页配置 WiFi
 * 已联网: 访问设备 IP, 打开网页控制设备
 */
#include "mico.h"
#include "mico_board.h"
#include <time.h>
#include "sntp.h"
#include "ota_server.h"
#include "httpd.h"
#include "http-strings.h"
#include "http_parse.h"
#include "CheckSumUtils.h"

#define A1_LOG(...) custom_log("A1", __VA_ARGS__)
#define A1_VERSION "1.0"

/* ------------------- 硬件配置(可按实机调整) ------------------- */
#ifndef A1_FAN_PWM
#define A1_FAN_PWM        MICO_PWM_3
#endif
#ifndef A1_FAN_ENABLE_GPIO
#define A1_FAN_ENABLE_GPIO MICO_GPIO_19
#endif
#ifndef A1_FAN_TACHO_GPIO
#define A1_FAN_TACHO_GPIO  MICO_GPIO_10
#endif
/* 转速脉冲数/转 (常见风扇 2), 用于把 tacho 脉冲换算成 RPM */
#ifndef A1_TACHO_PPR
#define A1_TACHO_PPR       2
#endif
#ifndef A1_LED_PWM
#define A1_LED_PWM         MICO_PWM_4   /* 通道3 = GPIO_13 = LED */
#endif
#define A1_BUTTON_GPIO     MICO_GPIO_23

#define SOFTAP_SSID_PREFIX "A1_"
#define SOFTAP_IP          "192.168.4.1"

/* ------------------- 用户配置 ------------------- */
#define A1_CFG_VERSION 7

typedef struct {
    uint32_t version;
    char     name[33];
    int      power, speed, mode, led, lock;
    int      led_mode;      /* 0 off,1 按风速渐变,2 固定色 */
    int      led_hue;       /* 0-359 */
    int      led_bright;    /* 0-100 */
    uint32_t filter_seconds, total_seconds;
    char     mqtt_host[64]; uint16_t mqtt_port; char mqtt_user[32], mqtt_pass[32];
    struct { uint8_t hour, minute, repeat, action, on; } task[5];
    int8_t   pwm_ch, fan_en_gpio, tacho_gpio, led_pwm;
    /* 灯环夜灯定时: en=启用; on/off 为 时*60+分; -1 表示不用 */
    uint8_t  night_en; int16_t night_on, night_off;
} a1_config_t;

static system_config_t *sys_context = NULL;
static a1_config_t      *cfg = NULL;
static volatile int      cfg_dirty = 0;   /* 延迟落盘: 频繁操作不每次都写flash */
static volatile int      g_panel_hold = 0; /* 滑条操作后延迟落盘, 避免滑动时写flash卡顿 */
static void cfg_save_now(void) { mico_system_context_update(sys_context); cfg_dirty = 0; }
static char              a1_mac[16] = {0};
static char              sta_ip[16] = {0};
static volatile int      softap_mode = 0;
static volatile int      sta_connected = 0;
static volatile int      wifi_evt = -1;
static volatile int      wifi_fail_cnt = 0;
static volatile int      wifi_fallback_done = 0;
static volatile uint32_t fan_pulses = 0, fan_rpm = 0;
/* 倒计时: 剩余秒数(0=未启用), 动作 0=关机 1=开机 */
static volatile int32_t g_cd_sec = 0;
static volatile int     g_cd_action = 0;

/* 扫描结果 */
#define MAX_SCAN_AP 24
typedef struct { char ssid[33]; int16_t rssi; } ap_item_t;
static ap_item_t scan_ap[MAX_SCAN_AP];
static volatile int scan_count = 0;
static volatile int scanning = 0;

static char *cfg_ssid(void) { return sys_context->micoSystemConfig.ssid; }
static char *cfg_key(void)  { return sys_context->micoSystemConfig.user_key; }

/* ------------------- 模式映射 ------------------- */
static int mode_to_speed(int mode)
{
    switch (mode) {
    case 1: return 0;
    case 2: return 20;
    case 3: return 35;
    case 4: return 60;
    case 5: return 85;
    case 6: return 100;
    default: return -1;
    }
}


/* ============ 灯环 = 31 颗 WS2812 (单总线) ============
 * 数据脚 = MICO_GPIO_13。整环由固件逐颗发 WS2812 数据控制, 可开关/调色/调亮度。
 * 默认走"直接写 GPIO 输出寄存器 0x40001000 的 bit12"(见方式C), 时序用计数写。
 * 另保留 3 路 RGB(方式A)供实验, 引脚默认禁用。
 *   扫描: /api/diag/ledsweep / ledpin / ledset   (串口: led ws2scan / pin / z)
 */
#ifndef A1_LED_R_GPIO
#define A1_LED_R_GPIO -1
#endif
#ifndef A1_LED_G_GPIO
#define A1_LED_G_GPIO -1
#endif
#ifndef A1_LED_B_GPIO
#define A1_LED_B_GPIO -1
#endif
#ifndef A1_LED_WS_GPIO
#define A1_LED_WS_GPIO MICO_GPIO_13
#endif
static int g_led_r = A1_LED_R_GPIO, g_led_g = A1_LED_G_GPIO, g_led_b = A1_LED_B_GPIO;
static int g_led_ws_pin = A1_LED_WS_GPIO;     /* WS2812 单总线引脚 */
/* 默认 2=WS2812(直接寄存器通道); 1=三路RGB(引脚默认 -1 禁用, 需自行用 diag 设置)。 */
static volatile int g_led_type = 2;
static volatile int g_led_r_out = 0, g_led_g_out = 0, g_led_b_out = 0;  /* 最近一次输出(调试) */
static volatile bool g_led_pins_ready = false; /* 惰性: 只有用到灯环才初始化引脚 */
static mico_mutex_t g_led_mux;
#define LED_THRESH 40

static void led_pin_cfg(int alias)
{
    if (alias < 0 || alias >= MICO_GPIO_MAX) return;
    MicoGpioInitialize((mico_gpio_t)alias, OUTPUT_PUSH_PULL);
    MicoGpioOutputLow((mico_gpio_t)alias);
}
static void led_hw_init(void)
{
    /* 只初始化当前方式用到的引脚, 避免碰其它可能影响系统的脚 */
    if (g_led_type == 2) {
        led_pin_cfg(g_led_ws_pin);
    } else {
        led_pin_cfg(g_led_r); led_pin_cfg(g_led_g); led_pin_cfg(g_led_b);
    }
    g_led_pins_ready = true;
}
static void led_ch_set(int alias, int on)
{
    if (alias < 0 || alias >= MICO_GPIO_MAX) return;
    if (on) MicoGpioOutputHigh((mico_gpio_t)alias);
    else    MicoGpioOutputLow((mico_gpio_t)alias);
}
static void led_drive(int r, int g, int b)
{
    if (!g_led_pins_ready) return;   /* 开机不动灯环引脚, 只有用到才初始化 */
    mico_rtos_lock_mutex(&g_led_mux);
    led_ch_set(g_led_r, r > LED_THRESH);
    led_ch_set(g_led_g, g > LED_THRESH);
    led_ch_set(g_led_b, b > LED_THRESH);
    mico_rtos_unlock_mutex(&g_led_mux);
}

/* ---- 方式B: WS2812 单总线 (默认 GPIO_13/别名7) ---- */
#define LED_WS_NUM 32
static inline void led_nop(uint32_t n) { while (n--) __asm volatile("nop"); }
/* WS2812 位时序可调(用 nop 次数近似): scale 越大, 高/低电平越长。
 * T1H≈2*scale, T1L≈scale; T0H≈scale, T0L≈2*scale。用 /api/diag/wssweep 标定。 */
static volatile int g_ws_scale = 10;
static inline void led_bit(int one)
{
    mico_gpio_t p = (mico_gpio_t)g_led_ws_pin;
    int s = g_ws_scale < 1 ? 1 : g_ws_scale;
    if (one) { MicoGpioOutputHigh(p); led_nop(s*2); MicoGpioOutputLow(p); led_nop(s); }
    else     { MicoGpioOutputHigh(p); led_nop(s);   MicoGpioOutputLow(p); led_nop(s*2); }
}
static void ws_send_byte(uint8_t b) { for (int i = 7; i >= 0; i--) led_bit((b >> i) & 1); }
/* ---- 方式C: 快速 WS2812 —— 直接写 GPIO 输出寄存器 0x40001000, 避免函数调用开销 ---- */
#ifndef A1_GPIO_OUT_ADDR
#define A1_GPIO_OUT_ADDR 0x40001000
#endif
#define A1_GPIO_OUT (*(volatile uint32_t *)A1_GPIO_OUT_ADDR)
static volatile int g_ws2_scale = 2;
static volatile int g_ws2_bit = 12;      /* 实测: 0x40001000 的 bit12 = 灯环数据脚 */
/* 位时序: 用"重复写 GPIO 寄存器"卡时间。
 * 1 = 高写 g_z_h1 次 + 低写 g_z_l1 次; 0 = 高写 g_z_h0 次 + 低写 g_z_l0 次; MSB 先发。 */
static volatile int g_ws2_scale_unused = 0;
static volatile int g_z_h1 = 8, g_z_l1 = 6, g_z_h0 = 2, g_z_l0 = 6;
static uint32_t g_ws_hi = 0, g_ws_lo = 0;
static void ws2_send_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1 << i)) {
            for (volatile int k = 0; k < g_z_h1; k++) A1_GPIO_OUT = g_ws_hi;
            for (volatile int k = 0; k < g_z_l1; k++) A1_GPIO_OUT = g_ws_lo;
        } else {
            for (volatile int k = 0; k < g_z_h0; k++) A1_GPIO_OUT = g_ws_hi;
            for (volatile int k = 0; k < g_z_l0; k++) A1_GPIO_OUT = g_ws_lo;
        }
    }
}
static volatile int g_ws_num = 31;        /* 灯珠数量(运行时以实机为准调整) */
static volatile int g_ws_dummy = 0;       /* 1=帧首垫一颗哑灯(缓解链首U1冒绿) */
static uint8_t g_ws_frame[96*3];
static void ws2_raw(const uint8_t *data, int leds)
{
    MicoGpioInitialize(MICO_GPIO_13, OUTPUT_PUSH_PULL);
    uint32_t base = A1_GPIO_OUT;
    g_ws_hi = base | (1u<<g_ws2_bit);
    g_ws_lo = base & ~(1u<<g_ws2_bit);
    __disable_irq();
    A1_GPIO_OUT = g_ws_lo;
    { for (volatile int k = 0; k < 9000; k++); }
    if (g_ws_dummy) { ws2_send_byte(0); ws2_send_byte(0); ws2_send_byte(0); }  /* 哑灯=黑 */
    for (int i = 0; i < leds; i++) {
        ws2_send_byte(data[i*3]); ws2_send_byte(data[i*3+1]); ws2_send_byte(data[i*3+2]);
    }
    A1_GPIO_OUT = g_ws_lo;
    __enable_irq();
    { for (volatile int k = 0; k < 9000; k++); }
}
static void ws2_flush(int r, int g, int b)
{
    int n = g_ws_num; if (n < 0) n = 0; if (n > 96) n = 96;
    for (int i = 0; i < n; i++) { g_ws_frame[i*3]=(uint8_t)g; g_ws_frame[i*3+1]=(uint8_t)r; g_ws_frame[i*3+2]=(uint8_t)b; }
    ws2_raw(g_ws_frame, n);
}
/* 扫描 0x40001000 的每个 bit, 找出对应 GPIO13 的那个(能点亮灯环的) */
static volatile int g_ws2_scan = 0;
static void ws2_scan_thread(void *arg)
{
    (void)arg;
    g_ws2_scan = 1;
    MicoGpioInitialize(MICO_GPIO_13, OUTPUT_PUSH_PULL);
    A1_LOG("WS2SCAN: 先开继电器(bit11)给灯环供电");
    A1_GPIO_OUT |= (1u<<11);            /* 开继电器/风机 => 灯环通电 */
    mico_thread_msleep(600);
    for (int bit = 0; bit < 32; bit++) {
        if (bit == 11) continue;          /* 跳过继电器 bit, 保持供电 */
        g_ws2_bit = bit;
        A1_LOG("WS2SCAN bit=%d", bit);
        ws2_flush(255, 0, 0);
        mico_thread_msleep(700);
        ws2_flush(0, 0, 0);
        mico_thread_msleep(200);
    }
    A1_LOG("WS2SCAN done (note which bit made the ring turn RED)");
    g_ws2_scan = 0;
    mico_rtos_delete_thread(NULL);
}

/* ---- 方式D: WS2812 用 GPIO API, 发帧期间关中断(避免 WiFi 中断打断波形) ---- */
static void ws_api_flush(int r, int g, int b)
{
    mico_gpio_t p = (mico_gpio_t)g_led_ws_pin;
    if (!g_led_pins_ready) { MicoGpioInitialize(p, OUTPUT_PUSH_PULL); g_led_pins_ready = true; }
    __disable_irq();
    MicoGpioOutputLow(p); led_nop(2000);
    for (int i = 0; i < LED_WS_NUM; i++) { ws_send_byte((uint8_t)g); ws_send_byte((uint8_t)r); ws_send_byte((uint8_t)b); }
    MicoGpioOutputLow(p);
    __enable_irq();
}
static void ws2812_flush(int r, int g, int b)
{
    if (!g_led_pins_ready) return;
    if (g_led_ws_pin < 0 || g_led_ws_pin >= MICO_GPIO_MAX) return;
    mico_rtos_lock_mutex(&g_led_mux);
    mico_gpio_t p = (mico_gpio_t)g_led_ws_pin;
    MicoGpioInitialize(p, OUTPUT_PUSH_PULL);
    MicoGpioOutputLow(p);
    led_nop(9000);   /* WS2812 复位需 >50us; 之前仅~6us 导致只有第一帧生效 */
    for (int i = 0; i < LED_WS_NUM; i++) { ws_send_byte((uint8_t)g); ws_send_byte((uint8_t)r); ws_send_byte((uint8_t)b); } /* GRB */
    MicoGpioOutputLow(p);
    led_nop(9000);
    mico_rtos_unlock_mutex(&g_led_mux);
}
static void hsl2rgb(int h, int s, int l, int *r, int *g, int *b)
{
    h = ((h % 360) + 360) % 360;
    int c = (100 - (2*l - 100 < 0 ? -(2*l-100) : (2*l-100))) * s / 100;
    int x = c * (60 - (h % 120 - 60 < 0 ? -(h%120-60) : (h%120-60))) / 60;
    int m = l - c/2;
    int rr,gg,bb;
    if (h < 60)      { rr=c; gg=x; bb=0; }
    else if (h < 120){ rr=x; gg=c; bb=0; }
    else if (h < 180){ rr=0; gg=c; bb=x; }
    else if (h < 240){ rr=0; gg=x; bb=c; }
    else if (h < 300){ rr=x; gg=0; bb=c; }
    else             { rr=c; gg=0; bb=x; }
    *r=(rr+m)*255/100; *g=(gg+m)*255/100; *b=(bb+m)*255/100;
}
static void led_ring_refresh(void)
{
    if (cfg->led && cfg->led_mode >= 3) {   /* 3/4/5/6 = 特效, 交给 fx 线程输出 */
        g_led_r_out = g_led_g_out = g_led_b_out = 0;
        return;
    }
    int r = 0, g = 0, b = 0;
    if (cfg->led && cfg->led_mode != 0) {
        if (cfg->led_mode == 2) {
            hsl2rgb(cfg->led_hue, 100, 50, &r,&g,&b);
        } else if (cfg->power) {
            int sp = cfg->speed; if (sp < 0) sp = 0; if (sp > 100) sp = 100;
            int hue = 240 - (240 * sp) / 100;   /* 蓝 -> 红 */
            hsl2rgb(hue, 100, 50, &r,&g,&b);
            /* 随速模式: 亮度也随速度线性增加(10%~100%), 速度越快越亮 */
            int ss = 10 + (90 * sp) / 100;
            r = r * ss / 100; g = g * ss / 100; b = b * ss / 100;
        } else {
            /* 关机且随速模式 => 熄灭 */
            r = g = b = 0;
        }
        /* 亮度: 整帧线性缩放 */
        int br = cfg->led_bright; if (br < 0) br = 0; if (br > 100) br = 100;
        r = r * br / 100; g = g * br / 100; b = b * br / 100;
    }
    g_led_r_out = r; g_led_g_out = g; g_led_b_out = b;
    if (g_led_type == 2) ws2_flush(r, g, b);   /* 快速寄存器通道(bit12), 才能正确时序 */
    else                 led_drive(r, g, b);
}

/* ---- 灯环特效: 3=跑马灯 4=流光 5=呼吸 6=静态彩虹 ---- */
static volatile int g_fx_pos = 0;
static void led_fx_thread(void *arg)
{
    (void)arg;
    while (1) {
        int n = g_ws_num; if (n < 1) n = 1; if (n > 96) n = 96;
        if (cfg->led && cfg->led_mode >= 3) {
            int br = cfg->led_bright; if (br < 0) br = 0; if (br > 100) br = 100;
            int r, g, b;
            for (int k = 0; k < n; k++) {
                if (cfg->led_mode == 3) {              /* 跑马灯: 亮点+拖尾 */
                    int head = g_fx_pos % n;
                    int d = (k - head + n) % n;
                    int v = (d < 6) ? (6 - d) : 0;
                    hsl2rgb(cfg->led_hue, 100, 50, &r,&g,&b);
                    g_ws_frame[k*3]   = (uint8_t)(g * v / 6 * br / 100);
                    g_ws_frame[k*3+1] = (uint8_t)(r * v / 6 * br / 100);
                    g_ws_frame[k*3+2] = (uint8_t)(b * v / 6 * br / 100);
                } else if (cfg->led_mode == 4) {       /* 流光: 彩虹绕圈流动 */
                    int hue = (k * 360 / n + g_fx_pos * 6) % 360;
                    hsl2rgb(hue, 100, 50, &r,&g,&b);
                    g_ws_frame[k*3]   = (uint8_t)(g * br / 100);
                    g_ws_frame[k*3+1] = (uint8_t)(r * br / 100);
                    g_ws_frame[k*3+2] = (uint8_t)(b * br / 100);
                } else if (cfg->led_mode == 5) {       /* 呼吸 */
                    int v = g_fx_pos % 60; if (v > 30) v = 60 - v; v = v * 100 / 30;
                    hsl2rgb(cfg->led_hue, 100, 50, &r,&g,&b);
                    g_ws_frame[k*3]   = (uint8_t)(g * v / 100 * br / 100);
                    g_ws_frame[k*3+1] = (uint8_t)(r * v / 100 * br / 100);
                    g_ws_frame[k*3+2] = (uint8_t)(b * v / 100 * br / 100);
                } else {                                /* 6: 静态彩虹 */
                    int hue = (k * 360 / n) % 360;
                    hsl2rgb(hue, 100, 50, &r,&g,&b);
                    g_ws_frame[k*3]   = (uint8_t)(g * br / 100);
                    g_ws_frame[k*3+1] = (uint8_t)(r * br / 100);
                    g_ws_frame[k*3+2] = (uint8_t)(b * br / 100);
                }
            }
            ws2_raw(g_ws_frame, n);
            g_fx_pos++;
            mico_thread_msleep(40);
        } else {
            mico_thread_msleep(80);
        }
    }
}


/* ================= MQTT + HomeAssistant ================= */
static void a1_apply(void);
static void mqtt_pub(const char *topic, const char *payload, int retain);
#include "MQTTClient.h"
static Client  mqtt_c;
static Network mqtt_n;
static volatile bool mqtt_connected = false;
static char mqtt_set[64], mqtt_state[64], mqtt_avail[64], mqtt_disc[80];
static char mqtt_name[24];

static void mqtt_pub(const char *topic, const char *payload, int retain)
{
    MQTTMessage m; memset(&m,0,sizeof(m));
    m.qos = QOS0; m.retained = retain; m.payload = (void*)payload; m.payloadlen = strlen(payload);
    MQTTPublish(&mqtt_c, topic, &m);
}
static void mqtt_pub_state(void)
{
    char js[256];
    snprintf(js, sizeof(js),
        "{\"mac\":\"%s\",\"on\":%d,\"speed\":%d,\"mode\":%d,\"led\":%d,\"lock\":%d,\"rpm\":%lu}",
        a1_mac, cfg->power, cfg->speed, cfg->mode, cfg->led, cfg->lock, (unsigned long)fan_rpm);
    mqtt_pub(mqtt_state, js, 0);
}
static void mqtt_ha_discovery(void)
{
    char js[600];
    snprintf(js, sizeof(js),
      "{\"name\":\"%s\",\"uniq_id\":\"%s\",\"avty_t\":\"%s\",\"stat_t\":\"%s\","
      "\"cmd_t\":\"%s\",\"pct_cmd_t\":\"%s\",\"pct_stat_t\":\"%s\","
      "\"stat_on\":1,\"stat_off\":0,\"pl_on\":\"1\",\"pl_off\":\"0\","
      "\"pct_cmd_tpl\":\"{ \\\"mac\\\":\\\"%s\\\", \\\"speed\\\": {{ value }} }\","
      "\"pct_stat_tpl\":\"{{ value_json.speed }}\"}",
      mqtt_name, mqtt_name, mqtt_avail, mqtt_state, mqtt_set, mqtt_set, mqtt_state, a1_mac);
    mqtt_pub(mqtt_disc, js, 1);
}
static int json_int(const char *js, const char *key, int def)
{
    char pat[32]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p==' ') p++;
    return atoi(p);
}
static void mqtt_on_message(MessageData *md)
{
    char buf[256]; int n = md->message->payloadlen;
    if (n > (int)sizeof(buf)-1) n = sizeof(buf)-1;
    memcpy(buf, md->message->payload, n); buf[n]=0;
    A1_LOG("MQTT recv: %s", buf);
    int v;
    if ((v=json_int(buf,"on",-1))>=0) { cfg->power = v?1:0; }
    if ((v=json_int(buf,"speed",-1))>=0) { cfg->speed=v; cfg->mode=0; cfg->power=(v>0); }
    if ((v=json_int(buf,"mode",-1))>=0) { cfg->mode=v; cfg->power=(v>0); }
    if ((v=json_int(buf,"led",-1))>=0) { cfg->led=v?1:0; }
    if ((v=json_int(buf,"lock",-1))>=0) { cfg->lock=v?1:0; }
    a1_apply(); cfg_dirty = 1;
    mqtt_pub_state();
}
static bool wifi_is_up(void)
{
    IPStatusTypedef ip;
    if (micoWlanGetIPStatus(&ip, Station) != kNoErr) return false;
    return (ip.ip[0] && strcmp(ip.ip, "0.0.0.0") != 0);   /* 0.0.0.0 视为未连接 */
}

static void a1_mqtt_thread(void *arg)
{
    (void)arg;
    while (1) {
        if (strlen(cfg->mqtt_host) == 0 || !wifi_is_up()) { mico_thread_msleep(3000); continue; }
        ssl_opts ssl; memset(&ssl,0,sizeof(ssl)); ssl.ssl_enable = false;
        if (NewNetwork(&mqtt_n, cfg->mqtt_host, cfg->mqtt_port, ssl) != MQTT_SUCCESS) {
            A1_LOG("mqtt net connect fail, retry"); mico_thread_msleep(3000); continue;
        }
        if (MQTTClientInit(&mqtt_c, &mqtt_n, 5000) != MQTT_SUCCESS) { A1_LOG("mqtt init fail"); mico_thread_msleep(3000); continue; }
        MQTTPacket_connectData cd = MQTTPacket_connectData_initializer;
        cd.MQTTVersion = 4; cd.clientID.cstring = mqtt_name;
        cd.username.cstring = cfg->mqtt_user; cd.password.cstring = cfg->mqtt_pass;
        cd.keepAliveInterval = 60; cd.cleansession = 1;
        if (MQTTConnect(&mqtt_c, &cd) != MQTT_SUCCESS) { A1_LOG("mqtt connect fail"); MQTTClientDeinit(&mqtt_c); mico_thread_msleep(3000); continue; }
        MQTTSubscribe(&mqtt_c, mqtt_set, QOS0, mqtt_on_message);
        mqtt_connected = true;
        A1_LOG("mqtt connected, set=%s", mqtt_set);
        mqtt_pub(mqtt_avail, "online", 1);
        mqtt_ha_discovery();
        mqtt_pub_state();
        int tick = 0;
        while (1) {
            if (MQTTYield(&mqtt_c, 1000) != MQTT_SUCCESS) break;
            if (++tick >= 5) { tick = 0; mqtt_pub_state(); }
        }
        mqtt_connected = false;
        MQTTDisconnect(&mqtt_c); MQTTClientDeinit(&mqtt_c);
        A1_LOG("mqtt disconnected, retry in 5s");
        mico_thread_msleep(5000);
    }
}

/* ------------------- 硬件 ------------------- */
static void tacho_irq(void *arg) { (void)arg; fan_pulses++; }

static void a1_hw_init(void)
{
    if (cfg->fan_en_gpio >= 0) {
        MicoGpioInitialize((mico_gpio_t)cfg->fan_en_gpio, OUTPUT_PUSH_PULL);
        MicoGpioOutputHigh((mico_gpio_t)cfg->fan_en_gpio); /* 继电器断开 */
    }
    MicoPwmInitialize((mico_pwm_t)cfg->pwm_ch, 25000, 0.0f);
    MicoPwmStop((mico_pwm_t)cfg->pwm_ch);
    /* 注意: 不在这里初始化灯环引脚(曾导致 WiFi 挂掉), 改为用户操作灯环时才初始化 */
    if (cfg->tacho_gpio >= 0) {
        MicoGpioInitialize((mico_gpio_t)cfg->tacho_gpio, INPUT_PULL_UP);
        MicoGpioEnableIRQ((mico_gpio_t)cfg->tacho_gpio, IRQ_TRIGGER_RISING_EDGE, tacho_irq, NULL);
    }
    MicoGpioInitialize(A1_BUTTON_GPIO, INPUT_PULL_UP);
}

static bool is_led_pin(int p)
{
    if (p < 0) return false;
    if (g_led_type == 2) return (p == g_led_ws_pin);           /* 只有 WS 数据脚 */
    return (p == g_led_r || p == g_led_g || p == g_led_b);      /* 三路脚 */
}
static void a1_fan_set(int speed)
{
    if (speed < 0) speed = 0;
    if (speed > 100) speed = 100;
    /* 风机只有 PWM。若配置的“继电器”脚与灯环某一色冲突, 则跳过(避免抢脚)。 */
    bool use_en = (cfg->fan_en_gpio >= 0 && !is_led_pin(cfg->fan_en_gpio));
    if (speed == 0) {
        MicoPwmInitialize((mico_pwm_t)cfg->pwm_ch, 25000, 0.0f);
        MicoPwmStart((mico_pwm_t)cfg->pwm_ch);
        if (use_en) MicoGpioOutputHigh((mico_gpio_t)cfg->fan_en_gpio);
    } else {
        if (use_en) MicoGpioOutputLow((mico_gpio_t)cfg->fan_en_gpio);
        MicoPwmInitialize((mico_pwm_t)cfg->pwm_ch, 25000, (float)(100 - speed));
        MicoPwmStart((mico_pwm_t)cfg->pwm_ch);
    }
}
static void a1_led_set(int on)
{
    (void)on;
    led_ring_refresh();
}
static mico_mutex_t g_apply_mux;
static void a1_apply(void)
{
    mico_rtos_lock_mutex(&g_apply_mux);
    if (cfg->power && !cfg->lock) {
        int sp = cfg->speed, ms = mode_to_speed(cfg->mode);
        if (ms >= 0) sp = ms;
        if (sp < 1) sp = 50;   /* 开机默认风速 50% */
        a1_fan_set(sp);
    } else a1_fan_set(0);
    a1_led_set(cfg->led);
    mico_rtos_unlock_mutex(&g_apply_mux);
}

/* 只更新风机(滑条用): 避免每次滑动都重发整帧灯环, 提高手感 */
static void a1_apply_fan(void)
{
    mico_rtos_lock_mutex(&g_apply_mux);
    if (cfg->power && !cfg->lock) {
        int sp = cfg->speed, ms = mode_to_speed(cfg->mode);
        if (ms >= 0) sp = ms;
        if (sp < 1) sp = 50;
        a1_fan_set(sp);
    } else a1_fan_set(0);
    if (cfg->led && cfg->led_mode == 1) a1_led_set(cfg->led);  /* 随速灯效才需要重绘 */
    mico_rtos_unlock_mutex(&g_apply_mux);
}

/* ------------------- WiFi ------------------- */
static void scan_completed_cb(ScanResult *pApList, void *arg)
{
    (void)arg;
    scan_count = 0;
    scanning = 0;
    if (pApList) {
        int n = (unsigned char)pApList->ApNum;
        for (int i = 0; i < n && i < MAX_SCAN_AP; i++) {
            strncpy(scan_ap[i].ssid, pApList->ApList[i].ssid, sizeof(scan_ap[i].ssid) - 1);
            scan_ap[i].rssi = pApList->ApList[i].rssi;
            scan_count++;
        }
    }
    A1_LOG("scan done, %d ap", scan_count);
}

static void dhcp_cb(IPStatusTypedef *pnet, void *arg)
{
    (void)arg;
    if (pnet && pnet->ip[0]) {
        strncpy(sta_ip, pnet->ip, sizeof(sta_ip) - 1);
        sta_connected = 1;
        A1_LOG("got IP %s", sta_ip);
    }
}

/* ---- WiFi 事件回调(诊断/加固) ---- */
static void wifi_status_cb(WiFiEvent status, void *arg)
{
    (void)arg;
    wifi_evt = (int)status;
    A1_LOG("wifi status changed -> %d", (int)status);
}
static void wifi_fail_cb(OSStatus err, void *arg)
{
    (void)arg;
    wifi_fail_cnt++;
    A1_LOG("wifi connect FAILED err=%d (cnt=%d)", (int)err, wifi_fail_cnt);
}
static void wifi_fatal_cb(void *arg) { (void)arg; A1_LOG("wifi FATAL error"); }

static void wifi_start_softap(void)
{
    network_InitTypeDef_st n;
    memset(&n, 0, sizeof(n));
    n.wifi_mode = Soft_AP;
    /* 热点名 = A1_ + MAC 后四位; MAC 为空时用固定后缀, 保证热点名有效 */
    char s0 = '0', s1 = '0', s2 = '0', s3 = '0';
    if (a1_mac[9] && a1_mac[10] && a1_mac[12] && a1_mac[13]) {
        s0 = a1_mac[9]; s1 = a1_mac[10]; s2 = a1_mac[12]; s3 = a1_mac[13];
    }
    snprintf(n.wifi_ssid, sizeof(n.wifi_ssid), SOFTAP_SSID_PREFIX "%c%c%c%c", s0, s1, s2, s3);
    strcpy(n.wifi_key, "");
    strcpy((char *)n.local_ip_addr, SOFTAP_IP);
    strcpy((char *)n.net_mask, "255.255.255.0");
    strcpy((char *)n.gateway_ip_addr, SOFTAP_IP);
    n.dhcpMode = DHCP_Server;
    softap_mode = 1;
    sta_connected = 0;
    micoWlanStart(&n);
    A1_LOG("soft ap up: %s @ %s", n.wifi_ssid, SOFTAP_IP);
}

static void wifi_start_sta(void)
{
    network_InitTypeDef_st n;
    memset(&n, 0, sizeof(n));
    strncpy(n.wifi_ssid, cfg_ssid(), sizeof(n.wifi_ssid) - 1);
    strncpy(n.wifi_key, cfg_key(), sizeof(n.wifi_key) - 1);
    n.wifi_mode = Station;
    n.dhcpMode = DHCP_Client;
    n.wifi_retry_interval = 6000;
    softap_mode = 0;
    sta_connected = 0;
    wifi_fallback_done = 0;
    micoWlanStart(&n);
    A1_LOG("sta connect ssid=%s", n.wifi_ssid);
}

/* WiFi 守护: 跟踪连接状态; STA 约 40s 连不上自动回退到热点配网, 保证一定连得上 */
static void a1_wifi_thread(void *arg)
{
    (void)arg;
    mico_thread_msleep(12000);          /* 给首次连接时间 */
    int miss = 0;
    while (1) {
        mico_thread_msleep(5000);
        if (softap_mode) { miss = 0; continue; }
        IPStatusTypedef ip;
        if (micoWlanGetIPStatus(&ip, Station) == kNoErr && ip.ip[0] && strcmp(ip.ip, "0.0.0.0") != 0) {
            if (!sta_connected || strncmp(sta_ip, ip.ip, sizeof(sta_ip) - 1) != 0) {
                strncpy(sta_ip, ip.ip, sizeof(sta_ip) - 1);
                A1_LOG("wifi connected ip=%s", sta_ip);
            }
            sta_connected = 1;
            miss = 0;
        } else {
            sta_connected = 0;
            if (++miss >= 7 && !wifi_fallback_done) {
                A1_LOG("STA 连接失败, 回退到热点配网");
                wifi_fallback_done = 1;
                wifi_start_softap();
            }
        }
    }
}

/* ------------------- HTTP ------------------- */
#define HDRS (HTTPD_HDR_ADD_SERVER | HTTPD_HDR_ADD_CONN_CLOSE | HTTPD_HDR_ADD_PRAGMA_NO_CACHE)

static int send_json(httpd_request_t *req, const char *js)
{
    httpd_send_all_header(req, HTTP_RES_200, strlen(js), HTTP_CONTENT_JSON_STR);
    httpd_send_body(req->sock, (const unsigned char *)js, strlen(js));
    return kNoErr;
}
static int send_html(httpd_request_t *req, const char *h)
{
    httpd_send_all_header(req, HTTP_RES_200, strlen(h), HTTP_CONTENT_HTML_STR);
    httpd_send_body(req->sock, (const unsigned char *)h, strlen(h));
    return kNoErr;
}
static int get_int_tag(httpd_request_t *req, const char *tag, int def)
{
    char b[16] = {0};
    if (httpd_get_tag_from_url(req, (char *)tag, b, sizeof(b) - 1) != kNoErr) return def;
    if (b[0] == 0) return def;
    return (int)strtol(b, NULL, 0);   /* 支持 0x 十六进制 */
}
static void get_str_tag(httpd_request_t *req, const char *tag, char *out, int len)
{
    out[0] = 0;
    httpd_get_tag_from_url(req, (char *)tag, out, len - 1);
}

static int h_status(httpd_request_t *req)
{
    char js[1600];
    int n = snprintf(js, sizeof(js),
        "{\"mac\":\"%s\",\"ip\":\"%s\",\"softap\":%d,"
        "\"power\":%d,\"speed\":%d,\"mode\":%d,\"led\":%d,\"lock\":%d,"
        "\"led_mode\":%d,\"led_hue\":%d,\"led_bright\":%d,\"led_type\":%d,"
        "\"led_r\":%d,\"led_g\":%d,\"led_b\":%d,\"led_ws\":%d,"
        "\"out_r\":%d,\"out_g\":%d,\"out_b\":%d,\"ws_scale\":%d,"
        "\"night_en\":%d,\"night_on\":%d,\"night_off\":%d,"
        "\"cd_sec\":%ld,\"cd_action\":%d,"
        "\"rpm\":%lu,\"filter\":%lu,\"total\":%lu,"
        "\"ssid\":\"%s\",\"ver\":\"%s\",\"task\":[",
        a1_mac, sta_ip, softap_mode,
        cfg->power, cfg->speed, cfg->mode, cfg->led, cfg->lock,
        cfg->led_mode, cfg->led_hue, cfg->led_bright, g_led_type,
        g_led_r, g_led_g, g_led_b, g_led_ws_pin,
        g_led_r_out, g_led_g_out, g_led_b_out, g_ws_scale,
        cfg->night_en, cfg->night_on, cfg->night_off,
        (long)g_cd_sec, g_cd_action,
        (unsigned long)fan_rpm, (unsigned long)cfg->filter_seconds,
        (unsigned long)cfg->total_seconds, cfg_ssid(), A1_VERSION);
    for (int i = 0; i < 5 && n > 0 && n < (int)sizeof(js); i++) {
        n += snprintf(js + n, sizeof(js) - n,
            "%s{\"h\":%d,\"m\":%d,\"r\":%d,\"a\":%d,\"on\":%d}",
            i ? "," : "", cfg->task[i].hour, cfg->task[i].minute,
            cfg->task[i].repeat, cfg->task[i].action, cfg->task[i].on);
    }
    if (n < 0) n = 0;
    if (n >= (int)sizeof(js)) n = sizeof(js) - 1;
    snprintf(js + n, sizeof(js) - n, "]}");
    return send_json(req, js);
}
static int h_power(httpd_request_t *req){ int v=get_int_tag(req,"value",-1); if(v==0||v==1) cfg->power=v; a1_apply(); cfg_dirty = 1; return h_status(req);}
static int h_speed(httpd_request_t *req){ int v=get_int_tag(req,"value",-1); if(v>=0){cfg->speed=v;cfg->mode=0;cfg->power=(v>0);} a1_apply_fan(); cfg_dirty = 1; g_panel_hold = 2; return h_status(req);}
static int h_mode(httpd_request_t *req){ int v=get_int_tag(req,"value",-1); if(v>=0&&v<=6){cfg->mode=v;cfg->power=(v!=0);} a1_apply(); cfg_dirty = 1; return h_status(req);}
static int h_led(httpd_request_t *req){
    int v=get_int_tag(req,"value",-1); if(v==0||v==1) cfg->led=v;
    int mode=get_int_tag(req,"mode",-1); if(mode>=0&&mode<=6) cfg->led_mode=mode;
    int hue=get_int_tag(req,"hue",-1); if(hue>=0&&hue<=359) cfg->led_hue=hue;
    int br=get_int_tag(req,"bright",-1); if(br>=0&&br<=100) cfg->led_bright=br;
    int ty=get_int_tag(req,"type",-1); if(ty>=0&&ty<=2) g_led_type=ty;
    int nen=get_int_tag(req,"night",-1); if(nen>=0) cfg->night_en=nen?1:0;
    int non=get_int_tag(req,"n_on",-100000); if(non>-100000) cfg->night_on=(int16_t)non;
    int nof=get_int_tag(req,"n_off",-100000); if(nof>-100000) cfg->night_off=(int16_t)nof;
    led_hw_init();   /* 用户操作灯环时才初始化对应引脚 */
    a1_apply(); cfg_dirty = 1; return h_status(req);
}
static int h_lock(httpd_request_t *req){ int v=get_int_tag(req,"value",-1); if(v==0||v==1) cfg->lock=v; a1_apply(); cfg_dirty = 1; return h_status(req);}
static int h_filter_reset(httpd_request_t *req){ cfg->filter_seconds=0; mico_system_context_update(sys_context); return h_status(req);}
static int h_reboot(httpd_request_t *req){ send_json(req,"{\"ok\":1}"); mico_thread_msleep(300); mico_system_power_perform(sys_context, eState_Software_Reset); return kNoErr;}
static int h_factory(httpd_request_t *req){ send_json(req,"{\"ok\":1}"); mico_thread_msleep(300); mico_system_context_restore(sys_context); return kNoErr;}

static int h_wifi_scan(httpd_request_t *req)
{
    scanning = 1; scan_count = 0;
    micoWlanStartScan();
    int tries = 0;
    while (scanning && tries++ < 60) mico_thread_msleep(100);
    char *js = malloc(64 + MAX_SCAN_AP * 48);
    if (!js) return send_json(req, "{\"aps\":[]}");
    int n = sprintf(js, "{\"aps\":[");
    for (int i = 0; i < scan_count; i++)
        n += sprintf(js + n, "%s{\"ssid\":\"%s\",\"rssi\":%d}", i ? "," : "", scan_ap[i].ssid, scan_ap[i].rssi);
    sprintf(js + n, "]}");
    int r = send_json(req, js);
    free(js);
    return r;
}

static int h_wifi_status(httpd_request_t *req)
{
    char js[128];
    snprintf(js, sizeof(js),
             "{\"configured\":%d,\"connected\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"softap\":%d,\"evt\":%d,\"fails\":%d}",
             strlen(cfg_ssid()) > 0, sta_connected, sta_ip, cfg_ssid(), softap_mode, wifi_evt, wifi_fail_cnt);
    return send_json(req, js);
}

static int h_wifi_set(httpd_request_t *req)
{
    char ssid[33] = {0}, pass[65] = {0};
    get_str_tag(req, "ssid", ssid, sizeof(ssid));
    get_str_tag(req, "pass", pass, sizeof(pass));
    if (ssid[0] == 0) return send_json(req, "{\"ok\":0,\"err\":\"no ssid\"}");
    system_config_t *sc = sys_context;
    strncpy(sc->micoSystemConfig.ssid, ssid, sizeof(sc->micoSystemConfig.ssid) - 1);
    strncpy(sc->micoSystemConfig.user_key, pass, sizeof(sc->micoSystemConfig.user_key) - 1);
    sc->micoSystemConfig.user_keyLength = strlen(pass);
    sc->micoSystemConfig.configured = allConfigured;
    mico_system_context_update(sys_context);
    send_json(req, "{\"ok\":1}");
    A1_LOG("wifi set to %s, rebooting", ssid);
    mico_thread_msleep(500);
    mico_system_power_perform(sys_context, eState_Software_Reset);
    return kNoErr;
}

/* 配网页面 */
static const char page_setup[] =
"<!doctype html><html lang=zh><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'><title>A1 配网</title>"
"<style>body{font-family:sans-serif;margin:16px;max-width:520px}"
".c{border:1px solid #ddd;border-radius:10px;padding:12px;margin:10px 0}"
"input,button{font-size:16px;padding:8px;width:100%;box-sizing:border-box;margin:6px 0}"
"button{background:#1677ff;color:#fff;border:0;border-radius:8px}"
".ap{border-bottom:1px solid #eee;padding:8px;cursor:pointer}</style></head><body>"
"<h2>A1 配网</h2>"
"<div class=c><button onclick='scan()'>扫描 WiFi</button><div id=aps></div></div>"
"<div class=c><input id=ssid placeholder='WiFi 名称'><input id=pass placeholder='密码'>"
"<button onclick='save()'>保存并连接</button></div>"
"<script>"
"function scan(){document.getElementById('aps').innerHTML='扫描中...';"
"fetch('/api/wifi/scan?x='+Date.now()).then(r=>r.json()).then(d=>{"
"var h='';d.aps.forEach(a=>{h+='<div class=ap onclick=\"pick(\\''+a.ssid+'\\')\">'+a.ssid+' <small>'+a.rssi+'</small></div>'});"
"document.getElementById('aps').innerHTML=h||'未找到网络';})}"
"function pick(s){document.getElementById('ssid').value=s}"
"function save(){var s=document.getElementById('ssid').value,p=document.getElementById('pass').value;"
"fetch('/api/wifi/set?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)).then(r=>r.json())"
".then(d=>{alert(d.ok?'已保存,设备将重启连接':'失败');});}"
"scan();"
"</script></body></html>";

/* 控制页面 */
static const char page_ctrl[] =
"<!doctype html><html lang=zh><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>A1 净化器</title><style>"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}"
"body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Microsoft YaHei',sans-serif;background:#0f1420;color:#e8eefc}"
".wrap{max-width:460px;margin:0 auto;padding:16px 14px 40px}"
"h1{font-size:20px;font-weight:600;margin:8px 0 16px;letter-spacing:.5px}"
".card{background:linear-gradient(180deg,#1a2133,#151b2a);border:1px solid #24304a;border-radius:16px;padding:16px;margin-bottom:14px;box-shadow:0 4px 16px rgba(0,0,0,.25)}"
".lbl{font-size:13px;color:#8ea0c0;margin-bottom:10px;display:flex;justify-content:space-between;align-items:center}"
".val{color:#4da3ff;font-weight:600;font-size:15px}"
".big{display:flex;gap:10px}"
".power{flex:1;height:56px;border:0;border-radius:14px;font-size:17px;font-weight:600;color:#fff;background:#333c52;cursor:pointer;transition:.2s}"
".power.on{background:linear-gradient(135deg,#2ecc71,#1abc9c);box-shadow:0 4px 14px rgba(46,204,113,.4)}"
".seg{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}"
".seg button{height:46px;border:1px solid #2b3752;border-radius:12px;background:#1b2233;color:#b9c6e0;font-size:13px;cursor:pointer;transition:.15s}"
".seg button.active{background:linear-gradient(135deg,#4da3ff,#2f7fe0);color:#fff;border-color:transparent;box-shadow:0 3px 10px rgba(77,163,255,.35)}"
".trow{display:flex;gap:8px;align-items:center;margin-top:8px}"
".trow input,.trow select{height:34px;background:#1b2233;border:1px solid #2b3752;border-radius:8px;color:#e8eefc;padding:0 6px;font-size:13px;min-width:0}"
".trow input[type=time]{flex:1.2}.trow select{flex:1}"
"input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:6px;background:#2b3752;outline:none}"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#4da3ff;box-shadow:0 2px 8px rgba(77,163,255,.6);cursor:pointer}"
"input[type=range].hue{background:linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}"
".row{display:flex;gap:10px;align-items:center;margin-top:12px}"
".row .tag{font-size:13px;color:#8ea0c0;min-width:52px}"
".sw{position:relative;width:52px;height:30px;border-radius:15px;background:#333c52;transition:.2s;cursor:pointer}"
".sw.on{background:#4da3ff}"
".sw i{position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}"
".sw.on i{left:25px}"
".stat{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;text-align:center}"
".stat .n{font-size:20px;font-weight:700;color:#4da3ff}.stat .t{font-size:12px;color:#8ea0c0;margin-top:2px}"
"</style></head><body><div class=wrap>"
"<h1>A1 空气净化器</h1>"
"<div class=card><div class=big><button id=pw class=power onclick='tog()'>开机 / 关机</button></div></div>"
"<div class=card><div class=lbl><span>风速</span><span class=val id=sv>0%</span></div>"
"<input type=range id=sp min=1 max=100 oninput='setS(this.value)'></div>"
"<div class=card><div class=lbl><span>模式</span></div><div class=seg id=seg>"
"<button data-m=2 onclick='setM(2)'>睡眠</button><button data-m=3 onclick='setM(3)'>低速</button>"
"<button data-m=4 onclick='setM(4)'>中速</button><button data-m=5 onclick='setM(5)'>高速</button>"
"<button data-m=6 onclick='setM(6)'>极速</button></div></div>"
"<div class=card><div class=lbl><span>灯环</span><div class='sw' id=lsw onclick='togL()'><i></i></div></div>"
"<div class=lbl style='margin-top:4px'><span>效果</span></div>"
"<div class=seg style='grid-template-columns:repeat(3,1fr)'>"
"<button onclick='setLM(1)' id=lm1>随速</button><button onclick='setLM(2)' id=lm2>固定</button><button onclick='setLM(3)' id=lm3>跑马</button>"
"<button onclick='setLM(4)' id=lm4>流光</button><button onclick='setLM(5)' id=lm5>呼吸</button><button onclick='setLM(6)' id=lm6>彩虹</button></div>"
"<div class=lbl style='margin-top:12px'><span>颜色</span><span class=val id=hv>200</span></div>"
"<input type=range class=hue id=hue min=0 max=359 oninput='setH(this.value)'>"
"<div class=lbl style='margin-top:12px'><span>亮度</span><span class=val id=bv>80</span></div>"
"<input type=range id=br min=0 max=100 oninput='setB(this.value)'></div>"
"<div class=card><div class=lbl><span>夜灯定时</span><div class='sw' id=nsw onclick='togN()'><i></i></div></div>"
"<div class=row><span class=tag>开</span><input type=time id=nt_on style='flex:1;height:38px;background:#1b2233;border:1px solid #2b3752;border-radius:10px;color:#e8eefc;padding:0 8px;box-sizing:border-box'></div>"
"<div class=row><span class=tag>关</span><input type=time id=nt_off style='flex:1;height:38px;background:#1b2233;border:1px solid #2b3752;border-radius:10px;color:#e8eefc;padding:0 8px;box-sizing:border-box'></div>"
"<button onclick='saveN()' style='width:100%;height:40px;margin-top:8px;border:0;border-radius:10px;background:linear-gradient(135deg,#4da3ff,#2f7fe0);color:#fff;font-size:14px'>保存定时</button>"
"<div style='color:#8aa0c8;font-size:12px;margin-top:6px'>到点自动点亮/熄灭灯环(夜灯), 按设备时间</div></div>"
"<div class=card><div class=lbl><span>定时开关</span><span style='font-size:11px;color:#5a6b8c'>需联网对时</span></div>"
"<div class=trow><input type=time id=t0t onchange='saveT(0)'><select id=t0a onchange='saveT(0)'><option value=1>开</option><option value=0>关</option></select><select id=t0r onchange='saveT(0)'><option value=127>每天</option><option value=31>工作日</option><option value=96>周末</option><option value=0>一次</option></select><div class='sw' id=t0e onclick='togT(0)'><i></i></div></div>"
"<div class=trow><input type=time id=t1t onchange='saveT(1)'><select id=t1a onchange='saveT(1)'><option value=1>开</option><option value=0>关</option></select><select id=t1r onchange='saveT(1)'><option value=127>每天</option><option value=31>工作日</option><option value=96>周末</option><option value=0>一次</option></select><div class='sw' id=t1e onclick='togT(1)'><i></i></div></div>"
"<div class=trow><input type=time id=t2t onchange='saveT(2)'><select id=t2a onchange='saveT(2)'><option value=1>开</option><option value=0>关</option></select><select id=t2r onchange='saveT(2)'><option value=127>每天</option><option value=31>工作日</option><option value=96>周末</option><option value=0>一次</option></select><div class='sw' id=t2e onclick='togT(2)'><i></i></div></div>"
"<div class=trow><input type=time id=t3t onchange='saveT(3)'><select id=t3a onchange='saveT(3)'><option value=1>开</option><option value=0>关</option></select><select id=t3r onchange='saveT(3)'><option value=127>每天</option><option value=31>工作日</option><option value=96>周末</option><option value=0>一次</option></select><div class='sw' id=t3e onclick='togT(3)'><i></i></div></div>"
"<div class=trow><input type=time id=t4t onchange='saveT(4)'><select id=t4a onchange='saveT(4)'><option value=1>开</option><option value=0>关</option></select><select id=t4r onchange='saveT(4)'><option value=127>每天</option><option value=31>工作日</option><option value=96>周末</option><option value=0>一次</option></select><div class='sw' id=t4e onclick='togT(4)'><i></i></div></div></div>"
"<div class=card><div class=lbl><span>倒计时</span><span class=val id=cdv>--</span></div>"
"<div class=row><input id=cdmin type=number min=1 max=1440 placeholder='分钟' style='flex:1;height:38px;background:#1b2233;border:1px solid #2b3752;border-radius:10px;color:#e8eefc;padding:0 8px;box-sizing:border-box'>"
"<button onclick='cd(1)' style='flex:1;height:38px;border:0;border-radius:10px;background:#1b2233;color:#b9c6e0'>定时开机</button>"
"<button onclick='cd(0)' style='flex:1;height:38px;border:0;border-radius:10px;background:#1b2233;color:#b9c6e0'>定时关机</button></div>"
"<button onclick='cdCancel()' style='width:100%;height:36px;margin-top:8px;border:0;border-radius:10px;background:#333c52;color:#b9c6e0'>取消倒计时</button></div>"
"<div class=card><div class=lbl><span>OTA 固件升级</span></div>"
"<input id=fw type=file accept='.bin,.ota' style='width:100%;color:#e8eefc;margin-bottom:10px'>"
"<button id=ob onclick='doOtaUp()' style='width:100%;height:44px;border:0;border-radius:12px;background:linear-gradient(135deg,#f6a623,#f56c6c);color:#fff;font-size:15px'>上传并升级</button>"
"<div id=oprog style='color:#8aa0c8;font-size:12px;margin-top:8px'>选择 .ota.bin 文件后点击上传; 成功后设备自动重启</div></div>"
"<div class=card><div class=stat>"
"<div><div class=n id=rp>0</div><div class=t>转速 RPM</div></div>"
"<div><div class=n id=fl>0</div><div class=t>滤芯(h)</div></div>"
"<div><div class=n id=fr>0%</div><div class=t>滤芯剩余</div></div></div></div>"
"<div style='text-align:center;color:#5a6b8c;font-size:12px' id=ver></div>"
"</div><script>"
"var D={};var LIFE=2000;"
"function api(u){return fetch(u).then(function(r){return r.json();});}"
"function byId(i){return document.getElementById(i);}"
"function pad(n){return ('0'+n).slice(-2);}"
"function show(d){D=d;"
"var p=byId('pw');p.className='power'+(d.power?' on':'');p.textContent=d.power?'运行中 · 点击关机':'已关机 · 点击开机';"
"byId('sv').textContent=d.speed+'%';byId('sp').value=d.speed;"
"var bs=byId('seg').getElementsByTagName('button');"
"for(var i=0;i<bs.length;i++){var m=parseInt(bs[i].getAttribute('data-m'),10);bs[i].className=(m==d.mode)?'active':'';}"
"byId('lsw').className='sw'+(d.led?' on':'');"
"byId('hue').value=d.led_hue;byId('hv').textContent=d.led_hue;"
"byId('br').value=d.led_bright;byId('bv').textContent=d.led_bright;"
"byId('lm1').style.background=(d.led_mode==1)?'#4da3ff':'#1b2233';byId('lm2').style.background=(d.led_mode==2)?'#4da3ff':'#1b2233';"
"byId('lm3').style.background=(d.led_mode==3)?'#4da3ff':'#1b2233';byId('lm4').style.background=(d.led_mode==4)?'#4da3ff':'#1b2233';"
"byId('lm5').style.background=(d.led_mode==5)?'#4da3ff':'#1b2233';byId('lm6').style.background=(d.led_mode==6)?'#4da3ff':'#1b2233';"
"byId('nsw').className='sw'+(d.night_en?' on':'');"
"if(d.night_on>=0)byId('nt_on').value=('0'+Math.floor(d.night_on/60)).slice(-2)+':'+('0'+(d.night_on%60)).slice(-2);"
"if(d.night_off>=0)byId('nt_off').value=('0'+Math.floor(d.night_off/60)).slice(-2)+':'+('0'+(d.night_off%60)).slice(-2);"
"byId('rp').textContent=d.rpm;byId('fl').textContent=Math.floor(d.filter/3600);"
"var fh=d.filter/3600;byId('fr').textContent=Math.max(0,Math.round((LIFE-fh)/LIFE*100))+'%';"
"if(d.cd_sec>0){var cm=Math.floor(d.cd_sec/60),cs=d.cd_sec%60;byId('cdv').textContent=(d.cd_action?'开机':'关机')+' '+pad(cm)+':'+pad(cs);}else{byId('cdv').textContent='--';}"
"for(var i=0;i<5;i++){var x=(d.task&&d.task[i])?d.task[i]:{h:0,m:0,r:127,a:1,on:0};var et=byId('t'+i+'t');if(document.activeElement!=et)et.value=pad(x.h)+':'+pad(x.m);var ea=byId('t'+i+'a');if(document.activeElement!=ea)ea.value=x.a;var er=byId('t'+i+'r');if(document.activeElement!=er)er.value=x.r;byId('t'+i+'e').className='sw'+(x.on?' on':'');}"
"byId('ver').textContent='固件 '+d.ver+'  '+d.mac;}"
"function g(){return api('/api/status').then(show);}"
"function tog(){api('/api/power?value='+(D.power?0:1)).then(show);}"
"function setS(v){byId('sv').textContent=v+'%';api('/api/speed?value='+v).then(show);}"
"function setM(m){api('/api/mode?value='+m).then(show);}"
"function togL(){api('/api/led?value='+(D.led?0:1)).then(show);}"
"function setLM(m){api('/api/led?mode='+m).then(show);}"
"function saveT(i){var tv=byId('t'+i+'t').value.split(':');if(tv.length<2)return;var a=parseInt(byId('t'+i+'a').value,10);var r=parseInt(byId('t'+i+'r').value,10);api('/api/task?i='+i+'&hour='+parseInt(tv[0],10)+'&minute='+parseInt(tv[1],10)+'&repeat='+r+'&action='+a).then(g);}"
"function togT(i){var e=byId('t'+i+'e');var on=(e.className.indexOf('on')>=0)?0:1;e.className='sw'+(on?' on':'');api('/api/task?i='+i+'&on='+on).then(g);}"
"function cd(a){var m=parseInt(byId('cdmin').value,10);if(!m||m<1){alert('请输入分钟数');return;}api('/api/countdown?sec='+(m*60)+'&action='+a).then(show);}"
"function cdCancel(){api('/api/countdown?sec=0').then(show);}"
"function togN(){api('/api/led?night='+(D.night_en?0:1)).then(show);}"
"function saveN(){var a=byId('nt_on').value.split(':');var b=byId('nt_off').value.split(':');if(a.length<2||b.length<2){alert('请选择开/关时间');return;}api('/api/led?n_on='+(parseInt(a[0],10)*60+parseInt(a[1],10))+'&n_off='+(parseInt(b[0],10)*60+parseInt(b[1],10))).then(show);alert('定时已保存');}"
"function setH(v){byId('hv').textContent=v;api('/api/led?mode=2&hue='+v).then(show);}"
"function setB(v){byId('bv').textContent=v;api('/api/led?bright='+v).then(show);}"
"function doOtaUp(){var f=byId('fw').files[0];if(!f){alert('请先选择 .ota.bin 文件');return;}if(!confirm('确认升级到 '+f.name+' ('+f.size+' 字节)?\\n升级期间请勿断电, 完成后设备会自动重启'))return;var P=byId('oprog');P.textContent='1/3 上传中 0%';var x=new XMLHttpRequest();x.open('POST','/api/ota_upload?ts='+Date.now(),true);x.setRequestHeader('Content-Type','application/octet-stream');x.upload.onprogress=function(e){if(e.lengthComputable)P.textContent='1/3 上传中 '+Math.round(e.loaded/e.total*100)+'%';};x.onload=function(){P.textContent='2/3 上传完成, 设备写入flash+校验中…';waitOta();};x.onerror=function(){P.textContent='2/3 上传连接结束, 设备可能正在写入并重启…';waitOta();};x.send(f);}"
"function waitOta(){var n=0,P=byId('oprog');var t=setInterval(function(){n++;fetch('/api/status?ts='+Date.now()).then(function(r){return r.json();}).then(function(d){clearInterval(t);P.textContent='3/3 升级成功! 设备已重启, 当前版本 '+d.ver;}).catch(function(){P.textContent='3/3 等待设备重启…已 '+(n*2)+' 秒';});if(n>45){clearInterval(t);P.textContent='3/3 超时(90秒): 刷新页面看版本号, 变新即成功';}},2000);}"
"g();setInterval(g,3000);"
"</script></body></html>";

static int h_root(httpd_request_t *req)
{
    char *ssid = cfg_ssid();
    if (strlen(ssid) == 0 || softap_mode)
        return send_html(req, page_setup);
    return send_html(req, page_ctrl);
}


static int h_diag_pwm(httpd_request_t *req)
{
    int ch = get_int_tag(req, "ch", -1);
    int duty = get_int_tag(req, "duty", -1);
    int freq = get_int_tag(req, "freq", 25000);
    char js[128];
    if (ch < 0 || ch > 5 || duty < 0) return send_json(req, "{\"ok\":0}");
    MicoPwmInitialize((mico_pwm_t)ch, freq, (float)duty);
    MicoPwmStart((mico_pwm_t)ch);
    snprintf(js, sizeof(js), "{\"ok\":1,\"ch\":%d,\"duty\":%d,\"freq\":%d}", ch, duty, freq);
    return send_json(req, js);
}
static int h_diag_pwmstop(httpd_request_t *req)
{
    int ch = get_int_tag(req, "ch", -1);
    if (ch >= 0 && ch <= 5) MicoPwmStop((mico_pwm_t)ch);
    return send_json(req, "{\"ok\":1}");
}
static int h_diag_gpio(httpd_request_t *req)
{
    int pin = get_int_tag(req, "pin", -1);
    int val = get_int_tag(req, "val", -1);
    char js[96];
    if (pin < 0 || pin > 12) return send_json(req, "{\"ok\":0,\"err\":\"pin\"}");
    MicoGpioInitialize((mico_gpio_t)pin, (val == 2) ? INPUT_PULL_UP : OUTPUT_PUSH_PULL);
    if (val == 1) MicoGpioOutputHigh((mico_gpio_t)pin);
    else if (val == 0) MicoGpioOutputLow((mico_gpio_t)pin);
    snprintf(js, sizeof(js), "{\"ok\":1,\"pin\":%d,\"val\":%d}", pin, val);
    return send_json(req, js);
}

/* ---------------- 灯环 R/G/B 引脚诊断 ---------------- */
static volatile int g_led_sweep = 0;
static void led_sweep_thread(void *arg)
{
    (void)arg;
    g_led_sweep = 1;
    for (int a = 0; a < MICO_GPIO_MAX; a++) {
        MicoGpioInitialize((mico_gpio_t)a, OUTPUT_PUSH_PULL);
        MicoGpioOutputHigh((mico_gpio_t)a);
        A1_LOG("LEDSWEEP alias=%d ON", a);
        mico_thread_msleep(1200);
        MicoGpioOutputLow((mico_gpio_t)a);
        mico_thread_msleep(300);
    }
    g_led_sweep = 0;
    A1_LOG("LEDSWEEP done");
    led_hw_init();
    mico_rtos_delete_thread(NULL);
}
static int h_diag_ledpin(httpd_request_t *req)
{
    int pin = get_int_tag(req, "pin", -1);
    int val = get_int_tag(req, "val", -1);
    if (pin < 0 || pin >= MICO_GPIO_MAX) return send_json(req, "{\"ok\":0,\"err\":\"pin\"}");
    MicoGpioInitialize((mico_gpio_t)pin, OUTPUT_PUSH_PULL);
    if (val == 1) MicoGpioOutputHigh((mico_gpio_t)pin);
    else if (val == 0) MicoGpioOutputLow((mico_gpio_t)pin);
    return send_json(req, "{\"ok\":1}");
}
static int h_diag_ledset(httpd_request_t *req)
{
    int r = get_int_tag(req, "r", -1), g = get_int_tag(req, "g", -1), b = get_int_tag(req, "b", -1);
    int ws = get_int_tag(req, "ws", -1), ty = get_int_tag(req, "type", -1);
    mico_rtos_lock_mutex(&g_led_mux);
    if (r >= 0 && r < MICO_GPIO_MAX) g_led_r = r;
    if (g >= 0 && g < MICO_GPIO_MAX) g_led_g = g;
    if (b >= 0 && b < MICO_GPIO_MAX) g_led_b = b;
    if (ws >= 0 && ws < MICO_GPIO_MAX) g_led_ws_pin = ws;
    if (ty >= 0 && ty <= 2) g_led_type = ty;
    led_hw_init();
    mico_rtos_unlock_mutex(&g_led_mux);
    led_ring_refresh();
    char js[128]; snprintf(js, sizeof(js),
        "{\"ok\":1,\"type\":%d,\"r\":%d,\"g\":%d,\"b\":%d,\"ws\":%d}",
        g_led_type, g_led_r, g_led_g, g_led_b, g_led_ws_pin);
    return send_json(req, js);
}
static int h_diag_ledsweep(httpd_request_t *req)
{
    if (!g_led_sweep)
        mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "ledsweep",
                                (mico_thread_function_t)led_sweep_thread, 0x600, 0);
    return send_json(req, "{\"ok\":1,\"note\":\"watch ring+serial; alias 0..12 each 1.2s\"}");
}

/* WS2812 位时序标定: 递增 scale, 每档把前 8 颗点亮为红; 看哪档能点亮 */
static volatile int g_ws_sweep = 0;
static void ws_sweep_thread(void *arg)
{
    (void)arg;
    g_ws_sweep = 1;
    static const int scales[] = {2,4,6,8,10,12,16,20,26,32,40,50,64,80};
    for (int i = 0; i < (int)(sizeof(scales)/sizeof(scales[0])); i++) {
        g_ws_scale = scales[i];
        A1_LOG("WSSWEEP scale=%d", g_ws_scale);
        mico_rtos_lock_mutex(&g_led_mux);
        mico_gpio_t p = (mico_gpio_t)g_led_ws_pin;
        MicoGpioInitialize(p, OUTPUT_PUSH_PULL);
        MicoGpioOutputLow(p);
        led_nop(9000);
        for (int k = 0; k < LED_WS_NUM; k++) {
            int on = (k < 8);
            ws_send_byte(0); ws_send_byte(on ? 255 : 0); ws_send_byte(0);  /* GRB -> 红 */
        }
        MicoGpioOutputLow(p);
        led_nop(9000);
        mico_rtos_unlock_mutex(&g_led_mux);
        mico_thread_msleep(1200);
    }
    g_ws_sweep = 0;
    A1_LOG("WSSWEEP done");
    mico_rtos_delete_thread(NULL);
}
static int h_diag_wssweep(httpd_request_t *req)
{
    if (!g_ws_sweep)
        mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "wssweep",
                                (mico_thread_function_t)ws_sweep_thread, 0x600, 0);
    return send_json(req, "{\"ok\":1,\"note\":\"看灯环: 哪一档前几颗亮红, 记下该档 scale(串口也有打印)\"}");
}
static int h_diag_wsset(httpd_request_t *req)
{
    int s = get_int_tag(req, "scale", -1);
    if (s > 0) g_ws_scale = s;
    char js[64]; snprintf(js, sizeof(js), "{\"ok\":1,\"scale\":%d}", g_ws_scale);
    return send_json(req, js);
}

/* 一键自动测试: 遍历所有可能的灯环控制方式, 每步打印 AUTO STEP n, 观察灯环哪步变化 */
static volatile int g_auto_run = 0;
static void led_auto_thread(void *arg)
{
    (void)arg;
    g_auto_run = 1;
    static const int pins[] = {0,1,2,3,4,5,6,7,8,9,10,11,12};
    static const char *pname[] = {"GPIO1","GPIO2","GPIO7","GPIO8","GPIO9","GPIO10",
                                  "GPIO12","GPIO13","GPIO14","GPIO19","GPIO21","GPIO22","GPIO23"};
    int idx = 0, n = (int)(sizeof(pins)/sizeof(pins[0]));
    A1_LOG("AUTO start: watch the ring, note the STEP number when it changes");
    /* STEP 0: 全部候选脚拉低 */
    for (int i = 0; i < n; i++) { MicoGpioInitialize((mico_gpio_t)pins[i], OUTPUT_PUSH_PULL); MicoGpioOutputLow((mico_gpio_t)pins[i]); }
    A1_LOG("AUTO STEP 0: all candidate pins LOW"); mico_thread_msleep(2000);
    /* 每个脚单独拉高再拉低 */
    for (int i = 0; i < n; i++) {
        idx++;
        A1_LOG("AUTO STEP %d: alias %d (%s) HIGH", idx, pins[i], pname[i]);
        MicoGpioInitialize((mico_gpio_t)pins[i], OUTPUT_PUSH_PULL);
        MicoGpioOutputHigh((mico_gpio_t)pins[i]);
        mico_thread_msleep(1400);
        MicoGpioOutputLow((mico_gpio_t)pins[i]);
        mico_thread_msleep(300);
    }
    /* 3 路组合: 假设 9/10/11 = R/G/B */
    { static const int cb[6][3] = {{1,0,0},{0,1,0},{0,0,1},{1,1,0},{0,1,1},{1,1,1}};
      static const char *nm[6] = {"R(9)","G(10)","B(11)","R+G","G+B","R+G+B"};
      for (int c = 0; c < 6; c++) {
          idx++;
          A1_LOG("AUTO STEP %d: 3ch %s on pins 9/10/11", idx, nm[c]);
          MicoGpioInitialize((mico_gpio_t)9, OUTPUT_PUSH_PULL);
          MicoGpioInitialize((mico_gpio_t)10, OUTPUT_PUSH_PULL);
          MicoGpioInitialize((mico_gpio_t)11, OUTPUT_PUSH_PULL);
          if (cb[c][0]) MicoGpioOutputHigh((mico_gpio_t)9); else MicoGpioOutputLow((mico_gpio_t)9);
          if (cb[c][1]) MicoGpioOutputHigh((mico_gpio_t)10); else MicoGpioOutputLow((mico_gpio_t)10);
          if (cb[c][2]) MicoGpioOutputHigh((mico_gpio_t)11); else MicoGpioOutputLow((mico_gpio_t)11);
          mico_thread_msleep(1500);
      }
      MicoGpioOutputLow((mico_gpio_t)9); MicoGpioOutputLow((mico_gpio_t)10); MicoGpioOutputLow((mico_gpio_t)11);
    }
    /* WS2812: pin7, 几种时序 */
    g_led_type = 2; g_led_ws_pin = 7; led_hw_init();
    { static const int sc[] = {6,10,20,40};
      for (int s = 0; s < 4; s++) {
          idx++;
          g_ws_scale = sc[s];
          A1_LOG("AUTO STEP %d: WS2812 red scale=%d on pin7", idx, g_ws_scale);
          ws2812_flush(255, 0, 0);
          mico_thread_msleep(2000);
          ws2812_flush(0, 0, 0);
      }
    }
    /* 快速 WS2812(直接写 0x40001000 bit13) */
    { static const int sc[] = {4,8,12,20,30};
      for (int s = 0; s < 5; s++) {
          idx++;
          g_ws2_scale = sc[s];
          A1_LOG("AUTO STEP %d: WS2812-FAST(reg bit13) red scale=%d", idx, g_ws2_scale);
          ws2_flush(255, 0, 0);
          mico_thread_msleep(2000);
          ws2_flush(0, 0, 0);
      }
    }
    /* WS2812 via GPIO API, 关中断 */
    { static const int sc[] = {4,8,12,16,24,32};
      for (int s = 0; s < 6; s++) {
          idx++;
          g_ws_scale = sc[s];
          A1_LOG("AUTO STEP %d: WS2812-API(irq-off) red scale=%d", idx, g_ws_scale);
          ws_api_flush(255, 0, 0);
          mico_thread_msleep(2000);
          ws_api_flush(0, 0, 0);
      }
    }
    A1_LOG("AUTO DONE: report which STEPs changed the ring");
    g_auto_run = 0;
    mico_rtos_delete_thread(NULL);
}
static int h_diag_autotest(httpd_request_t *req)
{
    if (!g_auto_run)
        mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "autotest",
                                (mico_thread_function_t)led_auto_thread, 0x800, 0);
    return send_json(req, "{\"ok\":1,\"note\":\"看灯环+串口AUTO STEP n, 哪步变化记下来\"}");
}



/* ---------------- PM2.5 (I2C) ---------------- */
static mico_i2c_device_t g_i2cdev;
static mico_mutex_t      g_i2c_mux;
static bool i2c_setup(int addr7);

/* ---------------- 面板 (I2C 0x08) ---------------- */
#define PANEL_I2C_ADDR 0x08
static bool panel_read(uint8_t *rx)
{
    bool ok = false;
    mico_rtos_lock_mutex(&g_i2c_mux);
    if (i2c_setup(PANEL_I2C_ADDR)) {
        uint8_t tx[1] = {0x00}; mico_i2c_message_t m;
        OSStatus e = MicoI2cBuildCombinedMessage(&m, tx, rx, 1, 4, 2);
        if (e == kNoErr) e = MicoI2cTransfer(&g_i2cdev, &m, 1);
        MicoI2cFinalize(&g_i2cdev);
        ok = (e == kNoErr);
    }
    mico_rtos_unlock_mutex(&g_i2c_mux);
    return ok;
}

/* ---------------- 诊断: I2C / GPIO 读 ---------------- */
static bool i2c_setup(int addr7)
{
    g_i2cdev.port = MICO_I2C_1;
    g_i2cdev.address = (uint16_t)addr7;
    g_i2cdev.address_width = I2C_ADDRESS_WIDTH_7BIT;
    g_i2cdev.speed_mode = I2C_STANDARD_SPEED_MODE;
    return (MicoI2cInitialize(&g_i2cdev) == kNoErr);
}
static int h_diag_i2cscan(httpd_request_t *req)
{
    char *js = malloc(512); if (!js) return send_json(req, "{\"found\":[]}");
    mico_rtos_lock_mutex(&g_i2c_mux);
    int n = sprintf(js, "{\"found\":[");
    int first = 1;
    for (int a = 0x08; a <= 0x77; a++) {
        if (i2c_setup(a)) {
            uint8_t tx[1] = {0}; mico_i2c_message_t m;
            if (MicoI2cBuildTxMessage(&m, tx, 1, 2) == kNoErr &&
                MicoI2cTransfer(&g_i2cdev, &m, 1) == kNoErr) {
                n += sprintf(js + n, "%s\"0x%02X\"", first ? "" : ",", a); first = 0;
            }
            MicoI2cFinalize(&g_i2cdev);
        }
    }
    sprintf(js + n, "]}");
    mico_rtos_unlock_mutex(&g_i2c_mux);
    int r = send_json(req, js); free(js); return r;
}
static int h_diag_i2c(httpd_request_t *req)
{
    int addr = get_int_tag(req, "addr", -1);
    int reg  = get_int_tag(req, "reg", -1);
    int cnt  = get_int_tag(req, "n", 1);
    char js[160];
    if (addr < 0) return send_json(req, "{\"ok\":0,\"err\":\"addr\"}");
    if (cnt < 1 || cnt > 16) cnt = 1;
    mico_rtos_lock_mutex(&g_i2c_mux);
    if (!i2c_setup(addr)) { mico_rtos_unlock_mutex(&g_i2c_mux); return send_json(req, "{\"ok\":0,\"err\":\"init\"}"); }
    uint8_t rx[16] = {0}; mico_i2c_message_t m; OSStatus err;
    if (reg >= 0) { uint8_t tx[1] = {(uint8_t)reg};
        err = MicoI2cBuildCombinedMessage(&m, tx, rx, 1, cnt, 2); }
    else err = MicoI2cBuildRxMessage(&m, rx, cnt, 2);
    if (err == kNoErr) err = MicoI2cTransfer(&g_i2cdev, &m, 1);
    MicoI2cFinalize(&g_i2cdev);
    A1_LOG("diag i2c addr=0x%02X reg=%d n=%d -> %d", addr, reg, cnt, err);
    mico_rtos_unlock_mutex(&g_i2c_mux);
    int k = sprintf(js, "{\"ok\":%d,\"data\":[", err == kNoErr ? 1 : 0);
    for (int i = 0; i < cnt; i++) k += sprintf(js + k, "%s%d", i ? "," : "", rx[i]);
    sprintf(js + k, "]}");
    return send_json(req, js);
}
static int h_diag_gpioread(httpd_request_t *req)
{
    int pin = get_int_tag(req, "pin", -1);
    char js[64];
    if (pin < 0 || pin > 12) return send_json(req, "{\"ok\":0}");
    MicoGpioInitialize((mico_gpio_t)pin, INPUT_PULL_UP);
    snprintf(js, sizeof(js), "{\"pin\":%d,\"val\":%d}", pin, MicoGpioInputGet((mico_gpio_t)pin));
    return send_json(req, js);
}


static int h_mqtt(httpd_request_t *req)
{
    char b[80];
    if (httpd_get_tag_from_url(req,"host",b,sizeof(b)-1)==kNoErr) strncpy(cfg->mqtt_host,b,sizeof(cfg->mqtt_host)-1);
    if (httpd_get_tag_from_url(req,"user",b,sizeof(b)-1)==kNoErr) strncpy(cfg->mqtt_user,b,sizeof(cfg->mqtt_user)-1);
    if (httpd_get_tag_from_url(req,"pass",b,sizeof(b)-1)==kNoErr) strncpy(cfg->mqtt_pass,b,sizeof(cfg->mqtt_pass)-1);
    if (httpd_get_tag_from_url(req,"port",b,sizeof(b)-1)==kNoErr) cfg->mqtt_port = atoi(b);
    mico_system_context_update(sys_context);
    return h_status(req);
}
static void ota_status_cb(OTA_STATE_E state, float progress){ A1_LOG("OTA state=%d %.1f%%", (int)state, progress); }
static int h_ota(httpd_request_t *req)
{
    char url[200] = {0};
    get_str_tag(req, "url", url, sizeof(url));
    if (url[0] == 0) return send_json(req, "{\"ok\":0,\"err\":\"no url\"}");
    send_json(req, "{\"ok\":1}");
    mico_thread_msleep(200);
    A1_LOG("OTA start: %s", url);
    ota_server_start(url, NULL, ota_status_cb);
    return kNoErr;
}

/* 网页上传本地 .ota.bin: POST /api/ota_upload  (请求体 = 原始固件字节流)
 * 解析 Content-Length -> 擦除 OTA 分区 -> 流式写入 -> 写升级表 -> 自动重启 */
static int h_ota_upload(httpd_request_t *req)
{
    if (req->type != HTTPD_REQ_TYPE_POST)
        return send_json(req, "{\"ok\":0,\"err\":\"post only\"}");

    /* 先解析 HTTP 头, 拿到 Content-Length (body_nbytes/remaining_bytes) */
    char *hdr = malloc(HTTPD_MAX_MESSAGE);
    if (!hdr) return send_json(req, "{\"ok\":0,\"err\":\"mem\"}");
    OSStatus herr = httpd_parse_hdr_tags(req, req->sock, hdr, HTTPD_MAX_MESSAGE);
    free(hdr);
    if (herr != kNoErr) {
        A1_LOG("OTA up: header parse err %d", herr);
        return send_json(req, "{\"ok\":0,\"err\":\"hdr\"}");
    }

    mico_logic_partition_t *part = MicoFlashGetInfo(MICO_PARTITION_OTA_TEMP);
    int total = req->remaining_bytes;
    A1_LOG("OTA up: %d bytes, partition=%d", total, (int)part->partition_length);
    if (total <= 0 || total > (int)part->partition_length)
        return send_json(req, "{\"ok\":0,\"err\":\"size\"}");

    MicoFlashErase(MICO_PARTITION_OTA_TEMP, 0, part->partition_length);

    uint32_t offset = 0;
    int left = total;
    CRC16_Context cc; CRC16_Init(&cc);
    uint8_t *buf = malloc(1024);
    if (!buf) return send_json(req, "{\"ok\":0,\"err\":\"mem2\"}");
    while (left > 0) {
        int want = (left > 1024) ? 1024 : left;
        int r = httpd_recv(req->sock, buf, want, 0);
        if (r <= 0) { A1_LOG("OTA up: recv %d (left=%d)", r, left); break; }
        MicoFlashWrite(MICO_PARTITION_OTA_TEMP, &offset, buf, r);
        CRC16_Update(&cc, buf, r);
        left -= r;
    }
    free(buf);

    if (left != 0) {
        A1_LOG("OTA up: INCOMPLETE left=%d", left);
        return send_json(req, "{\"ok\":0,\"err\":\"incomplete\"}");
    }
    uint16_t crc = 0; CRC16_Final(&cc, &crc);
    A1_LOG("OTA up: OK %lu bytes crc=%04x, rebooting", (unsigned long)offset, crc);
    send_json(req, "{\"ok\":1,\"bytes\":1,\"reboot\":1}");
    mico_thread_msleep(800);                 /* 让响应先发出去 */
    mico_ota_switch_to_new_fw((int)offset, crc);
    mico_thread_msleep(300);
    mico_system_power_perform(sys_context, eState_Software_Reset);
    return kNoErr;
}
static int h_task(httpd_request_t *req)
{
    int i = get_int_tag(req, "i", -1);
    if (i < 0 || i > 4) return send_json(req, "{\"ok\":0}");
    int v;
    if ((v=get_int_tag(req,"hour",-1))>=0) cfg->task[i].hour=v;
    if ((v=get_int_tag(req,"minute",-1))>=0) cfg->task[i].minute=v;
    if ((v=get_int_tag(req,"repeat",-1))>=0) cfg->task[i].repeat=v;
    if ((v=get_int_tag(req,"action",-1))>=0) cfg->task[i].action=v;
    if ((v=get_int_tag(req,"on",-1))>=0) cfg->task[i].on=v?1:0;
    mico_system_context_update(sys_context);
    char js[128];
    snprintf(js,sizeof(js),"{\"ok\":1,\"i\":%d,\"h\":%d,\"m\":%d,\"rpt\":%d,\"act\":%d,\"on\":%d}",
             i,cfg->task[i].hour,cfg->task[i].minute,cfg->task[i].repeat,cfg->task[i].action,cfg->task[i].on);
    return send_json(req, js);
}

static int h_countdown(httpd_request_t *req)
{
    int sec = get_int_tag(req, "sec", -1);
    int act = get_int_tag(req, "action", -1);
    if (sec >= 0) {
        g_cd_sec = sec;
        if (act == 0 || act == 1) g_cd_action = act;
        A1_LOG("COUNTDOWN set %d sec, action=%d", (int)g_cd_sec, g_cd_action);
    }
    return h_status(req);
}

static int h_favicon(httpd_request_t *req){ httpd_send_all_header(req, HTTP_RES_200, 0, "image/x-icon"); return kNoErr; }
static struct httpd_wsgi_call handlers[] = {
    {"/",                 HDRS, 0, h_root,        NULL, NULL, NULL},
    {"/favicon.ico",      HDRS, 0, h_favicon,     NULL, NULL, NULL},
    {"/api/status",       HDRS, 0, h_status,      NULL, NULL, NULL},
    {"/api/power",        HDRS, 0, h_power,       NULL, NULL, NULL},
    {"/api/speed",        HDRS, 0, h_speed,       NULL, NULL, NULL},
    {"/api/mode",         HDRS, 0, h_mode,        NULL, NULL, NULL},
    {"/api/led",          HDRS, 0, h_led,         NULL, NULL, NULL},
    {"/api/lock",         HDRS, 0, h_lock,        NULL, NULL, NULL},
    {"/api/filter/reset", HDRS, 0, h_filter_reset, NULL, NULL, NULL},
    {"/api/reboot",       HDRS, 0, h_reboot,      NULL, NULL, NULL},
    {"/api/factory",      HDRS, 0, h_factory,     NULL, NULL, NULL},
    {"/api/wifi/scan",    HDRS, 0, h_wifi_scan,   NULL, NULL, NULL},
    {"/api/wifi/status",  HDRS, 0, h_wifi_status, NULL, NULL, NULL},
    {"/api/wifi/set",     HDRS, 0, h_wifi_set,    NULL, NULL, NULL},
    {"/api/mqtt",         HDRS, 0, h_mqtt,        NULL, NULL, NULL},
    {"/api/ota",          HDRS, 0, h_ota,         NULL, NULL, NULL},
    {"/api/ota_upload",   HDRS, 0, NULL,          h_ota_upload, NULL, NULL},
    {"/api/task",         HDRS, 0, h_task,        NULL, NULL, NULL},
    {"/api/countdown",    HDRS, 0, h_countdown,   NULL, NULL, NULL},
    {"/api/diag/pwm",     HDRS, 0, h_diag_pwm,    NULL, NULL, NULL},
    {"/api/diag/pwmstop", HDRS, 0, h_diag_pwmstop,NULL, NULL, NULL},
    {"/api/diag/gpio",    HDRS, 0, h_diag_gpio,   NULL, NULL, NULL},
    {"/api/diag/gpioread",HDRS, 0, h_diag_gpioread,NULL, NULL, NULL},
    {"/api/diag/ledpin",  HDRS, 0, h_diag_ledpin,  NULL, NULL, NULL},
    {"/api/diag/ledset",  HDRS, 0, h_diag_ledset,  NULL, NULL, NULL},
    {"/api/diag/ledsweep",HDRS, 0, h_diag_ledsweep,NULL, NULL, NULL},
    {"/api/diag/wssweep", HDRS, 0, h_diag_wssweep, NULL, NULL, NULL},
    {"/api/diag/wsset",   HDRS, 0, h_diag_wsset,   NULL, NULL, NULL},
    {"/api/diag/autotest",HDRS, 0, h_diag_autotest,NULL, NULL, NULL},
    {"/api/diag/i2cscan", HDRS, 0, h_diag_i2cscan, NULL, NULL, NULL},
    {"/api/diag/i2c",     HDRS, 0, h_diag_i2c,     NULL, NULL, NULL},
};

static void a1_web_start(void)
{
    httpd_init();
    httpd_start();
    httpd_register_wsgi_handlers(handlers, sizeof(handlers) / sizeof(handlers[0]));
    A1_LOG("httpd started");
}


/* ================= 定时对时 + 定时任务 ================= */
static volatile bool time_synced = false;
static void sntp_synced_cb(void){ time_synced = true; A1_LOG("sntp time synced"); }

static void task_apply(uint8_t action){
    if (action == 0) { cfg->power = 0; }
    else { cfg->power = 1; if (cfg->speed < 1) cfg->speed = 25; cfg->mode = 0; }
    A1_LOG("TIMER action=%d", action);
    a1_apply(); cfg_dirty = 1;
}
static void tasks_check(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return;
    int day = (tm->tm_wday + 6) % 7;   /* Mon=0 .. Sun=6 */
    for (int i = 0; i < 5; i++) {
        if (!cfg->task[i].on) continue;
        if (cfg->task[i].hour != tm->tm_hour || cfg->task[i].minute != tm->tm_min) continue;
        if (cfg->task[i].repeat == 0) { task_apply(cfg->task[i].action); cfg->task[i].on = 0; cfg_dirty = 1; }
        else if (cfg->task[i].repeat & (1 << day)) { task_apply(cfg->task[i].action); }
    }
}

/* 灯环夜灯定时: 到点在 [on, off) 区间内点亮, 之外熄灭 (支持跨午夜) */
static void night_check(void)
{
    if (!cfg->night_en || cfg->night_on < 0 || cfg->night_off < 0) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return;
    int now = tm->tm_hour * 60 + tm->tm_min;
    int on = cfg->night_on, off = cfg->night_off, want;
    if (on == off) return;
    else if (on < off) want = (now >= on && now < off);
    else               want = (now >= on || now < off);
    if (want != cfg->led) {
        cfg->led = want ? 1 : 0;
        A1_LOG("NIGHT led -> %d", cfg->led);
        cfg_dirty = 1; a1_apply();
    }
}

/* ------------------- 后台线程 ------------------- */
static void a1_tick_thread(void *arg)
{
    (void)arg;
    uint32_t last = 0; int bprev = 1, hold = 0; int sec = 0; int panel_key_prev = 0;
    while (1) {
        mico_thread_msleep(50);
        int b = MicoGpioInputGet(A1_BUTTON_GPIO);
        if (b == 0 && bprev == 1) hold = 0;      /* 按下开始 */
        if (b == 0) hold++;
        if (b == 1 && bprev == 0) {              /* 松开 */
            if (hold > 0 && hold < 100) {        /* 短按 <5s: 开关 */
                cfg->power = !cfg->power;
                A1_LOG("key pressed -> power=%d", cfg->power);
                a1_apply(); cfg_dirty = 1;
            }
            hold = 0;
        }
        if (b == 0 && hold == 120) {             /* 长按 6s: 恢复出厂 */
            A1_LOG("key long press -> factory reset");
            mico_system_context_restore(sys_context);
            hold = 0;
        }
        bprev = b;
        /* 每 2 个 50ms (100ms) 读一次面板, 滑条更跟手 */
        static int pdiv = 0;
        static int panel_warm = 0;   /* 上电忽略前 1.5s，防止抖动误触发 */
        if (panel_warm < 30) panel_warm++;
        if (++pdiv >= 2) {
            pdiv = 0;
            uint8_t rx[4] = {0};
            if (panel_warm >= 30 && panel_read(rx)) {
                /* 诊断: 面板 4 字节一有变化就打印(用于找"WiFi键") */
                static uint8_t prev_rx[4] = {0,0,0,0};
                if (memcmp(prev_rx, rx, 4) != 0) {
                    A1_LOG("PANEL rx=%02X %02X %02X %02X", rx[0], rx[1], rx[2], rx[3]);
                    memcpy(prev_rx, rx, 4);
                }
                int key = rx[1] & 0x01;
                if (key && !panel_key_prev) {                   /* 短按: 开/关机 */
                    cfg->power = !cfg->power;
                    A1_LOG("PANEL power key -> power=%d", cfg->power);
                    a1_apply(); cfg_dirty = 1;
                }
                panel_key_prev = key;
                int sl = rx[0];
                if (sl != 0xFF && sl <= 100) {
                    if (cfg->speed != sl || !cfg->power) {
                        cfg->speed = sl; cfg->mode = 0; cfg->power = 1;
                        a1_apply_fan(); cfg_dirty = 1; g_panel_hold = 2;
                    }
                }
            }
        }
        if (++sec >= 20) {                       /* 每 1s */
            sec = 0;
            uint32_t p = fan_pulses;
            fan_rpm = (uint32_t)((uint64_t)(p - last) * 60 / A1_TACHO_PPR);
            last = p;
            if (g_cd_sec > 0) {                      /* 倒计时 */
                g_cd_sec--;
                if (g_cd_sec == 0) {
                    if (g_cd_action) { cfg->power = 1; if (cfg->speed < 1) cfg->speed = 25; }
                    else cfg->power = 0;
                    A1_LOG("COUNTDOWN done -> power=%d", cfg->power);
                    a1_apply(); cfg_dirty = 1;
                }
            }
            if (cfg->power) { cfg->total_seconds++; cfg->filter_seconds++; }
            if (g_panel_hold > 0) g_panel_hold--;
            if (cfg_dirty && g_panel_hold == 0) cfg_save_now();   /* 延迟落盘: 滑动停止后再写 flash */
            { static int last_min = -1; time_t t=time(NULL); struct tm *tm=localtime(&t);
              if (time_synced && tm && tm->tm_min != last_min) { last_min = tm->tm_min; tasks_check(); night_check(); } }
        }
    }
}


/* ------------------- main ------------------- */
void appRestoreDefault_callback(void * const user_config_data, uint32_t size)
{
    (void)size;
    a1_config_t *u = (a1_config_t *)user_config_data;
    memset(u, 0, sizeof(*u));
    u->version = A1_CFG_VERSION;
    strncpy(u->name, "A1", sizeof(u->name) - 1);
    u->led = 0; u->led_mode = 1; u->led_hue = 200; u->led_bright = 80;
    u->night_en = 0; u->night_on = -1; u->night_off = -1;
    u->mqtt_port = 1883;
    u->pwm_ch = A1_FAN_PWM; u->fan_en_gpio = A1_FAN_ENABLE_GPIO;
    u->tacho_gpio = A1_FAN_TACHO_GPIO; u->led_pwm = A1_LED_PWM;
}

/* ---------------- 串口 CLI 测试命令: led ... ---------------- */
static void cli_led(CLI_ARGS)
{
    (void)xWriteBufferLen;
    if (argc < 2) { cmd_printf("led <on|off|type N|hue N|bright N|scale N|test|wssweep|status>\r\n"); return; }
    const char *c = argv[1];
    if (!strcmp(c, "on")) {
        cfg->led = 1; led_hw_init(); a1_apply(); cfg_dirty = 1;
        cmd_printf("led on (type=%d)\r\n", g_led_type);
    } else if (!strcmp(c, "off")) {
        cfg->led = 0; led_hw_init(); a1_apply(); cfg_dirty = 1;
        cmd_printf("led off\r\n");
    } else if (!strcmp(c, "type") && argc >= 3) {
        g_led_type = atoi(argv[2]); led_hw_init(); a1_apply();
        cmd_printf("type=%d\r\n", g_led_type);
    } else if (!strcmp(c, "hue") && argc >= 3) {
        cfg->led_hue = atoi(argv[2]); cfg->led_mode = 2; cfg->led = 1; led_hw_init(); a1_apply();
        cmd_printf("hue=%d\r\n", cfg->led_hue);
    } else if (!strcmp(c, "bright") && argc >= 3) {
        cfg->led_bright = atoi(argv[2]); cfg->led = 1; led_hw_init(); a1_apply();
        cmd_printf("bright=%d\r\n", cfg->led_bright);
    } else if (!strcmp(c, "scale") && argc >= 3) {
        g_ws_scale = atoi(argv[2]);
        cmd_printf("ws_scale=%d (再 led test 看是否变红)\r\n", g_ws_scale);
    } else if (!strcmp(c, "test")) {
        led_hw_init(); ws2812_flush(255, 0, 0);
        cmd_printf("sent r=255 g=0 b=0 (type=%d scale=%d)\r\n", g_led_type, g_ws_scale);
    } else if (!strcmp(c, "wssweep")) {
        if (!g_ws_sweep)
            mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "wssweep",
                                    (mico_thread_function_t)ws_sweep_thread, 0x600, 0);
        cmd_printf("wssweep started\r\n");
    } else if (!strcmp(c, "pin") && argc >= 4) {
        int pa = atoi(argv[2]), pv = atoi(argv[3]);
        if (pa >= 0 && pa < MICO_GPIO_MAX) {
            MicoGpioInitialize((mico_gpio_t)pa, OUTPUT_PUSH_PULL);
            if (pv) MicoGpioOutputHigh((mico_gpio_t)pa);
            else    MicoGpioOutputLow((mico_gpio_t)pa);
            cmd_printf("pin alias=%d -> %d\r\n", pa, pv ? 1 : 0);
        } else cmd_printf("bad pin (0..%d)\r\n", MICO_GPIO_MAX - 1);
    } else if (!strcmp(c, "psw")) {
        if (!g_led_sweep)
            mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "ledsweep",
                                    (mico_thread_function_t)led_sweep_thread, 0x600, 0);
        cmd_printf("pin sweep alias 0..%d (each 1.2s, watch ring)\r\n", MICO_GPIO_MAX - 1);
    } else if (!strcmp(c, "ws2")) {
        if (argc >= 3) g_ws2_scale = atoi(argv[2]);
        if (argc >= 4) g_ws2_bit = atoi(argv[3]);
        ws2_flush(255, 0, 0);
        cmd_printf("ws2 red sent (fast reg), scale=%d bit=%d\r\n", g_ws2_scale, g_ws2_bit);
    } else if (!strcmp(c, "z") && argc >= 6) {
        g_z_h1 = atoi(argv[2]); g_z_l1 = atoi(argv[3]); g_z_h0 = atoi(argv[4]); g_z_l0 = atoi(argv[5]);
        ws2_flush(255, 0, 0);
        cmd_printf("z: 1=高%d低%d  0=高%d低%d (发红帧)\r\n", g_z_h1, g_z_l1, g_z_h0, g_z_l0);
    } else if (!strcmp(c, "dummy") && argc >= 3) {
        g_ws_dummy = atoi(argv[2]) ? 1 : 0;
        ws2_flush(255, 0, 0);
        cmd_printf("dummy=%d (发红帧看看)\r\n", g_ws_dummy);
    } else if (!strcmp(c, "count") && argc >= 3) {
        g_ws_num = atoi(argv[2]);
        cmd_printf("led count = %d\r\n", g_ws_num);
    } else if (!strcmp(c, "walk")) {
        int n = g_ws_num; if (n < 1) n = 1; if (n > 96) n = 96;
        for (int i = 0; i <= n; i++) {              /* 走到 n, 看第0颗和第n颗是否重合 */
            for (int k = 0; k < n; k++) { g_ws_frame[k*3]=0; g_ws_frame[k*3+1]=0; g_ws_frame[k*3+2]=0; }
            if (i < n) { g_ws_frame[i*3]=0; g_ws_frame[i*3+1]=255; g_ws_frame[i*3+2]=0; } /* 红 */
            ws2_raw(g_ws_frame, n);
            cmd_printf("walk LED=%d\r\n", i);
            mico_thread_msleep(1000);   /* 1 秒一颗, 便于数 */
        }
        ws2_flush(0, 0, 0);
        cmd_printf("walk done\r\n");
    } else if (!strcmp(c, "ws2scan")) {
        if (!g_ws2_scan)
            mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "ws2scan",
                                    (mico_thread_function_t)ws2_scan_thread, 0x600, 0);
        cmd_printf("ws2scan started: 0x40001000 bit0..31, watch ring\r\n");
    } else if (!strcmp(c, "zbuf")) {
        /* 往 32 字 RGB 缓冲区写颜色, 验证内核/硬件是否在轮询它 */
        int r = (argc >= 5) ? atoi(argv[2]) : 0;
        int g = (argc >= 5) ? atoi(argv[3]) : 0;
        int b = (argc >= 5) ? atoi(argv[4]) : 0;
        uint32_t addr = (argc >= 6) ? (uint32_t)strtoul(argv[5], NULL, 0) : 0x10030320u;
        volatile uint32_t *buf = (volatile uint32_t *)addr;
        uint32_t col = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        for (int i = 0; i < 32; i++) buf[i] = col;
        cmd_printf("zbuf: 32x0x%06lx -> 0x%08lx\r\n", (unsigned long)col, (unsigned long)addr);
    } else if (!strcmp(c, "zbufw")) {
        /* 按 0x00RRGGBB 之外的常见 WS2812 打包(GRB) 再写一次 */
        int r = (argc >= 5) ? atoi(argv[2]) : 0;
        int g = (argc >= 5) ? atoi(argv[3]) : 0;
        int b = (argc >= 5) ? atoi(argv[4]) : 0;
        volatile uint32_t *buf = (volatile uint32_t *)0x10030320u;
        uint32_t col = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;  /* GRB */
        for (int i = 0; i < 32; i++) buf[i] = col;
        cmd_printf("zbufw(GRB): 32x0x%06lx\r\n", (unsigned long)col);
    } else if (!strcmp(c, "ws3")) {
        if (argc >= 3) g_ws_scale = atoi(argv[2]);
        ws_api_flush(255, 0, 0);
        cmd_printf("ws3 red sent (GPIO API, irq-off), scale=%d\r\n", g_ws_scale);
    } else if (!strcmp(c, "auto")) {
        if (!g_auto_run)
            mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "autotest",
                                    (mico_thread_function_t)led_auto_thread, 0x800, 0);
        cmd_printf("AUTO test started: watch ring, note STEP number\r\n");
    } else if (!strcmp(c, "status")) {
        cmd_printf("type=%d led=%d mode=%d hue=%d bright=%d scale=%d r=%d g=%d b=%d ws=%d\r\n",
                   g_led_type, cfg->led, cfg->led_mode, cfg->led_hue, cfg->led_bright,
                   g_ws_scale, g_led_r, g_led_g, g_led_b, g_led_ws_pin);
    } else {
        cmd_printf("?\r\n");
    }
}
static const struct cli_command led_cli_cmd = {
    "led", "LED test: auto|on|off|zbuf r g b [addr]|ws2scan|ws2|ws3 [s]|type N|z h1 l1 h0 l0|t a b c d|count N|walk|pin N V|psw|status", cli_led
};

/* PWM 测试: 灯环很可能是 PWM 驱动的 3 路 R/G/B */
static void cli_pwm(CLI_ARGS)
{
    (void)xWriteBufferLen;
    if (argc < 2) { cmd_printf("pwm <ch 0-5> <duty 0-100> [freq] | pwm stop <ch> | pwm scan\r\n"); return; }
    if (!strcmp(argv[1], "stop") && argc >= 3) {
        int ch = atoi(argv[2]);
        if (ch >= 0 && ch <= 5) MicoPwmStop((mico_pwm_t)ch);
        cmd_printf("pwm ch%d stop\r\n", ch);
        return;
    }
    if (!strcmp(argv[1], "scan")) {
        for (int ch = 0; ch <= 5; ch++) {
            if (ch == 2) continue;               /* ch2 是风机, 跳过 */
            cmd_printf("PWM ch%d -> duty100\r\n", ch);
            MicoPwmInitialize((mico_pwm_t)ch, 25000, 100.0f); MicoPwmStart((mico_pwm_t)ch);
            mico_thread_msleep(1500);
            cmd_printf("PWM ch%d -> duty0\r\n", ch);
            MicoPwmInitialize((mico_pwm_t)ch, 25000, 0.0f); MicoPwmStart((mico_pwm_t)ch);
            mico_thread_msleep(500);
        }
        cmd_printf("PWM scan done\r\n");
        return;
    }
    int ch = atoi(argv[1]);
    int duty = (argc >= 3) ? atoi(argv[2]) : 100;
    int freq = (argc >= 4) ? atoi(argv[3]) : 25000;
    if (ch < 0 || ch > 5 || duty < 0) { cmd_printf("bad args\r\n"); return; }
    MicoPwmInitialize((mico_pwm_t)ch, freq, (float)duty); MicoPwmStart((mico_pwm_t)ch);
    cmd_printf("pwm ch%d duty=%d freq=%d\r\n", ch, duty, freq);
}
static const struct cli_command pwm_cli_cmd = {
    "pwm", "PWM test: pwm <ch> <duty> [freq] | pwm stop <ch> | pwm scan", cli_pwm
};

int main(void)
{
    A1_LOG(">>> main entry");
    sys_context = mico_system_context_init(sizeof(a1_config_t));
    A1_LOG(">>> context init done");
    cfg = (a1_config_t *)((system_context_t *)sys_context)->user_config_data;
    if (cfg == NULL) return -1;
    /* 配置合法性钳制(防止旧配置导致非法通道) */
    if (cfg->pwm_ch < 0 || cfg->pwm_ch > 5)   cfg->pwm_ch = A1_FAN_PWM;
    if (cfg->led_pwm < 0 || cfg->led_pwm > 5) cfg->led_pwm = A1_LED_PWM;
    if (cfg->fan_en_gpio < 0 || cfg->fan_en_gpio > 12) cfg->fan_en_gpio = A1_FAN_ENABLE_GPIO;

    A1_LOG(">>> calling mico_system_init");
    mico_system_init(sys_context);
    A1_LOG(">>> system init done");
    mico_rtos_init_mutex(&g_i2c_mux);
    mico_rtos_init_mutex(&g_apply_mux);
    mico_rtos_init_mutex(&g_led_mux);
    cli_register_command(&led_cli_cmd);   /* 串口: led ... */
    cli_register_command(&pwm_cli_cmd);   /* 串口: pwm ... */
    A1_LOG("=== A1 firmware %s ===", A1_VERSION);

    if (cfg->version != A1_CFG_VERSION) {
        A1_LOG("cfg version mismatch, defaults");
        appRestoreDefault_callback(cfg, sizeof(a1_config_t));
        mico_system_context_update(sys_context);
    }
    A1_LOG("step: cfg ok");

    IPStatusTypedef ip;
    if (micoWlanGetIPStatus(&ip, Station) == kNoErr) strncpy(a1_mac, ip.mac, sizeof(a1_mac) - 1);
    A1_LOG("step: mac=%s", a1_mac);

    snprintf(mqtt_name, sizeof(mqtt_name), "A1_%c%c%c%c", a1_mac[9],a1_mac[10],a1_mac[12],a1_mac[13]);
    snprintf(mqtt_set,  sizeof(mqtt_set),  "device/a1/%s/set", a1_mac);
    snprintf(mqtt_state,sizeof(mqtt_state),"device/a1/%s/state", a1_mac);
    snprintf(mqtt_avail,sizeof(mqtt_avail),"device/a1/%s/availability", a1_mac);
    snprintf(mqtt_disc, sizeof(mqtt_disc), "homeassistant/fan/%s/config", mqtt_name);
    mico_system_notify_register(mico_notify_WIFI_SCAN_COMPLETED, scan_completed_cb, NULL);
    mico_system_notify_register(mico_notify_DHCP_COMPLETED, dhcp_cb, NULL);
    mico_system_notify_register(mico_notify_WIFI_STATUS_CHANGED, wifi_status_cb, NULL);
    mico_system_notify_register(mico_notify_WIFI_CONNECT_FAILED, wifi_fail_cb, NULL);
    mico_system_notify_register(mico_notify_WIFI_Fatal_ERROR, wifi_fatal_cb, NULL);
    A1_LOG("step: notify reg");

    A1_LOG("step: hw init begin");
    a1_hw_init();
    A1_LOG("step: hw init done");
    A1_LOG("step: web start begin");
    a1_web_start();
    A1_LOG("step: web start done");

    if (strlen(cfg_ssid()) > 0) wifi_start_sta();
    else                        wifi_start_softap();
    A1_LOG("step: wifi started");

    sntp_start_auto_time_sync(3600000, sntp_synced_cb);
    /* 默认上电: 开机运行 + 风速 25% (每次上电都是这个状态) */
    cfg->power = 1; cfg->mode = 0; cfg->speed = 25;
    a1_apply();
    mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "a1tick",
                            (mico_thread_function_t)a1_tick_thread, 0x600, 0);
    mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "mqtt",
                            (mico_thread_function_t)a1_mqtt_thread, 0x1000, 0);
    mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "wifi",
                            (mico_thread_function_t)a1_wifi_thread, 0x800, 0);
    mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "ledfx",
                            (mico_thread_function_t)led_fx_thread, 0x800, 0);

    A1_LOG("init done, ssid='%s'", cfg_ssid());
    return 0;
}
