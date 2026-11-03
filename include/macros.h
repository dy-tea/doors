#pragma once

#include <wlr/util/log.h>

#if DEBUG
#define ONCE() \
  do { \
    static int _called_once_flag_##__LINE__ = 0; \
    if (_called_once_flag_##__LINE__) { \
     	wlr_log(WLR_ERROR, "Function %s called more than once", __func__); \
      exit(1); \
    } \
    _called_once_flag_##__LINE__ = 1; \
  } while(0)
#else
#define ONCE() ;
#endif

#if __has_attribute(__fallthrough__)
#define fallthrough __attribute__((__fallthrough__))
#else
#define fallthrough                                                                                                    \
	do {                                                                                                                 \
	} while (0) /* fallthrough */
#endif

#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b
#define CLAMP(val, lo, hi) MIN(MAX(val, lo), hi)
