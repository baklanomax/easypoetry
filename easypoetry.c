/* includes */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* defines */

#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

enum editor_highlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/* data */

struct editor_syntax {
	char *filetype;
	char **filematch;
	char **keywords;
	char *singleline_comment_start;
	char *multiline_comment_start;
	char *multiline_comment_end;
	int flags;
};

typedef struct erow {
	int idx;
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *hl;
	int hl_open_comment;
} erow;

struct editor_config {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct editor_syntax *syntax;
	struct termios orig_termios;
};

struct editor_config e;

/* filetypes */

char *C_HL_extensions[] = {
	".c",
	".h",
	".cpp",
	NULL
};

char *C_HL_keywords[] = {
	"switch",
	"if",
	"while",
	"for",
	"break",
	"continue",
	"return",
	"else",
	"struct",
	"union",
	"typedef",
	"static",
	"enum",
	"class",
	"case",
	"int|",
	"long|",
	"double|",
	"float|",
	"char|",
	"unsigned|",
	"signed|",
	"void|",
	NULL
};

struct editor_syntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//",
		"/*",
		"*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* prototypes */

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

/* terminal */

void die_cur(const char *s) {
	perror(s);
	write(STDOUT_FILENO, "\x1b[999D", 6);
	exit(1);
}

void die_last(const char *s) {
	write(STDOUT_FILENO, "\x1b[999B", 6);
	write(STDOUT_FILENO, "\x1b[999D", 6);
	write(STDOUT_FILENO, "\x1b[2K", 4);
	perror(s);
	write(STDOUT_FILENO, "\x1b[999D", 6);
	exit(1);
}

void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &e.orig_termios) == -1)
		die_last("tcsetattr");
}

void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &e.orig_termios) == -1)
		die_cur("tcgetattr");
	atexit(disable_raw_mode);
	struct termios raw = e.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die_cur("tcsetattr");
}

int editor_read_key() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
		if (nread == -1 && errno != EAGAIN)
			die_last("read");
	if (c == '\x1b') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';
		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~')
					switch (seq[1]) {
						case '1':
							return HOME_KEY;

						case '3':
							return DEL_KEY;

						case '4':
							return END_KEY;

						case '5':
							return PAGE_UP;

						case '6':
							return PAGE_DOWN;

						case '7':
							return HOME_KEY;

						case '8':
							return END_KEY;
					}
			} else
				switch (seq[1]) {
					case 'A':
						return ARROW_UP;

					case 'B':
						return ARROW_DOWN;

					case 'C':
						return ARROW_RIGHT;

					case 'D':
						return ARROW_LEFT;

					case 'H':
						return HOME_KEY;

					case 'F':
						return END_KEY;
				}
		} else if (seq[0] == 'O')
			switch (seq[1]) {
				case 'H':
					return HOME_KEY;

				case 'F':
					return END_KEY;
			}
		return '\x1b';
	} else
		return c;
}

int get_cursor_position(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';
	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;
	return 0;
}

int get_window_size(int *rows, int *cols) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/* syntax highlighting */

