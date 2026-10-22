#!/usr/bin/env python3

import argparse
import re
import datetime
from pathlib import Path
import subprocess
import sys
import time

root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description='')
ap.add_argument('--no-wifi',dest='wifi',action='store_false',help='The native Wi-Fi driver (on by default; --no-wifi to leave it off); needs raw fw/ in the root image.')
ap.add_argument('--wifi-native',action='store_true',help='Use native 802.11 TX rather than Ethernet offload for diagnosis.')
ap.add_argument('--wifi-rx-single',action='store_true',help='Route receive traffic to the peer REO queue without hash spreading.')
ap.add_argument('--wifi-rx-native',action='store_true',help='Receive native 802.11 frames instead of Ethernet decapsulation offload.')
ap.add_argument('--wifi-open-test',action='store_true',help='Explicitly test an open access point; refuses protected access points.')
ap.add_argument('--wifi-arg',action='append',help='Extra single-digit Wi-Fi diagnostic boot-arg, e.g. wifi_reauth=1.')
ap.add_argument('--wifi-legacy',action='store_true',help='Associate without HT/VHT so the access point uses 802.11a/g rates (diagnosis).')
ap.add_argument('--wifi-ba',action='store_true',help='Handle access point Block Ack requests for receive aggregation.')
ap.add_argument('--wifi-loud',action='store_true',help='Include native Wi-Fi driver diagnostics in kernel log.')
ap.add_argument('--wifi-width',choices=['auto','20'],default='auto',help='Limit association to 20 MHz for diagnosis.')
ap.add_argument('--touch',choices=['off','probe','on'],default='on',help='Opt-in Synaptics probe or experimental report driver.')
ap.add_argument('--fbscale',type=int,choices=[1,2,3],default=3,help='Render scale; 2 reduces software rendering load at lower sharpness.')
ap.add_argument('--verbose',action='store_true',help='Show diagnostic kernel console over the UI.')
ap.add_argument('--reuse-system',action='store_true',help='Use system.packfs already uploaded in this same loader session.')
ap.add_argument('--root-image',type=Path,default=root/'work/images/root.packfs')
ap.add_argument('--system-image',type=Path,default=root/'work/images/system.packfs')
ap.add_argument('--smoke',action='store_true',help='Raw syscall PID 1 test, expected to exit and panic.')
ap.add_argument('--no-gpu',dest='gpu',action='store_false',help='Ace 6T: wake the Adreno (steps 1-18) and keep it for the system, CoreAnimation drawing with it (on by default; --no-gpu: the processor draws). Needs a fresh UEFI session: a second GPU start in one does not come up.')
ap.add_argument('--cpufreq',type=int,choices=[0,1,2,3],default=0,help='Ace 6T core speed over SCMI: 1 log the levels, 2 raise to the top level, 3 also list every level.')
ap.add_argument('--adsp',type=int,choices=[0,1,2],default=0,help='ADSP stage 1: 0 off (default), 1 boot/link/SPF and keep running, 2 then stop; firmware in system.packfs /fw.')
ap.add_argument('--island',type=int,choices=range(4),default=0,help='Dynamic Island in SpringBoard: 0 off (default), 1 trace the gates, 2 on, 3 on with a test element.')
ap.add_argument('--adsp-vote',type=int,choices=[0,1],default=1,help='ADSP proxy votes: 1 LCX/LMX/XO/ICC (default), 0 skip all votes and retain bootloader state.')
ap.add_argument('--haptic',type=int,choices=range(6),default=5,help='AW86927: 0 off, 1 read, 2 reset, 3 load, 4 pulse, 5 iOS (default).')
ap.add_argument('--torch',type=int,choices=range(4),default=3,help='PM8550: 0 off, 1 read, 2 pulse, 3 iOS (default, initially dark).')
ap.add_argument('--seconds',type=int,default=0,help='Come back to the PC with the kernel log after N seconds (0, the default: iOS keeps running).')
ap.add_argument('--cpus',type=int,choices=range(1,9),default=8)
ap.add_argument('--kernel',type=Path,help='The kernel the UEFI image was built for (artifacts/ace6t/mach-macan-vN).')
args=ap.parse_args()
def quiet(kind,error,trace):
    if issubclass(kind,KeyboardInterrupt): print('\nrun: stopped',file=sys.stderr); return
    if issubclass(kind,(RuntimeError,subprocess.CalledProcessError,OSError)): print(f'run: {error}',file=sys.stderr); return
    sys.__excepthook__(kind,error,trace)
