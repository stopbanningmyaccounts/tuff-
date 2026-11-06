
#include <unistd.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 10000
#define MAX_COLS  1024
#define TAB       4
#define UNDO_MAX  200
#define CTRL(k)   ((k) & 0x1f)

static char *lines[MAX_LINES];
static int nlines = 0;
static int cx = 0, cy = 0;          
static int top = 0, left = 0;       
static char filename[256] = "";     
static char status[320] = "";
static int dirty = 0;
static int chain = 0;               
static char needle[128] = "";       
static char clip[MAX_COLS];         
static int have_clip = 0;

static char *new_line(void) { return calloc(MAX_COLS, 1); }

static void clamp_cx(void) {
    int len = strlen(lines[cy]);
    if (cx > len) cx = len;
}

static int rx_of(const char *l, int upto) {
    int r = 0;
    for (int i = 0; i < upto && l[i]; i++)
        r = (l[i] == '\t') ? (r / TAB + 1) * TAB : r + 1;
    return r;
}

static void expand(const char *l, char *out) {
    int r = 0;
    for (; *l; l++) {
        if (*l == '\t') { int n = TAB - r % TAB; while (n--) out[r++] = ' '; }
        else out[r++] = *l;
    }
    out[r] = 0;
}


static int use_tabs(void) {
    const char *b = strrchr(filename, '/');
    b = b ? b + 1 : filename;
    size_t n = strlen(b);
    return !strcmp(b, "Makefile") || !strcmp(b, "makefile") || !strcmp(b, "GNUmakefile")
        || (n > 3 && !strcmp(b + n - 3, ".mk"));
}


static int load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(status, sizeof status, "New file"); return 0; }
    char buf[MAX_COLS];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        if ((n == sizeof buf - 1 && buf[n - 1] != '\n') || nlines >= MAX_LINES) {
            fclose(f);
            return -1;      
        }
        buf[strcspn(buf, "\r\n")] = 0;
        lines[nlines] = new_line();
        strcpy(lines[nlines++], buf);
    }
    fclose(f);
    return 0;
}

static int save(void) {
    FILE *f = fopen(filename, "w");
    if (!f) { snprintf(status, sizeof status, "Cannot write %s", filename); return -1; }
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
    dirty = 0;
    snprintf(status, sizeof status, "Saved");
    return 0;
}

static int prompt(const char *label, char *buf, size_t size) {
    size_t len = strlen(buf);
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        attron(A_REVERSE);
        mvhline(h - 1, 0, ' ', w);
        mvprintw(h - 1, 0, " %s%s", label, buf);
        attroff(A_REVERSE);
        move(h - 1, 1 + (int)strlen(label) + (int)len);
        refresh();

        int c = getch();
        if (c == '\n' || c == '\r' || c == KEY_ENTER) return len > 0;
        if (c == 27 || c == CTRL('c') || c == CTRL('q')) return 0;
        if (c == KEY_BACKSPACE || c == 127 || c == 8) {
            if (len > 0) buf[--len] = 0;
        } else if (c >= 32 && c < 127 && len < size - 1) {
            buf[len++] = (char)c;
            buf[len] = 0;
        }
    }
}

static int save_as(void) {
    char name[256];
    snprintf(name, sizeof name, "%s", filename);
    if (!prompt("Save as: ", name, sizeof name)) {
        snprintf(status, sizeof status, "Cancelled");
        return -1;
    }
    if (strcmp(name, filename) != 0 && access(name, F_OK) == 0) {
        char ans[8] = "";
        if (!prompt("File exists. Overwrite? (type y): ", ans, sizeof ans) || ans[0] != 'y') {
            snprintf(status, sizeof status, "Cancelled");
            return -1;
        }
    }
    char old[256];
    snprintf(old, sizeof old, "%s", filename);
    snprintf(filename, sizeof filename, "%s", name);
    if (save() != 0) {
        snprintf(filename, sizeof filename, "%s", old);
        return -1;
    }
    return 0;
}

static int save_current(void) {
    return filename[0] ? save() : save_as();
}

typedef struct { char *text; int cx, cy; } Snap;
static Snap undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_n = 0, redo_n = 0;

static Snap snap_take(void) {
    size_t total = 1;
    for (int i = 0; i < nlines; i++) total += strlen(lines[i]) + 1;
    char *t = malloc(total), *p = t;
    if (!t) return (Snap){ NULL, cx, cy };
    for (int i = 0; i < nlines; i++) {
        size_t n = strlen(lines[i]);
        memcpy(p, lines[i], n);
        p += n;
        *p++ = '\n';
    }
    *p = 0;
    return (Snap){ t, cx, cy };
}

