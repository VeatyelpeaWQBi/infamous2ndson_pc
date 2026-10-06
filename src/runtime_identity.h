#ifndef RUNTIME_IDENTITY_H
#define RUNTIME_IDENTITY_H
#include <stdint.h>
typedef struct {
    char nid[12], library[48], module[48];
    uint16_t library_version, module_version;
    int is_data;
} RuntimeImportIdentity;
/* NID#library:version#module:version#F|D. Legacy strings do not parse. */
int runtime_identity_parse(const char *name, RuntimeImportIdentity *out);
const char *runtime_identity_alias(const char *name, int is_data);
const char *runtime_extra_symbol(const char *name);
#endif
