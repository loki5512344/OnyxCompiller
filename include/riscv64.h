/*
 * riscv64.h — RISC-V (RV64IMA) instruction encoders.
 *
 * Subset sufficient for C/C++ codegen:
 *   - I-type: addi, slti, sltiu, xori, ori, andi, slli, srli, srai, lb,
 *             lbu, lh, lhu, lw, lwu, ld, jalr
 *   - R-type: add, sub, sll, slt, sltu, xor, srl, sra, or, and,
 *             mul, mulh, mulhu, div, divu, rem, remu
 *   - S-type: sb, sh, sw, sd
 *   - B-type: beq, bne, blt, bge, bltu, bgeu
 *   - U-type: lui, auipc
 *   - J-type: jal
 *   - FENCE, ECALL, EBREAK, NOP (addi x0,x0,0)
 *
 * Branches and jumps use labels; the codegen emits fixups that are
 * resolved at function end.
 */
#ifndef CC_RISCV64_H
#define CC_RISCV64_H

#include "cc.h"

/* Register names — keep ABI names for clarity. */
#define RV_ZERO  0
#define RV_RA    1
#define RV_SP    2
#define RV_GP    3
#define RV_TP    4
#define RV_T0    5
#define RV_T1    6
#define RV_T2    7
#define RV_S0    8
#define RV_S1    9
#define RV_A0   10
#define RV_A1   11
#define RV_A2   12
#define RV_A3   13
#define RV_A4   14
#define RV_A5   15
#define RV_A6   16
#define RV_A7   17
#define RV_S2   18
#define RV_S3   19
#define RV_S4   20
#define RV_S5   21
#define RV_S6   22
#define RV_S7   23
#define RV_S8   24
#define RV_S9   25
#define RV_S10  26
#define RV_S11  27
#define RV_T3   28
#define RV_T4   29
#define RV_T5   30
#define RV_T6   31

#define RV_FP   RV_S0   /* frame pointer alias */

/* Fixup records for forward branches/jumps. */
typedef enum {
    FX_BRANCH,   /* 13-bit immediate, B-type */
    FX_JUMP,     /* 21-bit immediate, J-type */
} fixup_kind_t;

typedef struct fixup {
    fixup_kind_t kind;
    uint32_t patch_off;   /* offset in g_text where the instruction is */
    uint32_t target_off;  /* offset of the label (target) */
    struct fixup *next;
} fixup_t;

/* Emit instructions into g_text. */
void rv_nop(void);
void rv_addi(int rd, int rs1, int imm);   /* imm in [-2048, 2047] */
void rv_addi_imm(int rd, int rs1, int64_t imm);  /* large imm handled via lui+addi */
void rv_add(int rd, int rs1, int rs2);
void rv_sub(int rd, int rs1, int rs2);
void rv_mul(int rd, int rs1, int rs2);
void rv_div(int rd, int rs1, int rs2);
void rv_divu(int rd, int rs1, int rs2);
void rv_rem(int rd, int rs1, int rs2);
void rv_remu(int rd, int rs1, int rs2);
void rv_and(int rd, int rs1, int rs2);
void rv_or(int rd, int rs1, int rs2);
void rv_xor(int rd, int rs1, int rs2);
void rv_sll(int rd, int rs1, int rs2);
void rv_srl(int rd, int rs1, int rs2);
void rv_sra(int rd, int rs1, int rs2);
void rv_slt(int rd, int rs1, int rs2);
void rv_sltu(int rd, int rs1, int rs2);
void rv_slti(int rd, int rs1, int imm);
void rv_sltiu(int rd, int rs1, int imm);

void rv_lb(int rd, int rs1, int imm);
void rv_lbu(int rd, int rs1, int imm);
void rv_lh(int rd, int rs1, int imm);
void rv_lhu(int rd, int rs1, int imm);
void rv_lw(int rd, int rs1, int imm);
void rv_lwu(int rd, int rs1, int imm);
void rv_ld(int rd, int rs1, int imm);
void rv_sb(int rs2, int rs1, int imm);
void rv_sh(int rs2, int rs1, int imm);
void rv_sw(int rs2, int rs1, int imm);
void rv_sd(int rs2, int rs1, int imm);

void rv_lui(int rd, int imm20);            /* imm20 in [0, 0xfffff] */
void rv_auipc(int rd, int imm20);
void rv_jal(int rd, int32_t off21);
void rv_jalr(int rd, int rs1, int imm);

void rv_beq(int rs1, int rs2, int32_t off13);
void rv_bne(int rs1, int rs2, int32_t off13);
void rv_blt(int rs1, int rs2, int32_t off13);
void rv_bge(int rs1, int rs2, int32_t off13);
void rv_bltu(int rs1, int rs2, int32_t off13);
void rv_bgeu(int rs1, int rs2, int32_t off13);

void rv_ecall(void);
void rv_ebreak(void);
void rv_fence(void);
void rv_ret(void);              /* jalr x0, x1, 0 */
void rv_mv(int rd, int rs);     /* addi rd, rs, 0 */
void rv_neg(int rd, int rs);    /* sub rd, x0, rs */
void rv_not(int rd, int rs);    /* xori rd, rs, -1 */
void rv_seqz(int rd, int rs);   /* sltiu rd, rs, 1 */

/* Branch/jump to a label not yet known: emit a placeholder and push
 * a fixup. The caller resolves the label later via rv_label(). */
void rv_branch(int rs1, int rs2, int op_token, uint32_t *label_out);
void rv_jal_label(int rd, uint32_t *label_out);

/* Set a label = current text offset. */
void rv_label_set(uint32_t *label);

/* Resolve all pending fixups in this function (called at function end). */
void rv_resolve_fixups(void);

#endif /* CC_RISCV64_H */
