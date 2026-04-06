/*
 * Minimal includes.h stub for standalone wpa_ctrl compilation.
 * Maps to standard POSIX headers required by wpa_ctrl.c.
 *
 * This file is intentionally NOT copied from wpa_supplicant source.
 * It provides only what wpa_ctrl.c needs for Linux/embedded targets.
 */
#ifndef INCLUDES_H
#define INCLUDES_H

#include "build_config.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <sys/time.h>

#endif /* INCLUDES_H */
