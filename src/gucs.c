#include <string.h>
#include "postgres.h"
#include "utils/guc.h"
#include "rustica_wamr.h"

char *rst_listen_addresses = NULL;
int rst_port = 8080;

void
rst_init_gucs() {
    DefineCustomStringVariable(
        "rustica.listen_addresses",
        "Sets the host name or IP address(es) to listen to.",
        "Default is 'localhost'.",
        &rst_listen_addresses,
        "localhost",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL);
    DefineCustomIntVariable("rustica.port",
                            "Sets the TCP port the server listens on.",
                            "Default is 8080.",
                            &rst_port,
                            8080,
                            1,
                            65535,
                            PGC_USERSET,
                            0,
                            NULL,
                            NULL,
                            NULL);
}
