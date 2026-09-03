#ifndef COMMON_H_
#define COMMON_H_

/*
 * Minimal compatibility shim for the UDS stack.
 * The old common.h pulled in MCU headers and standard C libraries.
 * The current project uses LL library with hc32_ll.h as the entry point.
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

/* MCU peripheral library */
#include "hc32_ll.h"

#endif /* COMMON_H_ */
