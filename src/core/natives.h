/* Shared helpers for the native layer. */
#ifndef EXEN_NATIVES_H
#define EXEN_NATIVES_H

#include <stdarg.h>

#include "vm.h"

void root_push(ExenVM *vm, ref_t r);
void root_pop(ExenVM *vm, int n);
uint32_t *inst(ExenVM *vm, ref_t obj, int cls);
ref_t new_char_array(ExenVM *vm, const uint8_t *src, int n);
ref_t vm_new_cstring(ExenVM *vm, const char *s);
void vm_log(ExenVM *vm, const char *fmt, ...);
void natives_register(int index, ExNativeFn fn);
const char *native_class(int index);
int native_implemented(int index);

#endif
