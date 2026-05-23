/*
 * Myriad runtime, 32-bit profile.
 *
 * Same Polka cartridge format as the 64-bit runtime (main.c); the only
 * cart-side difference is the INT32_SAFE flag (Polka header bit 0). When set,
 * every Int constant / arithmetic result fits in i32 and every Float is exactly
 * representable as f32. 
 *
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC           0xECFF00ECu
#define VERSION         0x0201
#define FLAG_INT32_SAFE 0x0001u
#define SPEC_MAJOR      0u
#define SPEC_MINOR      1u
#define SPEC_PATCH      0u
#define REGS_PER_FRAME  64
#define TOTAL_RAM       (4u * 1024 * 1024)
#define MAX_FRAMES      512
#define MAX_REGS        (REGS_PER_FRAME * MAX_FRAMES)
#define MAX_HANDLERS    64
#define MAX_REGIONS     64
#define DISPATCH_MISS   0xFFFFu
#define HANDLE_NONE     0xFFFFFFFFu

typedef uint32_t Val;

#define BLK_FREE 0u
#define BLK_CELL 1u
#define BLK_RAW  2u

typedef struct {
    uint32_t size;
    uint32_t kind;
} BlkHdr;

typedef struct Cell {
    uint32_t rc;
    uint32_t size;
    uint32_t gen;
    uint32_t region_id;
} Cell;

typedef struct {
    uint8_t   kind;
    uint8_t   param_count;
    uint8_t   reg_count;
    uint16_t  const_count;
    uint16_t  string_count;
    uint32_t  code_count;
    uint16_t  name_len;
    uint64_t* constants;
    uint8_t*  const_mask;
    uint8_t*  code;
    char*     name;
} Function;

typedef struct {
    uint32_t  magic;
    uint16_t  version;
    uint16_t  flags;
    uint32_t  entry_fn_id;
    uint32_t  fn_count;
    Function* fns;
} Cart;

typedef struct {
    uint16_t fn_id;
    uint32_t ip;
    uint32_t base;
    uint8_t  dest;
    uint64_t mask;
} Frame;

typedef struct ContCell {
    Val      regs[REGS_PER_FRAME];
    uint64_t mask;
    uint32_t body_sp;
    uint8_t  consumed;
    uint32_t region_id;
} ContCell;

typedef struct ArmSnap {
    uint16_t fn_id;
    uint32_t ip;
    uint32_t base;
    uint8_t  dest;
    uint64_t mask;
    uint32_t sp;
    Val      regs[REGS_PER_FRAME];
    uint8_t  ra;
    struct ArmSnap* prev;
} ArmSnap;

typedef struct {
    uint16_t  effect_id;
    Val       dispatch;
    uint8_t   dispatch_is_handle;
    uint32_t  install_sp;
    uint32_t  body_sp;
    uint16_t  pending_arm_fn;
    Val       pending_arm_env;
    uint8_t   pending_arm_env_is_handle;
    ContCell* last_cont;
    ArmSnap*  arm_cont;
} Handler;

typedef struct {
    uint32_t id;
} Region;

typedef struct {
    Cart*    cart;
    uint8_t* arena;
    uint32_t arena_size;
    Val      regs[MAX_REGS];
    Frame    stack[MAX_FRAMES];
    uint32_t sp;
    Handler  handlers[MAX_HANDLERS];
    uint32_t hsp;
    Region   regions[MAX_REGIONS];
    uint32_t rsp;
    uint32_t next_region_id;
    int      halted;
    uint32_t exit_code;
} VM;

static void die(const char* msg) {
    fprintf(stderr, "myriad: %s\n", msg);
    exit(1);
}

static uint16_t rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t* p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}
static float    u_to_f(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }
static uint32_t f_to_u(float f)    { union { uint32_t u; float f; } v; v.f = f; return v.u; }

static void arena_init(VM* vm) {
    vm->arena_size &= ~7u;
    if (vm->arena_size < sizeof(BlkHdr) + 4) die("arena too small");
    BlkHdr* h = (BlkHdr*)vm->arena;
    h->size = vm->arena_size;
    h->kind = BLK_FREE;
}
static void* arena_alloc(VM* vm, uint32_t nbytes, uint32_t kind) {
    uint32_t need = (uint32_t)sizeof(BlkHdr) + ((nbytes + 3u) & ~3u);
    if (need < sizeof(BlkHdr) + 4) need = sizeof(BlkHdr) + 4;

    uint32_t off = 0;
    while (off < vm->arena_size) {
        BlkHdr* h = (BlkHdr*)(vm->arena + off);
        if (h->size == 0) die("arena corrupt");
        if (h->kind == BLK_FREE) {
            uint32_t end = off + h->size;
            while (end < vm->arena_size) {
                BlkHdr* nh = (BlkHdr*)(vm->arena + end);
                if (nh->kind != BLK_FREE) break;
                h->size += nh->size;
                end = off + h->size;
            }
            if (h->size >= need) {
                uint32_t leftover = h->size - need;
                if (leftover >= sizeof(BlkHdr) + 4) {
                    BlkHdr* nb = (BlkHdr*)(vm->arena + off + need);
                    nb->size = leftover;
                    nb->kind = BLK_FREE;
                    h->size = need;
                }
                h->kind = kind;
                void* p = (uint8_t*)h + sizeof(BlkHdr);
                memset(p, 0, h->size - sizeof(BlkHdr));
                return p;
            }
        }
        off += h->size;
    }
    return NULL;
}

static void arena_free(VM* vm, void* payload) {
    if (!payload) return;
    BlkHdr* h = (BlkHdr*)((uint8_t*)payload - sizeof(BlkHdr));
    h->kind = BLK_FREE;
    uint32_t off = (uint32_t)((uint8_t*)h - vm->arena);
    uint32_t end = off + h->size;
    if (end < vm->arena_size) {
        BlkHdr* nh = (BlkHdr*)(vm->arena + end);
        if (nh->kind == BLK_FREE) h->size += nh->size;
    }
}

static Cell* H2C(VM* vm, Val h) { return (Cell*)(vm->arena + h); }
static Val   C2H(VM* vm, Cell* c) { return (Val)((uint8_t*)c - vm->arena); }

static uint32_t cell_tagwords(uint32_t size) { return (size + 31) / 32; }
static uint32_t* cell_tags(Cell* c) { return (uint32_t*)((uint8_t*)c + sizeof(Cell)); }
static Val* cell_slots(Cell* c) { return (Val*)(cell_tags(c) + cell_tagwords(c->size)); }

static int cell_tag_get(Cell* c, uint32_t i) {
    return (int)((cell_tags(c)[i >> 5] >> (i & 31)) & 1);
}
static void cell_tag_set(Cell* c, uint32_t i, int is_handle) {
    uint32_t* t = cell_tags(c);
    if (is_handle) t[i >> 5] |= (1u << (i & 31));
    else           t[i >> 5] &= ~(1u << (i & 31));
}

static void vm_drop_handle(VM* vm, Val h);

static Cell* cell_alloc(VM* vm, uint32_t size) {
    uint32_t bytes = (uint32_t)sizeof(Cell) + (cell_tagwords(size) + size) * 4u;
    Cell* c = (Cell*)arena_alloc(vm, bytes, BLK_CELL);
    if (!c) die("oom");
    c->rc = 1;
    c->size = size;
    c->gen = 1;
    c->region_id = (vm->rsp > 0) ? vm->regions[vm->rsp - 1].id : 0;
    return c;
}

static void cell_free(VM* vm, Cell* c) {
    Val* slots = cell_slots(c);
    for (uint32_t i = 0; i < c->size; i++) {
        if (cell_tag_get(c, i)) vm_drop_handle(vm, slots[i]);
    }
    c->gen++;
    arena_free(vm, c);
}

static void vm_drop_handle(VM* vm, Val h) {
    if (h == HANDLE_NONE || h == 0) return;
    Cell* c = H2C(vm, h);
    if (c->rc == 0) return;
    if (--c->rc == 0) cell_free(vm, c);
}

static void vm_bump_handle(VM* vm, Val h) {
    if (h == HANDLE_NONE || h == 0) return;
    H2C(vm, h)->rc++;
}

static Cell* make_string_cell(VM* vm, const char* s, uint32_t len) {
    uint32_t body_slots = (len + 3) / 4;
    Cell* c = cell_alloc(vm, 1 + body_slots);
    Val* slots = cell_slots(c);
    slots[0] = (Val)len;
    cell_tag_set(c, 0, 0);
    for (uint32_t i = 0; i < body_slots; i++) {
        uint32_t w = 0;
        for (uint32_t j = 0; j < 4; j++) {
            uint32_t k = i * 4 + j;
            if (k < len) w |= ((uint32_t)(uint8_t)s[k]) << (j * 8);
        }
        slots[1 + i] = w;
        cell_tag_set(c, 1 + i, 0);
    }
    return c;
}

static uint32_t string_byte_len(Cell* c) { return cell_slots(c)[0]; }

static int string_byte_at(Cell* c, uint32_t i) {
    uint32_t len = string_byte_len(c);
    if (i >= len) return -1;
    Val w = cell_slots(c)[1 + (i / 4)];
    return (int)((w >> ((i % 4) * 8)) & 0xFF);
}

static void string_write_stream(Cell* c, FILE* f) {
    uint32_t len = string_byte_len(c);
    for (uint32_t i = 0; i < len; i++) fputc(string_byte_at(c, i), f);
}

static uint8_t* slurp(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) die("cannot open cart");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) die("ftell failed");
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) die("oom");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) die("short read");
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static Cart* load_cart(VM* vm, const char* path) {
    size_t total;
    uint8_t* buf = slurp(path, &total);
    size_t pos = 0;
    if (total < 16) die("cart too short");

    Cart* c = (Cart*)calloc(1, sizeof(Cart));
    c->magic       = rd_u32(buf + pos); pos += 4;
    c->version     = rd_u16(buf + pos); pos += 2;
    c->flags       = rd_u16(buf + pos); pos += 2;
    c->entry_fn_id = rd_u32(buf + pos); pos += 4;
    if (c->magic != MAGIC) die("bad magic");
    if (c->version != VERSION) die("unsupported version");
    if (!(c->flags & FLAG_INT32_SAFE))
        die("cart not INT32_SAFE; 32-bit runtime requires it");

    c->fn_count = rd_u32(buf + pos); pos += 4;
    if (c->entry_fn_id >= c->fn_count) die("entry_fn_id OOB");
    c->fns = (Function*)calloc(c->fn_count, sizeof(Function));

    for (uint32_t i = 0; i < c->fn_count; i++) {
        if (pos + 12 > total) die("function table truncated");
        uint8_t kind = buf[pos];
        Function* fn = &c->fns[i];
        fn->kind = kind;
        if (kind == 0x00) {
            fn->param_count  = buf[pos + 1];
            fn->reg_count    = buf[pos + 2];
            fn->const_count  = rd_u16(buf + pos + 4);
            fn->string_count = rd_u16(buf + pos + 6);
            fn->code_count   = rd_u32(buf + pos + 8);
            if (fn->reg_count > REGS_PER_FRAME) die("reg_count > 64");
        } else if (kind == 0x01) {
            fn->param_count = buf[pos + 1];
            fn->name_len    = rd_u16(buf + pos + 4);
        } else {
            die("unknown function kind");
        }
        pos += 12;
    }

    if (c->fns[c->entry_fn_id].kind != 0x00) die("entry must be bytecode");

    for (uint32_t i = 0; i < c->fn_count; i++) {
        Function* fn = &c->fns[i];
        if (fn->kind == 0x00) {
            size_t cn = (size_t)fn->const_count * 8;
            if (pos + cn > total) die("constants truncated");
            fn->constants = (uint64_t*)malloc(cn ? cn : 1);
            for (uint16_t k = 0; k < fn->const_count; k++)
                fn->constants[k] = rd_u64(buf + pos + k * 8);
            pos += cn;

            size_t mb = (fn->const_count + 7) / 8;
            if (pos + mb > total) die("const_mask truncated");
            fn->const_mask = (uint8_t*)malloc(mb ? mb : 1);
            memcpy(fn->const_mask, buf + pos, mb);
            pos += mb;

            Cell** pool = NULL;
            if (fn->string_count) {
                pool = (Cell**)calloc(fn->string_count, sizeof(Cell*));
            }
            for (uint16_t s = 0; s < fn->string_count; s++) {
                if (pos + 4 > total) die("string len truncated");
                uint32_t sl = rd_u32(buf + pos); pos += 4;
                if (pos + sl > total) die("string body truncated");
                Cell* sc = make_string_cell(vm, (const char*)(buf + pos), sl);
                pool[s] = sc;
                pos += sl;
            }

            for (uint16_t k = 0; k < fn->const_count; k++) {
                uint8_t bit = (fn->const_mask[k / 8] >> (k % 8)) & 1;
                if (bit) {
                    uint64_t idx = fn->constants[k];
                    if (idx >= fn->string_count) die("handle const points past string pool");
                    fn->constants[k] = (uint64_t)C2H(vm, pool[idx]);
                }
            }
            free(pool);

            size_t cb = (size_t)fn->code_count * 4;
            if (pos + cb > total) die("code truncated");
            fn->code = (uint8_t*)malloc(cb ? cb : 1);
            memcpy(fn->code, buf + pos, cb);
            for (size_t k = 0; k < cb; k += 4) {
                uint8_t op = fn->code[k];
                if (op > 0x2D) die("reserved opcode");
            }
            pos += cb;
        } else {
            if (pos + fn->name_len > total) die("native name truncated");
            fn->name = (char*)malloc(fn->name_len + 1);
            memcpy(fn->name, buf + pos, fn->name_len);
            fn->name[fn->name_len] = 0;
            pos += fn->name_len;
        }
    }

    free(buf);
    return c;
}

static Cell* string_from_cstr(VM* vm, const char* s) {
    return make_string_cell(vm, s, (uint32_t)strlen(s));
}

static Cell* int_to_string(VM* vm, int32_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d", v);
    return make_string_cell(vm, buf, (uint32_t)n);
}

static Cell* float_to_string(VM* vm, float f) {
    char buf[64];
    double d = (double)f;
    int n;
    if (isnan(d)) n = snprintf(buf, sizeof buf, "NaN");
    else if (isinf(d)) n = snprintf(buf, sizeof buf, d < 0 ? "-Inf" : "Inf");
    else if (d == (double)(long long)d && fabs(d) < 1e16) n = snprintf(buf, sizeof buf, "%.1f", d);
    else n = snprintf(buf, sizeof buf, "%g", d);
    return make_string_cell(vm, buf, (uint32_t)n);
}

typedef struct {
    Val     value;
    uint8_t is_handle;
} NativeRet;

static NativeRet nret_val(Val v) { NativeRet r; r.value = v; r.is_handle = 0; return r; }
static NativeRet nret_handle(VM* vm, Cell* c) { NativeRet r; r.value = C2H(vm, c); r.is_handle = 1; return r; }

static NativeRet invoke_native(VM* vm, Function* fn, Val* args, uint8_t* arg_tags) {
    const char* name = fn->name;
    if (!strcmp(name, "print"))   { string_write_stream(H2C(vm, args[0]), stdout); return nret_val(0); }
    if (!strcmp(name, "println")) { string_write_stream(H2C(vm, args[0]), stdout); fputc('\n', stdout); return nret_val(0); }
    if (!strcmp(name, "ceil"))    return nret_val(f_to_u(ceilf(u_to_f(args[0]))));
    if (!strcmp(name, "flr"))     return nret_val(f_to_u(floorf(u_to_f(args[0]))));
    if (!strcmp(name, "cos"))     return nret_val(f_to_u(cosf(u_to_f(args[0]))));
    if (!strcmp(name, "sin"))     return nret_val(f_to_u(sinf(u_to_f(args[0]))));
    if (!strcmp(name, "sqrt"))    return nret_val(f_to_u(sqrtf(u_to_f(args[0]))));
    if (!strcmp(name, "max"))     { int32_t a=(int32_t)args[0],b=(int32_t)args[1]; return nret_val((Val)(a>b?a:b)); }
    if (!strcmp(name, "min"))     { int32_t a=(int32_t)args[0],b=(int32_t)args[1]; return nret_val((Val)(a<b?a:b)); }
    if (!strcmp(name, "abs"))     { int32_t a=(int32_t)args[0]; return nret_val((Val)(a<0?-a:a)); }
    if (!strcmp(name, "__float_max")) { float a=u_to_f(args[0]),b=u_to_f(args[1]); return nret_val(f_to_u(a>b?a:b)); }
    if (!strcmp(name, "__float_min")) { float a=u_to_f(args[0]),b=u_to_f(args[1]); return nret_val(f_to_u(a<b?a:b)); }
    if (!strcmp(name, "__float_abs")) return nret_val(f_to_u(fabsf(u_to_f(args[0]))));
    if (!strcmp(name, "__int_to_f"))  return nret_val(f_to_u((float)(int32_t)args[0]));
    if (!strcmp(name, "__char_to_f")) return nret_val(f_to_u((float)(int32_t)args[0]));
    if (!strcmp(name, "__bool_to_f")) return nret_val(f_to_u(args[0] ? 1.0f : 0.0f));
    if (!strcmp(name, "__float_to_i")) return nret_val((Val)(int32_t)u_to_f(args[0]));
    if (!strcmp(name, "__char_to_i")) return nret_val(args[0]);
    if (!strcmp(name, "__bool_to_i")) return nret_val(args[0] ? 1u : 0u);
    if (!strcmp(name, "__int_to_c"))  return nret_val(args[0] & 0xFFu);
    if (!strcmp(name, "__int_to_s"))  return nret_handle(vm, int_to_string(vm, (int32_t)args[0]));
    if (!strcmp(name, "__float_to_s")) return nret_handle(vm, float_to_string(vm, u_to_f(args[0])));
    if (!strcmp(name, "__bool_to_s")) return nret_handle(vm, string_from_cstr(vm, args[0] ? "true" : "false"));
    if (!strcmp(name, "__char_to_s")) { char ch = (char)(args[0] & 0xFF); return nret_handle(vm, make_string_cell(vm, &ch, 1)); }
    if (!strcmp(name, "__string_to_s")) { vm_bump_handle(vm, args[0]); NativeRet r; r.value = args[0]; r.is_handle = 1; return r; }
    if (!strcmp(name, "__unit_to_s")) return nret_handle(vm, string_from_cstr(vm, "()"));
    if (!strcmp(name, "__concat")) {
        Cell* a = H2C(vm, args[0]);
        Cell* b = H2C(vm, args[1]);
        uint32_t la = string_byte_len(a), lb = string_byte_len(b);
        uint32_t total = la + lb;
        char* tmp = (char*)malloc(total ? total : 1);
        for (uint32_t i = 0; i < la; i++) tmp[i] = (char)string_byte_at(a, i);
        for (uint32_t i = 0; i < lb; i++) tmp[la + i] = (char)string_byte_at(b, i);
        Cell* r = make_string_cell(vm, tmp, total);
        free(tmp);
        return nret_handle(vm, r);
    }
    if (!strcmp(name, "__to_str")) {
        if (arg_tags && arg_tags[0]) { vm_bump_handle(vm, args[0]); NativeRet r; r.value = args[0]; r.is_handle = 1; return r; }
        return nret_handle(vm, int_to_string(vm, (int32_t)args[0]));
    }
    if (!strcmp(name, "__str_len")) return nret_val((Val)(int32_t)string_byte_len(H2C(vm, args[0])));
    if (!strcmp(name, "__str_byte_at")) {
        Cell* s = H2C(vm, args[0]);
        int32_t i = (int32_t)args[1];
        if (i < 0) return nret_val((Val)-1);
        int b = string_byte_at(s, (uint32_t)i);
        return nret_val((Val)(int32_t)b);
    }
    if (!strcmp(name, "__str_eq")) {
        Cell* a = H2C(vm, args[0]);
        Cell* b = H2C(vm, args[1]);
        uint32_t la = string_byte_len(a), lb = string_byte_len(b);
        if (la != lb) return nret_val(0);
        for (uint32_t i = 0; i < la; i++) if (string_byte_at(a, i) != string_byte_at(b, i)) return nret_val(0);
        return nret_val(1);
    }
    if (!strcmp(name, "halt"))  { vm->halted = 1; vm->exit_code = (uint32_t)args[0]; return nret_val(0); }
    if (!strcmp(name, "abort")) { vm->halted = 1; vm->exit_code = 1; return nret_val(0); }
    fprintf(stderr, "myriad: unresolved import: %s\n", name);
    exit(1);
}

static int native_known(const char* name) {
    static const char* names[] = {
        "print","println","ceil","flr","cos","sin","sqrt","max","min","abs",
        "__float_max","__float_min","__float_abs",
        "__int_to_f","__char_to_f","__bool_to_f","__float_to_i","__char_to_i","__bool_to_i",
        "__int_to_c","__int_to_s","__float_to_s","__bool_to_s","__char_to_s","__string_to_s","__unit_to_s",
        "__concat","__to_str","__str_len","__str_byte_at","__str_eq",
        "halt","abort", NULL
    };
    for (int i = 0; names[i]; i++) if (!strcmp(name, names[i])) return 1;
    return 0;
}

static void validate_natives(Cart* c) {
    for (uint32_t i = 0; i < c->fn_count; i++) {
        Function* fn = &c->fns[i];
        if (fn->kind == 0x01 && !native_known(fn->name)) {
            fprintf(stderr, "myriad: unresolved import: %s\n", fn->name);
            exit(1);
        }
    }
}

static Val dei(VM* vm, uint16_t port, uint8_t* out_handle_tag) {
    *out_handle_tag = 0;
    uint8_t dev = (uint8_t)(port >> 8), p = (uint8_t)(port & 0xFF);
    if (dev == 0x00) {
        /* Spec version split across 32-bit-readable fields (Runtime §6.1);
         * the old 64-bit packing was unreadable on 32-bit carts. */
        if (p == 0x00) return SPEC_MAJOR;
        if (p == 0x03) return (Val)vm->cart->flags;
        if (p == 0x04) return SPEC_MINOR;
        if (p == 0x05) return SPEC_PATCH;
        die("DEI: bad System port");
    }
    if (dev == 0x10) {
        if (p == 0x00) { int ch = getchar(); return ch == EOF ? (Val)-1 : (Val)(ch & 0xFF); }
        die("DEI: bad Console port");
    }
    die("DEI: absent device");
    return 0;
}

