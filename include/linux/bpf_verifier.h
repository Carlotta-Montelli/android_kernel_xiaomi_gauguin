/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#ifndef _LINUX_BPF_VERIFIER_H
#define _LINUX_BPF_VERIFIER_H 1

#include <linux/bpf.h> /* for enum bpf_reg_type */
#include <linux/filter.h> /* for MAX_BPF_STACK */
#include <linux/tnum.h>

/* Maximum variable offset umax_value permitted when resolving memory accesses.
 * In practice this is far bigger than any realistic pointer offset; this limit
 * ensures that umax_value + (int)off + (int)size cannot overflow a u64.
 */
#define BPF_MAX_VAR_OFF	(1 << 29)
/* Maximum variable size permitted for ARG_CONST_SIZE[_OR_ZERO].  This ensures
 * that converting umax_value to int cannot overflow.
 */
#define BPF_MAX_VAR_SIZ	(1 << 29)

/* Liveness marks, used for registers and spilled-regs (in stack slots).
 * Read marks propagate upwards until they find a write mark; they record that
 * "one of this state's descendants read this reg" (and therefore the reg is
 * relevant for states_equal() checks).
 * Write marks collect downwards and do not propagate; they record that "the
 * straight-line code that reached this state (from its parent) wrote this reg"
 * (and therefore that reads propagated from this state or its descendants
 * should not propagate to its parent).
 * A state with a write mark can receive read marks; it just won't propagate
 * them to its parent, since the write mark is a property, not of the state,
 * but of the link between it and its parent.  See mark_reg_read() and
 * mark_stack_slot_read() in kernel/bpf/verifier.c.
 */
enum bpf_reg_liveness {
	REG_LIVE_NONE = 0, /* reg hasn't been read or written this branch */
	REG_LIVE_READ, /* reg was read, so we're sensitive to initial value */
	REG_LIVE_WRITTEN, /* reg was written first, screening off later reads */
	REG_LIVE_DONE = 4, /* liveness won't be updating this register anymore */
};

struct bpf_reg_state {
	/* Ordering of fields matters.  See states_equal() */
	enum bpf_reg_type type;
	union {
		/* valid when type == PTR_TO_PACKET */
		u16 range;

		/* valid when type == CONST_PTR_TO_MAP | PTR_TO_MAP_VALUE |
		 *   PTR_TO_MAP_VALUE_OR_NULL
		 */
		struct bpf_map *map_ptr;

		u32 mem_size; /* for PTR_TO_MEM | PTR_TO_MEM_OR_NULL */
		u32 btf_id; /* for PTR_TO_BTF_ID */

		/* Max size from any of the above. */
		unsigned long raw;
	};
	/* Fixed part of pointer offset, pointer types only */
	s32 off;
	/* For PTR_TO_PACKET, used to find other pointers with the same variable
	 * offset, so they can share range knowledge.
	 * For PTR_TO_MAP_VALUE_OR_NULL this is used to share which map value we
	 * came from, when one is tested for != NULL.
	 * For PTR_TO_MEM_OR_NULL this is used to identify memory allocation
	 * for the purpose of tracking that it's freed.
	 * For PTR_TO_SOCKET this is used to share which pointers retain the
	 * same reference to the socket, to determine proper reference freeing.
	 */
	u32 id;
	/* For scalar types (SCALAR_VALUE), this represents our knowledge of
	 * the actual value.
	 * For pointer types, this represents the variable part of the offset
	 * from the pointed-to object, and is shared with all bpf_reg_states
	 * with the same id as us.
	 */
	struct tnum var_off;
	/* Used to determine if any memory access using this register will
	 * result in a bad access.
	 * These refer to the same value as var_off, not necessarily the actual
	 * contents of the register.
	 */
	s64 smin_value; /* minimum possible (s64)value */
	s64 smax_value; /* maximum possible (s64)value */
	u64 umin_value; /* minimum possible (u64)value */
	u64 umax_value; /* maximum possible (u64)value */
	/* parentage chain for liveness checking */
    u32 ref_obj_id;
	struct bpf_reg_state *parent;
	/* Inside the callee two registers can be both PTR_TO_STACK like
	 * R1=fp-8 and R2=fp-8, but one of them points to this function stack
	 * while another to the caller's stack. To differentiate them 'frameno'
	 * is used which is an index in bpf_verifier_state->frame[] array
	 * pointing to bpf_func_state.
	 */
	u32 frameno;
	s32 subreg_def;
	enum bpf_reg_liveness live;
};