sys.excepthook=quiet
if not 0<=args.seconds<=3600: ap.error('seconds must be 0..3600 (0: no limit, iOS keeps running)')
logdir=root/'work'/'logs'/('ios-run-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
logdir.mkdir(parents=True)
sys.path.insert(0,str(Path(__file__).resolve().parent))
from hackios_tool import find_tool
base=[find_tool('fastboot'),'-s','xnuloader']
def log(text):
    with (logdir/'host.log').open('a',encoding='utf-8',errors='replace') as f: f.write(text)
def fb(command,timeout=30,live=False):
    import threading
    started=time.time()
    log(time.strftime('%H:%M:%S ')+' '.join(command)+'\n')
    process=subprocess.Popen(base+command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,stdin=subprocess.DEVNULL)
    chunks=[]
    def reader():
        while True:
            block=process.stdout.read1(4096) if hasattr(process.stdout,'read1') else process.stdout.read(1)
            if not block: break
            chunks.append(block)
            if live: sys.stdout.write(block.decode(errors='replace')); sys.stdout.flush()
    thread=threading.Thread(target=reader,daemon=True); thread.start()
    shown=started
    while process.poll() is None:
        if time.time()-started>timeout:
            process.kill()
            log('TIMEOUT after %d s\n'%timeout)
            raise RuntimeError(f'USB did not answer in {timeout} s ({" ".join(command[:1])}); no automatic reboot was attempted. Logs: {logdir}')
        if live and time.time()-shown>=30:
            shown=time.time()
            print(f'\n  ... still sending, {int(shown-started)} s', flush=True)
        time.sleep(0.2)
    thread.join(5)
    text=b''.join(chunks).decode(errors='replace')
    log(text+'\n(%.1f s, exit %d)\n'%(time.time()-started,process.returncode))
    if not live: print(text,flush=True,end='' if text.endswith('\n') else '\n')
    if process.returncode: raise RuntimeError(text.strip() or f'fastboot {command[0]} failed')
    return text
try: product=fb(['getvar','product'],10)
except RuntimeError: raise RuntimeError('the iOS loader does not answer over USB: start it with --boot (logs: '+str(logdir)+')') from None
if 'product: xnuloader' not in product: raise RuntimeError('the phone is not in the iOS loader: run --boot')
mac_file=root/'work'/'wifi-mac'
if not mac_file.is_file():
    import secrets
    mac_file.write_text('02:'+':'.join(f'{b:02x}' for b in secrets.token_bytes(5))+'\n')
mac=mac_file.read_text().strip()
if not re.fullmatch(r'([0-9a-f]{2}:){5}[0-9a-f]{2}',mac): raise RuntimeError(f'{mac_file} is not a MAC address')
kernel=args.kernel or root/'work/kit/kernel'
ramdisk=root/'userland/obj/hello' if args.smoke else args.root_image.resolve()
touch_arg={'off':'','probe':' macan_touch_probe=1','on':' macan_touch=1'}[args.touch]
command=f'cpus={args.cpus} debug=0x8'+(f' loader_return_after={args.seconds}' if args.seconds else '')+f' epoch={int(time.time())} sb_delay=8 fbscale={args.fbscale}'+(' -v' if args.verbose else '')+touch_arg+(f' wifi=1 wifi_mac={mac}' if args.wifi else '')+(' wifi_wide=0' if args.wifi and args.wifi_width=='20' else '')+(' wifi_loud=1' if args.wifi and args.wifi_loud else '')+(' wifi_native=1' if args.wifi and args.wifi_native else '')
command+=f' haptic={args.haptic} torch={args.torch} adsp={args.adsp} adsp_vote={args.adsp_vote} island={args.island}'
payload=logdir/'payload.bin'
if args.wifi and args.wifi_rx_single: command+=' wifi_rx_single=1'
if args.wifi and args.wifi_rx_native: command+=' wifi_rx_native=1'
if args.wifi and args.wifi_open_test: command+=' wifi_open_test=1'
if args.wifi and args.wifi_legacy: command+=' wifi_legacy=1'
if args.cpufreq: command+=f' macan_cpufreq={args.cpufreq}'
if args.gpu: command+=' macan_gpu=18 macan_gpu_keep=1 macan_gpu_pause=0'
for extra in args.wifi_arg or []:
    if not re.fullmatch(r'wifi_[a-z_]+=[0-9]',extra): ap.error('--wifi-arg takes wifi_NAME=DIGIT')
    command+=' '+extra
if args.wifi and args.wifi_ba: command+=' wifi_ba=1'
subprocess.run([sys.executable,str(root/'loader/mkpayload.py'),'-o',str(payload),f'kernel={kernel}',f'ramdisk={ramdisk}','--cmdline',command],check=True)
def megabytes(path): return f'{path.stat().st_size/1048576:,.0f} MB'
print(f'run: sending the kernel and the root image ({megabytes(payload)})',flush=True)
fb(['stage',str(payload)],300,live=True)
if not args.smoke and not args.reuse_system:
    system=args.system_image
    if not system.is_file(): raise RuntimeError('Missing system.packfs')
    fb(['oem','ramdisk'],30)
    print(f'run: sending the iOS system image ({megabytes(system)}); over USB 3 this takes about a minute, over USB 2 several',flush=True)
    fb(['stage',str(system)],1800,live=True)
print('run: starting iOS',flush=True)
fb(['oem','run','0'],10)
if args.seconds == 0:
    print('iOS started with no time limit; logs (if it comes back):',logdir,flush=True)
    raise SystemExit(0)
while True:
    result=fb(['getvar','result'],args.seconds+120)
    if 'result: running ' not in result: break
    time.sleep(0.25)
fb(['get_staged',str(logdir/'kernel.log')],60)
raw=(logdir/'kernel.log').read_bytes()
(logdir/'kernel-text.log').write_text(raw.replace(b'\0',b'').decode(errors='replace'))
print('Kernel returned. Logs:',logdir,flush=True)
