#define _GNU_SOURCE
#if defined(_WIN32) || defined(_WIN64)
  #include <curses.h>   // PDCurses on Windows
#else
  #include <ncurses.h>  // ncurses on Linux
#endif
#include <locale.h>
#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>
#include <sys/statvfs.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#define HIST_MAX 4096
#define EWMA_ALPHA 0.20
#define DEFAULT_DELAY_MS 500
#define MIN_DELAY_MS 100
#define MAX_DELAY_MS 2000

typedef struct {
  double v[HIST_MAX];
  int head;
  int len;
} Hist;

static void hist_push(Hist *h, double x) {
  h->v[h->head] = x;
  h->head = (h->head + 1) % HIST_MAX;
  if (h->len < HIST_MAX) h->len++;
}
static double hist_get_latest(const Hist *h) {
  if (h->len <= 0) return 0.0;
  int idx = (h->head - 1 + HIST_MAX) % HIST_MAX;
  return h->v[idx];
}
static double hist_get_lastN(const Hist *h, int count, int i) {
  if (count <= 0 || h->len <= 0) return 0.0;
  count = MIN(count, h->len);
  int start = (h->head - count + HIST_MAX) % HIST_MAX;
  int idx = (start + i) % HIST_MAX;
  return h->v[idx];
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ---------------------------
// /proc readers
// ---------------------------
static int read_cpu(unsigned long long *total, unsigned long long *idle) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f) return 0;

  char line[512];
  if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
  fclose(f);

  unsigned long long user=0,nice=0,sys=0,idl=0,iow=0,irq=0,sirq=0,stl=0;
  int n = sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                 &user,&nice,&sys,&idl,&iow,&irq,&sirq,&stl);
  if (n < 4) return 0;

  *idle  = idl + iow;
  *total = user + nice + sys + idl + iow + irq + sirq + stl;
  return 1;
}

static int read_load(double *l1, double *l5, double *l15) {
  FILE *f = fopen("/proc/loadavg", "r");
  if (!f) return 0;
  int ok = fscanf(f, "%lf %lf %lf", l1, l5, l15) == 3;
  fclose(f);
  return ok;
}

static int read_mem(unsigned long long *mem_total, unsigned long long *mem_avail) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return 0;

  char key[64];
  unsigned long long val = 0;
  char unit[32];

  *mem_total = 0;
  *mem_avail = 0;

  while (fscanf(f, "%63s %llu %31s", key, &val, unit) == 3) {
    if (strcmp(key, "MemTotal:") == 0) *mem_total = val * 1024ULL;
    if (strcmp(key, "MemAvailable:") == 0) *mem_avail = val * 1024ULL;
    if (*mem_total && *mem_avail) break;
  }
  fclose(f);
  return (*mem_total != 0);
}

static int read_uptime(double *up) {
  FILE *f = fopen("/proc/uptime", "r");
  if (!f) return 0;
  int ok = fscanf(f, "%lf", up) == 1;
  fclose(f);
  return ok;
}

static int read_temp(double *celsius) {
  FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (!f) return 0;
  long mv = 0;
  int ok = fscanf(f, "%ld", &mv) == 1;
  fclose(f);
  if (!ok) return 0;
  *celsius = (double)mv / 1000.0;
  return 1;
}

static int is_pid_dir(const char *name) {
  if (!name || !*name) return 0;
  for (const char *p=name; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
  return 1;
}

// ---------------------------
// Network
// ---------------------------
static int choose_iface(char *out, size_t outsz) {
  const char *env = getenv("IFACE");
  if (env && *env) { snprintf(out, outsz, "%s", env); return 1; }

  FILE *f = fopen("/proc/net/dev", "r");
  if (!f) return 0;

  char line[512];
  fgets(line, sizeof(line), f);
  fgets(line, sizeof(line), f);

  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ') p++;
    char *colon = strchr(p, ':');
    if (!colon) continue;
    *colon = '\0';
    if (strcmp(p, "lo") == 0) continue;

    snprintf(out, outsz, "%s", p);
    fclose(f);
    return 1;
  }

  fclose(f);
  return 0;
}

static int read_net_dev(const char *iface,
                        unsigned long long *rxB, unsigned long long *txB,
                        unsigned long long *rxErr, unsigned long long *rxDrop,
                        unsigned long long *txErr, unsigned long long *txDrop) {
  FILE *f = fopen("/proc/net/dev", "r");
  if (!f) return 0;

  char line[512];
  fgets(line, sizeof(line), f);
  fgets(line, sizeof(line), f);

  int ok = 0;
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ') p++;
    char *colon = strchr(p, ':');
    if (!colon) continue;
    *colon = '\0';
    if (strcmp(p, iface) != 0) continue;

    unsigned long long a[16] = {0};
    int n = sscanf(colon + 1,
      "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
      &a[0],&a[1],&a[2],&a[3],&a[4],&a[5],&a[6],&a[7],
      &a[8],&a[9],&a[10],&a[11],&a[12],&a[13],&a[14],&a[15]
    );
    if (n >= 12) {
      *rxB = a[0];
      *rxErr = a[2];
      *rxDrop = a[3];
      *txB = a[8];
      *txErr = a[10];
      *txDrop = a[11];
      ok = 1;
    }
    break;
  }

  fclose(f);
  return ok;
}

