/*
 * Linux Socket Filter - Kernel level socket filtering
 *
 * Based on the design of the Berkeley Packet Filter. The new
 * internal format has been designed by PLUMgrid:
 *
 *	Copyright (c) 2011 - 2014 PLUMgrid, http://plumgrid.com
 *
 * Authors:
 *
 *	Jay Schulist <jschlst@samba.org>
 *	Alexei Starovoitov <ast@plumgrid.com>
 *	Daniel Borkmann <dborkman@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Andi Kleen - Fix a few bad bugs and races.
 * Kris Katterjohn - Added many additional checks in bpf_check_classic()
 */

#include <uapi/linux/btf.h>
#include <linux/filter.h>
#include <linux/skbuff.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/moduleloader.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/frame.h>
#include <linux/rbtree_latch.h>
#include <linux/kallsyms.h>
#include <linux/rcupdate.h>
#include <linux/perf_event.h>
#include <linux/nospec.h>

#include <asm/barrier.h>
#include <asm/unaligned.h>

/* Registers */
#define BPF_R0	regs[BPF_REG_0]
#define BPF_R1	regs[BPF_REG_1]
#define BPF_R2	regs[BPF_REG_2]
#define BPF_R3	regs[BPF_REG_3]
#define BPF_R4	regs[BPF_REG_4]
#define BPF_R5	regs[BPF_REG_5]
#define BPF_R6	regs[BPF_REG_6]
#define BPF_R7	regs[BPF_REG_7]
#define BPF_R8	regs[BPF_REG_8]
#define BPF_R9	regs[BPF_REG_9]
#define BPF_R10	regs[BPF_REG_10]

/* Named registers */
#define DST	regs[insn->dst_reg]
#define SRC	regs[insn->src_reg]
#define FP	regs[BPF_REG_FP]
#define AX	regs[BPF_REG_AX]
#define ARG1	regs[BPF_REG_ARG1]
#define CTX	regs[BPF_REG_CTX]
#define IMM	insn->imm

/* No hurry in this branch
 *
 * Exported for the bpf jit load helper.
 */
void *bpf_internal_load_pointer_neg_helper(const struct sk_buff *skb, int k, unsigned int size)
{
	u8 *ptr = NULL;

	if (k >= SKF_NET_OFF) {
		ptr = skb_network_header(skb) + k - SKF_NET_OFF;
	} else if (k >= SKF_LL_OFF) {
		if (unlikely(!skb_mac_header_was_set(skb)))
			return NULL;
		ptr = skb_mac_header(skb) + k - SKF_LL_OFF;
	}
	if (ptr >= skb->head && ptr + size <= skb_tail_pointer(skb))
		return ptr;

	return NULL;
}

struct bpf_prog *bpf_prog_alloc(unsigned int size, gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | gfp_extra_flags;
	struct bpf_prog_aux *aux;
	struct bpf_prog *fp;

	size = round_up(size, PAGE_SIZE);
	fp = __vmalloc(size, gfp_flags, PAGE_KERNEL);
	if (fp == NULL)
		return NULL;

	aux = kzalloc(sizeof(*aux), GFP_KERNEL | gfp_extra_flags);
	if (aux == NULL) {
		vfree(fp);
		return NULL;
	}

	fp->pages = size / PAGE_SIZE;
	fp->aux = aux;
	fp->aux->prog = fp;
	fp->jit_requested = ebpf_jit_enabled();

	INIT_LIST_HEAD_RCU(&fp->aux->ksym_lnode);

	return fp;
}
EXPORT_SYMBOL_GPL(bpf_prog_alloc);

int bpf_prog_alloc_jited_linfo(struct bpf_prog *prog)
{
	if (!prog->aux->nr_linfo || !prog->jit_requested)
		return 0;
	prog->aux->jited_linfo = kcalloc(prog->aux->nr_linfo,
					 sizeof(*prog->aux->jited_linfo),
					 GFP_KERNEL | __GFP_NOWARN);
	if (!prog->aux->jited_linfo)
		return -ENOMEM;
	return 0;
}
void bpf_prog_free_jited_linfo(struct bpf_prog *prog)
{
	kfree(prog->aux->jited_linfo);
	prog->aux->jited_linfo = NULL;
}
void bpf_prog_free_unused_jited_linfo(struct bpf_prog *prog)
{
	if (prog->aux->jited_linfo && !prog->aux->jited_linfo[0])
		bpf_prog_free_jited_linfo(prog);
}
/* The jit engine is responsible to provide an array
 * for insn_off to the jited_off mapping (insn_to_jit_off).
 *
 * The idx to this array is the insn_off.  Hence, the insn_off
 * here is relative to the prog itself instead of the main prog.
 * This array has one entry for each xlated bpf insn.
 *
 * jited_off is the byte off to the last byte of the jited insn.
 *
 * Hence, with
 * insn_start:
 *      The first bpf insn off of the prog.  The insn off
 *      here is relative to the main prog.
 *      e.g. if prog is a subprog, insn_start > 0
 * linfo_idx:
 *      The prog's idx to prog->aux->linfo and jited_linfo
 *
 * jited_linfo[linfo_idx] = prog->bpf_func
 *
 * For i > linfo_idx,
 *
 * jited_linfo[i] = prog->bpf_func +
 *	insn_to_jit_off[linfo[i].insn_off - insn_start - 1]
 */
void bpf_prog_fill_jited_linfo(struct bpf_prog *prog,
			       const u32 *insn_to_jit_off)
{
	u32 linfo_idx, insn_start, insn_end, nr_linfo, i;
	const struct bpf_line_info *linfo;
	void **jited_linfo;
	if (!prog->aux->jited_linfo)
		/* Userspace did not provide linfo */
		return;
	linfo_idx = prog->aux->linfo_idx;
	linfo = &prog->aux->linfo[linfo_idx];
	insn_start = linfo[0].insn_off;
	insn_end = insn_start + prog->len;
	jited_linfo = &prog->aux->jited_linfo[linfo_idx];
	jited_linfo[0] = prog->bpf_func;
	nr_linfo = prog->aux->nr_linfo - linfo_idx;
	for (i = 1; i < nr_linfo && linfo[i].insn_off < insn_end; i++)
		/* The verifier ensures that linfo[i].insn_off is
		 * strictly increasing
		 */
		jited_linfo[i] = prog->bpf_func +
			insn_to_jit_off[linfo[i].insn_off - insn_start - 1];
}
void bpf_prog_free_linfo(struct bpf_prog *prog)
{
	bpf_prog_free_jited_linfo(prog);
	kvfree(prog->aux->linfo);
}

struct bpf_prog *bpf_prog_realloc(struct bpf_prog *fp_old, unsigned int size,
				  gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | gfp_extra_flags;
	struct bpf_prog *fp;
	u32 pages, delta;
	int ret;

	BUG_ON(fp_old == NULL);

	size = round_up(size, PAGE_SIZE);
	pages = size / PAGE_SIZE;
	if (pages <= fp_old->pages)
		return fp_old;

	delta = pages - fp_old->pages;
	ret = __bpf_prog_charge(fp_old->aux->user, delta);
	if (ret)
		return NULL;

	fp = __vmalloc(size, gfp_flags, PAGE_KERNEL);
	if (fp == NULL) {
		__bpf_prog_uncharge(fp_old->aux->user, delta);
	} else {
		memcpy(fp, fp_old, fp_old->pages * PAGE_SIZE);
		fp->pages = pages;
		fp->aux->prog = fp;

		/* We keep fp->aux from fp_old around in the new
		 * reallocated structure.
		 */
		fp_old->aux = NULL;
		__bpf_prog_free(fp_old);
	}

	return fp;
}

void __bpf_prog_free(struct bpf_prog *fp)
{
	kfree(fp->aux);
	vfree(fp);
}

