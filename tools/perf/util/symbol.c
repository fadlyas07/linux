// SPDX-License-Identifier: GPL-2.0
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include "annotate.h"
#include "build-id.h"
#include "cap.h"
#include "cpumap.h"
#include "debug.h"
#include "demangle-cxx.h"
#include "demangle-java.h"
#include "demangle-ocaml.h"
#include "demangle-rust-v0.h"
#include "dso.h"
#include "util.h" // lsdir()
#include "debug.h"
#include "event.h"
#include "machine.h"
#include "map.h"
#include "symbol.h"
#include "map_symbol.h"
#include "mem-events.h"
#include "mem-info.h"
#include "symsrc.h"
#include "strlist.h"
#include "intlist.h"
#include "namespaces.h"
#include "header.h"
#include "path.h"
#include <linux/ctype.h>
#include <linux/log2.h>
#include <linux/zalloc.h>

#include <elf.h>
#include <limits.h>
#include <symbol/kallsyms.h>
#include <sys/utsname.h>

static int dso__load_kernel_sym(struct dso *dso, struct map *map);
static int dso__load_guest_kernel_sym(struct dso *dso, struct map *map);
static bool symbol__is_idle(const char *name);

int vmlinux_path__nr_entries;
char **vmlinux_path;

struct symbol_conf symbol_conf = {
	.nanosecs		= false,
	.use_modules		= true,
	.try_vmlinux_path	= true,
	.demangle		= true,
	.demangle_kernel	= false,
	.cumulate_callchain	= true,
	.time_quantum		= 100 * NSEC_PER_MSEC, /* 100ms */
	.show_hist_headers	= true,
	.symfs			= "",
	.event_group		= true,
	.inline_name		= true,
	.res_sample		= 0,
};

struct map_list_node {
	struct list_head node;
	struct map *map;
};

static struct map_list_node *map_list_node__new(void)
{
	return malloc(sizeof(struct map_list_node));
}

static enum dso_binary_type binary_type_symtab[] = {
	DSO_BINARY_TYPE__KALLSYMS,
	DSO_BINARY_TYPE__GUEST_KALLSYMS,
	DSO_BINARY_TYPE__JAVA_JIT,
	DSO_BINARY_TYPE__DEBUGLINK,
	DSO_BINARY_TYPE__BUILD_ID_CACHE,
	DSO_BINARY_TYPE__BUILD_ID_CACHE_DEBUGINFO,
	DSO_BINARY_TYPE__FEDORA_DEBUGINFO,
	DSO_BINARY_TYPE__UBUNTU_DEBUGINFO,
	DSO_BINARY_TYPE__BUILDID_DEBUGINFO,
	DSO_BINARY_TYPE__GNU_DEBUGDATA,
	DSO_BINARY_TYPE__SYSTEM_PATH_DSO,
	DSO_BINARY_TYPE__GUEST_KMODULE,
	DSO_BINARY_TYPE__GUEST_KMODULE_COMP,
	DSO_BINARY_TYPE__SYSTEM_PATH_KMODULE,
	DSO_BINARY_TYPE__SYSTEM_PATH_KMODULE_COMP,
	DSO_BINARY_TYPE__OPENEMBEDDED_DEBUGINFO,
	DSO_BINARY_TYPE__MIXEDUP_UBUNTU_DEBUGINFO,
	DSO_BINARY_TYPE__NOT_FOUND,
};

#define DSO_BINARY_TYPE__SYMTAB_CNT ARRAY_SIZE(binary_type_symtab)

static bool symbol_type__filter(char __symbol_type)
{
	// Since 'U' == undefined and 'u' == unique global symbol, we can't use toupper there
	char symbol_type = toupper(__symbol_type);
	return symbol_type == 'T' || symbol_type == 'W' || symbol_type == 'D' || symbol_type == 'B' ||
	       __symbol_type == 'u' || __symbol_type == 'l';
}

static int prefix_underscores_count(const char *str)
{
	const char *tail = str;

	while (*tail == '_')
		tail++;

	return tail - str;
}

const char * __weak arch__normalize_symbol_name(const char *name)
{
	return name;
}

int __weak arch__compare_symbol_names(const char *namea, const char *nameb)
{
	return strcmp(namea, nameb);
}

int __weak arch__compare_symbol_names_n(const char *namea, const char *nameb,
					unsigned int n)
{
	return strncmp(namea, nameb, n);
}

int __weak arch__choose_best_symbol(struct symbol *syma,
				    struct symbol *symb __maybe_unused)
{
	/* Avoid "SyS" kernel syscall aliases */
	if (strlen(syma->name) >= 3 && !strncmp(syma->name, "SyS", 3))
		return SYMBOL_B;
	if (strlen(syma->name) >= 10 && !strncmp(syma->name, "compat_SyS", 10))
		return SYMBOL_B;

	return SYMBOL_A;
}

static int choose_best_symbol(struct symbol *syma, struct symbol *symb)
{
	s64 a;
	s64 b;
	size_t na, nb;

	/* Prefer a symbol with non zero length */
	a = syma->end - syma->start;
	b = symb->end - symb->start;
	if ((b == 0) && (a > 0))
		return SYMBOL_A;
	else if ((a == 0) && (b > 0))
		return SYMBOL_B;

	if (syma->type != symb->type) {
		if (syma->type == STT_NOTYPE)
			return SYMBOL_B;
		if (symb->type == STT_NOTYPE)
			return SYMBOL_A;
	}

	/* Prefer a non weak symbol over a weak one */
	a = syma->binding == STB_WEAK;
	b = symb->binding == STB_WEAK;
	if (b && !a)
		return SYMBOL_A;
	if (a && !b)
		return SYMBOL_B;

	/* Prefer a global symbol over a non global one */
	a = syma->binding == STB_GLOBAL;
	b = symb->binding == STB_GLOBAL;
	if (a && !b)
		return SYMBOL_A;
	if (b && !a)
		return SYMBOL_B;

	/* Prefer a symbol with less underscores */
	a = prefix_underscores_count(syma->name);
	b = prefix_underscores_count(symb->name);
	if (b > a)
		return SYMBOL_A;
	else if (a > b)
		return SYMBOL_B;

	/* Choose the symbol with the longest name */
	na = strlen(syma->name);
	nb = strlen(symb->name);
	if (na > nb)
		return SYMBOL_A;
	else if (na < nb)
		return SYMBOL_B;

	return arch__choose_best_symbol(syma, symb);
}

void symbols__fixup_duplicate(struct rb_root_cached *symbols)
{
	struct rb_node *nd;
	struct symbol *curr, *next;

	if (symbol_conf.allow_aliases)
		return;

	nd = rb_first_cached(symbols);

	while (nd) {
		curr = rb_entry(nd, struct symbol, rb_node);
again:
		nd = rb_next(&curr->rb_node);
		if (!nd)
			break;

		next = rb_entry(nd, struct symbol, rb_node);
		if (curr->start != next->start)
			continue;

		if (choose_best_symbol(curr, next) == SYMBOL_A) {
			if (next->type == STT_GNU_IFUNC)
				curr->ifunc_alias = true;
			rb_erase_cached(&next->rb_node, symbols);
			symbol__delete(next);
			goto again;
		} else {
			if (curr->type == STT_GNU_IFUNC)
				next->ifunc_alias = true;
			nd = rb_next(&curr->rb_node);
			rb_erase_cached(&curr->rb_node, symbols);
			symbol__delete(curr);
		}
	}
}

/* Update zero-sized symbols using the address of the next symbol */
void symbols__fixup_end(struct rb_root_cached *symbols, bool is_kallsyms)
{
	struct rb_node *nd, *prevnd = rb_first_cached(symbols);
	struct symbol *curr, *prev;

	if (prevnd == NULL)
		return;

	curr = rb_entry(prevnd, struct symbol, rb_node);

	for (nd = rb_next(prevnd); nd; nd = rb_next(nd)) {
		prev = curr;
		curr = rb_entry(nd, struct symbol, rb_node);

		/*
		 * On some architecture kernel text segment start is located at
		 * some low memory address, while modules are located at high
		 * memory addresses (or vice versa).  The gap between end of
		 * kernel text segment and beginning of first module's text
		 * segment is very big.  Therefore do not fill this gap and do
		 * not assign it to the kernel dso map (kallsyms).
		 *
		 * Also BPF code can be allocated separately from text segments
		 * and modules.  So the last entry in a module should not fill
		 * the gap too.
		 *
		 * In kallsyms, it determines module symbols using '[' character
		 * like in:
		 *   ffffffffc1937000 T hdmi_driver_init  [snd_hda_codec_hdmi]
		 */
		if (prev->end == prev->start) {
			const char *prev_mod;
			const char *curr_mod;

			if (!is_kallsyms) {
				prev->end = curr->start;
				continue;
			}

			prev_mod = strchr(prev->name, '[');
			curr_mod = strchr(curr->name, '[');

			/* Last kernel/module symbol mapped to end of page */
			if (!prev_mod != !curr_mod)
				prev->end = roundup(prev->end + 4096, 4096);
			/* Last symbol in the previous module */
			else if (prev_mod && strcmp(prev_mod, curr_mod))
				prev->end = roundup(prev->end + 4096, 4096);
			else
				prev->end = curr->start;

			pr_debug4("%s sym:%s end:%#" PRIx64 "\n",
				  __func__, prev->name, prev->end);
		}
	}

	/* Last entry */
	if (curr->end == curr->start)
		curr->end = roundup(curr->start, 4096) + 4096;
}

struct symbol *symbol__new(u64 start, u64 len, u8 binding, u8 type, const char *name)
{
	size_t namelen = strlen(name) + 1;
	struct symbol *sym = calloc(1, (symbol_conf.priv_size +
					sizeof(*sym) + namelen));
	if (sym == NULL)
		return NULL;

	if (symbol_conf.priv_size) {
		if (symbol_conf.init_annotation) {
			struct annotation *notes = (void *)sym;
			annotation__init(notes);
		}
		sym = ((void *)sym) + symbol_conf.priv_size;
	}

