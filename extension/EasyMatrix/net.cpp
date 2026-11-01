#include <HTTPClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include "common.h"
#include "preferencesUtil.h"
#include "light.h"
#include "task.h"
#include "emotion_display.h"

void sendNTPpacket(IPAddress &address);
void connectWiFi(int timeOut_s);
void startAP();
void startServer();
void scanWiFi();
void getHTML();
void handleNotFound();
void handleRoot();
void handleConfigWifi();
void handleDrawPage();
void handleDrawData();

// Wifi相关
String ssid;  //WIFI名称
String pass;  //WIFI密码
String WifiNames; // 根据搜索到的wifi生成的option字符串
String PassHTML; // 根据密码生成的HTML字符串
String RGBColors; // 根据查询到的NVS中的值生成的RGBinput字符串
String CityHTML; // 根据NVS中的天气位置生成的HTML字符串
bool apConfig; // 系统启动时是否需要配网
// SoftAP相关
const char *APssid = "EasyMatrix";
IPAddress staticIP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 254);
IPAddress subnet(255, 255, 255, 0);
WebServer server(80);
// 是否顺利连接上wifi
bool wifiConnected = false;
// 是否NTP对时成功
bool RTCSuccess = false;
// 天气相关（数据来源：Open-Meteo，免费且不需要 API key）
String city; // 天气位置：城市名 或 "纬度,经度"
int weatherCode = -1; // WMO 天气代码
float weatherTemp = 0; // 气温（℃）
bool weatherValid = false; // 是否已经取到过有效天气
float latitude = 0; // 解析出来的纬度
float longitude = 0; // 解析出来的经度
bool wifiBusy = false; // WiFi 互斥标志

// 开启SoftAP进行配网
void wifiConfigBySoftAP(){
  // 开启AP模式，如果开启失败，重启系统
  startAP();
  // 扫描WiFi,并将扫描到的WiFi组成option选项字符串
  scanWiFi();
  // 将从NVS中获取到的颜色信息组合成字符串,将wifi密码组合成字符串
  getHTML();
  // 启动服务器
  startServer();
  // 重新生成动画任务,提示用户配网（启动动画任务此前已被删除，先判空避免重复删除）
  if(showTextTask != NULL){
    extensionDisplayDeleteTask(showTextTask);
    showTextTask = NULL;
    delay(300);
  }
  createShowIpTask();
}

// 处理服务器请求
void doClient(){
  server.handleClient();
}
// 处理404情况的函数'handleNotFound'
void handleNotFound(){
  handleRoot();//访问不存在目录则返回配置页面
}
// 处理网站根目录的访问请求
void handleRoot(){
  server.send(200,"text/html", ROOT_HTML_PAGE1 + WifiNames + ROOT_HTML_PAGE2 + PassHTML + ROOT_HTML_PAGE3 + RGBColors + ROOT_HTML_PAGE4 + CityHTML + ROOT_HTML_PAGE5);
}
// 提交数据后的提示页面
void handleConfigWifi(){
  //判断是否有WiFi名称
  if (server.hasArg("ssid")){
    Serial.print("获得WiFi名称:");
    ssid = server.arg("ssid");
    Serial.println(ssid);
  }else{
    Serial.println("错误, 没有发现WiFi名称");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现WiFi名称");
    return;
  }
  //判断是否有WiFi密码
  if (server.hasArg("pass")){
    Serial.print("获得WiFi密码:");
    pass = server.arg("pass");
    Serial.println(pass);
  }else{
    Serial.println("错误, 没有发现WiFi密码");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现WiFi密码");
    return;
  }
  // 判断是否有RGB数据
  if (server.hasArg("red") && server.hasArg("green") && server.hasArg("blue")){
    Serial.print("获得RGB数据:");
    clockColor[0] = server.arg("red").toInt();
    clockColor[1] = server.arg("green").toInt();
    clockColor[2] = server.arg("blue").toInt();
    Serial.print(clockColor[0]);Serial.print(" ");Serial.print(clockColor[1]);Serial.print(" ");Serial.println(clockColor[2]);
  }else{
    Serial.println("错误, 没有发现RGB颜色数据");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现RGB颜色数据");
    return;
  }
  // 判断是否有天气位置（可选字段：城市名 或 纬度,经度）
  if (server.hasArg("city")){
    city = server.arg("city");
    city.trim();
    Serial.print("获得天气位置:");
    Serial.println(city);
  }
  // 将信息存入nvs中
  recordInfos(ssid, pass, clockColor[0], clockColor[1], clockColor[2], false);
  // 获得了所需要的一切信息，给客户端回复
  server.send(200, "text/html", "<meta charset='UTF-8'><style type='text/css'>body {font-size: 2rem;}</style><br/><br/>WiFi: " + ssid + "<br/>密码: " + pass + "<br/>RGB颜色: " + clockColor[0] + " " + clockColor[1] + " " + clockColor[2] + "<br/>天气位置: " + city + "<br/>已取得相关信息,正在尝试连接,请手动关闭此页面。");
  ESP.restart();
}