int bpf_prog_calc_tag(struct bpf_prog *fp)
{
	const u32 bits_offset = SHA_MESSAGE_BYTES - sizeof(__be64);
	u32 raw_size = bpf_prog_tag_scratch_size(fp);
	u32 digest[SHA_DIGEST_WORDS];
	u32 ws[SHA_WORKSPACE_WORDS];
	u32 i, bsize, psize, blocks;
	struct bpf_insn *dst;
	bool was_ld_map;
	u8 *raw, *todo;
	__be32 *result;
	__be64 *bits;

	raw = vmalloc(raw_size);
	if (!raw)
		return -ENOMEM;

	sha_init(digest);
	memset(ws, 0, sizeof(ws));

	/* We need to take out the map fd for the digest calculation
	 * since they are unstable from user space side.
	 */
	dst = (void *)raw;
	for (i = 0, was_ld_map = false; i < fp->len; i++) {
		dst[i] = fp->insnsi[i];
		if (!was_ld_map &&
		    dst[i].code == (BPF_LD | BPF_IMM | BPF_DW) &&
		    (dst[i].src_reg == BPF_PSEUDO_MAP_FD ||
		     dst[i].src_reg == BPF_PSEUDO_MAP_VALUE)) {
			was_ld_map = true;
			dst[i].imm = 0;
		} else if (was_ld_map &&
			   dst[i].code == 0 &&
			   dst[i].dst_reg == 0 &&
			   dst[i].src_reg == 0 &&
			   dst[i].off == 0) {
			was_ld_map = false;
			dst[i].imm = 0;
		} else {
			was_ld_map = false;
		}
	}

	psize = bpf_prog_insn_size(fp);
	memset(&raw[psize], 0, raw_size - psize);
	raw[psize++] = 0x80;

	bsize  = round_up(psize, SHA_MESSAGE_BYTES);
	blocks = bsize / SHA_MESSAGE_BYTES;
	todo   = raw;
	if (bsize - psize >= sizeof(__be64)) {
		bits = (__be64 *)(todo + bsize - sizeof(__be64));
	} else {
		bits = (__be64 *)(todo + bsize + bits_offset);
		blocks++;
	}
	*bits = cpu_to_be64((psize - 1) << 3);

	while (blocks--) {
		sha_transform(digest, todo, ws);
		todo += SHA_MESSAGE_BYTES;
	}

	result = (__force __be32 *)digest;
	for (i = 0; i < SHA_DIGEST_WORDS; i++)
		result[i] = cpu_to_be32(digest[i]);
	memcpy(fp->tag, result, sizeof(fp->tag));

	vfree(raw);
	return 0;
}

static int bpf_adj_delta_to_imm(struct bpf_insn *insn, u32 pos, u32 delta,
				u32 curr, const bool probe_pass)
{
	const s64 imm_min = S32_MIN, imm_max = S32_MAX;
	s64 imm = insn->imm;

	if (curr < pos && curr + imm + 1 > pos)
		imm += delta;
	else if (curr > pos + delta && curr + imm + 1 <= pos + delta)
		imm -= delta;
	if (imm < imm_min || imm > imm_max)
		return -ERANGE;
	if (!probe_pass)
		insn->imm = imm;
	return 0;
}

static int bpf_adj_delta_to_off(struct bpf_insn *insn, u32 pos, u32 delta,
				u32 curr, const bool probe_pass)
{
	const s32 off_min = S16_MIN, off_max = S16_MAX;
	s32 off = insn->off;

	if (curr < pos && curr + off + 1 > pos)
		off += delta;
	else if (curr > pos + delta && curr + off + 1 <= pos + delta)
		off -= delta;
	if (off < off_min || off > off_max)
		return -ERANGE;
	if (!probe_pass)
		insn->off = off;
	return 0;
}

static int bpf_adj_branches(struct bpf_prog *prog, u32 pos, u32 delta,
			    const bool probe_pass)
{
	u32 i, insn_cnt = prog->len + (probe_pass ? delta : 0);
	struct bpf_insn *insn = prog->insnsi;
	int ret = 0;

	for (i = 0; i < insn_cnt; i++, insn++) {
		u8 code;

		/* In the probing pass we still operate on the original,
		 * unpatched image in order to check overflows before we
		 * do any other adjustments. Therefore skip the patchlet.
		 */
		if (probe_pass && i == pos) {
			i += delta + 1;
			insn++;
		}
		code = insn->code;
		if ((BPF_CLASS(code) != BPF_JMP &&
		     BPF_CLASS(code) != BPF_JMP32) ||
		    BPF_OP(code) == BPF_EXIT)
			continue;
		/* Adjust offset of jmps if we cross patch boundaries. */
		if (BPF_OP(code) == BPF_CALL) {
			if (insn->src_reg != BPF_PSEUDO_CALL)
				continue;
			ret = bpf_adj_delta_to_imm(insn, pos, delta, i,
						   probe_pass);
		} else {
			ret = bpf_adj_delta_to_off(insn, pos, delta, i,
						   probe_pass);
		}
		if (ret)
			break;
	}

	return ret;
}

static void bpf_adj_linfo(struct bpf_prog *prog, u32 off, u32 delta)
{
	struct bpf_line_info *linfo;
	u32 i, nr_linfo;
	nr_linfo = prog->aux->nr_linfo;
	if (!nr_linfo || !delta)
		return;
	linfo = prog->aux->linfo;
	for (i = 0; i < nr_linfo; i++)
		if (off < linfo[i].insn_off)
			break;
	/* Push all off < linfo[i].insn_off by delta */
	for (; i < nr_linfo; i++)
		linfo[i].insn_off += delta;
}

struct bpf_prog *bpf_patch_insn_single(struct bpf_prog *prog, u32 off,
				       const struct bpf_insn *patch, u32 len)
{
	u32 insn_adj_cnt, insn_rest, insn_delta = len - 1;
	const u32 cnt_max = S16_MAX;
	struct bpf_prog *prog_adj;

	/* Since our patchlet doesn't expand the image, we're done. */
	if (insn_delta == 0) {
		memcpy(prog->insnsi + off, patch, sizeof(*patch));
		return prog;
	}

	insn_adj_cnt = prog->len + insn_delta;

	/* Reject anything that would potentially let the insn->off
	 * target overflow when we have excessive program expansions.
	 * We need to probe here before we do any reallocation where
	 * we afterwards may not fail anymore.
	 */
	if (insn_adj_cnt > cnt_max &&
	    bpf_adj_branches(prog, off, insn_delta, true))
		return NULL;

	/* Several new instructions need to be inserted. Make room
	 * for them. Likely, there's no need for a new allocation as
	 * last page could have large enough tailroom.
	 */
	prog_adj = bpf_prog_realloc(prog, bpf_prog_size(insn_adj_cnt),
				    GFP_USER);
	if (!prog_adj)
		return NULL;

	prog_adj->len = insn_adj_cnt;

	/* Patching happens in 3 steps:
	 *
	 * 1) Move over tail of insnsi from next instruction onwards,
	 *    so we can patch the single target insn with one or more
	 *    new ones (patching is always from 1 to n insns, n > 0).
	 * 2) Inject new instructions at the target location.
	 * 3) Adjust branch offsets if necessary.
	 */
	insn_rest = insn_adj_cnt - off - len;

	memmove(prog_adj->insnsi + off + len, prog_adj->insnsi + off + 1,
		sizeof(*patch) * insn_rest);
	memcpy(prog_adj->insnsi + off, patch, sizeof(*patch) * len);

	/* We are guaranteed to not fail at this point, otherwise
	 * the ship has sailed to reverse to the original state. An
	 * overflow cannot happen at this point.
	 */
	BUG_ON(bpf_adj_branches(prog_adj, off, insn_delta, false));

	bpf_adj_linfo(prog_adj, off, insn_delta);

	return prog_adj;
}

void bpf_prog_kallsyms_del_subprogs(struct bpf_prog *fp)
{
	int i;

	for (i = 0; i < fp->aux->func_cnt; i++)
		bpf_prog_kallsyms_del(fp->aux->func[i]);
}

void bpf_prog_kallsyms_del_all(struct bpf_prog *fp)
{
	bpf_prog_kallsyms_del_subprogs(fp);
	bpf_prog_kallsyms_del(fp);
}

#ifdef CONFIG_BPF_JIT
/* All BPF JIT sysctl knobs here. */
int bpf_jit_enable   __read_mostly = IS_BUILTIN(CONFIG_BPF_JIT_ALWAYS_ON);
int bpf_jit_harden   __read_mostly;
int bpf_jit_kallsyms __read_mostly;
long bpf_jit_limit   __read_mostly;
long bpf_jit_limit_max __read_mostly;

#ifndef CONFIG_MODULES
void * __weak module_alloc(unsigned long size)
{
	return vmalloc_exec(size);
}

void __weak module_memfree(void *module_region)
{
	vfree(module_region);
}
#endif

#ifndef CONFIG_MODULES
void * __weak module_alloc(unsigned long size)
{
	return vmalloc_exec(size);
}

void __weak module_memfree(void *module_region)
{
	vfree(module_region);
}
#endif

static __always_inline void
bpf_get_prog_addr_region(const struct bpf_prog *prog,
			 unsigned long *symbol_start,
			 unsigned long *symbol_end)
{
	const struct bpf_binary_header *hdr = bpf_jit_binary_hdr(prog);
	unsigned long addr = (unsigned long)hdr;

	WARN_ON_ONCE(!bpf_prog_ebpf_jited(prog));

	*symbol_start = addr;
	*symbol_end   = addr + hdr->pages * PAGE_SIZE;
}

