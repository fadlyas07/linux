// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <inttypes.h>
#include <regex.h>
#include <stdlib.h>
#include <linux/mman.h>
#include <linux/time64.h>
#include "debug.h"
#include "dso.h"
#include "sort.h"
#include "hist.h"
#include "cacheline.h"
#include "comm.h"
#include "map.h"
#include "maps.h"
#include "symbol.h"
#include "map_symbol.h"
#include "branch.h"
#include "thread.h"
#include "evsel.h"
#include "evlist.h"
#include "srcline.h"
#include "strlist.h"
#include "strbuf.h"
#include "mem-events.h"
#include "mem-info.h"
#include "annotate.h"
#include "annotate-data.h"
#include "event.h"
#include "time-utils.h"
#include "cgroup.h"
#include "machine.h"
#include "trace-event.h"
#include <linux/kernel.h>
#include <linux/string.h>

#ifdef HAVE_LIBTRACEEVENT
#include <event-parse.h>
#endif

regex_t		parent_regex;
const char	default_parent_pattern[] = "^sys_|^do_page_fault";
const char	*parent_pattern = default_parent_pattern;
const char	*default_sort_order = "comm,dso,symbol";
const char	default_branch_sort_order[] = "comm,dso_from,symbol_from,symbol_to,cycles";
const char	default_mem_sort_order[] = "local_weight,mem,sym,dso,symbol_daddr,dso_daddr,snoop,tlb,locked,blocked,local_ins_lat,local_p_stage_cyc";
const char	default_top_sort_order[] = "dso,symbol";
const char	default_diff_sort_order[] = "dso,symbol";
const char	default_tracepoint_sort_order[] = "trace";
const char	*sort_order;
const char	*field_order;
regex_t		ignore_callees_regex;
int		have_ignore_callees = 0;
enum sort_mode	sort__mode = SORT_MODE__NORMAL;
static const char *const dynamic_headers[] = {"local_ins_lat", "ins_lat", "local_p_stage_cyc", "p_stage_cyc"};
static const char *const arch_specific_sort_keys[] = {"local_p_stage_cyc", "p_stage_cyc"};

/*
 * Some architectures have Adjacent Cacheline Prefetch feature, which
 * behaves like the cacheline size is doubled. Enable this flag to
 * check things in double cacheline granularity.
 */
bool chk_double_cl;

/*
 * Replaces all occurrences of a char used with the:
 *
 * -t, --field-separator
 *
 * option, that uses a special separator character and don't pad with spaces,
 * replacing all occurrences of this separator in symbol names (and other
 * output) with a '.' character, that thus it's the only non valid separator.
*/
static int repsep_snprintf(char *bf, size_t size, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(bf, size, fmt, ap);
	if (symbol_conf.field_sep && n > 0) {
		char *sep = bf;

		while (1) {
			sep = strchr(sep, *symbol_conf.field_sep);
			if (sep == NULL)
				break;
			*sep = '.';
		}
	}
	va_end(ap);

	if (n >= (int)size)
		return size - 1;
	return n;
}

static int64_t cmp_null(const void *l, const void *r)
{
	if (!l && !r)
		return 0;
	else if (!l)
		return -1;
	else
		return 1;
}

/* --sort pid */

static int64_t
sort__thread_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return thread__tid(right->thread) - thread__tid(left->thread);
}

static int hist_entry__thread_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	const char *comm = thread__comm_str(he->thread);

	width = max(7U, width) - 8;
	return repsep_snprintf(bf, size, "%7d:%-*.*s", thread__tid(he->thread),
			       width, width, comm ?: "");
}

static int hist_entry__thread_filter(struct hist_entry *he, int type, const void *arg)
{
	const struct thread *th = arg;

	if (type != HIST_FILTER__THREAD)
		return -1;

	return th && !RC_CHK_EQUAL(he->thread, th);
}

struct sort_entry sort_thread = {
	.se_header	= "    Pid:Command",
	.se_cmp		= sort__thread_cmp,
	.se_snprintf	= hist_entry__thread_snprintf,
	.se_filter	= hist_entry__thread_filter,
	.se_width_idx	= HISTC_THREAD,
};

/* --sort tgid */

static int64_t
sort__tgid_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return thread__pid(right->thread) - thread__pid(left->thread);
}

static int hist_entry__tgid_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	int tgid = thread__pid(he->thread);
	const char *comm = NULL;

	/* display comm of the thread-group leader */
	if (thread__pid(he->thread) == thread__tid(he->thread)) {
		comm = thread__comm_str(he->thread);
	} else {
		struct maps *maps = thread__maps(he->thread);
		struct thread *leader = machine__find_thread(maps__machine(maps),
							     tgid, tgid);
		if (leader) {
			comm = thread__comm_str(leader);
			thread__put(leader);
		}
	}
	width = max(7U, width) - 8;
	return repsep_snprintf(bf, size, "%7d:%-*.*s", tgid, width, width, comm ?: "");
}

struct sort_entry sort_tgid = {
	.se_header	= "   Tgid:Command",
	.se_cmp		= sort__tgid_cmp,
	.se_snprintf	= hist_entry__tgid_snprintf,
	.se_width_idx	= HISTC_TGID,
};

/* --sort simd */

static int64_t
sort__simd_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (left->simd_flags.arch != right->simd_flags.arch)
		return (int64_t) left->simd_flags.arch - right->simd_flags.arch;

	return (int64_t) left->simd_flags.pred - right->simd_flags.pred;
}

static const char *hist_entry__get_simd_name(struct simd_flags *simd_flags)
{
	u64 arch = simd_flags->arch;

	if (arch & SIMD_OP_FLAGS_ARCH_SVE)
		return "SVE";
	else
		return "n/a";
}

static int hist_entry__simd_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width __maybe_unused)
{
	const char *name;

	if (!he->simd_flags.arch)
		return repsep_snprintf(bf, size, "");

	name = hist_entry__get_simd_name(&he->simd_flags);

	if (he->simd_flags.pred & SIMD_OP_FLAGS_PRED_EMPTY)
		return repsep_snprintf(bf, size, "[e] %s", name);
	else if (he->simd_flags.pred & SIMD_OP_FLAGS_PRED_PARTIAL)
		return repsep_snprintf(bf, size, "[p] %s", name);

	return repsep_snprintf(bf, size, "[.] %s", name);
}

struct sort_entry sort_simd = {
	.se_header	= "Simd   ",
	.se_cmp		= sort__simd_cmp,
	.se_snprintf	= hist_entry__simd_snprintf,
	.se_width_idx	= HISTC_SIMD,
};

/* --sort comm */

/*
 * We can't use pointer comparison in functions below,
 * because it gives different results based on pointer
 * values, which could break some sorting assumptions.
 */
static int64_t
sort__comm_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return strcmp(comm__str(right->comm), comm__str(left->comm));
}

static int64_t
sort__comm_collapse(struct hist_entry *left, struct hist_entry *right)
{
	return strcmp(comm__str(right->comm), comm__str(left->comm));
}

static int64_t
sort__comm_sort(struct hist_entry *left, struct hist_entry *right)
{
	return strcmp(comm__str(right->comm), comm__str(left->comm));
}

static int hist_entry__comm_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*.*s", width, width, comm__str(he->comm));
}

struct sort_entry sort_comm = {
	.se_header	= "Command",
	.se_cmp		= sort__comm_cmp,
	.se_collapse	= sort__comm_collapse,
	.se_sort	= sort__comm_sort,
	.se_snprintf	= hist_entry__comm_snprintf,
	.se_filter	= hist_entry__thread_filter,
	.se_width_idx	= HISTC_COMM,
};

/* --sort dso */

static int64_t _sort__dso_cmp(struct map *map_l, struct map *map_r)
{
	struct dso *dso_l = map_l ? map__dso(map_l) : NULL;
	struct dso *dso_r = map_r ? map__dso(map_r) : NULL;
	const char *dso_name_l, *dso_name_r;

	if (!dso_l || !dso_r)
		return cmp_null(dso_r, dso_l);

	if (verbose > 0) {
		dso_name_l = dso__long_name(dso_l);
		dso_name_r = dso__long_name(dso_r);
	} else {
		dso_name_l = dso__short_name(dso_l);
		dso_name_r = dso__short_name(dso_r);
	}

	return strcmp(dso_name_l, dso_name_r);
}

static int64_t
sort__dso_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return _sort__dso_cmp(right->ms.map, left->ms.map);
}

static int _hist_entry__dso_snprintf(struct map *map, char *bf,
				     size_t size, unsigned int width)
{
	const struct dso *dso = map ? map__dso(map) : NULL;
	const char *dso_name = "[unknown]";

	if (dso)
		dso_name = verbose > 0 ? dso__long_name(dso) : dso__short_name(dso);

	return repsep_snprintf(bf, size, "%-*.*s", width, width, dso_name);
}

static int hist_entry__dso_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	return _hist_entry__dso_snprintf(he->ms.map, bf, size, width);
}

static int hist_entry__dso_filter(struct hist_entry *he, int type, const void *arg)
{
	const struct dso *dso = arg;

	if (type != HIST_FILTER__DSO)
		return -1;

	return dso && (!he->ms.map || map__dso(he->ms.map) != dso);
}

struct sort_entry sort_dso = {
	.se_header	= "Shared Object",
	.se_cmp		= sort__dso_cmp,
	.se_snprintf	= hist_entry__dso_snprintf,
	.se_filter	= hist_entry__dso_filter,
	.se_width_idx	= HISTC_DSO,
};

/* --sort symbol */

static int64_t _sort__addr_cmp(u64 left_ip, u64 right_ip)
{
	return (int64_t)(right_ip - left_ip);
}

int64_t _sort__sym_cmp(struct symbol *sym_l, struct symbol *sym_r)
{
	if (!sym_l || !sym_r)
		return cmp_null(sym_l, sym_r);

	if (sym_l == sym_r)
		return 0;

	if (sym_l->inlined || sym_r->inlined) {
		int ret = strcmp(sym_l->name, sym_r->name);

		if (ret)
			return ret;
		if ((sym_l->start <= sym_r->end) && (sym_l->end >= sym_r->start))
			return 0;
	}

	if (sym_l->start != sym_r->start)
		return (int64_t)(sym_r->start - sym_l->start);

	return (int64_t)(sym_r->end - sym_l->end);
}

static int64_t
sort__sym_cmp(struct hist_entry *left, struct hist_entry *right)
{
	int64_t ret;

	if (!left->ms.sym && !right->ms.sym)
		return _sort__addr_cmp(left->ip, right->ip);

	/*
	 * comparing symbol address alone is not enough since it's a
	 * relative address within a dso.
	 */
	if (!hists__has(left->hists, dso)) {
		ret = sort__dso_cmp(left, right);
		if (ret != 0)
			return ret;
	}

	return _sort__sym_cmp(left->ms.sym, right->ms.sym);
}

static int64_t
sort__sym_sort(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->ms.sym || !right->ms.sym)
		return cmp_null(left->ms.sym, right->ms.sym);

	return strcmp(right->ms.sym->name, left->ms.sym->name);
}

static int _hist_entry__sym_snprintf(struct map_symbol *ms,
				     u64 ip, char level, char *bf, size_t size,
				     unsigned int width)
{
	struct symbol *sym = ms->sym;
	struct map *map = ms->map;
	size_t ret = 0;

	if (verbose > 0) {
		struct dso *dso = map ? map__dso(map) : NULL;
		char o = dso ? dso__symtab_origin(dso) : '!';
		u64 rip = ip;

		if (dso && dso__kernel(dso) && dso__adjust_symbols(dso))
			rip = map__unmap_ip(map, ip);

		ret += repsep_snprintf(bf, size, "%-#*llx %c ",
				       BITS_PER_LONG / 4 + 2, rip, o);
	}

	ret += repsep_snprintf(bf + ret, size - ret, "[%c] ", level);
	if (sym && map) {
		if (sym->type == STT_OBJECT) {
			ret += repsep_snprintf(bf + ret, size - ret, "%s", sym->name);
			ret += repsep_snprintf(bf + ret, size - ret, "+0x%llx",
					ip - map__unmap_ip(map, sym->start));
		} else {
			ret += repsep_snprintf(bf + ret, size - ret, "%.*s",
					       width - ret,
					       sym->name);
			if (sym->inlined)
				ret += repsep_snprintf(bf + ret, size - ret,
						       " (inlined)");
		}
	} else {
		size_t len = BITS_PER_LONG / 4;
		ret += repsep_snprintf(bf + ret, size - ret, "%-#.*llx",
				       len, ip);
	}

	return ret;
}

int hist_entry__sym_snprintf(struct hist_entry *he, char *bf, size_t size, unsigned int width)
{
	return _hist_entry__sym_snprintf(&he->ms, he->ip,
					 he->level, bf, size, width);
}

static int hist_entry__sym_filter(struct hist_entry *he, int type, const void *arg)
{
	const char *sym = arg;

	if (type != HIST_FILTER__SYMBOL)
		return -1;

	return sym && (!he->ms.sym || !strstr(he->ms.sym->name, sym));
}

struct sort_entry sort_sym = {
	.se_header	= "Symbol",
	.se_cmp		= sort__sym_cmp,
	.se_sort	= sort__sym_sort,
	.se_snprintf	= hist_entry__sym_snprintf,
	.se_filter	= hist_entry__sym_filter,
	.se_width_idx	= HISTC_SYMBOL,
};

/* --sort symoff */

static int64_t
sort__symoff_cmp(struct hist_entry *left, struct hist_entry *right)
{
	int64_t ret;

	ret = sort__sym_cmp(left, right);
	if (ret)
		return ret;

	return left->ip - right->ip;
}

static int64_t
sort__symoff_sort(struct hist_entry *left, struct hist_entry *right)
{
	int64_t ret;

	ret = sort__sym_sort(left, right);
	if (ret)
		return ret;

	return left->ip - right->ip;
}

static int
hist_entry__symoff_snprintf(struct hist_entry *he, char *bf, size_t size, unsigned int width)
{
	struct symbol *sym = he->ms.sym;

	if (sym == NULL)
		return repsep_snprintf(bf, size, "[%c] %-#.*llx", he->level, width - 4, he->ip);

	return repsep_snprintf(bf, size, "[%c] %s+0x%llx", he->level, sym->name, he->ip - sym->start);
}

struct sort_entry sort_sym_offset = {
	.se_header	= "Symbol Offset",
	.se_cmp		= sort__symoff_cmp,
	.se_sort	= sort__symoff_sort,
	.se_snprintf	= hist_entry__symoff_snprintf,
	.se_filter	= hist_entry__sym_filter,
	.se_width_idx	= HISTC_SYMBOL_OFFSET,
};

/* --sort srcline */

char *hist_entry__srcline(struct hist_entry *he)
{
	return map__srcline(he->ms.map, he->ip, he->ms.sym);
}

static int64_t
sort__srcline_cmp(struct hist_entry *left, struct hist_entry *right)
{
	int64_t ret;

	ret = _sort__addr_cmp(left->ip, right->ip);
	if (ret)
		return ret;

	return sort__dso_cmp(left, right);
}

static int64_t
sort__srcline_collapse(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->srcline)
		left->srcline = hist_entry__srcline(left);
	if (!right->srcline)
		right->srcline = hist_entry__srcline(right);

	return strcmp(right->srcline, left->srcline);
}

static int64_t
sort__srcline_sort(struct hist_entry *left, struct hist_entry *right)
{
	return sort__srcline_collapse(left, right);
}

static void
sort__srcline_init(struct hist_entry *he)
{
	if (!he->srcline)
		he->srcline = hist_entry__srcline(he);
}

static int hist_entry__srcline_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-.*s", width, he->srcline);
}

struct sort_entry sort_srcline = {
	.se_header	= "Source:Line",
	.se_cmp		= sort__srcline_cmp,
	.se_collapse	= sort__srcline_collapse,
	.se_sort	= sort__srcline_sort,
	.se_init	= sort__srcline_init,
	.se_snprintf	= hist_entry__srcline_snprintf,
	.se_width_idx	= HISTC_SRCLINE,
};