static void free_arm_chain(VM* vm, ArmSnap* a) {
    while (a) {
        ArmSnap* p = a->prev;
        for (int i = 0; i < REGS_PER_FRAME; i++) {
            if (a->mask & (1ULL << i)) vm_drop_handle(vm, a->regs[i]);
        }
        arena_free(vm, a);
        a = p;
    }
}

static void free_handler(VM* vm, Handler* h) {
    if (h->last_cont) {
        if (!h->last_cont->consumed) {
            for (int i = 0; i < REGS_PER_FRAME; i++) {
                if (h->last_cont->mask & (1ULL << i)) vm_drop_handle(vm, h->last_cont->regs[i]);
            }
        }
        arena_free(vm, h->last_cont);
        h->last_cont = NULL;
    }
    free_arm_chain(vm, h->arm_cont);
    h->arm_cont = NULL;
    if (h->dispatch_is_handle) vm_drop_handle(vm, h->dispatch);
    if (h->pending_arm_env_is_handle) vm_drop_handle(vm, h->pending_arm_env);
    h->dispatch_is_handle = 0;
    h->pending_arm_env_is_handle = 0;
}

static void region_push(VM* vm) {
    if (vm->rsp >= MAX_REGIONS) die("region stack overflow");
    Region* r = &vm->regions[vm->rsp++];
    r->id = ++vm->next_region_id;
}
static void region_pop(VM* vm) {
    if (vm->rsp == 0) die("region pop with empty stack");
    Region* r = &vm->regions[--vm->rsp];
    uint32_t id = r->id;
    uint32_t off = 0;
    while (off < vm->arena_size) {
        BlkHdr* h = (BlkHdr*)(vm->arena + off);
        if (h->size == 0) die("arena corrupt");
        if (h->kind == BLK_CELL) {
            Cell* c = (Cell*)(vm->arena + off + sizeof(BlkHdr));
            if (c->region_id == id) {
                c->region_id = 0;
                c->rc = 0;
                c->gen++;
                h->kind = BLK_FREE;
            }
        }
        off += h->size;
    }
}