static void bpf_get_prog_name(const struct bpf_prog *prog, char *sym)
{
	const char *end = sym + KSYM_NAME_LEN;
	const struct btf_type *type;
	const char *func_name;

	BUILD_BUG_ON(sizeof("bpf_prog_") +
		     sizeof(prog->tag) * 2 +
		     /* name has been null terminated.
		      * We should need +1 for the '_' preceding
		      * the name.  However, the null character
		      * is double counted between the name and the
		      * sizeof("bpf_prog_") above, so we omit
		      * the +1 here.
		      */
		     sizeof(prog->aux->name) > KSYM_NAME_LEN);

	sym += snprintf(sym, KSYM_NAME_LEN, "bpf_prog_");
	sym  = bin2hex(sym, prog->tag, sizeof(prog->tag));

	/* prog->aux->name will be ignored if full btf name is available */
	if (prog->aux->btf) {
		type = btf_type_by_id(prog->aux->btf,
				      prog->aux->func_info[prog->aux->func_idx].type_id);
		func_name = btf_name_by_offset(prog->aux->btf, type->name_off);
		snprintf(sym, (size_t)(end - sym), "_%s", func_name);
		return;
	}

	if (prog->aux->name[0])
		snprintf(sym, (size_t)(end - sym), "_%s", prog->aux->name);
	else
		*sym = 0;
}

static __always_inline unsigned long
bpf_get_prog_addr_start(struct latch_tree_node *n)
{
	unsigned long symbol_start, symbol_end;
	const struct bpf_prog_aux *aux;

	aux = container_of(n, struct bpf_prog_aux, ksym_tnode);
	bpf_get_prog_addr_region(aux->prog, &symbol_start, &symbol_end);

	return symbol_start;
}

static __always_inline bool bpf_tree_less(struct latch_tree_node *a,
					  struct latch_tree_node *b)
{
	return bpf_get_prog_addr_start(a) < bpf_get_prog_addr_start(b);
}

static __always_inline int bpf_tree_comp(void *key, struct latch_tree_node *n)
{
	unsigned long val = (unsigned long)key;
	unsigned long symbol_start, symbol_end;
	const struct bpf_prog_aux *aux;

	aux = container_of(n, struct bpf_prog_aux, ksym_tnode);
	bpf_get_prog_addr_region(aux->prog, &symbol_start, &symbol_end);

	if (val < symbol_start)
		return -1;
	if (val >= symbol_end)
		return  1;

	return 0;
}

static const struct latch_tree_ops bpf_tree_ops = {
	.less	= bpf_tree_less,
	.comp	= bpf_tree_comp,
};

static DEFINE_SPINLOCK(bpf_lock);
static LIST_HEAD(bpf_kallsyms);
static struct latch_tree_root bpf_tree __cacheline_aligned;

static void bpf_prog_ksym_node_add(struct bpf_prog_aux *aux)
{
	WARN_ON_ONCE(!list_empty(&aux->ksym_lnode));
	list_add_tail_rcu(&aux->ksym_lnode, &bpf_kallsyms);
	latch_tree_insert(&aux->ksym_tnode, &bpf_tree, &bpf_tree_ops);
}

static void bpf_prog_ksym_node_del(struct bpf_prog_aux *aux)
{
	if (list_empty(&aux->ksym_lnode))
		return;

	latch_tree_erase(&aux->ksym_tnode, &bpf_tree, &bpf_tree_ops);
	list_del_rcu(&aux->ksym_lnode);
}

static bool bpf_prog_kallsyms_candidate(const struct bpf_prog *fp)
{
	return fp->jited && !bpf_prog_was_classic(fp);
}

static bool bpf_prog_kallsyms_verify_off(const struct bpf_prog *fp)
{
	return list_empty(&fp->aux->ksym_lnode) ||
	       fp->aux->ksym_lnode.prev == LIST_POISON2;
}

void bpf_prog_kallsyms_add(struct bpf_prog *fp)
{
	if (!bpf_prog_kallsyms_candidate(fp) ||
	    !capable(CAP_SYS_ADMIN))
		return;

	spin_lock_bh(&bpf_lock);
	bpf_prog_ksym_node_add(fp->aux);
	spin_unlock_bh(&bpf_lock);
}

void bpf_prog_kallsyms_del(struct bpf_prog *fp)
{
	if (!bpf_prog_kallsyms_candidate(fp))
		return;

	spin_lock_bh(&bpf_lock);
	bpf_prog_ksym_node_del(fp->aux);
	spin_unlock_bh(&bpf_lock);
}

static struct bpf_prog *bpf_prog_kallsyms_find(unsigned long addr)
{
	struct latch_tree_node *n;

	if (!bpf_jit_kallsyms_enabled())
		return NULL;

	n = latch_tree_find((void *)addr, &bpf_tree, &bpf_tree_ops);
	return n ?
	       container_of(n, struct bpf_prog_aux, ksym_tnode)->prog :
	       NULL;
}

const char *__bpf_address_lookup(unsigned long addr, unsigned long *size,
				 unsigned long *off, char *sym)
{
	unsigned long symbol_start, symbol_end;
	struct bpf_prog *prog;
	char *ret = NULL;

	rcu_read_lock();
	prog = bpf_prog_kallsyms_find(addr);
	if (prog) {
		bpf_get_prog_addr_region(prog, &symbol_start, &symbol_end);
		bpf_get_prog_name(prog, sym);

		ret = sym;
		if (size)
			*size = symbol_end - symbol_start;
		if (off)
			*off  = addr - symbol_start;
	}
	rcu_read_unlock();

	return ret;
}

bool is_bpf_text_address(unsigned long addr)
{
	bool ret;

	rcu_read_lock();
	ret = bpf_prog_kallsyms_find(addr) != NULL;
	rcu_read_unlock();

	return ret;
}

int bpf_get_kallsym(unsigned int symnum, unsigned long *value, char *type,
		    char *sym)
{
	unsigned long symbol_start, symbol_end;
	struct bpf_prog_aux *aux;
	unsigned int it = 0;
	int ret = -ERANGE;

	if (!bpf_jit_kallsyms_enabled())
		return ret;

	rcu_read_lock();
	list_for_each_entry_rcu(aux, &bpf_kallsyms, ksym_lnode) {
		if (it++ != symnum)
			continue;

		bpf_get_prog_addr_region(aux->prog, &symbol_start, &symbol_end);
		bpf_get_prog_name(aux->prog, sym);

		*value = symbol_start;
		*type  = BPF_SYM_ELF_TYPE;

		ret = 0;
		break;
	}
	rcu_read_unlock();

	return ret;
}

static atomic_long_t bpf_jit_current;

/* Can be overridden by an arch's JIT compiler if it has a custom,
 * dedicated BPF backend memory area, or if neither of the two
 * below apply.
 */
u64 __weak bpf_jit_alloc_exec_limit(void)
{
#if defined(MODULES_VADDR)
	return MODULES_END - MODULES_VADDR;
#else
	return VMALLOC_END - VMALLOC_START;
#endif
}

static int __init bpf_jit_charge_init(void)
{
	/* Only used as heuristic here to derive limit. */
	bpf_jit_limit_max = bpf_jit_alloc_exec_limit();
	bpf_jit_limit = min_t(u64, round_up(bpf_jit_limit_max >> 1,
					    PAGE_SIZE), LONG_MAX);
	return 0;
}
pure_initcall(bpf_jit_charge_init);

static int bpf_jit_charge_modmem(u32 pages)
{
	if (atomic_long_add_return(pages, &bpf_jit_current) >
	    (bpf_jit_limit >> PAGE_SHIFT)) {
		if (!capable(CAP_SYS_ADMIN)) {
			atomic_long_sub(pages, &bpf_jit_current);
			return -EPERM;
		}
	}

	return 0;
}

static void bpf_jit_uncharge_modmem(u32 pages)
{
	atomic_long_sub(pages, &bpf_jit_current);
}

#if IS_ENABLED(CONFIG_BPF_JIT) && IS_ENABLED(CONFIG_CFI_CLANG)
bool __weak arch_bpf_jit_check_func(const struct bpf_prog *prog)
{
	return true;
}
EXPORT_SYMBOL_GPL(arch_bpf_jit_check_func);
#endif

void *__weak bpf_jit_alloc_exec(unsigned long size)
{
	return module_alloc(size);
}

void __weak bpf_jit_free_exec(void *addr)
{
	module_memfree(addr);
}

struct bpf_binary_header *
bpf_jit_binary_alloc(unsigned int proglen, u8 **image_ptr,
		     unsigned int alignment,
		     bpf_jit_fill_hole_t bpf_fill_ill_insns)
{
	struct bpf_binary_header *hdr;
	u32 size, hole, start, pages;

	/* Most of BPF filters are really small, but if some of them
	 * fill a page, allow at least 128 extra bytes to insert a
	 * random section of illegal instructions.
	 */
	size = round_up(proglen + sizeof(*hdr) + 128, PAGE_SIZE);
	pages = size / PAGE_SIZE;

	if (bpf_jit_charge_modmem(pages))
		return NULL;
	hdr = bpf_jit_alloc_exec(size);
	if (!hdr) {
		bpf_jit_uncharge_modmem(pages);
		return NULL;
	}

	/* Fill space with illegal/arch-dep instructions. */
	bpf_fill_ill_insns(hdr, size);

	bpf_jit_set_header_magic(hdr);
	hdr->pages = pages;
	hole = min_t(unsigned int, size - (proglen + sizeof(*hdr)),
		     PAGE_SIZE - sizeof(*hdr));
	start = (get_random_int() % hole) & ~(alignment - 1);

	/* Leave a random number of instructions before BPF code. */
	*image_ptr = &hdr->image[start];

	return hdr;
}

