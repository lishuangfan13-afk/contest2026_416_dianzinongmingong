#!/usr/bin/env python3
"""Windows one-click entry: provide network and request board-owned voice once."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
STATE=HERE.parents[1]/'out/pc_link'
# Covers the board's 90-second quit drain, bounded serial cleanup, outstanding
# control threads and WSL gateway cleanup. Confirmation still ends it early.
STOP_SESSION_WAIT_SECONDS=240


def choose_port(ports,requested=None):
    if requested:
        match=next((p for p in ports if p.device.upper()==requested.upper()),None)
        if not match:raise RuntimeError('没有找到指定串口 '+requested+'，请确认 USB2UART 数据线已连接。')
        return match.device
    candidates=[p for p in ports if p.vid==0x0403 and p.pid==0x6001]
    if len(candidates)==1:return candidates[0].device
    if not candidates:raise RuntimeError('没有找到开发板的 FTDI 串口，请连接 USB2UART 数据线。')
    raise RuntimeError('找到多个 FTDI 串口，请用 --port 指定：'+', '.join(p.device for p in candidates))


def preflight(requested=None):
    if os.name!='nt':raise RuntimeError('请在连接开发板的 Windows 电脑上运行。')
    try:
        from serial.tools import list_ports
    except ImportError:
        raise RuntimeError('缺少串口组件。请先运行：'+sys.executable+' -m pip install pyserial') from None
    port=choose_port(list(list_ports.comports()),requested)
    if not shutil.which('wsl'):raise RuntimeError('未找到 WSL；需要已准备好的 Ubuntu-22.04 网络环境。')
    script="import json,os,shutil; from pathlib import Path; print(json.dumps({'root':os.geteuid()==0,'tools':{x:bool(shutil.which(x)) for x in ['pppd','ip','iptables']},'ppp_device':Path('/dev/ppp').exists(),'tun_device':Path('/dev/net/tun').exists()}))"
    try:
        p=subprocess.run(['wsl','-d','Ubuntu-22.04','--','python3','-c',script],
                         capture_output=True,timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
        info=json.loads(p.stdout) if p.returncode==0 else None
    except (OSError,ValueError,subprocess.TimeoutExpired):info=None
    if not info:raise RuntimeError('Ubuntu-22.04 未能完成环境检查；请确认 WSL 和其中的 Python 3 可运行。')
    if not info['root']:raise RuntimeError('当前 WSL 用户不能配置网络转发，请先按电脑网络共享说明准备 root 网络环境。')
    missing=[k for k,v in info['tools'].items() if not v]
    if missing:raise RuntimeError('WSL 缺少 '+', '.join(missing)+'；请先完成电脑网络共享环境准备。')
    if not info['ppp_device'] or not info['tun_device']:
        raise RuntimeError('WSL 没有可用的 PPP/TUN 设备，请先完成电脑网络共享环境准备。')
    return port


def process_running(pid):
    """Read process liveness; never send a Windows process-control signal."""
    if not isinstance(pid,int) or isinstance(pid,bool) or pid<=0:return None
    if os.name=='nt':
        import ctypes
        from ctypes import wintypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
        kernel.OpenProcess.restype=wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]
        kernel.GetExitCodeProcess.restype=wintypes.BOOL
        kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        kernel.CloseHandle.restype=wintypes.BOOL
        handle=kernel.OpenProcess(0x1000,False,pid)
        if not handle:return False if ctypes.get_last_error()==87 else None
        try:
            result=wintypes.DWORD()
            return result.value==259 if kernel.GetExitCodeProcess(handle,ctypes.byref(result)) else None
        finally:kernel.CloseHandle(handle)
    try:os.kill(pid,0)
    except ProcessLookupError:return False
    except PermissionError:return None
    else:return True


def read_status_once(path):
    if os.name!='nt':return json.loads(path.read_text(encoding='utf-8'))
    # FILE_SHARE_DELETE lets the writer replace a snapshot while this reader
    # finishes reading the previous complete snapshot on Windows.
    import ctypes
    import msvcrt
    from ctypes import wintypes
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,
                                wintypes.LPVOID,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
    kernel.CreateFileW.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    kernel.CloseHandle.restype=wintypes.BOOL
    handle=kernel.CreateFileW(str(path),0x80000000,7,None,3,0x80,None)
    if handle==wintypes.HANDLE(-1).value:raise ctypes.WinError(ctypes.get_last_error())
    try:descriptor=msvcrt.open_osfhandle(handle,os.O_RDONLY)
    except BaseException:
        kernel.CloseHandle(handle);raise
    with os.fdopen(descriptor,'r',encoding='utf-8') as stream:return json.load(stream)


def read_status(path):
    # Windows can briefly deny an open while the writer atomically replaces
    # this document. Retry that overlap instead of declaring no session.
    for attempt in range(8):
        try:return read_status_once(path)
        except (OSError,ValueError):
            if attempt==7:raise
            time.sleep(.01)


def active_sessions(state_dir):
    active=[]
    if state_dir.exists():
        for status_path in state_dir.glob('*/status.json'):
            try:status=read_status(status_path)
            except (OSError,ValueError):
                raise RuntimeError('供网会话状态暂时无法读取，请稍后重试并检查原启动窗口。') from None
            if not isinstance(status,dict):
                raise RuntimeError('供网会话状态格式异常，请查看原启动窗口和状态记录。')
            if status.get('phase') in ['stopped','failed']:continue
            if process_running(status.get('forwarder_pid')) is False:continue
            active.append(status_path)
    return active


def stop_session(state_dir=STATE,wait_seconds=STOP_SESSION_WAIT_SECONDS):
    active=active_sessions(state_dir)
    if not active:
        print('当前没有正在运行的朝夕供网会话。');return 0
    if len(active)>1:
        raise RuntimeError('检测到多个未结束的会话，请先查看各启动窗口；此次没有停止其他会话。')
    status_path=active[0]
    (status_path.parent/'stop.request').write_text('user requested stop\n',encoding='utf-8')
    print('已请求结束，正在等待开发板返回控制台和电脑网络清理……',flush=True)
    deadline=time.monotonic()+wait_seconds
    while time.monotonic()<deadline:
        try:status=read_status(status_path)
        except (OSError,ValueError):time.sleep(.2);continue
        if status.get('phase')=='stopped':
            print('会话已正常结束，可以关闭启动窗口。');return 0
        if status.get('phase')=='failed':
            raise RuntimeError('会话已结束但有清理或连接错误，请查看启动窗口和本机状态记录。')
        time.sleep(.2)
    raise RuntimeError('尚未收到结束确认，请保留启动窗口并查看其中的提示。')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--duration',type=int,default=3600)
    parser.add_argument('--state-dir',type=Path,default=STATE)
    parser.add_argument('--check-only',action='store_true',help='Inspect the environment without opening the board serial port')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--auto-voice',action='store_true',help='Enable automatic wake listening (default)')
    mode.add_argument('--manual-voice','--network-only',dest='manual_voice',action='store_true',help='Stop board automatic listening after control is verified; keep IP forwarding')
    parser.add_argument('--stop',action='store_true')
    args=parser.parse_args()
    try:
        if args.stop:return stop_session(args.state_dir)
        if not 60<=args.duration<=3600:raise RuntimeError('会话时长需要在 60 到 3600 秒之间。')
        port=preflight(args.port)
        print('环境检查通过，开发板串口：'+port,flush=True)
        if args.check_only:
            print('这里只检查了环境，没有打开串口，也没有开始录音。');return 0
        if active_sessions(args.state_dir):
            raise RuntimeError('已有朝夕供网会话，请保留原窗口；要切换模式，先使用停止入口结束原会话。')
        command=[sys.executable,str(HERE/'start.py'),'--port',port,
                 '--duration',str(args.duration),'--state-dir',str(args.state_dir)]
        command.append('--manual-voice' if args.manual_voice else '--auto-voice')
        print('已选择：'+('联网后停止板端自动监听。' if args.manual_voice else '板端自动检测唤醒词。'),flush=True)
        print('正在建立电脑供网。请保留此窗口；结束时双击“停止朝夕语音.cmd”。',flush=True)
        return subprocess.call(command,cwd=HERE.parents[1])
    except (OSError,RuntimeError) as error:
        print('未能完成：'+str(error),file=sys.stderr,flush=True);return 1
    except KeyboardInterrupt:
        print('启动窗口收到中断；请查看供网程序的退出和清理结果。',file=sys.stderr);return 130


if __name__=='__main__':raise SystemExit(main())
