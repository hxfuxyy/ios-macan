#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil
import subprocess
import os
import zlib
import hashlib
import json

def main():
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description='')
    ap.add_argument('--mu', type=Path, default=root/'uefi'/'mu')
    ap.add_argument('--kernel', type=Path, help='Optional rebuilt GIC kernel: enable only CRC-matched early checkpoints.')
    args = ap.parse_args()
    mu = args.mu.resolve()
    if not (mu/'Platforms/OnePlus/macanPkg/macan.dsc').is_file():
        ap.error('Expected the UEFI tree (uefi/mu).')
    env=os.environ | {'XNU_PLATFORM':'macan'}
    if args.kernel:
        kernel=args.kernel.resolve()
        symbols=subprocess.check_output(['llvm-nm',str(kernel)],text=True)
        if '_gicr_stride' not in symbols: raise ValueError('Expected rebuilt kernel with GIC stride support.')
        data=kernel.read_bytes()
        generated=root/'artifacts/ace6t/generated'; generated.mkdir(parents=True, exist_ok=True)
        header=generated/'macan-kernel.h'
        header.write_text(f'#define MACAN_KERNEL_EXECUTION 1\n#define MACAN_KERNEL_SIZE {len(data)}u\n#define MACAN_KERNEL_CRC32 0x{zlib.crc32(data):08x}u\n')
        env['MACAN_KERNEL_HEADER']=str(header)
        (generated/'kernel-provenance.json').write_text(json.dumps({'path':str(kernel),'bytes':len(data),'crc32':f'{zlib.crc32(data):08x}','sha256':hashlib.sha256(data).hexdigest(),'allowed_checkpoints':[0,1,2,3,4,5,*range(10,22)],'note':'CRC pins the exact test image and detects corruption; it is not a security signature.'},indent=2)+'\n')
    subprocess.run(['bash', str(root/'loader/build.sh')], env=env, check=True)
    rel = Path('Platforms/OnePlus/macanPkg/Applications/XnuRamLoader')
    (mu/rel).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root/'loader/loader-macan.efi', mu/rel/'loader-macan.efi')
    app = '  macanPkg/Applications/XnuRamLoader/XnuRamLoader.inf\n'
    for name, marker, line in [('macan.dsc','[Components]\n',app),('macan.fdf','  # SmBios\n','  INF'+app[1:])]:
        path = mu/'Platforms/OnePlus/macanPkg'/name
        text = path.read_text()
        if 'Applications/XnuRamLoader/XnuRamLoader.inf' not in text:
            if text.count(marker)!=1: raise ValueError('Unexpected platform marker')
            path.write_text(text.replace(marker,marker+line,1))
    print('Gated macan loader staged in',mu)

if __name__=='__main__':
    main()