static void snap_restore(const Snap *s) {
    for (int i = 0; i < nlines; i++) free(lines[i]);
    nlines = 0;
    const char *p = s->text;
    while (*p && nlines < MAX_LINES) {
        const char *e = strchr(p, '\n');
        size_t n = (size_t)(e - p);
        lines[nlines] = new_line();
        memcpy(lines[nlines++], p, n);
        p = e + 1;
    }
    if (nlines == 0) lines[nlines++] = new_line();
    cy = s->cy; cx = s->cx;
    if (cy >= nlines) cy = nlines - 1;
    clamp_cx();
}

static void clear_snaps(Snap *st, int *n) {
    for (int i = 0; i < *n; i++) free(st[i].text);
    *n = 0;
}

static void push_undo(void) {
    Snap s = snap_take();
    if (!s.text) return;
    if (undo_n == UNDO_MAX) {
        free(undo_stack[0].text);
        memmove(&undo_stack[0], &undo_stack[1], (UNDO_MAX - 1) * sizeof(Snap));
        undo_n--;
    }
    undo_stack[undo_n++] = s;
    clear_snaps(redo_stack, &redo_n);
}

static void begin_edit(int kind) {
    if (!kind || chain != kind) push_undo();
    chain = kind;
}

static void undo(void) {
    if (!undo_n) { snprintf(status, sizeof status, "Nothing to undo"); return; }
    Snap cur = snap_take();
    if (!cur.text) { snprintf(status, sizeof status, "Out of memory"); return; }
    redo_stack[redo_n++] = cur;
    Snap s = undo_stack[--undo_n];
    snap_restore(&s);
    free(s.text);
    dirty = 1;
}

static void redo(void) {
    if (!redo_n) { snprintf(status, sizeof status, "Nothing to redo"); return; }
    Snap cur = snap_take();
    if (!cur.text) { snprintf(status, sizeof status, "Out of memory"); return; }
    undo_stack[undo_n++] = cur;
    Snap s = redo_stack[--redo_n];
    snap_restore(&s);
    free(s.text);
    dirty = 1;
}

static void insert_char(int c) {
    char *l = lines[cy];
    int len = strlen(l);
    if (len >= MAX_COLS - 1) return;
    memmove(l + cx + 1, l + cx, len - cx + 1);
    l[cx++] = (char)c;
    dirty = 1;
}

static void maybe_dedent(void) {
    for (int i = 0; i < cx; i++)
        if (lines[cy][i] != ' ') return;
    if (cx >= TAB) {
        memmove(lines[cy] + cx - TAB, lines[cy] + cx, strlen(lines[cy] + cx) + 1);
        cx -= TAB;
    }
}

static void insert_tab(void) {
    begin_edit(0);
    if (use_tabs()) { insert_char('\t'); return; }
    int n = TAB - rx_of(lines[cy], cx) % TAB;   
    while (n--) insert_char(' ');
}


static void insert_newline(void) {
    if (nlines >= MAX_LINES) return;
    char *cur = lines[cy];
    int ind = 0;
    while (cur[ind] == ' ' || cur[ind] == '\t') ind++;
    if (ind > cx) ind = cx;
    int extra = (cx > 0 && cur[cx - 1] == '{') ? TAB : 0;
    if (ind + extra + (int)strlen(cur + cx) >= MAX_COLS) return;

    begin_edit(0);
    char *nl = new_line();
    memcpy(nl, cur, ind);
    memset(nl + ind, ' ', extra);
    strcpy(nl + ind + extra, cur + cx);          
    cur[cx] = 0;                                 
    memmove(&lines[cy + 2], &lines[cy + 1], (nlines - cy - 1) * sizeof(char *));
    lines[cy + 1] = nl;
    nlines++; cy++; cx = ind + extra;
    dirty = 1;
}

static void backspace(void) {
    if (cx > 0) {
        begin_edit(2);
        memmove(lines[cy] + cx - 1, lines[cy] + cx, strlen(lines[cy] + cx) + 1);
        cx--;
        dirty = 1;
    } else if (cy > 0) {
        int plen = strlen(lines[cy - 1]);
        if (plen + (int)strlen(lines[cy]) >= MAX_COLS) return;
        begin_edit(2);
        strcat(lines[cy - 1], lines[cy]);
        free(lines[cy]);
        memmove(&lines[cy], &lines[cy + 1], (nlines - cy - 1) * sizeof(char *));
        nlines--; cy--; cx = plen;
        dirty = 1;
    }
}

static void delete_char(void) {
    int len = strlen(lines[cy]);
    if (cx < len) {
        begin_edit(0);
        memmove(lines[cy] + cx, lines[cy] + cx + 1, len - cx);
        dirty = 1;
    } else if (cy < nlines - 1) {
        if (len + (int)strlen(lines[cy + 1]) >= MAX_COLS) return;
        begin_edit(0);
        strcat(lines[cy], lines[cy + 1]);
        free(lines[cy + 1]);
        memmove(&lines[cy + 1], &lines[cy + 2], (nlines - cy - 2) * sizeof(char *));
        nlines--;
        dirty = 1;
    }
}