////////////////////////////////////// 自定义图案（网页画板） //////////////////////////////////////
// 画板地址：http://192.168.1.1/draw
// 上传格式：1024 个十六进制字符 = 256 个像素 × RGB565（每像素 4 位十六进制，高位在前）
// 打开画板页面：注入板子上当前的图案，方便在现有基础上改
void handleDrawPage(){
  String body = String(DRAW_PAGE_1);
  body += "var CUR=\"";
  char buf[8];
  for(int i = 0; i < LIGHTCOUNT; i++){
    sprintf(buf, "%04x", drawImage[i]);
    body += buf;
  }
  body += "\";\n";
  body += String(DRAW_PAGE_2);
  server.send(200, "text/html", body);
}
// 单个十六进制字符转数值，非法返回 -1
int hexValue(char c){
  if(c >= '0' && c <= '9'){
    return c - '0';
  }
  if(c >= 'a' && c <= 'f'){
    return c - 'a' + 10;
  }
  if(c >= 'A' && c <= 'F'){
    return c - 'A' + 10;
  }
  return -1;
}
// 接收画板上传的图案：存进 NVS，并立刻画到屏幕上当预览（不重启）
void handleDrawData(){
  if(!server.hasArg("pix")){
    server.send(200, "text/plain; charset=utf-8", "没收到图案数据");
    return;
  }
  String pix = server.arg("pix");
  if(pix.length() < LIGHTCOUNT * 4){
    server.send(200, "text/plain; charset=utf-8", "图案数据长度不对");
    return;
  }
  for(int i = 0; i < LIGHTCOUNT; i++){
    int h1 = hexValue(pix.charAt(i * 4));
    int h2 = hexValue(pix.charAt(i * 4 + 1));
    int h3 = hexValue(pix.charAt(i * 4 + 2));
    int h4 = hexValue(pix.charAt(i * 4 + 3));
    if(h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0){
      server.send(200, "text/plain; charset=utf-8", "图案数据里有非法字符");
      return;
    }
    drawImage[i] = (uint16_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
  }
  drawImageValid = true;
  recordDrawImage(); // 存进 NVS，断电重启也在
  Serial.println("已保存网页画板的图案");
  // 停掉滚动显示IP的任务，把图案直接画出来当预览
  if(showIpTask != NULL){
    extensionDisplayDeleteTask(showIpTask);
    showIpTask = NULL;
    delay(50);
  }
  matrix.setBrightness(brightness);
  matrix.drawRGBBitmap(0, 0, drawImage, MATRIX_WIDTH, MATRIX_SIDE);
  matrix.show();
  server.send(200, "text/plain; charset=utf-8", "已保存到板子！屏幕上应该已经显示出来了。按 键3 退出配网后，用 键3 翻页就能看到这一页。");
}

// 连接WiFi
void connectWiFi(int timeOut_s){
  int connectTime = 0; //用于连接计时，如果长时间连接不成功，则提示失败
  Serial.print("正在连接网络");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    connectTime++;
    if (connectTime > 2 * timeOut_s){ //长时间连接不上，提示连接失败
      Serial.println("网络连接失败...");
      // 停止启动加载动画（配网流程里可能已经删过，重复删除无效句柄会触发断言）
      if(showTextTask != NULL){
        extensionDisplayDeleteTask(showTextTask);
        showTextTask = NULL;
        delay(300);
      }
      // 屏幕显示wifi连接失败
      drawFailed(4, 24, "WIFI");
      delay(2000);
      // 清空屏幕
      clearMatrix();
      // 至节奏灯页面
      currentPage = RHYTHM;
      // 跳出循环
      return;
    }
  }
  wifiConnected = true;
  Serial.println("网络连接成功");
  Serial.print("本地IP： ");
  Serial.println(WiFi.localIP());
}