/* --sort srcline_from */

static char *addr_map_symbol__srcline(struct addr_map_symbol *ams)
{
	return map__srcline(ams->ms.map, ams->al_addr, ams->ms.sym);
}

static int64_t
sort__srcline_from_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->branch_info->from.addr - right->branch_info->from.addr;
}

static int64_t
sort__srcline_from_collapse(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info->srcline_from)
		left->branch_info->srcline_from = addr_map_symbol__srcline(&left->branch_info->from);

	if (!right->branch_info->srcline_from)
		right->branch_info->srcline_from = addr_map_symbol__srcline(&right->branch_info->from);

	return strcmp(right->branch_info->srcline_from, left->branch_info->srcline_from);
}

static int64_t
sort__srcline_from_sort(struct hist_entry *left, struct hist_entry *right)
{
	return sort__srcline_from_collapse(left, right);
}

static void sort__srcline_from_init(struct hist_entry *he)
{
	if (!he->branch_info->srcline_from)
		he->branch_info->srcline_from = addr_map_symbol__srcline(&he->branch_info->from);
}

static int hist_entry__srcline_from_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*.*s", width, width, he->branch_info->srcline_from);
}

struct sort_entry sort_srcline_from = {
	.se_header	= "From Source:Line",
	.se_cmp		= sort__srcline_from_cmp,
	.se_collapse	= sort__srcline_from_collapse,
	.se_sort	= sort__srcline_from_sort,
	.se_init	= sort__srcline_from_init,
	.se_snprintf	= hist_entry__srcline_from_snprintf,
	.se_width_idx	= HISTC_SRCLINE_FROM,
};

/* --sort srcline_to */

static int64_t
sort__srcline_to_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->branch_info->to.addr - right->branch_info->to.addr;
}

static int64_t
sort__srcline_to_collapse(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info->srcline_to)
		left->branch_info->srcline_to = addr_map_symbol__srcline(&left->branch_info->to);

	if (!right->branch_info->srcline_to)
		right->branch_info->srcline_to = addr_map_symbol__srcline(&right->branch_info->to);

	return strcmp(right->branch_info->srcline_to, left->branch_info->srcline_to);
}

static int64_t
sort__srcline_to_sort(struct hist_entry *left, struct hist_entry *right)
{
	return sort__srcline_to_collapse(left, right);
}

static void sort__srcline_to_init(struct hist_entry *he)
{
	if (!he->branch_info->srcline_to)
		he->branch_info->srcline_to = addr_map_symbol__srcline(&he->branch_info->to);
}

static int hist_entry__srcline_to_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*.*s", width, width, he->branch_info->srcline_to);
}

struct sort_entry sort_srcline_to = {
	.se_header	= "To Source:Line",
	.se_cmp		= sort__srcline_to_cmp,
	.se_collapse	= sort__srcline_to_collapse,
	.se_sort	= sort__srcline_to_sort,
	.se_init	= sort__srcline_to_init,
	.se_snprintf	= hist_entry__srcline_to_snprintf,
	.se_width_idx	= HISTC_SRCLINE_TO,
};

static int hist_entry__sym_ipc_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{

	struct symbol *sym = he->ms.sym;
	struct annotated_branch *branch;
	double ipc = 0.0, coverage = 0.0;
	char tmp[64];

	if (!sym)
		return repsep_snprintf(bf, size, "%-*s", width, "-");

	branch = symbol__annotation(sym)->branch;

	if (branch && branch->hit_cycles)
		ipc = branch->hit_insn / ((double)branch->hit_cycles);

	if (branch && branch->total_insn) {
		coverage = branch->cover_insn * 100.0 /
			((double)branch->total_insn);
	}

	snprintf(tmp, sizeof(tmp), "%-5.2f [%5.1f%%]", ipc, coverage);
	return repsep_snprintf(bf, size, "%-*s", width, tmp);
}

struct sort_entry sort_sym_ipc = {
	.se_header	= "IPC   [IPC Coverage]",
	.se_cmp		= sort__sym_cmp,
	.se_snprintf	= hist_entry__sym_ipc_snprintf,
	.se_width_idx	= HISTC_SYMBOL_IPC,
};

static int hist_entry__sym_ipc_null_snprintf(struct hist_entry *he
					     __maybe_unused,
					     char *bf, size_t size,
					     unsigned int width)
{
	char tmp[64];

	snprintf(tmp, sizeof(tmp), "%-5s %2s", "-", "-");
	return repsep_snprintf(bf, size, "%-*s", width, tmp);
}

struct sort_entry sort_sym_ipc_null = {
	.se_header	= "IPC   [IPC Coverage]",
	.se_cmp		= sort__sym_cmp,
	.se_snprintf	= hist_entry__sym_ipc_null_snprintf,
	.se_width_idx	= HISTC_SYMBOL_IPC,
};

/* --sort callchain_branch_predicted */

static int64_t
sort__callchain_branch_predicted_cmp(struct hist_entry *left __maybe_unused,
				     struct hist_entry *right __maybe_unused)
{
	return 0;
}

static int hist_entry__callchain_branch_predicted_snprintf(
	struct hist_entry *he, char *bf, size_t size, unsigned int width)
{
	u64 branch_count, predicted_count;
	double percent = 0.0;
	char str[32];

	callchain_branch_counts(he->callchain, &branch_count,
				&predicted_count, NULL, NULL);

	if (branch_count)
		percent = predicted_count * 100.0 / branch_count;

	snprintf(str, sizeof(str), "%.1f%%", percent);
	return repsep_snprintf(bf, size, "%-*.*s", width, width, str);
}

struct sort_entry sort_callchain_branch_predicted = {
	.se_header	= "Predicted",
	.se_cmp		= sort__callchain_branch_predicted_cmp,
	.se_snprintf	= hist_entry__callchain_branch_predicted_snprintf,
	.se_width_idx	= HISTC_CALLCHAIN_BRANCH_PREDICTED,
};

/* --sort callchain_branch_abort */

static int64_t
sort__callchain_branch_abort_cmp(struct hist_entry *left __maybe_unused,
				 struct hist_entry *right __maybe_unused)
{
	return 0;
}

static int hist_entry__callchain_branch_abort_snprintf(struct hist_entry *he,
						       char *bf, size_t size,
						       unsigned int width)
{
	u64 branch_count, abort_count;
	char str[32];

	callchain_branch_counts(he->callchain, &branch_count,
				NULL, &abort_count, NULL);

	snprintf(str, sizeof(str), "%" PRId64, abort_count);
	return repsep_snprintf(bf, size, "%-*.*s", width, width, str);
}

struct sort_entry sort_callchain_branch_abort = {
	.se_header	= "Abort",
	.se_cmp		= sort__callchain_branch_abort_cmp,
	.se_snprintf	= hist_entry__callchain_branch_abort_snprintf,
	.se_width_idx	= HISTC_CALLCHAIN_BRANCH_ABORT,
};

/* --sort callchain_branch_cycles */

static int64_t
sort__callchain_branch_cycles_cmp(struct hist_entry *left __maybe_unused,
				  struct hist_entry *right __maybe_unused)
{
	return 0;
}

static int hist_entry__callchain_branch_cycles_snprintf(struct hist_entry *he,
							char *bf, size_t size,
							unsigned int width)
{
	u64 branch_count, cycles_count, cycles = 0;
	char str[32];

	callchain_branch_counts(he->callchain, &branch_count,
				NULL, NULL, &cycles_count);

	if (branch_count)
		cycles = cycles_count / branch_count;

	snprintf(str, sizeof(str), "%" PRId64 "", cycles);
	return repsep_snprintf(bf, size, "%-*.*s", width, width, str);
}

struct sort_entry sort_callchain_branch_cycles = {
	.se_header	= "Cycles",
	.se_cmp		= sort__callchain_branch_cycles_cmp,
	.se_snprintf	= hist_entry__callchain_branch_cycles_snprintf,
	.se_width_idx	= HISTC_CALLCHAIN_BRANCH_CYCLES,
};

/* --sort srcfile */

static char no_srcfile[1];

static char *hist_entry__get_srcfile(struct hist_entry *e)
{
	char *sf, *p;
	struct map *map = e->ms.map;

	if (!map)
		return no_srcfile;

	sf = __get_srcline(map__dso(map), map__rip_2objdump(map, e->ip),
			 e->ms.sym, false, true, true, e->ip);
	if (sf == SRCLINE_UNKNOWN)
		return no_srcfile;
	p = strchr(sf, ':');
	if (p && *sf) {
		*p = 0;
		return sf;
	}
	free(sf);
	return no_srcfile;
}

static int64_t
sort__srcfile_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return sort__srcline_cmp(left, right);
}

static int64_t
sort__srcfile_collapse(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->srcfile)
		left->srcfile = hist_entry__get_srcfile(left);
	if (!right->srcfile)
		right->srcfile = hist_entry__get_srcfile(right);

	return strcmp(right->srcfile, left->srcfile);
}

static int64_t
sort__srcfile_sort(struct hist_entry *left, struct hist_entry *right)
{
	return sort__srcfile_collapse(left, right);
}

static void sort__srcfile_init(struct hist_entry *he)
{
	if (!he->srcfile)
		he->srcfile = hist_entry__get_srcfile(he);
}

static int hist_entry__srcfile_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-.*s", width, he->srcfile);
}

struct sort_entry sort_srcfile = {
	.se_header	= "Source File",
	.se_cmp		= sort__srcfile_cmp,
	.se_collapse	= sort__srcfile_collapse,
	.se_sort	= sort__srcfile_sort,
	.se_init	= sort__srcfile_init,
	.se_snprintf	= hist_entry__srcfile_snprintf,
	.se_width_idx	= HISTC_SRCFILE,
};

/* --sort parent */

static int64_t
sort__parent_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct symbol *sym_l = left->parent;
	struct symbol *sym_r = right->parent;

	if (!sym_l || !sym_r)
		return cmp_null(sym_l, sym_r);

	return strcmp(sym_r->name, sym_l->name);
}

static int hist_entry__parent_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*.*s", width, width,
			      he->parent ? he->parent->name : "[other]");
}

struct sort_entry sort_parent = {
	.se_header	= "Parent symbol",
	.se_cmp		= sort__parent_cmp,
	.se_snprintf	= hist_entry__parent_snprintf,
	.se_width_idx	= HISTC_PARENT,
};

/* --sort cpu */

static int64_t
sort__cpu_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return right->cpu - left->cpu;
}

static int hist_entry__cpu_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%*.*d", width, width, he->cpu);
}

struct sort_entry sort_cpu = {
	.se_header      = "CPU",
	.se_cmp	        = sort__cpu_cmp,
	.se_snprintf    = hist_entry__cpu_snprintf,
	.se_width_idx	= HISTC_CPU,
};

/* --sort parallelism */

static int64_t
sort__parallelism_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return right->parallelism - left->parallelism;
}

static int hist_entry__parallelism_filter(struct hist_entry *he, int type, const void *arg)
{
	const unsigned long *parallelism_filter = arg;

	if (type != HIST_FILTER__PARALLELISM)
		return -1;

	return test_bit(he->parallelism, parallelism_filter);
}

static int hist_entry__parallelism_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%*d", width, he->parallelism);
}

struct sort_entry sort_parallelism = {
	.se_header      = "Parallelism",
	.se_cmp	        = sort__parallelism_cmp,
	.se_filter	= hist_entry__parallelism_filter,
	.se_snprintf    = hist_entry__parallelism_snprintf,
	.se_width_idx	= HISTC_PARALLELISM,
};

/* --sort cgroup_id */

static int64_t _sort__cgroup_dev_cmp(u64 left_dev, u64 right_dev)
{
	return (int64_t)(right_dev - left_dev);
}

static int64_t _sort__cgroup_inode_cmp(u64 left_ino, u64 right_ino)
{
	return (int64_t)(right_ino - left_ino);
}

static int64_t
sort__cgroup_id_cmp(struct hist_entry *left, struct hist_entry *right)
{
	int64_t ret;

	ret = _sort__cgroup_dev_cmp(right->cgroup_id.dev, left->cgroup_id.dev);
	if (ret != 0)
		return ret;

	return _sort__cgroup_inode_cmp(right->cgroup_id.ino,
				       left->cgroup_id.ino);
}

static int hist_entry__cgroup_id_snprintf(struct hist_entry *he,
					  char *bf, size_t size,
					  unsigned int width __maybe_unused)
{
	return repsep_snprintf(bf, size, "%lu/0x%lx", he->cgroup_id.dev,
			       he->cgroup_id.ino);
}

struct sort_entry sort_cgroup_id = {
	.se_header      = "cgroup id (dev/inode)",
	.se_cmp	        = sort__cgroup_id_cmp,
	.se_snprintf    = hist_entry__cgroup_id_snprintf,
	.se_width_idx	= HISTC_CGROUP_ID,
};

/* --sort cgroup */

static int64_t
sort__cgroup_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return right->cgroup - left->cgroup;
}

static int hist_entry__cgroup_snprintf(struct hist_entry *he,
				       char *bf, size_t size,
				       unsigned int width __maybe_unused)
{
	const char *cgrp_name = "N/A";

	if (he->cgroup) {
		struct cgroup *cgrp = cgroup__find(maps__machine(he->ms.maps)->env,
						   he->cgroup);
		if (cgrp != NULL)
			cgrp_name = cgrp->name;
		else
			cgrp_name = "unknown";
	}

	return repsep_snprintf(bf, size, "%s", cgrp_name);
}

struct sort_entry sort_cgroup = {
	.se_header      = "Cgroup",
	.se_cmp	        = sort__cgroup_cmp,
	.se_snprintf    = hist_entry__cgroup_snprintf,
	.se_width_idx	= HISTC_CGROUP,
};

/* --sort socket */

static int64_t
sort__socket_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return right->socket - left->socket;
}

static int hist_entry__socket_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%*.*d", width, width-3, he->socket);
}

static int hist_entry__socket_filter(struct hist_entry *he, int type, const void *arg)
{
	int sk = *(const int *)arg;

	if (type != HIST_FILTER__SOCKET)
		return -1;

	return sk >= 0 && he->socket != sk;
}

struct sort_entry sort_socket = {
	.se_header      = "Socket",
	.se_cmp	        = sort__socket_cmp,
	.se_snprintf    = hist_entry__socket_snprintf,
	.se_filter      = hist_entry__socket_filter,
	.se_width_idx	= HISTC_SOCKET,
};

/* --sort time */

static int64_t
sort__time_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return right->time - left->time;
}

static int hist_entry__time_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	char he_time[32];

	if (symbol_conf.nanosecs)
		timestamp__scnprintf_nsec(he->time, he_time,
					  sizeof(he_time));
	else
		timestamp__scnprintf_usec(he->time, he_time,
					  sizeof(he_time));

	return repsep_snprintf(bf, size, "%-.*s", width, he_time);
}

struct sort_entry sort_time = {
	.se_header      = "Time",
	.se_cmp	        = sort__time_cmp,
	.se_snprintf    = hist_entry__time_snprintf,
	.se_width_idx	= HISTC_TIME,
};

/* --sort trace */

#ifdef HAVE_LIBTRACEEVENT
static char *get_trace_output(struct hist_entry *he)
{
	struct trace_seq seq;
	struct evsel *evsel;
	struct tep_record rec = {
		.data = he->raw_data,
		.size = he->raw_size,
	};
	struct tep_event *tp_format;

	evsel = hists_to_evsel(he->hists);

	trace_seq_init(&seq);
	tp_format = evsel__tp_format(evsel);
	if (tp_format) {
		if (symbol_conf.raw_trace)
			tep_print_fields(&seq, he->raw_data, he->raw_size, tp_format);
		else
			tep_print_event(tp_format->tep, &seq, &rec, "%s", TEP_PRINT_INFO);
	}

	/*
	 * Trim the buffer, it starts at 4KB and we're not going to
	 * add anything more to this buffer.
	 */
	return realloc(seq.buffer, seq.len + 1);
}