// ---------------------------
// Disk
// ---------------------------
static int is_partition_name(const char *name) {
  if (strncmp(name, "sd", 2) == 0 && isalpha((unsigned char)name[2])) return isdigit((unsigned char)name[3]);
  if (strncmp(name, "mmcblk", 6) == 0) { const char *p = strchr(name, 'p'); return (p && isdigit((unsigned char)p[1])); }
  if (strncmp(name, "nvme", 4) == 0) { const char *p = strchr(name, 'p'); return (p && isdigit((unsigned char)p[1])); }
  return 0;
}
static int disk_score(const char *name) {
  if (!name || !*name) return 0;
  if (is_partition_name(name)) return 0;
  if (strncmp(name, "loop", 4) == 0) return 0;
  if (strncmp(name, "ram", 3) == 0) return 0;
  if (strncmp(name, "dm-", 3) == 0) return 0;
  if (strncmp(name, "md", 2) == 0) return 0;
  if (strncmp(name, "zram", 4) == 0) return 0;
  if (strncmp(name, "sr", 2) == 0) return 0;

  if (strncmp(name, "mmcblk0", 7) == 0) return 1000;
  if (strncmp(name, "nvme", 4) == 0) return 900;
  if (strncmp(name, "sd", 2) == 0) return 800;
  if (strcmp(name, "vda") == 0) return 700;
  return 100;
}

static int choose_disk(char *out, size_t outsz) {
  const char *env = getenv("DISK");
  if (env && *env) { snprintf(out, outsz, "%s", env); return 1; }

  FILE *f = fopen("/proc/diskstats", "r");
  if (!f) return 0;

  char line[512];
  int best = 0;
  char bestName[64] = {0};

  while (fgets(line, sizeof(line), f)) {
    unsigned int major=0, minor=0;
    char name[64] = {0};
    if (sscanf(line, "%u %u %63s", &major, &minor, name) != 3) continue;

    int sc = disk_score(name);
    if (sc > best) {
      best = sc;
      snprintf(bestName, sizeof(bestName), "%s", name);
    }
  }

  fclose(f);
  if (best <= 0 || !bestName[0]) return 0;
  snprintf(out, outsz, "%s", bestName);
  return 1;
}

static int read_diskstats(const char *dev,
                          unsigned long long *rd_sectors,
                          unsigned long long *wr_sectors) {
  FILE *f = fopen("/proc/diskstats", "r");
  if (!f) return 0;

  char line[512];
  int ok = 0;

  while (fgets(line, sizeof(line), f)) {
    unsigned int major=0, minor=0;
    char name[64] = {0};

    unsigned long long reads=0, reads_merged=0, sectors_read=0, ms_read=0;
    unsigned long long writes=0, writes_merged=0, sectors_written=0, ms_write=0;

    int n = sscanf(line,
      "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu",
      &major, &minor, name,
      &reads, &reads_merged, &sectors_read, &ms_read,
      &writes, &writes_merged, &sectors_written, &ms_write
    );
    if (n < 11) continue;

    if (strcmp(name, dev) == 0) {
      *rd_sectors = sectors_read;
      *wr_sectors = sectors_written;
      ok = 1;
      break;
    }
  }

  fclose(f);
  return ok;
}

// ---------------------------
// FS usage (/)
// ---------------------------
static int read_fs_usage(const char *path,
                         double *usedPct,
                         unsigned long long *usedB,
                         unsigned long long *totB,
                         double *inodePct) {
  struct statvfs v;
  if (statvfs(path, &v) != 0) return 0;

  unsigned long long fr = (unsigned long long)v.f_frsize;
  unsigned long long total = (unsigned long long)v.f_blocks * fr;
  unsigned long long avail = (unsigned long long)v.f_bavail * fr;
  unsigned long long used  = (total > avail) ? (total - avail) : 0;

  double p = 0.0;
  if (total > 0) p = (double)used / (double)total * 100.0;

  unsigned long long itot = (unsigned long long)v.f_files;
  unsigned long long iav  = (unsigned long long)v.f_favail;
  unsigned long long iuse = (itot > iav) ? (itot - iav) : 0;
  double ip = 0.0;
  if (itot > 0) ip = (double)iuse / (double)itot * 100.0;

  *usedPct = p;
  *usedB = used;
  *totB = total;
  *inodePct = ip;
  return 1;
}

// ---------------------------
// Pi throttling
// ---------------------------
static int read_throttled(unsigned int *flags_out) {
  FILE *fp = popen("vcgencmd get_throttled 2>/dev/null", "r");
  if (!fp) return 0;
  char buf[128] = {0};
  if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return 0; }
  pclose(fp);

  char *p = strstr(buf, "0x");
  if (!p) return 0;
  unsigned int v = (unsigned int)strtoul(p, NULL, 16);
  *flags_out = v;
  return 1;
}

