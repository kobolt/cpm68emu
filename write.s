*************************************************************
*                                                           *
* Program to write local CP/M file to remote emulator host. *
*                                                           *
*************************************************************

* BDOS Functions:
prntstr = 9
open    = 15
close   = 16
readseq = 20
dsetdma = 26

* Emulator Functions:
ropen   = 10
rwrite  = 11
rclose  = 13

        .text

start:  link    a6,#0
        move.l  8(a6),a0     * Base page address.
        lea     $5c(a0),a1   * Offset to first FCB.
        move.l  a1,fcb

        move.w  #open,d0     * Open local file.
        move.l  fcb,d1
        trap    #2
        cmpi.w  #$00ff,d0
        bne     lopnok

        move.w  #prntstr,d0  * Print error message.
        move.l  #lopnerr,d1
        trap    #2
        bra     exit

lopnok: move.l  fcb,a0
        clr.b   32(a0)       * Clear current record in FCB.

        moveq   #ropen,d0    * Open remote file.
        move.l  fcb,d1
        add.l   #1,d1        * Skip drive code part of FCB.
        moveq   #$77,d2      * Open for 0x77 = 'w' = writing.
        trap    #15
        cmpi.w  #$00ff,d0
        bne     ropnok

        move.w  #prntstr,d0  * Print error message.
        move.l  #ropnerr,d1
        trap    #2
        bra     exit

ropnok: move.w  #dsetdma,d0  * Prepare DMA buffer area.
        move.l  #buf,d1
        trap    #2

loop:   move.w  #readseq,d0  * Read from local file.
        move.l  fcb,d1
        trap    #2
        tst.w   d0
        bne     done
        moveq   #rwrite,d0   * Write to remote file.
        move.l  #buf,d1
        trap    #15
        bra     loop

done:   moveq   #rclose,d0   * Close remote file.
        trap    #15
        move.w  #close,d0    * Close local file.
        move.l  fcb,d1
        trap    #2

exit:   unlk    a6           * Exit to CCP.
        rts

        .bss

        .even

buf:    .ds.b   128
fcb:    .ds.l   1

        .data

lopnerr: .dc.b 'Open local file failed!',13,10,'$'
ropnerr: .dc.b 'Open remote file failed!',13,10,'$'

        .end

