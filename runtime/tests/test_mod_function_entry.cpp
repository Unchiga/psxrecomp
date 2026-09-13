#include "cpu_state.h"
#include "mod_plugins.h"

#include <cstdio>

namespace {

int failures;
int plain_calls;
int inner_calls;
int outer_calls;

void check(bool condition, const char* name) {
    if (condition) std::printf("PASS  %s\n", name);
    else {
        std::fprintf(stderr, "FAIL  %s\n", name);
        ++failures;
    }
}

void plain(CPUState*, uint32_t) { ++plain_calls; }

void inner_skip(CPUState* cpu, uint32_t) {
    ++inner_calls;
    psx_mod_skip_current_function(cpu);
}

void outer_nested(CPUState* cpu, uint32_t) {
    ++outer_calls;
    check(psx_mod_function_entry(cpu, 0x2000u) == 1,
          "nested callback can replace its own function");
}

void outer_skip_after_nested(CPUState* cpu, uint32_t) {
    ++outer_calls;
    check(psx_mod_function_entry(cpu, 0x2000u) == 1,
          "nested replacement remains local");
    psx_mod_skip_current_function(cpu);
}

}  // namespace

int main() {
    CPUState cpu{};
    CPUState other{};

    check(psx_mod_register_function_entry_plugin(
              "test.plain", 0x1000u, plain) == 1,
          "registers a callback");
    check(psx_mod_register_function_entry_plugin(
              "test.plain", 0x1000u, plain) == 0,
          "rejects a duplicate registration");
    check(psx_mod_register_function_entry_plugin(
              "test.inner", 0x2000u, inner_skip) == 1,
          "registers a replacing callback");
    check(psx_mod_function_entry(&cpu, 0x1000u) == 0 && plain_calls == 1,
          "ordinary callback preserves the guest body");
    check(psx_mod_function_entry(&cpu, 0x2000u) == 1 && inner_calls == 1,
          "callback can replace the current guest body");
    check(psx_mod_function_entry(nullptr, 0x2000u) == 0,
          "null CPU cannot be replaced");

    check(psx_mod_register_function_entry_plugin(
              "test.outer.noskip", 0x3000u, outer_nested) == 1,
          "registers nested non-replacing callback");
    check(psx_mod_function_entry(&cpu, 0x3000u) == 0,
          "inner replacement does not replace its outer caller");

    check(psx_mod_register_function_entry_plugin(
              "test.outer.skip", 0x4000u, outer_skip_after_nested) == 1,
          "registers nested replacing callback");
    check(psx_mod_function_entry(&cpu, 0x4000u) == 1,
          "outer callback may replace after a nested replacement");

    psx_mod_skip_current_function(&other);
    check(psx_mod_function_entry(&cpu, 0x1000u) == 0,
          "out-of-callback and foreign-CPU skip requests are ignored");
    check(outer_calls == 2 && inner_calls == 3,
          "nested callbacks ran exactly once per dispatch");
    return failures ? 1 : 0;
}