static void throttled_summary(unsigned int flags, char *out, size_t n) {
  int uv   = (flags & (1u<<0))  != 0;
  int cap  = (flags & (1u<<1))  != 0;
  int thr  = (flags & (1u<<2))  != 0;
  int tmp  = (flags & (1u<<3))  != 0;

  int uvh  = (flags & (1u<<16)) != 0;
  int caph = (flags & (1u<<17)) != 0;
  int thrh = (flags & (1u<<18)) != 0;
  int tmph = (flags & (1u<<19)) != 0;

  if (flags == 0) { snprintf(out, n, "PWR OK"); return; }

  char now[64] = {0};
  char hist[64] = {0};

  if (uv)  strncat(now, "UV ",  sizeof(now)-strlen(now)-1);
  if (cap) strncat(now, "CAP ", sizeof(now)-strlen(now)-1);
  if (thr) strncat(now, "THR ", sizeof(now)-strlen(now)-1);
  if (tmp) strncat(now, "TMP ", sizeof(now)-strlen(now)-1);

  if (uvh)  strncat(hist, "UV ",  sizeof(hist)-strlen(hist)-1);
  if (caph) strncat(hist, "CAP ", sizeof(hist)-strlen(hist)-1);
  if (thrh) strncat(hist, "THR ", sizeof(hist)-strlen(hist)-1);
  if (tmph) strncat(hist, "TMP ", sizeof(hist)-strlen(hist)-1);

  for (int i=(int)strlen(now)-1; i>=0 && now[i]==' '; i--) now[i]=0;
  for (int i=(int)strlen(hist)-1; i>=0 && hist[i]==' '; i--) hist[i]=0;

  if (hist[0]) snprintf(out, n, "PWR %s |H:%s", now[0]?now:"OK", hist);
  else         snprintf(out, n, "PWR %s", now[0]?now:"OK");
}

// ---------------------------
// Process tracking
// ---------------------------
typedef struct {
  int pid;
  char comm[64];
  char state;
  unsigned long long last_jiff;
  double cpu_cur;
  double cpu_avg;
  unsigned long long rss_bytes;
  int seen;
} ProcTrack;

typedef struct {
  ProcTrack *a;
  int n;
  int cap;
} ProcTable;

static void proctable_init(ProcTable *t) { t->a=NULL; t->n=0; t->cap=0; }
static void proctable_free(ProcTable *t) { free(t->a); t->a=NULL; t->n=0; t->cap=0; }

static ProcTrack* proctable_get(ProcTable *t, int pid) {
  for (int i=0;i<t->n;i++) if (t->a[i].pid == pid) return &t->a[i];
  return NULL;
}

static ProcTrack* proctable_upsert(ProcTable *t, int pid) {
  ProcTrack *p = proctable_get(t, pid);
  if (p) return p;
  if (t->n == t->cap) {
    t->cap = (t->cap == 0) ? 256 : t->cap * 2;
    t->a = (ProcTrack*)realloc(t->a, sizeof(ProcTrack) * t->cap);
  }
  ProcTrack *nw = &t->a[t->n++];
  memset(nw, 0, sizeof(*nw));
  nw->pid = pid;
  nw->cpu_avg = 0.0;
  return nw;
}

static void proctable_prune_unseen(ProcTable *t) {
  int w = 0;
  for (int i=0;i<t->n;i++) {
    if (t->a[i].seen) {
      t->a[i].seen = 0;
      t->a[w++] = t->a[i];
    }
  }
  t->n = w;
}

static int read_proc_stat(int pid, char *comm_out, size_t comm_sz, char *state_out,
                          unsigned long long *jiff_out) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *f = fopen(path, "r");
  if (!f) return 0;

  char buf[4096];
  if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
  fclose(f);

  char *lp = strchr(buf, '(');
  char *rp = strrchr(buf, ')');
  if (!lp || !rp || rp <= lp) return 0;

  size_t len = (size_t)(rp - lp - 1);
  if (len >= comm_sz) len = comm_sz - 1;
  memcpy(comm_out, lp + 1, len);
  comm_out[len] = '\0';

  char *after = rp + 2;
  char *save = NULL;
  char *tok = strtok_r(after, " ", &save);
  if (!tok) return 0;

  *state_out = tok[0];

  unsigned long long ut=0, st=0;
  for (int idx = 1; idx <= 13; idx++) {
    tok = strtok_r(NULL, " ", &save);
    if (!tok) return 0;
    if (idx == 12) ut = strtoull(tok, NULL, 10);
    if (idx == 13) st = strtoull(tok, NULL, 10);
  }
  *jiff_out = ut + st;
  return 1;
}

static unsigned long long read_proc_rss_bytes(int pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/statm", pid);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  unsigned long long size=0, rss=0;
  int ok = fscanf(f, "%llu %llu", &size, &rss) == 2;
  fclose(f);
  if (!ok) return 0;
  long page = sysconf(_SC_PAGESIZE);
  return rss * (unsigned long long)page;
}

