/*
 * 121, Set Variable (変数操作). Layout from the public format readers (WodiLib, WolfTL, wolf-rpg-
 * formats); operators and limits from the editor's manual, help/04ev_calc.html and
 * help/04ev_value.html. docs/behaviour/commands.md has the table.
 */
#include "vm_internal.h"

#include <math.h>

#define PI 3.14159265358979323846

enum {
    F_CLAMP_SMALL = 0x01, /* results held to ±999,999 */
    F_REAL = 0x02,        /* real-number calculation */
    F_RHS1_RAW = 0x04,    /* right operand 1 is a number, never a call value */
    F_RHS2_RAW = 0x08,
    F_LHS_INDIRECT = 0x10, /* "X番の変数呼出": the operand's value is itself a call value */
    F_RHS1_INDIRECT = 0x20,
    F_RHS2_INDIRECT = 0x40,
};

enum { A_SET, A_ADD, A_SUB, A_MUL, A_DIV, A_MOD, A_RAISE, A_LOWER, A_ABS, A_ATAN, A_SIN, A_COS, A_SQRT };
enum { O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_AND, O_RANDOM, O_OR, O_XOR, O_SHIFT };

static int32_t operand(wolf_vm *vm, const wolf_thread *t, int32_t v, int raw, int indirect) {
    int32_t x = raw ? v : wolf_value(vm, t, v);
    return indirect ? wolf_get(vm, t, x) : x;
}

/* Joins the two right-hand values. Integer and real forms. */
static int64_t join_int(wolf_vm *vm, int op, int64_t a, int64_t b) {
    switch (op) {
    case O_ADD: return a + b;
    case O_SUB: return a - b;
    case O_MUL: return a * b;
    case O_DIV: return b == 0 ? a : a / b; /* by our reading, as the manual's ÷= (÷0 acts as ÷1) */
    case O_MOD: return b == 0 ? a : a % b;
    case O_AND: return (int64_t)((int32_t)a & (int32_t)b);
    case O_OR: return (int64_t)((int32_t)a | (int32_t)b);
    case O_XOR: return (int64_t)((int32_t)a ^ (int32_t)b);
    case O_SHIFT:
        if (b >= 32 || b <= -32) return 0;
        return b >= 0 ? (int64_t)(int32_t)((uint32_t)(int32_t)a << b) : (int64_t)((int32_t)a >> -b);
    case O_RANDOM: {
        int64_t lo = a < b ? a : b, hi = a < b ? b : a;
        return lo + (int64_t)wolf_random(vm, (uint32_t)(hi - lo));
    }
    default: return a + b;
    }
}

static double join_real(wolf_vm *vm, int op, double a, double b) {
    switch (op) {
    case O_ADD: return a + b;
    case O_SUB: return a - b;
    case O_MUL: return a * b;
    case O_DIV: return b == 0 ? a : a / b;
    case O_MOD: return b == 0 ? a : fmod(a, b);
    default: return (double)join_int(vm, op, (int64_t)a, (int64_t)b);
    }
}

static int64_t trunc64(double d) {
    if (d != d) return 0;
    if (d > 9e18) return INT64_MAX / 2;
    if (d < -9e18) return INT64_MIN / 2;
    return (int64_t)d;
}

/* The assignment, from the left value and the joined right value (atan takes both halves). */
static int assign_int(int how, int64_t left, int64_t right, int32_t x, int32_t y, int64_t *out) {
    switch (how) {
    case A_SET: *out = right; return 1;
    case A_ADD: *out = left + right; return 1;
    case A_SUB: *out = left - right; return 1;
    case A_MUL: *out = left * right; return 1;
    case A_DIV: *out = right == 0 ? left : left / right; return 1; /* ÷0 leaves it as it is */
    case A_MOD: *out = right == 0 ? left : left % right; return 1;
    case A_RAISE: *out = right > left ? right : left; return 1;
    case A_LOWER: *out = right < left ? right : left; return 1;
    case A_ABS: *out = right < 0 ? -right : right; return 1;
    case A_ATAN: /* the right side is an X and a Y vector (Y down); the angle in degrees x10 */
        *out = trunc64(atan2((double)y, (double)x) * 180.0 / PI * 10.0); return 1;
    case A_SIN: *out = trunc64(sin((double)right / 10.0 * PI / 180.0) * 1000.0); return 1;
    case A_COS: *out = trunc64(cos((double)right / 10.0 * PI / 180.0) * 1000.0); return 1;
    case A_SQRT: *out = right < 0 ? 0 : trunc64(sqrt((double)right) * 1000.0); return 1;
    default: return 0;
    }
}