static void cut_line(void) {
    strcpy(clip, lines[cy]);
    have_clip = 1;
    begin_edit(0);
    if (nlines == 1) {
        lines[0][0] = 0;
    } else {
        free(lines[cy]);
        memmove(&lines[cy], &lines[cy + 1], (nlines - cy - 1) * sizeof(char *));
        nlines--;
        if (cy >= nlines) cy = nlines - 1;
    }
    cx = 0;
    clamp_cx();
    dirty = 1;
}

static void copy_line(void) {
    strcpy(clip, lines[cy]);
    have_clip = 1;
    snprintf(status, sizeof status, "Line copied");
}

static void paste_line(void) {
    if (!have_clip) { snprintf(status, sizeof status, "Clipboard empty"); return; }
    if (nlines >= MAX_LINES) return;
    begin_edit(0);
    memmove(&lines[cy + 1], &lines[cy], (nlines - cy) * sizeof(char *));
    lines[cy] = new_line();
    strcpy(lines[cy], clip);
    nlines++; cy++;
    dirty = 1;
}

static void dup_line(void) {
    if (nlines >= MAX_LINES) return;
    begin_edit(0);
    memmove(&lines[cy + 2], &lines[cy + 1], (nlines - cy - 1) * sizeof(char *));
    lines[cy + 1] = new_line();
    strcpy(lines[cy + 1], lines[cy]);
    nlines++; cy++;
    dirty = 1;
}

static int find_next(int inclusive) {
    if (!needle[0]) { snprintf(status, sizeof status, "No search string (Ctrl-F)"); return 0; }
    for (int k = 0; k <= nlines; k++) {
        int y = (cy + k) % nlines;
        int off = (k == 0) ? cx + (inclusive ? 0 : 1) : 0;
        if (off > (int)strlen(lines[y])) continue;
        char *p = strstr(lines[y] + off, needle);
        if (p) {
            if (k > 0 && y <= cy) snprintf(status, sizeof status, "Search wrapped");
            cy = y;
            cx = (int)(p - lines[y]);
            return 1;
        }
    }
    snprintf(status, sizeof status, "Not found: %s", needle);
    return 0;
}

static void find_prompt(void) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", needle);     
    if (!prompt("Find: ", tmp, sizeof tmp)) return;
    snprintf(needle, sizeof needle, "%s", tmp);
    find_next(1);
}

static void goto_line(void) {
    char buf[16] = "";
    if (!prompt("Go to line: ", buf, sizeof buf)) return;
    int n = atoi(buf);
    if (n < 1) n = 1;
    if (n > nlines) n = nlines;
    cy = n - 1;
    cx = 0;
}

static int gutter_width(void) {
    int d = 1;
    for (int n = nlines; n >= 10; n /= 10) d++;
    return d + 1;                                
}

static void draw(void) {
    static char ex[MAX_COLS * TAB + 1];
    int h, w;
    getmaxyx(stdscr, h, w);
    int rows = h - 1;
    int gw = gutter_width();
    int tw = w - gw;
    if (tw < 1) tw = 1;
    int rx = rx_of(lines[cy], cx);

    if (cy < top) top = cy;
    if (cy >= top + rows) top = cy - rows + 1;
    if (rx < left) left = rx;
    if (rx >= left + tw) left = rx - tw + 1;

    erase();
    for (int i = 0; i < rows && top + i < nlines; i++) {
        int ln = top + i;
        attron(A_DIM);
        mvprintw(i, 0, "%*d ", gw - 1, ln + 1);
        attroff(A_DIM);
        expand(lines[ln], ex);
        if ((int)strlen(ex) > left) mvaddnstr(i, gw, ex + left, tw);
    }

    attron(A_REVERSE);
    mvhline(h - 1, 0, ' ', w);
    mvprintw(h - 1, 0, " %s%s  Ln %d/%d, Col %d  | F1 help | %s",
             filename[0] ? filename : "[No Name]", dirty ? "*" : "",
             cy + 1, nlines, rx + 1, status);
    attroff(A_REVERSE);
    move(cy - top, gw + rx - left);
    refresh();
}

