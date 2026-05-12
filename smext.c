/*
 * smext.c — a minimal terminal text editor
 * Features: open/edit/save, arrow keys, flags below
 * Flags:    --readonly  --backup  --numbers  --wrap=N  --justify
 * Compile:  cc smext.c -o smext
 * Usage:    smext [flags] filename
 *
 * Design notes (same philosophy as PopSiQL):
 *   - POSIX only, zero deps beyond <termios.h>
 *   - Fixed array buffer: MAX_ROWS lines × MAX_LINE_LEN chars
 *   - Full redraw every keystroke (simple, correct, fast enough)
 *   - Justify applies on save only (non-destructive editing)
 *   - Wrap is display-only (buffer stores raw lines)
 *   - Line numbers widen the gutter by GUTTER chars
 */

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>

/* ─── tuneable limits ───────────────────────────────────────────── */
#define MAX_ROWS      4096
#define MAX_LINE_LEN    80
#define GUTTER           5   /* width of line-number column + space */
#define STATUS_FMT      "  %s%s  |  row %-4d col %-3d  |  Ctrl-S save  Ctrl-Q quit"

/* ─── flags ─────────────────────────────────────────────────────── */
static int   f_readonly = 0;
static int   f_backup   = 0;
static int   f_numbers  = 0;
static int   f_wrap     = 0;   /* 0 = off; >0 = column to wrap at  */
static int   f_justify  = 0;

/* ─── editor state ──────────────────────────────────────────────── */
static char  buf[MAX_ROWS][MAX_LINE_LEN];
static int   nrows    = 0;
static int   crow     = 0;   /* cursor row                          */
static int   ccol     = 0;   /* cursor col                          */
static int   dirty    = 0;
static int   tw       = 80;  /* terminal width  (from ioctl)        */
static int   th       = 24;  /* terminal height (from ioctl)        */
static char  filename[256];
static struct termios orig_termios;

/* ─── ANSI helpers ──────────────────────────────────────────────── */
static void wr(const char *s, int n) { write(1, s, n); }
static void ws(const char *s)        { wr(s, strlen(s)); }

static void ansi(const char *seq)    { ws("\033["); ws(seq); }
static void clear_screen(void)       { ws("\033[2J\033[H"); }
static void hide_cursor(void)        { ws("\033[?25l"); }
static void show_cursor(void)        { ws("\033[?25h"); }

/* move cursor to 1-based row,col */
static void move(int r, int c) {
    char b[32];
    int n = 0;
    /* itoa row */
    b[n++] = '\033'; b[n++] = '[';
    if (r >= 100) b[n++] = '0' + r/100;
    if (r >=  10) b[n++] = '0' + (r/10)%10;
    b[n++] = '0' + r%10;
    b[n++] = ';';
    if (c >= 100) b[n++] = '0' + c/100;
    if (c >=  10) b[n++] = '0' + (c/10)%10;
    b[n++] = '0' + c%10;
    b[n++] = 'H';
    wr(b, n);
}

static void set_bg(int code) { char b[16]; b[0]='\033';b[1]='[';
    b[2]='0'+code/10; b[3]='0'+code%10; b[4]='m'; wr(b,5); }
static void reset_attr(void) { ws("\033[0m"); }

/* ─── raw mode ──────────────────────────────────────────────────── */
static void disable_raw(void) { tcsetattr(0, TCSAFLUSH, &orig_termios); }

static void enable_raw(void) {
    tcgetattr(0, &orig_termios);
    atexit(disable_raw);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT|ICRNL|INPCK|ISTRIP|IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |=  (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO|ICANON|IEXTEN|ISIG);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
}

/* ─── terminal size ─────────────────────────────────────────────── */
static void get_term_size(void) {
    struct winsize ws2;
    if (ioctl(1, TIOCGWINSZ, &ws2) == 0) {
        tw = ws2.ws_col;
        th = ws2.ws_row;
    }
}
static void handle_sigwinch(int _) { (void)_; get_term_size(); }

/* ─── justify helper (used on save) ────────────────────────────── */
/*
 * Justify a single line to width w.
 * Last line of paragraph (followed by blank or EOF) stays left-aligned.
 * Lines with one word stay left-aligned.
 * Result written into dst (must be MAX_LINE_LEN).
 */
static void justify_line(const char *src, char *dst, int w,
                          int is_last) {
    /* collect words */
    char words[32][MAX_LINE_LEN]; int nw = 0;
    const char *p = src;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ' ') words[nw][i++] = *p++;
        words[nw][i] = '\0';
        if (i) nw++;
    }
    if (nw == 0) { dst[0] = '\0'; return; }

    /* measure content width */
    int content = 0;
    for (int i = 0; i < nw; i++) content += (int)strlen(words[i]);

    if (nw == 1 || is_last || content >= w) {
        /* left-align */
        dst[0] = '\0';
        for (int i = 0; i < nw; i++) {
            if (i) { strncat(dst, " ", MAX_LINE_LEN-1); }
            strncat(dst, words[i], MAX_LINE_LEN-1);
        }
        return;
    }

    /* distribute spaces */
    int gaps     = nw - 1;
    int total_sp = w - content;
    int base     = total_sp / gaps;
    int extra    = total_sp % gaps;

    dst[0] = '\0';
    for (int i = 0; i < nw; i++) {
        strncat(dst, words[i], MAX_LINE_LEN-1);
        if (i < nw-1) {
            int sp = base + (i < extra ? 1 : 0);
            for (int s = 0; s < sp && (int)strlen(dst) < MAX_LINE_LEN-1; s++)
                strncat(dst, " ", MAX_LINE_LEN-1);
        }
    }
}

