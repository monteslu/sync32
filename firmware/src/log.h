#ifndef SYNC32_LOG_H
#define SYNC32_LOG_H
#include <stdint.h>
void s32_log_init(void);                 // call once, early in main
void s32_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void s32_log_dump_stdio(void);
uint32_t s32_log_boot_count(void);
void s32_log_mark_alive(void);           // this boot is healthy
uint32_t s32_log_rapid_deaths(void);     // consecutive boots that never were
#endif
