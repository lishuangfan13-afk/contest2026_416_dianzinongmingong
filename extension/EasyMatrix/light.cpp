#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include "MyFont.h"
#include <Arduino.h>
#include <arduinoFFT.h>
#include "common.h"
#include "preferencesUtil.h"
#include "task.h"
#include "net.h"
#include "emotion_display.h"

int clockColor[3];
uint16_t mainColor;
uint16_t weekColor;
int brightness;
int showIpIndex = MATRIX_SIDE * 4;
int weekChromatism = 50; // 绘制星期时的色差基础值
// 时钟页面
bool drawTimeFirstTime = true; // 直接显示时间
int hour,minu,sec; // 之前显示的时、分、秒的数值
int timeIndex = 0; // 滑动动画帧数指针
int animIndex = 0; // 时钟页面动画索引
int timeModel; // 时间跳变模式
int timeFormat; // 时间制式（24小时制/12小时制）
bool chimeOn; // 整点报时开关
int timeIconOverride = -1; // 时刻图案演示用：>=0 时强制显示该编号的图案（-1=正常按钟点判断）
// 闹钟页面
bool clockOpen; // 闹钟是否开启
int clockH,clockM,clockBellNum; // 闹钟时、分、闹铃编号
int tmpClockH,tmpClockM,tmpClockBellNum; // 用来显示的临时闹钟时、分、闹铃编号
int clockChoosed = CLOCK_H; // 默认选中时
// 记录分页面
int timePage;
int rhythmPage;
int animPage;
// 亮度页面
int brightModel; // 亮度调节模式
int brightSamplingTime; // 亮度采样次数
int brightSamplingValue; // 采样值
// 动画页相关
int matrixArray[13][32]; // 点阵二维数组，用来记录一些数据
int animInterval1 = 60; // 动画1每一帧动画间隔
int animInterval2 = 80; // 动画2每一帧动画间隔
int animInterval3 = 100; // 动画3每一帧动画间隔
int hackAnimProbability = 8; // 骇客动画产生的概率，数值越大，新产生的下坠动画就越少
int lightedCount = 0; // 随机点动画已点亮的个数
bool increasing = true; // 随机点动画正在增加状态 
unsigned long animTime; // 记录上一次动画的时间
// 自定义图案页（网页画板上传的 32×8 图案，RGB565 按行存储）
uint16_t drawImage[LIGHTCOUNT]; // 256 个像素 × 2 字节 = 512 字节
bool drawImageValid = false; // 是否已经上传/保存过图案
// 节奏灯页面
int rhythmBandsModel; // 节奏灯频段模式
uint16_t hsv2rgb(uint16_t hue, uint8_t saturation, uint8_t value);
unsigned int sampling_period_us; //采样周期
byte peak[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int oldBarHeights[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int bandValues[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int bandIndex[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
double vReal[SAMPLES];
double vImag[SAMPLES];
int maxBandValue;
unsigned long starTime;
unsigned long peekDecayTime;
unsigned long changeColorTime;
int colorTime;
// arduinoFFT 2.x 起改为类模板，类名为 ArduinoFFT<T>，且方法名首字母小写
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);
int model2ColorArray[2][3] = {{0, 220, 255}, {240, 45, 255}};
int model4ColorArrar[8][3] = {{240, 45, 255},{253, 98, 248},{253, 169, 205},{255, 196, 123},{253, 214, 200},{253, 192, 255},{249, 175, 255},{0, 220, 255}};

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(MATRIX_SIDE, MATRIX_SIDE, MATRIX_COUNT, 1, DATAPIN,
NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE + 
NEO_TILE_TOP + NEO_TILE_LEFT + NEO_TILE_ROWS + NEO_TILE_PROGRESSIVE, NEO_GRB + NEO_KHZ800);

// 重置亮度采样值
void clearBrightSampling(){
  brightSamplingTime = 0;
  brightSamplingValue = 0;
}

// 根据亮度采样值和次数，计算自动亮度
void calculateBrightnessValue(){
  int val = brightSamplingValue / brightSamplingTime; // 三次采样平均值
  // Serial.println(val);
  brightness = map(val, 0, 4095, 1, 65);
  // if(val >= 3000){
  //   brightness = 65;
  // }else if(val >= 2000){
  //   brightness = 45;
  // }else{
  //   brightness = map(val, 0, 1999, 0, 44);
  // }
  // Serial.print("亮度：");
  // Serial.println(brightness);
}

// 初始化矩阵
void initMatrix(){
  matrix.begin();
  matrix.setFont(&MyFont);
  matrix.setTextWrap(false);
  // 计算采样周期（单位：uS）
  sampling_period_us = round(1000000 * (1.0 / SAMPLING_FREQ));
  // 初始化拾音器
  pinMode(AUDIO_IN_PIN, INPUT);
  // 采样一次亮度值，确定初始亮度
  if(brightModel == BRIGHT_MODEL_AUTO){
    brightSamplingValue+=analogRead(LIGHT_ADC);
    brightSamplingTime++;
    calculateBrightnessValue();
    clearBrightSampling();
  }
}

// 清空屏幕
void clearMatrix(){
  matrix.fillScreen(0);
  matrix.show();
}

// 绘制对时文字
void drawCheckTimeText(){
  matrix.fillScreen(0);
  matrix.setBrightness(brightness);
  matrix.drawBitmap(0, 0, checkingTime, 32, 8, mainColor);
  matrix.show();
}

// 绘制文字
void drawText(int x, int y, String text){
  if(!extensionLegacyDisplayBegin()) return;
  matrix.setBrightness(brightness);
  matrix.setFont(&MyFont);
  matrix.fillScreen(0);
  matrix.setCursor(x,y);
  matrix.setTextColor(mainColor);
  matrix.print(text);
  matrix.show();
  extensionLegacyDisplayEnd();
}

// 绘制wifi连接错误的文字和图案
void drawFailed(int textX, int failedX, String text){
  drawText(textX, 6, text);
  matrix.setCursor(failedX,6);
  matrix.setTextColor(matrix.Color(255, 0, 0));
  matrix.print("X");
  matrix.setBrightness(brightness);
  matrix.show();
}

// 轮播配网IP地址
void showIp(){
  if(!extensionLegacyDisplayBegin()) return;
  matrix.fillScreen(0);
  matrix.setTextColor(mainColor);
  matrix.setCursor(showIpIndex, 6);
  matrix.print("192.168.1.1");
  matrix.setBrightness(brightness);
  matrix.show();
  extensionLegacyDisplayEnd();
  delay(100);
  showIpIndex--;
  if(showIpIndex < -36){
    showIpIndex = MATRIX_SIDE * 4;
  }
}

///////////////////////////////// 天气与温度（数据来自 Open-Meteo） /////////////////////////////////
// 天气图标、气温、"鲸鱼"（版本日志里叫海豚）动画都画在 11×8 的图块里。
// 时间页的"时:分""日期"两个子页保持原来的鲸鱼动画不变；
// 新增的"天气温度页"（TIME_TEMP）左边画天气图标、右边画气温与摄氏度。
// 还没取到天气数据时，图标位置回退显示鲸鱼动画，气温显示 "--"。
// Open-Meteo 的天气代码 → 图标编号（图标见 img/weatherIcons.h）
int weatherCodeToIcon(int code){
  if(code == 0){
    return 0; // 晴
  }
  if(code == 1 || code == 2){
    return 1; // 多云
  }
  if(code == 3){
    return 2; // 阴
  }
  if(code == 45 || code == 48){
    return 3; // 雾
  }
  if((code >= 51 && code <= 67) || (code >= 80 && code <= 82)){
    return 4; // 毛毛雨/雨/阵雨
  }
  if((code >= 71 && code <= 77) || code == 85 || code == 86){
    return 5; // 雪
  }
  if(code >= 95){
    return 6; // 雷暴
  }
  return 1;
}
// 画天气图标
void drawSideWeatherIcon(int x, int y){
  matrix.drawRGBBitmap(x, y, weatherIcons[weatherCodeToIcon(weatherCode)], 11, 8);
}
// 原来那套海豚喷水动画（没有天气数据时的回退显示）
void drawSideDolphin(int x, int y){
  switch(animIndex){
    case 0:
    case 1:
      matrix.drawRGBBitmap(x, y, timeAnim0, 11, 8);
      break;
    case 2:
    case 3:
      matrix.drawRGBBitmap(x, y, timeAnim1, 11, 8);
      break;
    case 4:
    case 5:
      matrix.drawRGBBitmap(x, y, timeAnim2, 11, 8);
      break;
    case 6:
    case 7:
      matrix.drawRGBBitmap(x, y, timeAnim3, 11, 8);
      break;
    case 8:
    case 9:
      matrix.drawRGBBitmap(x, y, timeAnim4, 11, 8);
      break;
    case 10:
    case 11:
      matrix.drawRGBBitmap(x, y, timeAnim5, 11, 8);
      break;
    case 12:
    case 13:
      matrix.drawRGBBitmap(x, y, timeAnim6, 11, 8);
      break;
    case 14:
    case 15:
      matrix.drawRGBBitmap(x, y, timeAnim7, 11, 8);
      break;
    case 16:
    case 17:
      matrix.drawRGBBitmap(x, y, timeAnim8, 11, 8);
      break;
    case 18:
    case 19:
      matrix.drawRGBBitmap(x, y, timeAnim0, 11, 8);
      break;
    default:
      animIndex = 9;
      break;
  }
  animIndex++;
  if(animIndex == 20){
    animIndex = 0;
  }
}
///////////////////////////////// 气温大字（自绘七段数码管） /////////////////////////////////
// 板载 MyFont 是 Picopixel 小字体（数字 3 宽 × 5 高），放在 11×8 的天气图标旁边太小，
// 所以气温改成自己画的"七段数码管"：每个字符 4 宽 × 5 高（段线 1 像素），
// 度点是 2×2 的实心方块，单位 C 用同一套段码拼（上横 + 左下竖 + 下横），整块在右边 21 列里居中。
#define SEG_A 0x01 // 上横
#define SEG_B 0x02 // 右上竖
#define SEG_C 0x04 // 右下竖
#define SEG_D 0x08 // 下横
#define SEG_E 0x10 // 左下竖
#define SEG_F 0x20 // 左上竖
#define SEG_G 0x40 // 中横
#define TEMP_CHAR_W 4 // 字符宽
#define TEMP_CHAR_H 5 // 字符高
#define TEMP_GAP    1 // 字符间距
#define TEMP_X0     11 // 气温区起始列（左边 11 列留给天气图标）
#define TEMP_X1     31 // 气温区结束列
// 字符 -> 段码
uint8_t tempSegOf(char c){
  switch(c){
    case '0': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
    case '1': return SEG_B | SEG_C;
    case '2': return SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
    case '3': return SEG_A | SEG_B | SEG_G | SEG_C | SEG_D;
    case '4': return SEG_F | SEG_G | SEG_B | SEG_C;
    case '5': return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
    case '6': return SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D;
    case '7': return SEG_A | SEG_B | SEG_C;
    case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
    case '9': return SEG_A | SEG_F | SEG_G | SEG_B | SEG_C | SEG_D;
    case '-': return SEG_G;                                 // 负号就是中间一横
    case 'L': return SEG_F | SEG_E | SEG_D;                 // 低于 -9 度显示 LO
    case 'O': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
    case 'H': return SEG_F | SEG_E | SEG_G | SEG_B | SEG_C; // 高于 99 度显示 HI
    default:  return 0;
  }
}
// 画一个七段字符（宽 4、高 5），(x,y) 是左上角
// 上下两截竖笔画各画 2 像素、上下相接（上截 1~2 行、下截 3~4 行），
// 这样"0""1""7"既不断格，竖笔画也能占满整个字高
void drawTempChar(int x, int y, uint8_t seg, uint16_t color){
  if(seg & SEG_A) matrix.drawFastHLine(x, y, TEMP_CHAR_W, color);
  if(seg & SEG_F) matrix.drawFastVLine(x, y + 1, 2, color);
  if(seg & SEG_B) matrix.drawFastVLine(x + TEMP_CHAR_W - 1, y + 1, 2, color);
  if(seg & SEG_G) matrix.drawFastHLine(x, y + 2, TEMP_CHAR_W, color);
  if(seg & SEG_E) matrix.drawFastVLine(x, y + 3, 2, color);
  if(seg & SEG_C) matrix.drawFastVLine(x + TEMP_CHAR_W - 1, y + 3, 2, color);
  if(seg & SEG_D) matrix.drawFastHLine(x, y + TEMP_CHAR_H - 1, TEMP_CHAR_W, color);
}
// 在温度下方（屏幕最后一行）画温度的二进制
// 每一位固定占 3 格：1 = 亮 2 格 + 暗 1 格，0 = 亮 1 格 + 暗 2 格，
// 这样"1"和"0"一眼能分辨，也不用额外画分隔线；最后一位后面不留空格。
// 负数取绝对值（例如 -9 画 9 = 1001）；0~99 最多 7 位，占 7*3-1 = 20 格，右侧 21 格放得下。
void drawTempBinary(int value){
  if(value < 0){
    value = -value;
  }
  if(value > 127){
    value = 127;
  }
  int bits = 1; // 求位数
  for(int v = value; v > 1; v >>= 1){
    bits++;
  }
  int width = bits * 3 - 1;
  int x = TEMP_X0 + (TEMP_X1 - TEMP_X0 + 1 - width) / 2; // 跟上面的数字一样居中
  int y = MATRIX_SIDE - 1; // 最下面一行
  for(int i = bits - 1; i >= 0; i--){
    matrix.drawPixel(x, y, mainColor); // 每一位至少有 1 格亮
    if((value >> i) & 1){
      matrix.drawPixel(x + 1, y, mainColor); // 是 1 就再亮一格
    }
    x += 3;
  }
}
// 天气温度页：左边 11×8 天气图标，右边"数字 + 度点 + C"
void drawTempPage(){
  // 左边：天气图标；还没数据时显示鲸鱼动画，跟其它子页保持一致
  if(weatherValid){
    drawSideWeatherIcon(0, 0);
  }else{
    drawSideDolphin(0, 0);
  }
  // 右边：气温（取整数）
  String s;
  bool digitOnly = true;
  if(!weatherValid){
    s = "--"; // 还没取到天气数据
    digitOnly = false;
  }else if(weatherTemp < -9.5f){
    s = "LO"; // 低于-9度，两位数放不下，用字母表示
    digitOnly = false;
  }else if(weatherTemp > 99.5f){
    s = "HI"; // 高于99度
    digitOnly = false;
  }else{
    s = String((int)(weatherTemp + (weatherTemp >= 0 ? 0.5f : -0.5f)));
  }
  // 先算整块宽度：字符 + 字符间距 +（数字时）度点 2 像素与单位 C
  int total = s.length() * TEMP_CHAR_W + (s.length() - 1) * TEMP_GAP;
  if(digitOnly){
    total += TEMP_GAP + 2 + TEMP_GAP + TEMP_CHAR_W;
  }
  // 在右侧区域里水平居中；字符 5 高，在 8 行的屏上上下留空（(8-5)/2 = 1 行）
  int x = TEMP_X0 + (TEMP_X1 - TEMP_X0 + 1 - total) / 2;
  int y = (MATRIX_SIDE - TEMP_CHAR_H) / 2;
  for(unsigned int i = 0; i < s.length(); i++){
    char c = s.charAt(i);
    if(c == '1' || c == 'I'){
      // 数字 1（以及 HI 的 I）：不用段码，直接在 4×5 的格子里画居中的 2 宽 × 5 高实心竖块
      matrix.fillRect(x + 1, y, 2, TEMP_CHAR_H, mainColor);
    }else{
      drawTempChar(x, y, tempSegOf(c), mainColor);
    }
    x += TEMP_CHAR_W + TEMP_GAP;
  }
  if(digitOnly){
    matrix.fillRect(x, y, 2, 2, mainColor); // 2×2 的"度"点
    x += 2 + TEMP_GAP;
    // 摄氏度 C：上横 + 左下竖 + 下横（跟数字同一套段码，风格统一）
    drawTempChar(x, y, SEG_A | SEG_F | SEG_E | SEG_D, mainColor);
    // 数字下面（最底行）再画一遍这个温度的二进制
    drawTempBinary(s.toInt());
  }
}
//////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////// 时刻图案（太阳/月亮） ////////////////////////////////////
// 按 24 小时钟点选图案：0早晨 1上午 2中午 3下午 4傍晚 5晚上（19~5，跨零点）
// 时段边界在 common.h 里用宏定义，要调整时段改那里即可（左闭右开）
int hourToTimeIcon(int hour){
  // 演示模式：串口输入 icons 后会强制指定图案，方便一次看全 6 张
  if(timeIconOverride >= 0){
    return timeIconOverride;
  }
  if(hour >= TIME_DAWN_START && hour < TIME_MORNING_START){
    return 0; // 早晨
  }
  if(hour >= TIME_MORNING_START && hour < TIME_NOON_START){
    return 1; // 上午
  }
  if(hour >= TIME_NOON_START && hour < TIME_AFTERNOON_START){
    return 2; // 中午
  }
  if(hour >= TIME_AFTERNOON_START && hour < TIME_DUSK_START){
    return 3; // 下午
  }
  if(hour >= TIME_DUSK_START && hour < TIME_NIGHT_START){
    return 4; // 傍晚
  }
  return 5; // 晚上：19~5，跨零点（原来的"深夜"已并入这一段）
}
// 画时刻图案（11×8），hour24 必须是 24 小时制的钟点
void drawTimeIcon(int x, int y, int hour24){
  matrix.drawRGBBitmap(x, y, timeIcons[hourToTimeIcon(hour24)], 11, 8);
}
////////////////////////////////////////////////////////////////////////////////////////////

// 绘制自定义图案页面：整屏画网页上传的 32×8 图案
void drawCustom(){
  // 图案是静态的，限速重绘即可（防止被"对时中"等画面盖掉后不恢复）
  static unsigned long prevCustomTime = 0;
  if(prevCustomTime <= millis() && (millis() - prevCustomTime) < 500){
    return;
  }
  prevCustomTime = millis();
  matrix.setBrightness(brightness);
  if(drawImageValid){
    matrix.drawRGBBitmap(0, 0, drawImage, MATRIX_WIDTH, MATRIX_SIDE);
  }else{
    // 还没上传过图案：给个提示（用板载字体，提示到网页画板去画）
    matrix.fillScreen(0);
    matrix.setFont(&MyFont);
    matrix.setTextColor(mainColor);
    matrix.setCursor(2, 6);
    matrix.print("EMPTY");
  }
  matrix.show();
}

// 绘制时间页面
void drawTime(){
  // 获取RTC时间
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)){
    Serial.println("获取RTC时间失败");
    return;
  }
  // 12小时制换算：0点显示12点、13点显示1点。直接改 timeinfo，
  // 后面的绘制和“是否要播放动画”的比较都自动按12小时制的数值走
  // 时刻图案要按 24 小时的钟点来判断，先存下来（下面 12 小时制换算会改写 timeinfo）
  int hour24 = timeinfo.tm_hour;
  bool isPM = (timeinfo.tm_hour >= 12);
  if(timeFormat == TIME_FORMAT_12){
    if(timeinfo.tm_hour > 12){
      timeinfo.tm_hour -= 12;
    }else if(timeinfo.tm_hour == 0){
      timeinfo.tm_hour = 12;
    }
  }
  matrix.setBrightness(brightness);
  matrix.setFont(&MyFont);
  matrix.setTextColor(mainColor);
  if(timePage == TIME_H_M_S){
    // 清屏
    matrix.fillScreen(0);
    // 绘制时
    if(timeModel == TIME_MODEL_DIRECT || drawTimeFirstTime || hour == timeinfo.tm_hour){
      matrix.setCursor(2, 5);
      String h;
      if(timeinfo.tm_hour < 10){
        h = "0" + String(timeinfo.tm_hour);
      }else{
        h = String(timeinfo.tm_hour);
      }
      matrix.print(h);
    }else if(hour != timeinfo.tm_hour){
      if(hour / 10 != timeinfo.tm_hour / 10){ // 时进位了，两个数字都要动画刷新
        matrix.setCursor(2, 6 + timeIndex);
        if(hour < 10){
          matrix.print("0" + String(hour));
        }else{
          matrix.print(String(hour));
        }
        matrix.setCursor(2, 0 + timeIndex);
        if(timeinfo.tm_hour < 10){
          matrix.print("0" + String(timeinfo.tm_hour));
        }else{
          matrix.print(String(timeinfo.tm_hour));
        }
      }else{ // 只需刷新个位数
        matrix.setCursor(2, 5);
        matrix.print(String(hour / 10));
        matrix.setCursor(6, 6 + timeIndex);
        matrix.print(String(hour % 10));
        matrix.setCursor(6, 0 + timeIndex);
        matrix.print(String(timeinfo.tm_hour % 10));
      }
    }
    // 绘制分
    if(timeModel == TIME_MODEL_DIRECT || drawTimeFirstTime || minu == timeinfo.tm_min){
      matrix.setCursor(12, 5);
      String m;
      if(timeinfo.tm_min < 10){
        m = "0" + String(timeinfo.tm_min);
      }else{
        m = String(timeinfo.tm_min);
      }
      matrix.print(m);
    }else if(minu != timeinfo.tm_min){
      if(minu / 10 != timeinfo.tm_min / 10){ // 分进位了，两个数字都要动画刷新
        matrix.setCursor(12, 6 + timeIndex);
        if(minu < 10){
          matrix.print("0" + String(minu));
        }else{
          matrix.print(String(minu));
        }
        matrix.setCursor(12, 0 + timeIndex);
        if(timeinfo.tm_min < 10){
          matrix.print("0" + String(timeinfo.tm_min));
        }else{
          matrix.print(String(timeinfo.tm_min));
        }
      }else{ // 只需刷新个位数
        matrix.setCursor(12, 5);
        matrix.print(String(minu / 10));
        matrix.setCursor(16, 6 + timeIndex);
        matrix.print(String(minu % 10));
        matrix.setCursor(16, 0 + timeIndex);
        matrix.print(String(timeinfo.tm_min % 10));
      }
    }
    // 绘制秒
    if(timeModel == TIME_MODEL_DIRECT || drawTimeFirstTime || sec == timeinfo.tm_sec){
      matrix.setCursor(22, 5);
      String s;
      if(timeinfo.tm_sec < 10){
        s = "0" + String(timeinfo.tm_sec);
      }else{
        s = String(timeinfo.tm_sec);
      }
      matrix.print(s);
    }else if(sec != timeinfo.tm_sec){
      if(sec / 10 != timeinfo.tm_sec / 10){ // 秒进位了，两个数字都要动画刷新
        matrix.setCursor(22, 6 + timeIndex);
        if(sec < 10){
          matrix.print("0" + String(sec));
        }else{
          matrix.print(String(sec));
        }
        matrix.setCursor(22, 0 + timeIndex);
        if(timeinfo.tm_sec < 10){
          matrix.print("0" + String(timeinfo.tm_sec));
        }else{
          matrix.print(String(timeinfo.tm_sec));
        }
      }else{ // 只需刷新个位数
        matrix.setCursor(22, 5);
        matrix.print(String(sec / 10));
        matrix.setCursor(26, 6 + timeIndex);
        matrix.print(String(sec % 10));
        matrix.setCursor(26, 0 + timeIndex);
        matrix.print(String(timeinfo.tm_sec % 10));
      }
    }
    // 绘制两个冒号
    matrix.setCursor(10, 5);
    matrix.print(":");
    matrix.setCursor(20, 5);
    matrix.print(":");
    // 绘制星期
    matrix.fillRect(0, 7, 31, 7, 0);
    int weekNum = timeinfo.tm_wday;
    if(weekNum == 0){
      weekNum = 7;
    }
    // 绘制6条直线(除了当天的)
    for(int i = 1; i <= 7; i++){
      if(i == weekNum){
        continue;
      }
      matrix.drawFastHLine(2 + (i - 1) * 4, 7, 3, weekColor);
    }
    matrix.drawFastHLine(2 + (weekNum - 1) * 4, 7, 3, mainColor);
  }else if(timePage == TIME_H_M){
    // 清屏
    matrix.fillScreen(0);
    // 左侧：按当前时刻显示太阳/月亮图案（早晨/上午/中午/下午/傍晚/晚上）
    drawTimeIcon(0, 0, hour24);
    // 绘制时
    if(timeModel == TIME_MODEL_DIRECT || drawTimeFirstTime || hour == timeinfo.tm_hour){
      matrix.setCursor(14, 5);
      String h;
      if(timeinfo.tm_hour < 10){
        h = "0" + String(timeinfo.tm_hour);
      }else{
        h = String(timeinfo.tm_hour);
      }
      matrix.print(h);
    }else if(hour != timeinfo.tm_hour){
      if(hour / 10 != timeinfo.tm_hour / 10){ // 时进位了，两个数字都要动画刷新
        matrix.setCursor(14, 6 + timeIndex);
        if(hour < 10){
          matrix.print("0" + String(hour));
        }else{
          matrix.print(String(hour));
        }
        matrix.setCursor(14, 0 + timeIndex);
        if(timeinfo.tm_hour < 10){
          matrix.print("0" + String(timeinfo.tm_hour));
        }else{
          matrix.print(String(timeinfo.tm_hour));
        }
      }else{ // 只需刷新个位数
        matrix.setCursor(14, 5);
        matrix.print(String(hour / 10));
        matrix.setCursor(18, 6 + timeIndex);
        matrix.print(String(hour % 10));
        matrix.setCursor(18, 0 + timeIndex);
        matrix.print(String(timeinfo.tm_hour % 10));
      }
    }
    // 绘制分
    if(timeModel == TIME_MODEL_DIRECT || drawTimeFirstTime || minu == timeinfo.tm_min){
      matrix.setCursor(24, 5);
      String m;
      if(timeinfo.tm_min < 10){
        m = "0" + String(timeinfo.tm_min);
      }else{
        m = String(timeinfo.tm_min);
      }
      matrix.print(m);
    }else if(minu != timeinfo.tm_min){
      if(minu / 10 != timeinfo.tm_min / 10){ // 分进位了，两个数字都要动画刷新
        matrix.setCursor(24, 6 + timeIndex);
        if(minu < 10){
          matrix.print("0" + String(minu));
        }else{
          matrix.print(String(minu));
        }
        matrix.setCursor(24, 0 + timeIndex);
        if(timeinfo.tm_min < 10){
          matrix.print("0" + String(timeinfo.tm_min));
        }else{
          matrix.print(String(timeinfo.tm_min));
        }
      }else{ // 只需刷新个位数
        matrix.setCursor(24, 5);
        matrix.print(String(minu / 10));
        matrix.setCursor(28, 6 + timeIndex);
        matrix.print(String(minu % 10));
        matrix.setCursor(28, 0 + timeIndex);
        matrix.print(String(timeinfo.tm_min % 10));
      }
    }
    // 绘制冒号
    matrix.setCursor(22, 5);
    matrix.print(":");
    // 绘制星期
    matrix.fillRect(12, 7, 31, 7, 0);
    int weekNum = timeinfo.tm_wday;
    if(weekNum == 0){
      weekNum = 7;
    }
    // 绘制6条直线(除了当天的)
    for(int i = 1; i <= 7; i++){
      if(i == weekNum){
        continue;
      }
      matrix.drawFastHLine(12 + (i - 1) * 3, 7, 2, weekColor);
    }
    matrix.drawFastHLine(12 + (weekNum - 1) * 3, 7, 2, mainColor);
  }else if(timePage == TIME_DATE){
    // 清屏
    matrix.fillScreen(0);
    // 左侧：保持原来的鲸鱼动画
    drawSideDolphin(0, 0);
    // 写入日期
    matrix.setCursor(12, 5);
    String month = timeinfo.tm_mon < 9 ? "0" : "";
    month = month + (timeinfo.tm_mon + 1);
    String day = timeinfo.tm_mday < 10 ? "0" : "";
    day = day + timeinfo.tm_mday;
    matrix.print(month + "-" + day);
    // 绘制星期
    int weekNum = timeinfo.tm_wday;
    if(weekNum == 0){
      weekNum = 7;
    }
    // 绘制6条直线(除了当天的)
    for(int i = 1; i <= 7; i++){
      if(i == weekNum){
        continue;
      }
      matrix.drawFastHLine(12 + (i - 1) * 3, 7, 2, weekColor);
    }
    matrix.drawFastHLine(12 + (weekNum - 1) * 3, 7, 2, mainColor);
  }else if(timePage == TIME_TEMP){
    // 天气温度页：整屏只显示"天气图标 + 气温℃"
    matrix.fillScreen(0);
    drawTempPage();
  }
  if(drawTimeFirstTime){
    drawTimeFirstTime = false;
    hour = timeinfo.tm_hour;
    minu = timeinfo.tm_min;
    sec = timeinfo.tm_sec;
  }else if(sec != timeinfo.tm_sec){
    timeIndex++;
    if(timeIndex == 6){
      timeIndex = 0;
      hour = timeinfo.tm_hour;
      minu = timeinfo.tm_min;
      sec = timeinfo.tm_sec;
    }
  } 
  // 12小时制下，下午/晚上在右上角点亮一小段横条作为 PM 指示（上午不显示）
  // 天气温度页没有时间，不画这个指示（避免压到右边的 "C"）
  if(timeFormat == TIME_FORMAT_12 && isPM && timePage != TIME_TEMP){
    matrix.drawFastHLine(29, 0, 3, weekColor);
  }
  matrix.show();
}

// 绘制亮度调节页面
void drawBright(){
  matrix.setBrightness(brightness);
  matrix.setFont(&MyFont);
  matrix.fillScreen(0);
  matrix.setTextColor(mainColor);
  if(brightModel == BRIGHT_MODEL_MANUAL){
    // 亮度图标
    matrix.drawFastVLine(13, 3, 2, mainColor);
    matrix.drawPixel(14, 2, mainColor);
    matrix.drawPixel(14, 5, mainColor);
    matrix.drawPixel(15, 1, mainColor);
    matrix.drawPixel(15, 6, mainColor);
    matrix.drawFastVLine(16, 1, 6, mainColor);
    matrix.drawFastVLine(17, 2, 4, mainColor);
    matrix.drawFastVLine(18, 3, 2, mainColor);
    // +=号
    if(brightness > 5){
      matrix.setCursor(3, 6);
      matrix.print("-");
    }
    if(brightness < 145){
      matrix.setCursor(26, 6);
      matrix.print("+");
    }
  }else{
    drawText(7,6,"AUTO");
  }
  matrix.show();
}

// 绘制动画页面
void drawAnim(){
  if(animPage == ANIM_MODEL1){
    if((millis() - animTime) < animInterval1) return;
    matrix.fillScreen(0);
    randomSeed(analogRead(RANDOM_SEED_PIN));
    for(int i = 0; i < MATRIX_COUNT * MATRIX_SIDE; i++){
      int animCount = 0; // 记录往下移一格后这一列还有几条动画存在（矩阵高度为8，动画高度为6，所以一列最多存在两条动画）
      int index1 = 0; // 记录往下移一格后第一个动画的头部位置
      int index2 = 0; // 记录往下移一格后第二个动画的头部位置
      // 根据矩阵二维数组判断这一列是否已经有动画，动画的起始位置（最上端）的值为1
      for(int j = 0; j < 13; j++){
        if(matrixArray[j][i] == 1){ // 这个位置是有动画的
          // 把这个动画往下移一格
          if((j + 1) <= 12){
            matrix.drawRGBBitmap(i,(j + 1 - 5),animHack,1,6);
            animCount++;
            if(animCount == 1){
              index1 = j + 1;
            }
            if(animCount == 2){
              index2 = j + 1;
            }
          }
        }
        // 将这个位置置为0
        matrixArray[j][i] = 0;
      }
      // 将新位置的值置为1
      if(animCount == 2){
        matrixArray[index1][i] = 1;
        matrixArray[index2][i] = 1;
      }else if(animCount == 1){
        matrixArray[index1][i] = 1;
      }
      int x = random(0, hackAnimProbability); // 随机数，如果是1则产生新动画
      if(x == 1){
        // 如果这一列没有动画，或者只有1个动画，并且这条动画与这条新生成的动画距离>=（6+1），就进入下面的判断
        if(animCount == 0 || (animCount == 1 && index1 >= 7)){
          // 判断左边同一行有没有动画，有的话也不生成新动画
          if(i == 0 || matrixArray[0][i - 1] == 0){
            matrix.drawRGBBitmap(i,-5,animHack,1,6);
            matrixArray[0][i] = 1;
          }
        }
      }
    }
  }else if(animPage == ANIM_MODEL2){
    if((millis() - animTime) < animInterval2) return;
    // matrixArray[0][0]记录大步骤，0：红色→黄色 1：黄色→绿色 2：绿色→蓝色 3：蓝色→紫色 4：紫色→红色
    // matrixArray[0][1]记录每一个大步骤的小步,每一种颜色渐变过程持续50步
    // matrixArray[0][2]记录颜色R
    // matrixArray[0][3]记录颜色G
    // matrixArray[0][4]记录颜色B
    if(matrixArray[0][0] == 0 && matrixArray[0][1] == 0){ // 刚开始设置为红色，初始值250
      matrixArray[0][2] = 250;
    }
    // 循环绘制竖线
    int tmpR = matrixArray[0][2];
    int tmpG = matrixArray[0][3];
    int tmpB = matrixArray[0][4];
    int tmpStep = matrixArray[0][0];
    for(int i = 0; i < MATRIX_SIDE * MATRIX_COUNT; i++){
      matrix.drawFastVLine(i, 0, 8, matrix.Color(tmpR, tmpG, tmpB));
      // 计算出下一个循环的颜色
      if(i != MATRIX_SIDE * MATRIX_COUNT - 1){
        if(tmpStep == 0){ // 红色→黄色
          tmpG = tmpG + 5;
          if(tmpG == 250){
            tmpStep++;
          }
        }else if(tmpStep == 1){ // 黄色→绿色
          tmpR = tmpR - 5;
          if(tmpR == 0){
            tmpStep++;
          }
        }else if(tmpStep == 2){ // 绿色→蓝色
          tmpG = tmpG - 5;
          tmpB = tmpB + 5;
          if(tmpG == 0){
            tmpStep++;
          }
        }else if(tmpStep == 3){ // 蓝色→紫色
          tmpR = tmpR + 5;
          if(tmpR == 250){
            tmpStep++;
          }
        }else if(tmpStep == 4){ // 紫色→红色
          tmpB = tmpB - 5;
          if(tmpB == 0){
            tmpStep = 0;
          }
        }
      }      
    }   
    if(matrixArray[0][0] == 0){ // 红色→黄色
      matrixArray[0][3] = matrixArray[0][3] + 5;
    }else if(matrixArray[0][0] == 1){ // 黄色→绿色
      matrixArray[0][2] = matrixArray[0][2] - 5;
    }else if(matrixArray[0][0] == 2){ // 绿色→蓝色
      matrixArray[0][3] = matrixArray[0][3] - 5;
      matrixArray[0][4] = matrixArray[0][4] + 5;
    }else if(matrixArray[0][0] == 3){ // 蓝色→紫色
      matrixArray[0][2] = matrixArray[0][2] + 5;
    }else if(matrixArray[0][0] == 4){ // 紫色→红色
      matrixArray[0][4] = matrixArray[0][4] - 5;
    }
    matrixArray[0][1] = matrixArray[0][1] + 1;
    if(matrixArray[0][1] == 50){
      matrixArray[0][1] = 0;
      matrixArray[0][0] = matrixArray[0][0] + 1;
      if(matrixArray[0][0] == 5){
        matrixArray[0][0] = 0;
      }
    } 
  }else if(animPage == ANIM_MODEL3){
    if((millis() - animTime) < animInterval3) return;
    // 计算是填充过程还是消失过程
    if(lightedCount == 0){
      increasing = true;
    }else if(lightedCount == MATRIX_SIDE * MATRIX_SIDE * MATRIX_COUNT){
      increasing = false;
    }
    randomSeed(analogRead(RANDOM_SEED_PIN));
    if(increasing){
      int num = random(1, 4); // 每次随机填充num个点
      if(num + lightedCount > MATRIX_SIDE * MATRIX_SIDE * MATRIX_COUNT){ // 不能超过灯珠总数量
        num = MATRIX_SIDE * MATRIX_SIDE * MATRIX_COUNT - lightedCount;
      }
      for(int i = 0; i < num; i++){
        int x = random(0, 32);
        int y = random(0, 8);
        while(matrixArray[y][x] == 1){ // 这个随机生成的坐标已经点亮
          x = random(0, 32);
          y = random(0, 8);
        }
        matrixArray[y][x] = 1;
        // 已点亮灯的个数 + 1
        lightedCount++;
        int r = random(10, 256);
        int g = random(10, 256);
        int b = random(10, 256);
        matrix.drawPixel(x, y, matrix.Color(r, g, b));
      }
    }else{
      int num = random(1, 4); // 每次随机消失num个点
      if(num > lightedCount){ // 不能大于剩余已点亮数量
        num = lightedCount;
      }
      for(int i = 0; i < num; i++){
        int x = random(0, 32);
        int y = random(0, 8);
        while(matrixArray[y][x] == 0){ // 这个随机生成的坐标已经熄灭
          x = random(0, 32);
          y = random(0, 8);
        }
        matrixArray[y][x] = 0;
        // 已点亮灯的个数 - 1
        lightedCount--;
        matrix.drawPixel(x, y, 0);
      }
    }
  }
  matrix.setBrightness(brightness);
  matrix.show();
  animTime = millis();
}

// 绘制节奏灯页面
void drawRHYTHM(){
  matrix.clear();
  // 重置频率块的值
  for (int i = 0; i < NUM_BANDS; i++){
    bandValues[i] = 0;
  }
  // 采样SAMPLES次
  for (int i = 0; i < SAMPLES; i++) {
    starTime = micros();
    vReal[i] = analogRead(AUDIO_IN_PIN);
    vImag[i] = 0;
    // Serial.println(micros() - starTime);
    while ((micros() - starTime) < sampling_period_us) { /* chill */ }
    // Serial.println(vReal[i]);
  }
  // 进行FFT计算（arduinoFFT 2.x 的 API 为小驼峰命名）
  FFT.dcRemoval();
  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();
  // 解析计算结果
  if(rhythmBandsModel == RHYTHM_BANDS_MODEL1){ // 左高频，右低频，32个频段
    for (int i = 2; i < (SAMPLES/2); i++){
      if (vReal[i] > NOISE) {
        // Serial.println(vReal[i]);
        // 去除前面6段低频杂音和一些高频尖叫
        if (i>6    && i<=9   ) bandValues[0]   += (int)vReal[i];
        if (i>9    && i<=11  ) bandValues[1]   += (int)vReal[i];
        if (i>11   && i<=13  ) bandValues[2]   += (int)vReal[i];
        if (i>13   && i<=15  ) bandValues[3]   += (int)vReal[i];
        if (i>15   && i<=17  ) bandValues[4]   += (int)vReal[i];
        if (i>17   && i<=19  ) bandValues[5]   += (int)vReal[i];
        if (i>19   && i<=21  ) bandValues[6]   += (int)vReal[i];
        if (i>21   && i<=23  ) bandValues[7]   += (int)vReal[i];
        if (i>23   && i<=25  ) bandValues[8]   += (int)vReal[i];
        if (i>25   && i<=27  ) bandValues[9]   += (int)vReal[i];
        if (i>27   && i<=29  ) bandValues[10]  += (int)vReal[i];
        if (i>29   && i<=31  ) bandValues[11]  += (int)vReal[i];
        if (i>31   && i<=33  ) bandValues[12]  += (int)vReal[i];
        if (i>33   && i<=35  ) bandValues[13]  += (int)vReal[i];
        if (i>35   && i<=38  ) bandValues[14]  += (int)vReal[i];
        if (i>38   && i<=41  ) bandValues[15]  += (int)vReal[i];
        if (i>41   && i<=44  ) bandValues[16]  += (int)vReal[i];
        if (i>44   && i<=47  ) bandValues[17]  += (int)vReal[i];
        if (i>47   && i<=50  ) bandValues[18]  += (int)vReal[i];
        if (i>50   && i<=53  ) bandValues[19]  += (int)vReal[i];
        if (i>53   && i<=56  ) bandValues[20]  += (int)vReal[i];
        if (i>56   && i<=59  ) bandValues[21]  += (int)vReal[i];
        if (i>59   && i<=62  ) bandValues[22]  += (int)vReal[i];
        if (i>62   && i<=65  ) bandValues[23]  += (int)vReal[i];
        if (i>65   && i<=68  ) bandValues[24]  += (int)vReal[i];
        if (i>68   && i<=71  ) bandValues[25]  += (int)vReal[i];
        if (i>71   && i<=74  ) bandValues[26]  += (int)vReal[i];
        if (i>74   && i<=77  ) bandValues[27]  += (int)vReal[i];
        if (i>77   && i<=80  ) bandValues[28]  += (int)vReal[i];
        if (i>80   && i<=83  ) bandValues[29]  += (int)vReal[i];
        if (i>83   && i<=87  ) bandValues[30]  += (int)vReal[i];
        if (i>87   && i<=91  ) bandValues[31]  += (int)vReal[i];
      }
    }
  }else{
     for (int i = 2; i < (SAMPLES/2); i++){
      if (vReal[i] > NOISE) {
        // Serial.println(vReal[i]);
        // 去除前面6段低频杂音和一些高频尖叫
        if (i>6    && i<=9   ) bandValues[15]   += (int)vReal[i];
        if (i>9    && i<=11  ) bandValues[16]   += (int)vReal[i];
        if (i>11   && i<=13  ) bandValues[14]   += (int)vReal[i];
        if (i>13   && i<=15  ) bandValues[17]   += (int)vReal[i];
        if (i>15   && i<=17  ) bandValues[13]   += (int)vReal[i];
        if (i>17   && i<=19  ) bandValues[18]   += (int)vReal[i];
        if (i>19   && i<=21  ) bandValues[12]   += (int)vReal[i];
        if (i>21   && i<=23  ) bandValues[19]   += (int)vReal[i];
        if (i>23   && i<=25  ) bandValues[11]   += (int)vReal[i];
        if (i>25   && i<=27  ) bandValues[20]   += (int)vReal[i];
        if (i>27   && i<=29  ) bandValues[10]  += (int)vReal[i];
        if (i>29   && i<=31  ) bandValues[21]  += (int)vReal[i];
        if (i>31   && i<=33  ) bandValues[9]  += (int)vReal[i];
        if (i>33   && i<=35  ) bandValues[22]  += (int)vReal[i];
        if (i>35   && i<=38  ) bandValues[8]  += (int)vReal[i];
        if (i>38   && i<=41  ) bandValues[23]  += (int)vReal[i];
        if (i>41   && i<=44  ) bandValues[7]  += (int)vReal[i];
        if (i>44   && i<=47  ) bandValues[24]  += (int)vReal[i];
        if (i>47   && i<=50  ) bandValues[6]  += (int)vReal[i];
        if (i>50   && i<=53  ) bandValues[25]  += (int)vReal[i];
        if (i>53   && i<=56  ) bandValues[5]  += (int)vReal[i];
        if (i>56   && i<=59  ) bandValues[26]  += (int)vReal[i];
        if (i>59   && i<=62  ) bandValues[4]  += (int)vReal[i];
        if (i>62   && i<=65  ) bandValues[27]  += (int)vReal[i];
        if (i>65   && i<=68  ) bandValues[3]  += (int)vReal[i];
        if (i>68   && i<=71  ) bandValues[28]  += (int)vReal[i];
        if (i>71   && i<=74  ) bandValues[2]  += (int)vReal[i];
        if (i>74   && i<=77  ) bandValues[29]  += (int)vReal[i];
        if (i>77   && i<=80  ) bandValues[1]  += (int)vReal[i];
        if (i>80   && i<=83  ) bandValues[30]  += (int)vReal[i];
        if (i>83   && i<=87  ) bandValues[0]  += (int)vReal[i];
        if (i>87   && i<=91  ) bandValues[31]  += (int)vReal[i];
      }
    }
  }
  // 寻找bandValues最大值并根据最大值计算倍率
  maxBandValue = 0;
  for(int i = 0; i < NUM_BANDS; i++){
    if(bandValues[i] > maxBandValue){
      maxBandValue = bandValues[i];
    }
    if(bandValues[i] > MATRIX_SIDE * AMPLITUDE){
      bandIndex[i] = 1;
    }else{
      bandIndex[i] = 0;
    }
  }
  // 将FFT数据处理为条形高度
  int color = 0;
  int r, g, b;
  for (byte band = 0; band < NUM_BANDS; band++) {
    // 根据倍率缩放条形图高度
    int barHeight;
    if(bandIndex[band] == 1){
      barHeight = (MATRIX_SIDE * bandValues[band]) / maxBandValue;
    }else{
      barHeight = bandValues[band] / AMPLITUDE;
    } 
    if (barHeight > MATRIX_SIDE) barHeight = MATRIX_SIDE;
    // 旧的高度值和新的高度值平均一下
    barHeight = ((oldBarHeights[band] * 1) + barHeight) / 2;
    // 如果条形的高度大于顶点高度，则调整顶点高度
    if (barHeight > peak[band]) {
      // 顶点最高只到第0行：peak 同时用作数组下标，取 8 会越界，且对应的像素在屏幕外
      peak[band] = min(MATRIX_SIDE - 1, barHeight);
    }
    // 绘制操作
    switch (rhythmPage){
      case RHYTHM_MODEL1:
        // 绘制条形
        matrix.drawFastVLine((MATRIX_WIDTH - 1 - band), (MATRIX_SIDE - barHeight), barHeight, hsv2rgb(color,100,100));
        color += 360 / (NUM_BANDS + 4);
        // 绘制顶点
        matrix.drawPixel((MATRIX_WIDTH - 1 - band), MATRIX_SIDE - peak[band] - 1, matrix.Color(150,150,150));
        break;  
      case RHYTHM_MODEL2:
        // 高度为0时不能往下走：下面循环体里会除以 barHeight
        if (barHeight < 1) break;
        // 绘制条形（矩阵行号是0~7，从第7行也就是最下面一行开始画）
        r = model2ColorArray[0][0];
        g = model2ColorArray[0][1];
        b = model2ColorArray[0][2];
        for (int y = MATRIX_SIDE - 1; y >= MATRIX_SIDE - barHeight; y--) {
          matrix.drawPixel((MATRIX_WIDTH - 1 - band), y, matrix.Color(r, g, b));
          r+=(model2ColorArray[1][0] - model2ColorArray[0][0]) / barHeight;
          g+=(model2ColorArray[1][1] - model2ColorArray[0][1]) / barHeight;
          b+=(model2ColorArray[1][2] - model2ColorArray[0][2]) / barHeight;
        }
        // 绘制顶点
        matrix.drawPixel((MATRIX_WIDTH - 1 - band), MATRIX_SIDE - peak[band] - 1, matrix.Color(150,150,150));
        break;
      case RHYTHM_MODEL3:
        // 绘制条形,此模式不绘制顶点（同样从第7行开始画，才用满8行高度）
        for (int y = MATRIX_SIDE - 1; y >= MATRIX_SIDE - barHeight; y--) {
          matrix.drawPixel((MATRIX_WIDTH - 1 - band), y, hsv2rgb(y * (255 / MATRIX_WIDTH / 5) + colorTime, 100, 100));
        }
        break;
      case RHYTHM_MODEL4:
        // 此模式下，只绘制顶点      
        matrix.drawPixel((MATRIX_WIDTH - 1 - band), (MATRIX_SIDE - peak[band] - 1), 
        matrix.Color(model4ColorArrar[peak[band]][0], model4ColorArrar[peak[band]][1], model4ColorArrar[peak[band]][2]));
        break;  
      default:
        break;
    }   
    // 将值记录到oldBarHeights
    oldBarHeights[band] = barHeight;
  }  
  // 70毫秒降低一次顶点
  if((millis() - peekDecayTime) >= 70){
    for (byte band = 0; band < NUM_BANDS; band++){
      if (peak[band] > 0) peak[band] -= 1;
    }
    colorTime++;
    peekDecayTime = millis();
  }
  // 10毫秒变换一次颜色
  if((millis() - changeColorTime) >= 10){
    colorTime++;
    changeColorTime = millis();
  }
  matrix.setBrightness(brightness);
  matrix.show();
}

// HSV转RGB格式
uint16_t hsv2rgb(uint16_t hue, uint8_t saturation, uint8_t value){
  uint8_t r, g, b;
  uint16_t h = (hue / 60) % 6;
  uint16_t F = 100 * hue / 60 - 100 * h;
  uint16_t P = value * (100 - saturation) / 100;
  uint16_t Q = value * (10000 - F * saturation) / 10000;
  uint16_t T = value * (10000 - saturation * (100 - F)) / 10000;
  switch (h){
    case 0:
        r = value;
        g = T;
        b = P;
        break;
    case 1:
        r = Q;
        g = value;
        b = P;
        break;
    case 2:
        r = P;
        g = value;
        b = T;
        break;
    case 3:
        r = P;
        g = Q;
        b = value;
        break;
    case 4:
        r = T;
        g = P;
        b = value;
        break;
    case 5:
        r = value;
        g = P;
        b = Q;
        break;
    default:
      return matrix.Color(255, 0, 0);
  }
  r = r * 255 / 100;
  g = g * 255 / 100;
  b = b * 255 / 100;
  return matrix.Color(r, g, b);
}

// 重新设置闹钟相关的临时数据
void resetTmpClockData(){
  tmpClockH = clockH;
  tmpClockM = clockM;
  tmpClockBellNum = clockBellNum;
  clockChoosed = CLOCK_H;
}

// 绘制闹钟页面
void drawClock(){
  // 清屏
  matrix.fillScreen(0);
  if(clockOpen){
    // 绘制时间
    matrix.setTextColor(mainColor);
    matrix.setCursor(2, 5);
    String h;
    if(tmpClockH < 10){
      h = "0" + String(tmpClockH);
    }else{
      h = String(tmpClockH);
    }
    String m;
    if(tmpClockM < 10){
      m = "0" + String(tmpClockM);
    }else{
      m = String(tmpClockM);
    }
    matrix.print(h + ":" + m);
    // 绘制闹钟图标
    matrix.drawBitmap(22, 1, bell, 8, 6, mainColor);
    // 绘制指示线
    if(clockChoosed == CLOCK_H){
      matrix.drawFastHLine(2, 7, 7, weekColor);
    }else if(clockChoosed == CLOCK_M){
      matrix.drawFastHLine(12, 7, 7, weekColor);
    }else if(clockChoosed == CLOCK_BELL){
      matrix.drawFastHLine(22, 7, 8, weekColor);
    }
  }else{
    matrix.drawBitmap(8, 1, bell, 8, 6, mainColor);
    matrix.setCursor(20, 6);
    matrix.setTextColor(matrix.Color(255, 0, 0));
    matrix.print("X");  
  } 
  matrix.setBrightness(brightness);
  matrix.show();
}

///////////////////////////////////// 贪吃蛇小游戏 /////////////////////////////////////
// 玩法：蛇朝当前方向前进（八个方向，走出屏幕后从另一边绕回来），
// 键1 顺时针转 45°，键2 逆时针转 45°，吃到食物变长并加速，撞到自己就结束。
// 键3 短按退出游戏，长按键1或键2 重开一局。
int gameState = GAME_STATE_OVER; // 当前游戏状态
int gameScore = 0; // 吃掉的食物个数
// 蛇的行进方向：0=向右，每 +1 顺时针转 45°（屏幕坐标 y 轴向下）
//        0右  1右下  2下  3左下  4左  5左上  6上  7右上
int gameDir = 0;
const int dirStepX[8] = {  1,  1,  0, -1, -1, -1,  0,  1};
const int dirStepY[8] = {  0,  1,  1,  1,  0, -1, -1, -1};
int snakeX[GAME_MAX_LEN]; // 蛇身坐标，下标0是蛇头
int snakeY[GAME_MAX_LEN];
int snakeLen = 0; // 当前蛇长
int gameFoodX = 0; // 食物坐标
int gameFoodY = 0;
unsigned long gameTickTime = 0; // 上一次走格的时间
unsigned long gameTickInterval = 260; // 走一格的间隔，吃得越多越快

// 在没被蛇占据的位置里随机放一个食物
void spawnFood(){
  int freeCount = GAME_COLS * GAME_ROWS - snakeLen;
  if(freeCount <= 0) return;
  int index = random(0, freeCount); // 先随机抽第几个空格子，再找到它
  for(int y = 0; y < GAME_ROWS; y++){
    for(int x = 0; x < GAME_COLS; x++){
      bool occupied = false;
      for(int i = 0; i < snakeLen; i++){
        if(snakeX[i] == x && snakeY[i] == y){
          occupied = true;
          break;
        }
      }
      if(occupied) continue;
      if(index == 0){
        gameFoodX = x;
        gameFoodY = y;
        return;
      }
      index--;
    }
  }
}

// 绘制一帧游戏画面
void drawGameFrame(){
  matrix.setBrightness(brightness);
  matrix.fillScreen(0);
  // 食物
  matrix.drawPixel(gameFoodX, gameFoodY, matrix.Color(255, 0, 0));
  // 蛇身：蛇头白色，身体绿色并逐节变暗
  for(int i = snakeLen - 1; i >= 0; i--){
    uint16_t color;
    if(i == 0){
      color = matrix.Color(255, 255, 255);
    }else{
      int g = 255 - i * 3;
      if(g < 60) g = 60;
      color = matrix.Color(0, g, 90);
    }
    matrix.drawPixel(snakeX[i], snakeY[i], color);
  }
  matrix.show();
}

// 撞到自己：响两声下行的音，并显示 "OVER + 得分"，画面保持到玩家操作
void gameOver(){
  gameState = GAME_STATE_OVER;
  tone(BUZZER, 500, 150);
  delay(170);
  tone(BUZZER, 300, 260);
  delay(280);
  noTone(BUZZER);
  matrix.setBrightness(brightness);
  matrix.setFont(&MyFont);
  matrix.fillScreen(0);
  matrix.setTextColor(mainColor);
  matrix.setCursor(2, 5);
  matrix.print("OVER");
  matrix.setCursor(22, 5);
  matrix.print(String(gameScore));
  matrix.show();
}

// 开始新的一局
void gameStart(){
  randomSeed(millis()); // 每次开局换一套食物位置
  snakeLen = 3;
  for(int i = 0; i < snakeLen; i++){
    snakeX[i] = snakeLen - 1 - i; // 蛇头在最右，身体向左排开
    snakeY[i] = GAME_ROWS / 2;
  }
  gameDir = 0; // 开局朝右
  gameScore = 0;
  gameTickInterval = 260;
  gameTickTime = millis();
  gameState = GAME_STATE_RUN;
  spawnFood();
  drawGameFrame();
  tone(BUZZER, 1200, 80);
}

// 转向：delta = 1 顺时针 45°，delta = -1 逆时针 45°
void gameTurn(int delta){
  if(gameState != GAME_STATE_RUN) return;
  gameDir = (gameDir + delta + 8) % 8;
}

// 每 gameTickInterval 走一格，由主循环反复调用
void drawGame(){
  if(gameState != GAME_STATE_RUN) return; // 结束画面保持不动
  if(gameTickTime <= millis() && (millis() - gameTickTime) < gameTickInterval) return;
  gameTickTime = millis();
  // 计算新蛇头：按当前方向走一格，四个边都是环绕穿越
  int newX = snakeX[0] + dirStepX[gameDir];
  int newY = snakeY[0] + dirStepY[gameDir];
  if(newX < 0) newX = GAME_COLS - 1;
  if(newX >= GAME_COLS) newX = 0;
  if(newY < 0) newY = GAME_ROWS - 1;
  if(newY >= GAME_ROWS) newY = 0;
  bool ate = (newX == gameFoodX && newY == gameFoodY);
  // 撞到自己？吃到食物时尾巴不缩，所以要把最后一节也算上
  int checkLen = ate ? snakeLen : snakeLen - 1;
  for(int i = 0; i < checkLen; i++){
    if(snakeX[i] == newX && snakeY[i] == newY){
      gameOver();
      return;
    }
  }
  // 整体前移一格
  int newLen = ate ? min(snakeLen + 1, GAME_MAX_LEN) : snakeLen;
  for(int i = newLen - 1; i > 0; i--){
    snakeX[i] = snakeX[i - 1];
    snakeY[i] = snakeY[i - 1];
  }
  snakeX[0] = newX;
  snakeY[0] = newY;
  snakeLen = newLen;
  if(ate){
    gameScore++;
    gameTickInterval = 260 - min(150, gameScore * 6); // 越吃越快
    spawnFood();
    tone(BUZZER, 1500, 60);
  }
  drawGameFrame();
}
////////////////////////////////////////////////////////////////////////////////////////