/* ─── file I/O ──────────────────────────────────────────────────── */
static void load_file(void) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { nrows = 1; buf[0][0] = '\0'; return; }

    char tmp[MAX_LINE_LEN * MAX_ROWS];
    int n = (int)read(fd, tmp, sizeof(tmp)-1);
    close(fd);
    if (n < 0) n = 0;
    tmp[n] = '\0';

    nrows = 0;
    char *p = tmp;
    while (*p && nrows < MAX_ROWS) {
        int i = 0;
        while (*p && *p != '\n' && i < MAX_LINE_LEN-1)
            buf[nrows][i++] = *p++;
        buf[nrows][i] = '\0';
        nrows++;
        if (*p == '\n') p++;
    }
    if (nrows == 0) { nrows = 1; buf[0][0] = '\0'; }
}

static void save_file(void) {
    if (f_readonly) return;

    /* --backup: copy original to filename.bak */
    if (f_backup) {
        char bak[280];
        strncpy(bak, filename, 270); bak[270]='\0';
        strncat(bak, ".bak", 279);
        int src = open(filename, O_RDONLY);
        if (src >= 0) {
            int dst2 = open(bak, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (dst2 >= 0) {
                char tmp2[4096]; int n2;
                while ((n2=(int)read(src,tmp2,sizeof(tmp2)))>0)
                    write(dst2,tmp2,n2);
                close(dst2);
            }
            close(src);
        }
    }

    int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return;

    int jw = f_justify ? (f_wrap > 0 ? f_wrap : MAX_LINE_LEN-1) : 0;

    for (int r = 0; r < nrows; r++) {
        char out_line[MAX_LINE_LEN];
        if (jw && buf[r][0]) {
            int is_last = (r == nrows-1) || (buf[r+1][0] == '\0');
            justify_line(buf[r], out_line, jw, is_last);
        } else {
            strncpy(out_line, buf[r], MAX_LINE_LEN-1);
            out_line[MAX_LINE_LEN-1] = '\0';
        }
        write(fd, out_line, strlen(out_line));
        write(fd, "\n", 1);
    }
    close(fd);
    dirty = 0;
}

/* ─── display ───────────────────────────────────────────────────── */
static void draw(void) {
    hide_cursor();
    clear_screen();

    int gutter = f_numbers ? GUTTER : 0;
    int text_w = (f_wrap > 0) ? f_wrap : tw - gutter;
    int visible = th - 1;   /* last row = status bar */

    /* scroll offset: keep cursor visible */
    static int row_off = 0;
    if (crow < row_off) row_off = crow;
    if (crow >= row_off + visible) row_off = crow - visible + 1;

    for (int screen_r = 0; screen_r < visible; screen_r++) {
        int r = screen_r + row_off;
        move(screen_r + 1, 1);

        if (r >= nrows) {
            /* empty lines below buffer */
            if (f_numbers) {
                ws("    ~");
            } else {
                ws("~");
            }
            continue;
        }

        if (f_numbers) {
            /* right-align row number in GUTTER-1 chars + space */
            char nb[8]; int rn = r+1; int ni=3;
            nb[4]=' '; nb[5]='\0';
            nb[3]='0'+rn%10; rn/=10;
            nb[2]=(rn?'0'+rn%10:' '); rn/=10;
            nb[1]=(rn?'0'+rn%10:' '); rn/=10;
            nb[0]=(rn?'0'+rn%10:' ');
            (void)ni;
            ws("\033[2m"); wr(nb,5); reset_attr();
        }

        /* display line, truncated to text_w */
        int len = (int)strlen(buf[r]);
        int show = len < text_w ? len : text_w;
        wr(buf[r], show);
    }

    /* status bar */
    move(th, 1);
    set_bg(7);  /* light gray background */
    ws("\033[30m");  /* black text */
    char status[256];
    int sn = 0;
    const char *fn = filename;
    const char *mod = dirty ? "*" : "";
    /* hand-roll snprintf equivalent for POSIX-only */
    ws("  "); ws(fn); ws(mod);
    ws("  |  row ");
    /* print crow+1 */
    { int v=crow+1; char tmp[8]; int ti=7; tmp[ti]='\0';
      do { tmp[--ti]='0'+v%10; v/=10; } while(v);
      ws(tmp+ti); }
    ws(" col ");
    { int v=ccol+1; char tmp[8]; int ti=7; tmp[ti]='\0';
      do { tmp[--ti]='0'+v%10; v/=10; } while(v);
      ws(tmp+ti); }
    ws(f_readonly ? "  |  READONLY" : "  |  Ctrl-S save  Ctrl-Q quit");
    /* pad to end of line */
    ws("\033[K");
    reset_attr();
    (void)status; (void)sn;

    /* reposition cursor */
    int screen_row = crow - row_off + 1;
    int screen_col = ccol + 1 + gutter;
    move(screen_row, screen_col);
    show_cursor();
}

/* ─── editing primitives ────────────────────────────────────────── */
static void clamp_col(void) {
    int len = (int)strlen(buf[crow]);
    if (ccol > len) ccol = len;
}

static void insert_char(char c) {
    if (f_readonly) return;
    int len = (int)strlen(buf[crow]);
    if (len >= MAX_LINE_LEN-1) return;
    memmove(&buf[crow][ccol+1], &buf[crow][ccol], len - ccol + 1);
    buf[crow][ccol] = c;
    ccol++;
    dirty = 1;
}

static void delete_char(void) {   /* backspace */
    if (f_readonly) return;
    if (ccol > 0) {
        int len = (int)strlen(buf[crow]);
        memmove(&buf[crow][ccol-1], &buf[crow][ccol], len - ccol + 1);
        ccol--;
        dirty = 1;
    } else if (crow > 0) {
        /* merge with previous line */
        int prev_len = (int)strlen(buf[crow-1]);
        int cur_len  = (int)strlen(buf[crow]);
        if (prev_len + cur_len < MAX_LINE_LEN-1) {
            strncat(buf[crow-1], buf[crow], MAX_LINE_LEN-1);
            /* shift rows up */
            for (int r = crow; r < nrows-1; r++)
                memcpy(buf[r], buf[r+1], MAX_LINE_LEN);
            nrows--;
            crow--;
            ccol = prev_len;
            dirty = 1;
        }
    }
}

static void insert_newline(void) {
    if (f_readonly) return;
    if (nrows >= MAX_ROWS) return;
    /* shift rows down */
    for (int r = nrows; r > crow+1; r--)
        memcpy(buf[r], buf[r-1], MAX_LINE_LEN);
    nrows++;
    /* split current line at ccol */
    strncpy(buf[crow+1], &buf[crow][ccol], MAX_LINE_LEN-1);
    buf[crow+1][MAX_LINE_LEN-1] = '\0';
    buf[crow][ccol] = '\0';
    crow++;
    ccol = 0;
    dirty = 1;
}

/* ─── input ─────────────────────────────────────────────────────── */
static int read_key(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) return -1;
    if (c == '\033') {
        unsigned char seq[3];
        if (read(0, &seq[0], 1) != 1) return '\033';
        if (read(0, &seq[1], 1) != 1) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 1000; /* up    */
                case 'B': return 1001; /* down  */
                case 'C': return 1002; /* right */
                case 'D': return 1003; /* left  */
            }
        }
        return '\033';
    }
    return (int)c;
}