// 启动服务器
void startServer(){
  // 当浏览器请求服务器根目录(网站首页)时调用自定义函数handleRoot处理，设置主页回调函数，必须添加第二个参数HTTP_GET，否则无法强制门户
  server.on("/", HTTP_GET, handleRoot);
  // 当浏览器请求服务器/configwifi(表单字段)目录时调用自定义函数handleConfigWifi处理
  server.on("/configwifi", HTTP_POST, handleConfigWifi);
  // 自定义图案画板页面
  server.on("/draw", HTTP_GET, handleDrawPage);
  // 画板上传图案数据
  server.on("/drawdata", HTTP_POST, handleDrawData);
  // 当浏览器请求的网络资源无法在服务器找到时调用自定义函数handleNotFound处理   
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("服务器启动成功！");
}
// 开启AP模式，如果开启失败，重启系统
void startAP(){
  Serial.println("开启AP模式...");
  WiFi.enableAP(true); // 使能AP模式
  //传入参数静态IP地址,网关,掩码
  WiFi.softAPConfig(staticIP, gateway, subnet);
  if (!WiFi.softAP(APssid)) {
    Serial.println("AP模式启动失败");
    ESP.restart(); // Ap模式启动失败，重启系统
  }  
  Serial.println("AP模式启动成功");
  Serial.print("IP地址: ");
  Serial.println(WiFi.softAPIP());
}
// 扫描WiFi,并将扫描到的Wifi组成option选项字符串
void scanWiFi(){
  Serial.println("开始扫描WiFi");
  int n = WiFi.scanNetworks();
  if (n){
    Serial.print("扫描到");
    Serial.print(n);
    Serial.println("个WIFI");
    WifiNames = "";
    bool hasSavedSSID = false; // 记录是否扫描到已经存储在NVS中的wifi
    for (size_t i = 0; i < n; i++){
      int32_t rssi = WiFi.RSSI(i);
      String signalStrength;
      if(rssi >= -35){
        signalStrength = " (信号极强)";
      }else if(rssi >= -50){
        signalStrength = " (信号强)";
      }else if(rssi >= -70){
        signalStrength = " (信号中)";
      }else{
        signalStrength = " (信号弱)";
      }
      // 每个热点只输出一次；若与NVS中保存的SSID相同，则把它标记为选中项
      if(WiFi.SSID(i).equals(ssid) && !hasSavedSSID){
        hasSavedSSID = true;
        WifiNames += "<option selected value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + signalStrength + "</option>";
      }else{
        WifiNames += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + signalStrength + "</option>";
      }
    }
    if(!hasSavedSSID && !ssid.equals("")){ // 如果扫描到的wifi中，没有已经存储的wifi，则手动添加进去
      WifiNames += "<option selected value='" + ssid + "'>" + ssid + "</option>";
    }
  }else{
    Serial.println("没扫描到WIFI");
  }
}
// 将从NVS中获取到的颜色信息组合成字符串,将wifi密码组合成字符串
void getHTML(){
  RGBColors = "";
  RGBColors += "R<input type='number' placeholder='0-255' name='red' id='red' class='rgb' value='" + String(clockColor[0]) + "'>";
  RGBColors += "G<input type='number' placeholder='0-255' name='green' id='green'  class='rgb' value='" + String(clockColor[1]) + "'>";
  RGBColors += "B<input type='number' placeholder='0-255' name='blue' id='blue'  class='rgb' value='" + String(clockColor[2]) + "'>";
  PassHTML = "<input type='text' placeholder='请输入WiFi密码' name='pass' id='pass' class='passAndCity commonWidth' value='" + pass + "'>";
  CityHTML = "<input type='text' placeholder='如:上海 或 31.23,121.47（留空则不显示天气）' name='city' id='city' class='passAndCity commonWidth' value='" + city + "'>";
}
// 获取NTP并同步RTC时间
void getNTPTime(){
  // 8 * 3600 东八区时间修正
  // 使用夏令时 daylightOffset_sec 就填写3600，否则就填写0；
  Serial.println("执行NTP对时");
  configTime( 8 * 3600, 0, NTP1, NTP2, NTP3);
}

