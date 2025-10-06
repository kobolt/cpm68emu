*****************************************************************
*                                                               *
*               CP/M-68K BIOS                                   *
*       Basic Input/Output Subsystem                            *
*            For Emulated System                                *
*                                                               *
*****************************************************************

        .globl  _init           * BIOS initialization entry point
        .globl  _ccp            * CCP entry point

_init:  move.l  #traphndl,$8c   * Set up trap #3 handler
        move.l  #welcome,a0     * Display welcome message
weloop: move.b  (a0)+,d1
        cmpi.b  #$24,d1         * Compare against '$'
        beq     wedone
        jsr     conout
        bra     weloop
wedone: clr.l   d0              * Log on disk A:, user 0
        rts

traphndl:
        cmpi    #nfuncs,d0
        bcc     trapng
        lsl     #2,d0           * Multiply bios function by 4
        movea.l 6(pc,d0),a0     * Get handler address
        jsr     (a0)            * Call handler
trapng:
        rte

biosbase:
        .dc.l  _init    *  0 - Initialization
        .dc.l  wboot    *  1 - Warm boot
        .dc.l  constat  *  2 - Console status
        .dc.l  conin    *  3 - Read console character
        .dc.l  conout   *  4 - Write console character
        .dc.l  lstout   *  5 - List character output
        .dc.l  pun      *  6 - Auxiliary output
        .dc.l  rdr      *  7 - Auxiliary input
        .dc.l  home     *  8 - Home
        .dc.l  seldsk   *  9 - Select disk drive
        .dc.l  settrk   * 10 - Select track number
        .dc.l  setsec   * 11 - Select sector number
        .dc.l  setdma   * 12 - Set DMA address
        .dc.l  read     * 13 - Read sector
        .dc.l  write    * 14 - Write sector
        .dc.l  listst   * 15 - Return list status
        .dc.l  sectran  * 16 - Sector translate
        .dc.l  home     * 17 - N/A
        .dc.l  getseg   * 18 - Get address of memory region table
        .dc.l  getiob   * 19 - Get I/O byte
        .dc.l  setiob   * 20 - Set I/O byte
        .dc.l  flush    * 21 - Flush buffers
        .dc.l  setexc   * 22 - Set exception handle address

        nfuncs=(*-biosbase)/4

wboot:  jmp     _ccp

constat: moveq #1,d0            * Console Status
        trap 15                 * Call emulator, status byte in d0 after
        rts

conin:  bsr     constat         * See if key pressed
        tst     d0
        beq     conin           * Wait until key pressed
        moveq #2,d0             * Console Read
        trap 15                 * Call emulator, key byte in d0 after
        rts

conout: moveq #3,d0             * Console Write
        trap 15                 * Call emulator, output byte in d1 before
        rts

lstout: rts

pun:    rts

rdr:    rts

listst: move.b  #$ff,d0 * Device not ready
        rts

home:   rts

seldsk: moveq #4,d0             * RAM Disk Select
        trap 15                 * Call emulator, disk no in d1 before
        tst    d0               * Value 0 means no disk, do not set dph
        beq    nodisk
        move.l #dph0,d0         * Point d0 at dph0 if OK
        mulu   #26,d1           * Calculate dph offset (26 = dph size)
        add.l  d1,d0            * Add dph offset, in case other disk
nodisk: rts

settrk: moveq #5,d0             * RAM Disk Track Set
        trap 15                 * Call emulator, track no in d1 before
        rts

setsec: moveq #6,d0             * RAM Disk Sector Set
        trap 15                 * Call emulator, sector no in d1 before
        rts

setdma: moveq #7,d0             * RAM Disk DMA Set
        trap 15                 * Call emulator, dma address in d1 before
        rts

sectran: move.w d1,d0           * No sector translation, just 1-to-1 mapping
        rts

read:   moveq #8,d0             * RAM Disk Read
        trap 15                 * Call emulator to perform the read
        clr.l   d0              * Always OK
        rts

write:  moveq #9,d0             * RAM Disk Write
        trap 15                 * Call emulator to perform the write
        clr.l   d0              * Always OK
        rts

flush:  clr.l   d0              * Return successful
        rts

getseg: move.l  #memrgn,d0
        rts

getiob: rts

setiob: rts

setexc: andi.l  #$ff,d1         * Do only for exceptions 0 - 255
        cmpi    #47,d1          * Skip Trap 15
        beq     noset
        cmpi    #9,d1           * Skip trace exception
        beq     noset
        lsl     #2,d1           * Multiply exception number by 4
        movea.l d1,a0
        move.l  (a0),d0         * Return old vector value
        move.l  d2,(a0)         * Insert new vector
noset:  rts



        .data

* Welcome text

welcome: .dc.b  13,10,'CP/M-68K Emulator',13,10,'$'

* Memory region definition

memrgn: .dc.w   1       * 1 memory region
        .dc.l   $000400 * Start
        .dc.l   $F00000 * Length/size

* Disk parameter header

dph0:   .dc.l   0       * No translation table used
        .dc.w   0       * Three scratchpad words
        .dc.w   0
        .dc.w   0
        .dc.l   dirbuf  * Pointer to directory buffer
        .dc.l   dpb     * Pointer to disk parameter block
        .dc.l   0       * Check vector not used
        .dc.l   alv0    * Pointer to allocation vector

dph1:   .dc.l   0       * No translation table used
        .dc.w   0       * Three scratchpad words
        .dc.w   0
        .dc.w   0
        .dc.l   dirbuf  * Pointer to directory buffer
        .dc.l   dpb     * Pointer to disk parameter block
        .dc.l   0       * Check vector not used
        .dc.l   alv1    * Pointer to allocation vector

dph2:   .dc.l   0       * No translation table used
        .dc.w   0       * Three scratchpad words
        .dc.w   0
        .dc.w   0
        .dc.l   dirbuf  * Pointer to directory buffer
        .dc.l   dpb     * Pointer to disk parameter block
        .dc.l   0       * Check vector not used
        .dc.l   alv2    * Pointer to allocation vector

dph3:   .dc.l   0       * No translation table used
        .dc.w   0       * Three scratchpad words
        .dc.w   0
        .dc.w   0
        .dc.l   dirbuf  * Pointer to directory buffer
        .dc.l   dpb     * Pointer to disk parameter block
        .dc.l   0       * Check vector not used
        .dc.l   alv3    * Pointer to allocation vector

* Disk parameter block

dpb:    .dc.w   256     * Sectors per track
        .dc.b   4       * Block shift (in relation to block size)
        .dc.b   15      * Block mask (in relation to block size)
        .dc.b   0       * Extent mask
        .dc.b   0       * Dummy fill
        .dc.w   8191    * Disk size
        .dc.w   4095    * Directory entries
        .dc.w   0       * Reserved
        .dc.w   0       * Check vector not used for unremovable RAM disk
        .dc.w   1       * Track offset



        .bss

dirbuf: .ds.b   128     * Directory buffer
alv0:   .ds.b   1024    * Allocation vector = (disk size / 8) + 1
alv1:   .ds.b   1024    * Allocation vector = (disk size / 8) + 1
alv2:   .ds.b   1024    * Allocation vector = (disk size / 8) + 1
alv3:   .ds.b   1024    * Allocation vector = (disk size / 8) + 1

        .end
