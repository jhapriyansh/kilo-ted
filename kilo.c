/***    includes        ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/***    defines ***/
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

/***    data    ***/
struct editorConfig {
        int cx, cy;     // cursor position
        int screen_rows;
        int screen_cols;
        struct termios orig_attr;
};

struct editorConfig E;

/***    terminal        ***/
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
        raw.c_cc[VTIME] = 1; // 0.1s
        if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(){
        int nread;
        char c;
        while((nread=read(STDIN_FILENO, &c, 1)) != 1){
                if(nread== -1 && errno != EAGAIN) die("read");
        }

        if(c=='\x1b'){
                char seq[3];
                if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
                if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

                if(seq[0] == '['){
                        if(seq[1] >='0' && seq[1] <= '9'){
                                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                                if(seq[2] == '~'){
                                        switch(seq[1]){
                                                // PAGE_UP is sent as \x1b[5~
                                                // PAGE_DOWN is sent as \x1b[6~
                                                // HOME_KEY is sent as \x1b[1~ \x1b[7~ \x1b[H \x1b[OH
                                                // END_KEY is sent as \x1b[4~ \x1b[8~ \x1b[F \x1b[OF
                                                case '1': return HOME_KEY;
                                                case '3': return DEL_KEY;
                                                case '4': return END_KEY;
                                                case '5': return PAGE_UP;
                                                case '6': return PAGE_DOWN;
                                                case '7': return HOME_KEY;
                                                case '8': return END_KEY;
                                        }
                                }
                        }
                        else {
                                switch(seq[1]){
                                        case 'A': return ARROW_UP;
                                        case 'B': return ARROW_DOWN;
                                        case 'C': return ARROW_RIGHT;
                                        case 'D': return ARROW_LEFT;
                                        case 'H': return HOME_KEY;
                                        case 'F': return END_KEY;
                                }
                        }
                } else if (seq[0] == 'O'){
                        switch (seq[1]) {
                                case 'H': return HOME_KEY;
                                case 'F': return END_KEY;
                        }
                }

                return '\x1b';
        } else {
                return c;
        }
}

int getCursorPosition(int *rows, int *cols) {
        char buff[32];
        // reading the values into the buffer.
        unsigned int i = 0;

        if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
        // the n command (https://vt100.net/docs/vt100-ug/chapter3.html#DSR)
        // used to query terminal for status information
        // argument 6 asks for cursor position
        while(i < sizeof(buff) - 1) {
                if(read(STDIN_FILENO, &buff[i], 1) != 1) break;
                if(buff[i]=='R') break;
                i++;
        }
        buff[i] = '\0';
        // here &buff[1] means, start printing from the first character and not the zeroth character.
        // printf("\r\n&buff[1]: '%s'\r\n", &buff[1]);

        // scan if the read responded with the proper escape sequence.
        if(buff[0]!='\x1b' || buff[1]!='[') return -1;
        // pass the pointer of the third character to sscanf.
        if(sscanf(&buff[2], "%d;%d", rows, cols) != 2) return -1;
        return 0;
}

int getWindowSize(int *rows, int* cols){
        struct winsize ws;
        // we init the struct and then fill in values using ioctl
        // the the meaning of TIOCGWINSZ is
        // Terminal IOCtl Get WINdow SiZe
        if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
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

/***    append buffer   ***/
struct abuf {
        char *b;
        int len;
};


// instead of small individual writes making it unpredictable at times.
// we make one big write in buffer and write it out, this would avoid the annoying flickering effect.

// this acts like a constructor for our abuf.
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
        // reallocate the char* pointer in abuf struct to current length + new length.
        char *new = realloc(ab->b, ab->len + len);

        if(new==NULL) return;
        // append the new string a the end of existing string.
        memcpy(&new[ab->len], s, len);
        // update abuf and the length.
        ab->b = new;
        ab->len += len;
}

void abFree(struct abuf *ab){
        // deallocate memory
        // kind of like a destructor.
        free(ab->b);
}

