#ifndef __NET_H
#define __NET_H
#include "common.h"

void wifiConfigBySoftAP(void);
void doClient(void);
void connectWiFi(int timeOut_s);
void getNTPTime(void);
void checkWiFiStatus(void);
void checkTime(int limitTime);
void disConnectWifi();
void checkTimeTicker();
extern String ssid;
extern String pass;
extern bool wifiConnected;
extern bool apConfig;
extern bool RTCSuccess;
// 天气（Open-Meteo）
void startWeatherTask();
bool fetchWeatherOnce();
extern String city; // 天气位置：城市名 或 "纬度,经度"
extern int weatherCode; // 天气代码（WMO）
extern float weatherTemp; // 气温（℃）
extern bool weatherValid; // 是否取到过有效天气
extern bool wifiBusy; // WiFi 互斥标志：天气更新与NTP对时不能同时操作WiFi

#endif