// 系统启动时进行NTP对时
void checkTime(int limitTime){
  // 用 millis 计时：RTC 尚未对上时间时 time() 会从0跳到当前时间，
  // 用墙钟差值判断超时会在对时成功的瞬间被误判成超时
  unsigned long start = millis();
  // 获取NTP并同步至RTC,第一次同步失败，就一直尝试同步
  getNTPTime();
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)){
    if((millis() - start) > (unsigned long)limitTime * 1000UL){
      Serial.println("时钟对时超时...");
      // RTCSuccess初始化时就是false，所以只需要在对时成功后赋值true就可以
      // 这样也可以在启动对时成功，之后每小时一次对时失败时，不会将RTCSuccess置为false，影响某些程序运行
      // RTCSuccess = false;
      // 停止启动加载动画（可能已经被删除过，先判空）
      if(showTextTask != NULL){
        extensionDisplayDeleteTask(showTextTask);
        showTextTask = NULL;
        delay(300);
      }
      // 屏幕显示TIME获取失败
      drawFailed(4, 24, "TIME");
      delay(2000);
      // 清空屏幕
      clearMatrix();
      // 至节奏灯页面
      currentPage = RHYTHM;
      // 跳出循环
      return;
    }
    Serial.println("时钟对时失败...");
    getNTPTime();
  }
  // 到了这一步，说明对时成功
  RTCSuccess = true;
}

// 关闭wifi
void disConnectWifi(){
  WiFi.disconnect();
}

// 定时对时
void checkTimeTicker(){
  // 天气任务可能正在用WiFi，先等它让出来（最多等30秒）
  int waitLock = 0;
  while(wifiBusy && waitLock < 60){
    delay(500);
    waitLock++;
  }
  if(wifiBusy){
    Serial.println("WiFi被天气任务占用，本次对时跳过");
    return;
  }
  wifiBusy = true;
  // 重新连接wifi
  Serial.println("重新连接网络...");
  int connectTime = 0; //连接计时
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    connectTime++;
    if (connectTime > 20){ //循环20次（10秒）连接不上，就退出
      Serial.println("网络连接失败...");
      wifiConnected = false;
      // 跳出循环
      break;
    }
  }
  // 连接成功就刷新状态：否则只要失败过一次，wifiConnected 会一直是 false，
  // 之后每次定时对时都会被跳过
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  // wifi连接成功，进行对时
  if(wifiConnected){
    // 只对一次，不管是否成功，因为启动时已经成功进行了对时，这次无关紧要
    getNTPTime();
    // 对时后如果闹钟开启，就重置闹钟时间
    if(clockOpen){
      tickerClock.detach();
      startTickerClock(getClockRemainSeconds());
    }
  }
  // 关闭wifi功能
  disConnectWifi();
  wifiBusy = false;
}