int is_separator(int c) {
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(erow *row) {
	row->hl = realloc(row->hl, row->rsize);
	memset(row->hl, HL_NORMAL, row->rsize);
	if (e.syntax == NULL)
		return;
	char **keywords = e.syntax->keywords;
	char *scs = e.syntax->singleline_comment_start;
	char *mcs = e.syntax->multiline_comment_start;
	char *mce = e.syntax->multiline_comment_end;
	int scs_len = scs ? strlen(scs) : 0;
	int mcs_len = mcs ? strlen(mcs) : 0;
	int mce_len = mce ? strlen(mce) : 0;
	int prev_sep = 1;
	int in_string = 0;
	int in_comment = (row->idx > 0 && e.row[row->idx - 1].hl_open_comment);
	int i = 0;
	while (i < row->rsize) {
		char c = row->render[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
		if (scs_len && !in_string && !in_comment)
			if (!strncmp(&row->render[i], scs, scs_len)) {
				memset(&row->hl[i], HL_COMMENT, row->rsize - i);
				break;
			}
		if (mcs_len && mce_len && !in_string) {
			if (in_comment) {
				row->hl[i] = HL_MLCOMMENT;
				if (!strncmp(&row->render[i], mce, mce_len)) {
					memset(&row->hl[i], HL_MLCOMMENT, mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				} else {
					i++;
					continue;
				}
			} else if (!strncmp(&row->render[i], mcs, mcs_len)) {
				memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}
		if (e.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				if (c == '\\' && i + 1 < row->rsize) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string)
					in_string = 0;
				i++;
				prev_sep = 1;
				continue;
			} else if (c == '"' || c == '\'') {
				in_string = c;
				row->hl[i] = HL_STRING;
				i++;
				continue;
			}
		}
		if (e.syntax->flags & HL_HIGHLIGHT_NUMBERS)
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		if (prev_sep) {
			int j;
			for (j = 0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2)
					klen--;
				if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
					memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue;
			}
		}
		prev_sep = is_separator(c);
		i++;
	}
	int changed = (row->hl_open_comment != in_comment);
	row->hl_open_comment = in_comment;
	if (changed && row->idx + 1 < e.numrows)
		editor_update_syntax(&e.row[row->idx + 1]);
}

int editor_syntax_to_color(int hl) {
	switch (hl) {
		case HL_COMMENT:
		case HL_MLCOMMENT:
			return 36;

		case HL_KEYWORD1:
			return 33;

		case HL_KEYWORD2:
			return 32;

		case HL_STRING:
			return 35;

		case HL_NUMBER:
			return 31;

		case HL_MATCH:
			return 34;

		default:
			return 37;
	}
}

void editor_select_syntax_highlight() {
	e.syntax = NULL;
	if (e.filename == NULL)
		return;
	char *ext = strrchr(e.filename, '.');
	for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
		struct editor_syntax *s = &HLDB[j];
		unsigned int i = 0;
		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');
			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(e.filename, s->filematch[i]))) {
				e.syntax = s;
				int filerow;
				for (filerow = 0; filerow < e.numrows; filerow++)
					editor_update_syntax(&e.row[filerow]);
				return;
			}
			i++;
		}
	}
}

/* row operations */

int editor_row_cx_to_rx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

int editor_row_rx_to_cx(erow *row, int rx) {
	int cur_rx = 0;
	int cx;
	for (cx = 0; cx < row->size; cx++) {
		if (row->chars[cx] == '\t')
			cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
		cur_rx++;
		if (cur_rx > rx)
			return cx;
	}
	return cx;
}

void editor_update_row(erow *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t')
			tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);
	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0)
				row->render[idx++] = ' ';
		} else
			row->render[idx++] = row->chars[j];
	}
	row->render[idx] = '\0';
	row->rsize = idx;
	editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len) {
	if (at < 0 || at > e.numrows)
		return;
	e.row = realloc(e.row, sizeof(erow) * (e.numrows + 1));
	memmove(&e.row[at + 1], &e.row[at], sizeof(erow) * (e.numrows - at));
	for (int j = at + 1; j <= e.numrows; j++)
		e.row[j].idx++;
	e.row[at].idx = at;
	e.row[at].size = len;
	e.row[at].chars = malloc(len + 1);
	memcpy(e.row[at].chars, s, len);
	e.row[at].chars[len] = '\0';
	e.row[at].rsize = 0;
	e.row[at].render = NULL;
	e.row[at].hl = NULL;
	e.row[at].hl_open_comment = 0;
	editor_update_row(&e.row[at]);
	e.numrows++;
	e.dirty++;
}

void editor_free_row(erow *row) {
	free(row->render);
	free(row->chars);
	free(row->hl);
}

