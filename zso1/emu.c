#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <asm/ptrace.h>
#include <unistd.h>
#include <errno.h>
#include <linux/random.h>
#include <sys/syscall.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ncurses.h>

#include "emuerr.h"
#include "alienos.h"

#define LOADER_PATH "./loader"

#define MAX_X 80
#define MAX_Y 24

#define LONG_SIZE (sizeof (long))
#define COL_OFF 1

#define MY_SYS_getrandom 318

typedef struct user_regs_struct reg_t;


/**ALIENOS SYSCALL ABI**
 * Syscall number: rax
 * Return value: rax
 * Arguments: rdi, rsi, rdx, r10
 */

/* void noreturn end(int status) */
static void end(pid_t child, reg_t regs)
{
    (void) child;
    int status;

    status = (int) regs.rdi;
    if (status < 0 || status > 63)
        EMUERR("end: invalid status");

    exit(status);
}

/* uint32_t getrand() */
static void getrand(pid_t child, reg_t regs)
{
    long r;
    uint32_t randval;

    r = syscall(MY_SYS_getrandom, &randval, 4, 0);
    SYSERR(r, "getrandom");

    regs.rax = randval;

    r = ptrace(PTRACE_SETREGS, child, NULL, &regs);
    SYSERR(r, "PTRACE_SETREGS");
}

/* int getkey() */
static void getkey(pid_t child, reg_t regs)
{
    long r;
    int ch;
    int key = 0;

    while (!key)
    {
        ch = getch();
        NERR(ch);

        switch (ch) {
        case '\n':
            key = ALIEN_KEY_ENTER;
            break;
        case KEY_UP:
            key = ALIEN_KEY_UP;
            break;
        case KEY_LEFT:
            key = ALIEN_KEY_LEFT;
            break;
        case KEY_DOWN:
            key = ALIEN_KEY_DOWN;
            break;
        case KEY_RIGHT:
            key = ALIEN_KEY_RIGHT;
            break;
        default:
            if (ch >= ALIEN_ASCII_MIN && ch <= ALIEN_ASCII_MAX)
                key = ch;
            break;
        }
    }

    regs.rax = key;
    r = ptrace(PTRACE_SETREGS, child, NULL, &regs);
    SYSERR(r, "PTRACE_SETREGS");
}

/* void print(int x, int y, uint16_t *chars, int n) */
static void print(pid_t child, reg_t regs)
{
    static long buf[MAX_X];
    static uint16_t chars[MAX_X];

    int x, y, n;
    uint64_t addr;
    uint64_t len;
    int padding1, padding2;
    long *start;
    uint64_t longn;
    uint64_t i;
    long r;
    int oldx, oldy;
    int ch, color;

    x = (int) regs.rdi;
    y = (int) regs.rsi;
    n = (int) regs.r10;
    addr = (uint64_t) regs.rdx;

    if (y < 0 || y >= MAX_Y || x < 0 || n <= 0 || x + n > MAX_X)
        EMUERR("print: invalid x or y or n");

    /* Make sure reads are naturally aligned with long type. */
    len = n * sizeof (uint16_t);

    padding1 = addr % LONG_SIZE;
    padding2 = LONG_SIZE - (addr + len) % LONG_SIZE;

    start = (long *) (addr - padding1);
    longn = (len + padding1 + padding2) / LONG_SIZE;

    for (i = 0; i < longn; ++i)
    {
        errno = 0;
        r = ptrace(PTRACE_PEEKDATA, child, start + i, NULL);
        if (errno) SYSERR(-1, "PTRACE_PEEKDATA");

        buf[i] = r;
    }
    memcpy(chars, (char *) buf + padding1, len);

    getyx(stdscr, oldy, oldx);
    NERR(move(y, x));
    for (i = 0; i < n; ++i)
    {
        ch = chars[i] & 0xFF;
        color = (chars[i] >> 8 & 7) + COL_OFF;

        NERR(attron(COLOR_PAIR(color)));
        addch(ch); // SUBTLE: not every ERR returned here is actually an error...
        NERR(attroff(COLOR_PAIR(color)));
    }
    NERR(move(oldy, oldx));
    NERR(refresh());
}

/* void setcursor(int x, int y) */
static void setcursor(pid_t child, reg_t regs)
{
    (void) child;
    int x, y;

    x = (int) regs.rdi;
    y = (int) regs.rsi;
    if (x < 0 || x >= MAX_X || y < 0 || y >= MAX_Y)
        EMUERR("setcursor: invalid x or y");

    NERR(move(y, x));
    NERR(refresh());
}

