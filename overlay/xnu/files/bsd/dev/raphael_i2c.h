/* Open-drain, bounded software I2C for the raphael bring-up driver. APSL-2.0. */
#ifndef RAPHAEL_I2C_H
#define RAPHAEL_I2C_H
#include <stdint.h>
struct raphael_i2c {
    void *cookie;
    /* release=1 means input/high impedance; release=0 means drive LOW. */
    void (*sda)(void *, int release);
    void (*scl)(void *, int release);
    int (*read_sda)(void *);
    int (*read_scl)(void *);
    void (*wait_half)(void *);
};
static inline int raphael_i2c_high(struct raphael_i2c *b)
{
    b->scl(b->cookie, 1);
    /* 200 half-periods bound clock stretching (~1 ms at nominal 100 kHz). */
    for (unsigned i = 0; i < 200; ++i) {
        if (b->read_scl(b->cookie)) { b->wait_half(b->cookie); return 1; }
        b->wait_half(b->cookie);
    }
    return 0;
}
static inline void raphael_i2c_low(struct raphael_i2c *b)
{
    b->scl(b->cookie, 0); b->wait_half(b->cookie);
}
static inline int raphael_i2c_stop(struct raphael_i2c *b)
{
    raphael_i2c_low(b); b->sda(b->cookie, 0); b->wait_half(b->cookie);
    int ok = raphael_i2c_high(b);
    b->sda(b->cookie, 1); b->wait_half(b->cookie);
    return ok && b->read_sda(b->cookie);
}
static inline int raphael_i2c_start(struct raphael_i2c *b)
{
    b->sda(b->cookie, 1);
    if (!raphael_i2c_high(b) || !b->read_sda(b->cookie)) return 0;
    b->sda(b->cookie, 0); b->wait_half(b->cookie); raphael_i2c_low(b);
    return 1;
}
static inline int raphael_i2c_send(struct raphael_i2c *b, uint8_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        b->sda(b->cookie, !!(value & 0x80)); b->wait_half(b->cookie);
        if (!raphael_i2c_high(b)) return 0;
        raphael_i2c_low(b); value <<= 1;
    }
    b->sda(b->cookie, 1); b->wait_half(b->cookie);
    if (!raphael_i2c_high(b)) return 0;
    int ack = !b->read_sda(b->cookie);
    raphael_i2c_low(b);
    return ack;
}
static inline int raphael_i2c_receive(struct raphael_i2c *b, uint8_t *value, int last)
{
    uint8_t next = 0;
    b->sda(b->cookie, 1);
    for (unsigned i = 0; i < 8; ++i) {
        if (!raphael_i2c_high(b)) return 0;
        next = (uint8_t)((next << 1) | !!b->read_sda(b->cookie));
        raphael_i2c_low(b);
    }
    b->sda(b->cookie, last); /* ACK all but the final byte, then NACK. */
    b->wait_half(b->cookie);
    if (!raphael_i2c_high(b)) return 0;
    raphael_i2c_low(b); b->sda(b->cookie, 1);
    *value = next;
    return 1;
}
/* GT9886 register addresses are 16-bit, high byte first; data bytes are not swapped. */
static inline int raphael_i2c_reg(struct raphael_i2c *b, uint16_t reg,
    uint8_t *data, unsigned length, int reading)
{
    int ok = 0;
    if (!length || !data || length > 256) return 0;
    if (!raphael_i2c_start(b) || !raphael_i2c_send(b, 0x5d << 1) ||
        !raphael_i2c_send(b, (uint8_t)(reg >> 8)) ||
        !raphael_i2c_send(b, (uint8_t)reg)) goto done;
    if (reading && (!raphael_i2c_start(b) || !raphael_i2c_send(b, (0x5d << 1) | 1))) goto done;
    for (unsigned i = 0; i < length; ++i) {
        if (reading ? !raphael_i2c_receive(b, &data[i], i + 1 == length) :
            !raphael_i2c_send(b, data[i])) goto done;
    }
    ok = 1;
done:
    if (!raphael_i2c_stop(b)) ok = 0;
    return ok;
}
#endif