void bpf_jit_binary_free(struct bpf_binary_header *hdr)
{
	u32 pages = hdr->pages;

	bpf_jit_free_exec(hdr);
	bpf_jit_uncharge_modmem(pages);
}

/* This symbol is only overridden by archs that have different
 * requirements than the usual eBPF JITs, f.e. when they only
 * implement cBPF JIT, do not set images read-only, etc.
 */
void __weak bpf_jit_free(struct bpf_prog *fp)
{
	if (fp->jited) {
		struct bpf_binary_header *hdr = bpf_jit_binary_hdr(fp);

		bpf_jit_binary_unlock_ro(hdr);
		bpf_jit_binary_free(hdr);

		WARN_ON_ONCE(!bpf_prog_kallsyms_verify_off(fp));
	}

	bpf_prog_unlock_free(fp);
}

static int bpf_jit_blind_insn(const struct bpf_insn *from,
			      const struct bpf_insn *aux,
			      struct bpf_insn *to_buff)
{
	struct bpf_insn *to = to_buff;
	u32 imm_rnd = get_random_int();
	s16 off;

	BUILD_BUG_ON(BPF_REG_AX  + 1 != MAX_BPF_JIT_REG);
	BUILD_BUG_ON(MAX_BPF_REG + 1 != MAX_BPF_JIT_REG);

	/* Constraints on AX register:
	 *
	 * AX register is inaccessible from user space. It is mapped in
	 * all JITs, and used here for constant blinding rewrites. It is
	 * typically "stateless" meaning its contents are only valid within
	 * the executed instruction, but not across several instructions.
	 * There are a few exceptions however which are further detailed
	 * below.
	 *
	 * Constant blinding is only used by JITs, not in the interpreter.
	 * In restricted circumstances, the verifier can also use the AX
	 * register for rewrites as long as they do not interfere with
	 * the above cases!
	 */
	if (from->dst_reg == BPF_REG_AX || from->src_reg == BPF_REG_AX)
		goto out;

	if (from->imm == 0 &&
	    (from->code == (BPF_ALU   | BPF_MOV | BPF_K) ||
	     from->code == (BPF_ALU64 | BPF_MOV | BPF_K))) {
		*to++ = BPF_ALU64_REG(BPF_XOR, from->dst_reg, from->dst_reg);
		goto out;
	}

	switch (from->code) {
	case BPF_ALU | BPF_ADD | BPF_K:
	case BPF_ALU | BPF_SUB | BPF_K:
	case BPF_ALU | BPF_AND | BPF_K:
	case BPF_ALU | BPF_OR  | BPF_K:
	case BPF_ALU | BPF_XOR | BPF_K:
	case BPF_ALU | BPF_MUL | BPF_K:
	case BPF_ALU | BPF_MOV | BPF_K:
	case BPF_ALU | BPF_DIV | BPF_K:
	case BPF_ALU | BPF_MOD | BPF_K:
		*to++ = BPF_ALU32_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU32_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU32_REG(from->code, from->dst_reg, BPF_REG_AX);
		break;

	case BPF_ALU64 | BPF_ADD | BPF_K:
	case BPF_ALU64 | BPF_SUB | BPF_K:
	case BPF_ALU64 | BPF_AND | BPF_K:
	case BPF_ALU64 | BPF_OR  | BPF_K:
	case BPF_ALU64 | BPF_XOR | BPF_K:
	case BPF_ALU64 | BPF_MUL | BPF_K:
	case BPF_ALU64 | BPF_MOV | BPF_K:
	case BPF_ALU64 | BPF_DIV | BPF_K:
	case BPF_ALU64 | BPF_MOD | BPF_K:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU64_REG(from->code, from->dst_reg, BPF_REG_AX);
		break;

	case BPF_JMP | BPF_JEQ  | BPF_K:
	case BPF_JMP | BPF_JNE  | BPF_K:
	case BPF_JMP | BPF_JGT  | BPF_K:
	case BPF_JMP | BPF_JLT  | BPF_K:
	case BPF_JMP | BPF_JGE  | BPF_K:
	case BPF_JMP | BPF_JLE  | BPF_K:
	case BPF_JMP | BPF_JSGT | BPF_K:
	case BPF_JMP | BPF_JSLT | BPF_K:
	case BPF_JMP | BPF_JSGE | BPF_K:
	case BPF_JMP | BPF_JSLE | BPF_K:
	case BPF_JMP | BPF_JSET | BPF_K:
		/* Accommodate for extra offset in case of a backjump. */
		off = from->off;
		if (off < 0)
			off -= 2;
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_JMP_REG(from->code, from->dst_reg, BPF_REG_AX, off);
		break;

	case BPF_LD | BPF_IMM | BPF_DW:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ aux[1].imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU64_IMM(BPF_LSH, BPF_REG_AX, 32);
		*to++ = BPF_ALU64_REG(BPF_MOV, aux[0].dst_reg, BPF_REG_AX);
		break;
	case 0: /* Part 2 of BPF_LD | BPF_IMM | BPF_DW. */
		*to++ = BPF_ALU32_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ aux[0].imm);
		*to++ = BPF_ALU32_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU64_REG(BPF_OR,  aux[0].dst_reg, BPF_REG_AX);
		break;

	case BPF_ST | BPF_MEM | BPF_DW:
	case BPF_ST | BPF_MEM | BPF_W:
	case BPF_ST | BPF_MEM | BPF_H:
	case BPF_ST | BPF_MEM | BPF_B:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_STX_MEM(from->code, from->dst_reg, BPF_REG_AX, from->off);
		break;
	}
out:
	return to - to_buff;
}

static struct bpf_prog *bpf_prog_clone_create(struct bpf_prog *fp_other,
					      gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | gfp_extra_flags;
	struct bpf_prog *fp;

	fp = __vmalloc(fp_other->pages * PAGE_SIZE, gfp_flags, PAGE_KERNEL);
	if (fp != NULL) {
		/* aux->prog still points to the fp_other one, so
		 * when promoting the clone to the real program,
		 * this still needs to be adapted.
		 */
		memcpy(fp, fp_other, fp_other->pages * PAGE_SIZE);
	}

	return fp;
}

static void bpf_prog_clone_free(struct bpf_prog *fp)
{
	/* aux was stolen by the other clone, so we cannot free
	 * it from this path! It will be freed eventually by the
	 * other program on release.
	 *
	 * At this point, we don't need a deferred release since
	 * clone is guaranteed to not be locked.
	 */
	fp->aux = NULL;
	__bpf_prog_free(fp);
}

void bpf_jit_prog_release_other(struct bpf_prog *fp, struct bpf_prog *fp_other)
{
	/* We have to repoint aux->prog to self, as we don't
	 * know whether fp here is the clone or the original.
	 */
	fp->aux->prog = fp;
	bpf_prog_clone_free(fp_other);
}

struct bpf_prog *bpf_jit_blind_constants(struct bpf_prog *prog)
{
	struct bpf_insn insn_buff[16], aux[2];
	struct bpf_prog *clone, *tmp;
	int insn_delta, insn_cnt;
	struct bpf_insn *insn;
	int i, rewritten;

	if (!bpf_jit_blinding_enabled(prog) || prog->blinded)
		return prog;

	clone = bpf_prog_clone_create(prog, GFP_USER);
	if (!clone)
		return ERR_PTR(-ENOMEM);

	insn_cnt = clone->len;
	insn = clone->insnsi;

	for (i = 0; i < insn_cnt; i++, insn++) {
		/* We temporarily need to hold the original ld64 insn
		 * so that we can still access the first part in the
		 * second blinding run.
		 */
		if (insn[0].code == (BPF_LD | BPF_IMM | BPF_DW) &&
		    insn[1].code == 0)
			memcpy(aux, insn, sizeof(aux));

		rewritten = bpf_jit_blind_insn(insn, aux, insn_buff);
		if (!rewritten)
			continue;

		tmp = bpf_patch_insn_single(clone, i, insn_buff, rewritten);
		if (!tmp) {
			/* Patching may have repointed aux->prog during
			 * realloc from the original one, so we need to
			 * fix it up here on error.
			 */
			bpf_jit_prog_release_other(prog, clone);
			return ERR_PTR(-ENOMEM);
		}

		clone = tmp;
		insn_delta = rewritten - 1;

		/* Walk new program and skip insns we just inserted. */
		insn = clone->insnsi + i + insn_delta;
		insn_cnt += insn_delta;
		i        += insn_delta;
	}

	clone->blinded = 1;
	return clone;
}
#endif /* CONFIG_BPF_JIT */