static int64_t
sort__trace_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct evsel *evsel;

	evsel = hists_to_evsel(left->hists);
	if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
		return 0;

	if (left->trace_output == NULL)
		left->trace_output = get_trace_output(left);
	if (right->trace_output == NULL)
		right->trace_output = get_trace_output(right);

	return strcmp(right->trace_output, left->trace_output);
}

static int hist_entry__trace_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	struct evsel *evsel;

	evsel = hists_to_evsel(he->hists);
	if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
		return scnprintf(bf, size, "%-.*s", width, "N/A");

	if (he->trace_output == NULL)
		he->trace_output = get_trace_output(he);
	return repsep_snprintf(bf, size, "%-.*s", width, he->trace_output);
}

struct sort_entry sort_trace = {
	.se_header      = "Trace output",
	.se_cmp	        = sort__trace_cmp,
	.se_snprintf    = hist_entry__trace_snprintf,
	.se_width_idx	= HISTC_TRACE,
};
#endif /* HAVE_LIBTRACEEVENT */

/* sort keys for branch stacks */

static int64_t
sort__dso_from_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	return _sort__dso_cmp(left->branch_info->from.ms.map,
			      right->branch_info->from.ms.map);
}

static int hist_entry__dso_from_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	if (he->branch_info)
		return _hist_entry__dso_snprintf(he->branch_info->from.ms.map,
						 bf, size, width);
	else
		return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int hist_entry__dso_from_filter(struct hist_entry *he, int type,
				       const void *arg)
{
	const struct dso *dso = arg;

	if (type != HIST_FILTER__DSO)
		return -1;

	return dso && (!he->branch_info || !he->branch_info->from.ms.map ||
		map__dso(he->branch_info->from.ms.map) != dso);
}

static int64_t
sort__dso_to_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	return _sort__dso_cmp(left->branch_info->to.ms.map,
			      right->branch_info->to.ms.map);
}

static int hist_entry__dso_to_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	if (he->branch_info)
		return _hist_entry__dso_snprintf(he->branch_info->to.ms.map,
						 bf, size, width);
	else
		return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int hist_entry__dso_to_filter(struct hist_entry *he, int type,
				     const void *arg)
{
	const struct dso *dso = arg;

	if (type != HIST_FILTER__DSO)
		return -1;

	return dso && (!he->branch_info || !he->branch_info->to.ms.map ||
		map__dso(he->branch_info->to.ms.map) != dso);
}

static int64_t
sort__sym_from_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct addr_map_symbol *from_l, *from_r;

	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	from_l = &left->branch_info->from;
	from_r = &right->branch_info->from;

	if (!from_l->ms.sym && !from_r->ms.sym)
		return _sort__addr_cmp(from_l->addr, from_r->addr);

	return _sort__sym_cmp(from_l->ms.sym, from_r->ms.sym);
}

static int64_t
sort__sym_to_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct addr_map_symbol *to_l, *to_r;

	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	to_l = &left->branch_info->to;
	to_r = &right->branch_info->to;

	if (!to_l->ms.sym && !to_r->ms.sym)
		return _sort__addr_cmp(to_l->addr, to_r->addr);

	return _sort__sym_cmp(to_l->ms.sym, to_r->ms.sym);
}

static int hist_entry__sym_from_snprintf(struct hist_entry *he, char *bf,
					 size_t size, unsigned int width)
{
	if (he->branch_info) {
		struct addr_map_symbol *from = &he->branch_info->from;

		return _hist_entry__sym_snprintf(&from->ms, from->al_addr,
						 from->al_level, bf, size, width);
	}

	return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int hist_entry__sym_to_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	if (he->branch_info) {
		struct addr_map_symbol *to = &he->branch_info->to;

		return _hist_entry__sym_snprintf(&to->ms, to->al_addr,
						 to->al_level, bf, size, width);
	}

	return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int hist_entry__sym_from_filter(struct hist_entry *he, int type,
				       const void *arg)
{
	const char *sym = arg;

	if (type != HIST_FILTER__SYMBOL)
		return -1;

	return sym && !(he->branch_info && he->branch_info->from.ms.sym &&
			strstr(he->branch_info->from.ms.sym->name, sym));
}

static int hist_entry__sym_to_filter(struct hist_entry *he, int type,
				       const void *arg)
{
	const char *sym = arg;

	if (type != HIST_FILTER__SYMBOL)
		return -1;

	return sym && !(he->branch_info && he->branch_info->to.ms.sym &&
		        strstr(he->branch_info->to.ms.sym->name, sym));
}

struct sort_entry sort_dso_from = {
	.se_header	= "Source Shared Object",
	.se_cmp		= sort__dso_from_cmp,
	.se_snprintf	= hist_entry__dso_from_snprintf,
	.se_filter	= hist_entry__dso_from_filter,
	.se_width_idx	= HISTC_DSO_FROM,
};

struct sort_entry sort_dso_to = {
	.se_header	= "Target Shared Object",
	.se_cmp		= sort__dso_to_cmp,
	.se_snprintf	= hist_entry__dso_to_snprintf,
	.se_filter	= hist_entry__dso_to_filter,
	.se_width_idx	= HISTC_DSO_TO,
};

struct sort_entry sort_sym_from = {
	.se_header	= "Source Symbol",
	.se_cmp		= sort__sym_from_cmp,
	.se_snprintf	= hist_entry__sym_from_snprintf,
	.se_filter	= hist_entry__sym_from_filter,
	.se_width_idx	= HISTC_SYMBOL_FROM,
};

struct sort_entry sort_sym_to = {
	.se_header	= "Target Symbol",
	.se_cmp		= sort__sym_to_cmp,
	.se_snprintf	= hist_entry__sym_to_snprintf,
	.se_filter	= hist_entry__sym_to_filter,
	.se_width_idx	= HISTC_SYMBOL_TO,
};

static int _hist_entry__addr_snprintf(struct map_symbol *ms,
				     u64 ip, char level, char *bf, size_t size,
				     unsigned int width)
{
	struct symbol *sym = ms->sym;
	struct map *map = ms->map;
	size_t ret = 0, offs;

	ret += repsep_snprintf(bf + ret, size - ret, "[%c] ", level);
	if (sym && map) {
		if (sym->type == STT_OBJECT) {
			ret += repsep_snprintf(bf + ret, size - ret, "%s", sym->name);
			ret += repsep_snprintf(bf + ret, size - ret, "+0x%llx",
					ip - map__unmap_ip(map, sym->start));
		} else {
			ret += repsep_snprintf(bf + ret, size - ret, "%.*s",
					       width - ret,
					       sym->name);
			offs = ip - sym->start;
			if (offs)
				ret += repsep_snprintf(bf + ret, size - ret, "+0x%llx", offs);
		}
	} else {
		size_t len = BITS_PER_LONG / 4;
		ret += repsep_snprintf(bf + ret, size - ret, "%-#.*llx",
				       len, ip);
	}

	return ret;
}

static int hist_entry__addr_from_snprintf(struct hist_entry *he, char *bf,
					 size_t size, unsigned int width)
{
	if (he->branch_info) {
		struct addr_map_symbol *from = &he->branch_info->from;

		return _hist_entry__addr_snprintf(&from->ms, from->al_addr,
						 he->level, bf, size, width);
	}

	return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int hist_entry__addr_to_snprintf(struct hist_entry *he, char *bf,
				       size_t size, unsigned int width)
{
	if (he->branch_info) {
		struct addr_map_symbol *to = &he->branch_info->to;

		return _hist_entry__addr_snprintf(&to->ms, to->al_addr,
						 he->level, bf, size, width);
	}

	return repsep_snprintf(bf, size, "%-*.*s", width, width, "N/A");
}

static int64_t
sort__addr_from_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct addr_map_symbol *from_l;
	struct addr_map_symbol *from_r;
	int64_t ret;

	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	from_l = &left->branch_info->from;
	from_r = &right->branch_info->from;

	/*
	 * comparing symbol address alone is not enough since it's a
	 * relative address within a dso.
	 */
	ret = _sort__dso_cmp(from_l->ms.map, from_r->ms.map);
	if (ret != 0)
		return ret;

	return _sort__addr_cmp(from_l->addr, from_r->addr);
}

static int64_t
sort__addr_to_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct addr_map_symbol *to_l;
	struct addr_map_symbol *to_r;
	int64_t ret;

	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	to_l = &left->branch_info->to;
	to_r = &right->branch_info->to;

	/*
	 * comparing symbol address alone is not enough since it's a
	 * relative address within a dso.
	 */
	ret = _sort__dso_cmp(to_l->ms.map, to_r->ms.map);
	if (ret != 0)
		return ret;

	return _sort__addr_cmp(to_l->addr, to_r->addr);
}

struct sort_entry sort_addr_from = {
	.se_header	= "Source Address",
	.se_cmp		= sort__addr_from_cmp,
	.se_snprintf	= hist_entry__addr_from_snprintf,
	.se_filter	= hist_entry__sym_from_filter, /* shared with sym_from */
	.se_width_idx	= HISTC_ADDR_FROM,
};

struct sort_entry sort_addr_to = {
	.se_header	= "Target Address",
	.se_cmp		= sort__addr_to_cmp,
	.se_snprintf	= hist_entry__addr_to_snprintf,
	.se_filter	= hist_entry__sym_to_filter, /* shared with sym_to */
	.se_width_idx	= HISTC_ADDR_TO,
};


static int64_t
sort__mispredict_cmp(struct hist_entry *left, struct hist_entry *right)
{
	unsigned char mp, p;

	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	mp = left->branch_info->flags.mispred != right->branch_info->flags.mispred;
	p  = left->branch_info->flags.predicted != right->branch_info->flags.predicted;
	return mp || p;
}

static int hist_entry__mispredict_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width){
	static const char *out = "N/A";

	if (he->branch_info) {
		if (he->branch_info->flags.predicted)
			out = "N";
		else if (he->branch_info->flags.mispred)
			out = "Y";
	}

	return repsep_snprintf(bf, size, "%-*.*s", width, width, out);
}

static int64_t
sort__cycles_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	return left->branch_info->flags.cycles -
		right->branch_info->flags.cycles;
}

static int hist_entry__cycles_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	if (!he->branch_info)
		return scnprintf(bf, size, "%-.*s", width, "N/A");
	if (he->branch_info->flags.cycles == 0)
		return repsep_snprintf(bf, size, "%-*s", width, "-");
	return repsep_snprintf(bf, size, "%-*hd", width,
			       he->branch_info->flags.cycles);
}

struct sort_entry sort_cycles = {
	.se_header	= "Basic Block Cycles",
	.se_cmp		= sort__cycles_cmp,
	.se_snprintf	= hist_entry__cycles_snprintf,
	.se_width_idx	= HISTC_CYCLES,
};

/* --sort daddr_sym */
int64_t
sort__daddr_cmp(struct hist_entry *left, struct hist_entry *right)
{
	uint64_t l = 0, r = 0;

	if (left->mem_info)
		l = mem_info__daddr(left->mem_info)->addr;
	if (right->mem_info)
		r = mem_info__daddr(right->mem_info)->addr;

	return (int64_t)(r - l);
}

static int hist_entry__daddr_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	uint64_t addr = 0;
	struct map_symbol *ms = NULL;

	if (he->mem_info) {
		addr = mem_info__daddr(he->mem_info)->addr;
		ms = &mem_info__daddr(he->mem_info)->ms;
	}
	return _hist_entry__sym_snprintf(ms, addr, he->level, bf, size, width);
}

int64_t
sort__iaddr_cmp(struct hist_entry *left, struct hist_entry *right)
{
	uint64_t l = 0, r = 0;

	if (left->mem_info)
		l = mem_info__iaddr(left->mem_info)->addr;
	if (right->mem_info)
		r = mem_info__iaddr(right->mem_info)->addr;

	return (int64_t)(r - l);
}

static int hist_entry__iaddr_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	uint64_t addr = 0;
	struct map_symbol *ms = NULL;

	if (he->mem_info) {
		addr = mem_info__iaddr(he->mem_info)->addr;
		ms   = &mem_info__iaddr(he->mem_info)->ms;
	}
	return _hist_entry__sym_snprintf(ms, addr, he->level, bf, size, width);
}

static int64_t
sort__dso_daddr_cmp(struct hist_entry *left, struct hist_entry *right)
{
	struct map *map_l = NULL;
	struct map *map_r = NULL;

	if (left->mem_info)
		map_l = mem_info__daddr(left->mem_info)->ms.map;
	if (right->mem_info)
		map_r = mem_info__daddr(right->mem_info)->ms.map;

	return _sort__dso_cmp(map_l, map_r);
}

static int hist_entry__dso_daddr_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	struct map *map = NULL;

	if (he->mem_info)
		map = mem_info__daddr(he->mem_info)->ms.map;

	return _hist_entry__dso_snprintf(map, bf, size, width);
}

static int64_t
sort__locked_cmp(struct hist_entry *left, struct hist_entry *right)
{
	union perf_mem_data_src data_src_l;
	union perf_mem_data_src data_src_r;

	if (left->mem_info)
		data_src_l = *mem_info__data_src(left->mem_info);
	else
		data_src_l.mem_lock = PERF_MEM_LOCK_NA;

	if (right->mem_info)
		data_src_r = *mem_info__data_src(right->mem_info);
	else
		data_src_r.mem_lock = PERF_MEM_LOCK_NA;

	return (int64_t)(data_src_r.mem_lock - data_src_l.mem_lock);
}

static int hist_entry__locked_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	char out[10];

	perf_mem__lck_scnprintf(out, sizeof(out), he->mem_info);
	return repsep_snprintf(bf, size, "%.*s", width, out);
}

static int64_t
sort__tlb_cmp(struct hist_entry *left, struct hist_entry *right)
{
	union perf_mem_data_src data_src_l;
	union perf_mem_data_src data_src_r;

	if (left->mem_info)
		data_src_l = *mem_info__data_src(left->mem_info);
	else
		data_src_l.mem_dtlb = PERF_MEM_TLB_NA;

	if (right->mem_info)
		data_src_r = *mem_info__data_src(right->mem_info);
	else
		data_src_r.mem_dtlb = PERF_MEM_TLB_NA;

	return (int64_t)(data_src_r.mem_dtlb - data_src_l.mem_dtlb);
}

static int hist_entry__tlb_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	char out[64];

	perf_mem__tlb_scnprintf(out, sizeof(out), he->mem_info);
	return repsep_snprintf(bf, size, "%-*s", width, out);
}

static int64_t
sort__lvl_cmp(struct hist_entry *left, struct hist_entry *right)
{
	union perf_mem_data_src data_src_l;
	union perf_mem_data_src data_src_r;

	if (left->mem_info)
		data_src_l = *mem_info__data_src(left->mem_info);
	else
		data_src_l.mem_lvl = PERF_MEM_LVL_NA;

	if (right->mem_info)
		data_src_r = *mem_info__data_src(right->mem_info);
	else
		data_src_r.mem_lvl = PERF_MEM_LVL_NA;

	return (int64_t)(data_src_r.mem_lvl - data_src_l.mem_lvl);
}

static int hist_entry__lvl_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	char out[64];

	perf_mem__lvl_scnprintf(out, sizeof(out), he->mem_info);
	return repsep_snprintf(bf, size, "%-*s", width, out);
}

static int64_t
sort__snoop_cmp(struct hist_entry *left, struct hist_entry *right)
{
	union perf_mem_data_src data_src_l;
	union perf_mem_data_src data_src_r;

	if (left->mem_info)
		data_src_l = *mem_info__data_src(left->mem_info);
	else
		data_src_l.mem_snoop = PERF_MEM_SNOOP_NA;

	if (right->mem_info)
		data_src_r = *mem_info__data_src(right->mem_info);
	else
		data_src_r.mem_snoop = PERF_MEM_SNOOP_NA;

	return (int64_t)(data_src_r.mem_snoop - data_src_l.mem_snoop);
}