static int cmp_proc_avg(const void *A, const void *B) {
  const ProcTrack *a = (const ProcTrack*)A;
  const ProcTrack *b = (const ProcTrack*)B;
  if (a->cpu_avg < b->cpu_avg) return 1;
  if (a->cpu_avg > b->cpu_avg) return -1;
  if (a->cpu_cur < b->cpu_cur) return 1;
  if (a->cpu_cur > b->cpu_cur) return -1;
  return (a->pid - b->pid);
}

// ---------------------------
// Formatting + graphs
// ---------------------------
static void fmt_bytes(unsigned long long b, char *out, size_t n) {
  const char *u = "B";
  double v = (double)b;
  if (v >= 1024) { v/=1024; u="KB"; }
  if (v >= 1024) { v/=1024; u="MB"; }
  if (v >= 1024) { v/=1024; u="GB"; }
  snprintf(out, n, "%.1f%s", v, u);
}

static void fmt_uptime(double sec, char *out, size_t n) {
  int s = (int)sec;
  int d = s / 86400; s %= 86400;
  int h = s / 3600;  s %= 3600;
  int m = s / 60;    s %= 60;
  snprintf(out, n, "%dd %02d:%02d:%02d", d, h, m, s);
}

static void draw_single_graph(WINDOW *w, const char *title,
                              const Hist *h, int count,
                              double vmin, double vmax,
                              int color_pair, const char *unit) {
  int H, W;
  getmaxyx(w, H, W);
  box(w, 0, 0);

  int x0=1, y0=1, x1=W-2, y1=H-2;
  int pw = x1-x0+1, ph = y1-y0+1;
  if (pw < 10 || ph < 4) return;

  int n = MIN(count, pw);
  n = MIN(n, h->len);

  double latest = hist_get_latest(h);

  wattron(w, A_BOLD);
  mvwprintw(w, 0, 2, " %s ", title);
  wattroff(w, A_BOLD);

  char num[64];
  snprintf(num, sizeof(num), "%.1f%s", latest, unit?unit:"");
  mvwprintw(w, 0, MAX(2, W-(int)strlen(num)-2), "%s", num);

  int midy = y0 + ph/2;
  for (int x=x0; x<=x1; x++) mvwaddch(w, midy, x, ACS_HLINE);

  double range = vmax - vmin;
  if (range <= 0.0001) range = 1.0;

  int prevx=-1, prevy=-1;

  if (color_pair > 0) wattron(w, COLOR_PAIR(color_pair));

  for (int col=0; col<n; col++) {
    double val = hist_get_lastN(h, n, col);
    double t = (val - vmin) / range;
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    int y = y1 - (int)(t * (ph - 1) + 0.5);
    int x = x0 + col;

    mvwaddch(w, y, x, 'o');

    if (prevx >= 0) {
      int dy = y - prevy;
      int steps = abs(dy);
      if (steps > 0) {
        int dir = (dy > 0) ? 1 : -1;
        for (int s=1; s<=steps; s++) {
          int yy = prevy + s*dir;
          mvwaddch(w, yy, x, ACS_VLINE);
        }
      }
    }
    prevx=x; prevy=y;
  }

  if (color_pair > 0) wattroff(w, COLOR_PAIR(color_pair));
}

