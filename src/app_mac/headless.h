/*
 * headless.h -- AppSandbox headless daemon (AppSandbox --headless), macOS.
 * Mirrors src/app_win/headless.h.
 */

#ifndef HEADLESS_MAC_H
#define HEADLESS_MAC_H

/* Run the AppSandbox headless daemon. Owns the core for the process lifetime
   and serves a Docker-style local HTTP API over loopback. Returns the process
   exit code (2 = another AppSandbox instance is running). */
int run_headless(int argc, const char *argv[]);

int asb_host_prepare_user(void);
int asb_api_start(int with_gui);
void asb_api_stop(void);

int asb_instance_lock_acquire(void);

#endif /* HEADLESS_MAC_H */
