/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PMU_H
#define __PMU_H

#include <linux/bitmap.h>
#include <linux/compiler.h>
#include <linux/perf_event.h>
#include <linux/list.h>
#include <stdbool.h>
#include <stdio.h>
#include "parse-events.h"
#include "pmu-events/pmu-events.h"
#include "map_symbol.h"
#include "mem-events.h"

struct evsel_config_term;
struct hashmap;
struct perf_cpu_map;
struct print_callbacks;

enum {
	PERF_PMU_FORMAT_VALUE_CONFIG,
	PERF_PMU_FORMAT_VALUE_CONFIG1,
	PERF_PMU_FORMAT_VALUE_CONFIG2,
	PERF_PMU_FORMAT_VALUE_CONFIG3,
	PERF_PMU_FORMAT_VALUE_CONFIG_END,
};

#define PERF_PMU_FORMAT_BITS 64
#define MAX_PMU_NAME_LEN 128

struct perf_event_attr;

struct perf_pmu_caps {
	char *name;
	char *value;
	struct list_head list;
};

enum {
	PERF_PMU_TYPE_PE_START    = 0,
	PERF_PMU_TYPE_PE_END      = 0xFFFDFFFF,
	PERF_PMU_TYPE_DRM_START   = 0xFFFE0000,
	PERF_PMU_TYPE_DRM_END     = 0xFFFEFFFF,
	PERF_PMU_TYPE_HWMON_START = 0xFFFF0000,
	PERF_PMU_TYPE_HWMON_END   = 0xFFFFFFFD,
	PERF_PMU_TYPE_TOOL = 0xFFFFFFFE,
	PERF_PMU_TYPE_FAKE = 0xFFFFFFFF,
};

/**
 * struct perf_pmu
 */
struct perf_pmu {
	/** @name: The name of the PMU such as "cpu". */
	const char *name;
	/**
	 * @alias_name: Optional alternate name for the PMU determined in
	 * architecture specific code.
	 */
	char *alias_name;
	/**
	 * @id: Optional PMU identifier read from
	 * <sysfs>/bus/event_source/devices/<name>/identifier.
	 */
	const char *id;
	/**
	 * @type: Perf event attributed type value, read from
	 * <sysfs>/bus/event_source/devices/<name>/type.
	 */
	__u32 type;
	/**
	 * @selectable: Can the PMU name be selected as if it were an event?
	 */
	bool selectable;
	/**
	 * @is_core: Is the PMU the core CPU PMU? Determined by the name being
	 * "cpu" or by the presence of
	 * <sysfs>/bus/event_source/devices/<name>/cpus. There may be >1 core
	 * PMU on systems like Intel hybrid.
	 */
	bool is_core;
	/**
	 * @is_uncore: Is the PMU not within the CPU core? Determined by the
	 * presence of <sysfs>/bus/event_source/devices/<name>/cpumask.
	 */
	bool is_uncore;
	/**
	 * @auxtrace: Are events auxiliary events? Determined in architecture
	 * specific code.
	 */
	bool auxtrace;
	/**
	 * @formats_checked: Only check PMU's formats are valid for
	 * perf_event_attr once.
	 */
	bool formats_checked;
	/** @config_masks_present: Are there config format values? */
	bool config_masks_present;
	/** @config_masks_computed: Set when masks are lazily computed. */
	bool config_masks_computed;
	/**
	 * @max_precise: Number of levels of :ppp precision supported by the
	 * PMU, read from
	 * <sysfs>/bus/event_source/devices/<name>/caps/max_precise.
	 */
	int max_precise;
	/**
	 * @perf_event_attr_init_default: Optional function to default
	 * initialize PMU specific parts of the perf_event_attr.
	 */
	void (*perf_event_attr_init_default)(const struct perf_pmu *pmu,
					     struct perf_event_attr *attr);
	/**
	 * @cpus: Empty or the contents of either of:
	 * <sysfs>/bus/event_source/devices/<name>/cpumask.
	 * <sysfs>/bus/event_source/devices/<cpu>/cpus.
	 */
	struct perf_cpu_map *cpus;
	/**
	 * @format: Holds the contents of files read from
	 * <sysfs>/bus/event_source/devices/<name>/format/. The contents specify
	 * which event parameter changes what config, config1 or config2 bits.
	 */
	struct list_head format;
	/**
	 * @aliases: List of struct perf_pmu_alias. Each alias corresponds to an
	 * event read from <sysfs>/bus/event_source/devices/<name>/events/ or
	 * from json events in pmu-events.c.
	 */
	struct hashmap *aliases;
	/**
	 * @events_table: The events table for json events in pmu-events.c.
	 */
	const struct pmu_events_table *events_table;
	/** @sysfs_aliases: Number of sysfs aliases loaded. */
	uint32_t sysfs_aliases;
	/** @cpu_json_aliases: Number of json event aliases loaded specific to the CPUID. */
	uint32_t cpu_json_aliases;
	/** @sys_json_aliases: Number of json event aliases loaded matching the PMU's identifier. */
	uint32_t sys_json_aliases;
	/**
	 * @cpu_common_json_aliases: Number of json events that overlapped with sysfs when
	 * loading all sysfs events.
	 */
	uint32_t cpu_common_json_aliases;
	/** @sysfs_aliases_loaded: Are sysfs aliases loaded from disk? */
	bool sysfs_aliases_loaded;
	/**
	 * @cpu_aliases_added: Have all json events table entries for the PMU
	 * been added?
	 */
	bool cpu_aliases_added;
	/** @caps_initialized: Has the list caps been initialized? */
	bool caps_initialized;
	/** @nr_caps: The length of the list caps. */
	u32 nr_caps;
	/**
	 * @caps: Holds the contents of files read from
	 * <sysfs>/bus/event_source/devices/<name>/caps/.
	 *
	 * The contents are pairs of the filename with the value of its
	 * contents, for example, max_precise (see above) may have a value of 3.
	 */
	struct list_head caps;
	/** @list: Element on pmus list in pmu.c. */
	struct list_head list;

