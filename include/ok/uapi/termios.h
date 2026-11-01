#ifndef OK_UAPI_TERMIOS_H
#define OK_UAPI_TERMIOS_H

/*
 * Native terminal attributes. The layout and every value below are the Linux
 * asm-generic ones and are identical on all ten profiles: four target-endian
 * 32-bit flag words, c_line and 19 control characters, 36 bytes with no
 * padding. OK_TCGETS/OK_TCSETS* copy exactly sizeof(struct ok_termios).
 * Speeds live only in the CBAUD bits of c_cflag. A control character of 0
 * (_POSIX_VDISABLE) never matches input.
 */

#include <stdint.h>

#define OK_NCCS 19

struct ok_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[OK_NCCS];
};

struct ok_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#ifdef __cplusplus
static_assert(sizeof(struct ok_termios) == 36, "struct ok_termios must be 36 bytes");
static_assert(sizeof(struct ok_winsize) == 8, "struct ok_winsize must be 8 bytes");
#else
_Static_assert(sizeof(struct ok_termios) == 36, "struct ok_termios must be 36 bytes");
_Static_assert(sizeof(struct ok_winsize) == 8, "struct ok_winsize must be 8 bytes");
#endif

/* c_cc indices */
#define OK_VINTR 0
#define OK_VQUIT 1
#define OK_VERASE 2
#define OK_VKILL 3
#define OK_VEOF 4
#define OK_VTIME 5
#define OK_VMIN 6
#define OK_VSWTC 7
#define OK_VSTART 8
#define OK_VSTOP 9
#define OK_VSUSP 10
#define OK_VEOL 11
#define OK_VREPRINT 12
#define OK_VDISCARD 13
#define OK_VWERASE 14
#define OK_VLNEXT 15
#define OK_VEOL2 16

/* c_iflag */
#define OK_IGNBRK 0x00000001U
#define OK_BRKINT 0x00000002U
#define OK_IGNPAR 0x00000004U
#define OK_PARMRK 0x00000008U
#define OK_INPCK 0x00000010U
#define OK_ISTRIP 0x00000020U
#define OK_INLCR 0x00000040U
#define OK_IGNCR 0x00000080U
#define OK_ICRNL 0x00000100U
#define OK_IUCLC 0x00000200U
#define OK_IXON 0x00000400U
#define OK_IXANY 0x00000800U
#define OK_IXOFF 0x00001000U
#define OK_IMAXBEL 0x00002000U
#define OK_IUTF8 0x00004000U

/* c_oflag */
#define OK_OPOST 0x00000001U
#define OK_OLCUC 0x00000002U
#define OK_ONLCR 0x00000004U
#define OK_OCRNL 0x00000008U
#define OK_ONOCR 0x00000010U
#define OK_ONLRET 0x00000020U
#define OK_OFILL 0x00000040U
#define OK_OFDEL 0x00000080U
#define OK_NLDLY 0x00000100U
#define OK_NL0 0x00000000U
#define OK_NL1 0x00000100U
#define OK_CRDLY 0x00000600U
#define OK_CR0 0x00000000U
#define OK_CR1 0x00000200U
#define OK_CR2 0x00000400U
#define OK_CR3 0x00000600U
#define OK_TABDLY 0x00001800U
#define OK_TAB0 0x00000000U
#define OK_TAB1 0x00000800U
#define OK_TAB2 0x00001000U
#define OK_TAB3 0x00001800U
#define OK_XTABS 0x00001800U
#define OK_BSDLY 0x00002000U
#define OK_BS0 0x00000000U
#define OK_BS1 0x00002000U
#define OK_VTDLY 0x00004000U
#define OK_VT0 0x00000000U
#define OK_VT1 0x00004000U
#define OK_FFDLY 0x00008000U
#define OK_FF0 0x00000000U
#define OK_FF1 0x00008000U

/* c_cflag */
#define OK_CBAUD 0x0000100fU
#define OK_CBAUDEX 0x00001000U
#define OK_B0 0x00000000U
#define OK_B50 0x00000001U
#define OK_B75 0x00000002U
#define OK_B110 0x00000003U
#define OK_B134 0x00000004U
#define OK_B150 0x00000005U
#define OK_B200 0x00000006U
#define OK_B300 0x00000007U
#define OK_B600 0x00000008U
#define OK_B1200 0x00000009U
#define OK_B1800 0x0000000aU
#define OK_B2400 0x0000000bU
#define OK_B4800 0x0000000cU
#define OK_B9600 0x0000000dU
#define OK_B19200 0x0000000eU
#define OK_B38400 0x0000000fU
#define OK_B57600 0x00001001U
#define OK_B115200 0x00001002U
#define OK_B230400 0x00001003U
#define OK_B460800 0x00001004U
#define OK_B500000 0x00001005U
#define OK_B576000 0x00001006U
#define OK_B921600 0x00001007U
#define OK_B1000000 0x00001008U
#define OK_B1152000 0x00001009U
#define OK_B1500000 0x0000100aU
#define OK_B2000000 0x0000100bU
#define OK_B2500000 0x0000100cU
#define OK_B3000000 0x0000100dU
#define OK_B3500000 0x0000100eU
#define OK_B4000000 0x0000100fU
#define OK_CSIZE 0x00000030U
#define OK_CS5 0x00000000U
#define OK_CS6 0x00000010U
#define OK_CS7 0x00000020U
#define OK_CS8 0x00000030U
#define OK_CSTOPB 0x00000040U
#define OK_CREAD 0x00000080U
#define OK_PARENB 0x00000100U
#define OK_PARODD 0x00000200U
#define OK_HUPCL 0x00000400U
#define OK_CLOCAL 0x00000800U
#define OK_CIBAUD 0x100f0000U
#define OK_IBSHIFT 16
#define OK_CMSPAR 0x40000000U
#define OK_CRTSCTS 0x80000000U

/* c_lflag */
#define OK_ISIG 0x00000001U
#define OK_ICANON 0x00000002U
#define OK_XCASE 0x00000004U
#define OK_ECHO 0x00000008U
#define OK_ECHOE 0x00000010U
#define OK_ECHOK 0x00000020U
#define OK_ECHONL 0x00000040U
#define OK_NOFLSH 0x00000080U
#define OK_TOSTOP 0x00000100U
#define OK_ECHOCTL 0x00000200U
#define OK_ECHOPRT 0x00000400U
#define OK_ECHOKE 0x00000800U
#define OK_FLUSHO 0x00001000U
#define OK_PENDIN 0x00004000U
#define OK_IEXTEN 0x00008000U
#define OK_EXTPROC 0x00010000U

/* OK_TCFLSH queue selectors */
#define OK_TCIFLUSH 0
#define OK_TCOFLUSH 1
#define OK_TCIOFLUSH 2

#endif
