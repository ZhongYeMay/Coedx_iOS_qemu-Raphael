/*
 * Experimental Redmi K20 Pro (raphael) GT9886 over GPIO I2C.
 * APSL-2.0. Board facts: sm8150-xiaomi-raphael.dts, sm8150.dtsi,
 * pinctrl-sm8150.c; packet format: goodix_gtx8.c/.h. No Linux code included.
 * Requires raphael=1 raphael_touch=1; no firmware upload, suspend or IRQ driver.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include "raphael_gt9886.h"
#include "raphael_i2c.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int);
extern kern_return_t thread_terminate(thread_t);

/* Every pin used below belongs to SOUTH, whose named resource is 0x03d00000.
 * Do not use the redwood TLMM base, or the first resource (WEST) of sm8150. */
#define SOUTH_PHYS 0x03d00000UL
#define SOUTH_SIZE 0x100000UL
#define SDA 57u
#define SCL 58u
#define IRQ 122u
#define RESET 54u
#define AVDD 98u
#define VDDIO 100u
static const unsigned pins[] = { SDA, SCL, IRQ, RESET, AVDD, VDDIO };
static uint32_t saved_cfg[6], saved_out[6];
static vm_offset_t south;
static unsigned mapped_pins;
static volatile uint32_t touch_stop, bus_lock;
static struct raphael_frame previous;

