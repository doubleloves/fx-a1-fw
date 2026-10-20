#ifndef __MICO_CONFIG_H
#define __MICO_CONFIG_H

#define APP_INFO "A1 Air Purifier"

/* 自己管理 WiFi/SoftAP, 不用 SDK 的 easylink 配网 */
#define MICO_WLAN_CONNECTION_ENABLE     0
#define MICO_CONFIG_SERVER_ENABLE       0

#define MICO_CLI_ENABLE                 1
#define MICO_SYSTEM_MONITOR_ENABLE      1
#define MICO_SYSTEM_DISCOVERY_ENABLE    1

#define MICO_LOCAL_SERVER_PORT 80

#endif
