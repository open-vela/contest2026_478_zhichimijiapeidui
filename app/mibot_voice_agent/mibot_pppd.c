/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Route-A PPP client for the SF32 side of the dedicated ESP32 network link.
 * NuttX's pppd() owns the TUN device and negotiates IPv4 over the UART.  The
 * application is intentionally separate from mibot_agent so the transport
 * can be diagnosed without starting the AI services.
 */

#include <nuttx/config.h>

#include <stdio.h>
#include <unistd.h>

#include "netutils/pppd.h"

#ifndef CONFIG_MIBOT_ROUTE_A_NET_TTY
#  define CONFIG_MIBOT_ROUTE_A_NET_TTY "/dev/ttyS1"
#endif

#ifndef CONFIG_MIBOT_ROUTE_A_PPP_RETRY_SEC
#  define CONFIG_MIBOT_ROUTE_A_PPP_RETRY_SEC 5
#endif

int mibot_pppd_main(int argc, char *argv[])
{
  const struct pppd_settings_s settings =
  {
    .ttyname = CONFIG_MIBOT_ROUTE_A_NET_TTY,
    /* The ESP32 peer is a passive PPP server; no modem chat is needed. */
    .connect_script = NULL,
    .disconnect_script = NULL,
#ifdef CONFIG_NETUTILS_PPPD_PAP
    .pap_username = "",
    .pap_password = "",
#endif
  };
  int ret;

  (void)argc;
  (void)argv;

  printf("mibot_pppd: PPP client on %s\n", settings.ttyname);

  /* pppd normally stays in this loop for the lifetime of the link.  Retry
   * after an open/TUN failure so a late-created UART or filesystem does not
   * require a reboot. */
  for (;;)
    {
      ret = pppd(&settings);
      printf("mibot_pppd: pppd exited (%d), retrying\n", ret);
      sleep(CONFIG_MIBOT_ROUTE_A_PPP_RETRY_SEC);
    }

  return 0;
}