/* Base function for offset calculation. Needs to go into .text section,
 * therefore keeping it non-static as well; will also be used by JITs
 * anyway later on, so do not let the compiler omit it. This also needs
 * to go into kallsyms for correlation from e.g. bpftool, so naming
 * must not change.
 */
noinline u64 __bpf_call_base(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5)
{
	return 0;
}
EXPORT_SYMBOL_GPL(__bpf_call_base);

/* All UAPI available opcodes. */
#define BPF_INSN_MAP(INSN_2, INSN_3)		\
	/* 32 bit ALU operations. */		\
	/*   Register based. */			\
	INSN_3(ALU, ADD, X),			\
	INSN_3(ALU, SUB, X),			\
	INSN_3(ALU, AND, X),			\
	INSN_3(ALU, OR,  X),			\
	INSN_3(ALU, LSH, X),			\
	INSN_3(ALU, RSH, X),			\
	INSN_3(ALU, XOR, X),			\
	INSN_3(ALU, MUL, X),			\
	INSN_3(ALU, MOV, X),			\
	INSN_3(ALU, DIV, X),			\
	INSN_3(ALU, MOD, X),			\
	INSN_2(ALU, NEG),			\
	INSN_3(ALU, END, TO_BE),		\
	INSN_3(ALU, END, TO_LE),		\
	/*   Immediate based. */		\
	INSN_3(ALU, ADD, K),			\
	INSN_3(ALU, SUB, K),			\
	INSN_3(ALU, AND, K),			\
	INSN_3(ALU, OR,  K),			\
	INSN_3(ALU, LSH, K),			\
	INSN_3(ALU, RSH, K),			\
	INSN_3(ALU, XOR, K),			\
	INSN_3(ALU, MUL, K),			\
	INSN_3(ALU, MOV, K),			\
	INSN_3(ALU, DIV, K),			\
	INSN_3(ALU, MOD, K),			\
	/* 64 bit ALU operations. */		\
	/*   Register based. */			\
	INSN_3(ALU64, ADD,  X),			\
	INSN_3(ALU64, SUB,  X),			\
	INSN_3(ALU64, AND,  X),			\
	INSN_3(ALU64, OR,   X),			\
	INSN_3(ALU64, LSH,  X),			\
	INSN_3(ALU64, RSH,  X),			\
	INSN_3(ALU64, XOR,  X),			\
	INSN_3(ALU64, MUL,  X),			\
	INSN_3(ALU64, MOV,  X),			\
	INSN_3(ALU64, ARSH, X),			\
	INSN_3(ALU64, DIV,  X),			\
	INSN_3(ALU64, MOD,  X),			\
	INSN_2(ALU64, NEG),			\
	/*   Immediate based. */		\
	INSN_3(ALU64, ADD,  K),			\
	INSN_3(ALU64, SUB,  K),			\
	INSN_3(ALU64, AND,  K),			\
	INSN_3(ALU64, OR,   K),			\
	INSN_3(ALU64, LSH,  K),			\
	INSN_3(ALU64, RSH,  K),			\
	INSN_3(ALU64, XOR,  K),			\
	INSN_3(ALU64, MUL,  K),			\
	INSN_3(ALU64, MOV,  K),			\
	INSN_3(ALU64, ARSH, K),			\
	INSN_3(ALU64, DIV,  K),			\
	INSN_3(ALU64, MOD,  K),			\
	/* Call instruction. */			\
	INSN_2(JMP, CALL),			\
	/* Exit instruction. */			\
	INSN_2(JMP, EXIT),			\
	/* Jump instructions. */		\
	/*   Register based. */			\
	INSN_3(JMP, JEQ,  X),			\
	INSN_3(JMP, JNE,  X),			\
	INSN_3(JMP, JGT,  X),			\
	INSN_3(JMP, JLT,  X),			\
	INSN_3(JMP, JGE,  X),			\
	INSN_3(JMP, JLE,  X),			\
	INSN_3(JMP, JSGT, X),			\
	INSN_3(JMP, JSLT, X),			\
	INSN_3(JMP, JSGE, X),			\
	INSN_3(JMP, JSLE, X),			\
	INSN_3(JMP, JSET, X),			\
	/*   Immediate based. */		\
	INSN_3(JMP, JEQ,  K),			\
	INSN_3(JMP, JNE,  K),			\
	INSN_3(JMP, JGT,  K),			\
	INSN_3(JMP, JLT,  K),			\
	INSN_3(JMP, JGE,  K),			\
	INSN_3(JMP, JLE,  K),			\
	INSN_3(JMP, JSGT, K),			\
	INSN_3(JMP, JSLT, K),			\
	INSN_3(JMP, JSGE, K),			\
	INSN_3(JMP, JSLE, K),			\
	INSN_3(JMP, JSET, K),			\
	INSN_2(JMP, JA),			\
	/* Store instructions. */		\
	/*   Register based. */			\
	INSN_3(STX, MEM,  B),			\
	INSN_3(STX, MEM,  H),			\
	INSN_3(STX, MEM,  W),			\
	INSN_3(STX, MEM,  DW),			\
	INSN_3(STX, XADD, W),			\
	INSN_3(STX, XADD, DW),			\
	/*   Immediate based. */		\
	INSN_3(ST, MEM, B),			\
	INSN_3(ST, MEM, H),			\
	INSN_3(ST, MEM, W),			\
	INSN_3(ST, MEM, DW),			\
	/* Load instructions. */		\
	/*   Register based. */			\
	INSN_3(LDX, MEM, B),			\
	INSN_3(LDX, MEM, H),			\
	INSN_3(LDX, MEM, W),			\
	INSN_3(LDX, MEM, DW),			\
	/*   Immediate based. */		\
	INSN_3(LD, IMM, DW)

bool bpf_opcode_in_insntable(u8 code)
{
#define BPF_INSN_2_TBL(x, y)    [BPF_##x | BPF_##y] = true
#define BPF_INSN_3_TBL(x, y, z) [BPF_##x | BPF_##y | BPF_##z] = true
	static const bool public_insntable[256] = {
		[0 ... 255] = false,
		/* Now overwrite non-defaults ... */
		BPF_INSN_MAP(BPF_INSN_2_TBL, BPF_INSN_3_TBL),
		/* UAPI exposed, but rewritten opcodes. cBPF carry-over. */
		[BPF_LD | BPF_ABS | BPF_B] = true,
		[BPF_LD | BPF_ABS | BPF_H] = true,
		[BPF_LD | BPF_ABS | BPF_W] = true,
		[BPF_LD | BPF_IND | BPF_B] = true,
		[BPF_LD | BPF_IND | BPF_H] = true,
		[BPF_LD | BPF_IND | BPF_W] = true,
	};
#undef BPF_INSN_3_TBL
#undef BPF_INSN_2_TBL
	return public_insntable[code];
}

#ifndef CONFIG_BPF_JIT_ALWAYS_ON
/**
 *	__bpf_prog_run - run eBPF program on a given context
 *	@ctx: is the data we are operating on
 *	@insn: is the array of eBPF instructions
 *
 * Decode and execute eBPF instructions.
 */
