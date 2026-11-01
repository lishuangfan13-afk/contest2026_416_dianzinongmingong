// 自定义图案网页（由脚本从 MyFont.h 字模 + 模板生成，勿手改）
// 拆成两段：中间由 net.cpp 注入 var CUR="<当前图案 1024 位十六进制>";

// 自定义图案网页（由脚本从 MyFont.h 字模 + 模板生成，勿手改）
// 拆成两段：中间由 net.cpp 注入 var CUR="<当前图案 1024 位十六进制>";
const char DRAW_PAGE_1[] PROGMEM = R"rawliteral(<!DOCTYPE html><html lang='zh'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1.0'>
<title>EasyMatrix 自定义图案</title>
<style type='text/css'>
body{font-family:sans-serif;margin:0;padding:10px;background:#f2f2f2;color:#222}
h2{font-size:1.1rem;text-align:center;margin:4px 0 6px}
#dbg{text-align:center;font-size:0.8rem;color:#c0392b;min-height:1.1em;word-break:break-all}
#wrap{text-align:center;overflow-x:auto}
canvas{background:#5a5a5a;border-radius:6px;box-shadow:0 0 8px #999;touch-action:none;max-width:100%;height:auto;cursor:crosshair}
.row{margin:8px 0;text-align:center}
.sw{display:inline-block;width:30px;height:30px;margin:2px;border-radius:5px;border:3px solid #ddd;box-shadow:0 0 3px #777;cursor:pointer;vertical-align:middle}
.sw.on{border-color:#1e90ff;transform:scale(1.15)}
button{padding:9px 13px;margin:3px;border:none;border-radius:5px;background:#1e90ff;color:#fff;font-size:0.95rem;cursor:pointer}
button.g{background:#8a8a8a}
button.on{background:#e67e22}
input[type=text]{padding:8px;border:1px solid #ccc;border-radius:5px;width:120px;font-size:1rem}
#stat{text-align:center;color:#555;font-size:0.9rem;margin-top:4px}
#msg{text-align:center;margin-top:6px;color:#0a7d00;min-height:1.3em;font-size:0.95rem}
#tip{text-align:center;color:#777;font-size:0.85rem;margin-top:6px;line-height:1.5}
a{display:block;text-align:center;margin-top:12px;color:#1e90ff;text-decoration:none}
</style>
</head>
<body>
<div id='dbg'></div>
<h2>自定义图案 · 32 × 8</h2>
<div id='wrap'><canvas id='cv' width='768' height='192'></canvas></div>
<div id='stat'>载入中…</div>
<div class='row' id='pal'></div>
<div class='row'>
  <input type='color' id='cpick' value='#00ffff'>
  <button onclick='useCustom()'>用此颜色</button>
  <button id='ebtn' class='g' onclick='toggleErase()'>橡皮</button>
  <button class='g' onclick='clearAll()'>清空</button>
  <button class='g' onclick='invert()'>反色</button>
  <button class='g' onclick='fillAll()'>整屏填充</button>
</div>
<div class='row'>
  <input type='text' id='txt' value='YYX' maxlength='24'>
  <button onclick='stampText()'>贴上文字</button>
  <button class='g' onclick='shift(-1)'>&#8592;</button>
  <button class='g' onclick='shift(1)'>&#8594;</button>
</div>
<div class='row'>
  <button onclick='upload()'>上传到板子</button>
  <button class='g' onclick='loadCur()'>还原板子上的图案</button>
</div>
<div id='msg'></div>
<div id='tip'>拖动涂色 · 右键或"橡皮"擦除 · 每 8 列（对应一块 8×8 点阵）有分隔线</div>
<a href='/'>&#8592; 返回配置页</a>
<script type='text/plain' id='fontdata'>0,0,2,0,1;1,5,2,0,-4,e8;3,2,4,0,-4,b4;5,5,6,0,-4,57,d5,f5,00;3,6,4,0,-4,4e,3e,80;3,5,4,0,-4,a5,4a;4,5,5,0,-4,4a,5a,50;1,2,2,0,-4,c0;2,5,3,0,-4,6a,40;2,5,3,0,-4,95,80;3,3,4,0,-3,aa,80;3,3,4,0,-3,5d,00;2,2,3,0,0,60;3,1,4,0,-2,e0;1,1,2,0,0,80;3,5,4,0,-4,25,48;3,5,4,0,-4,f6,de;3,5,4,0,-4,59,2e;3,5,4,0,-4,e7,ce;3,5,4,0,-4,e7,9e;3,5,4,0,-4,b7,92;3,5,4,0,-4,f3,9e;3,5,4,0,-4,f3,de;3,5,4,0,-4,e4,92;3,5,4,0,-4,f7,de;3,5,4,0,-4,f7,9e;1,3,2,0,-3,a0;2,4,3,0,-3,46;2,3,3,0,-3,64;3,3,4,0,-3,e3,80;2,3,3,0,-3,98;3,5,4,0,-4,c5,04;3,5,4,0,-4,56,c6;4,5,5,0,-4,69,9f,90;4,5,5,0,-4,e9,a9,e0;4,5,5,0,-4,78,88,70;4,5,5,0,-4,e9,99,e0;3,5,4,0,-4,f3,ce;3,5,4,0,-4,f3,c8;4,5,5,0,-4,78,b9,70;3,5,4,0,-4,b7,da;3,5,4,0,-4,e9,2e;4,5,5,0,-4,11,19,60;4,5,5,0,-4,9a,ca,90;3,5,4,0,-4,92,4e;5,5,6,0,-4,8e,eb,18,80;4,5,5,0,-4,9d,db,90;4,5,5,0,-4,69,99,60;4,5,5,0,-4,e9,9e,80;4,5,5,0,-4,69,9b,70;4,5,5,0,-4,e9,9e,90;4,5,5,0,-4,78,61,e0;3,5,4,0,-4,e9,24;4,5,5,0,-4,99,99,60;3,5,4,0,-4,b6,d4;5,5,6,0,-4,8c,6b,55,00;5,5,6,0,-4,8a,88,a8,80;5,5,6,0,-4,8c,5c,42,00;5,5,6,0,-4,f8,88,8f,80;2,5,3,0,-4,ea,c0;3,5,4,0,-4,91,12;2,5,3,0,-4,d5,c0;3,2,4,0,-4,54;4,1,4,0,1,f0;2,2,3,0,-4,90;3,4,4,0,-3,c7,f0;3,5,4,0,-4,93,5e;3,3,4,0,-2,71,80;3,5,4,0,-4,25,de;3,4,4,0,-3,5e,30;2,5,3,0,-4,6e,80;3,5,4,0,-3,77,9c;3,5,4,0,-4,93,5a;1,5,2,0,-4,b8;2,6,3,0,-4,45,60;3,5,4,0,-4,92,ea;2,5,3,0,-4,aa,40;5,3,6,0,-2,d5,6a;3,3,4,0,-2,d6,80;3,3,4,0,-2,55,00;3,4,4,0,-2,d7,40;3,4,4,0,-2,75,90;2,3,3,0,-2,e8;3,4,4,0,-3,71,e0;2,5,3,0,-4,ba,40;3,3,4,0,-2,b5,80;3,3,4,0,-2,b5,00;5,3,6,0,-2,8d,54;3,3,4,0,-2,aa,80;3,4,4,0,-2,ac,e0;3,4,4,0,-3,e5,70;3,5,4,0,-4,6a,26;1,6,2,0,-4,fc;3,5,4,0,-4,c8,ac;4,2,5,0,-3,5a</script>
<script type='text/javascript'>
)rawliteral";

const char DRAW_PAGE_2[] PROGMEM = R"rawliteral(
var DBG=document.getElementById('dbg');
window.onerror=function(m,s,l,c){try{DBG.innerHTML='脚本错误: '+m+'（第 '+l+' 行）';}catch(e){}return false;};
function setDbg(s){try{DBG.textContent=s;}catch(e){}}
setDbg('1 开始');
var FD=document.getElementById('fontdata').textContent.replace(/\s/g,'');
var GLY=FD.split(';');
var gl=[];for(var i=0;i<GLY.length;i++){gl.push(GLY[i].split(',').map(Number));}
setDbg('2 字模 '+gl.length+' 个');
var W=32,H=8,SC=24;
var grid=[],cur='#00ffff',erase=false,tx=0,drawing=false;
for(var y0=0;y0<H;y0++){var row=[];for(var x0=0;x0<W;x0++){row.push(0);}grid.push(row);}
var cv=document.getElementById('cv');
var ctx=cv.getContext('2d');
setDbg('3 画布就绪');
var PAL=['#ffffff','#ff0000','#ff8000','#ffff00','#00ff00','#00ffff','#0080ff','#0000ff','#8000ff','#ff00ff','#ff0080','#404040'];
function hx2r(h){h=h.replace('#','');return [parseInt(h.substr(0,2),16),parseInt(h.substr(2,2),16),parseInt(h.substr(4,2),16)];}
function r2h(r,g,b){return '#'+[r,g,b].map(function(v){v=Math.max(0,Math.min(255,Math.round(v)));return ('0'+v.toString(16)).slice(-2);}).join('');}
function setPx(x,y,c){if(x>=0&&x<W&&y>=0&&y<H){grid[y][x]=(c===null||c===undefined)?0:c;}}
function render(){
  ctx.clearRect(0,0,cv.width,cv.height);
  for(var y=0;y<H;y++){for(var x=0;x<W;x++){
    ctx.fillStyle=grid[y][x]?grid[y][x]:'#1e1e1e';
    ctx.fillRect(x*SC+1,y*SC+1,SC-2,SC-2);
  }}
  ctx.fillStyle='#8a8a8a';
  for(var k=8;k<W;k+=8){ctx.fillRect(k*SC-1,0,2,cv.height);}
  var n=0;for(var yy=0;yy<H;yy++){for(var xx=0;xx<W;xx++){if(grid[yy][xx]){n++;}}}
  document.getElementById('stat').textContent='已点亮 '+n+' / 256 个像素';
}
function markSw(el){var a=document.getElementsByClassName('sw');for(var i=0;i<a.length;i++){a[i].className='sw';}if(el){el.className='sw on';}}
function pick(c,el){cur=c;erase=false;document.getElementById('ebtn').className='g';markSw(el);}
function useCustom(){pick(document.getElementById('cpick').value,null);}
function toggleErase(){erase=!erase;document.getElementById('ebtn').className=erase?'on':'g';}
function clearAll(){for(var y=0;y<H;y++){for(var x=0;x<W;x++){grid[y][x]=0;}}render();}
function invert(){for(var y=0;y<H;y++){for(var x=0;x<W;x++){grid[y][x]=grid[y][x]?0:cur;}}render();}
function fillAll(){for(var y=0;y<H;y++){for(var x=0;x<W;x++){grid[y][x]=erase?0:cur;}}render();}
function shift(d){tx+=d;if(tx<-20){tx=20;}if(tx>20){tx=-20;}document.getElementById('msg').textContent='文字起始列: '+tx+'（再点一次"贴上文字"生效）';}
function evPos(e){var r=cv.getBoundingClientRect();var sx=cv.width/r.width,sy=cv.height/r.height;return [Math.floor(((e.clientX-r.left)*sx)/SC),Math.floor(((e.clientY-r.top)*sy)/SC)];}
function paintAt(e,bErase){var p=evPos(e);setPx(p[0],p[1],(erase||bErase)?null:cur);render();}
function onDown(e){if(e.preventDefault){e.preventDefault();}drawing=true;if(cv.setPointerCapture&&e.pointerId!==undefined){try{cv.setPointerCapture(e.pointerId);}catch(err){}}paintAt(e,e.button===2);}
function onMove(e){if(!drawing){return;}if(e.preventDefault){e.preventDefault();}paintAt(e,false);}
function onUp(){drawing=false;}
if(window.PointerEvent){
  cv.addEventListener('pointerdown',onDown);
  cv.addEventListener('pointermove',onMove);
  window.addEventListener('pointerup',onUp);
  cv.addEventListener('pointercancel',onUp);
}else{
  cv.addEventListener('mousedown',onDown);
  cv.addEventListener('mousemove',onMove);
  window.addEventListener('mouseup',onUp);
  cv.addEventListener('touchstart',function(e){e.preventDefault();drawing=true;paintAt(e,false);});
  cv.addEventListener('touchmove',function(e){e.preventDefault();if(drawing){paintAt(e,false);}});
  cv.addEventListener('touchend',function(){drawing=false;});
}
cv.addEventListener('contextmenu',function(e){e.preventDefault();});
function stampText(){
  var s=document.getElementById('txt').value,x=tx;
  for(var i=0;i<s.length;i++){
    var code=s.charCodeAt(i);if(code<32||code>126){x+=2;continue;}
    var g=gl[code-32];if(!g||!g[0]){x+=2;continue;}
    var w=g[0],h=g[1],adv=g[2],xo=g[3],yo=g[4];
    for(var r=0;r<h;r++){for(var c=0;c<w;c++){
      var idx=r*w+c,b=g[5+(idx>>3)]||0;
      if((b>>(7-(idx&7)))&1){setPx(x+xo+c,5+yo+r,cur);}
    }}
    x+=adv;
  }
  render();
  document.getElementById('msg').textContent='已贴上文字（起始列 '+tx+'）';
}
function toHex(){
  var s='';
  for(var y=0;y<H;y++){for(var x=0;x<W;x++){
    var c=grid[y][x],v=0;
    if(c){var q=hx2r(c);v=((q[0]>>3)<<11)|((q[1]>>2)<<5)|(q[2]>>3);}
    s+=('000'+v.toString(16)).slice(-4);
  }}
  return s;
}
function fromHex(h){
  if(!h||h.length<W*H*4){return;}
  for(var y=0;y<H;y++){for(var x=0;x<W;x++){
    var v=parseInt(h.substr((y*W+x)*4,4),16);
    grid[y][x]=v?r2h(((v>>11)&31)*8,((v>>5)&63)*4,(v&31)*8):0;
  }}
  render();
}
function loadCur(){fromHex(CUR);document.getElementById('msg').textContent='已读回板子上的图案';}
function upload(){
  var m=document.getElementById('msg');m.textContent='上传中...';
  if(!window.fetch){m.textContent='这个浏览器太旧，不支持上传，请换 Chrome/Edge 或新一点的手机浏览器';return;}
  fetch('/drawdata',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pix='+toHex()})
   .then(function(r){return r.text();})
   .then(function(t){m.textContent=t;})
   .catch(function(){m.textContent='上传失败：请确认手机还连在 EasyMatrix 热点上';});
}
for(var p=0;p<PAL.length;p++){
  (function(c){
    var d=document.createElement('span');d.className='sw';d.style.background=c;
    d.onclick=function(){pick(c,d);};
    document.getElementById('pal').appendChild(d);
  })(PAL[p]);
}
setDbg('4 色板就绪');
fromHex(CUR);
render();
document.getElementById('tip').textContent=document.getElementById('tip').textContent+' · 脚本已就绪 ✓';
setDbg('');
</script>
</body>
</html>
)rawliteral";
