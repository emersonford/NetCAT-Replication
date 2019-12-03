/* vim: set noet: */
#ifndef DEBUG_H_
#include <stdio.h>

#define DEBUG 0
#define debug_print(fmt, ...) \
	if (DEBUG) { fprintf(stdout, fmt, ##__VA_ARGS__); }

#define data_print(fmt, ...) \
	do { if (!DEBUG) fprintf(stdout, fmt, ##__VA_ARGS__); } while (0)

#endif // DEBUG_H_