void editor_del_row(int at) {
	if (at < 0 || at >= e.numrows)
		return;
	editor_free_row(&e.row[at]);
	memmove(&e.row[at], &e.row[at + 1], sizeof(erow) * (e.numrows - at - 1));
	for (int j = at; j < e.numrows - 1; j++)
		e.row[j].idx--;
	e.numrows--;
	e.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c) {
	if (at < 0 || at > row->size)
		at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editor_update_row(row);
	e.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editor_update_row(row);
	e.dirty++;
}

void editor_row_del_char(erow *row, int at) {
	if (at < 0 || at >= row->size)
		return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editor_update_row(row);
	e.dirty++;
}

/* editor operations */

void editor_insert_char(int c) {
	if (e.cy == e.numrows)
		editor_insert_row(e.numrows, "", 0);
	editor_row_insert_char(&e.row[e.cy], e.cx, c);
	e.cx++;
}

void editor_insert_newline() {
	if (e.cx == 0)
		editor_insert_row(e.cy, "", 0);
	else {
		erow *row = &e.row[e.cy];
		editor_insert_row(e.cy + 1, &row->chars[e.cx], row->size - e.cx);
		row = &e.row[e.cy];
		row->size = e.cx;
		row->chars[row->size] = '\0';
		editor_update_row(row);
	}
	e.cy++;
	e.cx = 0;
}

void editor_del_char() {
	if (e.cy == e.numrows)
		return;
	if (e.cx == 0 && e.cy == 0)
		return;
	erow *row = &e.row[e.cy];
	if (e.cx > 0) {
		editor_row_del_char(row, e.cx - 1);
		e.cx--;
	} else {
		e.cx = e.row[e.cy - 1].size;
		editor_row_append_string(&e.row[e.cy - 1], row->chars, row->size);
		editor_del_row(e.cy);
		e.cy--;
	}
}

/* file i/o */

char *editor_rows_to_string(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < e.numrows; j++)
		totlen += e.row[j].size + 1;
	*buflen = totlen;
	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < e.numrows; j++) {
		memcpy(p, e.row[j].chars, e.row[j].size);
		p += e.row[j].size;
		*p = '\n';
		p++;
	}
	return buf;
}

void editor_open(char *filename) {
	free(e.filename);
	e.filename = strdup(filename);
	editor_select_syntax_highlight();
	FILE *fp = fopen(filename, "r");
	if (!fp)
		die_cur("fopen");
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		editor_insert_row(e.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	e.dirty = 0;
}

void editor_save() {
	if (e.filename == NULL) {
		e.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
		if (e.filename == NULL) {
			editor_set_status_message("Save aborted");
			return;
		}
		editor_select_syntax_highlight();
	}
	int len;
	char *buf = editor_rows_to_string(&len);
	int fd = open(e.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1)
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				e.dirty = 0;
				editor_set_status_message("%d bytes written to disk", len);
				return;
			}
		close(fd);
	}
	free(buf);
	editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/* find */