/***    output  ***/
void editorDrawRows(struct abuf *ab){
        // printing ~ at the begining of everyline like vim.
        int y;
        for(y = 0; y < E.screen_rows; y++){
                // display the welcome message a third of the way odwn the screen.
                if(y==E.screen_rows/3){
                        char welcome[80];
                        int welcomelen = snprintf(welcome, sizeof(welcome), "kilo editor -- version %s", KILO_VERSION);
                        if (welcomelen > E.screen_cols) welcomelen = E.screen_cols;
                        int padding = (E.screen_cols - welcomelen) / 2;
                        if(padding){
                                abAppend(ab, "~", 1);
                                padding--;
                        }
                        while(padding--) abAppend(ab, " ", 1);
                        abAppend(ab, welcome, welcomelen);
                } else {
                        abAppend(ab, "~", 1);
                }


                // clearing line by lin instead of clearing entire screen.
                abAppend(ab, "\x1b[K", 3);
                // <esc>[K https://vt100.net/docs/vt100-ug/chapter3.html#EL
                if(y<E.screen_rows-1){
                        abAppend(ab, "\r\n", 2);
                }
        }
}

void editorRefreshScreen(){
        struct abuf ab = ABUF_INIT;

        abAppend(&ab, "\x1b[?25l", 6);
        // escape sequence to hide cursor.

        // abAppend(&ab, "\x1b[2J", 4);
        // clears the screen. the \x1b[2J is a escape sequence.
        // escape sequences start with a \x1b (27 in hex) followed by [
        // here J command takes an argument and then instructs the terminal screen to clear itself.
        // 0J would clean the screen from cursor to the end of the screen.
        // 1J would clean the screen upto the cursor.
        // 2j in this case would clear the entire screen.

        // VT100 User Guide (https://vt100.net/docs/vt100-ug/chapter3.html)

        abAppend(&ab, "\x1b[H", 3);
        // another escape sequence, the H command takes 2 arguments and reposition's the cursor.
        // eg the terminal is of size 80x24, to position it into center of the screen we do <esc>[12;40H
        // multiple arguments can be separated by ;
        // the default arguments are 1;1 so we can just type [H and it would have the same result.
        // rows and cols start from 1 and not from 0.

        editorDrawRows(&ab);

        char buff[32];
        snprintf(buff, sizeof(buff), "\x1b[%d;%dH", E.cy+1, E.cx+1);
        // we add plus 1 as we initialized x and y as 0 but the indexing of cols and rows in terminal starts with 1
        abAppend(&ab, buff, strlen(buff));


        // abAppend(&ab, "\x1b[H", 3);
        // reset the cursor to begining of screen after drawing tildes. will be replaced with argumented H command to enable cursor movement.

        abAppend(&ab, "\x1b[?25h", 6);
        // show the cursor after drawing the tildes
        // the 25l and 25h might not work in some terminals (not a big problem).
        // latest VT models: https://vt100.net/docs/vt510-rm/DECTCEM.html

        // replaced write stdout individually with abAppend and wrote finally.
        write(STDOUT_FILENO, ab.b, ab.len);
        abFree(&ab);
}

/***    input   ***/
void editorMoveCursor(int key){
        switch (key){
                case ARROW_LEFT:
                        if(E.cx != 0) E.cx--;
                        break;
                case ARROW_RIGHT:
                        if(E.cx != E.screen_cols - 1) E.cx++;
                        break;
                case ARROW_UP:
                        if(E.cy != 0) E.cy--;
                        break;
                case ARROW_DOWN:
                        if(E.cy !=E.screen_rows - 1) E.cy++;
                        break;
        }
}

void editorProcessKeypress(){
        int c = editorReadKey();

        switch (c){
                case CTRL_KEY('q'):
                        // clear screen and place the cursor at the starting.
                        write(STDOUT_FILENO, "\x1b[2J", 4);
                        write(STDOUT_FILENO, "\x1b[H", 3);
                        exit(0);
                        break;

                case HOME_KEY:
                        E.cx = 0;
                        break;
                
                case END_KEY:
                        E.cx = E.screen_cols - 1;
                        break;
                
                case PAGE_UP:
                case PAGE_DOWN:
                        {
                                int times = E.screen_rows;
                                // Making page up and page down move cursor to top and botton of page for now.
                                while(times--)
                                        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                        }
                        break;

                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                        editorMoveCursor(c);
                        break;
        }
}

/***    init    ***/
void initEditor(){
        E.cx = 0;
        E.cy = 0;
        //setting initial cursor positions
        if(getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
        // getting size of terminal
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