static int hist_entry__snoop_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	char out[64];

	perf_mem__snp_scnprintf(out, sizeof(out), he->mem_info);
	return repsep_snprintf(bf, size, "%-*s", width, out);
}

int64_t
sort__dcacheline_cmp(struct hist_entry *left, struct hist_entry *right)
{
	u64 l, r;
	struct map *l_map, *r_map;
	struct dso *l_dso, *r_dso;
	int rc;

	if (!left->mem_info)  return -1;
	if (!right->mem_info) return 1;

	/* group event types together */
	if (left->cpumode > right->cpumode) return -1;
	if (left->cpumode < right->cpumode) return 1;

	l_map = mem_info__daddr(left->mem_info)->ms.map;
	r_map = mem_info__daddr(right->mem_info)->ms.map;

	/* if both are NULL, jump to sort on al_addr instead */
	if (!l_map && !r_map)
		goto addr;

	if (!l_map) return -1;
	if (!r_map) return 1;

	l_dso = map__dso(l_map);
	r_dso = map__dso(r_map);
	rc = dso__cmp_id(l_dso, r_dso);
	if (rc)
		return rc;
	/*
	 * Addresses with no major/minor numbers or build ID are assumed to be
	 * anonymous in userspace.  Sort those on pid then address.
	 *
	 * The kernel and non-zero major/minor mapped areas are
	 * assumed to be unity mapped.  Sort those on address.
	 */
	if (left->cpumode != PERF_RECORD_MISC_KERNEL && (map__flags(l_map) & MAP_SHARED) == 0) {
		const struct dso_id *dso_id = dso__id_const(l_dso);

		if (!dso_id->mmap2_valid)
			dso_id = dso__id_const(r_dso);

		if (!build_id__is_defined(&dso_id->build_id) &&
		    (!dso_id->mmap2_valid || (dso_id->maj == 0 && dso_id->min == 0))) {
			/* userspace anonymous */

			if (thread__pid(left->thread) > thread__pid(right->thread))
				return -1;
			if (thread__pid(left->thread) < thread__pid(right->thread))
				return 1;
		}
	}

addr:
	/* al_addr does all the right addr - start + offset calculations */
	l = cl_address(mem_info__daddr(left->mem_info)->al_addr, chk_double_cl);
	r = cl_address(mem_info__daddr(right->mem_info)->al_addr, chk_double_cl);

	if (l > r) return -1;
	if (l < r) return 1;

	return 0;
}

static int hist_entry__dcacheline_snprintf(struct hist_entry *he, char *bf,
					  size_t size, unsigned int width)
{

	uint64_t addr = 0;
	struct map_symbol *ms = NULL;
	char level = he->level;

	if (he->mem_info) {
		struct map *map = mem_info__daddr(he->mem_info)->ms.map;
		struct dso *dso = map ? map__dso(map) : NULL;
		const struct dso_id *dso_id = dso ? dso__id_const(dso) : &dso_id_empty;

		addr = cl_address(mem_info__daddr(he->mem_info)->al_addr, chk_double_cl);
		ms = &mem_info__daddr(he->mem_info)->ms;

		/* print [s] for shared data mmaps */
		if ((he->cpumode != PERF_RECORD_MISC_KERNEL) &&
		     map && !(map__prot(map) & PROT_EXEC) &&
		     (map__flags(map) & MAP_SHARED) &&
		     (!dso_id->mmap2_valid || (dso_id->maj == 0 && dso_id->min == 0)))
			level = 's';
		else if (!map)
			level = 'X';
	}
	return _hist_entry__sym_snprintf(ms, addr, level, bf, size, width);
}

struct sort_entry sort_mispredict = {
	.se_header	= "Branch Mispredicted",
	.se_cmp		= sort__mispredict_cmp,
	.se_snprintf	= hist_entry__mispredict_snprintf,
	.se_width_idx	= HISTC_MISPREDICT,
};

static int64_t
sort__weight_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->weight - right->weight;
}

static int hist_entry__local_weight_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*llu", width, he->weight);
}

struct sort_entry sort_local_weight = {
	.se_header	= "Local Weight",
	.se_cmp		= sort__weight_cmp,
	.se_snprintf	= hist_entry__local_weight_snprintf,
	.se_width_idx	= HISTC_LOCAL_WEIGHT,
};

static int hist_entry__global_weight_snprintf(struct hist_entry *he, char *bf,
					      size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*llu", width,
			       he->weight * he->stat.nr_events);
}

struct sort_entry sort_global_weight = {
	.se_header	= "Weight",
	.se_cmp		= sort__weight_cmp,
	.se_snprintf	= hist_entry__global_weight_snprintf,
	.se_width_idx	= HISTC_GLOBAL_WEIGHT,
};

static int64_t
sort__ins_lat_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->ins_lat - right->ins_lat;
}

static int hist_entry__local_ins_lat_snprintf(struct hist_entry *he, char *bf,
					      size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*u", width, he->ins_lat);
}

struct sort_entry sort_local_ins_lat = {
	.se_header	= "Local INSTR Latency",
	.se_cmp		= sort__ins_lat_cmp,
	.se_snprintf	= hist_entry__local_ins_lat_snprintf,
	.se_width_idx	= HISTC_LOCAL_INS_LAT,
};

static int hist_entry__global_ins_lat_snprintf(struct hist_entry *he, char *bf,
					       size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*u", width,
			       he->ins_lat * he->stat.nr_events);
}

struct sort_entry sort_global_ins_lat = {
	.se_header	= "INSTR Latency",
	.se_cmp		= sort__ins_lat_cmp,
	.se_snprintf	= hist_entry__global_ins_lat_snprintf,
	.se_width_idx	= HISTC_GLOBAL_INS_LAT,
};

static int64_t
sort__p_stage_cyc_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->weight3 - right->weight3;
}

static int hist_entry__global_p_stage_cyc_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*u", width, he->weight3 * he->stat.nr_events);
}


static int hist_entry__p_stage_cyc_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*u", width, he->weight3);
}

struct sort_entry sort_local_p_stage_cyc = {
	.se_header      = "Local Pipeline Stage Cycle",
	.se_cmp         = sort__p_stage_cyc_cmp,
	.se_snprintf	= hist_entry__p_stage_cyc_snprintf,
	.se_width_idx	= HISTC_LOCAL_P_STAGE_CYC,
};

struct sort_entry sort_global_p_stage_cyc = {
	.se_header      = "Pipeline Stage Cycle",
	.se_cmp         = sort__p_stage_cyc_cmp,
	.se_snprintf    = hist_entry__global_p_stage_cyc_snprintf,
	.se_width_idx   = HISTC_GLOBAL_P_STAGE_CYC,
};

struct sort_entry sort_mem_daddr_sym = {
	.se_header	= "Data Symbol",
	.se_cmp		= sort__daddr_cmp,
	.se_snprintf	= hist_entry__daddr_snprintf,
	.se_width_idx	= HISTC_MEM_DADDR_SYMBOL,
};

struct sort_entry sort_mem_iaddr_sym = {
	.se_header	= "Code Symbol",
	.se_cmp		= sort__iaddr_cmp,
	.se_snprintf	= hist_entry__iaddr_snprintf,
	.se_width_idx	= HISTC_MEM_IADDR_SYMBOL,
};

struct sort_entry sort_mem_daddr_dso = {
	.se_header	= "Data Object",
	.se_cmp		= sort__dso_daddr_cmp,
	.se_snprintf	= hist_entry__dso_daddr_snprintf,
	.se_width_idx	= HISTC_MEM_DADDR_DSO,
};

struct sort_entry sort_mem_locked = {
	.se_header	= "Locked",
	.se_cmp		= sort__locked_cmp,
	.se_snprintf	= hist_entry__locked_snprintf,
	.se_width_idx	= HISTC_MEM_LOCKED,
};

struct sort_entry sort_mem_tlb = {
	.se_header	= "TLB access",
	.se_cmp		= sort__tlb_cmp,
	.se_snprintf	= hist_entry__tlb_snprintf,
	.se_width_idx	= HISTC_MEM_TLB,
};

struct sort_entry sort_mem_lvl = {
	.se_header	= "Memory access",
	.se_cmp		= sort__lvl_cmp,
	.se_snprintf	= hist_entry__lvl_snprintf,
	.se_width_idx	= HISTC_MEM_LVL,
};

struct sort_entry sort_mem_snoop = {
	.se_header	= "Snoop",
	.se_cmp		= sort__snoop_cmp,
	.se_snprintf	= hist_entry__snoop_snprintf,
	.se_width_idx	= HISTC_MEM_SNOOP,
};

struct sort_entry sort_mem_dcacheline = {
	.se_header	= "Data Cacheline",
	.se_cmp		= sort__dcacheline_cmp,
	.se_snprintf	= hist_entry__dcacheline_snprintf,
	.se_width_idx	= HISTC_MEM_DCACHELINE,
};

static int64_t
sort__blocked_cmp(struct hist_entry *left, struct hist_entry *right)
{
	union perf_mem_data_src data_src_l;
	union perf_mem_data_src data_src_r;

	if (left->mem_info)
		data_src_l = *mem_info__data_src(left->mem_info);
	else
		data_src_l.mem_blk = PERF_MEM_BLK_NA;

	if (right->mem_info)
		data_src_r = *mem_info__data_src(right->mem_info);
	else
		data_src_r.mem_blk = PERF_MEM_BLK_NA;

	return (int64_t)(data_src_r.mem_blk - data_src_l.mem_blk);
}

static int hist_entry__blocked_snprintf(struct hist_entry *he, char *bf,
					size_t size, unsigned int width)
{
	char out[16];

	perf_mem__blk_scnprintf(out, sizeof(out), he->mem_info);
	return repsep_snprintf(bf, size, "%.*s", width, out);
}

struct sort_entry sort_mem_blocked = {
	.se_header	= "Blocked",
	.se_cmp		= sort__blocked_cmp,
	.se_snprintf	= hist_entry__blocked_snprintf,
	.se_width_idx	= HISTC_MEM_BLOCKED,
};

static int64_t
sort__phys_daddr_cmp(struct hist_entry *left, struct hist_entry *right)
{
	uint64_t l = 0, r = 0;

	if (left->mem_info)
		l = mem_info__daddr(left->mem_info)->phys_addr;
	if (right->mem_info)
		r = mem_info__daddr(right->mem_info)->phys_addr;

	return (int64_t)(r - l);
}

static int hist_entry__phys_daddr_snprintf(struct hist_entry *he, char *bf,
					   size_t size, unsigned int width)
{
	uint64_t addr = 0;
	size_t ret = 0;
	size_t len = BITS_PER_LONG / 4;

	addr = mem_info__daddr(he->mem_info)->phys_addr;

	ret += repsep_snprintf(bf + ret, size - ret, "[%c] ", he->level);

	ret += repsep_snprintf(bf + ret, size - ret, "%-#.*llx", len, addr);

	ret += repsep_snprintf(bf + ret, size - ret, "%-*s", width - ret, "");

	if (ret > width)
		bf[width] = '\0';

	return width;
}

struct sort_entry sort_mem_phys_daddr = {
	.se_header	= "Data Physical Address",
	.se_cmp		= sort__phys_daddr_cmp,
	.se_snprintf	= hist_entry__phys_daddr_snprintf,
	.se_width_idx	= HISTC_MEM_PHYS_DADDR,
};

static int64_t
sort__data_page_size_cmp(struct hist_entry *left, struct hist_entry *right)
{
	uint64_t l = 0, r = 0;

	if (left->mem_info)
		l = mem_info__daddr(left->mem_info)->data_page_size;
	if (right->mem_info)
		r = mem_info__daddr(right->mem_info)->data_page_size;

	return (int64_t)(r - l);
}

static int hist_entry__data_page_size_snprintf(struct hist_entry *he, char *bf,
					  size_t size, unsigned int width)
{
	char str[PAGE_SIZE_NAME_LEN];

	return repsep_snprintf(bf, size, "%-*s", width,
			get_page_size_name(mem_info__daddr(he->mem_info)->data_page_size, str));
}

struct sort_entry sort_mem_data_page_size = {
	.se_header	= "Data Page Size",
	.se_cmp		= sort__data_page_size_cmp,
	.se_snprintf	= hist_entry__data_page_size_snprintf,
	.se_width_idx	= HISTC_MEM_DATA_PAGE_SIZE,
};

static int64_t
sort__code_page_size_cmp(struct hist_entry *left, struct hist_entry *right)
{
	uint64_t l = left->code_page_size;
	uint64_t r = right->code_page_size;

	return (int64_t)(r - l);
}

static int hist_entry__code_page_size_snprintf(struct hist_entry *he, char *bf,
					  size_t size, unsigned int width)
{
	char str[PAGE_SIZE_NAME_LEN];

	return repsep_snprintf(bf, size, "%-*s", width,
			       get_page_size_name(he->code_page_size, str));
}

struct sort_entry sort_code_page_size = {
	.se_header	= "Code Page Size",
	.se_cmp		= sort__code_page_size_cmp,
	.se_snprintf	= hist_entry__code_page_size_snprintf,
	.se_width_idx	= HISTC_CODE_PAGE_SIZE,
};

static int64_t
sort__abort_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	return left->branch_info->flags.abort !=
		right->branch_info->flags.abort;
}

static int hist_entry__abort_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	static const char *out = "N/A";

	if (he->branch_info) {
		if (he->branch_info->flags.abort)
			out = "A";
		else
			out = ".";
	}

	return repsep_snprintf(bf, size, "%-*s", width, out);
}

struct sort_entry sort_abort = {
	.se_header	= "Transaction abort",
	.se_cmp		= sort__abort_cmp,
	.se_snprintf	= hist_entry__abort_snprintf,
	.se_width_idx	= HISTC_ABORT,
};

static int64_t
sort__in_tx_cmp(struct hist_entry *left, struct hist_entry *right)
{
	if (!left->branch_info || !right->branch_info)
		return cmp_null(left->branch_info, right->branch_info);

	return left->branch_info->flags.in_tx !=
		right->branch_info->flags.in_tx;
}

static int hist_entry__in_tx_snprintf(struct hist_entry *he, char *bf,
				    size_t size, unsigned int width)
{
	static const char *out = "N/A";

	if (he->branch_info) {
		if (he->branch_info->flags.in_tx)
			out = "T";
		else
			out = ".";
	}

	return repsep_snprintf(bf, size, "%-*s", width, out);
}

struct sort_entry sort_in_tx = {
	.se_header	= "Branch in transaction",
	.se_cmp		= sort__in_tx_cmp,
	.se_snprintf	= hist_entry__in_tx_snprintf,
	.se_width_idx	= HISTC_IN_TX,
};

static int64_t
sort__transaction_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return left->transaction - right->transaction;
}

static inline char *add_str(char *p, const char *str)
{
	strcpy(p, str);
	return p + strlen(str);
}

static struct txbit {
	unsigned flag;
	const char *name;
	int skip_for_len;
} txbits[] = {
	{ PERF_TXN_ELISION,        "EL ",        0 },
	{ PERF_TXN_TRANSACTION,    "TX ",        1 },
	{ PERF_TXN_SYNC,           "SYNC ",      1 },
	{ PERF_TXN_ASYNC,          "ASYNC ",     0 },
	{ PERF_TXN_RETRY,          "RETRY ",     0 },
	{ PERF_TXN_CONFLICT,       "CON ",       0 },
	{ PERF_TXN_CAPACITY_WRITE, "CAP-WRITE ", 1 },
	{ PERF_TXN_CAPACITY_READ,  "CAP-READ ",  0 },
	{ 0, NULL, 0 }
};

int hist_entry__transaction_len(void)
{
	int i;
	int len = 0;

	for (i = 0; txbits[i].name; i++) {
		if (!txbits[i].skip_for_len)
			len += strlen(txbits[i].name);
	}
	len += 4; /* :XX<space> */
	return len;
}