static void region_detach_walk(VM* vm, Val h, Val* visited, uint32_t* vn, uint32_t vcap) {
    if (h == HANDLE_NONE || h == 0) return;
    Cell* c = H2C(vm, h);
    for (uint32_t i = 0; i < *vn; i++) if (visited[i] == h) return;
    if (*vn < vcap) visited[(*vn)++] = h;
    /* top-only (Runtime §5): detach + recurse only while the cell is attached
     * to the current top region; a cell in another region, no region, or
     * already forgotten stops this branch. */
    uint32_t top = (vm->rsp > 0) ? vm->regions[vm->rsp - 1].id : 0;
    if (top == 0 || c->region_id != top) return;
    c->region_id = 0;
    Val* slots = cell_slots(c);
    for (uint32_t i = 0; i < c->size; i++) {
        if (cell_tag_get(c, i)) region_detach_walk(vm, slots[i], visited, vn, vcap);
    }
}

static void region_deep_forget(VM* vm, Val h) {
    Val visited[4096];
    uint32_t vn = 0;
    region_detach_walk(vm, h, visited, &vn, 4096);
}

static void deo(VM* vm, uint16_t port, Val v, uint8_t v_is_handle) {
    uint8_t dev = (uint8_t)(port >> 8), p = (uint8_t)(port & 0xFF);
    if (dev == 0x00) {
        if (p == 0x01) { vm->halted = 1; vm->exit_code = (uint32_t)v; return; }
        if (p == 0x02) {
            if (v != 0 && v != HANDLE_NONE) string_write_stream(H2C(vm, v), stderr);
            fputc('\n', stderr);
            vm->halted = 1;
            vm->exit_code = 1;
            return;
        }
        die("DEO: bad System port");
    }
    if (dev == 0x10) {
        if (p == 0x01) { fputc((int)(v & 0xFF), stdout); return; }
        if (p == 0x02) { fputc((int)(v & 0xFF), stderr); return; }
        if (p == 0x03) { fflush(stdout); fflush(stderr); return; }
        die("DEO: bad Console port");
    }
    if (dev == 0xE0) {
        if (p == 0x01) {
            if (vm->hsp == 0) die("DEO 0xE001 with no handler");
            Handler* h = &vm->handlers[vm->hsp - 1];
            free_handler(vm, h);
            vm->hsp--;
            return;
        }
        if (p == 0x03) {
            if (vm->hsp == 0) die("DEO 0xE003 with no handler");
            vm->handlers[vm->hsp - 1].pending_arm_fn = (uint16_t)v;
            return;
        }
        if (p == 0x04) {
            if (vm->hsp == 0) die("DEO 0xE004 with no handler");
            Handler* h = &vm->handlers[vm->hsp - 1];
            if (h->pending_arm_env_is_handle) vm_drop_handle(vm, h->pending_arm_env);
            h->pending_arm_env = v;
            h->pending_arm_env_is_handle = v_is_handle;
            if (v_is_handle) vm_bump_handle(vm, v);
            return;
        }
        die("DEO: bad Dispatch port");
    }
    if (dev == 0xE1) {
        if (p == 0x00) { region_push(vm); return; }
        if (p == 0x01) { region_pop(vm); return; }
        if (p == 0x02) { region_deep_forget(vm, v); return; }
        die("DEO: bad Region port");
    }
    die("DEO: absent device");
}

