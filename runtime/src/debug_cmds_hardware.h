/* Debug-server commands: emulated hardware state.
 *
 * Declared for the dispatch table in debug_server.c, which stays there because
 * it is the one place every command name is visible at once. */
#ifndef DEBUG_CMDS_HARDWARE_H
#define DEBUG_CMDS_HARDWARE_H

void handle_a0_history(int id, const char *json);
void handle_c0_history(int id, const char *json);
void handle_capture_quads(int id, const char *json);
void handle_cdrom_command_history(int id, const char *json);
void handle_cdrom_command_history_clear(int id, const char *json);
void handle_cdrom_sector_dump(int id, const char *json);
void handle_cdrom_sector_history(int id, const char *json);
void handle_cdrom_sector_history_clear(int id, const char *json);
void handle_cdrom_state(int id, const char *json);
void handle_cdrom_trace_clear(int id, const char *json);
void handle_cdrom_trace_dump(int id, const char *json);
void handle_cycles_to_next_event(int id, const char *json);
void handle_dma_cdrom_history(int id, const char *json);
void handle_dma_state(int id, const char *json);
void handle_dma_trace_clear(int id, const char *json);
void handle_dma_trace_dump(int id, const char *json);
void handle_fast_loads(int id, const char *json);
void handle_fill_ram(int id, const char *json);
void handle_geom_correction(int id, const char *json);
void handle_get_quads(int id, const char *json);
void handle_gpu_frame_dump(int id, const char *json);
void handle_gpu_opcodes(int id, const char *json);
void handle_gpu_ring_stats(int id, const char *json);
void handle_gpu_state(int id, const char *json);
void handle_gte_frame_stats(int id, const char *json);
void handle_gte_intpl_dump(int id, const char *json);
void handle_gte_latch_dump(int id, const char *json);
void handle_gte_ring_dump(int id, const char *json);
void handle_gte_state(int id, const char *json);
void handle_irq_state(int id, const char *json);
void handle_mc_status(int id, const char *json);
void handle_mem_words(int id, const char *json);
void handle_ram_dump_file(int id, const char *json);
void handle_read_ram(int id, const char *json);
void handle_sio_state(int id, const char *json);
void handle_spu_events(int id, const char *json);
void handle_spu_events_reset(int id, const char *json);
void handle_spu_ram(int id, const char *json);
void handle_spu_status(int id, const char *json);
void handle_spu_voices(int id, const char *json);
void handle_timers_state(int id, const char *json);
void handle_vblank_rate(int id, const char *json);
void handle_write_ram(int id, const char *json);
void handle_ws_aspect_cone_site(int id, const char *json);

#endif /* DEBUG_CMDS_HARDWARE_H */