static int assign_real(int how, double left, double right, int64_t *out) {
    double r;
    switch (how) {
    case A_SET: r = right; break;
    case A_ADD: r = left + right; break;
    case A_SUB: r = left - right; break;
    case A_MUL: r = left * right; break;
    case A_DIV: r = right == 0 ? left : left / right; break;
    case A_MOD: r = right == 0 ? left : fmod(left, right); break;
    case A_RAISE: r = right > left ? right : left; break;
    case A_LOWER: r = right < left ? right : left; break;
    case A_ABS: r = fabs(right); break;
    default: return 0;
    }
    *out = trunc64(r);
    return 1;
}

static int32_t limit(int64_t v, int clamp_small, int wrap) {
    if (clamp_small) return (int32_t)(v > 999999 ? 999999 : v < -999999 ? -999999 : v);
    if (wrap) return (int32_t)(uint32_t)(uint64_t)v;
    return (int32_t)(v > 2000000000 ? 2000000000 : v < -2000000000 ? -2000000000 : v);
}

int wolf_h_set_variable(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 4) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 121 has %d arguments; skipped", c->nint);
        return WOLF_NEXT;
    }
    int32_t lhs = c->ints[0], r1 = c->ints[1], r2 = c->ints[2];
    uint32_t flags = (uint32_t)c->ints[3];
    uint32_t f = flags & 0xFF;
    int how = (int)((flags >> 8) & 0xF);
    int op = (int)((flags >> 12) & 0xF);
    int multi = (flags >> 16) & 1;
    int32_t count = multi && c->nint >= 5 ? wolf_value(vm, t, c->ints[4]) : 1;
    if (count < 1) count = 1;
    if (count > 100000) count = 100000;

    int32_t a = operand(vm, t, r1, f & F_RHS1_RAW, f & F_RHS1_INDIRECT);
    int32_t b = operand(vm, t, r2, f & F_RHS2_RAW, f & F_RHS2_INDIRECT);
    /* Common events skip the limits when nothing is ticked and only plain variables are used:
     * values then wrap at 32 bits (manual, 計算式の説明). Elsewhere, ±2,000,000,000. */
    int in_common = t->depth && t->stack[t->depth - 1].common >= 0;
    int wrap = in_common && f == 0 && !multi &&
               wolf_is_plain(lhs) && wolf_is_plain(r1) && wolf_is_plain(r2);
    int32_t target = (f & F_LHS_INDIRECT) ? wolf_get(vm, t, lhs) : lhs;

    for (int32_t i = 0; i < count; i++) {
        int32_t ref = target + i;
        int64_t result;
        int ok;
        if ((f & F_REAL) && how <= A_ABS) {
            double right = join_real(vm, op, (double)a, (double)b);
            ok = assign_real(how, (double)wolf_get(vm, t, ref), right, &result);
        } else {
            int64_t right = how == A_ATAN ? 0 : join_int(vm, op, a, b);
            /* the runtime's operands are 32-bit; folding here also keeps left * right in range */
            if (wrap) right = (int32_t)(uint32_t)(uint64_t)right;
            else right = right > INT32_MAX ? INT32_MAX : right < INT32_MIN ? INT32_MIN : right;
            ok = assign_int(how, wolf_get(vm, t, ref), right, a, b, &result);
        }
        if (!ok) {
            wolf_once(vm, ONCE_BAD_ARGS, "a command 121 uses assignment %d, which is not known; skipped", how);
            return WOLF_NEXT;
        }
        wolf_set(vm, t, ref, limit(result, f & F_CLAMP_SMALL, wrap));
    }
    return WOLF_NEXT;
}