static void dispatch_lookup(VM* vm, uint16_t effect_id, uint8_t op_id, Handler** out_h, uint16_t* out_fn, Val* out_env, uint8_t* out_env_is_handle) {
    *out_h = NULL;
    *out_fn = DISPATCH_MISS;
    *out_env = 0;
    *out_env_is_handle = 0;
    for (int32_t i = (int32_t)vm->hsp - 1; i >= 0; i--) {
        Handler* h = &vm->handlers[i];
        if (h->effect_id != effect_id) continue;
        if (h->dispatch == HANDLE_NONE || h->dispatch == 0) continue;
        Cell* dt = H2C(vm, h->dispatch);
        uint32_t idx_fn = (uint32_t)op_id * 2;
        uint32_t idx_env = idx_fn + 1;
        if (idx_env >= dt->size) continue;
        Val* slots = cell_slots(dt);
        uint32_t f = slots[idx_fn] & 0xFFFFu;
        if (f == DISPATCH_MISS) continue;
        *out_h = h;
        *out_fn = (uint16_t)f;
        *out_env = slots[idx_env];
        *out_env_is_handle = cell_tag_get(dt, idx_env) ? 1 : 0;
        return;
    }
}

static Handler* active_handler(VM* vm) {
    for (int32_t i = (int32_t)vm->hsp - 1; i >= 0; i--) {
        Handler* h = &vm->handlers[i];
        if (h->last_cont && !h->last_cont->consumed) return h;
    }
    return NULL;
}