	sym->start   = start;
	sym->end     = len ? start + len : start;
	sym->type    = type;
	sym->binding = binding;
	sym->namelen = namelen - 1;

	pr_debug4("%s: %s %#" PRIx64 "-%#" PRIx64 "\n",
		  __func__, name, start, sym->end);
	memcpy(sym->name, name, namelen);

	return sym;
}

void symbol__delete(struct symbol *sym)
{
	if (symbol_conf.priv_size) {
		if (symbol_conf.init_annotation) {
			struct annotation *notes = symbol__annotation(sym);

			annotation__exit(notes);
		}
	}
	free(((void *)sym) - symbol_conf.priv_size);
}

void symbols__delete(struct rb_root_cached *symbols)
{
	struct symbol *pos;
	struct rb_node *next = rb_first_cached(symbols);

	while (next) {
		pos = rb_entry(next, struct symbol, rb_node);
		next = rb_next(&pos->rb_node);
		rb_erase_cached(&pos->rb_node, symbols);
		symbol__delete(pos);
	}
}

void __symbols__insert(struct rb_root_cached *symbols,
		       struct symbol *sym, bool kernel)
{
	struct rb_node **p = &symbols->rb_root.rb_node;
	struct rb_node *parent = NULL;
	const u64 ip = sym->start;
	struct symbol *s;
	bool leftmost = true;

	if (kernel) {
		const char *name = sym->name;
		/*
		 * ppc64 uses function descriptors and appends a '.' to the
		 * start of every instruction address. Remove it.
		 */
		if (name[0] == '.')
			name++;
		sym->idle = symbol__is_idle(name);
	}

	while (*p != NULL) {
		parent = *p;
		s = rb_entry(parent, struct symbol, rb_node);
		if (ip < s->start)
			p = &(*p)->rb_left;
		else {
			p = &(*p)->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&sym->rb_node, parent, p);
	rb_insert_color_cached(&sym->rb_node, symbols, leftmost);
}

void symbols__insert(struct rb_root_cached *symbols, struct symbol *sym)
{
	__symbols__insert(symbols, sym, false);
}

static struct symbol *symbols__find(struct rb_root_cached *symbols, u64 ip)
{
	struct rb_node *n;

	if (symbols == NULL)
		return NULL;

	n = symbols->rb_root.rb_node;

	while (n) {
		struct symbol *s = rb_entry(n, struct symbol, rb_node);

		if (ip < s->start)
			n = n->rb_left;
		else if (ip > s->end || (ip == s->end && ip != s->start))
			n = n->rb_right;
		else
			return s;
	}

	return NULL;
}

static struct symbol *symbols__first(struct rb_root_cached *symbols)
{
	struct rb_node *n = rb_first_cached(symbols);

	if (n)
		return rb_entry(n, struct symbol, rb_node);

	return NULL;
}

static struct symbol *symbols__last(struct rb_root_cached *symbols)
{
	struct rb_node *n = rb_last(&symbols->rb_root);

	if (n)
		return rb_entry(n, struct symbol, rb_node);

	return NULL;
}

static struct symbol *symbols__next(struct symbol *sym)
{
	struct rb_node *n = rb_next(&sym->rb_node);

	if (n)
		return rb_entry(n, struct symbol, rb_node);

	return NULL;
}

static int symbols__sort_name_cmp(const void *vlhs, const void *vrhs)
{
	const struct symbol *lhs = *((const struct symbol **)vlhs);
	const struct symbol *rhs = *((const struct symbol **)vrhs);

	return strcmp(lhs->name, rhs->name);
}

static struct symbol **symbols__sort_by_name(struct rb_root_cached *source, size_t *len)
{
	struct rb_node *nd;
	struct symbol **result;
	size_t i = 0, size = 0;

	for (nd = rb_first_cached(source); nd; nd = rb_next(nd))
		size++;

	result = malloc(sizeof(*result) * size);
	if (!result)
		return NULL;

	for (nd = rb_first_cached(source); nd; nd = rb_next(nd)) {
		struct symbol *pos = rb_entry(nd, struct symbol, rb_node);

		result[i++] = pos;
	}
	qsort(result, size, sizeof(*result), symbols__sort_name_cmp);
	*len = size;
	return result;
}

int symbol__match_symbol_name(const char *name, const char *str,
			      enum symbol_tag_include includes)
{
	const char *versioning;

	if (includes == SYMBOL_TAG_INCLUDE__DEFAULT_ONLY &&
	    (versioning = strstr(name, "@@"))) {
		int len = strlen(str);

		if (len < versioning - name)
			len = versioning - name;

		return arch__compare_symbol_names_n(name, str, len);
	} else
		return arch__compare_symbol_names(name, str);
}

static struct symbol *symbols__find_by_name(struct symbol *symbols[],
					    size_t symbols_len,
					    const char *name,
					    enum symbol_tag_include includes,
					    size_t *found_idx)
{
	size_t i, lower = 0, upper = symbols_len;
	struct symbol *s = NULL;

	if (found_idx)
		*found_idx = SIZE_MAX;

	if (!symbols_len)
		return NULL;

	while (lower < upper) {
		int cmp;

		i = (lower + upper) / 2;
		cmp = symbol__match_symbol_name(symbols[i]->name, name, includes);

		if (cmp > 0)
			upper = i;
		else if (cmp < 0)
			lower = i + 1;
		else {
			if (found_idx)
				*found_idx = i;
			s = symbols[i];
			break;
		}
	}
	if (s && includes != SYMBOL_TAG_INCLUDE__DEFAULT_ONLY) {
		/* return first symbol that has same name (if any) */
		for (; i > 0; i--) {
			struct symbol *tmp = symbols[i - 1];

			if (!arch__compare_symbol_names(tmp->name, s->name)) {
				if (found_idx)
					*found_idx = i - 1;
				s = tmp;
			} else
				break;
		}
	}
	assert(!found_idx || !s || s == symbols[*found_idx]);
	return s;
}

void dso__reset_find_symbol_cache(struct dso *dso)
{
	dso__set_last_find_result_addr(dso, 0);
	dso__set_last_find_result_symbol(dso, NULL);
}

void dso__insert_symbol(struct dso *dso, struct symbol *sym)
{
	__symbols__insert(dso__symbols(dso), sym, dso__kernel(dso));

	/* update the symbol cache if necessary */
	if (dso__last_find_result_addr(dso) >= sym->start &&
	    (dso__last_find_result_addr(dso) < sym->end ||
	    sym->start == sym->end)) {
		dso__set_last_find_result_symbol(dso, sym);
	}
}

void dso__delete_symbol(struct dso *dso, struct symbol *sym)
{
	rb_erase_cached(&sym->rb_node, dso__symbols(dso));
	symbol__delete(sym);
	dso__reset_find_symbol_cache(dso);
}

struct symbol *dso__find_symbol(struct dso *dso, u64 addr)
{
	if (dso__last_find_result_addr(dso) != addr || dso__last_find_result_symbol(dso) == NULL) {
		dso__set_last_find_result_addr(dso, addr);
		dso__set_last_find_result_symbol(dso, symbols__find(dso__symbols(dso), addr));
	}

	return dso__last_find_result_symbol(dso);
}

struct symbol *dso__find_symbol_nocache(struct dso *dso, u64 addr)
{
	return symbols__find(dso__symbols(dso), addr);
}

struct symbol *dso__first_symbol(struct dso *dso)
{
	return symbols__first(dso__symbols(dso));
}

struct symbol *dso__last_symbol(struct dso *dso)
{
	return symbols__last(dso__symbols(dso));
}

struct symbol *dso__next_symbol(struct symbol *sym)
{
	return symbols__next(sym);
}

struct symbol *dso__next_symbol_by_name(struct dso *dso, size_t *idx)
{
	if (*idx + 1 >= dso__symbol_names_len(dso))
		return NULL;

	++*idx;
	return dso__symbol_names(dso)[*idx];
}

