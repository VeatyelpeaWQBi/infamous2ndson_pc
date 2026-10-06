/* Explicit library namespaces for the audited SDK. Dump-local IDs are never
 * compared across games; unsupported versions and data/function mismatches fail. */
#include "runtime_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { const char *library, *module, *scope; uint16_t version; } Scope;
static const Scope scopes[]={
    {"libc","libc","q#q",257},
    {"libSceLibcInternal","libSceLibcInternal","q#q",257},
    {"libkernel","libkernel","p#J",257},
    {"libScePosix","libkernel","I#J",257},
    {"libScePad","libScePad","A#B",257},
    {"libSceGnmDriver","libSceGnmDriver","B#C",257},
    {"libSceVideoOut","libSceVideoOut","C#D",0},
    {"libSceUserService","libSceUserService","D#E",257},
    {"libSceNet","libSceNet","E#F",257},
    {"libSceFios2","libSceFios2","F#G",257},
    {"libSceAudioOut","libSceAudioOut","G#H",257},
    {"libSceNetCtl","libSceNetCtl","J#K",257},
    {"libSceHttp","libSceHttp","K#L",257},
    {"libSceSsl","libSceSsl","L#M",257},
    {"libSceSysmodule","libSceSysmodule","M#N",257},
    {"libSceAjm","libSceAjm","N#O",257},
    {"libSceSaveData","libSceSaveData","O#P",257},
    {"libSceNpManager","libSceNpManager","R#S",257},
    {"libSceSystemService","libSceSystemService","V#W",257},
    {"libSceCommonDialog","libSceCommonDialog","Z#a",257},
    {"libSceAppContent","libSceAppContentUtil","c#d",257},
    {"libSceSaveDataDialog","libSceSaveDataDialog","d#e",257},
    {"libSceMsgDialog","libSceMsgDialog","j#k",257},
    {"libSceNpTrophy","libSceNpTrophy","l#m",257},
    {"libScePlayGo","libScePlayGo","n#o",256},
    {"libSceDiscMap","libSceDiscMap","libSceDiscMap",257},
    {"libSceErrorDialog","libSceErrorDialog","libSceErrorDialog",257},
    {"libSceVideodec","libSceVideodec","libSceVideodec",257},
    {"libSceLibcInternalExt","libSceLibcInternal","libSceLibcInternalExt",257},
};
static const struct { const char *nid, *symbol; } names[]={
#include "import_names.inc"
#include "import_names_second_son.inc"
};
static int token(const char *value, int nid) {
    for (const char *p=value;*p;++p)
        if (!((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || (*p>='0' && *p<='9') ||
              (nid && (*p=='+' || *p=='-')) || (!nid && *p=='_'))) return 0;
    return *value!=0;
}
int runtime_identity_parse(const char *name, RuntimeImportIdentity *out) {
    char lib_text[6]={0},mod_text[6]={0},type; int end=0;
    RuntimeImportIdentity value={0};
    if (!name || strlen(name)>=128 || sscanf(name,"%11[^#]#%47[^:]:%5[0-9]#%47[^:]:%5[0-9]#%c%n",
            value.nid,value.library,lib_text,value.module,mod_text,&type,&end)!=6 ||
        name[end] || strlen(value.nid)!=11 ||
        (type!='F' && type!='D') || !token(value.nid,1) || !token(value.library,0) || !token(value.module,0)) return 0;
    unsigned long lib=strtoul(lib_text,NULL,10),mod=strtoul(mod_text,NULL,10);
    if (lib>65535 || mod>65535) return 0;
    value.library_version=(uint16_t)lib; value.module_version=(uint16_t)mod; value.is_data=type=='D';
    if (out) *out=value;
    return 1;
}
const char *runtime_identity_alias(const char *name,int is_data) {
    RuntimeImportIdentity x;
    if (!runtime_identity_parse(name,&x) || x.is_data!=is_data || x.library_version!=1) return NULL;
    const Scope *scope=NULL;
    for (size_t i=0;i<sizeof(scopes)/sizeof(*scopes);++i)
        if (!strcmp(x.library,scopes[i].library) && !strcmp(x.module,scopes[i].module) && x.module_version==scopes[i].version) {
            scope=&scopes[i]; break;
        }
    if (!scope) return NULL;
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        const char *key=names[i].nid;
        if (strncmp(key,x.nid,11) || key[11]!='#') continue;
        const char *suffix=key+12;
        int match=!strcmp(suffix,scope->scope) || !strcmp(suffix,x.library);
        /* These libc-internal exports are deliberately hosted, not all libc exports. */
        if (!strcmp(x.library,"libSceLibcInternal") && !strcmp(suffix,"libSceLibcInternal")) match=1;
        /* Several POSIX NIDs are also exported by libkernel; no other namespace aliases. */
        if ((!strcmp(x.library,"libkernel") || !strcmp(x.library,"libScePosix")) &&
            (!strcmp(suffix,"I#J") || !strcmp(suffix,"p#J"))) match=1;
        if (!match) continue;
        int data=!strcmp(names[i].symbol,"__stack_chk_guard") || !strcmp(names[i].symbol,"__progname") ||
                 !strcmp(x.nid,"ZT4ODD2Ts9o");
        if (data!=is_data) return NULL;
        return key;
    }
    return NULL;
}
const char *runtime_extra_symbol(const char *name) {
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(name,names[i].nid)) return names[i].symbol;
    return NULL;
}