enum bpf_stack_slot_type {
	STACK_INVALID,    /* nothing was stored in this stack slot */
	STACK_SPILL,      /* register spilled into stack */
	STACK_MISC,	  /* BPF program wrote some data into this slot */
	STACK_ZERO,	  /* BPF program wrote constant zero */
};

#define BPF_REG_SIZE 8	/* size of eBPF register in bytes */

struct bpf_stack_state {
	struct bpf_reg_state spilled_ptr;
	u8 slot_type[BPF_REG_SIZE];
};
struct bpf_reference_state {
	/* Track each reference created with a unique id, even if the same
	 * instruction creates the reference multiple times (eg, via CALL).
	 */
	int id;
	/* Instruction where the allocation of this reference occurred. This
	 * is used purely to inform the user of a reference leak.
	 */
	int insn_idx;
};

/* state of the program:
 * type of all registers and stack info
 */
struct bpf_func_state {
	struct bpf_reg_state regs[MAX_BPF_REG];
	/* index of call instruction that called into this func */
	int callsite;
	/* stack frame number of this function state from pov of
	 * enclosing bpf_verifier_state.
	 * 0 = main function, 1 = first callee.
	 */
	u32 frameno;
	/* subprog number == index within subprog_stack_depth
	 * zero == main subprog
	 */
	u32 subprogno;
	/* The following fields should be last. See copy_func_state() */
	int acquired_refs;
	struct bpf_reference_state *refs;
	int allocated_stack;
	struct bpf_stack_state *stack;
};

struct bpf_id_pair {
	u32 old;
	u32 cur;
};

/* Maximum number of register states that can exist at once */
#define BPF_ID_MAP_SIZE (MAX_BPF_REG + MAX_BPF_STACK / BPF_REG_SIZE)
#define MAX_CALL_FRAMES 8
struct bpf_verifier_state {
	/* call stack tracking */
	struct bpf_func_state *frame[MAX_CALL_FRAMES];
	struct bpf_verifier_state *parent;
	/*
	 * 'branches' field is the number of branches left to explore:
	 * 0 - all possible paths from this state reached bpf_exit or
	 * were safely pruned
	 * 1 - at least one path is being explored.
	 * This state hasn't reached bpf_exit
	 * 2 - at least two paths are being explored.
	 * This state is an immediate parent of two children.
	 * One is fallthrough branch with branches==1 and another
	 * state is pushed into stack (to be explored later) also with
	 * branches==1. The parent of this state has branches==1.
	 * The verifier state tree connected via 'parent' pointer looks like:
	 * 1
	 * 1
	 * 2 -> 1 (first 'if' pushed into stack)
	 * 1
	 * 2 -> 1 (second 'if' pushed into stack)
	 * 1
	 * 1
	 * 1 bpf_exit.
	 *
	 * Once do_check() reaches bpf_exit, it calls update_branch_counts()
	 * and the verifier state tree will look:
	 * 1
	 * 1
	 * 2 -> 1 (first 'if' pushed into stack)
	 * 1
	 * 1 -> 1 (second 'if' pushed into stack)
	 * 0
	 * 0
	 * 0 bpf_exit.
	 * After pop_stack() the do_check() will resume at second 'if'.
	 *
	 * If is_state_visited() sees a state with branches > 0 it means
	 * there is a loop. If such state is exactly equal to the current state
	 * it's an infinite loop. Note states_equal() checks for states
	 * equvalency, so two states being 'states_equal' does not mean
	 * infinite loop. The exact comparison is provided by
	 * states_maybe_looping() function. It's a stronger pre-check and
	 * much faster than states_equal().
	 *
	 * This algorithm may not find all possible infinite loops or
	 * loop iteration count may be too high.
	 * In such cases BPF_COMPLEXITY_LIMIT_INSNS limit kicks in.
	 */
	u32 branches;
	u32 insn_idx;
	u32 curframe;
	u32 active_spin_lock;
	bool speculative;
};