void handlesyscall(pid_t child)
{
    reg_t regs;
    long r;

    r = ptrace(PTRACE_GETREGS, child, NULL, &regs);
    SYSERR(r, "PTRACE_GETREGS");

    switch (regs.orig_rax) {
    case 0:
        end(child, regs);
        break;
    case 1:
        getrand(child, regs);
        break;
    case 2:
        getkey(child, regs);
        break;
    case 3:
        print(child, regs);
        break;
    case 4:
        setcursor(child, regs);
        break;
    default:
        EMUERR("invalid syscall number");
        break;
    }
}

static void definecolors()
{
    /* The terminal may support 8 colors only, so implemented a workaround... */

    NERR(init_pair(COL_OFF + 0,  COLOR_BLACK,   COLOR_WHITE)); // BLACK
    NERR(init_pair(COL_OFF + 1,  COLOR_BLUE,    COLOR_BLACK)); // BLUE
    NERR(init_pair(COL_OFF + 2,  COLOR_GREEN,   COLOR_BLACK)); // GREEN
    NERR(init_pair(COL_OFF + 3,  COLOR_CYAN,    COLOR_BLACK)); // TURQUOISE
    NERR(init_pair(COL_OFF + 4,  COLOR_RED,     COLOR_BLACK)); // RED
    NERR(init_pair(COL_OFF + 5,  COLOR_MAGENTA, COLOR_BLACK)); // PINK
    NERR(init_pair(COL_OFF + 6,  COLOR_YELLOW,  COLOR_BLACK)); // YELLOW
    NERR(init_pair(COL_OFF + 7,  COLOR_WHITE,   COLOR_CYAN));  // LIGHT GRAY
    NERR(init_pair(COL_OFF + 8,  COLOR_WHITE,   COLOR_MAGENTA)); // DARK GRAY
    NERR(init_pair(COL_OFF + 9,  COLOR_BLUE,    COLOR_WHITE)); // BLUE (BRIGHT)
    NERR(init_pair(COL_OFF + 10, COLOR_GREEN,   COLOR_WHITE)); // GREEN (BRIGHT)
    NERR(init_pair(COL_OFF + 11, COLOR_CYAN,    COLOR_WHITE)); // TURQUOISE(BRIGHT)
    NERR(init_pair(COL_OFF + 12, COLOR_RED,     COLOR_WHITE)); // RED (BRIGHT)
    NERR(init_pair(COL_OFF + 13, COLOR_MAGENTA, COLOR_WHITE)); // PINK (BRIGHT)
    NERR(init_pair(COL_OFF + 14, COLOR_YELLOW,  COLOR_WHITE)); // YELLOW (BRIGHT)
    NERR(init_pair(COL_OFF + 15, COLOR_WHITE,   COLOR_BLACK)); // WHITE
}

static void exitncurses() { endwin(); }

static void initncurses()
{
    int x, y;

    initscr();
    atexit(exitncurses);

    getmaxyx(stdscr, y, x);
    if (x != MAX_X || y != MAX_Y)
        EMUERR("The terminal size is not 80x24");

    NERR(noecho());
    NERR(cbreak());
    NERR(keypad(stdscr, 1));

    if (!has_colors())
        EMUERR("The terminal does not support color");

    NERR(start_color());
    definecolors();
}

int main(int argc, char *argv[])
{
    int r;
    pid_t child;
    int status;

    child = fork();
    SYSERR(child, "fork");

    if (child == 0)
    {
        argv[0] = LOADER_PATH;

        r = execvp(argv[0], argv);
        SYSERR(r, "execvp");
    }

    /* Wait until loader does TRACEME and raises SIGSTOP. */
    r = waitpid(child, &status, __WALL);
    SYSERR(r, "1. waitpid");

    /* If the loader exited because of an error, propagate it. */
    if (WIFEXITED(status))
        exit(WEXITSTATUS(status));

    if (WIFSIGNALED(status))
        EMUERR("The loader was terminated by a signal");

    /* Assume the loader was stopped by its own SIGSTOP. */
    if (WIFSTOPPED(status))
    {
        r = ptrace(PTRACE_SETOPTIONS, child, NULL,
                   PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD);
        SYSERR(r, "PTRACE_SETOPTIONS");
    }
    else EMUERR("Impossible...");

    initncurses();

    /* Start the alien program emulation. */
    for (;;)
    {
        r = ptrace(PTRACE_SYSEMU, child, NULL, NULL);
        SYSERR(r, "PTRACE_SYSEMU");

        r = waitpid(child, &status, __WALL);
        SYSERR(r, "waitpid");

        if (WIFSIGNALED(status))
            EMUERR("The alien program was terminated by a signal");

        if (WIFSTOPPED(status)) {
            int signum;

            signum = WSTOPSIG(status);

            if (signum != (SIGTRAP | 0x80))
                EMUERR("An unexpected signal delivered to the alien program");

            handlesyscall(child);
        }
    }
}
