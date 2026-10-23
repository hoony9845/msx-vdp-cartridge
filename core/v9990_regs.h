/*
 * V9990 I/O port, register and bit definitions.
 *
 * Source: Yamaha V9990 E-VDP-III Application Manual + openMSX (V9990.cc).
 * Items marked VERIFY have not yet been checked against the manual.
 */
#ifndef V9990_REGS_H
#define V9990_REGS_H

/* ---- I/O ports (base 0x60 on MSX; only the low nibble is decoded here) ---- */
#define V9990_PORT_VRAM_DATA     0x0  /* P#0 R/W */
#define V9990_PORT_PALETTE_DATA  0x1  /* P#1 R/W */
#define V9990_PORT_CMD_DATA      0x2  /* P#2 R/W */
#define V9990_PORT_REG_DATA      0x3  /* P#3 R/W */
#define V9990_PORT_REG_SELECT    0x4  /* P#4 W   */
#define V9990_PORT_STATUS        0x5  /* P#5 R   */
#define V9990_PORT_INT_FLAG      0x6  /* P#6 R/W (write 1 = clear) */
#define V9990_PORT_SYSTEM_CTRL   0x7  /* P#7 W   */
/* P#8-P#F: reserved (kanji ROM on some boards) */

/* P#4 register select */
#define V9990_REGSEL_WII   0x80  /* write increment inhibit */
#define V9990_REGSEL_RII   0x40  /* read increment inhibit */
#define V9990_REGSEL_MASK  0x3F

/* P#5 status */
#define V9990_ST_TR   0x80  /* command data transfer ready */
#define V9990_ST_VR   0x40  /* vertical retrace */
#define V9990_ST_HR   0x20  /* horizontal retrace */
#define V9990_ST_BD   0x10  /* border color detect (SRCH) */
#define V9990_ST_MSC  0x04  /* mirror of P#7 MCS */
#define V9990_ST_EO   0x02  /* second field (interlace) */
#define V9990_ST_CE   0x01  /* command executing */

/* P#6 interrupt flags / R#9 interrupt enable */
#define V9990_INT_CE  0x04  /* command end */
#define V9990_INT_HI  0x02  /* line interrupt */
#define V9990_INT_VI  0x01  /* vertical interrupt */

/* P#7 system control */
#define V9990_SYS_SRS 0x02  /* software reset */
#define V9990_SYS_MCS 0x01  /* master clock select */

/* ---- Registers ---- */
enum {
    V9990_R_VRAM_WRITE_ADDR0 = 0,   /* A7-A0 */
    V9990_R_VRAM_WRITE_ADDR1 = 1,   /* A15-A8 */
    V9990_R_VRAM_WRITE_ADDR2 = 2,   /* AII(7) A18-A16(2-0) */
    V9990_R_VRAM_READ_ADDR0  = 3,
    V9990_R_VRAM_READ_ADDR1  = 4,
    V9990_R_VRAM_READ_ADDR2  = 5,   /* writing this triggers a prefetch */
    V9990_R_SCREEN_MODE0     = 6,   /* DSPM(7-6) DCKM(5-4) XIMM(3-2) CLRM(1-0) */
    V9990_R_SCREEN_MODE1     = 7,   /* C25M(6) SM1(5) SM(4) PAL(3) EO(2) IL(1) HSCN(0) */
    V9990_R_CONTROL          = 8,   /* DISP(7) SPD(6) YSE(5) VWTE(4) VWM(3) DMAE(2) VSL(1-0) */
    V9990_R_INT_ENABLE       = 9,   /* IECE(2) IEH(1) IEV(0) */
    V9990_R_INT_LINE_LO      = 10,
    V9990_R_INT_LINE_HI      = 11,  /* IEHM(7) IL9-8(1-0) */
    V9990_R_INT_X            = 12,
    V9990_R_PALETTE_CTRL     = 13,  /* PLTM(7-6) YAE(5) PLTAIH(4) PLTO5-2(3-0) */
    V9990_R_PALETTE_PTR      = 14,
    V9990_R_BACKDROP         = 15,
    V9990_R_DISPLAY_ADJUST   = 16,
    V9990_R_SCROLL_AY_LO     = 17,
    V9990_R_SCROLL_AY_HI     = 18,  /* R512(7) R256(6) SCAY12-8(4-0) */
    V9990_R_SCROLL_AX_LO     = 19,  /* SCAX2-0 (2-0) */
    V9990_R_SCROLL_AX_HI     = 20,  /* SCAX10-3 */
    V9990_R_SCROLL_BY_LO     = 21,
    V9990_R_SCROLL_BY_HI     = 22,  /* SCBY8 (0) */
    V9990_R_SCROLL_BX_LO     = 23,  /* SCBX2-0 (2-0) */
    V9990_R_SCROLL_BX_HI     = 24,  /* SCBX8-3 (5-0) */
    V9990_R_SPRITE_PGT       = 25,  /* P1: SGBA17-15 (3-1), P2: SGBA18-15 (3-0) */
    V9990_R_LCD_CTRL         = 26,
    V9990_R_PRIORITY_CTRL    = 27,  /* PRY1-0 (3-2) PRX1-0 (1-0), units of 64 dots */
    V9990_R_SPRITE_PAL_CTRL  = 28,  /* CSPO5-2: cursor palette offset */
    /* command engine parameters */
    V9990_R_CMD_SX0 = 32, V9990_R_CMD_SX1 = 33,
    V9990_R_CMD_SY0 = 34, V9990_R_CMD_SY1 = 35,
    V9990_R_CMD_DX0 = 36, V9990_R_CMD_DX1 = 37,
    V9990_R_CMD_DY0 = 38, V9990_R_CMD_DY1 = 39,
    V9990_R_CMD_NX0 = 40, V9990_R_CMD_NX1 = 41,
    V9990_R_CMD_NY0 = 42, V9990_R_CMD_NY1 = 43,
    V9990_R_CMD_ARG = 44,           /* DIY(3) DIX(2) NEQ(1) MAJ(0) */
    V9990_R_CMD_LOP = 45,           /* TP(4) L11 L10 L01 L00 (3-0) */
    V9990_R_CMD_WM0 = 46, V9990_R_CMD_WM1 = 47,
    V9990_R_CMD_FC0 = 48, V9990_R_CMD_FC1 = 49,
    V9990_R_CMD_BC0 = 50, V9990_R_CMD_BC1 = 51,
    V9990_R_CMD_OP  = 52,           /* CM3-0 (7-4); writing starts the command */
    V9990_R_CMD_BX0 = 53, V9990_R_CMD_BX1 = 54, /* SRCH result (read only) */
    V9990_NUM_REGS  = 64
};

