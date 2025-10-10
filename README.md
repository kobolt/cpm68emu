# cpm68emu
Motorola 68000 emulator specialized for running CP/M-68K.

## Features
* All Motorola 68000 instructions implemented.
* Debugger with breakpoint and trace support.
* Full 24-bit address space available and mapped to RAM.
* TPA (Transient Program Area) placed at 0x400 for best compatibility.
* Four 16MB RAM disks (A: to D:) as default, can be pre-loaded with images.
* Using the somewhat standard "em68k" format for RAM disks.
* Trap #15 is used from the BIOS to communicate with the emulator.
* select() and poll() is used on keyboard input to relax the host CPU.
* Injection of keyboard input from command line, or a file, for automation.
* LF is converted to CR, and DEL is converted to BS, for better compatibility.
* Possible to add native CP/M-68K commands for READ, WRITE and QUIT.

## Tips
* Use Ctrl+C to enter the debugger, then enter the 'q' command to quit the emulator.
* Also possible to quit with the native QUIT command if it exists in the RAM disk.
* [cpmtools](http://www.moria.de/~michael/cpmtools/) can be used to transfer files to and from RAM disk images.
* Alternatively the YAZE-style READ and WRITE commands can be used for file transfers if they exist in the RAM disk already.
* Use 'z' from within the debugger to send Ctrl+C and other control codes to CP/M.
* Any changes that CP/M perform on the RAM disks are not saved automatically. RAM disk A can be saved with 'f' from the debugger.

## Known limitations
* Certain values in 68000 address error exception frames are not correct, but this has no practical effect on CP/M-68K.
* Handling of missing/illegal 68000 instructions is not always correct due to the way opcodes are decoded.
* The READ and WRITE commands operate on the host's current directory only, which limits the usefulness. A shell script wrapping the emulator can be used to overcome this.

## Memory map
| Start    | End      | Purpose                |
| -------- | -------- | ---------------------- |
| 0x000000 | 0x0003FF | Exception Vectors      |
| 0x000400 | 0xEFFFFF | Transient Program Area |
| 0xFF0000 | 0xFFFFFF | System (CP/M and BIOS) |

## RAM disk format
Add the following to "/usr/share/diskdefs" to be used by cpmtools:
```
diskdef em68k
  seclen 128
  tracks 512
  sectrk 256
  blocksize 2048
  maxdir 4096
  skew 0
  boottrk 1
  os 2.2
end
```

## Minimal initial RAM disk
The following cpmtools commands can be used to create a minimal default RAM disk:
```
mkfs.cpm -f em68k ramdisk.bin
cpmcp -f em68k ramdisk.bin read.68k 0:READ.68K
cpmcp -f em68k ramdisk.bin write.68k 0:WRITE.68K
cpmcp -f em68k ramdisk.bin quit.68k 0:QUIT.68K
```

## C compiler and assembler toolchain
The Digital Research CP/M-68K disk set and toolchain is quite useful since it provides a C compiler and assembler. Look for "68kv1_3.zip" at [cpm.z80.de](http://www.cpm.z80.de/binary.html)

Prepare a RAM disk by just copying all the files from all the disks:
```
cd /tmp/cpm68k13
unzip 68kv1_3.zip
mkfs.cpm -f em68k /tmp/cpm68k13.ramdisk.bin
for FILE in DISK*/*; do
  cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin $FILE 0:`basename $FILE`
done
```

It can be useful to "relocate" some of the .REL programs into .68K programs. The .68K files are tied to the TPA of the running system, but it is then no longer necessary to specify the file extension to call them.

Start the emulator:
```
./cpm68emu /tmp/cpm68k13.ramdisk.bin
```
And use the following commands:
```
RELOC.REL ED.REL ED.68K
RELOC.REL PIP.REL PIP.68K
RELOC.REL AS68.REL AS68.68K
RELOC.REL CP68.REL CP68.68K
RELOC.REL C068.REL C068.68K
RELOC.REL C168.REL C168.68K
RELOC.REL AR68.REL AR68.68K
RELOC.REL LO68.REL LO68.68K
RELOC.REL LINK68.REL LINK68.68K
```
The assembler needs to be initialized before it can be used, this is done by calling it once with the following arguments:
```
AS68 -I AS68INIT
```
Remember to enter the debugger (Ctrl+C) and save the contents of the RAM disk with the 'f' command afterwards.

## Fortran 77 compiler
There is a Fortran 77 compiler by Silicon Valley Software for CP/M-68K at [cpm.z80.de](http://www.cpm.z80.de/binary.html).

Extract all the files from the zip file to a RAM disk:
```
cd /tmp/fortran77
unzip SVS-FORTRAN-77-PATCHED-TO-WORK.zip
mkfs.cpm -f em68k /tmp/fortran77.ramdisk.bin
for FILE in /tmp/fortran77/*; do
  cpmcp -f em68k /tmp/fortran77.ramdisk.bin $FILE 0:`basename $FILE`
done
```

Start the emulator with the toolchain as the D: disk:
```
./cpm68emu /tmp/fortran77.ramdisk.bin -D /tmp/cpm68k13.ramdisk.bin
```
Use the following commands to set it up:
```
D:PIP.REL A: = D:LO68.68K
D:PIP.REL A: = D:CLIB
D:PIP.REL A: = D:S.O
D:RELOC.REL FORTRAN.REL FORTRAN.68K
D:RELOC.REL CODE.REL CODE.68K
D:RELOC.REL ULINKER.REL ULINKER.68K
```
Enter the debugger (Ctrl+C) and save the RAM disk with the 'f' command. Fortran files can then be compiled with the 'F' script, but it really likes to reside as the B: disk:
```
./cpm68emu -B /tmp/fortran77.ramdisk.bin
```

## Assembling the BIOS
CP/M and the BIOS is already included in the "emubios.srec" file, but here is how to assemble it again from source. Assuming the toolchain has already been setup on a RAM disk image, transfer the BIOS source file to the RAM disk and start the emulator:
```
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin emubios.s 0:EMUBIOS.S
./cpm68emu /tmp/cpm68k13.ramdisk.bin
```

CP/M commands to initialize, assemble, link, relocate and send the binary as an S-record to the console:
```
AS68.REL -I AS68INIT
AS68.REL EMUBIOS.S
LO68.REL -R -UCPM -O CPM.REL CPMLIB EMUBIOS.O
RELOC.REL -BFF0000 CPM.REL CPM.SYS
SENDC68.REL CPM.SYS >EMUBIOS.S68
TYPE EMUBIOS.S68
```

## Assembling the READ/WRITE/QUIT programs
Assuming the toolchain has already been setup on a RAM disk image, transfer the source files and start the emulator:
```
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin read.s 0:READ.S
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin write.s 0:WRITE.S
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin quit.s 0:QUIT.S
./cpm68emu /tmp/cpm68k13.ramdisk.bin
```

CP/M commands to initialize, assemble, link and relocate:
```
AS68.REL -I AS68INIT
AS68.REL READ.S
AS68.REL WRITE.S
AS68.REL QUIT.S
LO68.REL -R -O READ.REL READ.O
LO68.REL -R -O WRITE.REL WRITE.O
LO68.REL -R -O QUIT.REL QUIT.O
RELOC.REL READ.REL READ.68K
RELOC.REL WRITE.REL WRITE.68K
RELOC.REL QUIT.REL QUIT.68K
```

Enter the debugger (Ctrl+C) and use 'f' to save the RAM disk, then 'q' to quit, and transfer the binaries back out:
```
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin 0:READ.68K read.68k
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin 0:WRITE.68K write.68k
cpmcp -f em68k /tmp/cpm68k13.ramdisk.bin 0:QUIT.68K quit.68k
```

## Further information
Information on my blog:
* [CP/M-68K and Motorola 68000 Emulator](https://kobolt.github.io/article-264.html)

YouTube video:
* [CP/M-68K Emulator](https://www.youtube.com/watch?v=OsRkhFyflAo)

