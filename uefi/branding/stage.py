#!/usr/bin/env python3

from pathlib import Path
from PIL import Image
import shutil
root=Path(__file__).resolve().parents[2]
mu=root/'work/ace6t-mu'
assert (mu/'.darwin-project-prepared').is_file()
rel=Path('Platforms/OnePlus/macanPkg/Applications/DarwinMenu')
shutil.copytree(root/'uefi'/rel,mu/rel,dirs_exist_ok=True)
p=mu/'Platforms/OnePlus/macanPkg/macan.dsc';s=p.read_text()
for key,value in [('PcdFirmwareVendor','L"HackIOS UEFI"'),('PcdFirmwareVersionString','L"1.0"')]:
 line='  gEfiMdeModulePkgTokenSpaceGuid.'+key+'|'+value+'\n'
 if line not in s:s=s.replace('[PcdsFixedAtBuild]\n','[PcdsFixedAtBuild]\n'+line,1)
p.write_text(s)
logo=Image.open(root/'uefi/branding/assets/logo.png').convert('RGB')
logo.resize((240,240),Image.Resampling.LANCZOS).save(mu/'Silicon/Silicium/SiliciumPkg/Include/Resources/BootLogo.bmp')
print('HackIOS UEFI staged:',mu)
