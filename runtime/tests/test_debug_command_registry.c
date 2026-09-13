#include <assert.h>
#include <stdio.h>

#include "psx_debug_commands.h"

static int s_called;

static void handler(int id, const char *json)
{
    (void)json;
    s_called = id;
}

int main(void)
{
    static char names[129][24];

    for (int i = 0; i < 128; i++) {
        snprintf(names[i], sizeof names[i], "title_probe_%03d", i);
        assert(psx_debug_add_command(names[i], handler) == 1);
    }
    snprintf(names[128], sizeof names[128], "overflow_probe");
    assert(psx_debug_add_command(names[128], handler) == 0);
    assert(psx_debug_add_command("title_probe_000", handler) == 0);
    assert(psx_debug_run_command("title_probe_127", 127, "{}") == 1);
    assert(s_called == 127);
    assert(psx_debug_run_command("not_registered", 1, "{}") == 0);
    return 0;
}