 /*
  * Returns first symbol that matched with @name.
  */
struct symbol *dso__find_symbol_by_name(struct dso *dso, const char *name, size_t *idx)
{
	struct symbol *s = symbols__find_by_name(dso__symbol_names(dso),
						 dso__symbol_names_len(dso),
						 name, SYMBOL_TAG_INCLUDE__NONE, idx);
	if (!s) {
		s = symbols__find_by_name(dso__symbol_names(dso), dso__symbol_names_len(dso),
					  name, SYMBOL_TAG_INCLUDE__DEFAULT_ONLY, idx);
	}
	return s;
}

void dso__sort_by_name(struct dso *dso)
{
	mutex_lock(dso__lock(dso));
	if (!dso__sorted_by_name(dso)) {
		size_t len = 0;

		dso__set_symbol_names(dso, symbols__sort_by_name(dso__symbols(dso), &len));
		if (dso__symbol_names(dso)) {
			dso__set_symbol_names_len(dso, len);
			dso__set_sorted_by_name(dso);
		}
	}
	mutex_unlock(dso__lock(dso));
}

/*
 * While we find nice hex chars, build a long_val.
 * Return number of chars processed.
 */
static int hex2u64(const char *ptr, u64 *long_val)
{
	char *p;

	*long_val = strtoull(ptr, &p, 16);

	return p - ptr;
}


int modules__parse(const char *filename, void *arg,
		   int (*process_module)(void *arg, const char *name,
					 u64 start, u64 size))
{
	char *line = NULL;
	size_t n;
	FILE *file;
	int err = 0;

	file = fopen(filename, "r");
	if (file == NULL)
		return -1;

	while (1) {
		char name[PATH_MAX];
		u64 start, size;
		char *sep, *endptr;
		ssize_t line_len;

		line_len = getline(&line, &n, file);
		if (line_len < 0) {
			if (feof(file))
				break;
			err = -1;
			goto out;
		}

		if (!line) {
			err = -1;
			goto out;
		}

		line[--line_len] = '\0'; /* \n */

		sep = strrchr(line, 'x');
		if (sep == NULL)
			continue;

		hex2u64(sep + 1, &start);

		sep = strchr(line, ' ');
		if (sep == NULL)
			continue;

		*sep = '\0';

		scnprintf(name, sizeof(name), "[%s]", line);

		size = strtoul(sep + 1, &endptr, 0);
		if (*endptr != ' ' && *endptr != '\t')
			continue;

		err = process_module(arg, name, start, size);
		if (err)
			break;
	}
out:
	free(line);
	fclose(file);
	return err;
}

/*
 * These are symbols in the kernel image, so make sure that
 * sym is from a kernel DSO.
 */
static bool symbol__is_idle(const char *name)
{
	const char * const idle_symbols[] = {
		"acpi_idle_do_entry",
		"acpi_processor_ffh_cstate_enter",
		"arch_cpu_idle",
		"cpu_idle",
		"cpu_startup_entry",
		"idle_cpu",
		"intel_idle",
		"intel_idle_ibrs",
		"default_idle",
		"native_safe_halt",
		"enter_idle",
		"exit_idle",
		"mwait_idle",
		"mwait_idle_with_hints",
		"mwait_idle_with_hints.constprop.0",
		"poll_idle",
		"ppc64_runlatch_off",
		"pseries_dedicated_idle_sleep",
		"psw_idle",
		"psw_idle_exit",
		NULL
	};
	int i;
	static struct strlist *idle_symbols_list;

	if (idle_symbols_list)
		return strlist__has_entry(idle_symbols_list, name);

	idle_symbols_list = strlist__new(NULL, NULL);

	for (i = 0; idle_symbols[i]; i++)
		strlist__add(idle_symbols_list, idle_symbols[i]);

	return strlist__has_entry(idle_symbols_list, name);
}

static int map__process_kallsym_symbol(void *arg, const char *name,
				       char type, u64 start)
{
	struct symbol *sym;
	struct dso *dso = arg;
	struct rb_root_cached *root = dso__symbols(dso);

	if (!symbol_type__filter(type))
		return 0;

	/* Ignore local symbols for ARM modules */
	if (name[0] == '$')
		return 0;

	/*
	 * module symbols are not sorted so we add all
	 * symbols, setting length to 0, and rely on
	 * symbols__fixup_end() to fix it up.
	 */
	sym = symbol__new(start, 0, kallsyms2elf_binding(type), kallsyms2elf_type(type), name);
	if (sym == NULL)
		return -ENOMEM;
	/*
	 * We will pass the symbols to the filter later, in
	 * map__split_kallsyms, when we have split the maps per module
	 */
	__symbols__insert(root, sym, !strchr(name, '['));

	return 0;
}

/*
 * Loads the function entries in /proc/kallsyms into kernel_map->dso,
 * so that we can in the next step set the symbol ->end address and then
 * call kernel_maps__split_kallsyms.
 */
static int dso__load_all_kallsyms(struct dso *dso, const char *filename)
{
	return kallsyms__parse(filename, dso, map__process_kallsym_symbol);
}

static int maps__split_kallsyms_for_kcore(struct maps *kmaps, struct dso *dso)
{
	struct symbol *pos;
	int count = 0;
	struct rb_root_cached *root = dso__symbols(dso);
	struct rb_root_cached old_root = *root;
	struct rb_node *next = rb_first_cached(root);

	if (!kmaps)
		return -1;

	*root = RB_ROOT_CACHED;

	while (next) {
		struct map *curr_map;
		struct dso *curr_map_dso;
		char *module;

		pos = rb_entry(next, struct symbol, rb_node);
		next = rb_next(&pos->rb_node);

		rb_erase_cached(&pos->rb_node, &old_root);
		RB_CLEAR_NODE(&pos->rb_node);
		module = strchr(pos->name, '\t');
		if (module)
			*module = '\0';

		curr_map = maps__find(kmaps, pos->start);

		if (!curr_map) {
			symbol__delete(pos);
			continue;
		}
		curr_map_dso = map__dso(curr_map);
		pos->start -= map__start(curr_map) - map__pgoff(curr_map);
		if (pos->end > map__end(curr_map))
			pos->end = map__end(curr_map);
		if (pos->end)
			pos->end -= map__start(curr_map) - map__pgoff(curr_map);
		symbols__insert(dso__symbols(curr_map_dso), pos);
		++count;
		map__put(curr_map);
	}

	/* Symbols have been adjusted */
	dso__set_adjust_symbols(dso, true);

	return count;
}

/*
 * Split the symbols into maps, making sure there are no overlaps, i.e. the
 * kernel range is broken in several maps, named [kernel].N, as we don't have
 * the original ELF section names vmlinux have.
 */
static int maps__split_kallsyms(struct maps *kmaps, struct dso *dso, u64 delta,
				struct map *initial_map)
{
	struct machine *machine;
	struct map *curr_map = map__get(initial_map);
	struct symbol *pos;
	int count = 0, moved = 0;
	struct rb_root_cached *root = dso__symbols(dso);
	struct rb_node *next = rb_first_cached(root);
	int kernel_range = 0;
	bool x86_64;

	if (!kmaps)
		return -1;

	machine = maps__machine(kmaps);

	x86_64 = machine__is(machine, "x86_64");

	while (next) {
		char *module;

		pos = rb_entry(next, struct symbol, rb_node);
		next = rb_next(&pos->rb_node);

		module = strchr(pos->name, '\t');
		if (module) {
			struct dso *curr_map_dso;

			if (!symbol_conf.use_modules)
				goto discard_symbol;

			*module++ = '\0';
			curr_map_dso = map__dso(curr_map);
			if (strcmp(dso__short_name(curr_map_dso), module)) {
				if (!RC_CHK_EQUAL(curr_map, initial_map) &&
				    dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST &&
				    machine__is_default_guest(machine)) {
					/*
					 * We assume all symbols of a module are
					 * continuous in * kallsyms, so curr_map
					 * points to a module and all its
					 * symbols are in its kmap. Mark it as
					 * loaded.
					 */
					dso__set_loaded(curr_map_dso);
				}

				map__zput(curr_map);
				curr_map = maps__find_by_name(kmaps, module);
				if (curr_map == NULL) {
					pr_debug("%s/proc/{kallsyms,modules} "
					         "inconsistency while looking "
						 "for \"%s\" module!\n",
						 machine->root_dir, module);
					curr_map = map__get(initial_map);
					goto discard_symbol;
				}
				curr_map_dso = map__dso(curr_map);
				if (dso__loaded(curr_map_dso) &&
				    !machine__is_default_guest(machine))
					goto discard_symbol;
			}
			/*
			 * So that we look just like we get from .ko files,
			 * i.e. not prelinked, relative to initial_map->start.
			 */
			pos->start = map__map_ip(curr_map, pos->start);
			pos->end   = map__map_ip(curr_map, pos->end);
		} else if (x86_64 && is_entry_trampoline(pos->name)) {
			/*
			 * These symbols are not needed anymore since the
			 * trampoline maps refer to the text section and it's
			 * symbols instead. Avoid having to deal with
			 * relocations, and the assumption that the first symbol
			 * is the start of kernel text, by simply removing the
			 * symbols at this point.
			 */
			goto discard_symbol;
		} else if (!RC_CHK_EQUAL(curr_map, initial_map)) {
			char dso_name[PATH_MAX];
			struct dso *ndso;

			if (delta) {
				/* Kernel was relocated at boot time */
				pos->start -= delta;
				pos->end -= delta;
			}

			if (count == 0) {
				map__zput(curr_map);
				curr_map = map__get(initial_map);
				goto add_symbol;
			}

			if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
				snprintf(dso_name, sizeof(dso_name),
					"[guest.kernel].%d",
					kernel_range++);
			else
				snprintf(dso_name, sizeof(dso_name),
					"[kernel].%d",
					kernel_range++);

			ndso = dso__new(dso_name);
			map__zput(curr_map);
			if (ndso == NULL)
				return -1;

			dso__set_kernel(ndso, dso__kernel(dso));

			curr_map = map__new2(pos->start, ndso);
			if (curr_map == NULL) {
				dso__put(ndso);
				return -1;
			}

			map__set_mapping_type(curr_map, MAPPING_TYPE__IDENTITY);
			if (maps__insert(kmaps, curr_map)) {
				map__zput(curr_map);
				dso__put(ndso);
				return -1;
			}
			++kernel_range;
		} else if (delta) {
			/* Kernel was relocated at boot time */
			pos->start -= delta;
			pos->end -= delta;
		}
add_symbol:
		if (!RC_CHK_EQUAL(curr_map, initial_map)) {
			struct dso *curr_map_dso = map__dso(curr_map);

			rb_erase_cached(&pos->rb_node, root);
			symbols__insert(dso__symbols(curr_map_dso), pos);
			++moved;
		} else
			++count;

		continue;
discard_symbol:
		rb_erase_cached(&pos->rb_node, root);
		symbol__delete(pos);
	}