static void write_reg(VM* vm, Frame* fr, uint8_t r, Val v, uint8_t is_handle) {
    if (fr->mask & (1ULL << r)) vm_drop_handle(vm, vm->regs[fr->base + r]);
    vm->regs[fr->base + r] = v;
    if (is_handle) fr->mask |= (1ULL << r);
    else           fr->mask &= ~(1ULL << r);
}

static void do_call(VM* vm, Frame* caller, Function* caller_fn, uint16_t target_fn_id, uint8_t dest_a) {
    if (target_fn_id >= vm->cart->fn_count) die("call: fn_id OOB");
    Function* tgt = &vm->cart->fns[target_fn_id];
    uint32_t new_base = caller->base + caller_fn->reg_count;

    if (tgt->kind == 0x01) {
        Val     args_copy[REGS_PER_FRAME];
        uint8_t arg_tags[REGS_PER_FRAME];
        for (uint8_t i = 0; i < tgt->param_count; i++) {
            args_copy[i] = vm->regs[new_base + i];
            arg_tags[i] = (caller->mask >> (caller_fn->reg_count + i)) & 1;
        }
        for (uint8_t i = 0; i < tgt->param_count; i++) {
            uint8_t bit_pos = caller_fn->reg_count + i;
            caller->mask &= ~(1ULL << bit_pos);
            vm->regs[new_base + i] = 0;
        }
        NativeRet nr = invoke_native(vm, tgt, args_copy, arg_tags);
        for (uint8_t i = 0; i < tgt->param_count; i++) {
            if (arg_tags[i]) vm_drop_handle(vm, args_copy[i]);
        }
        write_reg(vm, caller, dest_a, nr.value, nr.is_handle);
        return;
    }

    if (vm->sp + 1 >= MAX_FRAMES) die("call stack overflow");
    if (new_base + REGS_PER_FRAME > MAX_REGS) die("register space exhausted");
    vm->sp++;
    Frame* nf = &vm->stack[vm->sp];
    nf->fn_id = target_fn_id;
    nf->ip    = 0;
    nf->base  = new_base;
    nf->dest  = dest_a;
    nf->mask  = 0;
    for (uint8_t i = 0; i < tgt->param_count; i++) {
        uint8_t bit_pos = caller_fn->reg_count + i;
        if ((caller->mask >> bit_pos) & 1) {
            nf->mask |= (1ULL << i);
            caller->mask &= ~(1ULL << bit_pos);
        }
    }
    for (int32_t i = (int32_t)vm->hsp - 1; i >= 0; i--) {
        Handler* h = &vm->handlers[i];
        if (h->install_sp == vm->sp - 1 && h->body_sp == 0) {
            h->body_sp = vm->sp;
        }
    }
}

