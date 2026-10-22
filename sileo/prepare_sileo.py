#!/usr/bin/env python3

from pathlib import Path, PurePosixPath
import hashlib,io,json,subprocess,tarfile,shutil,plistlib
root=Path(__file__).resolve().parents[1]
assets=root/'artifacts/ace6t/sileo';stage=root/'work/sileo-root'
if stage.exists(): raise RuntimeError('Use the existing staged root; refusing to reset it')
shutil.copytree(root/'userland/root',stage,symlinks=True)
entries={}
def ingest(data):
    with tarfile.open(fileobj=io.BytesIO(data),mode='r:') as tar:
        for member in tar:
            rel=PurePosixPath(member.name.removeprefix('./'))
            if rel.parts[:2]!=('var','jb'): continue
            if any(x in member.name for x in ['\t','\n']) or '..' in rel.parts: raise ValueError('Unsafe package path')
            if member.isdir(): kind='d';body=None
            elif member.issym():
                if any(x in member.linkname for x in ['\t','\n']): raise ValueError('Unsafe link')
                kind='l';body=member.linkname
            elif member.isfile() or member.islnk():kind='f';body=tar.extractfile(member).read()
            else: continue
            entries[str(rel)]=(kind,member.mode,member.uid,member.gid,body)
for metadata,name in [('bootstrap-provenance.json','bootstrap-iphoneos-arm64.tar.zst'),('package-provenance.json','org.coolstar.sileo_2.5.1_iphoneos-arm64.deb')]:
    data=(assets/name).read_bytes();provenance=json.loads((assets/metadata).read_text())
    assert hashlib.sha256(data).hexdigest()==provenance['sha256']
ingest(subprocess.check_output(['zstd','-dc',str(assets/'bootstrap-iphoneos-arm64.tar.zst')]))
ingest(subprocess.check_output(['xz','-dc'],input=subprocess.check_output(['ar','p',str(assets/'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'),'data.tar.xz'])))
files=stage/'bootstrap/files';files.mkdir(parents=True)
manifest=[];count=0
for name,(kind,mode,uid,gid,body) in sorted(entries.items(),key=lambda kv:({'d':0,'f':1,'l':2}[kv[1][0]],len(PurePosixPath(kv[0]).parts),kv[0])):
    destination='/private/'+name
    line=[kind,f'{mode:o}',str(uid),str(gid),destination]
    if kind=='f':
        count+=1;(files/str(count)).write_bytes(body);line.append('/redwood/bootstrap/files/'+str(count))
    elif kind=='l':line.append(body)
    manifest.append('\t'.join(line))
manifest.append('\t'.join(['f','644','0','0','/private/var/jb/.installed_dopamine','/redwood/bootstrap/files/1']))
(stage/'bootstrap/manifest').write_text('\n'.join(manifest)+'\n')
for name in ('bootstrap-copy','sileo-session','open-sileo','persona-init'):
    shutil.copy2(assets/name,stage/'bin'/name)
(stage/'etc/rc.jobs').write_text('/redwood/bin/sileo-session\n')
(stage/'etc/rc').write_text('/redwood/bin/bootstrap-copy\n/redwood/bin/persona-init\nexec /sbin/launchd.ios\n')
job=stage/'LaunchDaemons/com.redwood.demo.plist'
settings=plistlib.loads(job.read_bytes());settings['ProgramArguments']=['/redwood/bin/sileo-session']
job.write_bytes(plistlib.dumps(settings))
info=plistlib.loads(entries['var/jb/Applications/Sileo.app/Info.plist'][4]);print('Sileo bundle:',info['CFBundleIdentifier'])
(assets/'staging.json').write_text(json.dumps({'entries':len(entries),'files':count,'bundle_id':info['CFBundleIdentifier'],'scope':'RAM-only /private/var/jb, no Android writes','stage':str(stage)},indent=2)+'\n')
print('Staged the Sileo root in',stage)
