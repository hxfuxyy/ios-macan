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
ap.add_argument('--wifi',action=argparse.BooleanOptionalAction,default=True,help='The native Wi-Fi driver (on by default; --no-wifi to leave it off); needs raw fw/ in the root image.')
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
ap.add_argument('--gpu',action=argparse.BooleanOptionalAction,default=True,help='Ace 6T: wake the Adreno (steps 1-18) and keep it for the system, CoreAnimation drawing with it (on by default; --no-gpu: the processor draws). Needs a fresh UEFI session: a second GPU start in one does not come up.')
ap.add_argument('--cpufreq',type=int,choices=[0,1,2,3],default=0,help='Ace 6T core speed over SCMI: 1 log the levels, 2 raise to the top level, 3 also list every level.')
ap.add_argument('--adsp',type=int,choices=[0,1,2],default=0,help='ADSP stage 1: 0 off (default), 1 boot/link/SPF and keep running, 2 then stop; firmware in system.packfs /fw.')
ap.add_argument('--island',type=int,choices=range(4),default=0,help='Dynamic Island in SpringBoard: 0 off (default), 1 trace the gates, 2 on, 3 on with a test element.')
ap.add_argument('--adsp-vote',type=int,choices=[0,1],default=1,help='ADSP proxy votes: 1 LCX/LMX/XO/ICC (default), 0 skip all votes and retain bootloader state.')
ap.add_argument('--haptic',type=int,choices=range(6),default=5,help='AW86927: 0 off, 1 read, 2 reset, 3 load, 4 pulse, 5 iOS (default).')
ap.add_argument('--torch',type=int,choices=range(4),default=3,help='PM8550: 0 off, 1 read, 2 pulse, 3 iOS (default, initially dark).')
ap.add_argument('--seconds',type=int,default=600)
ap.add_argument('--cpus',type=int,choices=range(1,9),default=8)
ap.add_argument('--kernel',type=Path,help='The kernel the UEFI image was built for (artifacts/ace6t/mach-macan-vN).')
args=ap.parse_args()
if not 0<=args.seconds<=3600: ap.error('seconds must be 0..3600 (0: no limit, iOS keeps running)')
logdir=root/'work'/'logs'/('ios-run-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
logdir.mkdir(parents=True)
sys.path.insert(0,str(Path(__file__).resolve().parent))
from hackios_tool import find_tool
base=[find_tool('fastboot'),'-s','xnuloader']
def fb(command,timeout=30):
    try:
        result=subprocess.run(base+command,capture_output=True,text=True,timeout=timeout)
    except subprocess.TimeoutExpired as error:
        with (logdir/'host.log').open('a') as f: f.write(' '.join(command)+'\nTIMEOUT\n')
        raise RuntimeError('USB did not return; no automatic reboot was attempted. Logs: '+str(logdir)) from error
    text=result.stdout+result.stderr
    with (logdir/'host.log').open('a') as f:f.write(' '.join(command)+'\n'+text+'\n')
    print(text,flush=True)
    if result.returncode: raise RuntimeError(text)
    return text
if 'product: xnuloader' not in fb(['getvar','product'],10): raise RuntimeError('Not the USB XNU loader')
kernel=args.kernel or root/'work/kit/kernel'
ramdisk=root/'userland/obj/hello' if args.smoke else args.root_image.resolve()
touch_arg={'off':'','probe':' macan_touch_probe=1','on':' macan_touch=1'}[args.touch]
command=f'cpus={args.cpus} debug=0x8'+(f' loader_return_after={args.seconds}' if args.seconds else '')+f' epoch={int(time.time())} sb_delay=8 fbscale={args.fbscale}'+(' -v' if args.verbose else '')+touch_arg+(' wifi=1 wifi_mac=02:3b:66:1a:ce:06' if args.wifi else '')+(' wifi_wide=0' if args.wifi and args.wifi_width=='20' else '')+(' wifi_loud=1' if args.wifi and args.wifi_loud else '')+(' wifi_native=1' if args.wifi and args.wifi_native else '')
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
fb(['stage',str(payload)],90)
if not args.smoke and not args.reuse_system:
    system=args.system_image
    if not system.is_file(): raise RuntimeError('Missing system.packfs')
    fb(['oem','ramdisk'],30)
    fb(['stage',str(system)],1800)
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