	/**
	 * @config_masks: Derived from the PMU's format data, bits that are
	 * valid within the config value.
	 */
	__u64 config_masks[PERF_PMU_FORMAT_VALUE_CONFIG_END];

	/**
	 * @missing_features: Features to inhibit when events on this PMU are
	 * opened.
	 */
	struct {
		/**
		 * @exclude_guest: Disables perf_event_attr exclude_guest and
		 * exclude_host.
		 */
		bool exclude_guest;
		/**
		 * @checked: Are the missing features checked?
		 */
		bool checked;
	} missing_features;

	/**
	 * @mem_events: List of the supported mem events
	 */
	struct perf_mem_event *mem_events;
};

struct perf_pmu_info {
	const char *unit;
	double scale;
	double retirement_latency_mean;
	double retirement_latency_min;
	double retirement_latency_max;
	bool per_pkg;
	bool snapshot;
};

struct pmu_event_info {
	const struct perf_pmu *pmu;
	const char *name;
	const char* alias;
	const char *scale_unit;
	const char *desc;
	const char *long_desc;
	const char *encoding_desc;
	const char *topic;
	const char *pmu_name;
	const char *event_type_desc;
	const char *str;
	bool deprecated;
};

typedef int (*pmu_event_callback)(void *state, struct pmu_event_info *info);
typedef int (*pmu_format_callback)(void *state, const char *name, int config,
				   const unsigned long *bits);

void pmu_add_sys_aliases(struct perf_pmu *pmu);
int perf_pmu__config(struct perf_pmu *pmu, struct perf_event_attr *attr,
		     struct parse_events_terms *head_terms,
		     bool apply_hardcoded,
		     struct parse_events_error *error);
int perf_pmu__config_terms(const struct perf_pmu *pmu,
			   struct perf_event_attr *attr,
			   struct parse_events_terms *terms,
			   bool zero, bool apply_hardcoded,
			   struct parse_events_error *error);
