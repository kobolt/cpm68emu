*************************************
*                                   *
* Program to quit from within CP/M. *
*                                   *
*************************************

        .text

start:  moveq   #14,d0
        trap    #15
        rts

        .end