/* ─── main loop ─────────────────────────────────────────────────── */
static void run(void) {
    enable_raw();
    get_term_size();
    signal(SIGWINCH, handle_sigwinch);

    while (1) {
        draw();
        int k = read_key();

        switch (k) {
        case 17:  /* Ctrl-Q */
            disable_raw();
            clear_screen();
            ws("bye\n");
            exit(0);

        case 19:  /* Ctrl-S */
            save_file();
            break;

        case 1000: /* up */
            if (crow > 0) crow--;
            clamp_col();
            break;
        case 1001: /* down */
            if (crow < nrows-1) crow++;
            clamp_col();
            break;
        case 1002: /* right */
            if (ccol < (int)strlen(buf[crow])) ccol++;
            else if (crow < nrows-1) { crow++; ccol=0; }
            break;
        case 1003: /* left */
            if (ccol > 0) ccol--;
            else if (crow > 0) { crow--; ccol=(int)strlen(buf[crow]); }
            break;

        case 1:   /* Ctrl-A — home */
            ccol = 0;
            break;
        case 5:   /* Ctrl-E — end */
            ccol = (int)strlen(buf[crow]);
            break;

        case 127: /* backspace */
        case 8:
            delete_char();
            break;

        case 13:  /* Enter */
            insert_newline();
            break;

        default:
            if (k >= 32 && k < 127) insert_char((char)k);
            break;
        }
    }
}

/* ─── arg parsing ───────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    filename[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--readonly")) f_readonly = 1;
        else if (!strcmp(argv[i], "--backup"))   f_backup   = 1;
        else if (!strcmp(argv[i], "--numbers"))  f_numbers  = 1;
        else if (!strcmp(argv[i], "--justify"))  f_justify  = 1;
        else if (!strncmp(argv[i], "--wrap=", 7)) {
            f_wrap = atoi(argv[i]+7);
            if (f_wrap < 10 || f_wrap > MAX_LINE_LEN-1) f_wrap = MAX_LINE_LEN-1;
        }
        else if (argv[i][0] != '-') {
            strncpy(filename, argv[i], 254); filename[254]='\0';
        }
    }

    if (!filename[0]) {
        write(1,"usage: smext [--readonly] [--backup] [--numbers] [--wrap=N] [--justify] <file>\n",78);
        return 1;
    }

    load_file();
    run();
    return 0;
}