	if (!RC_CHK_EQUAL(curr_map, initial_map) &&
	    dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST &&
	    machine__is_default_guest(maps__machine(kmaps))) {
		dso__set_loaded(map__dso(curr_map));
	}
	map__put(curr_map);
	return count + moved;
}

bool symbol__restricted_filename(const char *filename,
				 const char *restricted_filename)
{
	bool restricted = false;

	if (symbol_conf.kptr_restrict) {
		char *r = realpath(filename, NULL);

		if (r != NULL) {
			restricted = strcmp(r, restricted_filename) == 0;
			free(r);
			return restricted;
		}
	}

	return restricted;
}

struct module_info {
	struct rb_node rb_node;
	char *name;
	u64 start;
};

static void add_module(struct module_info *mi, struct rb_root *modules)
{
	struct rb_node **p = &modules->rb_node;
	struct rb_node *parent = NULL;
	struct module_info *m;

	while (*p != NULL) {
		parent = *p;
		m = rb_entry(parent, struct module_info, rb_node);
		if (strcmp(mi->name, m->name) < 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}
	rb_link_node(&mi->rb_node, parent, p);
	rb_insert_color(&mi->rb_node, modules);
}

static void delete_modules(struct rb_root *modules)
{
	struct module_info *mi;
	struct rb_node *next = rb_first(modules);

	while (next) {
		mi = rb_entry(next, struct module_info, rb_node);
		next = rb_next(&mi->rb_node);
		rb_erase(&mi->rb_node, modules);
		zfree(&mi->name);
		free(mi);
	}
}

static struct module_info *find_module(const char *name,
				       struct rb_root *modules)
{
	struct rb_node *n = modules->rb_node;

	while (n) {
		struct module_info *m;
		int cmp;

		m = rb_entry(n, struct module_info, rb_node);
		cmp = strcmp(name, m->name);
		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return m;
	}

	return NULL;
}

static int __read_proc_modules(void *arg, const char *name, u64 start,
			       u64 size __maybe_unused)
{
	struct rb_root *modules = arg;
	struct module_info *mi;

	mi = zalloc(sizeof(struct module_info));
	if (!mi)
		return -ENOMEM;

	mi->name = strdup(name);
	mi->start = start;

	if (!mi->name) {
		free(mi);
		return -ENOMEM;
	}

	add_module(mi, modules);

	return 0;
}

static int read_proc_modules(const char *filename, struct rb_root *modules)
{
	if (symbol__restricted_filename(filename, "/proc/modules"))
		return -1;

	if (modules__parse(filename, modules, __read_proc_modules)) {
		delete_modules(modules);
		return -1;
	}

	return 0;
}

int compare_proc_modules(const char *from, const char *to)
{
	struct rb_root from_modules = RB_ROOT;
	struct rb_root to_modules = RB_ROOT;
	struct rb_node *from_node, *to_node;
	struct module_info *from_m, *to_m;
	int ret = -1;

	if (read_proc_modules(from, &from_modules))
		return -1;

	if (read_proc_modules(to, &to_modules))
		goto out_delete_from;

	from_node = rb_first(&from_modules);
	to_node = rb_first(&to_modules);
	while (from_node) {
		if (!to_node)
			break;

		from_m = rb_entry(from_node, struct module_info, rb_node);
		to_m = rb_entry(to_node, struct module_info, rb_node);

		if (from_m->start != to_m->start ||
		    strcmp(from_m->name, to_m->name))
			break;

		from_node = rb_next(from_node);
		to_node = rb_next(to_node);
	}

	if (!from_node && !to_node)
		ret = 0;

	delete_modules(&to_modules);
out_delete_from:
	delete_modules(&from_modules);

	return ret;
}

static int do_validate_kcore_modules_cb(struct map *old_map, void *data)
{
	struct rb_root *modules = data;
	struct module_info *mi;
	struct dso *dso;

	if (!__map__is_kmodule(old_map))
		return 0;

	dso = map__dso(old_map);
	/* Module must be in memory at the same address */
	mi = find_module(dso__short_name(dso), modules);
	if (!mi || mi->start != map__start(old_map))
		return -EINVAL;

	return 0;
}

static int do_validate_kcore_modules(const char *filename, struct maps *kmaps)
{
	struct rb_root modules = RB_ROOT;
	int err;

	err = read_proc_modules(filename, &modules);
	if (err)
		return err;

	err = maps__for_each_map(kmaps, do_validate_kcore_modules_cb, &modules);

	delete_modules(&modules);
	return err;
}

/*
 * If kallsyms is referenced by name then we look for filename in the same
 * directory.
 */
static bool filename_from_kallsyms_filename(char *filename,
					    const char *base_name,
					    const char *kallsyms_filename)
{
	char *name;

	strcpy(filename, kallsyms_filename);
	name = strrchr(filename, '/');
	if (!name)
		return false;

	name += 1;

	if (!strcmp(name, "kallsyms")) {
		strcpy(name, base_name);
		return true;
	}

	return false;
}

static int validate_kcore_modules(const char *kallsyms_filename,
				  struct map *map)
{
	struct maps *kmaps = map__kmaps(map);
	char modules_filename[PATH_MAX];

	if (!kmaps)
		return -EINVAL;

	if (!filename_from_kallsyms_filename(modules_filename, "modules",
					     kallsyms_filename))
		return -EINVAL;

	if (do_validate_kcore_modules(modules_filename, kmaps))
		return -EINVAL;

	return 0;
}

static int validate_kcore_addresses(const char *kallsyms_filename,
				    struct map *map)
{
	struct kmap *kmap = map__kmap(map);

	if (!kmap)
		return -EINVAL;

	if (kmap->ref_reloc_sym && kmap->ref_reloc_sym->name) {
		u64 start;

		if (kallsyms__get_function_start(kallsyms_filename,
						 kmap->ref_reloc_sym->name, &start))
			return -ENOENT;
		if (start != kmap->ref_reloc_sym->addr)
			return -EINVAL;
	}

	return validate_kcore_modules(kallsyms_filename, map);
}

struct kcore_mapfn_data {
	struct dso *dso;
	struct list_head maps;
};

static int kcore_mapfn(u64 start, u64 len, u64 pgoff, void *data)
{
	struct kcore_mapfn_data *md = data;
	struct map_list_node *list_node = map_list_node__new();

	if (!list_node)
		return -ENOMEM;

	list_node->map = map__new2(start, md->dso);
	if (!list_node->map) {
		free(list_node);
		return -ENOMEM;
	}

	map__set_end(list_node->map, map__start(list_node->map) + len);
	map__set_pgoff(list_node->map, pgoff);

	list_add(&list_node->node, &md->maps);

	return 0;
}

static bool remove_old_maps(struct map *map, void *data)
{
	const struct map *map_to_save = data;

	/*
	 * We need to preserve eBPF maps even if they are covered by kcore,
	 * because we need to access eBPF dso for source data.
	 */
	return !RC_CHK_EQUAL(map, map_to_save) && !__map__is_bpf_prog(map);
}

static int dso__load_kcore(struct dso *dso, struct map *map,
			   const char *kallsyms_filename)
{
	struct maps *kmaps = map__kmaps(map);
	struct kcore_mapfn_data md;
	struct map *map_ref, *replacement_map = NULL;
	struct machine *machine;
	bool is_64_bit;
	int err, fd;
	char kcore_filename[PATH_MAX];
	u64 stext;

	if (!kmaps)
		return -EINVAL;

	machine = maps__machine(kmaps);

	/* This function requires that the map is the kernel map */
	if (!__map__is_kernel(map))
		return -EINVAL;

	if (!filename_from_kallsyms_filename(kcore_filename, "kcore",
					     kallsyms_filename))
		return -EINVAL;

	/* Modules and kernel must be present at their original addresses */
	if (validate_kcore_addresses(kallsyms_filename, map))
		return -EINVAL;

	md.dso = dso;
	INIT_LIST_HEAD(&md.maps);

	fd = open(kcore_filename, O_RDONLY);
	if (fd < 0) {
		pr_debug("Failed to open %s. Note /proc/kcore requires CAP_SYS_RAWIO capability to access.\n",
			 kcore_filename);
		return -EINVAL;
	}

	/* Read new maps into temporary lists */
	err = file__read_maps(fd, map__prot(map) & PROT_EXEC, kcore_mapfn, &md,
			      &is_64_bit);
	if (err)
		goto out_err;
	dso__set_is_64_bit(dso, is_64_bit);

	if (list_empty(&md.maps)) {
		err = -EINVAL;
		goto out_err;
	}

	/* Remove old maps */
	maps__remove_maps(kmaps, remove_old_maps, map);
	machine->trampolines_mapped = false;

	/* Find the kernel map using the '_stext' symbol */
	if (!kallsyms__get_function_start(kallsyms_filename, "_stext", &stext)) {
		u64 replacement_size = 0;
		struct map_list_node *new_node;

		list_for_each_entry(new_node, &md.maps, node) {
			struct map *new_map = new_node->map;
			u64 new_size = map__size(new_map);

			if (!(stext >= map__start(new_map) && stext < map__end(new_map)))
				continue;

			/*
			 * On some architectures, ARM64 for example, the kernel
			 * text can get allocated inside of the vmalloc segment.
			 * Select the smallest matching segment, in case stext
			 * falls within more than one in the list.
			 */
			if (!replacement_map || new_size < replacement_size) {
				replacement_map = new_map;
				replacement_size = new_size;
			}
		}
	}

	if (!replacement_map)
		replacement_map = list_entry(md.maps.next, struct map_list_node, node)->map;

	/*
	 * Update addresses of vmlinux map. Re-insert it to ensure maps are
	 * correctly ordered. Do this before using maps__merge_in() for the
	 * remaining maps so vmlinux gets split if necessary.
	 */
	map_ref = map__get(map);
	maps__remove(kmaps, map_ref);

	map__set_start(map_ref, map__start(replacement_map));
	map__set_end(map_ref, map__end(replacement_map));
	map__set_pgoff(map_ref, map__pgoff(replacement_map));
	map__set_mapping_type(map_ref, map__mapping_type(replacement_map));

	err = maps__insert(kmaps, map_ref);
	map__put(map_ref);
	if (err)
		goto out_err;

	/* Add new maps */
	while (!list_empty(&md.maps)) {
		struct map_list_node *new_node = list_entry(md.maps.next, struct map_list_node, node);
		struct map *new_map = new_node->map;

		list_del_init(&new_node->node);

		/* skip if replacement_map, already inserted above */
		if (!RC_CHK_EQUAL(new_map, replacement_map)) {
			/*
			 * Merge kcore map into existing maps,
			 * and ensure that current maps (eBPF)
			 * stay intact.
			 */
			if (maps__merge_in(kmaps, new_map)) {
				err = -EINVAL;
				goto out_err;
			}
		}
		map__zput(new_node->map);
		free(new_node);
	}

	if (machine__is(machine, "x86_64")) {
		u64 addr;

		/*
		 * If one of the corresponding symbols is there, assume the
		 * entry trampoline maps are too.
		 */
		if (!kallsyms__get_function_start(kallsyms_filename,
						  ENTRY_TRAMPOLINE_NAME,
						  &addr))
			machine->trampolines_mapped = true;
	}

	/*
	 * Set the data type and long name so that kcore can be read via
	 * dso__data_read_addr().
	 */
	if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
		dso__set_binary_type(dso, DSO_BINARY_TYPE__GUEST_KCORE);
	else
		dso__set_binary_type(dso, DSO_BINARY_TYPE__KCORE);
	dso__set_long_name(dso, strdup(kcore_filename), true);

	close(fd);

	if (map__prot(map) & PROT_EXEC)
		pr_debug("Using %s for kernel object code\n", kcore_filename);
	else
		pr_debug("Using %s for kernel data\n", kcore_filename);

	return 0;

out_err:
	while (!list_empty(&md.maps)) {
		struct map_list_node *list_node;

		list_node = list_entry(md.maps.next, struct map_list_node, node);
		list_del_init(&list_node->node);
		map__zput(list_node->map);
		free(list_node);
	}
	close(fd);
	return err;
}

/*
 * If the kernel is relocated at boot time, kallsyms won't match.  Compute the
 * delta based on the relocation reference symbol.
 */
static int kallsyms__delta(struct kmap *kmap, const char *filename, u64 *delta)
{
	u64 addr;

	if (!kmap->ref_reloc_sym || !kmap->ref_reloc_sym->name)
		return 0;

	if (kallsyms__get_function_start(filename, kmap->ref_reloc_sym->name, &addr))
		return -1;

	*delta = addr - kmap->ref_reloc_sym->addr;
	return 0;
}

int __dso__load_kallsyms(struct dso *dso, const char *filename,
			 struct map *map, bool no_kcore)
{
	struct kmap *kmap = map__kmap(map);
	u64 delta = 0;

	if (symbol__restricted_filename(filename, "/proc/kallsyms"))
		return -1;

	if (!kmap || !kmap->kmaps)
		return -1;

	if (dso__load_all_kallsyms(dso, filename) < 0)
		return -1;

	if (kallsyms__delta(kmap, filename, &delta))
		return -1;

	symbols__fixup_end(dso__symbols(dso), true);
	symbols__fixup_duplicate(dso__symbols(dso));

	if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
		dso__set_symtab_type(dso, DSO_BINARY_TYPE__GUEST_KALLSYMS);
	else
		dso__set_symtab_type(dso, DSO_BINARY_TYPE__KALLSYMS);

	if (!no_kcore && !dso__load_kcore(dso, map, filename))
		return maps__split_kallsyms_for_kcore(kmap->kmaps, dso);
	else
		return maps__split_kallsyms(kmap->kmaps, dso, delta, map);
}

int dso__load_kallsyms(struct dso *dso, const char *filename,
		       struct map *map)
{
	return __dso__load_kallsyms(dso, filename, map, false);
}

static int dso__load_perf_map(const char *map_path, struct dso *dso)
{
	char *line = NULL;
	size_t n;
	FILE *file;
	int nr_syms = 0;

	file = fopen(map_path, "r");
	if (file == NULL)
		goto out_failure;

	while (!feof(file)) {
		u64 start, size;
		struct symbol *sym;
		int line_len, len;

		line_len = getline(&line, &n, file);
		if (line_len < 0)
			break;

		if (!line)
			goto out_failure;

		line[--line_len] = '\0'; /* \n */

		len = hex2u64(line, &start);

		len++;
		if (len + 2 >= line_len)
			continue;

		len += hex2u64(line + len, &size);

		len++;
		if (len + 2 >= line_len)
			continue;

		sym = symbol__new(start, size, STB_GLOBAL, STT_FUNC, line + len);

		if (sym == NULL)
			goto out_delete_line;

		symbols__insert(dso__symbols(dso), sym);
		nr_syms++;
	}

	free(line);
	fclose(file);

	return nr_syms;

out_delete_line:
	free(line);
out_failure:
	return -1;
}

#ifdef HAVE_LIBBFD_SUPPORT
#define PACKAGE 'perf'
#include <bfd.h>

static int bfd_symbols__cmpvalue(const void *a, const void *b)
{
	const asymbol *as = *(const asymbol **)a, *bs = *(const asymbol **)b;

	if (bfd_asymbol_value(as) != bfd_asymbol_value(bs))
		return bfd_asymbol_value(as) - bfd_asymbol_value(bs);

	return bfd_asymbol_name(as)[0] - bfd_asymbol_name(bs)[0];
}

static int bfd2elf_binding(asymbol *symbol)
{
	if (symbol->flags & BSF_WEAK)
		return STB_WEAK;
	if (symbol->flags & BSF_GLOBAL)
		return STB_GLOBAL;
	if (symbol->flags & BSF_LOCAL)
		return STB_LOCAL;
	return -1;
}

int dso__load_bfd_symbols(struct dso *dso, const char *debugfile)
{
	int err = -1;
	long symbols_size, symbols_count, i;
	asection *section;
	asymbol **symbols, *sym;
	struct symbol *symbol;
	bfd *abfd;
	u64 start, len;

	abfd = bfd_openr(debugfile, NULL);
	if (!abfd)
		return -1;

	if (!bfd_check_format(abfd, bfd_object)) {
		pr_debug2("%s: cannot read %s bfd file.\n", __func__,
			  dso__long_name(dso));
		goto out_close;
	}

	if (bfd_get_flavour(abfd) == bfd_target_elf_flavour)
		goto out_close;

	symbols_size = bfd_get_symtab_upper_bound(abfd);
	if (symbols_size == 0) {
		bfd_close(abfd);
		return 0;
	}

	if (symbols_size < 0)
		goto out_close;

	symbols = malloc(symbols_size);
	if (!symbols)
		goto out_close;

	symbols_count = bfd_canonicalize_symtab(abfd, symbols);
	if (symbols_count < 0)
		goto out_free;

	section = bfd_get_section_by_name(abfd, ".text");
	if (section) {
		for (i = 0; i < symbols_count; ++i) {
			if (!strcmp(bfd_asymbol_name(symbols[i]), "__ImageBase") ||
			    !strcmp(bfd_asymbol_name(symbols[i]), "__image_base__"))
				break;
		}
		if (i < symbols_count) {
			/* PE symbols can only have 4 bytes, so use .text high bits */
			u64 text_offset = (section->vma - (u32)section->vma)
				+ (u32)bfd_asymbol_value(symbols[i]);
			dso__set_text_offset(dso, text_offset);
			dso__set_text_end(dso, (section->vma - text_offset) + section->size);
		} else {
			dso__set_text_offset(dso, section->vma - section->filepos);
			dso__set_text_end(dso, section->filepos + section->size);
		}
	}

	qsort(symbols, symbols_count, sizeof(asymbol *), bfd_symbols__cmpvalue);

#ifdef bfd_get_section
#define bfd_asymbol_section bfd_get_section
#endif
	for (i = 0; i < symbols_count; ++i) {
		sym = symbols[i];
		section = bfd_asymbol_section(sym);
		if (bfd2elf_binding(sym) < 0)
			continue;

		while (i + 1 < symbols_count &&
		       bfd_asymbol_section(symbols[i + 1]) == section &&
		       bfd2elf_binding(symbols[i + 1]) < 0)
			i++;

		if (i + 1 < symbols_count &&
		    bfd_asymbol_section(symbols[i + 1]) == section)
			len = symbols[i + 1]->value - sym->value;
		else
			len = section->size - sym->value;

		start = bfd_asymbol_value(sym) - dso__text_offset(dso);
		symbol = symbol__new(start, len, bfd2elf_binding(sym), STT_FUNC,
				     bfd_asymbol_name(sym));
		if (!symbol)
			goto out_free;

		symbols__insert(dso__symbols(dso), symbol);
	}
#ifdef bfd_get_section
#undef bfd_asymbol_section
#endif

	symbols__fixup_end(dso__symbols(dso), false);
	symbols__fixup_duplicate(dso__symbols(dso));
	dso__set_adjust_symbols(dso, true);

	err = 0;
out_free:
	free(symbols);
out_close:
	bfd_close(abfd);
	return err;
}
#endif

static bool dso__is_compatible_symtab_type(struct dso *dso, bool kmod,
					   enum dso_binary_type type)
{
	switch (type) {
	case DSO_BINARY_TYPE__JAVA_JIT:
	case DSO_BINARY_TYPE__DEBUGLINK:
	case DSO_BINARY_TYPE__SYSTEM_PATH_DSO:
	case DSO_BINARY_TYPE__FEDORA_DEBUGINFO:
	case DSO_BINARY_TYPE__UBUNTU_DEBUGINFO:
	case DSO_BINARY_TYPE__MIXEDUP_UBUNTU_DEBUGINFO:
	case DSO_BINARY_TYPE__BUILDID_DEBUGINFO:
	case DSO_BINARY_TYPE__OPENEMBEDDED_DEBUGINFO:
	case DSO_BINARY_TYPE__GNU_DEBUGDATA:
		return !kmod && dso__kernel(dso) == DSO_SPACE__USER;

	case DSO_BINARY_TYPE__KALLSYMS:
	case DSO_BINARY_TYPE__VMLINUX:
	case DSO_BINARY_TYPE__KCORE:
		return dso__kernel(dso) == DSO_SPACE__KERNEL;

	case DSO_BINARY_TYPE__GUEST_KALLSYMS:
	case DSO_BINARY_TYPE__GUEST_VMLINUX:
	case DSO_BINARY_TYPE__GUEST_KCORE:
		return dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST;

	case DSO_BINARY_TYPE__GUEST_KMODULE:
	case DSO_BINARY_TYPE__GUEST_KMODULE_COMP:
	case DSO_BINARY_TYPE__SYSTEM_PATH_KMODULE:
	case DSO_BINARY_TYPE__SYSTEM_PATH_KMODULE_COMP:
		/*
		 * kernel modules know their symtab type - it's set when
		 * creating a module dso in machine__addnew_module_map().
		 */
		return kmod && dso__symtab_type(dso) == type;

	case DSO_BINARY_TYPE__BUILD_ID_CACHE:
	case DSO_BINARY_TYPE__BUILD_ID_CACHE_DEBUGINFO:
		return true;

	case DSO_BINARY_TYPE__BPF_PROG_INFO:
	case DSO_BINARY_TYPE__BPF_IMAGE:
	case DSO_BINARY_TYPE__OOL:
	case DSO_BINARY_TYPE__NOT_FOUND:
	default:
		return false;
	}
}

/* Checks for the existence of the perf-<pid>.map file in two different
 * locations.  First, if the process is a separate mount namespace, check in
 * that namespace using the pid of the innermost pid namespace.  If's not in a
 * namespace, or the file can't be found there, try in the mount namespace of
 * the tracing process using our view of its pid.
 */
static int dso__find_perf_map(char *filebuf, size_t bufsz,
			      struct nsinfo **nsip)
{
	struct nscookie nsc;
	struct nsinfo *nsi;
	struct nsinfo *nnsi;
	int rc = -1;

	nsi = *nsip;

	if (nsinfo__need_setns(nsi)) {
		snprintf(filebuf, bufsz, "/tmp/perf-%d.map", nsinfo__nstgid(nsi));
		nsinfo__mountns_enter(nsi, &nsc);
		rc = access(filebuf, R_OK);
		nsinfo__mountns_exit(&nsc);
		if (rc == 0)
			return rc;
	}

	nnsi = nsinfo__copy(nsi);
	if (nnsi) {
		nsinfo__put(nsi);

		nsinfo__clear_need_setns(nnsi);
		snprintf(filebuf, bufsz, "/tmp/perf-%d.map", nsinfo__tgid(nnsi));
		*nsip = nnsi;
		rc = 0;
	}

	return rc;
}

int dso__load(struct dso *dso, struct map *map)
{
	char *name;
	int ret = -1;
	u_int i;
	struct machine *machine = NULL;
	char *root_dir = (char *) "";
	int ss_pos = 0;
	struct symsrc ss_[2];
	struct symsrc *syms_ss = NULL, *runtime_ss = NULL;
	bool kmod;
	bool perfmap;
	struct nscookie nsc;
	char newmapname[PATH_MAX];
	const char *map_path = dso__long_name(dso);

	mutex_lock(dso__lock(dso));
	perfmap = is_perf_pid_map_name(map_path);

	if (perfmap) {
		if (dso__nsinfo(dso) &&
		    (dso__find_perf_map(newmapname, sizeof(newmapname),
					dso__nsinfo_ptr(dso)) == 0)) {
			map_path = newmapname;
		}
	}

	nsinfo__mountns_enter(dso__nsinfo(dso), &nsc);

	/* check again under the dso->lock */
	if (dso__loaded(dso)) {
		ret = 1;
		goto out;
	}

	kmod = dso__is_kmod(dso);

	if (dso__kernel(dso) && !kmod) {
		if (dso__kernel(dso) == DSO_SPACE__KERNEL)
			ret = dso__load_kernel_sym(dso, map);
		else if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
			ret = dso__load_guest_kernel_sym(dso, map);

		machine = maps__machine(map__kmaps(map));
		if (machine__is(machine, "x86_64"))
			machine__map_x86_64_entry_trampolines(machine, dso);
		goto out;
	}

	dso__set_adjust_symbols(dso, false);

	if (perfmap) {
		ret = dso__load_perf_map(map_path, dso);
		dso__set_symtab_type(dso, ret > 0
				? DSO_BINARY_TYPE__JAVA_JIT
				: DSO_BINARY_TYPE__NOT_FOUND);
		goto out;
	}

	if (machine)
		root_dir = machine->root_dir;

	name = malloc(PATH_MAX);
	if (!name)
		goto out;

	/*
	 * Read the build id if possible. This is required for
	 * DSO_BINARY_TYPE__BUILDID_DEBUGINFO to work
	 */
	if (!dso__has_build_id(dso) &&
	    is_regular_file(dso__long_name(dso))) {
		struct build_id bid = { .size = 0, };

		__symbol__join_symfs(name, PATH_MAX, dso__long_name(dso));
		if (filename__read_build_id(name, &bid) > 0)
			dso__set_build_id(dso, &bid);
	}

	/*
	 * Iterate over candidate debug images.
	 * Keep track of "interesting" ones (those which have a symtab, dynsym,
	 * and/or opd section) for processing.
	 */
	for (i = 0; i < DSO_BINARY_TYPE__SYMTAB_CNT; i++) {
		struct symsrc *ss = &ss_[ss_pos];
		bool next_slot = false;
		bool is_reg;
		bool nsexit;
		int bfdrc = -1;
		int sirc = -1;

		enum dso_binary_type symtab_type = binary_type_symtab[i];

		nsexit = (symtab_type == DSO_BINARY_TYPE__BUILD_ID_CACHE ||
		    symtab_type == DSO_BINARY_TYPE__BUILD_ID_CACHE_DEBUGINFO);

		if (!dso__is_compatible_symtab_type(dso, kmod, symtab_type))
			continue;

		if (dso__read_binary_type_filename(dso, symtab_type,
						   root_dir, name, PATH_MAX))
			continue;

		if (nsexit)
			nsinfo__mountns_exit(&nsc);

		is_reg = is_regular_file(name);
		if (!is_reg && errno == ENOENT && dso__nsinfo(dso)) {
			char *new_name = dso__filename_with_chroot(dso, name);
			if (new_name) {
				is_reg = is_regular_file(new_name);
				strlcpy(name, new_name, PATH_MAX);
				free(new_name);
			}
		}

#ifdef HAVE_LIBBFD_SUPPORT
		if (is_reg)
			bfdrc = dso__load_bfd_symbols(dso, name);
#endif
		if (is_reg && bfdrc < 0)
			sirc = symsrc__init(ss, dso, name, symtab_type);

		if (nsexit)
			nsinfo__mountns_enter(dso__nsinfo(dso), &nsc);

		if (bfdrc == 0) {
			ret = 0;
			break;
		}

		if (!is_reg || sirc < 0)
			continue;

		if (!syms_ss && symsrc__has_symtab(ss)) {
			syms_ss = ss;
			next_slot = true;
			if (!dso__symsrc_filename(dso))
				dso__set_symsrc_filename(dso, strdup(name));
		}

		if (!runtime_ss && symsrc__possibly_runtime(ss)) {
			runtime_ss = ss;
			next_slot = true;
		}

		if (next_slot) {
			ss_pos++;

			if (dso__binary_type(dso) == DSO_BINARY_TYPE__NOT_FOUND)
				dso__set_binary_type(dso, symtab_type);

			if (syms_ss && runtime_ss)
				break;
		} else {
			symsrc__destroy(ss);
		}

	}

	if (!runtime_ss && !syms_ss)
		goto out_free;

	if (runtime_ss && !syms_ss) {
		syms_ss = runtime_ss;
	}

	/* We'll have to hope for the best */
	if (!runtime_ss && syms_ss)
		runtime_ss = syms_ss;

	if (syms_ss)
		ret = dso__load_sym(dso, map, syms_ss, runtime_ss, kmod);
	else
		ret = -1;

	if (ret > 0) {
		int nr_plt;

		nr_plt = dso__synthesize_plt_symbols(dso, runtime_ss);
		if (nr_plt > 0)
			ret += nr_plt;
	}

	for (; ss_pos > 0; ss_pos--)
		symsrc__destroy(&ss_[ss_pos - 1]);
out_free:
	free(name);
	if (ret < 0 && strstr(dso__name(dso), " (deleted)") != NULL)
		ret = 0;
out:
	dso__set_loaded(dso);
	mutex_unlock(dso__lock(dso));
	nsinfo__mountns_exit(&nsc);

	return ret;
}

/*
 * Always takes ownership of vmlinux when vmlinux_allocated == true, even if
 * it returns an error.
 */
int dso__load_vmlinux(struct dso *dso, struct map *map,
		      const char *vmlinux, bool vmlinux_allocated)
{
	int err = -1;
	struct symsrc ss;
	char symfs_vmlinux[PATH_MAX];
	enum dso_binary_type symtab_type;

	if (vmlinux[0] == '/')
		snprintf(symfs_vmlinux, sizeof(symfs_vmlinux), "%s", vmlinux);
	else
		symbol__join_symfs(symfs_vmlinux, vmlinux);

	if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
		symtab_type = DSO_BINARY_TYPE__GUEST_VMLINUX;
	else
		symtab_type = DSO_BINARY_TYPE__VMLINUX;

	if (symsrc__init(&ss, dso, symfs_vmlinux, symtab_type)) {
		if (vmlinux_allocated)
			free((char *) vmlinux);
		return -1;
	}

	/*
	 * dso__load_sym() may copy 'dso' which will result in the copies having
	 * an incorrect long name unless we set it here first.
	 */
	dso__set_long_name(dso, vmlinux, vmlinux_allocated);
	if (dso__kernel(dso) == DSO_SPACE__KERNEL_GUEST)
		dso__set_binary_type(dso, DSO_BINARY_TYPE__GUEST_VMLINUX);
	else
		dso__set_binary_type(dso, DSO_BINARY_TYPE__VMLINUX);

	err = dso__load_sym(dso, map, &ss, &ss, 0);
	symsrc__destroy(&ss);

	if (err > 0) {
		dso__set_loaded(dso);
		pr_debug("Using %s for symbols\n", symfs_vmlinux);
	}

	return err;
}

int dso__load_vmlinux_path(struct dso *dso, struct map *map)
{
	int i, err = 0;
	char *filename = NULL;

	pr_debug("Looking at the vmlinux_path (%d entries long)\n",
		 vmlinux_path__nr_entries + 1);

	for (i = 0; i < vmlinux_path__nr_entries; ++i) {
		err = dso__load_vmlinux(dso, map, vmlinux_path[i], false);
		if (err > 0)
			goto out;
	}

	if (!symbol_conf.ignore_vmlinux_buildid)
		filename = dso__build_id_filename(dso, NULL, 0, false);
	if (filename != NULL) {
		err = dso__load_vmlinux(dso, map, filename, true);
		if (err > 0)
			goto out;
	}
out:
	return err;
}

static bool visible_dir_filter(const char *name, struct dirent *d)
{
	if (d->d_type != DT_DIR)
		return false;
	return lsdir_no_dot_filter(name, d);
}

static int find_matching_kcore(struct map *map, char *dir, size_t dir_sz)
{
	char kallsyms_filename[PATH_MAX];
	int ret = -1;
	struct strlist *dirs;
	struct str_node *nd;

	dirs = lsdir(dir, visible_dir_filter);
	if (!dirs)
		return -1;

	strlist__for_each_entry(nd, dirs) {
		scnprintf(kallsyms_filename, sizeof(kallsyms_filename),
			  "%s/%s/kallsyms", dir, nd->s);
		if (!validate_kcore_addresses(kallsyms_filename, map)) {
			strlcpy(dir, kallsyms_filename, dir_sz);
			ret = 0;
			break;
		}
	}

	strlist__delete(dirs);

	return ret;
}

/*
 * Use open(O_RDONLY) to check readability directly instead of access(R_OK)
 * since access(R_OK) only checks with real UID/GID but open() use effective
 * UID/GID and actual capabilities (e.g. /proc/kcore requires CAP_SYS_RAWIO).
 */
static bool filename__readable(const char *file)
{
	int fd = open(file, O_RDONLY);
	if (fd < 0)
		return false;
	close(fd);
	return true;
}

static char *dso__find_kallsyms(struct dso *dso, struct map *map)
{
	struct build_id bid = { .size = 0, };
	char sbuild_id[SBUILD_ID_SIZE];
	bool is_host = false;
	char path[PATH_MAX];

	if (!dso__has_build_id(dso)) {
		/*
		 * Last resort, if we don't have a build-id and couldn't find
		 * any vmlinux file, try the running kernel kallsyms table.
		 */
		goto proc_kallsyms;
	}

	if (sysfs__read_build_id("/sys/kernel/notes", &bid) == 0)
		is_host = dso__build_id_equal(dso, &bid);

	/* Try a fast path for /proc/kallsyms if possible */
	if (is_host) {
		/*
		 * Do not check the build-id cache, unless we know we cannot use
		 * /proc/kcore or module maps don't match to /proc/kallsyms.
		 * To check readability of /proc/kcore, do not use access(R_OK)
		 * since /proc/kcore requires CAP_SYS_RAWIO to read and access
		 * can't check it.
		 */
		if (filename__readable("/proc/kcore") &&
		    !validate_kcore_addresses("/proc/kallsyms", map))
			goto proc_kallsyms;
	}

	build_id__snprintf(dso__bid(dso), sbuild_id, sizeof(sbuild_id));

	/* Find kallsyms in build-id cache with kcore */
	scnprintf(path, sizeof(path), "%s/%s/%s",
		  buildid_dir, DSO__NAME_KCORE, sbuild_id);

	if (!find_matching_kcore(map, path, sizeof(path)))
		return strdup(path);

	/* Use current /proc/kallsyms if possible */
	if (is_host) {
proc_kallsyms:
		return strdup("/proc/kallsyms");
	}

	/* Finally, find a cache of kallsyms */
	if (!build_id_cache__kallsyms_path(sbuild_id, path, sizeof(path))) {
		pr_err("No kallsyms or vmlinux with build-id %s was found\n",
		       sbuild_id);
		return NULL;
	}

	return strdup(path);
}

static int dso__load_kernel_sym(struct dso *dso, struct map *map)
{
	int err;
	const char *kallsyms_filename = NULL;
	char *kallsyms_allocated_filename = NULL;
	char *filename = NULL;

	/*
	 * Step 1: if the user specified a kallsyms or vmlinux filename, use
	 * it and only it, reporting errors to the user if it cannot be used.
	 *
	 * For instance, try to analyse an ARM perf.data file _without_ a
	 * build-id, or if the user specifies the wrong path to the right
	 * vmlinux file, obviously we can't fallback to another vmlinux (a
	 * x86_86 one, on the machine where analysis is being performed, say),
	 * or worse, /proc/kallsyms.
	 *
	 * If the specified file _has_ a build-id and there is a build-id
	 * section in the perf.data file, we will still do the expected
	 * validation in dso__load_vmlinux and will bail out if they don't
	 * match.
	 */
	if (symbol_conf.kallsyms_name != NULL) {
		kallsyms_filename = symbol_conf.kallsyms_name;
		goto do_kallsyms;
	}

	if (!symbol_conf.ignore_vmlinux && symbol_conf.vmlinux_name != NULL) {
		return dso__load_vmlinux(dso, map, symbol_conf.vmlinux_name, false);
	}

	/*
	 * Before checking on common vmlinux locations, check if it's
	 * stored as standard build id binary (not kallsyms) under
	 * .debug cache.
	 */
	if (!symbol_conf.ignore_vmlinux_buildid)
		filename = __dso__build_id_filename(dso, NULL, 0, false, false);
	if (filename != NULL) {
		err = dso__load_vmlinux(dso, map, filename, true);
		if (err > 0)
			return err;
	}

	if (!symbol_conf.ignore_vmlinux && vmlinux_path != NULL) {
		err = dso__load_vmlinux_path(dso, map);
		if (err > 0)
			return err;
	}

	/* do not try local files if a symfs was given */
	if (symbol_conf.symfs[0] != 0)
		return -1;

	kallsyms_allocated_filename = dso__find_kallsyms(dso, map);
	if (!kallsyms_allocated_filename)
		return -1;

	kallsyms_filename = kallsyms_allocated_filename;

do_kallsyms:
	err = dso__load_kallsyms(dso, kallsyms_filename, map);
	if (err > 0)
		pr_debug("Using %s for symbols\n", kallsyms_filename);
	free(kallsyms_allocated_filename);

	if (err > 0 && !dso__is_kcore(dso)) {
		dso__set_binary_type(dso, DSO_BINARY_TYPE__KALLSYMS);
		dso__set_long_name(dso, DSO__NAME_KALLSYMS, false);
		map__fixup_start(map);
		map__fixup_end(map);
	}

	return err;
}

static int dso__load_guest_kernel_sym(struct dso *dso, struct map *map)
{
	int err;
	const char *kallsyms_filename;
	struct machine *machine = maps__machine(map__kmaps(map));
	char path[PATH_MAX];

	if (machine->kallsyms_filename) {
		kallsyms_filename = machine->kallsyms_filename;
	} else if (machine__is_default_guest(machine)) {
		/*
		 * if the user specified a vmlinux filename, use it and only
		 * it, reporting errors to the user if it cannot be used.
		 * Or use file guest_kallsyms inputted by user on commandline
		 */
		if (symbol_conf.default_guest_vmlinux_name != NULL) {
			err = dso__load_vmlinux(dso, map,
						symbol_conf.default_guest_vmlinux_name,
						false);
			return err;
		}

		kallsyms_filename = symbol_conf.default_guest_kallsyms;
		if (!kallsyms_filename)
			return -1;
	} else {
		sprintf(path, "%s/proc/kallsyms", machine->root_dir);
		kallsyms_filename = path;
	}

	err = dso__load_kallsyms(dso, kallsyms_filename, map);
	if (err > 0)
		pr_debug("Using %s for symbols\n", kallsyms_filename);
	if (err > 0 && !dso__is_kcore(dso)) {
		dso__set_binary_type(dso, DSO_BINARY_TYPE__GUEST_KALLSYMS);
		dso__set_long_name(dso, machine->mmap_name, false);
		map__fixup_start(map);
		map__fixup_end(map);
	}

	return err;
}

static void vmlinux_path__exit(void)
{
	while (--vmlinux_path__nr_entries >= 0)
		zfree(&vmlinux_path[vmlinux_path__nr_entries]);
	vmlinux_path__nr_entries = 0;

	zfree(&vmlinux_path);
}

static const char * const vmlinux_paths[] = {
	"vmlinux",
	"/boot/vmlinux"
};

static const char * const vmlinux_paths_upd[] = {
	"/boot/vmlinux-%s",
	"/usr/lib/debug/boot/vmlinux-%s",
	"/lib/modules/%s/build/vmlinux",
	"/usr/lib/debug/lib/modules/%s/vmlinux",
	"/usr/lib/debug/boot/vmlinux-%s.debug"
};

static int vmlinux_path__add(const char *new_entry)
{
	vmlinux_path[vmlinux_path__nr_entries] = strdup(new_entry);
	if (vmlinux_path[vmlinux_path__nr_entries] == NULL)
		return -1;
	++vmlinux_path__nr_entries;

	return 0;
}

static int vmlinux_path__init(struct perf_env *env)
{
	struct utsname uts;
	char bf[PATH_MAX];
	char *kernel_version;
	unsigned int i;

	vmlinux_path = malloc(sizeof(char *) * (ARRAY_SIZE(vmlinux_paths) +
			      ARRAY_SIZE(vmlinux_paths_upd)));
	if (vmlinux_path == NULL)
		return -1;

	for (i = 0; i < ARRAY_SIZE(vmlinux_paths); i++)
		if (vmlinux_path__add(vmlinux_paths[i]) < 0)
			goto out_fail;

	/* only try kernel version if no symfs was given */
	if (symbol_conf.symfs[0] != 0)
		return 0;

	if (env) {
		kernel_version = env->os_release;
	} else {
		if (uname(&uts) < 0)
			goto out_fail;

		kernel_version = uts.release;
	}

	for (i = 0; i < ARRAY_SIZE(vmlinux_paths_upd); i++) {
		snprintf(bf, sizeof(bf), vmlinux_paths_upd[i], kernel_version);
		if (vmlinux_path__add(bf) < 0)
			goto out_fail;
	}

	return 0;

out_fail:
	vmlinux_path__exit();
	return -1;
}

int setup_list(struct strlist **list, const char *list_str,
		      const char *list_name)
{
	if (list_str == NULL)
		return 0;

	*list = strlist__new(list_str, NULL);
	if (!*list) {
		pr_err("problems parsing %s list\n", list_name);
		return -1;
	}

	symbol_conf.has_filter = true;
	return 0;
}

int setup_intlist(struct intlist **list, const char *list_str,
		  const char *list_name)
{
	if (list_str == NULL)
		return 0;

	*list = intlist__new(list_str);
	if (!*list) {
		pr_err("problems parsing %s list\n", list_name);
		return -1;
	}
	return 0;
}

static int setup_addrlist(struct intlist **addr_list, struct strlist *sym_list)
{
	struct str_node *pos, *tmp;
	unsigned long val;
	char *sep;
	const char *end;
	int i = 0, err;

	*addr_list = intlist__new(NULL);
	if (!*addr_list)
		return -1;

	strlist__for_each_entry_safe(pos, tmp, sym_list) {
		errno = 0;
		val = strtoul(pos->s, &sep, 16);
		if (errno || (sep == pos->s))
			continue;

		if (*sep != '\0') {
			end = pos->s + strlen(pos->s) - 1;
			while (end >= sep && isspace(*end))
				end--;

			if (end >= sep)
				continue;
		}

		err = intlist__add(*addr_list, val);
		if (err)
			break;

		strlist__remove(sym_list, pos);
		i++;
	}

	if (i == 0) {
		intlist__delete(*addr_list);
		*addr_list = NULL;
	}

	return 0;
}

static bool symbol__read_kptr_restrict(void)
{
	bool value = false;
	FILE *fp = fopen("/proc/sys/kernel/kptr_restrict", "r");
	bool used_root;
	bool cap_syslog = perf_cap__capable(CAP_SYSLOG, &used_root);

	if (fp != NULL) {
		char line[8];

		if (fgets(line, sizeof(line), fp) != NULL)
			value = cap_syslog ? (atoi(line) >= 2) : (atoi(line) != 0);

		fclose(fp);
	}

	/* Per kernel/kallsyms.c:
	 * we also restrict when perf_event_paranoid > 1 w/o CAP_SYSLOG
	 */
	if (perf_event_paranoid() > 1 && !cap_syslog)
		value = true;

	return value;
}

int symbol__annotation_init(void)
{
	if (symbol_conf.init_annotation)
		return 0;

	if (symbol_conf.initialized) {
		pr_err("Annotation needs to be init before symbol__init()\n");
		return -1;
	}

	symbol_conf.priv_size += sizeof(struct annotation);
	symbol_conf.init_annotation = true;
	return 0;
}

static int setup_parallelism_bitmap(void)
{
	struct perf_cpu_map *map;
	struct perf_cpu cpu;
	int i, err = -1;

	if (symbol_conf.parallelism_list_str == NULL)
		return 0;

	map = perf_cpu_map__new(symbol_conf.parallelism_list_str);
	if (map == NULL) {
		pr_err("failed to parse parallelism filter list\n");
		return -1;
	}

	bitmap_fill(symbol_conf.parallelism_filter, MAX_NR_CPUS + 1);
	perf_cpu_map__for_each_cpu(cpu, i, map) {
		if (cpu.cpu <= 0 || cpu.cpu > MAX_NR_CPUS) {
			pr_err("Requested parallelism level %d is invalid.\n", cpu.cpu);
			goto out_delete_map;
		}
		__clear_bit(cpu.cpu, symbol_conf.parallelism_filter);
	}

	err = 0;
out_delete_map:
	perf_cpu_map__put(map);
	return err;
}

int symbol__init(struct perf_env *env)
{
	const char *symfs;

	if (symbol_conf.initialized)
		return 0;

	symbol_conf.priv_size = PERF_ALIGN(symbol_conf.priv_size, sizeof(u64));

	symbol__elf_init();

	if (symbol_conf.try_vmlinux_path && vmlinux_path__init(env) < 0)
		return -1;

	if (symbol_conf.field_sep && *symbol_conf.field_sep == '.') {
		pr_err("'.' is the only non valid --field-separator argument\n");
		return -1;
	}

	if (setup_parallelism_bitmap())
		return -1;

	if (setup_list(&symbol_conf.dso_list,
		       symbol_conf.dso_list_str, "dso") < 0)
		return -1;

	if (setup_list(&symbol_conf.comm_list,
		       symbol_conf.comm_list_str, "comm") < 0)
		goto out_free_dso_list;

	if (setup_intlist(&symbol_conf.pid_list,
		       symbol_conf.pid_list_str, "pid") < 0)
		goto out_free_comm_list;

	if (setup_intlist(&symbol_conf.tid_list,
		       symbol_conf.tid_list_str, "tid") < 0)
		goto out_free_pid_list;

	if (setup_list(&symbol_conf.sym_list,
		       symbol_conf.sym_list_str, "symbol") < 0)
		goto out_free_tid_list;

	if (symbol_conf.sym_list &&
	    setup_addrlist(&symbol_conf.addr_list, symbol_conf.sym_list) < 0)
		goto out_free_sym_list;

	if (setup_list(&symbol_conf.bt_stop_list,
		       symbol_conf.bt_stop_list_str, "symbol") < 0)
		goto out_free_sym_list;

	/*
	 * A path to symbols of "/" is identical to ""
	 * reset here for simplicity.
	 */
	symfs = realpath(symbol_conf.symfs, NULL);
	if (symfs == NULL)
		symfs = symbol_conf.symfs;
	if (strcmp(symfs, "/") == 0)
		symbol_conf.symfs = "";
	if (symfs != symbol_conf.symfs)
		free((void *)symfs);

	symbol_conf.kptr_restrict = symbol__read_kptr_restrict();

	symbol_conf.initialized = true;
	return 0;

out_free_sym_list:
	strlist__delete(symbol_conf.sym_list);
	intlist__delete(symbol_conf.addr_list);
out_free_tid_list:
	intlist__delete(symbol_conf.tid_list);
out_free_pid_list:
	intlist__delete(symbol_conf.pid_list);
out_free_comm_list:
	strlist__delete(symbol_conf.comm_list);
out_free_dso_list:
	strlist__delete(symbol_conf.dso_list);
	return -1;
}

void symbol__exit(void)
{
	if (!symbol_conf.initialized)
		return;
	strlist__delete(symbol_conf.bt_stop_list);
	strlist__delete(symbol_conf.sym_list);
	strlist__delete(symbol_conf.dso_list);
	strlist__delete(symbol_conf.comm_list);
	intlist__delete(symbol_conf.tid_list);
	intlist__delete(symbol_conf.pid_list);
	intlist__delete(symbol_conf.addr_list);
	vmlinux_path__exit();
	symbol_conf.sym_list = symbol_conf.dso_list = symbol_conf.comm_list = NULL;
	symbol_conf.bt_stop_list = NULL;
	symbol_conf.initialized = false;
}

int symbol__config_symfs(const struct option *opt __maybe_unused,
			 const char *dir, int unset __maybe_unused)
{
	char *bf = NULL;
	int ret;

	symbol_conf.symfs = strdup(dir);
	if (symbol_conf.symfs == NULL)
		return -ENOMEM;

	/* skip the locally configured cache if a symfs is given, and
	 * config buildid dir to symfs/.debug
	 */
	ret = asprintf(&bf, "%s/%s", dir, ".debug");
	if (ret < 0)
		return -ENOMEM;

	set_buildid_dir(bf);

	free(bf);
	return 0;
}

/*
 * Checks that user supplied symbol kernel files are accessible because
 * the default mechanism for accessing elf files fails silently. i.e. if
 * debug syms for a build ID aren't found perf carries on normally. When
 * they are user supplied we should assume that the user doesn't want to
 * silently fail.
 */
int symbol__validate_sym_arguments(void)
{
	if (symbol_conf.vmlinux_name &&
	    access(symbol_conf.vmlinux_name, R_OK)) {
		pr_err("Invalid file: %s\n", symbol_conf.vmlinux_name);
		return -EINVAL;
	}
	if (symbol_conf.kallsyms_name &&
	    access(symbol_conf.kallsyms_name, R_OK)) {
		pr_err("Invalid file: %s\n", symbol_conf.kallsyms_name);
		return -EINVAL;
	}
	return 0;
}

static bool want_demangle(bool is_kernel_sym)
{
	return is_kernel_sym ? symbol_conf.demangle_kernel : symbol_conf.demangle;
}

/*
 * Demangle C++ function signature, typically replaced by demangle-cxx.cpp
 * version.
 */
#ifndef HAVE_CXA_DEMANGLE_SUPPORT
char *cxx_demangle_sym(const char *str __maybe_unused, bool params __maybe_unused,
		       bool modifiers __maybe_unused)
{
#ifdef HAVE_LIBBFD_SUPPORT
	int flags = (params ? DMGL_PARAMS : 0) | (modifiers ? DMGL_ANSI : 0);

	return bfd_demangle(NULL, str, flags);
#elif defined(HAVE_CPLUS_DEMANGLE_SUPPORT)
	int flags = (params ? DMGL_PARAMS : 0) | (modifiers ? DMGL_ANSI : 0);

	return cplus_demangle(str, flags);
#else
	return NULL;
#endif
}
#endif /* !HAVE_CXA_DEMANGLE_SUPPORT */

char *dso__demangle_sym(struct dso *dso, int kmodule, const char *elf_name)
{
	struct demangle rust_demangle = {
		.style = DemangleStyleUnknown,
	};
	char *demangled = NULL;

	/*
	 * We need to figure out if the object was created from C++ sources
	 * DWARF DW_compile_unit has this, but we don't always have access
	 * to it...
	 */
	if (!want_demangle((dso && dso__kernel(dso)) || kmodule))
		return demangled;

	rust_demangle_demangle(elf_name, &rust_demangle);
	if (rust_demangle_is_known(&rust_demangle)) {
		/* A rust mangled name. */
		if (rust_demangle.mangled_len == 0)
			return demangled;

		for (size_t buf_len = roundup_pow_of_two(rust_demangle.mangled_len * 2);
		     buf_len < 1024 * 1024; buf_len += 32) {
			char *tmp = realloc(demangled, buf_len);

			if (!tmp) {
				/* Failure to grow output buffer, return what is there. */
				return demangled;
			}
			demangled = tmp;
			if (rust_demangle_display_demangle(&rust_demangle, demangled, buf_len,
							   /*alternate=*/true) == OverflowOk)
				return demangled;
		}
		/* Buffer exceeded sensible bounds, return what is there. */
		return demangled;
	}

	demangled = cxx_demangle_sym(elf_name, verbose > 0, verbose > 0);
	if (demangled)
		return demangled;

	demangled = ocaml_demangle_sym(elf_name);
	if (demangled)
		return demangled;

	return java_demangle_sym(elf_name, JAVA_DEMANGLE_NORET);
}