/* Writable bits of R#0-R#31 (read-back mask measured on real hardware,
 * as documented in openMSX). */
#define V9990_REG_WRITE_MASKS { \
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, \
    0xFF, 0x87, 0xFF, 0x83, 0x0F, 0xFF, 0xFF, 0xFF, \
    0xFF, 0xFF, 0xDF, 0x07, 0xFF, 0xFF, 0xC1, 0x07, \
    0x3F, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

/* VRAM layout of the pattern modes (CPU / logical addresses) */
#define V9990_P1_PGT_A      0x00000u
#define V9990_P1_PGT_B      0x40000u
#define V9990_P1_SPAT       0x3FE00u
#define V9990_P1_PNT_A      0x7C000u
#define V9990_P1_PNT_B      0x7E000u
#define V9990_P2_PGT        0x00000u
#define V9990_P2_SPAT       0x7BE00u
#define V9990_P2_PNT        0x7C000u

/* R#6 fields */
#define V9990_DSPM(r6)  (((r6) >> 6) & 3)   /* 0=P1 1=P2 2=bitmap 3=standby */
#define V9990_DCKM(r6)  (((r6) >> 4) & 3)
#define V9990_XIMM(r6)  (((r6) >> 2) & 3)   /* image width 256 << XIMM */
#define V9990_CLRM(r6)  ((r6) & 3)          /* 2/4/8/16 bpp */

/* R#7 fields */
#define V9990_R7_C25M 0x40
#define V9990_R7_PAL  0x08
#define V9990_R7_EO   0x04
#define V9990_R7_IL   0x02
#define V9990_R7_HSCN 0x01

/* R#8 fields */
#define V9990_R8_DISP 0x80
#define V9990_R8_SPD  0x40      /* sprite / cursor disable */

/* Sprite attribute byte 3 */
#define V9990_SPR_PR0 0x10      /* not displayed */
#define V9990_SPR_PR1 0x20      /* behind the front pattern layer */

/* R#44 ARG */
#define V9990_ARG_DIY 0x08
#define V9990_ARG_DIX 0x04
#define V9990_ARG_NEQ 0x02
#define V9990_ARG_MAJ 0x01

/* R#45 LOP */
#define V9990_LOP_TP  0x10

/* Commands (R#52 bits 7-4) */
enum {
    V9990_CMD_STOP = 0x0, V9990_CMD_LMMC = 0x1, V9990_CMD_LMMV = 0x2,
    V9990_CMD_LMCM = 0x3, V9990_CMD_LMMM = 0x4, V9990_CMD_CMMC = 0x5,
    V9990_CMD_CMMK = 0x6, V9990_CMD_CMMM = 0x7, V9990_CMD_BMXL = 0x8,
    V9990_CMD_BMLX = 0x9, V9990_CMD_BMLL = 0xA, V9990_CMD_LINE = 0xB,
    V9990_CMD_SRCH = 0xC, V9990_CMD_POINT = 0xD, V9990_CMD_PSET = 0xE,
    V9990_CMD_ADVN = 0xF
};

#define V9990_VRAM_SIZE   (512u * 1024u)
#define V9990_VRAM_MASK   (V9990_VRAM_SIZE - 1u)
#define V9990_PALETTE_ENTRIES 64

#endif