static void do_ret(VM* vm, Val v, uint8_t v_is_handle) {
    Frame* fr = &vm->stack[vm->sp];

    for (uint32_t i = vm->hsp; i-- > 0;) {
        Handler* h = &vm->handlers[i];
        if (h->body_sp == vm->sp && h->pending_arm_fn != DISPATCH_MISS) {
            uint16_t arm_fn = h->pending_arm_fn;
            Val env = h->pending_arm_env;
            uint8_t env_h = h->pending_arm_env_is_handle;
            h->pending_arm_fn = DISPATCH_MISS;
            h->pending_arm_env = 0;
            h->pending_arm_env_is_handle = 0;
            if (arm_fn >= vm->cart->fn_count) die("return arm fn_id OOB");
            Function* af = &vm->cart->fns[arm_fn];
            if (af->kind != 0x00) die("return arm must be bytecode");
            fr->fn_id = arm_fn;
            fr->ip = 0;
            fr->mask = 0;
            vm->regs[fr->base + 0] = env;
            if (env_h) fr->mask |= 1ULL << 0;
            vm->regs[fr->base + 1] = 0;
            vm->regs[fr->base + 2] = v;
            if (v_is_handle) fr->mask |= 1ULL << 2;
            return;
        }
    }

    for (uint32_t i = vm->hsp; i-- > 0;) {
        Handler* h = &vm->handlers[i];
        if (h->body_sp == vm->sp && h->arm_cont) {
            ArmSnap* as = h->arm_cont;
            h->arm_cont = as->prev;
            fr->fn_id = as->fn_id;
            fr->ip    = as->ip;
            fr->base  = as->base;
            fr->mask  = as->mask;
            memcpy(&vm->regs[as->base], as->regs, sizeof(Val) * REGS_PER_FRAME);
            vm->regs[as->base + as->ra] = v;
            if (v_is_handle) fr->mask |= (1ULL << as->ra);
            else             fr->mask &= ~(1ULL << as->ra);
            arena_free(vm, as);
            return;
        }
    }

    if (vm->sp == 0) {
        vm->halted = 1;
        vm->exit_code = (uint32_t)v;
        return;
    }
    Frame* caller = &vm->stack[vm->sp - 1];
    if (caller->mask & (1ULL << fr->dest)) vm_drop_handle(vm, vm->regs[caller->base + fr->dest]);
    vm->regs[caller->base + fr->dest] = v;
    if (v_is_handle) caller->mask |= (1ULL << fr->dest);
    else             caller->mask &= ~(1ULL << fr->dest);
    vm->sp--;
}

static void do_raise(VM* vm, uint8_t dest_a, uint8_t key_reg, uint8_t args_base) {
    Frame* fr = &vm->stack[vm->sp];
    Val key = vm->regs[fr->base + key_reg];
    uint16_t effect_id = (uint16_t)(key >> 8);
    uint8_t  op_id     = (uint8_t)(key & 0xFF);

    Handler* h;
    uint16_t arm_fn;
    Val      env;
    uint8_t  env_is_handle;
    dispatch_lookup(vm, effect_id, op_id, &h, &arm_fn, &env, &env_is_handle);
    if (!h) die("raise: no matching handler");
    if (arm_fn >= vm->cart->fn_count) die("raise: arm fn_id OOB");
    Function* af = &vm->cart->fns[arm_fn];
    if (af->kind != 0x00) die("raise: arm not bytecode");
    if (af->param_count < 2) die("raise: arm param_count < 2");
    uint8_t nargs = af->param_count - 2;

    if (h->last_cont) { arena_free(vm, h->last_cont); h->last_cont = NULL; }
    ContCell* cc = (ContCell*)arena_alloc(vm, sizeof(ContCell), BLK_RAW);
    if (!cc) die("oom");
    cc->mask     = fr->mask;
    cc->body_sp  = vm->sp;
    cc->consumed = 0;
    cc->region_id = vm->rsp ? vm->regions[vm->rsp - 1].id : 0;
    memcpy(cc->regs, &vm->regs[fr->base], sizeof(Val) * REGS_PER_FRAME);
    for (int i = 0; i < REGS_PER_FRAME; i++) {
        if (cc->mask & (1ULL << i)) vm_bump_handle(vm, cc->regs[i]);
    }
    h->last_cont = cc;

    if (vm->sp + 1 >= MAX_FRAMES) die("call stack overflow");
    Function* cur_fn = &vm->cart->fns[fr->fn_id];
    uint32_t arm_base = fr->base + cur_fn->reg_count;
    if (arm_base + REGS_PER_FRAME > MAX_REGS) die("register space exhausted");
    vm->sp++;
    Frame* af_fr = &vm->stack[vm->sp];
    af_fr->fn_id = arm_fn;
    af_fr->ip    = 0;
    af_fr->base  = arm_base;
    af_fr->dest  = dest_a;
    af_fr->mask  = 0;

    vm->regs[arm_base + 0] = env;
    if (env_is_handle) { af_fr->mask |= 1ULL << 0; vm_bump_handle(vm, env); }
    vm->regs[arm_base + 1] = 0;
    for (uint8_t i = 0; i < nargs; i++) {
        uint8_t src = args_base + i;
        vm->regs[arm_base + 2 + i] = vm->regs[fr->base + src];
        if (fr->mask & (1ULL << src)) {
            af_fr->mask |= (1ULL << (2 + i));
            fr->mask &= ~(1ULL << src);
        }
    }
}

