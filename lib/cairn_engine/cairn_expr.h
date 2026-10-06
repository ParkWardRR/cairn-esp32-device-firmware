/*
 * The formula expression language of cairn.engine/v1-draft: the evaluator.
 *
 * DRAFT AND PROVISIONAL. contracts/engine/v1 is not released; the language is
 * specified in engines/SPEC.draft.md and should move into that contract, with the
 * vectors in engines/vectors/expr.draft.txt.
 *
 * What it is: a stack machine over int32 with no loops, no jumps, no calls, no heap
 * and no state. It cannot run anything but arithmetic on four input bytes, it always
 * terminates in at most `len` steps, and it uses a fixed 16-entry stack. Every
 * result, and every intermediate, must fit in int32 or evaluation fails; nothing
 * wraps, so every implementation of the language (C, Go, Swift, TypeScript) agrees.
 *
 * The compiler is not here. Formulas are compiled at build time by tools/enginegen,
 * which also proves from the source expression that, for any input bytes, the
 * runtime errors below cannot occur. The evaluator checks them anyway: bytecode in a
 * flash image is only as trustworthy as the flash.
 */

#ifndef CAIRN_EXPR_H
#define CAIRN_EXPR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAIRN_EXPR_MAX_STACK 16

typedef enum {
    CAIRN_EXPR_OK = 0,
    CAIRN_EXPR_ERR_TRUNC,    /* an immediate runs past the end of the code */
    CAIRN_EXPR_ERR_BAD_OP,   /* an opcode this version does not define */
    CAIRN_EXPR_ERR_STACK,    /* underflow, or deeper than CAIRN_EXPR_MAX_STACK */
    CAIRN_EXPR_ERR_DIV_ZERO, /* division or remainder by zero */
    CAIRN_EXPR_ERR_OVERFLOW, /* a value left the int32 range */
    CAIRN_EXPR_ERR_SHIFT,    /* a shift count outside 0..31 */
    CAIRN_EXPR_ERR_RESULT,   /* the code did not leave exactly one value */
    CAIRN_EXPR_ERR_ARG       /* null code (with length) or null input */
} cairn_expr_status_t;

/* Opcodes. Part of the draft contract; engines/vectors/expr.draft.txt pins them. */
enum {
    CAIRN_OP_PUSH_U8  = 0x01, /* imm8 */
    CAIRN_OP_PUSH_U16 = 0x02, /* imm16, little endian */
    CAIRN_OP_PUSH_I32 = 0x03, /* imm32, little endian, two's complement */
    CAIRN_OP_PUSH_S8  = 0x04, /* imm8, sign-extended */
    CAIRN_OP_LOAD_A   = 0x10, /* 0x10..0x13 push input byte A..D */
    CAIRN_OP_LOAD_D   = 0x13,
    CAIRN_OP_ADD      = 0x20,
    CAIRN_OP_SUB      = 0x21,
    CAIRN_OP_MUL      = 0x22,
    CAIRN_OP_DIV      = 0x23, /* truncates toward zero */
    CAIRN_OP_MOD      = 0x24, /* the sign of the dividend */
    CAIRN_OP_NEG      = 0x25,
    CAIRN_OP_AND      = 0x26,
    CAIRN_OP_OR       = 0x27,
    CAIRN_OP_SHL      = 0x28, /* a * 2^b, checked against int32 */
    CAIRN_OP_SHR      = 0x29, /* arithmetic: floors */
    CAIRN_OP_MIN      = 0x30,
    CAIRN_OP_MAX      = 0x31,
    CAIRN_OP_CLAMP    = 0x32  /* x lo hi -> min(max(x, lo), hi) */
};

/*
 * Evaluate `code` over the four data bytes `in` (A, B, C, D). On CAIRN_EXPR_OK the
 * value is in *out; on any error *out is left untouched.
 */
cairn_expr_status_t cairn_expr_eval(const uint8_t *code, size_t len,
                                    const uint8_t in[4], int32_t *out);

/* "ERR_STACK" and so on, the names the vector files use; "OK" for success. */
const char *cairn_expr_status_name(cairn_expr_status_t s);

#ifdef __cplusplus
}
#endif

#endif /* CAIRN_EXPR_H */
