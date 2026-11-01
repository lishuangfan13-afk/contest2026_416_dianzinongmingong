#include <OneButton.h>
#include "Ticker.h"
#include "net.h"
#include "common.h"
#include "light.h"
#include "buzzer.h"
#include "preferencesUtil.h"
#include "emotion_display.h"
#include "emotion_protocol.h"

enum CurrentPage currentPage = SETTING;
int extensionDisplayBasePage(){ return (int)currentPage; }
bool playingMusic = false; // 是否正在播放音乐
bool belling = false; // 是否正在播放铃声
bool isCheckingTime = false; // 是否正在NTP对时中
int chimeHour = -1; // 上一次整点报时的钟点（避免同一小时内重复报时）
unsigned long prevChimeCheck = 0; // 整点报时检查的节流时间

void startShowText(void *param);
void startShowIp(void *param);
void startPlaySongs(void *param);
void startPlayBell(void *param);
void startChime(void *param);
void startTickerClock(int32_t seconds);
int32_t getClockRemainSeconds();
void btn1click();
void btn2click();
void btn3click();
void btn1LongClick();
void btn2LongClick();
void btn3LongClick();
void createPlaySongsTask();
void createBellTask();
void cancelBell();

///////////////////////////////////Freertos区域///////////////////////////////////////
// 按钮
OneButton button1(BTN1, true);
OneButton button2(BTN2, true);
OneButton button3(BTN3, true);
//闹钟响铃任务句柄
TaskHandle_t bellTask;
//播放歌曲的任务句柄(闹钟页面使用)
TaskHandle_t playSongsTask;
//系统启动动画的任务句柄
TaskHandle_t showTextTask;
//循环播放配网ip地址的任务句柄
TaskHandle_t showIpTask;
//创建闹铃任务
void createBellTask(){
  xTaskCreate(startPlayBell, "startPlayBell", 1000, NULL, 1, &bellTask);
}
//创建音乐播放任务(闹钟页面使用)
void createPlaySongsTask(){
  xTaskCreate(startPlaySongs, "startPlaySongs", 1000, NULL, 1, &playSongsTask);
}
//创建文字显示任务
void createShowTextTask(const char *text){
  xTaskCreate(startShowText, "startShowText", 1000, (void *)const_cast<char *>(text), 1, &showTextTask);
}
//创建轮播配网的ip地址的任务
void createShowIpTask(){
  xTaskCreate(startShowIp, "startShowIp", 1000, NULL, 1, &showIpTask);
}
// 启动文字显示，CONFIG不用了，改为采用显示ip地址的形式
void startShowText(void *param){
  String text = (const char *)param;
  int x;
  if(text.equals("START")){
    x = 2;
  }else if(text.equals("CONFIG")){
    x = 0;
  }
  int index = 0;
  while (true) {
    if(extensionDisplayActive()){
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    if(index % 3 == 0){
      drawText(x, 6, text + ".");
    }else if(index % 3 == 1){
      drawText(x, 6, text + "..");
    }else if(index % 3 == 2){
      drawText(x, 6, text + "...");
    }
    vTaskDelay(1000);
    index++;
  }
  Serial.println("退出启动文字显示");
}
// 轮播IP
void startShowIp(void *param){
  while (true) {
    if(extensionDisplayActive()){
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    showIp();
  }
  Serial.println("退出轮播配网IP动画");
}
// 播放歌曲(闹钟页面使用)
void startPlaySongs(void *param){
  while (true) {
    playSong(false);
    vTaskDelay(3000);
  }
  Serial.println("退出音乐播放");
}
// 响铃
void startPlayBell(void *param){
  while (true) {
    playSong(true);
    vTaskDelay(3000);
  }
  Serial.println("退出响铃任务");
}
//////////////////////////////////////////////////////////////////////////////////////


///////////////////////////////////定时器区域///////////////////////////////////////
Ticker tickerClock;
Ticker tickerCheckTime;
// NTP对时（Ticker 回调，不能与 net.cpp 里的 checkTime(int) 同名，
// 否则 Ticker::attach 无法在重载集合中推导出回调类型）
void onTimeCheckTicker(){
  // 只要将对时标志置为true即可，在主循环中进行对时操作
  isCheckingTime = true;
}
// 根据NVS中的闹钟时间计算出定时器需要多久之后触发
int32_t getClockRemainSeconds(){
  // 获取RTC时间
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)){
    Serial.println("获取RTC时间失败");
    return -1;
  }
  int32_t nowTime = (timeinfo.tm_hour * 60 + timeinfo.tm_min) * 60 + timeinfo.tm_sec;
  int32_t clockTime = (clockH * 60 + clockM) * 60;
  if(clockTime > nowTime){ // 闹钟时间在当天
    return (clockTime - nowTime);
  }else{ // 闹钟时间在第二天
    return (clockTime + ONE_DAY_SECONDS - nowTime);
  }
}
// 奏响闹铃
void ringingBell() {
  // 配置页面和闹钟设置页面不响铃
  if(currentPage == SETTING || currentPage == CLOCK){
    return;
  }
  // 进入时钟页面
  if(currentPage == RHYTHM){
    // 将二维数组重置为0
    memset(matrixArray,0,sizeof(matrixArray));
    // 将已点亮灯的个数置零
    lightedCount = 0;
  }
  currentPage = TIME;
  drawTimeFirstTime = true;
  // 创建播放音乐的任务
  createBellTask();
  belling = true;
  // 将定时器重置
  tickerClock.detach();
  startTickerClock(getClockRemainSeconds());
}
// 开启定时器
void startTickerClock(int32_t seconds){
  // seconds秒后，执行一次
  tickerClock.once(seconds, ringingBell);
}
void startTickerCheckTime(){
  // 每隔一段时间进行一次NTP对时
  tickerCheckTime.attach(TIME_CHECK_INTERVAL, onTimeCheckTicker);
}
// 整点报时任务：按当前钟点响对应次数（12小时制，12点响12声）
void startChime(void *param){
  int hour = (int)(intptr_t)param;
  int count = hour % 12;
  if(count == 0) count = 12;
  for(int i = 0; i < count; i++){
    tone(BUZZER, 1050, 100);
    vTaskDelay(200);
    noTone(BUZZER);
    vTaskDelay(120);
  }
  Serial.println("退出整点报时任务");
  vTaskDelete(NULL);
}
// 整点报时：由主循环调用，每500ms检查一次是否到了整点
void checkChime(){
  if(!chimeOn || !RTCSuccess) return; // 没开报时，或者还没对上时间
  if(belling || playingMusic) return; // 正在响铃/试听歌曲时不叠加
  if(currentPage == SETTING || currentPage == CLOCK) return; // 配网页和闹钟设置页不报时
  if(prevChimeCheck <= millis() && (millis() - prevChimeCheck) < 500) return;
  prevChimeCheck = millis();
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;
  if(timeinfo.tm_min == 0 && timeinfo.tm_sec <= 1 && timeinfo.tm_hour != chimeHour){
    chimeHour = timeinfo.tm_hour;
    xTaskCreate(startChime, "startChime", 1000, (void *)(intptr_t)timeinfo.tm_hour, 1, NULL);
  }
}
// 在闹钟响铃时，任何按键按下，都取消闹铃
void cancelBell(){
  vTaskDelete(bellTask);
  delay(300);
  belling = false;
}
//////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////// 按键区////////////////////////////////////////
// 初始化各按键
void btnInit(){
  button1.attachClick(btn1click);
  button1.setDebounceMs(10); //设置消抖时长 
  button2.attachClick(btn2click);
  button2.setDebounceMs(10); //设置消抖时长 
  button3.attachClick(btn3click);
  button3.setDebounceMs(10); //设置消抖时长 
  button1.attachLongPressStart(btn1LongClick);
  button1.setPressMs(1200); //设置长按时间
  button2.attachLongPressStart(btn2LongClick);
  button2.setPressMs(1200); //设置长按时间
  button3.attachLongPressStart(btn3LongClick);
  button3.setPressMs(1200); //设置长按时间
}
// ============ 时刻图案演示（测试用）：串口输入 icons 后逐个显示 6 张图案 ============
bool iconDemoOn = false; // 是否正在演示
int iconDemoStep = 0; // 当前演示到第几张（0~6）
int iconDemoOldPage = 1; // 演示前用户所在的子页，演示结束后还原
unsigned long iconDemoTime = 0; // 上一张的切换时间
// 顺序与 img/timeIcons.h 里的编号一致
const char *iconDemoNames[] = {
  "早晨 5~8", "上午 8~11", "中午 11~13", "下午 13~17",
  "傍晚 17~19", "晚上 19~5"
};
// 开始演示：临时切到"时:分"子页（时刻图案就显示在这一页）
void startIconDemo(){
  iconDemoOn = true;
  iconDemoStep = 0;
  iconDemoOldPage = timePage; // 记住用户原来的子页，结束后还原（不写NVS，不影响保存的设置）
  timePage = TIME_H_M;
  currentPage = TIME;
  timeIconOverride = 0;
  iconDemoTime = millis();
  drawTimeFirstTime = true;
  Serial.println("开始演示时刻图案：每张4秒，共6张");
  Serial.print("  [0] ");
  Serial.println(iconDemoNames[0]);
  drawTime();
}
// 演示推进：由主循环反复调用，走完 7 张自动结束
void runIconDemo(){
  if(!iconDemoOn){
    return;
  }
  if(iconDemoTime <= millis() && (millis() - iconDemoTime) < ICON_DEMO_INTERVAL){
    return;
  }
  iconDemoTime = millis();
  iconDemoStep++;
  if(iconDemoStep >= ICON_DEMO_COUNT){ // 演示结束，恢复正常显示
    iconDemoOn = false;
    timeIconOverride = -1;
    timePage = iconDemoOldPage; // 还原用户原来的子页
    Serial.println("时刻图案演示结束");
    drawTimeFirstTime = true;
    drawTime();
    return;
  }
  timeIconOverride = iconDemoStep;
  Serial.print("  [");
  Serial.print(iconDemoStep);
  Serial.print("] ");
  Serial.println(iconDemoNames[iconDemoStep]);
  drawTimeFirstTime = true; // 强制整屏重画
  drawTime();
}
// 串口命令：在串口监视器(115200)里回车发送。
//   config = 重启进入配网模式（裸板没按键时重新配网用）
//   icons  = 逐个演示 7 张时刻图案（测试用）
void checkSerialCommand(){
  static char line[EMOTION_LINE_MAX + 1];
  static size_t used = 0;
  static bool overflow = false;
  static bool invalid_ascii = false;
  // Incremental bounded input: no readString timeout can stall the display.
  for(unsigned budget = 0; budget < 256 && Serial.available(); ++budget){
    int value = Serial.read();
    if(value < 0) break;
    char ch = (char)value;
    if(ch != '\n'){
      unsigned char byte_value = (unsigned char)ch;
      if((byte_value < 32 && ch != '\r' && ch != '\t') || byte_value > 126) invalid_ascii = true;
      if(used < EMOTION_LINE_MAX && !overflow) line[used++] = ch;
      else overflow = true;
      continue;
    }
    if(overflow){
      extensionDisplayProtocolError("frame_too_long");
    }else if(invalid_ascii){
      extensionDisplayProtocolError("invalid_ascii");
    }else{
      line[used] = 0;
      if(used && !extensionDisplayHandleLine(line)){
        String cmd(line);
        cmd.trim();
        if(cmd.equalsIgnoreCase("config")){
          extensionDisplayRelease();
          Serial.println("收到串口命令：重启并进入配网模式");
          setApConfigWhenStart(true);
          delay(200);
          ESP.restart();
        }else if(cmd.equalsIgnoreCase("icons")){
          extensionDisplayRelease();
          Serial.println("收到串口命令：开始演示时刻图案");
          startIconDemo();
        }else if(cmd.length() > 0){
          Serial.println("可用命令：config = 重启进入配网模式 ; icons = 逐个演示7张时刻图案 ; JSON ping/status/emotion");
        }
      }
    }
    used = 0;
    overflow = false;
    invalid_ascii = false;
  }
}
// 监控按键
void watchBtn(){
  button1.tick();
  button2.tick();
  button3.tick();
}
// 按键方法
// >按键
void btn1click(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  switch(currentPage){
    case TIME:
      // 四个子页循环：时:分:秒 → 时:分 → 日期 → 天气温度
      if(timePage == TIME_H_M_S){
        timePage = TIME_H_M;
      }else if(timePage == TIME_H_M){
        timePage = TIME_DATE;
      }else if(timePage == TIME_DATE){
        timePage = TIME_TEMP;
      }else{
        timePage = TIME_H_M_S;
      }
      // 清屏
      clearMatrix();
      // 保存设置
      recordExtensionPage();
      // 绘制时间
      drawTimeFirstTime = true;
      drawTime();
      break;
    case RHYTHM:
      if(rhythmPage == RHYTHM_MODEL1){
        rhythmPage = RHYTHM_MODEL2;
      }else if(rhythmPage == RHYTHM_MODEL2){
        rhythmPage = RHYTHM_MODEL3;
      }else if(rhythmPage == RHYTHM_MODEL3){
        rhythmPage = RHYTHM_MODEL4;
      }else if(rhythmPage == RHYTHM_MODEL4){
        rhythmPage = RHYTHM_MODEL1;
      }
      // 保存设置
      recordExtensionPage();
      break;
    case ANIM:
      if(animPage == ANIM_MODEL1){
        animPage = ANIM_MODEL2;
      }else if(animPage == ANIM_MODEL2){
        // 将已点亮灯的个数置零
        lightedCount = 0;
        animPage = ANIM_MODEL3;
      }else if(animPage == ANIM_MODEL3){       
        animPage = ANIM_MODEL1;
      }
      // 将二维数组重置为0
      memset(matrixArray,0,sizeof(matrixArray));
      // 保存设置
      recordExtensionPage();
      // 清屏
      clearMatrix();
      break;
    case CLOCK:
      if(!clockOpen){
        return;
      }
      if(clockChoosed == CLOCK_H){
        tmpClockH++;
        if(tmpClockH == 24){
          tmpClockH = 0;
        }
        drawClock();
      }else if(clockChoosed == CLOCK_M){
        tmpClockM++;
        if(tmpClockM == 60){
          tmpClockM = 0;
        }
        drawClock();
      }else if(clockChoosed == CLOCK_BELL){
        tmpClockBellNum++;
        if(tmpClockBellNum == songCount){
          tmpClockBellNum = 0;
        }
        // 停止音乐播放
        vTaskDelete(playSongsTask);
        delay(300);
        // 开始音乐播放
        createPlaySongsTask();       
      }
      break;
    case BRIGHT:
      if(brightModel == BRIGHT_MODEL_AUTO) return;
      if(brightness >= 145){
        Serial.println("已达最大亮度");
        return;
      }
      brightness+=BRIGHTNESS_SPACING;
      Serial.print("当前亮度：");Serial.println(brightness);
      recordBrightness();
      drawBright();
      break;
    case GAME:
      gameTurn(1); // 蛇顺时针转 45°
      break;
    default:
      break;
  }
}
// <按键
void btn2click(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  switch(currentPage){
    case TIME:     
      // 反向子页循环：时:分:秒 → 天气温度 → 日期 → 时:分
      if(timePage == TIME_H_M_S){
        timePage = TIME_TEMP;
      }else if(timePage == TIME_H_M){
        timePage = TIME_H_M_S;
      }else if(timePage == TIME_DATE){
        timePage = TIME_H_M;
      }else{
        timePage = TIME_DATE;
      }
      // 清屏
      clearMatrix();
      // 保存设置
      recordExtensionPage();
      // 绘制时间
      drawTimeFirstTime = true;
      drawTime();
      break;
    case RHYTHM:
      if(rhythmPage == RHYTHM_MODEL1){
        rhythmPage = RHYTHM_MODEL4;
      }else if(rhythmPage == RHYTHM_MODEL2){
        rhythmPage = RHYTHM_MODEL1;
      }else if(rhythmPage == RHYTHM_MODEL3){
        rhythmPage = RHYTHM_MODEL2;
      }else if(rhythmPage == RHYTHM_MODEL4){
        rhythmPage = RHYTHM_MODEL3;
      }
      // 保存设置
      recordExtensionPage();
      break;
    case ANIM:
      if(animPage == ANIM_MODEL1){
        // 将已点亮灯的个数置零
        lightedCount = 0;
        animPage = ANIM_MODEL3;
      }else if(animPage == ANIM_MODEL2){
        animPage = ANIM_MODEL1;
      }else if(animPage == ANIM_MODEL3){
        animPage = ANIM_MODEL2;
      }
      // 将二维数组重置为0
      memset(matrixArray,0,sizeof(matrixArray));
      // 保存设置
      recordExtensionPage();
      // 清屏
      clearMatrix();
      break;
    case CLOCK:
      if(!clockOpen){
        return;
      }
      if(clockChoosed == CLOCK_H){
        tmpClockH--;
        if(tmpClockH == -1){
          tmpClockH = 23;
        }
        drawClock();
      }else if(clockChoosed == CLOCK_M){
        tmpClockM--;
        if(tmpClockM == -1){
          tmpClockM = 59;
        }
        drawClock();
      }else if(clockChoosed == CLOCK_BELL){
        tmpClockBellNum--;
        if(tmpClockBellNum == -1){
          tmpClockBellNum = (songCount - 1);
        }
        // 停止音乐播放
        vTaskDelete(playSongsTask);
        delay(300);
        // 开始音乐播放
        createPlaySongsTask();
      }
      break;
    case BRIGHT:
      if(brightModel == BRIGHT_MODEL_AUTO) return;
      if(brightness <= 5){
        Serial.println("已达最小亮度");
        return;
      }
      brightness-=BRIGHTNESS_SPACING;
      Serial.print("当前亮度：");Serial.println(brightness);
      recordBrightness();
      drawBright();
      break;
    case GAME:
      gameTurn(-1); // 蛇逆时针转 45°
      break;
    default:
      break;
  }
}
void btn3click(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  switch(currentPage){
    case SETTING:
      // 关闭显示IP的动画任务（画板上传图案时会先停掉它，所以先判空再删）
      if(showIpTask != NULL){
        extensionDisplayDeleteTask(showIpTask);
        showIpTask = NULL;
        delay(300);
      }
      // 清空屏幕
      clearMatrix();
      // 查看是否曾经配置过网络
      if(ssid.equals("")){ // 没有配置过，进入节奏灯页面
        // 至节奏灯页面
        currentPage = RHYTHM;
      }else{  //有配置过，尝试进行连接
        // 将页面置为时间页面
        currentPage = TIME;
        drawTimeFirstTime = true; 
        // 将重新配网标志位置为false
        setApConfigWhenStart(false);
        // 这一段要用WiFi，先锁住，避免和天气任务抢
        wifiBusy = true;
        // 连接WiFi,30秒超时后显示wifi连接失败的图案
        connectWiFi(30); 
        // 如果连接上了wifi,就进行NTP对时,超过30秒对时失败，就进入节奏灯页面
        if(wifiConnected){
          checkTime(30);
        }
        wifiBusy = false;
      }
      break;
    case TIME:
      // 清屏
      clearMatrix();
      currentPage = RHYTHM;
      break;
    case RHYTHM:
      // 将二维数组重置为0
      memset(matrixArray,0,sizeof(matrixArray));
      // 将已点亮灯的个数置零
      lightedCount = 0;
      // 清屏
      clearMatrix();
      currentPage = ANIM;
      break;
    case ANIM:
      // 至"自定义图案"页面（网页画板画的那张图）
      memset(matrixArray,0,sizeof(matrixArray));
      lightedCount = 0;
      clearMatrix();
      currentPage = DRAW;
      drawCustom();
      break;
    case DRAW:
      // 至闹钟页面（若还没对时成功则去亮度页）
      if (!RTCSuccess){
        // 至亮度调节页面
        currentPage = BRIGHT;
        drawBright();
      }else{
        currentPage = CLOCK;
        resetTmpClockData();
        drawClock();
      }  
      break;
    case CLOCK:
      // 保存当前设置的闹钟信息
      clockH = tmpClockH;
      clockM = tmpClockM;
      clockBellNum = tmpClockBellNum;
      // 记录闹钟配置信息
      recordClockPage();
      // 将定时器重置
      tickerClock.detach();
      if(clockOpen){
        startTickerClock(getClockRemainSeconds());
      }
      // 如果正在播放音乐，则停止音乐播放
      if(playingMusic){
        vTaskDelete(playSongsTask);
        delay(300);
      }
      playingMusic = false;
      currentPage = BRIGHT;
      drawBright();
      break;
    case BRIGHT:
      if (!RTCSuccess){
        // 至节奏灯页面
        currentPage = RHYTHM;
      }else{
        currentPage = TIME;
        drawTimeFirstTime = true;
      }
      break; 
    case GAME:
      // 退出游戏，回到动画页面
      memset(matrixArray,0,sizeof(matrixArray));
      lightedCount = 0;
      clearMatrix();
      currentPage = ANIM;
      break;
    default:
      break;
  }
}
void btn1LongClick(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  if(currentPage == TIME){ // 时间页长按：开关整点报时
    chimeOn = !chimeOn;
    chimeHour = -1; // 重置记录，避免刚打开就判定为"这一小时已经报过"
    recordExtensionPage();
    if(chimeOn){ // 打开时响两声高音（顺便试听），关闭时响一声低音
      tone(BUZZER, 1200, 120);
      delay(160);
      tone(BUZZER, 1600, 160);
      delay(200);
    }else{
      tone(BUZZER, 500, 220);
      delay(260);
    }
    noTone(BUZZER);
    return;
  }
  if(currentPage == ANIM){ // 动画页长按：进入贪吃蛇游戏
    memset(matrixArray,0,sizeof(matrixArray));
    lightedCount = 0;
    clearMatrix();
    currentPage = GAME;
    gameStart();
    return;
  }
  if(currentPage == GAME){ // 游戏页长按：重开一局
    gameStart();
    return;
  }
  if(currentPage != CLOCK || !clockOpen){
    return;
  }
  if(clockChoosed == CLOCK_H){
    clockChoosed = CLOCK_M;
    drawClock();
  }else if(clockChoosed == CLOCK_M){
    clockChoosed = CLOCK_BELL;
    drawClock();
    // 演奏当前铃声
    createPlaySongsTask();
    playingMusic = true;
  }else if(clockChoosed == CLOCK_BELL){
    // 停止演奏
    vTaskDelete(playSongsTask);
    delay(300);
    playingMusic = false;
    clockChoosed = CLOCK_H;
    drawClock();
  }
}
void btn2LongClick(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  if(currentPage == TIME){ // 时间页长按：12/24 小时制切换
    timeFormat = (timeFormat == TIME_FORMAT_24) ? TIME_FORMAT_12 : TIME_FORMAT_24;
    recordExtensionPage();
    clearMatrix();
    drawTimeFirstTime = true; // 制式变了，强制整屏重画，避免新旧数字做动画对比
    drawTime();
    return;
  }
  if(currentPage == GAME){ // 游戏页长按：重开一局
    gameStart();
    return;
  }
  if(currentPage != CLOCK || !clockOpen){
    return;
  }
  if(clockChoosed == CLOCK_H){
    clockChoosed = CLOCK_BELL;
    drawClock();
    // 演奏当前铃声
    createPlaySongsTask();
    playingMusic = true;
  }else if(clockChoosed == CLOCK_M){
    clockChoosed = CLOCK_H;
    drawClock();
  }else if(clockChoosed == CLOCK_BELL){
    // 停止演奏
    vTaskDelete(playSongsTask);
    delay(300);
    playingMusic = false;
    clockChoosed = CLOCK_M;
    drawClock();
  }
}
void btn3LongClick(){
  extensionDisplayRelease();
  if(belling){
    cancelBell();
    return;
  }
  if(currentPage == SETTING){
    return;
  }
  if(currentPage == CLOCK){ // 闹钟页面长按，关闭/开启闹钟
    if(clockChoosed == CLOCK_BELL){
      if(playingMusic){
        // 停止演奏
        vTaskDelete(playSongsTask);
        delay(300);
        playingMusic = false;
      }else{
        // 演奏当前铃声
        createPlaySongsTask();
        playingMusic = true;
      }
    }
    clockOpen = !clockOpen;
    drawClock();
    recordClockPage();
  }else if(currentPage == RHYTHM){ // 节奏灯页面长按，切换频段模式
    if(rhythmBandsModel == RHYTHM_BANDS_MODEL1){
      rhythmBandsModel = RHYTHM_BANDS_MODEL2;
    }else{
      rhythmBandsModel = RHYTHM_BANDS_MODEL1;
    }
    // 将相关数组重置为0（按元素类型计算字节数，避免只清掉一部分）
    memset(peak,0,NUM_BANDS * sizeof(peak[0]));
    memset(oldBarHeights,0,NUM_BANDS * sizeof(oldBarHeights[0]));
    // 保存新模式
    recordExtensionPage();
  }else if(currentPage == BRIGHT){ // 亮度页面长按，切换亮度调节模式
    if(brightModel == BRIGHT_MODEL_MANUAL){
      brightModel = BRIGHT_MODEL_AUTO;
      // 重置采样值
      clearBrightSampling();
    }else{
      brightModel = BRIGHT_MODEL_MANUAL;
    }
    // 保存新模式
    recordExtensionPage();
    // 重新读取NVS中的亮度值
    getBrightness();
    // 刷新页面
    drawBright();
  }else if(currentPage == TIME){ // 时间页面长按，切换时间跳变模式
    if(timeModel == TIME_MODEL_DIRECT){
      timeModel = TIME_MODEL_ANIM;
    }else{
      timeModel = TIME_MODEL_DIRECT;
    }
    // 保存新模式
    recordExtensionPage();
    // 重置时间动画索引
    timeIndex = 0;
  }else if(currentPage == GAME){ // 游戏页面长按，同样退出游戏（避免误触发重新配网）
    memset(matrixArray,0,sizeof(matrixArray));
    lightedCount = 0;
    clearMatrix();
    currentPage = ANIM;
  }else{ // 其他页面长按，重启并配网
    Serial.println("重启并配网");
    setApConfigWhenStart(true);
    ESP.restart();
  }
}
///////////////////////////////////////////////////////////////

