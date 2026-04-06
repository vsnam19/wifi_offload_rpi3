/*
 * Minimal build_config.h stub for standalone wpa_ctrl compilation.
 * Enables Unix-domain socket control interface only.
 */
#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

#ifndef CONFIG_CTRL_IFACE
#define CONFIG_CTRL_IFACE
#endif

#ifndef CONFIG_CTRL_IFACE_UNIX
#define CONFIG_CTRL_IFACE_UNIX
#endif

#ifndef CONFIG_OS_POSIX
#define CONFIG_OS_POSIX
#endif

#endif /* BUILD_CONFIG_H */