void editor_find_callback(char *query, int key) {
	static int last_match = -1;
	static int direction = 1;
	static int saved_hl_line;
	static char *saved_hl = NULL;
	if (saved_hl) {
		memcpy(e.row[saved_hl_line].hl, saved_hl, e.row[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl = NULL;
	}
	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	}
	else if (key == ARROW_RIGHT || key == ARROW_DOWN)
		direction = 1;
	else if (key == ARROW_LEFT || key == ARROW_UP)
		direction = -1;
	else {
		last_match = -1;
		direction = 1;
	}
	if (last_match == -1)
		direction = 1;
	int current = last_match;
	int i;
	for (i = 0; i < e.numrows; i++) {
		current += direction;
		if (current == -1)
			current = e.numrows - 1;
		else if (current == e.numrows)
			current = 0;
		erow *row = &e.row[current];
		char *match = strstr(row->render, query);
		if (match) {
			last_match = current;
			e.cy = current;
			e.cx = editor_row_rx_to_cx(row, match - row->render);
			e.rowoff = e.numrows;
			saved_hl_line = current;
			saved_hl = malloc(row->rsize);
			memcpy(saved_hl, row->hl, row->rsize);
			memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editor_find() {
	int saved_cx = e.cx;
	int saved_cy = e.cy;
	int saved_coloff = e.coloff;
	int saved_rowoff = e.rowoff;
	char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);
	if (query)
		free(query);
	else {
		e.cx = saved_cx;
		e.cy = saved_cy;
		e.coloff = saved_coloff;
		e.rowoff = saved_rowoff;
	}
}

/* append buffer */

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void ab_append(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);
	if (new == NULL)
		return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf *ab) {
	free(ab->b);
}

/* output */

void editor_scroll() {
	e.rx = 0;
	if (e.cy < e.numrows)
		e.rx = editor_row_cx_to_rx(&e.row[e.cy], e.cx);
	if (e.cy < e.rowoff)
		e.rowoff = e.cy;
	if (e.cy >= e.rowoff + e.screenrows)
		e.rowoff = e.cy - e.screenrows + 1;
	if (e.rx < e.coloff)
		e.coloff = e.rx;
	if (e.rx >= e.coloff + e.screencols)
		e.coloff = e.rx - e.screencols + 1;
}

void editor_draw_rows(struct abuf *ab) {
	int y;
	for (y = 0; y < e.screenrows; y++) {
		int filerow = y + e.rowoff;
		if (filerow >= e.numrows) {
			ab_append(ab, "\x1b[94m", 5);
			ab_append(ab, "~", 1);
			ab_append(ab, "\x1b[39m", 5);
		} else {
			int len = e.row[filerow].rsize - e.coloff;
			if (len < 0)
				len = 0;
			if (len > e.screencols)
				len = e.screencols;
			char *c = &e.row[filerow].render[e.coloff];
			unsigned char *hl = &e.row[filerow].hl[e.coloff];
			int current_color = -1;
			int j;
			for (j = 0; j < len; j++) {
				if (iscntrl(c[j])) {
					char sym = (c[j] <= 26) ? '@' + c[j] : '?';
					ab_append(ab, "\x1b[7m", 4);
					ab_append(ab, &sym, 1);
					ab_append(ab, "\x1b[m", 3);
					if (current_color != -1) {
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
						ab_append(ab, buf, clen);
					}
				} else if (hl[j] == HL_NORMAL) {
					if (current_color != -1) {
						ab_append(ab, "\x1b[39m", 5);
						current_color = -1;
					}
					ab_append(ab, &c[j], 1);
				} else {
					int color = editor_syntax_to_color(hl[j]);
					if (color != current_color) {
						current_color = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						ab_append(ab, buf, clen);
					}
					ab_append(ab, &c[j], 1);
				}
			}
			ab_append(ab, "\x1b[39m", 5);
		}
		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

void editor_draw_status_bar(struct abuf *ab) {
	ab_append(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", e.filename ? e.filename : "[No Name]", e.numrows, e.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", e.syntax ? e.syntax->filetype : "no ft", e.cy + 1, e.numrows);
	if (len > e.screencols)
		len = e.screencols;
	ab_append(ab, status, len);
	while (len < e.screencols) {
		if (e.screencols - len == rlen) {
			ab_append(ab, rstatus, rlen);
			break;
		} else {
			ab_append(ab, " ", 1);
			len++;
		}
	}
	ab_append(ab, "\x1b[m", 3);
	ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
	ab_append(ab, "\x1b[K", 3);
	int msglen = strlen(e.statusmsg);
	if (msglen > e.screencols)
		msglen = e.screencols;
	if (msglen && time(NULL) - e.statusmsg_time < 5)
		ab_append(ab, e.statusmsg, msglen);
}

void editor_refresh_screen() {
	editor_scroll();
	struct abuf ab = ABUF_INIT;
	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);
	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_message_bar(&ab);
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (e.cy - e.rowoff) + 1, (e.rx - e.coloff) + 1);
	ab_append(&ab, buf, strlen(buf));
	ab_append(&ab, "\x1b[?25h", 6);
	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e.statusmsg, sizeof(e.statusmsg), fmt, ap);
	va_end(ap);
	e.statusmsg_time = time(NULL);
}

/* input */

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
	size_t bufsize = 128;
	char *buf = malloc(bufsize);
	size_t buflen = 0;
	buf[0] = '\0';
	while (1) {
		editor_set_status_message(prompt, buf);
		editor_refresh_screen();
		int c = editor_read_key();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			if (buflen != 0)
				buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			editor_set_status_message("");
			if (callback)
				callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				editor_set_status_message("");
				if (callback)
					callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
		if (callback)
			callback(buf, c);
	}
}