static u64 ___bpf_prog_run(u64 *regs, const struct bpf_insn *insn, u64 *stack)
{
#define BPF_INSN_2_LBL(x, y)    [BPF_##x | BPF_##y] = &&x##_##y
#define BPF_INSN_3_LBL(x, y, z) [BPF_##x | BPF_##y | BPF_##z] = &&x##_##y##_##z
	static const void *jumptable[256] = {
		[0 ... 255] = &&default_label,
		/* Now overwrite non-defaults ... */
		BPF_INSN_MAP(BPF_INSN_2_LBL, BPF_INSN_3_LBL),
		/* Non-UAPI available opcodes. */
		[BPF_JMP | BPF_CALL_ARGS] = &&JMP_CALL_ARGS,
		[BPF_JMP | BPF_TAIL_CALL] = &&JMP_TAIL_CALL,
		[BPF_ST  | BPF_NOSPEC] = &&ST_NOSPEC,
	};
#undef BPF_INSN_3_LBL
#undef BPF_INSN_2_LBL
	u32 tail_call_cnt = 0;
	u64 tmp;

#define CONT	 ({ insn++; goto select_insn; })
#define CONT_JMP ({ insn++; goto select_insn; })

select_insn:
	goto *jumptable[insn->code];

	/* ALU */
#define ALU(OPCODE, OP)			\
	ALU64_##OPCODE##_X:		\
		DST = DST OP SRC;	\
		CONT;			\
	ALU_##OPCODE##_X:		\
		DST = (u32) DST OP (u32) SRC;	\
		CONT;			\
	ALU64_##OPCODE##_K:		\
		DST = DST OP IMM;		\
		CONT;			\
	ALU_##OPCODE##_K:		\
		DST = (u32) DST OP (u32) IMM;	\
		CONT;

	ALU(ADD,  +)
	ALU(SUB,  -)
	ALU(AND,  &)
	ALU(OR,   |)
	ALU(LSH, <<)
	ALU(RSH, >>)
	ALU(XOR,  ^)
	ALU(MUL,  *)
#undef ALU
	ALU_NEG:
		DST = (u32) -DST;
		CONT;
	ALU64_NEG:
		DST = -DST;
		CONT;
	ALU_MOV_X:
		DST = (u32) SRC;
		CONT;
	ALU_MOV_K:
		DST = (u32) IMM;
		CONT;
	ALU64_MOV_X:
		DST = SRC;
		CONT;
	ALU64_MOV_K:
		DST = IMM;
		CONT;
	LD_IMM_DW:
		DST = (u64) (u32) insn[0].imm | ((u64) (u32) insn[1].imm) << 32;
		insn++;
		CONT;
	ALU64_ARSH_X:
		(*(s64 *) &DST) >>= SRC;
		CONT;
	ALU64_ARSH_K:
		(*(s64 *) &DST) >>= IMM;
		CONT;
	ALU64_MOD_X:
		div64_u64_rem(DST, SRC, &tmp);
		DST = tmp;
		CONT;
	ALU_MOD_X:
		tmp = (u32) DST;
		DST = do_div(tmp, (u32) SRC);
		CONT;
	ALU64_MOD_K:
		div64_u64_rem(DST, IMM, &tmp);
		DST = tmp;
		CONT;
	ALU_MOD_K:
		tmp = (u32) DST;
		DST = do_div(tmp, (u32) IMM);
		CONT;
	ALU64_DIV_X:
		DST = div64_u64(DST, SRC);
		CONT;
	ALU_DIV_X:
		tmp = (u32) DST;
		do_div(tmp, (u32) SRC);
		DST = (u32) tmp;
		CONT;
	ALU64_DIV_K:
		DST = div64_u64(DST, IMM);
		CONT;
	ALU_DIV_K:
		tmp = (u32) DST;
		do_div(tmp, (u32) IMM);
		DST = (u32) tmp;
		CONT;
	ALU_END_TO_BE:
		switch (IMM) {
		case 16:
			DST = (__force u16) cpu_to_be16(DST);
			break;
		case 32:
			DST = (__force u32) cpu_to_be32(DST);
			break;
		case 64:
			DST = (__force u64) cpu_to_be64(DST);
			break;
		}
		CONT;
	ALU_END_TO_LE:
		switch (IMM) {
		case 16:
			DST = (__force u16) cpu_to_le16(DST);
			break;
		case 32:
			DST = (__force u32) cpu_to_le32(DST);
			break;
		case 64:
			DST = (__force u64) cpu_to_le64(DST);
			break;
		}
		CONT;

	/* CALL */
	JMP_CALL:
		/* Function call scratches BPF_R1-BPF_R5 registers,
		 * preserves BPF_R6-BPF_R9, and stores return value
		 * into BPF_R0.
		 */
		BPF_R0 = (__bpf_call_base + insn->imm)(BPF_R1, BPF_R2, BPF_R3,
						       BPF_R4, BPF_R5);
		CONT;

	JMP_CALL_ARGS:
		BPF_R0 = (__bpf_call_base_args + insn->imm)(BPF_R1, BPF_R2,
							    BPF_R3, BPF_R4,
							    BPF_R5,
							    insn + insn->off + 1);
		CONT;

	JMP_TAIL_CALL: {
		struct bpf_map *map = (struct bpf_map *) (unsigned long) BPF_R2;
		struct bpf_array *array = container_of(map, struct bpf_array, map);
		struct bpf_prog *prog;
		u32 index = BPF_R3;

		if (unlikely(index >= array->map.max_entries))
			goto out;
		if (unlikely(tail_call_cnt > MAX_TAIL_CALL_CNT))
			goto out;

		tail_call_cnt++;

		prog = READ_ONCE(array->ptrs[index]);
		if (!prog)
			goto out;

		/* ARG1 at this point is guaranteed to point to CTX from
		 * the verifier side due to the fact that the tail call is
		 * handeled like a helper, that is, bpf_tail_call_proto,
		 * where arg1_type is ARG_PTR_TO_CTX.
		 */
		insn = prog->insnsi;
		goto select_insn;
out:
		CONT;
	}
	/* JMP */
	JMP_JA:
		insn += insn->off;
		CONT;
	JMP_JEQ_X:
		if (DST == SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JEQ_K:
		if (DST == IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JNE_X:
		if (DST != SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JNE_K:
		if (DST != IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JGT_X:
		if (DST > SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JGT_K:
		if (DST > IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JLT_X:
		if (DST < SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JLT_K:
		if (DST < IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JGE_X:
		if (DST >= SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JGE_K:
		if (DST >= IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JLE_X:
		if (DST <= SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JLE_K:
		if (DST <= IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSGT_X:
		if (((s64) DST) > ((s64) SRC)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSGT_K:
		if (((s64) DST) > ((s64) IMM)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSLT_X:
		if (((s64) DST) < ((s64) SRC)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSLT_K:
		if (((s64) DST) < ((s64) IMM)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSGE_X:
		if (((s64) DST) >= ((s64) SRC)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSGE_K:
		if (((s64) DST) >= ((s64) IMM)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSLE_X:
		if (((s64) DST) <= ((s64) SRC)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSLE_K:
		if (((s64) DST) <= ((s64) IMM)) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSET_X:
		if (DST & SRC) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_JSET_K:
		if (DST & IMM) {
			insn += insn->off;
			CONT_JMP;
		}
		CONT;
	JMP_EXIT:
		return BPF_R0;

	/* ST, STX and LDX*/
	ST_NOSPEC:
		/* Speculation barrier for mitigating Speculative Store Bypass.
		 * In case of arm64, we rely on the firmware mitigation as
		 * controlled via the ssbd kernel parameter. Whenever the
		 * mitigation is enabled, it works for all of the kernel code
		 * with no need to provide any additional instructions here.
		 * In case of x86, we use 'lfence' insn for mitigation. We
		 * reuse preexisting logic from Spectre v1 mitigation that
		 * happens to produce the required code on x86 for v4 as well.
		 */
		barrier_nospec();
		CONT;
#define LDST(SIZEOP, SIZE)						\
	STX_MEM_##SIZEOP:						\
		*(SIZE *)(unsigned long) (DST + insn->off) = SRC;	\
		CONT;							\
	ST_MEM_##SIZEOP:						\
		*(SIZE *)(unsigned long) (DST + insn->off) = IMM;	\
		CONT;							\
	LDX_MEM_##SIZEOP:						\
		DST = *(SIZE *)(unsigned long) (SRC + insn->off);	\
		CONT;

	LDST(B,   u8)
	LDST(H,  u16)
	LDST(W,  u32)
	LDST(DW, u64)
#undef LDST
	STX_XADD_W: /* lock xadd *(u32 *)(dst_reg + off16) += src_reg */
		atomic_add((u32) SRC, (atomic_t *)(unsigned long)
			   (DST + insn->off));
		CONT;
	STX_XADD_DW: /* lock xadd *(u64 *)(dst_reg + off16) += src_reg */
		atomic64_add((u64) SRC, (atomic64_t *)(unsigned long)
			     (DST + insn->off));
		CONT;

	default_label:
		/* If we ever reach this, we have a bug somewhere. Die hard here
		 * instead of just returning 0; we could be somewhere in a subprog,
		 * so execution could continue otherwise which we do /not/ want.
		 *
		 * Note, verifier whitelists all opcodes in bpf_opcode_in_insntable().
		 */
		pr_warn("BPF interpreter: unknown opcode %02x\n", insn->code);
		BUG_ON(1);
		return 0;
}
STACK_FRAME_NON_STANDARD(___bpf_prog_run); /* jump table */

#define PROG_NAME(stack_size) __bpf_prog_run##stack_size
#define DEFINE_BPF_PROG_RUN(stack_size) \
static unsigned int PROG_NAME(stack_size)(const void *ctx, const struct bpf_insn *insn) \
{ \
	u64 stack[stack_size / sizeof(u64)]; \
	u64 regs[MAX_BPF_EXT_REG]; \
\
	FP = (u64) (unsigned long) &stack[ARRAY_SIZE(stack)]; \
	ARG1 = (u64) (unsigned long) ctx; \
	return ___bpf_prog_run(regs, insn, stack); \
}

#define PROG_NAME_ARGS(stack_size) __bpf_prog_run_args##stack_size
#define DEFINE_BPF_PROG_RUN_ARGS(stack_size) \
static u64 PROG_NAME_ARGS(stack_size)(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5, \
				      const struct bpf_insn *insn) \
{ \
	u64 stack[stack_size / sizeof(u64)]; \
	u64 regs[MAX_BPF_EXT_REG]; \
\
	FP = (u64) (unsigned long) &stack[ARRAY_SIZE(stack)]; \
	BPF_R1 = r1; \
	BPF_R2 = r2; \
	BPF_R3 = r3; \
	BPF_R4 = r4; \
	BPF_R5 = r5; \
	return ___bpf_prog_run(regs, insn, stack); \
}

#define EVAL1(FN, X) FN(X)
#define EVAL2(FN, X, Y...) FN(X) EVAL1(FN, Y)
#define EVAL3(FN, X, Y...) FN(X) EVAL2(FN, Y)
#define EVAL4(FN, X, Y...) FN(X) EVAL3(FN, Y)
#define EVAL5(FN, X, Y...) FN(X) EVAL4(FN, Y)
#define EVAL6(FN, X, Y...) FN(X) EVAL5(FN, Y)

EVAL6(DEFINE_BPF_PROG_RUN, 32, 64, 96, 128, 160, 192);
EVAL6(DEFINE_BPF_PROG_RUN, 224, 256, 288, 320, 352, 384);
EVAL4(DEFINE_BPF_PROG_RUN, 416, 448, 480, 512);

EVAL6(DEFINE_BPF_PROG_RUN_ARGS, 32, 64, 96, 128, 160, 192);
EVAL6(DEFINE_BPF_PROG_RUN_ARGS, 224, 256, 288, 320, 352, 384);
EVAL4(DEFINE_BPF_PROG_RUN_ARGS, 416, 448, 480, 512);

#define PROG_NAME_LIST(stack_size) PROG_NAME(stack_size),

static unsigned int (*interpreters[])(const void *ctx,
				      const struct bpf_insn *insn) = {
EVAL6(PROG_NAME_LIST, 32, 64, 96, 128, 160, 192)
EVAL6(PROG_NAME_LIST, 224, 256, 288, 320, 352, 384)
EVAL4(PROG_NAME_LIST, 416, 448, 480, 512)
};
#undef PROG_NAME_LIST
#define PROG_NAME_LIST(stack_size) PROG_NAME_ARGS(stack_size),
static u64 (*interpreters_args[])(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5,
				  const struct bpf_insn *insn) = {
EVAL6(PROG_NAME_LIST, 32, 64, 96, 128, 160, 192)
EVAL6(PROG_NAME_LIST, 224, 256, 288, 320, 352, 384)
EVAL4(PROG_NAME_LIST, 416, 448, 480, 512)
};
#undef PROG_NAME_LIST

void bpf_patch_call_args(struct bpf_insn *insn, u32 stack_depth)
{
	stack_depth = max_t(u32, stack_depth, 1);
	insn->off = (s16) insn->imm;
	insn->imm = interpreters_args[(round_up(stack_depth, 32) / 32) - 1] -
		__bpf_call_base_args;
	insn->code = BPF_JMP | BPF_CALL_ARGS;
}

#else
static unsigned int __bpf_prog_ret0_warn(const void *ctx,
					 const struct bpf_insn *insn)
{
	/* If this handler ever gets executed, then BPF_JIT_ALWAYS_ON
	 * is not working properly, so warn about it!
	 */
	WARN_ON_ONCE(1);
	return 0;
}
#endif

bool bpf_prog_array_compatible(struct bpf_array *array,
			       const struct bpf_prog *fp)
{
	if (fp->kprobe_override)
		return false;

	if (!array->owner_prog_type) {
		/* There's no owner yet where we could check for
		 * compatibility.
		 */
		array->owner_prog_type = fp->type;
		array->owner_jited = fp->jited;

		return true;
	}

	return array->owner_prog_type == fp->type &&
	       array->owner_jited == fp->jited;
}

static int bpf_check_tail_call(const struct bpf_prog *fp)
{
	struct bpf_prog_aux *aux = fp->aux;
	int i;

	for (i = 0; i < aux->used_map_cnt; i++) {
		struct bpf_map *map = aux->used_maps[i];
		struct bpf_array *array;

		if (map->map_type != BPF_MAP_TYPE_PROG_ARRAY)
			continue;

		array = container_of(map, struct bpf_array, map);
		if (!bpf_prog_array_compatible(array, fp))
			return -EINVAL;
	}

	return 0;
}

static void bpf_prog_select_func(struct bpf_prog *fp)
{
#ifndef CONFIG_BPF_JIT_ALWAYS_ON
	u32 stack_depth = max_t(u32, fp->aux->stack_depth, 1);

	fp->bpf_func = interpreters[(round_up(stack_depth, 32) / 32) - 1];
#else
	fp->bpf_func = __bpf_prog_ret0_warn;
#endif
}

/**
 *	bpf_prog_select_runtime - select exec runtime for BPF program
 *	@fp: bpf_prog populated with internal BPF program
 *	@err: pointer to error variable
 *
 * Try to JIT eBPF program, if JIT is not available, use interpreter.
 * The BPF program will be executed via BPF_PROG_RUN() macro.
 */
struct bpf_prog *bpf_prog_select_runtime(struct bpf_prog *fp, int *err)
{
	/* In case of BPF to BPF calls, verifier did all the prep
	 * work with regards to JITing, etc.
	 */
	if (fp->bpf_func)
		goto finalize;

	bpf_prog_select_func(fp);

	/* eBPF JITs can rewrite the program in case constant
	 * blinding is active. However, in case of error during
	 * blinding, bpf_int_jit_compile() must always return a
	 * valid program, which in this case would simply not
	 * be JITed, but falls back to the interpreter.
	 */
	if (!bpf_prog_is_dev_bound(fp->aux)) {
		*err = bpf_prog_alloc_jited_linfo(fp);
		if (*err)
			return fp;
		fp = bpf_int_jit_compile(fp);
		if (!fp->jited) {
			bpf_prog_free_jited_linfo(fp);
#ifdef CONFIG_BPF_JIT_ALWAYS_ON
			*err = -ENOTSUPP;
			return fp;
#endif
		} else {
			bpf_prog_free_unused_jited_linfo(fp);
		}
	} else {
		*err = bpf_prog_offload_compile(fp);
		if (*err)
			return fp;
	}

finalize:
	bpf_prog_lock_ro(fp);

	/* The tail call compatibility check can only be done at
	 * this late stage as we need to determine, if we deal
	 * with JITed or non JITed program concatenations and not
	 * all eBPF JITs might immediately support all features.
	 */
	*err = bpf_check_tail_call(fp);

	return fp;
}
EXPORT_SYMBOL_GPL(bpf_prog_select_runtime);

static unsigned int __bpf_prog_ret1(const void *ctx,
				    const struct bpf_insn *insn)
{
	return 1;
}

static struct bpf_prog_dummy {
	struct bpf_prog prog;
} dummy_bpf_prog = {
	.prog = {
		.bpf_func = __bpf_prog_ret1,
	},
};

/* to avoid allocating empty bpf_prog_array for cgroups that
 * don't have bpf program attached use one global 'empty_prog_array'
 * It will not be modified the caller of bpf_prog_array_alloc()
 * (since caller requested prog_cnt == 0)
 * that pointer should be 'freed' by bpf_prog_array_free()
 */
static struct {
	struct bpf_prog_array hdr;
	struct bpf_prog *null_prog;
} empty_prog_array = {
	.null_prog = NULL,
};

struct bpf_prog_array *bpf_prog_array_alloc(u32 prog_cnt, gfp_t flags)
{
	if (prog_cnt)
		return kzalloc(sizeof(struct bpf_prog_array) +
			       sizeof(struct bpf_prog_array_item) *
			       (prog_cnt + 1),
			       flags);

	return &empty_prog_array.hdr;
}

void bpf_prog_array_free(struct bpf_prog_array __rcu *progs)
{
	if (!progs ||
	    progs == (struct bpf_prog_array __rcu *)&empty_prog_array.hdr)
		return;
	kfree_rcu(progs, rcu);
}

int bpf_prog_array_length(struct bpf_prog_array __rcu *array)
{
	struct bpf_prog_array_item *item;
	u32 cnt = 0;

	rcu_read_lock();
	item = rcu_dereference(array)->items;
	for (; item->prog; item++)
		if (item->prog != &dummy_bpf_prog.prog)
			cnt++;
	rcu_read_unlock();
	return cnt;
}

bool bpf_prog_array_is_empty(struct bpf_prog_array *array)
{
	struct bpf_prog_array_item *item;
	for (item = array->items; item->prog; item++)
		if (item->prog != &dummy_bpf_prog.prog)
			return false;
	return true;
}


static bool bpf_prog_array_copy_core(struct bpf_prog_array __rcu *array,
				     u32 *prog_ids,
				     u32 request_cnt)
{
	struct bpf_prog_array_item *item;
	int i = 0;

	item = rcu_dereference_check(array, 1)->items;
	for (; item->prog; item++) {
		if (item->prog == &dummy_bpf_prog.prog)
			continue;
		prog_ids[i] = item->prog->aux->id;
		if (++i == request_cnt) {
			item++;
			break;
		}
	}

	return !!(item->prog);
}

int bpf_prog_array_copy_to_user(struct bpf_prog_array __rcu *array,
				__u32 __user *prog_ids, u32 cnt)
{
	unsigned long err = 0;
	bool nospc;
	u32 *ids;

	/* users of this function are doing:
	 * cnt = bpf_prog_array_length();
	 * if (cnt > 0)
	 *     bpf_prog_array_copy_to_user(..., cnt);
	 * so below kcalloc doesn't need extra cnt > 0 check, but
	 * bpf_prog_array_length() releases rcu lock and
	 * prog array could have been swapped with empty or larger array,
	 * so always copy 'cnt' prog_ids to the user.
	 * In a rare race the user will see zero prog_ids
	 */
	ids = kcalloc(cnt, sizeof(u32), GFP_USER | __GFP_NOWARN);
	if (!ids)
		return -ENOMEM;
	rcu_read_lock();
	nospc = bpf_prog_array_copy_core(array, ids, cnt);
	rcu_read_unlock();
	err = copy_to_user(prog_ids, ids, cnt * sizeof(u32));
	kfree(ids);
	if (err)
		return -EFAULT;
	if (nospc)
		return -ENOSPC;
	return 0;
}

void bpf_prog_array_delete_safe(struct bpf_prog_array __rcu *array,
				struct bpf_prog *old_prog)
{
	struct bpf_prog_array_item *item = array->items;

	for (; item->prog; item++)
		if (item->prog == old_prog) {
			WRITE_ONCE(item->prog, &dummy_bpf_prog.prog);
			break;
		}
}

int bpf_prog_array_copy(struct bpf_prog_array __rcu *old_array,
			struct bpf_prog *exclude_prog,
			struct bpf_prog *include_prog,
			struct bpf_prog_array **new_array)
{
	int new_prog_cnt, carry_prog_cnt = 0;
	struct bpf_prog_array_item *existing;
	struct bpf_prog_array *array;
	bool found_exclude = false;
	int new_prog_idx = 0;

	/* Figure out how many existing progs we need to carry over to
	 * the new array.
	 */
	if (old_array) {
		existing = old_array->items;
		for (; existing->prog; existing++) {
			if (existing->prog == exclude_prog) {
				found_exclude = true;
				continue;
			}
			if (existing->prog != &dummy_bpf_prog.prog)
				carry_prog_cnt++;
			if (existing->prog == include_prog)
				return -EEXIST;
		}
	}

	if (exclude_prog && !found_exclude)
		return -ENOENT;

	/* How many progs (not NULL) will be in the new array? */
	new_prog_cnt = carry_prog_cnt;
	if (include_prog)
		new_prog_cnt += 1;

	/* Do we have any prog (not NULL) in the new array? */
	if (!new_prog_cnt) {
		*new_array = NULL;
		return 0;
	}

	/* +1 as the end of prog_array is marked with NULL */
	array = bpf_prog_array_alloc(new_prog_cnt + 1, GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	/* Fill in the new prog array */
	if (carry_prog_cnt) {
		existing = old_array->items;
		for (; existing->prog; existing++)
			if (existing->prog != exclude_prog &&
			    existing->prog != &dummy_bpf_prog.prog) {
				array->items[new_prog_idx++].prog =
					existing->prog;
			}
	}
	if (include_prog)
		array->items[new_prog_idx++].prog = include_prog;
	array->items[new_prog_idx].prog = NULL;
	*new_array = array;
	return 0;
}

int bpf_prog_array_copy_info(struct bpf_prog_array __rcu *array,
			     u32 *prog_ids, u32 request_cnt,
			     u32 *prog_cnt)
{
	u32 cnt = 0;

	if (array)
		cnt = bpf_prog_array_length(array);

	*prog_cnt = cnt;

	/* return early if user requested only program count or nothing to copy */
	if (!request_cnt || !cnt)
		return 0;

	/* this function is called under trace/bpf_trace.c: bpf_event_mutex */
	return bpf_prog_array_copy_core(array, prog_ids, request_cnt) ? -ENOSPC
								     : 0;
}

static void bpf_prog_free_deferred(struct work_struct *work)
{
	struct bpf_prog_aux *aux;
	int i;

	aux = container_of(work, struct bpf_prog_aux, work);
	if (bpf_prog_is_dev_bound(aux))
		bpf_prog_offload_destroy(aux->prog);
#ifdef CONFIG_PERF_EVENTS
	if (aux->prog->has_callchain_buf)
		put_callchain_buffers();
#endif
	for (i = 0; i < aux->func_cnt; i++)
		bpf_jit_free(aux->func[i]);
	if (aux->func_cnt) {
		kfree(aux->func);
		bpf_prog_unlock_free(aux->prog);
	} else {
		bpf_jit_free(aux->prog);
	}
}

/* Free internal BPF program */
void bpf_prog_free(struct bpf_prog *fp)
{
	struct bpf_prog_aux *aux = fp->aux;

	INIT_WORK(&aux->work, bpf_prog_free_deferred);
	schedule_work(&aux->work);
}
EXPORT_SYMBOL_GPL(bpf_prog_free);

/* RNG for unpriviledged user space with separated state from prandom_u32(). */
static DEFINE_PER_CPU(struct rnd_state, bpf_user_rnd_state);

void bpf_user_rnd_init_once(void)
{
	prandom_init_once(&bpf_user_rnd_state);
}

BPF_CALL_0(bpf_user_rnd_u32)
{
	/* Should someone ever have the rather unwise idea to use some
	 * of the registers passed into this function, then note that
	 * this function is called from native eBPF and classic-to-eBPF
	 * transformations. Register assignments from both sides are
	 * different, f.e. classic always sets fn(ctx, A, X) here.
	 */
	struct rnd_state *state;
	u32 res;

	state = &get_cpu_var(bpf_user_rnd_state);
	res = prandom_u32_state(state);
	put_cpu_var(bpf_user_rnd_state);

	return res;
}

/* Weak definitions of helper functions in case we don't have bpf syscall. */
const struct bpf_func_proto bpf_map_lookup_elem_proto __weak;
const struct bpf_func_proto bpf_map_update_elem_proto __weak;
const struct bpf_func_proto bpf_map_delete_elem_proto __weak;
const struct bpf_func_proto bpf_spin_lock_proto __weak;
const struct bpf_func_proto bpf_spin_unlock_proto __weak;

const struct bpf_func_proto bpf_get_prandom_u32_proto __weak;
const struct bpf_func_proto bpf_get_smp_processor_id_proto __weak;
const struct bpf_func_proto bpf_get_numa_node_id_proto __weak;
const struct bpf_func_proto bpf_ktime_get_ns_proto __weak;
const struct bpf_func_proto bpf_ktime_get_boot_ns_proto __weak;

const struct bpf_func_proto bpf_get_current_pid_tgid_proto __weak;
const struct bpf_func_proto bpf_get_current_uid_gid_proto __weak;
const struct bpf_func_proto bpf_get_current_comm_proto __weak;
const struct bpf_func_proto bpf_get_current_cgroup_id_proto __weak;
const struct bpf_func_proto bpf_get_local_storage_proto __weak;

BPF_CALL_5(bpf_trace_printk_dummy, char *, fmt, u32, fmt_size, u64, arg1,
	   u64, arg2, u64, arg3)
{
	return 0;
}

static const struct bpf_func_proto bpf_trace_printk_dummy_proto = {
	.func		= bpf_trace_printk_dummy,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_PTR_TO_MEM,
	.arg2_type	= ARG_CONST_SIZE,
};

const struct bpf_func_proto * __weak bpf_get_trace_printk_proto(void)
{
	return &bpf_trace_printk_dummy_proto;
}

u64 __weak
bpf_event_output(struct bpf_map *map, u64 flags, void *meta, u64 meta_size,
		 void *ctx, u64 ctx_size, bpf_ctx_copy_t ctx_copy)
{
	return -ENOTSUPP;
}
EXPORT_SYMBOL_GPL(bpf_event_output);

/* Always built-in helper functions. */
const struct bpf_func_proto bpf_tail_call_proto = {
	.func		= NULL,
	.gpl_only	= false,
	.ret_type	= RET_VOID,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

/* Stub for JITs that only support cBPF. eBPF programs are interpreted.
 * It is encouraged to implement bpf_int_jit_compile() instead, so that
 * eBPF and implicitly also cBPF can get JITed!
 */
struct bpf_prog * __weak bpf_int_jit_compile(struct bpf_prog *prog)
{
	return prog;
}

/* Stub for JITs that support eBPF. All cBPF code gets transformed into
 * eBPF by the kernel and is later compiled by bpf_int_jit_compile().
 */
void __weak bpf_jit_compile(struct bpf_prog *prog)
{
}

bool __weak bpf_helper_changes_pkt_data(void *func)
{
	return false;
}

/* To execute LD_ABS/LD_IND instructions __bpf_prog_run() may call
 * skb_copy_bits(), so provide a weak definition of it for NET-less config.
 */
int __weak skb_copy_bits(const struct sk_buff *skb, int offset, void *to,
			 int len)
{
	return -EFAULT;
}

/* All definitions of tracepoints related to BPF. */
#define CREATE_TRACE_POINTS
#include <linux/bpf_trace.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(xdp_exception);