static int hist_entry__transaction_snprintf(struct hist_entry *he, char *bf,
					    size_t size, unsigned int width)
{
	u64 t = he->transaction;
	char buf[128];
	char *p = buf;
	int i;

	buf[0] = 0;
	for (i = 0; txbits[i].name; i++)
		if (txbits[i].flag & t)
			p = add_str(p, txbits[i].name);
	if (t && !(t & (PERF_TXN_SYNC|PERF_TXN_ASYNC)))
		p = add_str(p, "NEITHER ");
	if (t & PERF_TXN_ABORT_MASK) {
		sprintf(p, ":%" PRIx64,
			(t & PERF_TXN_ABORT_MASK) >>
			PERF_TXN_ABORT_SHIFT);
		p += strlen(p);
	}

	return repsep_snprintf(bf, size, "%-*s", width, buf);
}

struct sort_entry sort_transaction = {
	.se_header	= "Transaction                ",
	.se_cmp		= sort__transaction_cmp,
	.se_snprintf	= hist_entry__transaction_snprintf,
	.se_width_idx	= HISTC_TRANSACTION,
};

/* --sort symbol_size */

static int64_t _sort__sym_size_cmp(struct symbol *sym_l, struct symbol *sym_r)
{
	int64_t size_l = sym_l != NULL ? symbol__size(sym_l) : 0;
	int64_t size_r = sym_r != NULL ? symbol__size(sym_r) : 0;

	return size_l < size_r ? -1 :
		size_l == size_r ? 0 : 1;
}

static int64_t
sort__sym_size_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return _sort__sym_size_cmp(right->ms.sym, left->ms.sym);
}

static int _hist_entry__sym_size_snprintf(struct symbol *sym, char *bf,
					  size_t bf_size, unsigned int width)
{
	if (sym)
		return repsep_snprintf(bf, bf_size, "%*d", width, symbol__size(sym));

	return repsep_snprintf(bf, bf_size, "%*s", width, "unknown");
}

static int hist_entry__sym_size_snprintf(struct hist_entry *he, char *bf,
					 size_t size, unsigned int width)
{
	return _hist_entry__sym_size_snprintf(he->ms.sym, bf, size, width);
}

struct sort_entry sort_sym_size = {
	.se_header	= "Symbol size",
	.se_cmp		= sort__sym_size_cmp,
	.se_snprintf	= hist_entry__sym_size_snprintf,
	.se_width_idx	= HISTC_SYM_SIZE,
};

/* --sort dso_size */

static int64_t _sort__dso_size_cmp(struct map *map_l, struct map *map_r)
{
	int64_t size_l = map_l != NULL ? map__size(map_l) : 0;
	int64_t size_r = map_r != NULL ? map__size(map_r) : 0;

	return size_l < size_r ? -1 :
		size_l == size_r ? 0 : 1;
}

static int64_t
sort__dso_size_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return _sort__dso_size_cmp(right->ms.map, left->ms.map);
}

static int _hist_entry__dso_size_snprintf(struct map *map, char *bf,
					  size_t bf_size, unsigned int width)
{
	if (map && map__dso(map))
		return repsep_snprintf(bf, bf_size, "%*d", width, map__size(map));

	return repsep_snprintf(bf, bf_size, "%*s", width, "unknown");
}

static int hist_entry__dso_size_snprintf(struct hist_entry *he, char *bf,
					 size_t size, unsigned int width)
{
	return _hist_entry__dso_size_snprintf(he->ms.map, bf, size, width);
}

struct sort_entry sort_dso_size = {
	.se_header	= "DSO size",
	.se_cmp		= sort__dso_size_cmp,
	.se_snprintf	= hist_entry__dso_size_snprintf,
	.se_width_idx	= HISTC_DSO_SIZE,
};

/* --sort addr */

static int64_t
sort__addr_cmp(struct hist_entry *left, struct hist_entry *right)
{
	u64 left_ip = left->ip;
	u64 right_ip = right->ip;
	struct map *left_map = left->ms.map;
	struct map *right_map = right->ms.map;

	if (left_map)
		left_ip = map__unmap_ip(left_map, left_ip);
	if (right_map)
		right_ip = map__unmap_ip(right_map, right_ip);

	return _sort__addr_cmp(left_ip, right_ip);
}

static int hist_entry__addr_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width)
{
	u64 ip = he->ip;
	struct map *map = he->ms.map;

	if (map)
		ip = map__unmap_ip(map, ip);

	return repsep_snprintf(bf, size, "%-#*llx", width, ip);
}

struct sort_entry sort_addr = {
	.se_header	= "Address",
	.se_cmp		= sort__addr_cmp,
	.se_snprintf	= hist_entry__addr_snprintf,
	.se_width_idx	= HISTC_ADDR,
};

/* --sort type */

struct annotated_data_type unknown_type = {
	.self = {
		.type_name = (char *)"(unknown)",
		.children = LIST_HEAD_INIT(unknown_type.self.children),
	},
};

static int64_t
sort__type_cmp(struct hist_entry *left, struct hist_entry *right)
{
	return sort__addr_cmp(left, right);
}

static void sort__type_init(struct hist_entry *he)
{
	if (he->mem_type)
		return;

	he->mem_type = hist_entry__get_data_type(he);
	if (he->mem_type == NULL) {
		he->mem_type = &unknown_type;
		he->mem_type_off = 0;
	}
}

static int64_t
sort__type_collapse(struct hist_entry *left, struct hist_entry *right)
{
	struct annotated_data_type *left_type = left->mem_type;
	struct annotated_data_type *right_type = right->mem_type;

	if (!left_type) {
		sort__type_init(left);
		left_type = left->mem_type;
	}

	if (!right_type) {
		sort__type_init(right);
		right_type = right->mem_type;
	}

	return strcmp(left_type->self.type_name, right_type->self.type_name);
}

static int64_t
sort__type_sort(struct hist_entry *left, struct hist_entry *right)
{
	return sort__type_collapse(left, right);
}

static int hist_entry__type_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width)
{
	return repsep_snprintf(bf, size, "%-*s", width, he->mem_type->self.type_name);
}

struct sort_entry sort_type = {
	.se_header	= "Data Type",
	.se_cmp		= sort__type_cmp,
	.se_collapse	= sort__type_collapse,
	.se_sort	= sort__type_sort,
	.se_init	= sort__type_init,
	.se_snprintf	= hist_entry__type_snprintf,
	.se_width_idx	= HISTC_TYPE,
};

/* --sort typeoff */

static int64_t
sort__typeoff_sort(struct hist_entry *left, struct hist_entry *right)
{
	struct annotated_data_type *left_type = left->mem_type;
	struct annotated_data_type *right_type = right->mem_type;
	int64_t ret;

	if (!left_type) {
		sort__type_init(left);
		left_type = left->mem_type;
	}

	if (!right_type) {
		sort__type_init(right);
		right_type = right->mem_type;
	}

	ret = strcmp(left_type->self.type_name, right_type->self.type_name);
	if (ret)
		return ret;
	return left->mem_type_off - right->mem_type_off;
}

static int hist_entry__typeoff_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width __maybe_unused)
{
	struct annotated_data_type *he_type = he->mem_type;
	char buf[4096];

	if (he_type == &unknown_type || he_type == &stackop_type ||
	    he_type == &canary_type)
		return repsep_snprintf(bf, size, "%s", he_type->self.type_name);

	if (!annotated_data_type__get_member_name(he_type, buf, sizeof(buf),
						  he->mem_type_off))
		scnprintf(buf, sizeof(buf), "no field");

	return repsep_snprintf(bf, size, "%s +%#x (%s)", he_type->self.type_name,
			       he->mem_type_off, buf);
}

struct sort_entry sort_type_offset = {
	.se_header	= "Data Type Offset",
	.se_cmp		= sort__type_cmp,
	.se_collapse	= sort__typeoff_sort,
	.se_sort	= sort__typeoff_sort,
	.se_init	= sort__type_init,
	.se_snprintf	= hist_entry__typeoff_snprintf,
	.se_width_idx	= HISTC_TYPE_OFFSET,
};

/* --sort typecln */

/* TODO: use actual value in the system */
#define TYPE_CACHELINE_SIZE  64

static int64_t
sort__typecln_sort(struct hist_entry *left, struct hist_entry *right)
{
	struct annotated_data_type *left_type = left->mem_type;
	struct annotated_data_type *right_type = right->mem_type;
	int64_t left_cln, right_cln;
	int64_t ret;

	if (!left_type) {
		sort__type_init(left);
		left_type = left->mem_type;
	}

	if (!right_type) {
		sort__type_init(right);
		right_type = right->mem_type;
	}

	ret = strcmp(left_type->self.type_name, right_type->self.type_name);
	if (ret)
		return ret;

	left_cln = left->mem_type_off / TYPE_CACHELINE_SIZE;
	right_cln = right->mem_type_off / TYPE_CACHELINE_SIZE;
	return left_cln - right_cln;
}

static int hist_entry__typecln_snprintf(struct hist_entry *he, char *bf,
				     size_t size, unsigned int width __maybe_unused)
{
	struct annotated_data_type *he_type = he->mem_type;

	return repsep_snprintf(bf, size, "%s: cache-line %d", he_type->self.type_name,
			       he->mem_type_off / TYPE_CACHELINE_SIZE);
}

struct sort_entry sort_type_cacheline = {
	.se_header	= "Data Type Cacheline",
	.se_cmp		= sort__type_cmp,
	.se_collapse	= sort__typecln_sort,
	.se_sort	= sort__typecln_sort,
	.se_init	= sort__type_init,
	.se_snprintf	= hist_entry__typecln_snprintf,
	.se_width_idx	= HISTC_TYPE_CACHELINE,
};


struct sort_dimension {
	const char		*name;
	struct sort_entry	*entry;
	int			taken;
};

static int arch_support_sort_key(const char *sort_key, struct perf_env *env)
{
	const char *arch = perf_env__arch(env);

	if (!strcmp("x86", arch) || !strcmp("powerpc", arch)) {
		if (!strcmp(sort_key, "p_stage_cyc"))
			return 1;
		if (!strcmp(sort_key, "local_p_stage_cyc"))
			return 1;
	}
	return 0;
}

static const char *arch_perf_header_entry(const char *se_header, struct perf_env *env)
{
	const char *arch = perf_env__arch(env);

	if (!strcmp("x86", arch)) {
		if (!strcmp(se_header, "Local Pipeline Stage Cycle"))
			return "Local Retire Latency";
		else if (!strcmp(se_header, "Pipeline Stage Cycle"))
			return "Retire Latency";
	} else if (!strcmp("powerpc", arch)) {
		if (!strcmp(se_header, "Local INSTR Latency"))
			return "Finish Cyc";
		else if (!strcmp(se_header, "INSTR Latency"))
			return "Global Finish_cyc";
		else if (!strcmp(se_header, "Local Pipeline Stage Cycle"))
			return "Dispatch Cyc";
		else if (!strcmp(se_header, "Pipeline Stage Cycle"))
			return "Global Dispatch_cyc";
	}
	return se_header;
}

static void sort_dimension_add_dynamic_header(struct sort_dimension *sd, struct perf_env *env)
{
	sd->entry->se_header = arch_perf_header_entry(sd->entry->se_header, env);
}

#define DIM(d, n, func) [d] = { .name = n, .entry = &(func) }

static struct sort_dimension common_sort_dimensions[] = {
	DIM(SORT_PID, "pid", sort_thread),
	DIM(SORT_TGID, "tgid", sort_tgid),
	DIM(SORT_COMM, "comm", sort_comm),
	DIM(SORT_DSO, "dso", sort_dso),
	DIM(SORT_SYM, "symbol", sort_sym),
	DIM(SORT_PARENT, "parent", sort_parent),
	DIM(SORT_CPU, "cpu", sort_cpu),
	DIM(SORT_SOCKET, "socket", sort_socket),
	DIM(SORT_SRCLINE, "srcline", sort_srcline),
	DIM(SORT_SRCFILE, "srcfile", sort_srcfile),
	DIM(SORT_LOCAL_WEIGHT, "local_weight", sort_local_weight),
	DIM(SORT_GLOBAL_WEIGHT, "weight", sort_global_weight),
	DIM(SORT_TRANSACTION, "transaction", sort_transaction),
#ifdef HAVE_LIBTRACEEVENT
	DIM(SORT_TRACE, "trace", sort_trace),
#endif
	DIM(SORT_SYM_SIZE, "symbol_size", sort_sym_size),
	DIM(SORT_DSO_SIZE, "dso_size", sort_dso_size),
	DIM(SORT_CGROUP, "cgroup", sort_cgroup),
	DIM(SORT_CGROUP_ID, "cgroup_id", sort_cgroup_id),
	DIM(SORT_SYM_IPC_NULL, "ipc_null", sort_sym_ipc_null),
	DIM(SORT_TIME, "time", sort_time),
	DIM(SORT_CODE_PAGE_SIZE, "code_page_size", sort_code_page_size),
	DIM(SORT_LOCAL_INS_LAT, "local_ins_lat", sort_local_ins_lat),
	DIM(SORT_GLOBAL_INS_LAT, "ins_lat", sort_global_ins_lat),
	DIM(SORT_LOCAL_PIPELINE_STAGE_CYC, "local_p_stage_cyc", sort_local_p_stage_cyc),
	DIM(SORT_GLOBAL_PIPELINE_STAGE_CYC, "p_stage_cyc", sort_global_p_stage_cyc),
	DIM(SORT_ADDR, "addr", sort_addr),
	DIM(SORT_LOCAL_RETIRE_LAT, "local_retire_lat", sort_local_p_stage_cyc),
	DIM(SORT_GLOBAL_RETIRE_LAT, "retire_lat", sort_global_p_stage_cyc),
	DIM(SORT_SIMD, "simd", sort_simd),
	DIM(SORT_ANNOTATE_DATA_TYPE, "type", sort_type),
	DIM(SORT_ANNOTATE_DATA_TYPE_OFFSET, "typeoff", sort_type_offset),
	DIM(SORT_SYM_OFFSET, "symoff", sort_sym_offset),
	DIM(SORT_ANNOTATE_DATA_TYPE_CACHELINE, "typecln", sort_type_cacheline),
	DIM(SORT_PARALLELISM, "parallelism", sort_parallelism),
};

#undef DIM

#define DIM(d, n, func) [d - __SORT_BRANCH_STACK] = { .name = n, .entry = &(func) }

static struct sort_dimension bstack_sort_dimensions[] = {
	DIM(SORT_DSO_FROM, "dso_from", sort_dso_from),
	DIM(SORT_DSO_TO, "dso_to", sort_dso_to),
	DIM(SORT_SYM_FROM, "symbol_from", sort_sym_from),
	DIM(SORT_SYM_TO, "symbol_to", sort_sym_to),
	DIM(SORT_MISPREDICT, "mispredict", sort_mispredict),
	DIM(SORT_IN_TX, "in_tx", sort_in_tx),
	DIM(SORT_ABORT, "abort", sort_abort),
	DIM(SORT_CYCLES, "cycles", sort_cycles),
	DIM(SORT_SRCLINE_FROM, "srcline_from", sort_srcline_from),
	DIM(SORT_SRCLINE_TO, "srcline_to", sort_srcline_to),
	DIM(SORT_SYM_IPC, "ipc_lbr", sort_sym_ipc),
	DIM(SORT_ADDR_FROM, "addr_from", sort_addr_from),
	DIM(SORT_ADDR_TO, "addr_to", sort_addr_to),
	DIM(SORT_CALLCHAIN_BRANCH_PREDICTED,
		"callchain_branch_predicted",
		sort_callchain_branch_predicted),
	DIM(SORT_CALLCHAIN_BRANCH_ABORT,
		"callchain_branch_abort",
		sort_callchain_branch_abort),
	DIM(SORT_CALLCHAIN_BRANCH_CYCLES,
		"callchain_branch_cycles",
		sort_callchain_branch_cycles)
};