void editor_move_cursor(int key) {
	erow *row = (e.cy >= e.numrows) ? NULL : &e.row[e.cy];
	switch (key) {
		case ARROW_LEFT:
			if (e.cx != 0)
				e.cx--;
			else if (e.cy > 0) {
				e.cy--;
				e.cx = e.row[e.cy].size;
			}
			break;

		case ARROW_RIGHT:
			if (row && e.cx < row->size)
				e.cx++;
			else if (row && e.cx == row->size) {
				e.cy++;
				e.cx = 0;
			}
			break;

		case ARROW_UP:
			if (e.cy != 0)
				e.cy--;
			break;

		case ARROW_DOWN:
			if (e.cy + 1 < e.numrows)
				e.cy++;
			break;
	}
	row = (e.cy >= e.numrows) ? NULL : &e.row[e.cy];
	int rowlen = row ? row->size : 0;
	if (e.cx > rowlen)
		e.cx = rowlen;
}

void editor_process_keypress() {
	static int quit_times = KILO_QUIT_TIMES;
	int c = editor_read_key();
	switch (c) {
		case '\r':
			editor_insert_newline();
			break;

		case CTRL_KEY('q'):
			if (e.dirty && quit_times > 0) {
				editor_set_status_message("WARNING!!! File has unsaved changes. " "Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[999B", 6);
			write(STDOUT_FILENO, "\x1b[999D", 6);
			write(STDOUT_FILENO, "\x1b[2K", 4);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editor_save();
			break;

		case HOME_KEY:
			e.cx = 0;
			break;

		case END_KEY:
			if (e.cy < e.numrows)
				e.cx = e.row[e.cy].size;
			break;

		case CTRL_KEY('f'):
			editor_find();
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY)
				editor_move_cursor(ARROW_RIGHT);
			editor_del_char();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			if (c == PAGE_UP)
				e.cy = e.rowoff;
			else {
				e.cy = e.rowoff + e.screenrows - 1;
				if (e.cy > e.numrows)
					e.cy = e.numrows;
			}
			int times = e.screenrows;
			while (times--)
				editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editor_move_cursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editor_insert_char(c);
			break;
	}
	quit_times = KILO_QUIT_TIMES;
}

/* init */

void init_editor() {
	e.cx = 0;
	e.cy = 0;
	e.rx = 0;
	e.rowoff = 0;
	e.coloff = 0;
	e.numrows = 0;
	e.row = NULL;
	e.dirty = 0;
	e.filename = NULL;
	e.statusmsg[0] = '\0';
	e.statusmsg_time = 0;
	e.syntax = NULL;
	if (get_window_size(&e.screenrows, &e.screencols) == -1)
		die_cur("get_window_size");
	e.screenrows -= 2;
}

int main(int argc, char *argv[]) {
	enable_raw_mode();
	init_editor();
	if (argc > 1)
		editor_open(argv[1]);
	editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}
	return 0;
}
