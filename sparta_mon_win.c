#ifdef _WIN32
#include <curses.h>
#include <windows.h>
#include <time.h>
#include <stdio.h>

static void draw_screen(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char buf[64] = {0};
  if (t) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);

  erase();
  mvprintw(0, 0, "sparta-mon (Windows build)");
  mvprintw(1, 0, "Built with PDCurses via MSYS2/MinGW");
  mvprintw(3, 0, "Time: %s", buf[0] ? buf : "N/A");

  mvprintw(5, 0, "CPU: N/A   MEM: N/A   LOAD: N/A   DISK: N/A");
  mvprintw(7, 0, "Note: Linux /proc metrics are not implemented on Windows yet.");
  mvprintw(LINES - 2, 0, "Press Q to quit");
  refresh();
}

int main(void) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);

  for (;;) {
    draw_screen();
    int ch = getch();
    if (ch == 'q' || ch == 'Q') break;
    Sleep(250);
  }

  endwin();
  return 0;
}
#else
int main(void) { return 0; }
#endif
