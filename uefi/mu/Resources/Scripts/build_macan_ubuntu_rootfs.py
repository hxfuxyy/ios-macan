#!/usr/bin/env python3
"""Build a configured Ubuntu ARM64 GNOME payload on the PC, never on phone disks.

Requires bwrap, cc with static libc, and Linux >=6.7 for private binfmt_misc.
APT authenticates Ubuntu repository metadata. Base tarballs are SHA256 pinned.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[2]
VERSION="24.04.5"
BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/"
BASE_HASH={
    "amd64":"e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86",
    "arm64":"a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2",
}
PACKAGES="busybox-static weston seatd udev dbus-x11 gnome-shell gnome-session gnome-settings-daemon gnome-control-center nautilus gnome-terminal fonts-ubuntu ubuntu-settings ubuntu-wallpapers libgl1-mesa-dri mesa-utils"


def bwrap(root,extra=()):
    return ["bwrap","--unshare-user","--uid","0","--gid","0","--unshare-pid",
            "--die-with-parent","--bind",str(root),"/","--proc","/proc","--dev","/dev",
            "--ro-bind","/etc/resolv.conf","/etc/resolv.conf",*extra]


def mapped_bwrap(root,extra=()):
    # Standard rootless-container UID/GID mappings allow package scripts to
    # configure service users without changing any host account or binfmt rule.
    command=bwrap(root,extra)
    command.remove("--unshare-user")
    return ["unshare","--user","--map-root-user","--map-auto","--mount","--pid","--fork",*command]


def run(command,log):
    print(f"Running isolated build; log: {log}",flush=True)
    with log.open("wb") as stream:
        subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,check=True)


def finalize(directory):
    if os.geteuid()!=0:
        subprocess.run(["unshare","--user","--map-root-user","--map-auto","--mount",
                        "python3",str(Path(__file__).resolve()),"--out",str(directory),"--finalize-only"],check=True)
        return
    root=directory/"rootfs"
    status=(root/"var/lib/dpkg/status").read_text()
    records={}
    for paragraph in status.split("\n\n"):
        fields=dict(line.split(": ",1) for line in paragraph.splitlines() if ": " in line and not line.startswith(" "))
        if "Package" not in fields:
            continue
        if fields.get("Status") not in ("install ok installed","deinstall ok config-files"):
            raise ValueError(f"Unconfigured package: {fields['Package']} ({fields.get('Status')})")
        if fields.get("Status")=="install ok installed":
            records[fields["Package"]]=fields["Version"]
    for package in PACKAGES.split():
        if package not in records:
            raise ValueError(f"Missing package {package}")
    for src,dst in [("ace6t-gnome.sh","usr/local/sbin/ace6t-gnome"),
                    ("ace6t-gnome-session.sh","usr/local/sbin/ace6t-gnome-session")]:
        path=root/dst;path.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(ROOT/"Resources/LinuxRamTest"/src,path);path.chmod(0o755)
    # Generated state is temporary and belongs to this RAM session.
    (root/"etc/machine-id").write_text("")
    (root/"etc/hostname").write_text("ace6t-ubuntu\n")
    (root/"etc/hosts").write_text("127.0.0.1 localhost\n127.0.1.1 ace6t-ubuntu\n::1 localhost\n")
    # PID 1 is our diagnostic process; use a dedicated system bus, without
    # relying on setuid activation helpers on the nosuid RAM mount.
    (root/"etc/ace6t-system-bus.conf").write_text('''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type><user>root</user>
  <listen>unix:path=/run/dbus/system_bus_socket</listen><auth>EXTERNAL</auth>
  <standard_system_servicedirs/>
  <policy context="default"><allow user="*"/></policy>
  <policy user="root"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy>
  <includedir>/usr/share/dbus-1/system.d</includedir>
  <includedir>/etc/dbus-1/system.d</includedir>
</busconfig>
''')
    # GNOME dependencies include UDisks2; prevent D-Bus activation from gaining
    # a storage mounting service in this explicit no-disk RAM test.
    activation=root/"usr/share/dbus-1/system-services/org.freedesktop.UDisks2.service"
    if activation.exists():
        activation.write_text("[D-BUS Service]\nName=org.freedesktop.UDisks2\nExec=/bin/false\nUser=root\n")
    for relative in ("var/cache/apt/archives","var/lib/apt/lists","tmp","run"):
        path=root/relative
        for child in path.iterdir():
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child)
            else:
                child.unlink()
    # Relative equivalents keep extraction symlinks confined to the RAM root.
    # Walk without following symlinks; strip host owner IDs in archive metadata.
    for parent,dirs,files in os.walk(root):
        for name in dirs+files:
            path=Path(parent)/name
            if path.is_symlink() and os.readlink(path).startswith("/"):
                target=root/os.readlink(path).lstrip("/")
                new=os.path.relpath(target,path.parent);path.unlink();path.symlink_to(new)
    helper=root/"usr/bin/busybox"
    shutil.copyfile(helper,directory/"busybox-arm64");(directory/"busybox-arm64").chmod(0o755)
    package_record=directory/"packages.json";package_record.write_text(json.dumps(records,indent=2,sort_keys=True)+"\n")
    target=directory/"ubuntu-24.04-gnome-arm64.tar.gz"
    if target.exists():
        raise ValueError("Preserve previous rootfs: choose a new output directory")
    unpacked=0
    def metadata(info):
        nonlocal unpacked
        if info.isdev() or info.isfifo():
            raise ValueError(f"Unexpected special file in Ubuntu payload: {info.name}")
        # UID/GID are the actual Ubuntu IDs as viewed in the mapped namespace.
        info.uname=info.gname="";info.mtime=0
        if info.uid>65536 or info.gid>65536:
            raise ValueError(f"Unmapped owner in Ubuntu payload: {info.name}")
        if info.isreg(): unpacked+=info.size
        return info
    with target.open("wb") as raw,gzip.GzipFile(filename="",mode="wb",fileobj=raw,mtime=0,compresslevel=3) as zipped:
        with tarfile.open(fileobj=zipped,mode="w|",format=tarfile.GNU_FORMAT) as tar:
            tar.add(root,arcname=".",filter=metadata)
    if unpacked>3*1024**3 or target.stat().st_size>512*1024**2:
        target.unlink();raise ValueError("Rootfs exceeds phone RAM limits")
    sha=hashlib.file_digest(target.open("rb"),"sha256").hexdigest()
    record={"ubuntu_base":VERSION,"architecture":"arm64","desktop":"GNOME","packages":records,
            "bytes":target.stat().st_size,"unpacked_regular_bytes":unpacked,"sha256":sha,
            "busybox_sha256":hashlib.sha256(helper.read_bytes()).hexdigest(),
            "base_sha256":BASE_HASH,"source":BASE_URL,
            "on_device_gui":"pending", "storage":"temporary tmpfs; no phone disk mounts"}
    target.with_suffix(".manifest.json").write_text(json.dumps(record,indent=2)+"\n")
    target.with_suffix(".sha256").write_text(f"{sha}  {target.name}\n")
    print(f"Prepared {target} ({target.stat().st_size} bytes compressed, {unpacked} regular bytes in RAM)",flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out",type=Path,required=True)
    parser.add_argument("--finalize-only",action="store_true")
    args=parser.parse_args();directory=args.out.resolve();directory.mkdir(parents=True,exist_ok=True)
    if args.finalize_only:
        finalize(directory);return
    downloads=directory/"downloads";downloads.mkdir(exist_ok=True)
    for arch,dest in [("amd64","builder"),("arm64","rootfs")]:
        name=f"ubuntu-base-{VERSION}-base-{arch}.tar.gz";path=downloads/name
        if not path.exists():
            urllib.request.urlretrieve(BASE_URL+name,path)
        if hashlib.file_digest(path.open("rb"),"sha256").hexdigest()!=BASE_HASH[arch]:
            raise ValueError(f"Ubuntu Base checksum mismatch: {name}")
        root=directory/dest
        if not root.exists():
            root.mkdir()
            with tarfile.open(path) as tar:
                tar.extractall(root,filter="fully_trusted") # Only the exact pinned Canonical tarball.
    builder=directory/"builder";root=directory/"rootfs"
    run(bwrap(builder)+["/bin/sh","-c","export DEBIAN_FRONTEND=noninteractive; apt-get -o APT::Sandbox::User=root update && apt-get -o APT::Sandbox::User=root install -y --no-install-recommends qemu-user-static"],directory/"builder-apt.log")
    native=directory/"binfmt-bootstrap"
    subprocess.run(["cc","-O2","-static",str(ROOT/"Resources/Scripts/ubuntu_binfmt_bootstrap.c"),"-o",str(native)],check=True)
    policy=root/"usr/sbin/policy-rc.d";policy.write_text("#!/bin/sh\nexit 101\n");policy.chmod(0o755)
    extra=["--cap-add","CAP_SYS_ADMIN","--ro-bind",str(builder/"usr/bin/qemu-aarch64-static"),"/qemu-aarch64-static",
           "--ro-bind",str(native),"/binfmt-bootstrap"]
    run(mapped_bwrap(root,extra)+["/binfmt-bootstrap","/bin/sh","-c",
        "export DEBIAN_FRONTEND=noninteractive; apt-get -o APT::Sandbox::User=root update && apt-get -o APT::Sandbox::User=root install -y --no-install-recommends "+PACKAGES],directory/"arm64-apt.log")
    finalize(directory)


if __name__=="__main__":
    main()