#undef DIM

#define DIM(d, n, func) [d - __SORT_MEMORY_MODE] = { .name = n, .entry = &(func) }

static struct sort_dimension memory_sort_dimensions[] = {
	DIM(SORT_MEM_DADDR_SYMBOL, "symbol_daddr", sort_mem_daddr_sym),
	DIM(SORT_MEM_IADDR_SYMBOL, "symbol_iaddr", sort_mem_iaddr_sym),
	DIM(SORT_MEM_DADDR_DSO, "dso_daddr", sort_mem_daddr_dso),
	DIM(SORT_MEM_LOCKED, "locked", sort_mem_locked),
	DIM(SORT_MEM_TLB, "tlb", sort_mem_tlb),
	DIM(SORT_MEM_LVL, "mem", sort_mem_lvl),
	DIM(SORT_MEM_SNOOP, "snoop", sort_mem_snoop),
	DIM(SORT_MEM_DCACHELINE, "dcacheline", sort_mem_dcacheline),
	DIM(SORT_MEM_PHYS_DADDR, "phys_daddr", sort_mem_phys_daddr),
	DIM(SORT_MEM_DATA_PAGE_SIZE, "data_page_size", sort_mem_data_page_size),
	DIM(SORT_MEM_BLOCKED, "blocked", sort_mem_blocked),
};

#undef DIM

struct hpp_dimension {
	const char		*name;
	struct perf_hpp_fmt	*fmt;
	int			taken;
	int			was_taken;
	int			mem_mode;
};

#define DIM(d, n) { .name = n, .fmt = &perf_hpp__format[d], }
#define DIM_MEM(d, n) { .name = n, .fmt = &perf_hpp__format[d], .mem_mode = 1, }

static struct hpp_dimension hpp_sort_dimensions[] = {
	DIM(PERF_HPP__OVERHEAD, "overhead"),
	DIM(PERF_HPP__LATENCY, "latency"),
	DIM(PERF_HPP__OVERHEAD_SYS, "overhead_sys"),
	DIM(PERF_HPP__OVERHEAD_US, "overhead_us"),
	DIM(PERF_HPP__OVERHEAD_GUEST_SYS, "overhead_guest_sys"),
	DIM(PERF_HPP__OVERHEAD_GUEST_US, "overhead_guest_us"),
	DIM(PERF_HPP__OVERHEAD_ACC, "overhead_children"),
	DIM(PERF_HPP__LATENCY_ACC, "latency_children"),
	DIM(PERF_HPP__SAMPLES, "sample"),
	DIM(PERF_HPP__PERIOD, "period"),
	DIM(PERF_HPP__WEIGHT1, "weight1"),
	DIM(PERF_HPP__WEIGHT2, "weight2"),
	DIM(PERF_HPP__WEIGHT3, "weight3"),
	/* aliases for weight_struct */
	DIM(PERF_HPP__WEIGHT2, "ins_lat"),
	DIM(PERF_HPP__WEIGHT3, "retire_lat"),
	DIM(PERF_HPP__WEIGHT3, "p_stage_cyc"),
	/* used for output only when SORT_MODE__MEM */
	DIM_MEM(PERF_HPP__MEM_STAT_OP, "op"),
	DIM_MEM(PERF_HPP__MEM_STAT_CACHE, "cache"),
	DIM_MEM(PERF_HPP__MEM_STAT_MEMORY, "memory"),
	DIM_MEM(PERF_HPP__MEM_STAT_SNOOP, "snoop"),
	DIM_MEM(PERF_HPP__MEM_STAT_DTLB, "dtlb"),
};

#undef DIM_MEM
#undef DIM

struct hpp_sort_entry {
	struct perf_hpp_fmt hpp;
	struct sort_entry *se;
};

void perf_hpp__reset_sort_width(struct perf_hpp_fmt *fmt, struct hists *hists)
{
	struct hpp_sort_entry *hse;

	if (!perf_hpp__is_sort_entry(fmt))
		return;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);
	hists__new_col_len(hists, hse->se->se_width_idx, strlen(fmt->name));
}

static int __sort__hpp_header(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			      struct hists *hists, int line,
			      int *span __maybe_unused)
{
	struct hpp_sort_entry *hse;
	size_t len = fmt->user_len;
	const char *hdr = "";

	if (line == hists->hpp_list->nr_header_lines - 1)
		hdr = fmt->name;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);

	if (!len)
		len = hists__col_len(hists, hse->se->se_width_idx);

	return scnprintf(hpp->buf, hpp->size, "%-*.*s", len, len, hdr);
}

static int __sort__hpp_width(struct perf_hpp_fmt *fmt,
			     struct perf_hpp *hpp __maybe_unused,
			     struct hists *hists)
{
	struct hpp_sort_entry *hse;
	size_t len = fmt->user_len;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);

	if (!len)
		len = hists__col_len(hists, hse->se->se_width_idx);

	return len;
}

static int __sort__hpp_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			     struct hist_entry *he)
{
	struct hpp_sort_entry *hse;
	size_t len = fmt->user_len;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);

	if (!len)
		len = hists__col_len(he->hists, hse->se->se_width_idx);

	return hse->se->se_snprintf(he, hpp->buf, hpp->size, len);
}

static int64_t __sort__hpp_cmp(struct perf_hpp_fmt *fmt,
			       struct hist_entry *a, struct hist_entry *b)
{
	struct hpp_sort_entry *hse;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);
	return hse->se->se_cmp(a, b);
}

static int64_t __sort__hpp_collapse(struct perf_hpp_fmt *fmt,
				    struct hist_entry *a, struct hist_entry *b)
{
	struct hpp_sort_entry *hse;
	int64_t (*collapse_fn)(struct hist_entry *, struct hist_entry *);

	hse = container_of(fmt, struct hpp_sort_entry, hpp);
	collapse_fn = hse->se->se_collapse ?: hse->se->se_cmp;
	return collapse_fn(a, b);
}

static int64_t __sort__hpp_sort(struct perf_hpp_fmt *fmt,
				struct hist_entry *a, struct hist_entry *b)
{
	struct hpp_sort_entry *hse;
	int64_t (*sort_fn)(struct hist_entry *, struct hist_entry *);

	hse = container_of(fmt, struct hpp_sort_entry, hpp);
	sort_fn = hse->se->se_sort ?: hse->se->se_cmp;
	return sort_fn(a, b);
}

bool perf_hpp__is_sort_entry(struct perf_hpp_fmt *format)
{
	return format->header == __sort__hpp_header;
}

#define MK_SORT_ENTRY_CHK(key)					\
bool perf_hpp__is_ ## key ## _entry(struct perf_hpp_fmt *fmt)	\
{								\
	struct hpp_sort_entry *hse;				\
								\
	if (!perf_hpp__is_sort_entry(fmt))			\
		return false;					\
								\
	hse = container_of(fmt, struct hpp_sort_entry, hpp);	\
	return hse->se == &sort_ ## key ;			\
}

#ifdef HAVE_LIBTRACEEVENT
MK_SORT_ENTRY_CHK(trace)
#else
bool perf_hpp__is_trace_entry(struct perf_hpp_fmt *fmt __maybe_unused)
{
	return false;
}
#endif
MK_SORT_ENTRY_CHK(srcline)
MK_SORT_ENTRY_CHK(srcfile)
MK_SORT_ENTRY_CHK(thread)
MK_SORT_ENTRY_CHK(comm)
MK_SORT_ENTRY_CHK(dso)
MK_SORT_ENTRY_CHK(sym)
MK_SORT_ENTRY_CHK(parallelism)


static bool __sort__hpp_equal(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b)
{
	struct hpp_sort_entry *hse_a;
	struct hpp_sort_entry *hse_b;

	if (!perf_hpp__is_sort_entry(a) || !perf_hpp__is_sort_entry(b))
		return false;

	hse_a = container_of(a, struct hpp_sort_entry, hpp);
	hse_b = container_of(b, struct hpp_sort_entry, hpp);

	return hse_a->se == hse_b->se;
}

static void hse_free(struct perf_hpp_fmt *fmt)
{
	struct hpp_sort_entry *hse;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);
	free(hse);
}

static void hse_init(struct perf_hpp_fmt *fmt, struct hist_entry *he)
{
	struct hpp_sort_entry *hse;

	if (!perf_hpp__is_sort_entry(fmt))
		return;

	hse = container_of(fmt, struct hpp_sort_entry, hpp);

	if (hse->se->se_init)
		hse->se->se_init(he);
}

static struct hpp_sort_entry *
__sort_dimension__alloc_hpp(struct sort_dimension *sd, int level)
{
	struct hpp_sort_entry *hse;

	hse = malloc(sizeof(*hse));
	if (hse == NULL) {
		pr_err("Memory allocation failed\n");
		return NULL;
	}

	hse->se = sd->entry;
	hse->hpp.name = sd->entry->se_header;
	hse->hpp.header = __sort__hpp_header;
	hse->hpp.width = __sort__hpp_width;
	hse->hpp.entry = __sort__hpp_entry;
	hse->hpp.color = NULL;

	hse->hpp.cmp = __sort__hpp_cmp;
	hse->hpp.collapse = __sort__hpp_collapse;
	hse->hpp.sort = __sort__hpp_sort;
	hse->hpp.equal = __sort__hpp_equal;
	hse->hpp.free = hse_free;
	hse->hpp.init = hse_init;

	INIT_LIST_HEAD(&hse->hpp.list);
	INIT_LIST_HEAD(&hse->hpp.sort_list);
	hse->hpp.elide = false;
	hse->hpp.len = 0;
	hse->hpp.user_len = 0;
	hse->hpp.level = level;

	return hse;
}

static void hpp_free(struct perf_hpp_fmt *fmt)
{
	free(fmt);
}

static struct perf_hpp_fmt *__hpp_dimension__alloc_hpp(struct hpp_dimension *hd,
						       int level)
{
	struct perf_hpp_fmt *fmt;

	fmt = memdup(hd->fmt, sizeof(*fmt));
	if (fmt) {
		INIT_LIST_HEAD(&fmt->list);
		INIT_LIST_HEAD(&fmt->sort_list);
		fmt->free = hpp_free;
		fmt->level = level;
	}

	return fmt;
}

int hist_entry__filter(struct hist_entry *he, int type, const void *arg)
{
	struct perf_hpp_fmt *fmt;
	struct hpp_sort_entry *hse;
	int ret = -1;
	int r;

	perf_hpp_list__for_each_format(he->hpp_list, fmt) {
		if (!perf_hpp__is_sort_entry(fmt))
			continue;

		hse = container_of(fmt, struct hpp_sort_entry, hpp);
		if (hse->se->se_filter == NULL)
			continue;

		/*
		 * hist entry is filtered if any of sort key in the hpp list
		 * is applied.  But it should skip non-matched filter types.
		 */
		r = hse->se->se_filter(he, type, arg);
		if (r >= 0) {
			if (ret < 0)
				ret = 0;
			ret |= r;
		}
	}

	return ret;
}

static int __sort_dimension__add_hpp_sort(struct sort_dimension *sd,
					  struct perf_hpp_list *list,
					  int level)
{
	struct hpp_sort_entry *hse = __sort_dimension__alloc_hpp(sd, level);

	if (hse == NULL)
		return -1;

	perf_hpp_list__register_sort_field(list, &hse->hpp);
	return 0;
}

static int __sort_dimension__add_hpp_output(struct sort_dimension *sd,
					    struct perf_hpp_list *list,
					    int level)
{
	struct hpp_sort_entry *hse = __sort_dimension__alloc_hpp(sd, level);

	if (hse == NULL)
		return -1;

	perf_hpp_list__column_register(list, &hse->hpp);
	return 0;
}

#ifndef HAVE_LIBTRACEEVENT
bool perf_hpp__is_dynamic_entry(struct perf_hpp_fmt *fmt __maybe_unused)
{
	return false;
}
bool perf_hpp__defined_dynamic_entry(struct perf_hpp_fmt *fmt __maybe_unused,
				     struct hists *hists __maybe_unused)
{
	return false;
}
#else
struct hpp_dynamic_entry {
	struct perf_hpp_fmt hpp;
	struct evsel *evsel;
	struct tep_format_field *field;
	unsigned dynamic_len;
	bool raw_trace;
};

static int hde_width(struct hpp_dynamic_entry *hde)
{
	if (!hde->hpp.len) {
		int len = hde->dynamic_len;
		int namelen = strlen(hde->field->name);
		int fieldlen = hde->field->size;

		if (namelen > len)
			len = namelen;

		if (!(hde->field->flags & TEP_FIELD_IS_STRING)) {
			/* length for print hex numbers */
			fieldlen = hde->field->size * 2 + 2;
		}
		if (fieldlen > len)
			len = fieldlen;

		hde->hpp.len = len;
	}
	return hde->hpp.len;
}

static void update_dynamic_len(struct hpp_dynamic_entry *hde,
			       struct hist_entry *he)
{
	char *str, *pos;
	struct tep_format_field *field = hde->field;
	size_t namelen;
	bool last = false;

	if (hde->raw_trace)
		return;

	/* parse pretty print result and update max length */
	if (!he->trace_output)
		he->trace_output = get_trace_output(he);

	namelen = strlen(field->name);
	str = he->trace_output;

	while (str) {
		pos = strchr(str, ' ');
		if (pos == NULL) {
			last = true;
			pos = str + strlen(str);
		}

		if (!strncmp(str, field->name, namelen)) {
			size_t len;

			str += namelen + 1;
			len = pos - str;

			if (len > hde->dynamic_len)
				hde->dynamic_len = len;
			break;
		}

		if (last)
			str = NULL;
		else
			str = pos + 1;
	}
}

static int __sort__hde_header(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			      struct hists *hists __maybe_unused,
			      int line __maybe_unused,
			      int *span __maybe_unused)
{
	struct hpp_dynamic_entry *hde;
	size_t len = fmt->user_len;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);

	if (!len)
		len = hde_width(hde);

	return scnprintf(hpp->buf, hpp->size, "%*.*s", len, len, hde->field->name);
}

static int __sort__hde_width(struct perf_hpp_fmt *fmt,
			     struct perf_hpp *hpp __maybe_unused,
			     struct hists *hists __maybe_unused)
{
	struct hpp_dynamic_entry *hde;
	size_t len = fmt->user_len;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);

	if (!len)
		len = hde_width(hde);

	return len;
}

bool perf_hpp__defined_dynamic_entry(struct perf_hpp_fmt *fmt, struct hists *hists)
{
	struct hpp_dynamic_entry *hde;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);

	return hists_to_evsel(hists) == hde->evsel;
}

static int __sort__hde_entry(struct perf_hpp_fmt *fmt, struct perf_hpp *hpp,
			     struct hist_entry *he)
{
	struct hpp_dynamic_entry *hde;
	size_t len = fmt->user_len;
	char *str, *pos;
	struct tep_format_field *field;
	size_t namelen;
	bool last = false;
	int ret;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);

	if (!len)
		len = hde_width(hde);

	if (hde->raw_trace)
		goto raw_field;

	if (!he->trace_output)
		he->trace_output = get_trace_output(he);

	field = hde->field;
	namelen = strlen(field->name);
	str = he->trace_output;

	while (str) {
		pos = strchr(str, ' ');
		if (pos == NULL) {
			last = true;
			pos = str + strlen(str);
		}

		if (!strncmp(str, field->name, namelen)) {
			str += namelen + 1;
			str = strndup(str, pos - str);

			if (str == NULL)
				return scnprintf(hpp->buf, hpp->size,
						 "%*.*s", len, len, "ERROR");
			break;
		}

		if (last)
			str = NULL;
		else
			str = pos + 1;
	}

	if (str == NULL) {
		struct trace_seq seq;
raw_field:
		trace_seq_init(&seq);
		tep_print_field(&seq, he->raw_data, hde->field);
		str = seq.buffer;
	}

	ret = scnprintf(hpp->buf, hpp->size, "%*.*s", len, len, str);
	free(str);
	return ret;
}

