#ifndef CLI_H_
#define CLI_H_
#include <stdlib.h>
#include <stdint.h>

/** Route entry in configuration */
struct config_route {
  /** Destination IP address */
  uint32_t ip;
  /** Destination prefix length */
  uint8_t ip_prefix;
  /** Next hop IP */
  uint32_t next_hop_ip;
  /** Next pointer for route list */
  struct config_route *next;
};

static inline int parse_int64(const char *s, uint64_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static inline int parse_int32(const char *s, uint32_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static inline int parse_int16(const char *s, uint16_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static inline int parse_int8(const char *s, uint8_t *pi)
{
  char *end;
  *pi = strtoul(s, &end, 10);
  if (!*s || *end)
    return -1;
  return 0;
}

static inline int parse_double(const char *s, double *pd)
{
  char *end;
  *pd = strtod(s, &end);
  if (!*s || *end)
    return -1;
  return 0;
}
#endif