static void help(void) {
    static const char *keys[] = {
        "Ctrl-S  save                   Ctrl-O  save as",
        "Ctrl-B or F5  save, compile with gcc (g++ for .cpp/.cc) and run",
        "Ctrl-Q  quit (press twice if there are unsaved changes)",
        "",
        "Ctrl-Z  undo                   Ctrl-R  redo",
        "Ctrl-K  cut line               Ctrl-Y  copy line",
        "Ctrl-U  paste line above       Ctrl-D  duplicate line",
        "",
        "Ctrl-F  find                   Ctrl-N  find next",
        "Ctrl-G  go to line",
        "",
        "Arrows, Home, End, PgUp, PgDn, Delete, Backspace work as usual.",
        "Enter keeps the indent (and adds a level after '{').",
        "Typing '}' at the start of an indented line steps the indent back.",
        "Tab inserts spaces to the next stop (real tabs in Makefiles).",
    };
    int n = (int)(sizeof keys / sizeof keys[0]);
    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "tuff keys");
    attroff(A_BOLD);
    for (int i = 0; i < n; i++) mvprintw(2 + i, 2, "%s", keys[i]);
    mvprintw(3 + n, 2, "Press any key to return");
    refresh();
    getch();
}

static void build_and_run(void) {
    if (save_current() != 0) return;
    char out[300], cmd[1024];
    snprintf(out, sizeof out, "%s", filename);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0; else strcat(out, ".out");
    const char *pre = (out[0] == '/') ? "" : "./";

    const char *cc = "gcc";
    size_t fl = strlen(filename);
    if ((fl > 4 && !strcmp(filename + fl - 4, ".cpp")) ||
        (fl > 3 && !strcmp(filename + fl - 3, ".cc")))
        cc = "g++";

    snprintf(cmd, sizeof cmd, "%s -Wall -Wextra -g \"%s\" -o \"%s\" && %s\"%s\"",
             cc, filename, out, pre, out);

    def_prog_mode();
    endwin();
    printf("$ %s\n\n", cmd);
    int rc = system(cmd);
    printf("\n[exit status %d] Press Enter to return to editor...", rc);
    fflush(stdout);
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF) {}
    reset_prog_mode();
    refresh();
    snprintf(status, sizeof status, "Build %s", rc == 0 ? "OK" : "failed/ran with errors");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        snprintf(filename, sizeof filename, "%s", argv[1]);
        if (load(filename) != 0) {
            fprintf(stderr, "ted: %s is too large for this editor "
                            "(max %d lines, %d chars per line)\n",
                    filename, MAX_LINES, MAX_COLS - 1);
            return 1;
        }
    }
    if (nlines == 0) lines[nlines++] = new_line();

    initscr();
    set_escdelay(25);       
    raw(); 
    noecho();
    keypad(stdscr, TRUE);

    int quit_warned = 0;
    for (;;) {
        draw();
        int c = getch();
        status[0] = 0;
        int kind = (c >= 32 && c < 127) ? 1
                 : (c == KEY_BACKSPACE || c == 127 || c == 8) ? 2 : 0;
        if (kind != chain) chain = 0;       
        if (c != CTRL('q')) quit_warned = 0;

        switch (c) {
        case CTRL('q'):
            if (dirty && !quit_warned) {
                snprintf(status, sizeof status, "Unsaved changes! Ctrl-Q again to quit");
                quit_warned = 1;
                break;
            }
            endwin();
            return 0;

        case CTRL('s'): save_current(); break;
        case CTRL('o'): save_as(); break;
        case CTRL('b'):
        case KEY_F(5):  build_and_run(); break;

        case CTRL('z'): undo(); break;
        case CTRL('r'): redo(); break;
        case CTRL('k'): cut_line(); break;
        case CTRL('y'): copy_line(); break;
        case CTRL('u'): paste_line(); break;
        case CTRL('d'): dup_line(); break;
        case CTRL('f'): find_prompt(); break;
        case CTRL('n'): find_next(0); break;
        case CTRL('g'): goto_line(); break;
        case KEY_F(1):
        case CTRL('t'): help(); break;

        case KEY_UP:    if (cy > 0) cy--; clamp_cx(); break;
        case KEY_DOWN:  if (cy < nlines - 1) cy++; clamp_cx(); break;
        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = strlen(lines[cy]); }
            break;
        case KEY_RIGHT:
            if (cx < (int)strlen(lines[cy])) cx++;
            else if (cy < nlines - 1) { cy++; cx = 0; }
            break;
        case KEY_HOME:  cx = 0; break;
        case KEY_END:   cx = strlen(lines[cy]); break;
        case KEY_PPAGE: cy -= 20; if (cy < 0) cy = 0; clamp_cx(); break;
        case KEY_NPAGE: cy += 20; if (cy >= nlines) cy = nlines - 1; clamp_cx(); break;

        case KEY_BACKSPACE: case 127: case 8: backspace(); break;
        case KEY_DC:    delete_char(); break;
        case '\n': case '\r': case KEY_ENTER: insert_newline(); break;
        case '\t':      insert_tab(); break;
        default:
            if (c >= 32 && c < 127) {
                begin_edit(1);
                if (c == '}') maybe_dedent();
                insert_char(c);
            }
        }
    }
}
