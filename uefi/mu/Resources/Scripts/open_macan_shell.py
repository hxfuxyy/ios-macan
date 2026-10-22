#!/usr/bin/env python3
"""Interactive Ubuntu USB shell; the separate PID 1 diagnostic port stays free."""
import argparse
import fcntl
import os
import select
import termios
import tty

DEVICE = '/dev/serial/by-id/usb-Project_Silicium_Ace_6T_Linux_diagnostics_ace6t-linux-test-if02'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default=DEVICE)
    args = parser.parse_args()
    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
    old = termios.tcgetattr(fd)
    local = termios.tcgetattr(0)
    try:
        fcntl.ioctl(fd, termios.TIOCEXCL)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        print('Connecting to Ubuntu on Ace 6T. Ctrl+] closes this window connection.', flush=True)
        tty.setraw(0)
        os.write(fd, b'\r')
        while True:
            ready, _, _ = select.select([0, fd], [], [])
            for source in ready:
                data = os.read(source, 65536)
                if not data:
                    return
                if source == 0 and b'\x1d' in data:
                    return
                target = fd if source == 0 else 1
                while data:
                    sent = os.write(target, data)
                    if sent == 0:
                        raise OSError('USB disconnected')
                    data = data[sent:]
    finally:
        termios.tcsetattr(0, termios.TCSANOW, local)
        termios.tcsetattr(fd, termios.TCSANOW, old)
        fcntl.ioctl(fd, termios.TIOCNXCL)
        os.close(fd)


if __name__ == '__main__':
    main()