static uint32_t reg_read(unsigned pin, unsigned offset)
{
    return *(volatile uint32_t *)(south + 0x1000UL * pin + offset);
}
static void reg_write(unsigned pin, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(south + 0x1000UL * pin + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void gpio_mode(unsigned pin, int output)
{
    uint32_t cfg = reg_read(pin, 0);
    cfg &= ~((15u << 2) | (1u << 9)); /* GPIO mux, preserve pull/drive controls. */
    if (output) cfg |= 1u << 9;
    reg_write(pin, 0, cfg);
}
static void gpio_output(unsigned pin, int high)
{
    reg_write(pin, 4, high ? 2u : 0u);
    gpio_mode(pin, 1);
}
static void open_drain(unsigned pin, int release)
{
    /* Never drive a high level on the bus. External pull-ups must exist. */
    if (release) gpio_mode(pin, 0);
    else gpio_output(pin, 0);
}
static void bus_sda(void *cookie, int release) { (void)cookie; open_drain(SDA, release); }
static void bus_scl(void *cookie, int release) { (void)cookie; open_drain(SCL, release); }
static int bus_read_sda(void *cookie) { (void)cookie; return reg_read(SDA, 4) & 1u; }
static int bus_read_scl(void *cookie) { (void)cookie; return reg_read(SCL, 4) & 1u; }
static void half_period(void *cookie) { (void)cookie; delay_for_interval(5, NSEC_PER_USEC); }
static struct raphael_i2c bus = {
    .sda = bus_sda, .scl = bus_scl, .read_sda = bus_read_sda,
    .read_scl = bus_read_scl, .wait_half = half_period,
};
static void restore_pins(void)
{
    while (mapped_pins) {
        unsigned i = --mapped_pins;
        /* Remove output enable before restoring the saved latch and mux. */
        gpio_mode(pins[i], 0);
        reg_write(pins[i], 4, saved_out[i]);
        reg_write(pins[i], 0, saved_cfg[i]);
    }
}
static void report_frame(const struct raphael_frame *next)
{
    for (unsigned id = 0; id < RAPHAEL_TOUCHES; ++id) {
        unsigned before = previous.mask & (1u << id), now = next->mask & (1u << id);
        const struct raphael_contact *p = now ? &next->contact[id] : &previous.contact[id];
        if (now || before) redwood_touch_post(now ? (before ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN) :
            REDWOOD_TOUCH_UP, id, raphael_axis(p->x, 1080), raphael_axis(p->y, 2340));
    }
    previous = *next;
}
/* 1 accepted, 0 idle/non-touch, -1 transport/packet failure. */
static int poll_touch(void)
{
    uint8_t data[RAPHAEL_REPORT_MAX], zero = 0;
    struct raphael_frame frame;
    if (!raphael_i2c_reg(&bus, 0x4100, data, 12, 1)) return -1;
    if (!data[0]) return 0;
    unsigned n = data[1] & 15u;
    int valid = 1;
    if ((data[0] & 0x80) && n > RAPHAEL_TOUCHES) valid = 0;
    if (valid && (data[0] & 0x80) && n > 1 &&
        !raphael_i2c_reg(&bus, 0x410c, data + 12, (n - 1u) * 8u, 1)) return -1;
    int result = valid ? raphael_decode(data, n > 1 ? 4u + n * 8u : 12u, &frame) : -1;
    /* Clear a complete bad packet as well; don't publish duplicate events after a failed ACK. */
    if (!raphael_i2c_reg(&bus, 0x4100, &zero, 1, 0)) return -1;
    if (result == 1) report_frame(&frame);
    return result;
}
static void touch_thread(__unused void *arg, __unused wait_result_t result)
{
    uint8_t product[4];
    uint32_t reset = 0;
    int good = 0;
    uint32_t expected = 0;
    if (__atomic_load_n(&touch_stop, __ATOMIC_ACQUIRE)) goto done;
    if (!__atomic_compare_exchange_n(&bus_lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) goto done;
    if (__atomic_load_n(&touch_stop, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE); goto done;
    }
    south = ml_io_map(SOUTH_PHYS, SOUTH_SIZE);
    if (!south) { __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE); goto done; }
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        saved_cfg[i] = reg_read(pins[i], 0);
        saved_out[i] = reg_read(pins[i], 4) & 2u;
        ++mapped_pins;
    }
    gpio_output(VDDIO, 1); gpio_output(AVDD, 1);
    PE_parse_boot_argn("raphael_touch_reset", &reset, sizeof(reset));
    if (reset) gpio_output(RESET, 0);
    /* All waits outside the bus transfer are schedulable, not busy loops. */
    IOSleep(300);
    gpio_output(RESET, 1); IOSleep(100);
    gpio_mode(IRQ, 0);
    open_drain(SDA, 1); open_drain(SCL, 1);
    if (!raphael_i2c_reg(&bus, 0x4535, product, sizeof(product), 1) ||
        memcmp(product, "9886", 4)) {
        printf("raphael-touch: GT9886 probe failed; restoring pins\n");
        restore_pins(); __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE); goto done;
    }
    good = 1;
    printf("raphael-touch: GT9886 identified, GPIO I2C 0x5d, SOUTH TLMM; polling 8 ms\n");
    __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE);
    for (unsigned ticks = 0, errors = 0; !__atomic_load_n(&touch_stop, __ATOMIC_ACQUIRE); ++ticks) {
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&bus_lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            IOSleep(8); continue;
        }
        if (__atomic_load_n(&touch_stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE); break;
        }
        int r = 0;
        /* IRQ is active low. Also look periodically in case the level was missed. */
        if (!(reg_read(IRQ, 4) & 1u) || ticks % 10u == 0) r = poll_touch();
        if (r < 0 && ++errors <= 4) printf("raphael-touch: I2C or report failure %u\n", errors);
        __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE);
        IOSleep(8);
    }
done:
    if (!good) printf("raphael-touch: stopped before input enabled\n");
    thread_terminate(current_thread());
}
void raphael_touch_init(void)
{
    uint32_t board = 0, enabled = 0;
    thread_t thread;
    PE_parse_boot_argn("raphael", &board, sizeof(board));
    PE_parse_boot_argn("raphael_touch", &enabled, sizeof(enabled));
    if (board != 1 || enabled != 1) return;
    if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
void raphael_touch_quiesce(void)
{
    __atomic_store_n(&touch_stop, 1u, __ATOMIC_RELEASE);
    for (unsigned tries = 0; tries < 10000; ++tries) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&bus_lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            if (south) restore_pins();
            __atomic_store_n(&bus_lock, 0u, __ATOMIC_RELEASE); return;
        }
        delay_for_interval(10, NSEC_PER_USEC);
    }
    /* Do not race an in-flight transfer if a CPU has stopped. */
    printf("raphael-touch: busy at return, pin restoration skipped\n");
}