////////////////////////////////////// 天气（Open-Meteo） //////////////////////////////////////
// 免费且不需要 API key：按经纬度取当前气温与天气代码
// 把非 ASCII 字符（中文城市名）做百分号编码，才能拼进 URL
String urlEncode(const String &src){
  const char *hex = "0123456789ABCDEF";
  String out = "";
  for(size_t i = 0; i < src.length(); i++){
    uint8_t c = (uint8_t)src.charAt(i);
    if((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       c == '-' || c == '_' || c == '.' || c == '~'){
      out += (char)c;
    }else{
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}
// 发一个 HTTP GET，把响应体读进 body
// 这里用明文 HTTP：天气是公开数据、不含任何凭据，而换成 HTTPS 会把整套 mbedTLS
// 链接进来，固件体积要多出约 130KB（app 分区放不下），所以不划算。
bool httpGetText(const String &url, String &body){
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if(!http.begin(client, url)){
    Serial.println("HTTP 初始化失败");
    return false;
  }
  int code = http.GET();
  bool ok = (code == 200);
  if(ok){
    body = http.getString();
  }else{
    Serial.print("HTTP 请求失败，状态码：");
    Serial.println(code);
  }
  http.end();
  return ok;
}
// 把手填的"城市名"或"纬度,经度"解析成坐标
bool resolveLocation(){
  if(city.length() == 0){
    return false; // 没填位置就不显示天气
  }
  int comma = city.indexOf(',');
  if(comma > 0){ // 直接填了"纬度,经度"
    float lat = city.substring(0, comma).toFloat();
    float lon = city.substring(comma + 1).toFloat();
    if(lat != 0 || lon != 0){
      latitude = lat;
      longitude = lon;
      return true;
    }
  }
  if(latitude != 0 || longitude != 0){
    return true; // 之前已经解析成功过，不必每次都查
  }
  // 用 Open-Meteo 的地理编码接口把城市名换成坐标
  String url = "http://geocoding-api.open-meteo.com/v1/search?count=1&language=zh&format=json&name=" + urlEncode(city);
  String body;
  if(!httpGetText(url, body)){
    return false;
  }
  int li = body.indexOf("\"latitude\":");
  int oi = body.indexOf("\"longitude\":");
  if(li < 0 || oi < 0){
    Serial.println("城市名解析失败，请在配网页改填经纬度");
    return false;
  }
  latitude = body.substring(li + 11).toFloat();
  longitude = body.substring(oi + 12).toFloat();
  Serial.print("天气位置解析为坐标：");
  Serial.print(latitude, 4);
  Serial.print(", ");
  Serial.println(longitude, 4);
  return (latitude != 0 || longitude != 0);
}
// 取一次天气
bool fetchWeatherOnce(){
  if(!resolveLocation()){
    return false;
  }
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
               "&longitude=" + String(longitude, 4) +
               "&current=temperature_2m,weather_code&timezone=Asia%2FShanghai";
  String body;
  if(!httpGetText(url, body)){
    return false;
  }
  // 注意：响应里 current_units 排在 current 前面，且里面有同名字段
  // （"temperature_2m":"°C"），所以必须先从 "current": 处截断再找字段
  int ci = body.indexOf("\"current\":");
  if(ci < 0){
    Serial.println("天气响应格式不对");
    return false;
  }
  String cur = body.substring(ci);
  int ti = cur.indexOf("\"temperature_2m\":");
  if(ti < 0){
    return false;
  }
  float newTemp = cur.substring(ti + 17).toFloat();
  int wi = cur.indexOf("\"weather_code\":");
  int newCode = (wi >= 0) ? cur.substring(wi + 15).toInt() : weatherCode;
  // 只在数据有明显变化时写NVS，减少闪存擦写
  bool changed = (!weatherValid) || (newCode != weatherCode) ||
                 (newTemp - weatherTemp > 0.5f) || (weatherTemp - newTemp > 0.5f);
  weatherTemp = newTemp;
  weatherCode = newCode;
  weatherValid = true;
  if(changed){
    recordWeather();
  }
  Serial.print("天气更新：");
  Serial.print(weatherTemp);
  Serial.print("℃  代码 ");
  Serial.println(weatherCode);
  return true;
}
// 天气更新任务：每30分钟临时开一次WiFi取天气，取完就关掉
void startFetchWeather(void *param){
  vTaskDelay(60000); // 先等启动流程把NTP对时做完
  while(true){
    // 配网页面正在用AP模式，不能抢占WiFi
    if(currentPage != SETTING && !wifiBusy && ssid.length() > 0){
      wifiBusy = true;
      WiFi.begin(ssid, pass);
      int waitCount = 0;
      while(WiFi.status() != WL_CONNECTED && waitCount < 24){ // 最多等12秒
        vTaskDelay(500);
        waitCount++;
      }
      if(WiFi.status() == WL_CONNECTED){
        fetchWeatherOnce();
      }else{
        Serial.println("天气更新：WiFi连接失败");
      }
      WiFi.disconnect();
      wifiBusy = false;
    }
    vTaskDelay(WEATHER_INTERVAL);
  }
}
void startWeatherTask(){
  // TLS 握手需要较大的栈空间（ESP32 的 xTaskCreate 栈深单位是字节）
  xTaskCreate(startFetchWeather, "startFetchWeather", 10240, NULL, 1, NULL);
}



