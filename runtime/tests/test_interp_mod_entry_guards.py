"""Keep function-entry callbacks and their opt-in replacement contract intact."""

from pathlib import Path


def main():
    source = (Path(__file__).resolve().parents[1] /
              'src/dirty_ram_interp.c').read_text(encoding='utf-8')
    start = source.index('static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {')
    source = source[start:]
    static_dispatch = source[source.index('/* B-2: statically-compiled'):]
    static_dispatch = static_dispatch[:static_dispatch.index('/* A-1:')]
    assert static_dispatch.index('g_exec_phase = 3;') < static_dispatch.index(
        'psx_overlay_dispatch(cpu, addr);') < static_dispatch.index(
        'g_exec_phase = previous_phase;') < static_dispatch.index('if (handled) return 1;')
    assert source.count('if (psx_mod_function_entry(cpu, addr)) {') == 1
    top = source[source.index('if (psx_mod_function_entry(cpu, addr)) {'):]
    top = top[:top.index('}')]
    assert 'cpu->pc = cpu->gpr[31];' in top and 'return 1;' in top
    local_start = source.index('if (allow_local_dirty_flow && target != 0 &&')
    local_end = source.index('current_page = target_phys >> 12;', local_start)
    local = source[local_start:local_end]
    probe_start = local.index('if (psx_overlay_static_can_dispatch(target))')
    probe_end = local.index('#endif', probe_start)
    probe = local[probe_start:probe_end]
    assert 'g_dirty_interp_chain_target = target;' in probe
    assert 'OV_FPLOG_RET1();' in probe
    assert 'psx_overlay_dispatch(cpu' not in probe
    assert local.count('if (psx_mod_function_entry(cpu, target)) {') == 1
    replacement = local[local.index(
        'if (psx_mod_function_entry(cpu, target)) {'):]
    replacement = replacement[:replacement.index('}')]
    assert 'cpu->pc = cpu->gpr[31];' in replacement and 'return 1;' in replacement
    assert local.index('overlay_loader_dispatch(cpu, target)') < local.index(
        'psx_mod_function_entry(cpu, target)') < local.index('pc = target;')

    runtime = (Path(__file__).resolve().parents[1] /
               'src/mod_function_entry.cpp').read_text(encoding='utf-8')
    assert 'if (cpu && cpu == s_mod_entry_cpu) s_mod_entry_skip = true;' in runtime
    assert 'CPUState* const outer_cpu = s_mod_entry_cpu;' in runtime
    assert 'const bool outer_skip = s_mod_entry_skip;' in runtime
    assert runtime.index('s_mod_entry_cpu = cpu;') < runtime.index(
        'plugin.callback(cpu, address);') < runtime.index(
        's_mod_entry_cpu = outer_cpu;')
    assert 'return skip;' in runtime
    print('function-entry replacement guards: PASS')


if __name__ == '__main__':
    main()