static int64_t __sort__hde_cmp(struct perf_hpp_fmt *fmt,
			       struct hist_entry *a, struct hist_entry *b)
{
	struct hpp_dynamic_entry *hde;
	struct tep_format_field *field;
	unsigned offset, size;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);

	field = hde->field;
	if (field->flags & TEP_FIELD_IS_DYNAMIC) {
		unsigned long long dyn;

		tep_read_number_field(field, a->raw_data, &dyn);
		offset = dyn & 0xffff;
		size = (dyn >> 16) & 0xffff;
		if (tep_field_is_relative(field->flags))
			offset += field->offset + field->size;
		/* record max width for output */
		if (size > hde->dynamic_len)
			hde->dynamic_len = size;
	} else {
		offset = field->offset;
		size = field->size;
	}

	return memcmp(a->raw_data + offset, b->raw_data + offset, size);
}

bool perf_hpp__is_dynamic_entry(struct perf_hpp_fmt *fmt)
{
	return fmt->cmp == __sort__hde_cmp;
}

static bool __sort__hde_equal(struct perf_hpp_fmt *a, struct perf_hpp_fmt *b)
{
	struct hpp_dynamic_entry *hde_a;
	struct hpp_dynamic_entry *hde_b;

	if (!perf_hpp__is_dynamic_entry(a) || !perf_hpp__is_dynamic_entry(b))
		return false;

	hde_a = container_of(a, struct hpp_dynamic_entry, hpp);
	hde_b = container_of(b, struct hpp_dynamic_entry, hpp);

	return hde_a->field == hde_b->field;
}

static void hde_free(struct perf_hpp_fmt *fmt)
{
	struct hpp_dynamic_entry *hde;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);
	free(hde);
}

static void __sort__hde_init(struct perf_hpp_fmt *fmt, struct hist_entry *he)
{
	struct hpp_dynamic_entry *hde;

	if (!perf_hpp__is_dynamic_entry(fmt))
		return;

	hde = container_of(fmt, struct hpp_dynamic_entry, hpp);
	update_dynamic_len(hde, he);
}

static struct hpp_dynamic_entry *
__alloc_dynamic_entry(struct evsel *evsel, struct tep_format_field *field,
		      int level)
{
	struct hpp_dynamic_entry *hde;

	hde = malloc(sizeof(*hde));
	if (hde == NULL) {
		pr_debug("Memory allocation failed\n");
		return NULL;
	}

	hde->evsel = evsel;
	hde->field = field;
	hde->dynamic_len = 0;

	hde->hpp.name = field->name;
	hde->hpp.header = __sort__hde_header;
	hde->hpp.width  = __sort__hde_width;
	hde->hpp.entry  = __sort__hde_entry;
	hde->hpp.color  = NULL;

	hde->hpp.init = __sort__hde_init;
	hde->hpp.cmp = __sort__hde_cmp;
	hde->hpp.collapse = __sort__hde_cmp;
	hde->hpp.sort = __sort__hde_cmp;
	hde->hpp.equal = __sort__hde_equal;
	hde->hpp.free = hde_free;

	INIT_LIST_HEAD(&hde->hpp.list);
	INIT_LIST_HEAD(&hde->hpp.sort_list);
	hde->hpp.elide = false;
	hde->hpp.len = 0;
	hde->hpp.user_len = 0;
	hde->hpp.level = level;

	return hde;
}
#endif /* HAVE_LIBTRACEEVENT */

struct perf_hpp_fmt *perf_hpp_fmt__dup(struct perf_hpp_fmt *fmt)
{
	struct perf_hpp_fmt *new_fmt = NULL;

	if (perf_hpp__is_sort_entry(fmt)) {
		struct hpp_sort_entry *hse, *new_hse;

		hse = container_of(fmt, struct hpp_sort_entry, hpp);
		new_hse = memdup(hse, sizeof(*hse));
		if (new_hse)
			new_fmt = &new_hse->hpp;
#ifdef HAVE_LIBTRACEEVENT
	} else if (perf_hpp__is_dynamic_entry(fmt)) {
		struct hpp_dynamic_entry *hde, *new_hde;

		hde = container_of(fmt, struct hpp_dynamic_entry, hpp);
		new_hde = memdup(hde, sizeof(*hde));
		if (new_hde)
			new_fmt = &new_hde->hpp;
#endif
	} else {
		new_fmt = memdup(fmt, sizeof(*fmt));
	}

	INIT_LIST_HEAD(&new_fmt->list);
	INIT_LIST_HEAD(&new_fmt->sort_list);

	return new_fmt;
}

static int parse_field_name(char *str, char **event, char **field, char **opt)
{
	char *event_name, *field_name, *opt_name;

	event_name = str;
	field_name = strchr(str, '.');

	if (field_name) {
		*field_name++ = '\0';
	} else {
		event_name = NULL;
		field_name = str;
	}

	opt_name = strchr(field_name, '/');
	if (opt_name)
		*opt_name++ = '\0';

	*event = event_name;
	*field = field_name;
	*opt   = opt_name;

	return 0;
}

/* find match evsel using a given event name.  The event name can be:
 *   1. '%' + event index (e.g. '%1' for first event)
 *   2. full event name (e.g. sched:sched_switch)
 *   3. partial event name (should not contain ':')
 */
static struct evsel *find_evsel(struct evlist *evlist, char *event_name)
{
	struct evsel *evsel = NULL;
	struct evsel *pos;
	bool full_name;

	/* case 1 */
	if (event_name[0] == '%') {
		int nr = strtol(event_name+1, NULL, 0);

		if (nr > evlist->core.nr_entries)
			return NULL;

		evsel = evlist__first(evlist);
		while (--nr > 0)
			evsel = evsel__next(evsel);

		return evsel;
	}

	full_name = !!strchr(event_name, ':');
	evlist__for_each_entry(evlist, pos) {
		/* case 2 */
		if (full_name && evsel__name_is(pos, event_name))
			return pos;
		/* case 3 */
		if (!full_name && strstr(pos->name, event_name)) {
			if (evsel) {
				pr_debug("'%s' event is ambiguous: it can be %s or %s\n",
					 event_name, evsel->name, pos->name);
				return NULL;
			}
			evsel = pos;
		}
	}

	return evsel;
}

#ifdef HAVE_LIBTRACEEVENT
static int __dynamic_dimension__add(struct evsel *evsel,
				    struct tep_format_field *field,
				    bool raw_trace, int level)
{
	struct hpp_dynamic_entry *hde;

	hde = __alloc_dynamic_entry(evsel, field, level);
	if (hde == NULL)
		return -ENOMEM;

	hde->raw_trace = raw_trace;

	perf_hpp__register_sort_field(&hde->hpp);
	return 0;
}

static int add_evsel_fields(struct evsel *evsel, bool raw_trace, int level)
{
	int ret;
	struct tep_event *tp_format = evsel__tp_format(evsel);
	struct tep_format_field *field = tp_format ? tp_format->format.fields : NULL;
	while (field) {
		ret = __dynamic_dimension__add(evsel, field, raw_trace, level);
		if (ret < 0)
			return ret;

		field = field->next;
	}
	return 0;
}

static int add_all_dynamic_fields(struct evlist *evlist, bool raw_trace,
				  int level)
{
	int ret;
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
			continue;

		ret = add_evsel_fields(evsel, raw_trace, level);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int add_all_matching_fields(struct evlist *evlist,
				   char *field_name, bool raw_trace, int level)
{
	int ret = -ESRCH;
	struct evsel *evsel;

	evlist__for_each_entry(evlist, evsel) {
		struct tep_event *tp_format;
		struct tep_format_field *field;

		if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT)
			continue;

		tp_format = evsel__tp_format(evsel);
		if (tp_format == NULL)
			continue;

		field = tep_find_any_field(tp_format, field_name);
		if (field == NULL)
			continue;

		ret = __dynamic_dimension__add(evsel, field, raw_trace, level);
		if (ret < 0)
			break;
	}
	return ret;
}
#endif /* HAVE_LIBTRACEEVENT */

static int add_dynamic_entry(struct evlist *evlist, const char *tok,
			     int level)
{
	char *str, *event_name, *field_name, *opt_name;
	struct evsel *evsel;
	bool raw_trace = symbol_conf.raw_trace;
	int ret = 0;

	if (evlist == NULL)
		return -ENOENT;

	str = strdup(tok);
	if (str == NULL)
		return -ENOMEM;

	if (parse_field_name(str, &event_name, &field_name, &opt_name) < 0) {
		ret = -EINVAL;
		goto out;
	}

	if (opt_name) {
		if (strcmp(opt_name, "raw")) {
			pr_debug("unsupported field option %s\n", opt_name);
			ret = -EINVAL;
			goto out;
		}
		raw_trace = true;
	}

#ifdef HAVE_LIBTRACEEVENT
	if (!strcmp(field_name, "trace_fields")) {
		ret = add_all_dynamic_fields(evlist, raw_trace, level);
		goto out;
	}

	if (event_name == NULL) {
		ret = add_all_matching_fields(evlist, field_name, raw_trace, level);
		goto out;
	}
#else
	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type == PERF_TYPE_TRACEPOINT) {
			pr_err("%s %s", ret ? "," : "This perf binary isn't linked with libtraceevent, can't process", evsel__name(evsel));
			ret = -ENOTSUP;
		}
	}

	if (ret) {
		pr_err("\n");
		goto out;
	}
#endif

	evsel = find_evsel(evlist, event_name);
	if (evsel == NULL) {
		pr_debug("Cannot find event: %s\n", event_name);
		ret = -ENOENT;
		goto out;
	}

	if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT) {
		pr_debug("%s is not a tracepoint event\n", event_name);
		ret = -EINVAL;
		goto out;
	}

#ifdef HAVE_LIBTRACEEVENT
	if (!strcmp(field_name, "*")) {
		ret = add_evsel_fields(evsel, raw_trace, level);
	} else {
		struct tep_event *tp_format = evsel__tp_format(evsel);
		struct tep_format_field *field =
			tp_format ? tep_find_any_field(tp_format, field_name) : NULL;

		if (field == NULL) {
			pr_debug("Cannot find event field for %s.%s\n",
				 event_name, field_name);
			return -ENOENT;
		}

		ret = __dynamic_dimension__add(evsel, field, raw_trace, level);
	}
#else
	(void)level;
	(void)raw_trace;
#endif /* HAVE_LIBTRACEEVENT */

out:
	free(str);
	return ret;
}

static int __sort_dimension__add(struct sort_dimension *sd,
				 struct perf_hpp_list *list,
				 int level)
{
	if (sd->taken)
		return 0;

	if (__sort_dimension__add_hpp_sort(sd, list, level) < 0)
		return -1;

	if (sd->entry->se_collapse)
		list->need_collapse = 1;

	sd->taken = 1;

	return 0;
}

static int __hpp_dimension__add(struct hpp_dimension *hd,
				struct perf_hpp_list *list,
				int level)
{
	struct perf_hpp_fmt *fmt;

	if (hd->taken)
		return 0;

	fmt = __hpp_dimension__alloc_hpp(hd, level);
	if (!fmt)
		return -1;

	hd->taken = 1;
	hd->was_taken = 1;
	perf_hpp_list__register_sort_field(list, fmt);
	return 0;
}

static int __sort_dimension__add_output(struct perf_hpp_list *list,
					struct sort_dimension *sd,
					int level)
{
	if (sd->taken)
		return 0;

	if (__sort_dimension__add_hpp_output(sd, list, level) < 0)
		return -1;

	sd->taken = 1;
	return 0;
}

static int __hpp_dimension__add_output(struct perf_hpp_list *list,
				       struct hpp_dimension *hd,
				       int level)
{
	struct perf_hpp_fmt *fmt;

	if (hd->taken)
		return 0;

	fmt = __hpp_dimension__alloc_hpp(hd, level);
	if (!fmt)
		return -1;

	hd->taken = 1;
	perf_hpp_list__column_register(list, fmt);
	return 0;
}

int hpp_dimension__add_output(unsigned col, bool implicit)
{
	struct hpp_dimension *hd;

	BUG_ON(col >= PERF_HPP__MAX_INDEX);
	hd = &hpp_sort_dimensions[col];
	if (implicit && !hd->was_taken)
		return 0;
	return __hpp_dimension__add_output(&perf_hpp_list, hd, /*level=*/0);
}

int sort_dimension__add(struct perf_hpp_list *list, const char *tok,
			struct evlist *evlist, struct perf_env *env,
			int level)
{
	unsigned int i, j;

	/*
	 * Check to see if there are any arch specific
	 * sort dimensions not applicable for the current
	 * architecture. If so, Skip that sort key since
	 * we don't want to display it in the output fields.
	 */
	for (j = 0; j < ARRAY_SIZE(arch_specific_sort_keys); j++) {
		if (!strcmp(arch_specific_sort_keys[j], tok) &&
		    !arch_support_sort_key(tok, env)) {
			return 0;
		}
	}

	for (i = 0; i < ARRAY_SIZE(common_sort_dimensions); i++) {
		struct sort_dimension *sd = &common_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		for (j = 0; j < ARRAY_SIZE(dynamic_headers); j++) {
			if (sd->name && !strcmp(dynamic_headers[j], sd->name))
				sort_dimension_add_dynamic_header(sd, env);
		}

		if (sd->entry == &sort_parent && parent_pattern) {
			int ret = regcomp(&parent_regex, parent_pattern, REG_EXTENDED);
			if (ret) {
				char err[BUFSIZ];

				regerror(ret, &parent_regex, err, sizeof(err));
				pr_err("Invalid regex: %s\n%s", parent_pattern, err);
				return -EINVAL;
			}
			list->parent = 1;
		} else if (sd->entry == &sort_sym) {
			list->sym = 1;
			/*
			 * perf diff displays the performance difference amongst
			 * two or more perf.data files. Those files could come
			 * from different binaries. So we should not compare
			 * their ips, but the name of symbol.
			 */
			if (sort__mode == SORT_MODE__DIFF)
				sd->entry->se_collapse = sort__sym_sort;

		} else if (sd->entry == &sort_dso) {
			list->dso = 1;
		} else if (sd->entry == &sort_socket) {
			list->socket = 1;
		} else if (sd->entry == &sort_thread) {
			list->thread = 1;
		} else if (sd->entry == &sort_comm) {
			list->comm = 1;
		} else if (sd->entry == &sort_type_offset) {
			symbol_conf.annotate_data_member = true;
		}

		return __sort_dimension__add(sd, list, level);
	}

	for (i = 0; i < ARRAY_SIZE(bstack_sort_dimensions); i++) {
		struct sort_dimension *sd = &bstack_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		if ((sort__mode != SORT_MODE__BRANCH) &&
			strncasecmp(tok, "callchain_branch_predicted",
				    strlen(tok)) &&
			strncasecmp(tok, "callchain_branch_abort",
				    strlen(tok)) &&
			strncasecmp(tok, "callchain_branch_cycles",
				    strlen(tok)))
			return -EINVAL;

		if (sd->entry == &sort_sym_from || sd->entry == &sort_sym_to)
			list->sym = 1;

		__sort_dimension__add(sd, list, level);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(memory_sort_dimensions); i++) {
		struct sort_dimension *sd = &memory_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		if (sort__mode != SORT_MODE__MEMORY)
			return -EINVAL;

		if (sd->entry == &sort_mem_dcacheline && cacheline_size() == 0)
			return -EINVAL;

		if (sd->entry == &sort_mem_daddr_sym)
			list->sym = 1;

		__sort_dimension__add(sd, list, level);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(hpp_sort_dimensions); i++) {
		struct hpp_dimension *hd = &hpp_sort_dimensions[i];

		if (strncasecmp(tok, hd->name, strlen(tok)))
			continue;

		return __hpp_dimension__add(hd, list, level);
	}

	if (!add_dynamic_entry(evlist, tok, level))
		return 0;

	return -ESRCH;
}