__u64 perf_pmu__format_bits(struct perf_pmu *pmu, const char *name);
int perf_pmu__format_type(struct perf_pmu *pmu, const char *name);
int perf_pmu__check_alias(struct perf_pmu *pmu, struct parse_events_terms *head_terms,
			  struct perf_pmu_info *info, bool *rewrote_terms,
			  u64 *alternate_hw_config, struct parse_events_error *err);
int perf_pmu__find_event(struct perf_pmu *pmu, const char *event, void *state, pmu_event_callback cb);

void perf_pmu_format__set_value(void *format, int config, unsigned long *bits);
bool perf_pmu__has_format(const struct perf_pmu *pmu, const char *name);
int perf_pmu__for_each_format(struct perf_pmu *pmu, void *state, pmu_format_callback cb);

bool is_pmu_core(const char *name);
bool perf_pmu__supports_legacy_cache(const struct perf_pmu *pmu);
bool perf_pmu__auto_merge_stats(const struct perf_pmu *pmu);
bool perf_pmu__have_event(struct perf_pmu *pmu, const char *name);
size_t perf_pmu__num_events(struct perf_pmu *pmu);
int perf_pmu__for_each_event(struct perf_pmu *pmu, bool skip_duplicate_pmus,
			     void *state, pmu_event_callback cb);
bool perf_pmu__name_wildcard_match(const struct perf_pmu *pmu, const char *to_match);
bool perf_pmu__name_no_suffix_match(const struct perf_pmu *pmu, const char *to_match);

/**
 * perf_pmu_is_software - is the PMU a software PMU as in it uses the
 *                        perf_sw_context in the kernel?
 */
bool perf_pmu__is_software(const struct perf_pmu *pmu);

FILE *perf_pmu__open_file(const struct perf_pmu *pmu, const char *name);
FILE *perf_pmu__open_file_at(const struct perf_pmu *pmu, int dirfd, const char *name);

int perf_pmu__scan_file(const struct perf_pmu *pmu, const char *name, const char *fmt, ...)
	__scanf(3, 4);
int perf_pmu__scan_file_at(const struct perf_pmu *pmu, int dirfd, const char *name,
			   const char *fmt, ...) __scanf(4, 5);

bool perf_pmu__file_exists(const struct perf_pmu *pmu, const char *name);

int perf_pmu__test(void);

void perf_pmu__arch_init(struct perf_pmu *pmu);
void pmu_add_cpu_aliases_table(struct perf_pmu *pmu,
			       const struct pmu_events_table *table);

bool pmu_uncore_identifier_match(const char *compat, const char *id);

int perf_pmu__convert_scale(const char *scale, char **end, double *sval);

struct perf_pmu_caps *perf_pmu__get_cap(struct perf_pmu *pmu, const char *name);

int perf_pmu__caps_parse(struct perf_pmu *pmu);

void perf_pmu__warn_invalid_config(struct perf_pmu *pmu, __u64 config,
				   const char *name, int config_num,
				   const char *config_name);
void perf_pmu__warn_invalid_formats(struct perf_pmu *pmu);

bool perf_pmu__wildcard_match(const struct perf_pmu *pmu, const char *wildcard_to_match);

int perf_pmu__event_source_devices_scnprintf(char *pathname, size_t size);
int perf_pmu__pathname_scnprintf(char *buf, size_t size,
				 const char *pmu_name, const char *filename);
int perf_pmu__event_source_devices_fd(void);
int perf_pmu__pathname_fd(int dirfd, const char *pmu_name, const char *filename, int flags);

int perf_pmu__init(struct perf_pmu *pmu, __u32 type, const char *name);
struct perf_pmu *perf_pmu__lookup(struct list_head *pmus, int dirfd, const char *lookup_name,
				  bool eager_load);
struct perf_pmu *perf_pmu__create_placeholder_core_pmu(struct list_head *core_pmus);
void perf_pmu__delete(struct perf_pmu *pmu);

const char *perf_pmu__name_from_config(struct perf_pmu *pmu, u64 config);
bool perf_pmu__is_fake(const struct perf_pmu *pmu);

#endif /* __PMU_H */
