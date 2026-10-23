// Minimal reflection layer for Back 4 Blood's modified UE 4.25 (see docs/NOTES.md for the layout table).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

// ---- addresses for Steam build 14216215 (verified against byte signatures in ue_init) ----
// Static VAs at the preferred image base. The exe is DYNAMIC_BASE: Wine keeps the preferred base, Windows ASLR
// relocates it, so every address goes through VA() (g_base_delta is set first thing in ue_init).
extern uint64_t g_base_delta;
#define VA(a)                ((a) + g_base_delta)
#define ADDR_GUOBJECTARRAY   VA(0x14667C740ull)
#define ADDR_NAMEPOOL        VA(0x146986C80ull)
#define ADDR_PROCESSEVENT    VA(0x1426C6F70ull)
#define ADDR_GAMEENGINETICK  VA(0x143C955B0ull)
#define ADDR_SETCLIENTTRAVEL VA(0x144130880ull)  // UEngine::SetClientTravel(UEngine*, UWorld*, const TCHAR*, ETravelType)
#define ADDR_LOG_GATE        VA(0x1469BD96Dull)  // GLogEnabled-style gate checked before every UE_LOG
#define OBJECTS_XOR          0x8375ull
#define VTIDX_PROCESSEVENT   66

typedef struct UObject UObject;
typedef UObject UClass;
typedef UObject UStruct;
typedef UObject UFunction;
typedef struct FField FField;

typedef struct { uint32_t idx, num; } FName;
typedef struct { wchar_t *data; int32_t num, max; } FString;
typedef struct { void *data; int32_t num, max; } TArray;

// UObject
#define U_VTBL(o)    (*(void ***)(o))
#define U_FLAGS(o)   (*(uint32_t *)((char *)(o) + 0x08))
#define U_INDEX(o)   (*(int32_t *)((char *)(o) + 0x0C))
#define U_CLASS(o)   (*(UClass **)((char *)(o) + 0x10))
#define U_NAME(o)    (*(FName *)((char *)(o) + 0x18))
#define U_OUTER(o)   (*(UObject **)((char *)(o) + 0x20))
// UField / UStruct / UClass / UFunction
#define UF_NEXT(o)        (*(UObject **)((char *)(o) + 0x30))
#define US_SUPER(o)       (*(UStruct **)((char *)(o) + 0x48))
#define US_CHILDREN(o)    (*(UObject **)((char *)(o) + 0x50))
#define US_CHILDPROPS(o)  (*(FField **)((char *)(o) + 0x58))
#define US_PROPSSIZE(o)   (*(int32_t *)((char *)(o) + 0x60))
#define UC_CDO(o)         (*(UObject **)((char *)(o) + 0x120))
#define UFN_FLAGS(o)      (*(uint32_t *)((char *)(o) + 0xB8))
#define UFN_PARMSSIZE(o)  (*(uint16_t *)((char *)(o) + 0xBE))
// FField / FProperty
#define FF_CLASS(f)   (*(void **)((char *)(f) + 0x08))
#define FF_NAME(f)    (*(FName *)((char *)(f) + 0x24))
#define FF_NEXT(f)    (*(FField **)((char *)(f) + 0x30))
#define FP_FLAGS(f)   (*(uint64_t *)((char *)(f) + 0x38))
#define FP_ELSIZE(f)  (*(int32_t *)((char *)(f) + 0x40))
#define FP_OFFSET(f)  (*(int32_t *)((char *)(f) + 0x4C))

int ue_init(char *err, size_t errlen);             // verify build; returns 0 on success
int32_t ue_num_objects(void);
UObject *ue_object_at(int32_t i);
UObject *ue_weak_get(const void *weak_ptr);        // FWeakObjectPtr -> object, NULL if stale
int ue_object_usable(UObject *o, int32_t i);       // o is still at index i and not on its way out (GC: unreachable,
                                                   // pending kill, BeginDestroy/FinishDestroy done)
const char *ue_name(FName n, char *buf, size_t len); // ASCII-folded
const char *ue_obj_name(UObject *o, char *buf, size_t len);
const char *ue_full_path(UObject *o, char *buf, size_t len);   // Outer.Chain.Name
int ue_is_a(UObject *o, UClass *cls);
UClass *ue_find_class(const char *name);            // short name, e.g. "PlayerController" (native classes only)
UClass *ue_find_any_class(const char *name);        // native or Blueprint class, e.g. "TextButton_WBP_C"
UObject *ue_find_first_of(const char *class_name);  // first non-CDO instance (IsA)
UFunction *ue_find_function(UClass *cls, const char *name);
FField *ue_find_prop(UStruct *st, const char *name);
int32_t ue_prop_offset(UObject *o, const char *name); // -1 if missing
void *ue_get_ptr(UObject *o, const char *prop);       // reads an object/pointer property
void ue_process_event(UObject *o, UFunction *fn, void *params);
UObject *ue_engine(void);
UObject *ue_world(void);
UObject *ue_local_pc(void);
int ue_is_listen_server(UObject *world);   // has a NetDriver and no ServerConnection
int ue_num_clients(UObject *world);
const char *ue_world_package(UObject *world, char *buf, size_t len);  // e.g. /Game/Maps/.../MAP_X