/* This should match with sort_dimension__add() above */
static bool is_hpp_sort_key(const char *key, struct perf_env *env)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(arch_specific_sort_keys); i++) {
		if (!strcmp(arch_specific_sort_keys[i], key) &&
		    !arch_support_sort_key(key, env)) {
			return false;
		}
	}

	for (i = 0; i < ARRAY_SIZE(common_sort_dimensions); i++) {
		struct sort_dimension *sd = &common_sort_dimensions[i];

		if (sd->name && !strncasecmp(key, sd->name, strlen(key)))
			return false;
	}

	for (i = 0; i < ARRAY_SIZE(hpp_sort_dimensions); i++) {
		struct hpp_dimension *hd = &hpp_sort_dimensions[i];

		if (!strncasecmp(key, hd->name, strlen(key)))
			return true;
	}
	return false;
}

static int setup_sort_list(struct perf_hpp_list *list, char *str,
			   struct evlist *evlist, struct perf_env *env)
{
	char *tmp, *tok;
	int ret = 0;
	int level = 0;
	int next_level = 1;
	int prev_level = 0;
	bool in_group = false;
	bool prev_was_hpp = false;

	do {
		tok = str;
		tmp = strpbrk(str, "{}, ");
		if (tmp) {
			if (in_group)
				next_level = level;
			else
				next_level = level + 1;

			if (*tmp == '{')
				in_group = true;
			else if (*tmp == '}')
				in_group = false;

			*tmp = '\0';
			str = tmp + 1;
		}

		if (*tok) {
			if (is_hpp_sort_key(tok, env)) {
				/* keep output (hpp) sort keys in the same level */
				if (prev_was_hpp) {
					bool next_same = (level == next_level);

					level = prev_level;
					next_level = next_same ? level : level+1;
				}
				prev_was_hpp = true;
			} else {
				prev_was_hpp = false;
			}

			ret = sort_dimension__add(list, tok, evlist, env, level);
			if (ret == -EINVAL) {
				if (!cacheline_size() && !strncasecmp(tok, "dcacheline", strlen(tok)))
					ui__error("The \"dcacheline\" --sort key needs to know the cacheline size and it couldn't be determined on this system");
				else
					ui__error("Invalid --sort key: `%s'", tok);
				break;
			} else if (ret == -ESRCH) {
				ui__error("Unknown --sort key: `%s'", tok);
				break;
			}
			prev_level = level;
		}

		level = next_level;
	} while (tmp);

	return ret;
}

static const char *get_default_sort_order(struct evlist *evlist)
{
	const char *default_sort_orders[] = {
		default_sort_order,
		default_branch_sort_order,
		default_mem_sort_order,
		default_top_sort_order,
		default_diff_sort_order,
		default_tracepoint_sort_order,
	};
	bool use_trace = true;
	struct evsel *evsel;

	BUG_ON(sort__mode >= ARRAY_SIZE(default_sort_orders));

	if (evlist == NULL || evlist__empty(evlist))
		goto out_no_evlist;

	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type != PERF_TYPE_TRACEPOINT) {
			use_trace = false;
			break;
		}
	}

	if (use_trace) {
		sort__mode = SORT_MODE__TRACEPOINT;
		if (symbol_conf.raw_trace)
			return "trace_fields";
	}
out_no_evlist:
	return default_sort_orders[sort__mode];
}

static int setup_sort_order(struct evlist *evlist)
{
	char *new_sort_order;

	/*
	 * Append '+'-prefixed sort order to the default sort
	 * order string.
	 */
	if (!sort_order || is_strict_order(sort_order))
		return 0;

	if (sort_order[1] == '\0') {
		ui__error("Invalid --sort key: `+'");
		return -EINVAL;
	}

	/*
	 * We allocate new sort_order string, but we never free it,
	 * because it's checked over the rest of the code.
	 */
	if (asprintf(&new_sort_order, "%s,%s",
		     get_default_sort_order(evlist), sort_order + 1) < 0) {
		pr_err("Not enough memory to set up --sort");
		return -ENOMEM;
	}

	sort_order = new_sort_order;
	return 0;
}

/*
 * Adds 'pre,' prefix into 'str' is 'pre' is
 * not already part of 'str'.
 */
static char *prefix_if_not_in(const char *pre, char *str)
{
	char *n;

	if (!str || strstr(str, pre))
		return str;

	if (asprintf(&n, "%s,%s", pre, str) < 0)
		n = NULL;

	free(str);
	return n;
}

static char *setup_overhead(char *keys)
{
	if (sort__mode == SORT_MODE__DIFF)
		return keys;

	if (symbol_conf.prefer_latency) {
		keys = prefix_if_not_in("overhead", keys);
		keys = prefix_if_not_in("latency", keys);
		if (symbol_conf.cumulate_callchain) {
			keys = prefix_if_not_in("overhead_children", keys);
			keys = prefix_if_not_in("latency_children", keys);
		}
	} else if (!keys || (!strstr(keys, "overhead") &&
			!strstr(keys, "latency"))) {
		if (symbol_conf.enable_latency)
			keys = prefix_if_not_in("latency", keys);
		keys = prefix_if_not_in("overhead", keys);
		if (symbol_conf.cumulate_callchain) {
			if (symbol_conf.enable_latency)
				keys = prefix_if_not_in("latency_children", keys);
			keys = prefix_if_not_in("overhead_children", keys);
		}
	}

	return keys;
}

static int __setup_sorting(struct evlist *evlist, struct perf_env *env)
{
	char *str;
	const char *sort_keys;
	int ret = 0;

	ret = setup_sort_order(evlist);
	if (ret)
		return ret;

	sort_keys = sort_order;
	if (sort_keys == NULL) {
		if (is_strict_order(field_order)) {
			/*
			 * If user specified field order but no sort order,
			 * we'll honor it and not add default sort orders.
			 */
			return 0;
		}

		sort_keys = get_default_sort_order(evlist);
	}

	str = strdup(sort_keys);
	if (str == NULL) {
		pr_err("Not enough memory to setup sort keys");
		return -ENOMEM;
	}

	/*
	 * Prepend overhead fields for backward compatibility.
	 */
	if (!is_strict_order(field_order)) {
		str = setup_overhead(str);
		if (str == NULL) {
			pr_err("Not enough memory to setup overhead keys");
			return -ENOMEM;
		}
	}

	ret = setup_sort_list(&perf_hpp_list, str, evlist, env);

	free(str);
	return ret;
}

void perf_hpp__set_elide(int idx, bool elide)
{
	struct perf_hpp_fmt *fmt;
	struct hpp_sort_entry *hse;

	perf_hpp_list__for_each_format(&perf_hpp_list, fmt) {
		if (!perf_hpp__is_sort_entry(fmt))
			continue;

		hse = container_of(fmt, struct hpp_sort_entry, hpp);
		if (hse->se->se_width_idx == idx) {
			fmt->elide = elide;
			break;
		}
	}
}

static bool __get_elide(struct strlist *list, const char *list_name, FILE *fp)
{
	if (list && strlist__nr_entries(list) == 1) {
		if (fp != NULL)
			fprintf(fp, "# %s: %s\n", list_name,
				strlist__entry(list, 0)->s);
		return true;
	}
	return false;
}

static bool get_elide(int idx, FILE *output)
{
	switch (idx) {
	case HISTC_SYMBOL:
		return __get_elide(symbol_conf.sym_list, "symbol", output);
	case HISTC_DSO:
		return __get_elide(symbol_conf.dso_list, "dso", output);
	case HISTC_COMM:
		return __get_elide(symbol_conf.comm_list, "comm", output);
	default:
		break;
	}

	if (sort__mode != SORT_MODE__BRANCH)
		return false;

	switch (idx) {
	case HISTC_SYMBOL_FROM:
		return __get_elide(symbol_conf.sym_from_list, "sym_from", output);
	case HISTC_SYMBOL_TO:
		return __get_elide(symbol_conf.sym_to_list, "sym_to", output);
	case HISTC_DSO_FROM:
		return __get_elide(symbol_conf.dso_from_list, "dso_from", output);
	case HISTC_DSO_TO:
		return __get_elide(symbol_conf.dso_to_list, "dso_to", output);
	case HISTC_ADDR_FROM:
		return __get_elide(symbol_conf.sym_from_list, "addr_from", output);
	case HISTC_ADDR_TO:
		return __get_elide(symbol_conf.sym_to_list, "addr_to", output);
	default:
		break;
	}

	return false;
}

void sort__setup_elide(FILE *output)
{
	struct perf_hpp_fmt *fmt;
	struct hpp_sort_entry *hse;

	perf_hpp_list__for_each_format(&perf_hpp_list, fmt) {
		if (!perf_hpp__is_sort_entry(fmt))
			continue;

		hse = container_of(fmt, struct hpp_sort_entry, hpp);
		fmt->elide = get_elide(hse->se->se_width_idx, output);
	}

	/*
	 * It makes no sense to elide all of sort entries.
	 * Just revert them to show up again.
	 */
	perf_hpp_list__for_each_format(&perf_hpp_list, fmt) {
		if (!perf_hpp__is_sort_entry(fmt))
			continue;

		if (!fmt->elide)
			return;
	}

	perf_hpp_list__for_each_format(&perf_hpp_list, fmt) {
		if (!perf_hpp__is_sort_entry(fmt))
			continue;

		fmt->elide = false;
	}
}

int output_field_add(struct perf_hpp_list *list, const char *tok, int *level)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hpp_sort_dimensions); i++) {
		struct hpp_dimension *hd = &hpp_sort_dimensions[i];

		if (strncasecmp(tok, hd->name, strlen(tok)))
			continue;

		if (!strcasecmp(tok, "weight"))
			ui__warning("--fields weight shows the average value unlike in the --sort key.\n");

		if (hd->mem_mode && sort__mode != SORT_MODE__MEMORY)
			continue;

		return __hpp_dimension__add_output(list, hd, *level);
	}

	/*
	 * A non-output field will increase level so that it can be in a
	 * different hierarchy.
	 */
	(*level)++;

	for (i = 0; i < ARRAY_SIZE(common_sort_dimensions); i++) {
		struct sort_dimension *sd = &common_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		return __sort_dimension__add_output(list, sd, *level);
	}

	for (i = 0; i < ARRAY_SIZE(bstack_sort_dimensions); i++) {
		struct sort_dimension *sd = &bstack_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		if (sort__mode != SORT_MODE__BRANCH)
			return -EINVAL;

		return __sort_dimension__add_output(list, sd, *level);
	}

	for (i = 0; i < ARRAY_SIZE(memory_sort_dimensions); i++) {
		struct sort_dimension *sd = &memory_sort_dimensions[i];

		if (!sd->name || strncasecmp(tok, sd->name, strlen(tok)))
			continue;

		if (sort__mode != SORT_MODE__MEMORY)
			return -EINVAL;

		return __sort_dimension__add_output(list, sd, *level);
	}

	return -ESRCH;
}

static int setup_output_list(struct perf_hpp_list *list, char *str)
{
	char *tmp, *tok;
	int ret = 0;
	int level = 0;

	for (tok = strtok_r(str, ", ", &tmp);
			tok; tok = strtok_r(NULL, ", ", &tmp)) {
		ret = output_field_add(list, tok, &level);
		if (ret == -EINVAL) {
			ui__error("Invalid --fields key: `%s'", tok);
			break;
		} else if (ret == -ESRCH) {
			ui__error("Unknown --fields key: `%s'", tok);
			break;
		}
	}

	return ret;
}

void reset_dimensions(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(common_sort_dimensions); i++)
		common_sort_dimensions[i].taken = 0;

	for (i = 0; i < ARRAY_SIZE(hpp_sort_dimensions); i++)
		hpp_sort_dimensions[i].taken = 0;

	for (i = 0; i < ARRAY_SIZE(bstack_sort_dimensions); i++)
		bstack_sort_dimensions[i].taken = 0;

	for (i = 0; i < ARRAY_SIZE(memory_sort_dimensions); i++)
		memory_sort_dimensions[i].taken = 0;
}

bool is_strict_order(const char *order)
{
	return order && (*order != '+');
}

static int __setup_output_field(void)
{
	char *str, *strp;
	int ret = -EINVAL;

	if (field_order == NULL)
		return 0;

	strp = str = strdup(field_order);
	if (str == NULL) {
		pr_err("Not enough memory to setup output fields");
		return -ENOMEM;
	}

	if (!is_strict_order(field_order))
		strp++;

	if (!strlen(strp)) {
		ui__error("Invalid --fields key: `+'");
		goto out;
	}

	ret = setup_output_list(&perf_hpp_list, strp);

out:
	free(str);
	return ret;
}

int setup_sorting(struct evlist *evlist, struct perf_env *env)
{
	int err;

	err = __setup_sorting(evlist, env);
	if (err < 0)
		return err;

	if (parent_pattern != default_parent_pattern) {
		err = sort_dimension__add(&perf_hpp_list, "parent", evlist, env, -1);
		if (err < 0)
			return err;
	}

	reset_dimensions();

	/*
	 * perf diff doesn't use default hpp output fields.
	 */
	if (sort__mode != SORT_MODE__DIFF)
		perf_hpp__init();

	err = __setup_output_field();
	if (err < 0)
		return err;

	err = perf_hpp__alloc_mem_stats(&perf_hpp_list, evlist);
	if (err < 0)
		return err;

	/* copy sort keys to output fields */
	perf_hpp__setup_output_field(&perf_hpp_list);
	/* and then copy output fields to sort keys */
	perf_hpp__append_sort_keys(&perf_hpp_list);

	/* setup hists-specific output fields */
	if (perf_hpp__setup_hists_formats(&perf_hpp_list, evlist) < 0)
		return -1;

	return 0;
}

void reset_output_field(void)
{
	perf_hpp_list.need_collapse = 0;
	perf_hpp_list.parent = 0;
	perf_hpp_list.sym = 0;
	perf_hpp_list.dso = 0;

	field_order = NULL;
	sort_order = NULL;

	reset_dimensions();
	perf_hpp__reset_output_field(&perf_hpp_list);
}

#define INDENT (3*8 + 1)

static void add_key(struct strbuf *sb, const char *str, int *llen)
{
	if (!str)
		return;

	if (*llen >= 75) {
		strbuf_addstr(sb, "\n\t\t\t ");
		*llen = INDENT;
	}
	strbuf_addf(sb, " %s", str);
	*llen += strlen(str) + 1;
}

static void add_sort_string(struct strbuf *sb, struct sort_dimension *s, int n,
			    int *llen)
{
	int i;

	for (i = 0; i < n; i++)
		add_key(sb, s[i].name, llen);
}

static void add_hpp_sort_string(struct strbuf *sb, struct hpp_dimension *s, int n,
				int *llen)
{
	int i;

	for (i = 0; i < n; i++)
		add_key(sb, s[i].name, llen);
}

char *sort_help(const char *prefix, enum sort_mode mode)
{
	struct strbuf sb;
	char *s;
	int len = strlen(prefix) + INDENT;

	strbuf_init(&sb, 300);
	strbuf_addstr(&sb, prefix);
	add_hpp_sort_string(&sb, hpp_sort_dimensions,
			    ARRAY_SIZE(hpp_sort_dimensions), &len);
	add_sort_string(&sb, common_sort_dimensions,
			    ARRAY_SIZE(common_sort_dimensions), &len);
	if (mode == SORT_MODE__NORMAL || mode == SORT_MODE__BRANCH)
		add_sort_string(&sb, bstack_sort_dimensions,
				ARRAY_SIZE(bstack_sort_dimensions), &len);
	if (mode == SORT_MODE__NORMAL || mode == SORT_MODE__MEMORY)
		add_sort_string(&sb, memory_sort_dimensions,
				ARRAY_SIZE(memory_sort_dimensions), &len);
	s = strbuf_detach(&sb, NULL);
	strbuf_release(&sb);
	return s;
}