static void do_resume(VM* vm, uint8_t ra_idx, Val value, uint8_t value_is_handle) {
    Handler* h = active_handler(vm);
    if (!h) die("resume: no continuation");
    ContCell* cc = h->last_cont;
    if (!cc || cc->consumed) die("resume: continuation already consumed");
    if (vm->sp == 0) die("resume: arm frame missing");

    Frame* arm_fr = &vm->stack[vm->sp];
    ArmSnap* as = (ArmSnap*)arena_alloc(vm, sizeof(ArmSnap), BLK_RAW);
    if (!as) die("oom");
    as->fn_id = arm_fr->fn_id;
    as->ip    = arm_fr->ip;
    as->base  = arm_fr->base;
    as->dest  = arm_fr->dest;
    as->mask  = arm_fr->mask;
    as->sp    = vm->sp;
    memcpy(as->regs, &vm->regs[arm_fr->base], sizeof(Val) * REGS_PER_FRAME);
    as->ra    = ra_idx;
    as->prev  = h->arm_cont;
    h->arm_cont = as;

    uint32_t body_sp = cc->body_sp;
    Frame* body_fr = &vm->stack[body_sp];
    memcpy(&vm->regs[body_fr->base], cc->regs, sizeof(Val) * REGS_PER_FRAME);
    body_fr->mask = cc->mask;
    vm->regs[body_fr->base + arm_fr->dest] = value;
    if (value_is_handle) body_fr->mask |= (1ULL << arm_fr->dest);
    else                 body_fr->mask &= ~(1ULL << arm_fr->dest);

    cc->consumed = 1;
    vm->sp = body_sp;

    while (vm->hsp > 0 && vm->handlers[vm->hsp - 1].install_sp > body_sp) {
        Handler* hh = &vm->handlers[vm->hsp - 1];
        free_handler(vm, hh);
        vm->hsp--;
    }
}

