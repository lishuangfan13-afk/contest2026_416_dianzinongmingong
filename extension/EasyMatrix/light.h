#ifndef __LIGHT_H
#define __LIGHT_H

#include <Adafruit_NeoMatrix.h>

extern int clockColor[3];
extern int brightness;
extern uint16_t mainColor;
extern uint16_t weekColor;
extern Adafruit_NeoMatrix matrix;
extern int timePage;
extern int rhythmPage;
extern int animPage;
extern int rhythmBandsModel;
extern int timeModel;
extern int timeFormat;
extern bool chimeOn;
extern int brightModel;
extern int brightSamplingTime;
extern int brightSamplingValue;
// 与 light.cpp 中的定义保持一致：13行 = 8行显示区 + 5行下坠动画缓冲，
// 否则本文件看到的 sizeof 偏小，外部 memset 清不干净动画残留
extern int matrixArray[13][32];
extern byte peak[];
extern int oldBarHeights[];
extern int lightedCount;
extern int clockH;
extern int clockM;
extern int clockBellNum;
extern int tmpClockH;
extern int tmpClockM;
extern int tmpClockBellNum;
extern bool clockOpen;
extern int clockChoosed;
extern bool drawTimeFirstTime;
extern int timeIndex;
extern int timeIconOverride; // 时刻图案演示用：>=0 时强制显示该编号的图案，-1 为正常
extern uint16_t drawImage[256]; // 网页画板上传的自定义图案（32×8，RGB565）
extern bool drawImageValid; // 是否已经上传/保存过自定义图案
void drawCustom();
void initMatrix();
void drawWifiText();
void drawText(int x, int y, String text);
void drawCheckTimeText();
void drawFailed(int textX, int failedX, String text);
void clearMatrix();
void drawTime();
void showIp();
void drawBright();
void drawAnim();
void drawRHYTHM();
void resetTmpClockData();
void drawClock();
void calculateBrightnessValue();
void clearBrightSampling();
// 贪吃蛇小游戏
void drawGame();
void gameStart();
void gameTurn(int delta);

#endif

