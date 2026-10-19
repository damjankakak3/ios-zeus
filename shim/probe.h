/* zeus probe: shared helpers between probe.c (screen, DT, main) and usb.c */
#pragma once
#include <stdint.h>

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }
/* Every hardware register write goes through here (grep for wr32). */
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

uint64_t now(void);
uint64_t freq(void);
void delay_ms(uint64_t ms);

/* Text goes to the screen and to the log buffer (sent over USB). */
void puts_c(const char *s, uint32_t fg);
void puthex(uint64_t v, int digits, uint32_t fg);
void putdec(uint64_t v, uint32_t fg);
void line(const char *label, uint64_t v, int digits);
extern char logbuf[];
extern volatile uint32_t loglen;

#define C_WHITE	0x00ffffffU
#define C_GRAY	0x00a0a0a0U
#define C_RED	0x00ff0000U
#define C_GREEN	0x0000ff00U
#define C_BLUE	0x000000ffU
#define C_YEL	0x00ffff00U

void usb_start(void);
void usb_poll(void);