#define bpf_get_spilled_reg(slot, frame)				\
	(((slot < frame->allocated_stack / BPF_REG_SIZE) &&		\
	  (frame->stack[slot].slot_type[0] == STACK_SPILL))		\
	 ? &frame->stack[slot].spilled_ptr : NULL)

/* Iterate over 'frame', setting 'reg' to either NULL or a spilled register. */
#define bpf_for_each_spilled_reg(iter, frame, reg)			\
	for (iter = 0, reg = bpf_get_spilled_reg(iter, frame);		\
	     iter < frame->allocated_stack / BPF_REG_SIZE;		\
	     iter++, reg = bpf_get_spilled_reg(iter, frame))

/* linked list of verifier states used to prune search */
struct bpf_verifier_state_list {
	struct bpf_verifier_state state;
	struct bpf_verifier_state_list *next;
	int miss_cnt, hit_cnt;
};

/* Possible states for alu_state member. */
#define BPF_ALU_SANITIZE_SRC		(1U << 0)
#define BPF_ALU_SANITIZE_DST		(1U << 1)
#define BPF_ALU_NEG_VALUE		(1U << 2)
#define BPF_ALU_NON_POINTER		(1U << 3)
#define BPF_ALU_IMMEDIATE		(1U << 4)
#define BPF_ALU_SANITIZE		(BPF_ALU_SANITIZE_SRC | \
					 BPF_ALU_SANITIZE_DST)

struct bpf_insn_aux_data {
	union {
		enum bpf_reg_type ptr_type;	/* pointer type for load/store insns */
		unsigned long map_state;	/* pointer/poison value for maps */
		s32 call_imm;			/* saved imm field of call insn */
		u32 alu_limit;			/* limit for add/sub register with pointer */
		struct {
			u32 map_index;		/* index into used_maps[] */
			u32 map_off;		/* offset from value base address */
		};
	};
	int ctx_field_size; /* the ctx field size for load insn, maybe 0 */
	bool seen; /* this insn was processed by the verifier */
	bool sanitize_stack_spill; /* subject to Spectre v4 sanitation */
	u8 alu_state; /* used in combination with alu_limit */
	bool prune_point;
};

#define MAX_USED_MAPS 64 /* max number of maps accessed by one eBPF program */

#define BPF_VERIFIER_TMP_LOG_SIZE	1024

struct bpf_verifier_log {
	u32 level;
	char kbuf[BPF_VERIFIER_TMP_LOG_SIZE];
	char __user *ubuf;
	u32 len_used;
	u32 len_total;
};

static inline bool bpf_verifier_log_full(const struct bpf_verifier_log *log)
{
	return log->len_used >= log->len_total - 1;
}

#define BPF_LOG_LEVEL1	1
#define BPF_LOG_LEVEL2	2
#define BPF_LOG_STATS	4
#define BPF_LOG_LEVEL	(BPF_LOG_LEVEL1 | BPF_LOG_LEVEL2)
#define BPF_LOG_MASK	(BPF_LOG_LEVEL | BPF_LOG_STATS)
#define BPF_LOG_KERNEL	(BPF_LOG_MASK + 1) /* kernel internal flag */

static inline bool bpf_verifier_log_needed(const struct bpf_verifier_log *log)
{
	return (log->level && log->ubuf && !bpf_verifier_log_full(log)) ||
		log->level == BPF_LOG_KERNEL;
}

