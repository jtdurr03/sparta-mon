#ifdef _WIN32
#include <curses.h>
#include <windows.h>
#include <time.h>
#include <stdio.h>

static unsigned long long ft_to_ull(FILETIME ft) {
  return ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;
}

static int get_cpu_usage(double *out_percent) {
  static unsigned long long prev_idle = 0, prev_kernel = 0, prev_user = 0;
  FILETIME idle_ft, kernel_ft, user_ft;

  if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) return 0;

  unsigned long long idle = ft_to_ull(idle_ft);
  unsigned long long kernel = ft_to_ull(kernel_ft);
  unsigned long long user = ft_to_ull(user_ft);

  if (prev_idle == 0 && prev_kernel == 0 && prev_user == 0) {
    prev_idle = idle; prev_kernel = kernel; prev_user = user;
    return 0; // first sample needs a second to compute a delta
  }

  unsigned long long idle_d   = idle   - prev_idle;
  unsigned long long kernel_d = kernel - prev_kernel;
  unsigned long long user_d   = user   - prev_user;

  // kernel includes idle time on Windows
  unsigned long long total = kernel_d + user_d;
  unsigned long long busy  = (total > idle_d) ? (total - idle_d) : 0;

  prev_idle = idle; prev_kernel = kernel; prev_user = user;

  if (total == 0) return 0;

  *out_percent = (double)busy * 100.0 / (double)total;
  return 1;
}

static int get_mem_usage(unsigned long long *used_bytes,
                         unsigned long long *total_bytes,
                         double *out_percent) {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms)) return 0;

  unsigned long long total = (unsigned long long)ms.ullTotalPhys;
  unsigned long long avail = (unsigned long long)ms.ullAvailPhys;
  unsigned long long used  = total - avail;

  *used_bytes = used;
  *total_bytes = total;
  *out_percent = total ? ((double)used * 100.0 / (double)total) : 0.0;
  return 1;
}

static int get_disk_usage_c(unsigned long long *used_bytes,
                            unsigned long long *total_bytes,
                            double *out_percent) {
  ULARGE_INTEGER free_avail, total, total_free;
  if (!GetDiskFreeSpaceExA("C:\\", &free_avail, &total, &total_free)) return 0;

  unsigned long long t = (unsigned long long)total.QuadPart;
  unsigned long long f = (unsigned long long)total_free.QuadPart;
  unsigned long long u = t - f;

  *used_bytes = u;
  *total_bytes = t;
  *out_percent = t ? ((double)u * 100.0 / (double)t) : 0.0;
  return 1;
}

static double bytes_to_gb(unsigned long long b) {
  return (double)b / (1024.0 * 1024.0 * 1024.0);
}

static void draw_screen(void) {
  // time
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char buf[64] = {0};
  if (t) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);

  // uptime
  unsigned long long up_ms = (unsigned long long)GetTickCount64();
  unsigned long long up_s = up_ms / 1000ULL;
  unsigned long long up_h = up_s / 3600ULL;
  unsigned long long up_m = (up_s % 3600ULL) / 60ULL;
  unsigned long long up_sec = up_s % 60ULL;

  // cpu/mem/disk
  double cpu = 0.0;
  int has_cpu = get_cpu_usage(&cpu);

  unsigned long long mem_used=0, mem_total=0;
  double mem_pct=0.0;
  int has_mem = get_mem_usage(&mem_used, &mem_total, &mem_pct);

  unsigned long long d_used=0, d_total=0;
  double d_pct=0.0;
  int has_disk = get_disk_usage_c(&d_used, &d_total, &d_pct);

  erase();
  mvprintw(0, 0, "sparta-mon (Windows build)");
  mvprintw(1, 0, "PDCurses + MSYS2/MinGW | Q to quit");

  mvprintw(3, 0, "Time:   %s", buf[0] ? buf : "N/A");
  mvprintw(4, 0, "Uptime: %lluh %llum %llus", up_h, up_m, up_sec);

  if (has_cpu) mvprintw(6, 0, "CPU:  %5.1f%%", cpu);
  else         mvprintw(6, 0, "CPU:   N/A (sampling...)");

  if (has_mem) {
    mvprintw(7, 0, "MEM:  %5.1f%%  (%.2f / %.2f GB)",
             mem_pct, bytes_to_gb(mem_used), bytes_to_gb(mem_total));
  } else {
    mvprintw(7, 0, "MEM:   N/A");
  }

  if (has_disk) {
    mvprintw(8, 0, "DISK: %5.1f%%  C: (%.2f / %.2f GB)",
             d_pct, bytes_to_gb(d_used), bytes_to_gb(d_total));
  } else {
    mvprintw(8, 0, "DISK:  N/A");
  }

  mvprintw(10, 0, "LOAD: N/A (no /proc loadavg on Windows)");
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
