#include "common.h"

#include <unistd.h>

size_t processRssKb() {
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return 0;
  char line[256];
  size_t kb = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      unsigned long v = 0;
      if (sscanf(line + 6, "%lu", &v) == 1) kb = v;
      break;
    }
  }
  fclose(f);
  return kb;
}
