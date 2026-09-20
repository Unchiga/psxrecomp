/* Exercise the real pacing/flow-control predicates together. No disc image or
 * SDL is needed; unreferenced controller code is discarded by the linker.
 *
 * cc -DPSX_NO_DEBUG_TOOLS -ffunction-sections -fdata-sections \
 *   -Iruntime/include runtime/tests/test_cdrom_accelerated_consumer.c \
 *   -Wl,--gc-sections -o /tmp/cdrom-consumer-test && /tmp/cdrom-consumer-test
 */
#include "../src/cdrom.c"

int dma_cdrom_transfer_active(void) { return 0; }

int main(void) {
    int failed = 0;
    const int modes[] = {0x00, 0x20, 0x80, 0xA0, 0x48, 0xA8, 0xE0};
    const int divisors[] = {0, 1, 4, 32};
    for (unsigned m = 0; m < sizeof(modes)/sizeof(modes[0]); ++m) {
        for (unsigned d = 0; d < sizeof(divisors)/sizeof(divisors[0]); ++d) {
            mode_reg = (uint8_t)modes[m];
            g_disc_speed_divisor = divisors[d];
            xa_stream_active = 0;
            s_warm_route_active = 0;
            irq_flag = 1;  /* previous data-ready has not been acknowledged */
            pending_dataready = 0;
            int faster = apply_read_speed(225792) < 225792;
            int held = accelerated_consumer_blocked();
            if (!!faster != !!held) {
                fprintf(stderr, "FAIL mode=%02x divisor=%d faster=%d held=%d\n",
                        mode_reg, divisors[d], faster, held);
                failed = 1;
            }
            pending_dataready = 1;
            irq_flag = 0;
            if (!!accelerated_consumer_blocked() != !!faster) failed = 1;
            xa_stream_active = 1;
            if (apply_read_speed(225792) != 225792 || accelerated_consumer_blocked())
                failed = 1;
        }
    }

    /* Old snapshots can preserve the short-lived controller bug where XA EOF
     * stopped ReadS. Only that impossible EOF state is resumed; an ordinary
     * paused XA stream must remain stopped. */
    reading = 0;
    read_cmd = 0;
    read_delay = 0;
    mode_reg = 0x4A;
    xa_stream_active = 1;
    xa_data_end_pending = 1;
    last_sector_have_raw = 1;
    last_sector_raw_mode = CDROM_SECTOR_MODE2;
    last_sector_xa_submode = XA_SUBMODE_EOF | XA_SUBMODE_AUDIO;
    stat_reg = CDSTAT_MOTOR;
    cdrom_repair_legacy_xa_eof_snapshot();
    if (!reading || read_cmd != 0x1B || read_delay != 451584 ||
        xa_data_end_pending || !(stat_reg & CDSTAT_READ)) {
        failed = 1;
    }

    reading = 0;
    read_cmd = 0;
    read_delay = 0;
    last_sector_xa_submode = XA_SUBMODE_AUDIO;
    cdrom_repair_legacy_xa_eof_snapshot();
    if (reading || read_cmd || read_delay) failed = 1;

    if (!failed) puts("PASS: every accelerated mode protects its pending consumer");
    return failed;
}