static void draw_dual_graph(WINDOW *w, const char *title,
                            const Hist *a, const Hist *b,
                            int count, double vmin, double vmax,
                            int colorA, int colorB,
                            const char *labelA, const char *labelB,
                            const char *unit,
                            const char *extraLine) {
  int H, W;
  getmaxyx(w, H, W);
  box(w, 0, 0);

  int x0=1, y0=1, x1=W-2, y1=H-2;
  int pw = x1-x0+1, ph = y1-y0+1;
  if (pw < 10 || ph < 4) return;

  int n = MIN(count, pw);
  n = MIN(n, MIN(a->len, b->len));

  wattron(w, A_BOLD);
  mvwprintw(w, 0, 2, " %s ", title);
  wattroff(w, A_BOLD);

  double la = hist_get_latest(a);
  double lb = hist_get_latest(b);

  char top[128];
  snprintf(top, sizeof(top), "%s %.1f%s  %s %.1f%s",
           labelA, la, unit?unit:"",
           labelB, lb, unit?unit:"");
  mvwprintw(w, 0, MAX(2, W-(int)strlen(top)-2), "%s", top);

  if (extraLine && *extraLine && H >= 6) {
    mvwprintw(w, 1, 2, "%.*s", W-4, extraLine);
  }

  int midy = y0 + ph/2;
  for (int x=x0; x<=x1; x++) mvwaddch(w, midy, x, ACS_HLINE);

  double range = vmax - vmin;
  if (range <= 0.0001) range = 1.0;

  int prevx=-1, prevy=-1;
  if (colorA > 0) wattron(w, COLOR_PAIR(colorA));
  for (int col=0; col<n; col++) {
    double val = hist_get_lastN(a, n, col);
    double t = (val - vmin) / range;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int y = y1 - (int)(t * (ph - 1) + 0.5);
    int x = x0 + col;

    mvwaddch(w, y, x, 'o');

    if (prevx >= 0) {
      int dy = y - prevy;
      int steps = abs(dy);
      if (steps > 0) {
        int dir = (dy > 0) ? 1 : -1;
        for (int s=1; s<=steps; s++) {
          int yy = prevy + s*dir;
          mvwaddch(w, yy, x, ACS_VLINE);
        }
      }
    }
    prevx=x; prevy=y;
  }
  if (colorA > 0) wattroff(w, COLOR_PAIR(colorA));

  prevx=-1; prevy=-1;
  if (colorB > 0) wattron(w, COLOR_PAIR(colorB));
  for (int col=0; col<n; col++) {
    double val = hist_get_lastN(b, n, col);
    double t = (val - vmin) / range;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int y = y1 - (int)(t * (ph - 1) + 0.5);
    int x = x0 + col;

    chtype existing = mvwinch(w, y, x);
    char ch = (char)(existing & A_CHARTEXT);
    if (ch == 'o') mvwaddch(w, y, x, 'X');
    else mvwaddch(w, y, x, '*');

    if (prevx >= 0) {
      int dy = y - prevy;
      int steps = abs(dy);
      if (steps > 0) {
        int dir = (dy > 0) ? 1 : -1;
        for (int s=1; s<=steps; s++) {
          int yy = prevy + s*dir;
          chtype ex2 = mvwinch(w, yy, x);
          char ch2 = (char)(ex2 & A_CHARTEXT);
          if (ch2 == 'o') mvwaddch(w, yy, x, 'X');
          else if (ch2 == 0 || ch2 == ' ' || ch2 == ACS_HLINE) mvwaddch(w, yy, x, ACS_VLINE);
        }
      }
    }
    prevx=x; prevy=y;
  }
  if (colorB > 0) wattroff(w, COLOR_PAIR(colorB));
}

// ---------------------------
// Resize handling
// ---------------------------
static volatile sig_atomic_t g_resized = 0;
static void on_winch(int sig) { (void)sig; g_resized = 1; }

