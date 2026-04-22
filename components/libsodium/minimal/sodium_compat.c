#include <stddef.h>

void sodium_memzero(void *const pnt, const size_t len)
{
  volatile unsigned char *volatile p = (volatile unsigned char *volatile) pnt;
  size_t i                           = 0;
  while (i < len) {
    p[i++] = 0U;
  }
}
