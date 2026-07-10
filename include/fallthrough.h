#pragma once

#if __has_attribute(__fallthrough__)
#define fallthrough __attribute__((__fallthrough__))
#else
#define fallthrough                                                                                                    \
	do {                                                                                                                 \
	} while (0) /* fallthrough */
#endif