// ---------------------------
// Main
// ---------------------------
int main(void) {
  setlocale(LC_ALL, "");
  signal(SIGWINCH, on_winch);

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);
  leaveok(stdscr, TRUE);

  int use_color = has_colors();
  if (use_color) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_MAGENTA, -1); // title
    init_pair(2, COLOR_CYAN, -1);    // cyan
    init_pair(3, COLOR_GREEN, -1);   // green
    init_pair(4, COLOR_YELLOW, -1);  // yellow
    init_pair(5, COLOR_WHITE, -1);   // text
    init_pair(6, COLOR_RED, -1);     // hot
    init_pair(7, COLOR_MAGENTA, -1); // magenta
  }

  int delay_ms = DEFAULT_DELAY_MS;
  int running = 1;

  Hist h_cpu={0}, h_mem={0}, h_temp={0};
  Hist h_disk_r={0}, h_disk_w={0};
  Hist h_net_rx={0}, h_net_tx={0};

  unsigned long long prev_tot=0, prev_idle=0;
  read_cpu(&prev_tot, &prev_idle);

  char iface[64] = {0};
  int have_iface = choose_iface(iface, sizeof(iface));
  unsigned long long prev_rxB=0, prev_txB=0, prev_rxE=0, prev_rxD=0, prev_txE=0, prev_txD=0;
  int have_prev_net = 0;

  char disk[64] = {0};
  int have_disk = choose_disk(disk, sizeof(disk));
  unsigned long long prev_rdsec=0, prev_wrsec=0;
  int have_prev_disk = 0;

  ProcTable pt; proctable_init(&pt);
  int scroll = 0;

  WINDOW *wHdr=NULL;
  WINDOW *wCpu=NULL, *wMem=NULL;
  WINDOW *wTmp=NULL, *wDisk=NULL;
  WINDOW *wProc=NULL, *wNet=NULL;

  int last_lines=0, last_cols=0;
  double t_prev = now_s();

  while (running) {
    if (g_resized || LINES != last_lines || COLS != last_cols) {
      g_resized = 0;
      endwin(); refresh();

      if (wHdr) delwin(wHdr);
      if (wCpu) delwin(wCpu);
      if (wMem) delwin(wMem);
      if (wTmp) delwin(wTmp);
      if (wDisk) delwin(wDisk);
      if (wProc) delwin(wProc);
      if (wNet) delwin(wNet);

      last_lines = LINES;
      last_cols  = COLS;

      // Header + 3x2 grid below it
      int header_h = 2;
      int avail_h = MAX(6, LINES - header_h);

      int h1 = MAX(6, avail_h / 3);
      int h2 = MAX(6, avail_h / 3);
      int h3 = MAX(6, avail_h - h1 - h2);

      int wL = MAX(20, COLS / 2);
      int wR = MAX(20, COLS - wL);

      wHdr  = newwin(header_h, COLS, 0, 0);

      int y0 = header_h;
      wCpu  = newwin(h1, wL, y0, 0);
      wMem  = newwin(h1, wR, y0, wL);

      int y1 = y0 + h1;
      wTmp  = newwin(h2, wL, y1, 0);
      wDisk = newwin(h2, wR, y1, wL);

      int y2 = y1 + h2;
      wProc = newwin(h3, wL, y2, 0);   // bottom-left (TASKS)
      wNet  = newwin(h3, wR, y2, wL);  // bottom-right (NET)

      scroll = 0;
    }

    int ch = getch();
    if (ch != ERR) {
      if (ch == 'q' || ch == 'Q') running = 0;
      else if (ch == '+' || ch == '=') delay_ms = MAX(MIN_DELAY_MS, delay_ms - 50);
      else if (ch == '-' || ch == '_') delay_ms = MIN(MAX_DELAY_MS, delay_ms + 50);
      else if (ch == 'c' || ch == 'C') use_color = !use_color;
      else if (ch == KEY_UP) scroll = MAX(0, scroll - 1);
      else if (ch == KEY_DOWN) scroll = scroll + 1;
      else if (ch == KEY_PPAGE) scroll = MAX(0, scroll - 10);
      else if (ch == KEY_NPAGE) scroll = scroll + 10;
      else if (ch == KEY_HOME) scroll = 0;
    }

    double t_cur = now_s();
    double dt = t_cur - t_prev;
    if (dt <= 0) dt = 0.001;

    // CPU %
    unsigned long long tot=0, idle=0;
    double cpu_pct = 0.0;
    if (read_cpu(&tot, &idle)) {
      unsigned long long d_tot = tot - prev_tot;
      unsigned long long d_idle = idle - prev_idle;
      if (d_tot > 0) cpu_pct = (1.0 - (double)d_idle / (double)d_tot) * 100.0;
      prev_tot = tot;
      prev_idle = idle;
    }

    // Load
    double l1=0,l5=0,l15=0; read_load(&l1,&l5,&l15);

    // Mem %
    unsigned long long memT=0, memA=0;
    read_mem(&memT, &memA);
    double mem_used = (memT > memA) ? (double)(memT - memA) : 0.0;
    double mem_pct  = (memT > 0) ? (mem_used / (double)memT) * 100.0 : 0.0;

    // Uptime + Temp
    double up=0; read_uptime(&up);
    double tc=0; int have_tc = read_temp(&tc);

    // Disk rates (MB/s)
    double disk_r_mbs = 0.0, disk_w_mbs = 0.0;
    if (have_disk) {
      unsigned long long rd=0, wr=0;
      if (read_diskstats(disk, &rd, &wr)) {
        if (have_prev_disk) {
          unsigned long long d_rd = (rd >= prev_rdsec) ? (rd - prev_rdsec) : 0;
          unsigned long long d_wr = (wr >= prev_wrsec) ? (wr - prev_wrsec) : 0;
          double rBps = (double)d_rd * 512.0 / dt;
          double wBps = (double)d_wr * 512.0 / dt;
          disk_r_mbs = rBps / (1024.0*1024.0);
          disk_w_mbs = wBps / (1024.0*1024.0);
        }
        prev_rdsec = rd;
        prev_wrsec = wr;
        have_prev_disk = 1;
      }
    }

    // Net rates (MB/s) + errs/drops deltas
    double net_rx_mbs = 0.0, net_tx_mbs = 0.0;
    unsigned long long rxE=0, rxD=0, txE=0, txD=0;
    unsigned long long d_rxE=0, d_rxD=0, d_txE=0, d_txD=0;
    if (have_iface) {
      unsigned long long rxB=0, txB=0;
      if (read_net_dev(iface, &rxB, &txB, &rxE, &rxD, &txE, &txD)) {
        if (have_prev_net) {
          unsigned long long d_rx = (rxB >= prev_rxB) ? (rxB - prev_rxB) : 0;
          unsigned long long d_tx = (txB >= prev_txB) ? (txB - prev_txB) : 0;
          net_rx_mbs = ((double)d_rx / dt) / (1024.0*1024.0);
          net_tx_mbs = ((double)d_tx / dt) / (1024.0*1024.0);

          d_rxE = (rxE >= prev_rxE) ? (rxE - prev_rxE) : 0;
          d_rxD = (rxD >= prev_rxD) ? (rxD - prev_rxD) : 0;
          d_txE = (txE >= prev_txE) ? (txE - prev_txE) : 0;
          d_txD = (txD >= prev_txD) ? (txD - prev_txD) : 0;
        }
        prev_rxB=rxB; prev_txB=txB;
        prev_rxE=rxE; prev_rxD=rxD;
        prev_txE=txE; prev_txD=txD;
        have_prev_net = 1;
      }
    }

    // FS /
    double fsPct=0.0, inodePct=0.0;
    unsigned long long fsUsedB=0, fsTotB=0;
    int have_fs = read_fs_usage("/", &fsPct, &fsUsedB, &fsTotB, &inodePct);

    // Pi throttled
    unsigned int thrFlags=0;
    int have_thr = read_throttled(&thrFlags);
    char thrStr[128] = "PWR n/a";
    if (have_thr) throttled_summary(thrFlags, thrStr, sizeof(thrStr));

    // push histories
    hist_push(&h_cpu, cpu_pct);
    hist_push(&h_mem, mem_pct);
    hist_push(&h_temp, have_tc ? tc : 0.0);
    hist_push(&h_disk_r, disk_r_mbs);
    hist_push(&h_disk_w, disk_w_mbs);
    hist_push(&h_net_rx, net_rx_mbs);
    hist_push(&h_net_tx, net_tx_mbs);

    // Processes
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    for (int i=0;i<pt.n;i++) pt.a[i].seen = 0;

    DIR *d = opendir("/proc");
    if (d) {
      struct dirent *de;
      while ((de = readdir(d))) {
        if (!is_pid_dir(de->d_name)) continue;
        int pid = atoi(de->d_name);

        char comm[64]; char state='?'; unsigned long long jiff=0;
        if (!read_proc_stat(pid, comm, sizeof(comm), &state, &jiff)) continue;

        ProcTrack *p = proctable_upsert(&pt, pid);
        p->seen = 1;
        p->state = state;
        strncpy(p->comm, comm, sizeof(p->comm)-1);
        p->comm[sizeof(p->comm)-1] = '\0';

        p->rss_bytes = read_proc_rss_bytes(pid);

        unsigned long long dj = 0;
        if (p->last_jiff > 0 && jiff >= p->last_jiff) dj = (jiff - p->last_jiff);
        p->last_jiff = jiff;

        double curpct = 0.0;
        if (dj > 0) curpct = (double)dj / ((double)hz * dt) * 100.0;
        p->cpu_cur = curpct;

        if (p->cpu_avg <= 0.0001) p->cpu_avg = curpct;
        else p->cpu_avg = (1.0 - EWMA_ALPHA)*p->cpu_avg + EWMA_ALPHA*curpct;
      }
      closedir(d);
    }

    proctable_prune_unseen(&pt);
    qsort(pt.a, pt.n, sizeof(ProcTrack), cmp_proc_avg);

    // Clamp scroll based on TASKS window
    int procH, procW;
    getmaxyx(wProc, procH, procW);
    int proc_rows_visible = MAX(0, procH - 3);
    int maxScroll = MAX(0, pt.n - proc_rows_visible);
    scroll = MIN(scroll, maxScroll);

    // ---------------------------
    // Header (2 lines)
    // ---------------------------
    werase(wHdr);

    if (use_color) wattron(wHdr, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(wHdr, 0, 2, "SPARTA//MON");
    if (use_color) wattroff(wHdr, COLOR_PAIR(1) | A_BOLD);

    if (use_color) wattron(wHdr, COLOR_PAIR(5));
    mvwprintw(wHdr, 0, 16, "q quit | +/- speed | arrows scroll | c color | %dms", delay_ms);

    char upbuf[64]; fmt_uptime(up, upbuf, sizeof(upbuf));
    char fsLine[128] = "FS / n/a";
    if (have_fs) {
      char uB[32], tB[32];
      fmt_bytes(fsUsedB, uB, sizeof(uB));
      fmt_bytes(fsTotB, tB, sizeof(tB));
      snprintf(fsLine, sizeof(fsLine), "FS / %.1f%% (%s/%s) INO %.1f%%",
               fsPct, uB, tB, inodePct);
    }

    char line2[512];
    snprintf(line2, sizeof(line2),
      "CPU %.1f%% MEM %.1f%% LOAD %.2f %.2f %.2f TEMP %s  %s  %s  IF %s DK %s",
      cpu_pct, mem_pct, l1,l5,l15,
      have_tc ? "" : "n/a",
      fsLine,
      thrStr,
      have_iface ? iface : "n/a",
      have_disk ? disk : "n/a"
    );

    // put temp number where TEMP %s was left empty
    if (have_tc) {
      char tbuf[16];
      snprintf(tbuf, sizeof(tbuf), "%.1fC", tc);
      // replace "TEMP " + "" effectively by printing at end of prefix area
      // easiest: just build a clean line
      snprintf(line2, sizeof(line2),
        "CPU %.1f%% MEM %.1f%% LOAD %.2f %.2f %.2f TEMP %.1fC  %s  %s  IF %s DK %s",
        cpu_pct, mem_pct, l1,l5,l15, tc,
        fsLine, thrStr,
        have_iface ? iface : "n/a",
        have_disk ? disk : "n/a"
      );
    }

    mvwprintw(wHdr, 1, 2, "%.*s", COLS-4, line2);
    if (use_color) wattroff(wHdr, COLOR_PAIR(5));

    wnoutrefresh(wHdr);

    // ---------------------------
    // Graphs in 3x2 grid
    // ---------------------------
    werase(wCpu);  werase(wMem);
    werase(wTmp);  werase(wDisk);
    werase(wNet);

    int gH, gW;
    getmaxyx(wCpu, gH, gW);
    int samples = MIN(HIST_MAX, gW - 2);

    draw_single_graph(wCpu, "CPU % (time)", &h_cpu, samples, 0.0, 100.0, use_color?2:0, "%");
    draw_single_graph(wMem, "MEM % (time)", &h_mem, samples, 0.0, 100.0, use_color?3:0, "%");

    // temp scale
    double tmin=20.0, tmax=90.0;
    if (have_tc) {
      double latest = hist_get_latest(&h_temp);
      tmin = MIN(tmin, latest - 10.0);
      tmax = MAX(tmax, latest + 10.0);
      tmin = MAX(0.0, tmin);
    }
    int tColor = (use_color ? ((have_tc && tc >= 80.0) ? 6 : 4) : 0);
    draw_single_graph(wTmp, "TEMP C (time)", &h_temp, samples, tmin, tmax, tColor, "C");

    double diskMax = MAX(1.0, MAX(hist_get_latest(&h_disk_r), hist_get_latest(&h_disk_w)) * 1.5);
    char diskExtra[128];
    snprintf(diskExtra, sizeof(diskExtra), "R/W MB/s (dev: %s)", have_disk?disk:"n/a");
    draw_dual_graph(wDisk, "DISK I/O (time)", &h_disk_r, &h_disk_w, samples,
                    0.0, diskMax, use_color?2:0, use_color?7:0,
                    "RD", "WR", "MB/s", diskExtra);

    double netMax  = MAX(1.0, MAX(hist_get_latest(&h_net_rx),  hist_get_latest(&h_net_tx))  * 1.5);
    char netExtra[160];
    snprintf(netExtra, sizeof(netExtra),
             "errs/drops Δ rx %llu/%llu tx %llu/%llu (if: %s)",
             d_rxE, d_rxD, d_txE, d_txD, have_iface?iface:"n/a");
    draw_dual_graph(wNet, "NET I/O (time)", &h_net_rx, &h_net_tx, samples,
                    0.0, netMax, use_color?2:0, use_color?7:0,
                    "RX", "TX", "MB/s", netExtra);

    wnoutrefresh(wCpu);
    wnoutrefresh(wMem);
    wnoutrefresh(wTmp);
    wnoutrefresh(wDisk);
    wnoutrefresh(wNet);

    // ---------------------------
    // TASKS bottom-left
    // ---------------------------
    werase(wProc);
    box(wProc, 0, 0);
    wattron(wProc, A_BOLD);
    mvwprintw(wProc, 0, 2, " TASKS (avg CPU) ");
    wattroff(wProc, A_BOLD);

    int y = 1;
    if (use_color) wattron(wProc, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(wProc, y, 2, "PID    AVG  CUR   RSS     S CMD");
    if (use_color) wattroff(wProc, COLOR_PAIR(5) | A_BOLD);
    y++;

    int start = scroll;
    int end = MIN(pt.n, start + proc_rows_visible);

    for (int i=start; i<end; i++) {
      ProcTrack *p = &pt.a[i];

      char rssStr[32];
      fmt_bytes(p->rss_bytes, rssStr, sizeof(rssStr));

      int hot = (p->cpu_cur >= 80.0);
      if (use_color && hot) wattron(wProc, COLOR_PAIR(6) | A_BOLD);
      else if (use_color) wattron(wProc, COLOR_PAIR(5));

      mvwprintw(wProc, y, 2, "%-6d %4.1f %4.1f %-7s %c %.*s",
               p->pid, p->cpu_avg, p->cpu_cur, rssStr, p->state,
               MAX(0, procW - 30), p->comm);

      if (use_color && hot) wattroff(wProc, COLOR_PAIR(6) | A_BOLD);
      else if (use_color) wattroff(wProc, COLOR_PAIR(5));

      y++;
    }

    if (use_color) wattron(wProc, COLOR_PAIR(5) | A_DIM);
    mvwprintw(wProc, procH-2, 2, "tasks:%d scroll:%d/%d  (100%%=1 core)",
              pt.n, scroll, maxScroll);
    if (use_color) wattroff(wProc, COLOR_PAIR(5) | A_DIM);

    wnoutrefresh(wProc);

    // Commit all at once
    doupdate();

    t_prev = t_cur;
    usleep((useconds_t)delay_ms * 1000);
  }

  proctable_free(&pt);
  if (wHdr) delwin(wHdr);
  if (wCpu) delwin(wCpu);
  if (wMem) delwin(wMem);
  if (wTmp) delwin(wTmp);
  if (wDisk) delwin(wDisk);
  if (wProc) delwin(wProc);
  if (wNet) delwin(wNet);
  endwin();
  return 0;
}