static void run(VM* vm) {
    while (!vm->halted) {
        Frame* fr = &vm->stack[vm->sp];
        Function* fn = &vm->cart->fns[fr->fn_id];
        if (fn->kind != 0x00) die("executing non-bytecode frame");
        if (fr->ip >= fn->code_count) die("ip past end of code");
        const uint8_t* ins = fn->code + (size_t)fr->ip * 4;
        uint8_t op = ins[0], a = ins[1], b = ins[2], c = ins[3];
        uint32_t ra = fr->base + a, rb = fr->base + b, rc = fr->base + c;
        Val* R = vm->regs;
        fr->ip++;

        switch (op) {
            case 0x00: R[ra] = R[rb] + R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x01: R[ra] = R[rb] - R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x02: R[ra] = R[rb] * R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x03: if (!R[rc]) die("div by zero"); R[ra] = (Val)((int32_t)R[rb] / (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x04: if (!R[rc]) die("mod by zero"); R[ra] = (Val)((int32_t)R[rb] % (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x05: R[ra] = (Val)(-(int32_t)R[rb]); fr->mask &= ~(1ULL << a); break;
            case 0x06: R[ra] = (R[rb] == R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x07: R[ra] = (R[rb] != R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x08: R[ra] = ((int32_t)R[rb] <  (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x09: R[ra] = ((int32_t)R[rb] >  (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x0A: R[ra] = ((int32_t)R[rb] <= (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x0B: R[ra] = ((int32_t)R[rb] >= (int32_t)R[rc]); fr->mask &= ~(1ULL << a); break;
            case 0x0C: R[ra] = R[rb] & R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x0D: R[ra] = R[rb] | R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x0E: R[ra] = R[rb] ^ R[rc]; fr->mask &= ~(1ULL << a); break;
            case 0x0F: R[ra] = R[rb] << (R[rc] & 31); fr->mask &= ~(1ULL << a); break;
            case 0x10: R[ra] = (Val)((int32_t)R[rb] >> (R[rc] & 31)); fr->mask &= ~(1ULL << a); break;
            case 0x11: { int16_t off = (int16_t)(b | (c << 8)); fr->ip = (uint32_t)((int32_t)fr->ip + off); break; }
            case 0x12: { int16_t off = (int16_t)(b | (c << 8)); if (R[ra] == 0) fr->ip = (uint32_t)((int32_t)fr->ip + off); break; }
            case 0x13: { int16_t off = (int16_t)(b | (c << 8)); if (R[ra] != 0) fr->ip = (uint32_t)((int32_t)fr->ip + off); break; }
            case 0x14: do_call(vm, fr, fn, (uint16_t)(b | (c << 8)), a); break;
            case 0x16: do_call(vm, fr, fn, (uint16_t)R[rb], a); break;
            case 0x15: {
                uint8_t v_h = (fr->mask >> a) & 1;
                Val v = R[ra];
                if (v_h) fr->mask &= ~(1ULL << a);
                do_ret(vm, v, v_h);
                break;
            }
            case 0x17: {
                uint16_t idx = (uint16_t)(b | (c << 8));
                if (idx >= fn->const_count) die("pushconst idx OOB");
                uint8_t bit = (fn->const_mask[idx / 8] >> (idx % 8)) & 1;
                Val v = (Val)fn->constants[idx];
                if (bit) vm_bump_handle(vm, v);
                write_reg(vm, fr, a, v, bit);
                break;
            }
            case 0x18: {
                uint8_t src_h = (fr->mask >> b) & 1;
                if (src_h) vm_bump_handle(vm, R[rb]);
                write_reg(vm, fr, a, R[rb], src_h);
                break;
            }
            case 0x19: {
                uint8_t src_h = (fr->mask >> b) & 1;
                Val v = R[rb];
                R[rb] = 0;
                fr->mask &= ~(1ULL << b);
                write_reg(vm, fr, a, v, src_h);
                break;
            }
            case 0x1A: {
                if (R[rb] == HANDLE_NONE || R[rb] == 0) die("ld: null handle");
                Cell* p = H2C(vm, R[rb]);
                if (c >= p->size) die("ld OOB");
                Val* slots = cell_slots(p);
                Val v = slots[c];
                int slot_h = cell_tag_get(p, c);
                if (slot_h) vm_bump_handle(vm, v);
                write_reg(vm, fr, a, v, (uint8_t)slot_h);
                break;
            }
            case 0x1B: {
                if (R[rb] == HANDLE_NONE || R[rb] == 0) die("st: null handle");
                Cell* p = H2C(vm, R[rb]);
                if (c >= p->size) die("st OOB");
                Val* slots = cell_slots(p);
                int prev_h = cell_tag_get(p, c);
                if (prev_h) vm_drop_handle(vm, slots[c]);
                uint8_t src_h = (fr->mask >> a) & 1;
                if (src_h) vm_bump_handle(vm, R[ra]);
                slots[c] = R[ra];
                cell_tag_set(p, c, src_h);
                break;
            }
            case 0x1C: {
                if (R[rb] == HANDLE_NONE || R[rb] == 0) die("ldidx: null handle");
                Cell* p = H2C(vm, R[rb]);
                Val i = R[rc];
                if (i >= p->size) die("ldidx OOB");
                Val* slots = cell_slots(p);
                Val v = slots[i];
                int slot_h = cell_tag_get(p, i);
                if (slot_h) vm_bump_handle(vm, v);
                write_reg(vm, fr, a, v, (uint8_t)slot_h);
                break;
            }
            case 0x1D: {
                if (R[rb] == HANDLE_NONE || R[rb] == 0) die("stidx: null handle");
                Cell* p = H2C(vm, R[rb]);
                Val i = R[rc];
                if (i >= p->size) die("stidx OOB");
                Val* slots = cell_slots(p);
                int prev_h = cell_tag_get(p, i);
                if (prev_h) vm_drop_handle(vm, slots[i]);
                uint8_t src_h = (fr->mask >> a) & 1;
                if (src_h) vm_bump_handle(vm, R[ra]);
                slots[i] = R[ra];
                cell_tag_set(p, i, src_h);
                break;
            }
            case 0x1E: {
                uint16_t sz = (uint16_t)(b | (c << 8));
                Cell* p = cell_alloc(vm, sz);
                write_reg(vm, fr, a, C2H(vm, p), 1);
                break;
            }
            case 0x1F: {
                if (fr->mask & (1ULL << a)) vm_drop_handle(vm, R[ra]);
                R[ra] = 0;
                fr->mask &= ~(1ULL << a);
                break;
            }
            case 0x20: {
                uint8_t h_tag;
                R[ra] = dei(vm, (uint16_t)R[rb], &h_tag);
                fr->mask &= ~(1ULL << a);
                if (h_tag) fr->mask |= (1ULL << a);
                break;
            }
            case 0x21: {
                uint8_t src_h = (fr->mask >> a) & 1;
                deo(vm, (uint16_t)R[rb], R[ra], src_h);
                break;
            }
            case 0x22: {
                if (vm->hsp >= MAX_HANDLERS) die("handler stack overflow");
                Handler* h = &vm->handlers[vm->hsp++];
                memset(h, 0, sizeof(Handler));
                h->effect_id          = (uint16_t)(b | (c << 8));
                h->dispatch           = R[ra];
                h->dispatch_is_handle = (fr->mask >> a) & 1;
                if (h->dispatch_is_handle) vm_bump_handle(vm, h->dispatch);
                h->install_sp         = vm->sp;
                h->body_sp            = 0;
                h->pending_arm_fn     = DISPATCH_MISS;
                h->pending_arm_env    = 0;
                h->pending_arm_env_is_handle = 0;
                h->last_cont          = NULL;
                h->arm_cont           = NULL;
                break;
            }
            case 0x23: {
                uint8_t src_h = (fr->mask >> b) & 1;
                Val v = R[rb];
                if (src_h) {
                    fr->mask &= ~(1ULL << b);
                    R[rb] = 0;
                }
                do_resume(vm, a, v, src_h);
                break;
            }
            case 0x24: R[ra] = R[rb] + (Val)(int32_t)(int8_t)c; fr->mask &= ~(1ULL << a); break;
            case 0x25: R[ra] = R[rb] - (Val)(int32_t)(int8_t)c; fr->mask &= ~(1ULL << a); break;
            case 0x26: R[ra] = f_to_u(u_to_f(R[rb]) + u_to_f(R[rc])); fr->mask &= ~(1ULL << a); break;
            case 0x27: R[ra] = f_to_u(u_to_f(R[rb]) - u_to_f(R[rc])); fr->mask &= ~(1ULL << a); break;
            case 0x28: R[ra] = f_to_u(u_to_f(R[rb]) * u_to_f(R[rc])); fr->mask &= ~(1ULL << a); break;
            case 0x29: R[ra] = f_to_u(u_to_f(R[rb]) / u_to_f(R[rc])); fr->mask &= ~(1ULL << a); break;
            case 0x2A: R[ra] = f_to_u(-u_to_f(R[rb])); fr->mask &= ~(1ULL << a); break;
            case 0x2B: { float x = u_to_f(R[rb]), y = u_to_f(R[rc]); R[ra] = (x == x && y == y && x < y) ? 1 : 0; fr->mask &= ~(1ULL << a); break; }
            case 0x2C: { float x = u_to_f(R[rb]), y = u_to_f(R[rc]); R[ra] = (x == x && y == y && x == y) ? 1 : 0; fr->mask &= ~(1ULL << a); break; }
            case 0x2D: do_raise(vm, a, b, c); break;
            default: die("reserved opcode");
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <cart.pk>\n", argv[0]); return 2; }
    VM* vm = (VM*)calloc(1, sizeof(VM));
    if (!vm) die("oom");

    if (sizeof(VM) + (sizeof(BlkHdr) + 4) >= TOTAL_RAM) die("VM overhead exceeds RAM budget");
    vm->arena_size = TOTAL_RAM - (uint32_t)sizeof(VM);
    vm->arena = (uint8_t*)malloc(vm->arena_size);
    if (!vm->arena) die("oom");
    arena_init(vm);

    vm->cart = load_cart(vm, argv[1]);
    validate_natives(vm->cart);

    vm->stack[0].fn_id = (uint16_t)vm->cart->entry_fn_id;
    vm->stack[0].ip    = 0;
    vm->stack[0].base  = 0;
    vm->stack[0].dest  = 0;
    vm->stack[0].mask  = 0;
    vm->sp = 0;

    run(vm);
    fflush(stdout);
    fflush(stderr);
    int code = (int)vm->exit_code;
    free(vm->arena);
    free(vm);
    return code;
}
