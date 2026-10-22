iOS 16.5 on the OnePlus Ace 6T, from RAM.

On Linux: `git clone https://github.com/hxfuxyy/ios-macan.git && cd ios-macan`, then `./hackios --get` and
`./hackios --boot`. You only need an unlocked bootloader and USB debugging.

`hackios-kit.zip` is what `--get` downloads by itself: the kernel, the UEFI image and our userland. iOS itself is
not in it: `--get` downloads it from Apple.
