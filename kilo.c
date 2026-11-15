/***	includes	***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/***	defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)

/***	data	***/
struct editorConfig {
	int screen_rows;
	int screen_cols;
	struct termios orig_attr;
};

struct editorConfig E;

/***	terminal	***/
void die(const char *s){
	// clear screen on exit, and set cursor to starting position
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode(){
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_attr) == -1) die("tcsetattr");
}

void enableRawMode(){
	if(tcgetattr(STDIN_FILENO, &E.orig_attr) == -1) die("tcgetattr");
	atexit(disableRawMode);
	struct termios raw = E.orig_attr;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey(){
	int nread;
	char c;
	while((nread=read(STDIN_FILENO, &c, 1)) != 1){
		if(nread== -1 && errno != EAGAIN) die("read");
	}
	return c;
}

int getCursorPosition(int *rows, int *cols) {
	if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
	// the n command (https://vt100.net/docs/vt100-ug/chapter3.html#DSR)
	// used to query terminal for status information
	// argument 6 asks for cursor position
	printf("\r\n");
	char c;
	while(read(STDIN_FILENO, &c, 1) == 1){
		if(iscntrl(c)){
			printf("%d\r\n", c);
		} else {
			printf("%d ('%c')\r\n", c, c);
		}
	}
	editorReadKey();
	return -1;
}

int getWindowSize(int *rows, int* cols){
	struct winsize ws;
	// we init the struct and then fill in values using ioctl
	// the the meaning of TIOCGWINSZ is
	// Terminal IOCtl Get WINdow SiZe
	if(1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		// failback incase TIOCGWINSZ doesn't work.
		if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		// the escape sequence 999B (move cursor right) and 999C (move cursor down)
		// 999 specifies move the cursor by how much.
		// the reason we don't use 999;999H here is, it might move the cursor past the screen and we don't know what will happen.
		// the C and B commands make sure it doesn't move past the screen.
		return getCursorPosition(rows, cols);
	} else {
		// then we set the values we got into the global struct.
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/***	output	***/
void editorDrawRows(){
	// printing ~ at the begining of everyline like vim.
	int y;
	for(y = 0; y < E.screen_rows; y++){
		write(STDOUT_FILENO, "~\r\n", 3);
	}
}

void editorRefreshScreen(){
	write(STDOUT_FILENO, "\x1b[2J", 4);
	// clears the screen. the \x1b[2J is a escape sequence.
	// escape sequences start with a \x1b (27 in hex) followed by [
	// here J command takes an argument and then instructs the terminal screen to clear itself.
	// 0J would clean the screen from cursor to the end of the screen.
	// 1J would clean the screen upto the cursor.
	// 2j in this case would clear the entire screen.

	// VT100 User Guide (https://vt100.net/docs/vt100-ug/chapter3.html)

	write(STDOUT_FILENO, "\x1b[H", 3);
	// another escape sequence, the H command takes 2 arguments and reposition's the cursor.
	// eg the terminal is of size 80x24, to position it into center of the screen we do <esc>[12;40H
	// multiple arguments can be separated by ;
	// the default arguments are 1;1 so we can just type [H and it would have the same result.
	// rows and cols start from 1 and not from 0.

	editorDrawRows();
	write(STDOUT_FILENO, "\x1b[H", 3);
}

/***	input	***/
void editorProcessKeypress(){
	char c = editorReadKey();

	switch (c){
		case CTRL_KEY('q'):
			// clear screen and place the cursor at the starting.
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
	}
}

/***	init	***/
void initEditor(){
	// getting size of terminal
	if(getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
}

int main() {
	enableRawMode();
	initEditor();

	while(1){
		editorRefreshScreen();
		editorProcessKeypress();
	}
	return 0;
}
