/*
 * GT9886 Normandy wire-format decoder, independently implemented for HackiOS.
 * Protocol reference: goodix_gtx8.{c,h}, Aospa-raphael-unofficial/linux.
 * APSL-2.0, as the containing HackiOS kernel repository.
 */
#ifndef RAPHAEL_GT9886_H
#define RAPHAEL_GT9886_H
#include <stdint.h>
#define RAPHAEL_TOUCHES 10u
#define RAPHAEL_REPORT_MAX 84u
struct raphael_contact { uint16_t x, y; };
struct raphael_frame {
    uint16_t mask;
    struct raphael_contact contact[RAPHAEL_TOUCHES];
};
/* 1: complete touch frame, 0: other/no event, -1: invalid. Out unchanged on error. */
static inline int
raphael_decode(const uint8_t *data, unsigned length, struct raphael_frame *out)
{
    struct raphael_frame next = {0};
    unsigned count, need;
    uint8_t sum = 0;
    if (!data || !out || length < 2) return -1;
    if (!(data[0] & 0x80)) return 0;
    count = data[1] & 15u;
    if (count > RAPHAEL_TOUCHES) return -1;
    need = 2u + count * 8u + 2u;
    if (length < need) return -1;
    /* The reference GT9886 driver does not checksum an empty release frame. */
    if (count) {
        for (unsigned i = 0; i < need; ++i) sum = (uint8_t)(sum + data[i]);
        if (sum) return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = data + 2u + i * 8u;
        unsigned id = p[0];
        if (id >= RAPHAEL_TOUCHES || (next.mask & (1u << id))) return -1;
        next.mask |= (uint16_t)(1u << id);
        next.contact[id].x = (uint16_t)(p[1] | (uint16_t)p[2] << 8);
        next.contact[id].y = (uint16_t)(p[3] | (uint16_t)p[4] << 8);
    }
    *out = next;
    return 1;
}
static inline uint32_t raphael_axis(uint16_t coordinate, unsigned size)
{
    if (size < 2) return 0;
    if (coordinate >= size) coordinate = (uint16_t)(size - 1u);
    return (uint32_t)coordinate * 65535u / (size - 1u);
}
#endif