static inline bool
bpf_verifier_log_attr_valid(const struct bpf_verifier_log *log)
{
	return log->len_total >= 128 && log->len_total <= UINT_MAX >> 2 &&
	       log->level && log->ubuf && !(log->level & ~BPF_LOG_MASK);
}

#define BPF_MAX_SUBPROGS 256

struct bpf_subprog_info {
	u32 start; /* insn idx of function entry point */
	u32 linfo_idx; /* The idx to the main_prog->aux->linfo */
	u16 stack_depth; /* max. stack depth used by this function */
	bool has_tail_call;
};

/* single container for all structs
 * one verifier_env per bpf_check() call
 */
struct bpf_verifier_env {
	u32 insn_idx;
	u32 prev_insn_idx;
	struct bpf_prog *prog;		/* eBPF program being verified */
	const struct bpf_verifier_ops *ops;
	struct bpf_verifier_stack_elem *head; /* stack of verifier states to be processed */
	int stack_size;			/* number of states to be processed */
	bool strict_alignment;		/* perform strict pointer alignment checks */
	struct bpf_verifier_state *cur_state; /* current verifier state */
	struct bpf_verifier_state_list **explored_states; /* search pruning optimization */
	struct bpf_verifier_state_list *free_list;
	struct bpf_map *used_maps[MAX_USED_MAPS]; /* array of map's used by eBPF program */
	u32 used_map_cnt;		/* number of used maps */
	u32 id_gen;			/* used to generate unique reg IDs */
	bool explore_alu_limits;
	bool allow_ptr_leaks;
	bool seen_direct_write;
	struct bpf_insn_aux_data *insn_aux_data; /* array of per-insn state */
	const struct bpf_line_info *prev_linfo;
	struct bpf_verifier_log log;
	struct bpf_subprog_info subprog_info[BPF_MAX_SUBPROGS + 1];
	struct bpf_id_pair idmap_scratch[BPF_ID_MAP_SIZE];
	u32 subprog_cnt;
	/* number of instructions analyzed by the verifier */
	u32 prev_insn_processed, insn_processed;
	/* number of jmps, calls, exits analyzed so far */
	u32 prev_jmps_processed, jmps_processed;
	/* total verification time */
	u64 verification_time;
	/* maximum number of verifier states kept in 'branching' instructions */
	u32 max_states_per_insn;
	/* total number of allocated verifier states */
	u32 total_states;
	/* some states are freed during program analysis.
	 * this is peak number of states. this number dominates kernel
	 * memory consumption during verification
	 */
	u32 peak_states;
	/* longest register parentage chain walked for liveness marking */
	u32 longest_mark_read_walk;
};

__printf(2, 0) void bpf_verifier_vlog(struct bpf_verifier_log *log,
				      const char *fmt, va_list args);
__printf(2, 3) void bpf_verifier_log_write(struct bpf_verifier_env *env,
					   const char *fmt, ...);
__printf(2, 3) void bpf_log(struct bpf_verifier_log *log,
			    const char *fmt, ...);

static inline struct bpf_func_state *cur_func(struct bpf_verifier_env *env)
{
	struct bpf_verifier_state *cur = env->cur_state;

	return cur->frame[cur->curframe];
}

static inline struct bpf_reg_state *cur_regs(struct bpf_verifier_env *env)
{
	return cur_func(env)->regs;
}

int bpf_prog_offload_verifier_prep(struct bpf_verifier_env *env);
int bpf_prog_offload_verify_insn(struct bpf_verifier_env *env,
				 int insn_idx, int prev_insn_idx);

#include <linux/vmalloc.h>
#include <linux/mm.h>
static inline void *__compat_kvcalloc(size_t n, size_t size, gfp_t flags)
{
        return kvmalloc_array(n, size, flags | __GFP_ZERO);
}
#define kvcalloc __compat_kvcalloc

#endif /* _LINUX_BPF_VERIFIER_H */
