#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef CONFIG_FORCE4_SD_CARD
bool     sdcard_init(void);
bool     sdcard_is_mounted(void);
void     sdcard_list_files(void);
bool     sdcard_delete_file(const char *filename);
void     sdcard_print_info(void);
bool     sdcard_write_test(const char *filename, size_t nbytes);
#else
#include <stdio.h>
static inline bool   sdcard_init(void)                              { return false; }
static inline bool   sdcard_is_mounted(void)                        { return false; }
static inline void   sdcard_list_files(void)                        { printf("SD card not enabled\n"); }
static inline bool   sdcard_delete_file(const char *f)              { (void)f; return false; }
static inline void   sdcard_print_info(void)                        { printf("SD card not enabled\n"); }
static inline bool   sdcard_write_test(const char *f, size_t n)     { (void)f; (void)n; return false; }
#endif
