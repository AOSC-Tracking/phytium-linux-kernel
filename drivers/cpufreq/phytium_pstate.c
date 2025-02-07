// SPDX-License-Identifier: GPL-2.0-only
/*
 * phytium_pstate.c - Phytium Processor P-state Frequency Driver
 *
 * Copyright (C) 2024,Phytium Technology Co.,Ltd.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <acpi/processor.h>
#include <acpi/cppc_acpi.h>
#include <linux/processor.h>
#include <linux/cpufeature.h>

#include <uapi/linux/sched/types.h>

#define KHZ_PER_MHZ 1000
#define REFERENCE_PERF 50

/*
 * Available EPP Performance Levels. From large to small,
 * the lower the power consumption.
 */
#define PHYT_CPPC_EPP_PERFORMANCE		0x00
#define PHYT_CPPC_EPP_BALANCE_PERFORMANCE	0x40
#define PHYT_CPPC_EPP_BALANCE_POWERSAVE		0x80
#define PHYT_CPPC_EPP_POWERSAVE			0xc0

#define PHYT_PSTATE_TRANSITION_LATENCY		20000
#define PHYT_PSTATE_TRANSITION_DELAY		1000

/* AMU Register Encoding. */
#define PHYT_SYS_AMEVCNTR0_EL0(op2)	sys_reg(3, 3, 15, 9, (op2))
#define PHYT_SYS_AMEVCNTR0_CORE_EL0	PHYT_SYS_AMEVCNTR0_EL0(0)
#define PHYT_SYS_AMEVCNTR0_CONST_EL0	PHYT_SYS_AMEVCNTR0_EL0(1)

#define read_corecnt()	read_sysreg_s(PHYT_SYS_AMEVCNTR0_CORE_EL0)
#define read_constcnt()	read_sysreg_s(PHYT_SYS_AMEVCNTR0_CONST_EL0)

struct cppc_req_cached {
	u32	max_perf_cached;
	u32	min_perf_cached;
	u32	desired_perf_cached;
	u32	epp_cached;
};

struct phyt_cpudata {
	int	cpu;
	u32	policy;
	bool	boost_supported;

	/* EPP feature related attributes */
	s16	epp_policy;

	struct	cppc_req_cached req_cached;
	bool	suspended;

	/* CPC related data structure */
	struct	cppc_perf_caps perf_caps;
	struct	cppc_perf_ctrls perf_ctrls;
	struct	cppc_perf_fb_ctrs perf_fb_ctrs;
};

/*
 * enum phyt_pstate_mode - driver working mode of phytium pstate
 */
enum phyt_pstate_mode {
	PHYT_PSTATE_UNDEFINED = 0,
	PHYT_PSTATE_DISABLE,
	PHYT_PSTATE_PASSIVE,
	PHYT_PSTATE_ACTIVE,
	PHYT_PSTATE_MAX,
};

static const char * const phyt_pstate_mode_string[] = {
	[PHYT_PSTATE_UNDEFINED]   = "undefined",
	[PHYT_PSTATE_DISABLE]     = "disable",
	[PHYT_PSTATE_PASSIVE]     = "passive",
	[PHYT_PSTATE_ACTIVE]      = "active",
	NULL,
};

static struct cpufreq_driver *current_pstate_driver;
static struct cpufreq_driver phyt_pstate_driver;
static struct cpufreq_driver phyt_cpufreq_driver;
static int driver_state = PHYT_PSTATE_UNDEFINED;

/*
 * Phytium Energy Preference Performance (EPP)
 * display strings corresponding to EPP index in the
 * energy_perf_strings[]
 *	index		String
 *-------------------------------------
 *	0		performance
 *	1		balance_performance
 *	2		balance_power
 *	3		power
 */
enum energy_perf_value_index {
	EPP_INDEX_PERFORMANCE = 0,
	EPP_INDEX_BALANCE_PERFORMANCE,
	EPP_INDEX_BALANCE_POWERSAVE,
	EPP_INDEX_POWERSAVE,
};

static const char * const energy_perf_strings[] = {
	[EPP_INDEX_PERFORMANCE] = "performance",
	[EPP_INDEX_BALANCE_PERFORMANCE] = "balance_performance",
	[EPP_INDEX_BALANCE_POWERSAVE] = "balance_power",
	[EPP_INDEX_POWERSAVE] = "power",
	NULL
};

static unsigned int epp_values[] = {
	[EPP_INDEX_PERFORMANCE] = PHYT_CPPC_EPP_PERFORMANCE,
	[EPP_INDEX_BALANCE_PERFORMANCE] = PHYT_CPPC_EPP_BALANCE_PERFORMANCE,
	[EPP_INDEX_BALANCE_POWERSAVE] = PHYT_CPPC_EPP_BALANCE_POWERSAVE,
	[EPP_INDEX_POWERSAVE] = PHYT_CPPC_EPP_POWERSAVE,
};

typedef int (*cppc_mode_transition_fn)(int);

static inline int get_mode_idx_from_str(const char *str, size_t size)
{
	int i;

	for (i = 0; i < PHYT_PSTATE_MAX; i++) {
		if (!strncmp(str, phyt_pstate_mode_string[i], size))
			return i;
	}
	return -EINVAL;
}

static DEFINE_MUTEX(phyt_pstate_limits_lock);
static DEFINE_MUTEX(phyt_pstate_driver_lock);

static s16 phyt_pstate_get_epp(struct phyt_cpudata *cpudata)
{
	u64 epp;
	int ret;


	ret = cppc_get_epp_perf(cpudata->cpu, &epp);
	if (ret < 0) {
		pr_debug("Could not retrieve energy perf value (%d)\n", ret);
		return -EIO;
	}

	return (s16)(epp & 0xff);
}

static int phyt_pstate_get_energy_pref_index(struct phyt_cpudata *cpudata)
{
	s16 epp;
	int index = -EINVAL;

	epp = phyt_pstate_get_epp(cpudata);
	if (epp < 0)
		return epp;

	switch (epp) {
	case PHYT_CPPC_EPP_PERFORMANCE:
		index = EPP_INDEX_PERFORMANCE;
		break;
	case PHYT_CPPC_EPP_BALANCE_PERFORMANCE:
		index = EPP_INDEX_BALANCE_PERFORMANCE;
		break;
	case PHYT_CPPC_EPP_BALANCE_POWERSAVE:
		index = EPP_INDEX_BALANCE_POWERSAVE;
		break;
	case PHYT_CPPC_EPP_POWERSAVE:
		index = EPP_INDEX_POWERSAVE;
		break;
	default:
		break;
	}

	return index;
}

static int phyt_pstate_set_epp(struct phyt_cpudata *cpudata, u32 epp)
{
	int ret;
	struct cppc_perf_ctrls perf_ctrls;
	struct cppc_req_cached req_epp_cached = cpudata->req_cached;

	perf_ctrls.energy_perf = epp;
	ret = cppc_set_epp_perf(cpudata->cpu, &perf_ctrls, 1);
	if (ret) {
		pr_debug("failed to set energy perf value (%d)\n", ret);
		return ret;
	}
	req_epp_cached.epp_cached = epp;

	return ret;
}

static int phyt_pstate_set_energy_pref_index(struct phyt_cpudata *cpudata,
		int pref_index)
{
	int epp = -EINVAL;
	int ret;

	if (!pref_index) {
		pr_debug("EPP pref_index is invalid\n");
		return -EINVAL;
	}

	if (epp == -EINVAL)
		epp = epp_values[pref_index];

	if (epp > 0 && cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
		pr_debug("EPP cannot be set under performance policy\n");
		return -EBUSY;
	}

	ret = phyt_pstate_set_epp(cpudata, epp);

	return ret;
}

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,6,58)// in v6.6.58, cppc_acpi already export these API below
unsigned int cppc_perf_to_khz(struct cppc_perf_caps *caps, unsigned int perf)
{
	s64 retval, offset = 0;
	u64 mul, div;

	mul = caps->nominal_freq - caps->lowest_freq;
	mul *= KHZ_PER_MHZ;
	div = caps->nominal_perf - caps->lowest_perf;
	offset = caps->nominal_freq * KHZ_PER_MHZ -
		div64_u64(caps->nominal_perf * mul, div);

	retval = offset + div64_u64(perf * mul, div);
	if (retval >= 0)
		return retval;
	return 0;
}

unsigned int cppc_khz_to_perf(struct cppc_perf_caps *caps, unsigned int freq)
{
	s64 retval, offset = 0;
	u64 mul, div;

	mul = caps->nominal_perf - caps->lowest_perf;
	div = caps->nominal_freq - caps->lowest_freq;
	/*
	 * We don't need to convert to kHz for computing offset and can
	 * directly use nominal_freq and lowest_freq as the div64_u64
	 * will remove the frequency unit.
	 */
	offset = caps->nominal_perf -
	     div64_u64(caps->nominal_freq * mul, div);
	/* But we need it for computing the perf level. */
	div *= KHZ_PER_MHZ;

	retval = offset + div64_u64(freq * mul, div);
	if (retval >= 0)
		return retval;
	return 0;
}
#endif

static int phyt_pstate_init_perf(struct phyt_cpudata *cpudata)
{
	int ret;

	ret = cppc_get_perf_caps(cpudata->cpu, &cpudata->perf_caps);
	if (ret)
		return ret;

	ret = cppc_get_auto_sel_caps(cpudata->cpu, &cpudata->perf_caps);
	if (ret) {
		pr_warn("failed to get auto_sel, ret: %d\n", ret);
		return 0;
	}

	ret = cppc_set_auto_sel(cpudata->cpu,
			(driver_state == PHYT_PSTATE_PASSIVE) ? 0 : 1);
	if (ret)
		pr_warn("failed to set auto_sel, ret: %d\n", ret);

	return ret;
}

static int phyt_cpufreq_verify(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);

	return 0;
}

static int phyt_cpufreq_target(struct cpufreq_policy *policy,
			     unsigned int target_freq,
			     unsigned int relation)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	unsigned int cpu = policy->cpu;
	struct cpufreq_freqs freqs;
	u32 desired_perf;
	int ret = 0;

	desired_perf = cppc_khz_to_perf(&cpudata->perf_caps, target_freq);
	/* Return if it is exactly the same perf. */
	if (desired_perf == cpudata->perf_ctrls.desired_perf)
		return ret;

	cpudata->perf_ctrls.desired_perf = desired_perf;
	freqs.old = policy->cur;
	freqs.new = target_freq;

	cpufreq_freq_transition_begin(policy, &freqs);
	ret = cppc_set_perf(cpu, &cpudata->perf_ctrls);
	cpufreq_freq_transition_end(policy, &freqs, ret != 0);

	if (ret)
		pr_debug("Failed to set target on CPU:%d. ret:%d\n",
			 cpu, ret);

	pr_debug("Set target on CPU:%d, target:%u\n", cpu, target_freq);
	return ret;
}

static int phyt_cpufreq_set_boost(struct cpufreq_policy *policy, int state)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	int ret;

	if (!cpudata->boost_supported) {
		pr_err("Boost mode is not supported by this processor or SBIOS\n");
		return -EINVAL;
	}

	if (state)
		policy->cpuinfo.max_freq = cppc_perf_to_khz(&cpudata->perf_caps,
				cpudata->perf_caps.highest_perf);
	else
		policy->cpuinfo.max_freq = cppc_perf_to_khz(&cpudata->perf_caps,
				cpudata->perf_caps.nominal_perf);

	policy->max = policy->cpuinfo.max_freq;

	ret = freq_qos_update_request(policy->max_freq_req, policy->max);
	if (ret < 0)
		return ret;

	return 0;
}

static void phyt_pstate_boost_init(struct phyt_cpudata *cpudata)
{
	u32 highest_perf, nominal_perf;

	highest_perf = cpudata->perf_caps.highest_perf;
	nominal_perf = cpudata->perf_caps.nominal_perf;

	if (highest_perf <= nominal_perf)
		return;

	cpudata->boost_supported = true;
	current_pstate_driver->boost_enabled = false;
}

static int phyt_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	struct device *dev;
	struct phyt_cpudata *cpudata;
	struct cppc_perf_caps *caps;
	int ret;

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = policy->cpu;
	ret = phyt_pstate_init_perf(cpudata);
	if (ret)
		goto err;

	caps = &cpudata->perf_caps;

	policy->cpuinfo.transition_latency = PHYT_PSTATE_TRANSITION_LATENCY;
	policy->transition_delay_us = PHYT_PSTATE_TRANSITION_DELAY;
	policy->min = cppc_perf_to_khz(caps, caps->lowest_perf);
	policy->max = cppc_perf_to_khz(caps, caps->nominal_perf);
	policy->cpuinfo.min_freq = policy->min;
	policy->cpuinfo.max_freq = policy->max;
	policy->fast_switch_possible = false;
	policy->driver_data = cpudata;

	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.max_freq;

	phyt_pstate_boost_init(cpudata);

	return 0;
err:
	kfree(cpudata);
	return ret;
}

static int phyt_cpufreq_cpu_exit(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;

	kfree(cpudata);

	return 0;
}

/* Sysfs attributes */

static ssize_t show_energy_performance_available_preferences(
				struct cpufreq_policy *policy, char *buf)
{
	int i = 0;
	int offset = 0;
	struct phyt_cpudata *cpudata = policy->driver_data;

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE)
		return sysfs_emit_at(buf, offset, "%s\n",
				energy_perf_strings[EPP_INDEX_PERFORMANCE]);

	while (energy_perf_strings[i] != NULL)
		offset += sysfs_emit_at(buf, offset, "%s ", energy_perf_strings[i++]);

	offset += sysfs_emit_at(buf, offset, "\n");

	return offset;
}

static ssize_t store_energy_performance_preference(
		struct cpufreq_policy *policy, const char *buf, size_t count)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	char str_preference[21];
	ssize_t ret;

	ret = sscanf(buf, "%20s", str_preference);
	if (ret != 1)
		return -EINVAL;

	ret = match_string(energy_perf_strings, -1, str_preference);
	if (ret < 0)
		return -EINVAL;

	mutex_lock(&phyt_pstate_limits_lock);
	ret = phyt_pstate_set_energy_pref_index(cpudata, ret);
	mutex_unlock(&phyt_pstate_limits_lock);

	return ret ?: count;
}

static ssize_t show_energy_performance_preference(
				struct cpufreq_policy *policy, char *buf)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	int preference;

	preference = phyt_pstate_get_energy_pref_index(cpudata);
	if (preference < 0)
		return preference;

	return sysfs_emit(buf, "%s\n", energy_perf_strings[preference]);
}

static void phyt_pstate_driver_cleanup(void)
{
	int cpu;

	for_each_present_cpu(cpu)
		cppc_set_auto_sel(cpu, 0);

	driver_state = PHYT_PSTATE_DISABLE;
	current_pstate_driver = NULL;
}

static int phyt_pstate_register_driver(int mode)
{
	int ret;

	if (mode == PHYT_PSTATE_PASSIVE)
		current_pstate_driver = &phyt_cpufreq_driver;
	else if (mode == PHYT_PSTATE_ACTIVE)
		current_pstate_driver = &phyt_pstate_driver;
	else
		return -EINVAL;

	driver_state = mode;
	ret = cpufreq_register_driver(current_pstate_driver);
	if (ret) {
		phyt_pstate_driver_cleanup();
		return ret;
	}
	return 0;
}

static int phyt_pstate_unregister_driver(int dummy)
{
	cpufreq_unregister_driver(current_pstate_driver);
	phyt_pstate_driver_cleanup();

	return 0;
}

static int phyt_pstate_change_driver_mode(int mode)
{
	int ret;

	driver_state = mode;

	ret = phyt_pstate_unregister_driver(0);
	if (ret)
		return ret;

	ret = phyt_pstate_register_driver(mode);
	if (ret)
		return ret;

	return 0;
}

static cppc_mode_transition_fn mode_state_machine[PHYT_PSTATE_MAX][PHYT_PSTATE_MAX] = {
	[PHYT_PSTATE_DISABLE]         = {
		[PHYT_PSTATE_DISABLE]     = NULL,
		[PHYT_PSTATE_PASSIVE]     = phyt_pstate_register_driver,
		[PHYT_PSTATE_ACTIVE]      = phyt_pstate_register_driver,
	},
	[PHYT_PSTATE_PASSIVE]         = {
		[PHYT_PSTATE_DISABLE]     = phyt_pstate_unregister_driver,
		[PHYT_PSTATE_PASSIVE]     = NULL,
		[PHYT_PSTATE_ACTIVE]      = phyt_pstate_change_driver_mode,
	},
	[PHYT_PSTATE_ACTIVE]          = {
		[PHYT_PSTATE_DISABLE]     = phyt_pstate_unregister_driver,
		[PHYT_PSTATE_PASSIVE]     = phyt_pstate_change_driver_mode,
		[PHYT_PSTATE_ACTIVE]      = NULL,
	}
};

static ssize_t phyt_pstate_show_status(char *buf)
{
	if (!current_pstate_driver)
		return sysfs_emit(buf, "disable\n");

	return sysfs_emit(buf, "%s\n", phyt_pstate_mode_string[driver_state]);
}

static int phyt_pstate_update_status(const char *buf, size_t size)
{
	int mode_idx;

	if (size > strlen("passive") || size < strlen("active"))
		return -EINVAL;

	mode_idx = get_mode_idx_from_str(buf, size);

	if (mode_idx < 0 || mode_idx >= PHYT_PSTATE_MAX)
		return -EINVAL;

	if (mode_state_machine[driver_state][mode_idx])
		return mode_state_machine[driver_state][mode_idx](mode_idx);

	return 0;
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	ssize_t ret;

	mutex_lock(&phyt_pstate_driver_lock);
	ret = phyt_pstate_show_status(buf);
	mutex_unlock(&phyt_pstate_driver_lock);

	return ret;
}

static ssize_t status_store(struct device *a, struct device_attribute *b,
			    const char *buf, size_t count)
{
	char *p = memchr(buf, '\n', count);
	int ret;

	mutex_lock(&phyt_pstate_driver_lock);
	ret = phyt_pstate_update_status(buf, p ? p - buf : count);
	mutex_unlock(&phyt_pstate_driver_lock);

	return ret < 0 ? ret : count;
}

cpufreq_freq_attr_rw(energy_performance_preference);
cpufreq_freq_attr_ro(energy_performance_available_preferences);
static DEVICE_ATTR_RW(status);

static struct freq_attr *phyt_cpufreq_attr[] = {
	NULL,
};

static struct freq_attr *phyt_pstate_attr[] = {
	&energy_performance_preference,
	&energy_performance_available_preferences,
	NULL,
};

static struct attribute *pstate_global_attributes[] = {
	&dev_attr_status.attr,
	NULL
};

static const struct attribute_group phyt_pstate_global_attr_group = {
	.name = "phyt_pstate",
	.attrs = pstate_global_attributes,
};

static int phyt_pstate_cpu_init(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata;
	struct device *dev;
	struct cppc_perf_caps *caps;
	struct cppc_perf_ctrls perf_ctrls;
	int ret;

	dev = get_cpu_device(policy->cpu);
	if (!dev)
		return -ENODEV;

	cpudata = kzalloc(sizeof(*cpudata), GFP_KERNEL);
	if (!cpudata)
		return -ENOMEM;

	cpudata->cpu = policy->cpu;
	ret = phyt_pstate_init_perf(cpudata);
	if (ret)
		goto err;

	cpudata->epp_policy = 0;
	cpudata->req_cached.epp_cached = phyt_pstate_get_epp(cpudata);

	caps = &cpudata->perf_caps;

	policy->cpuinfo.transition_latency = PHYT_PSTATE_TRANSITION_LATENCY;
	policy->transition_delay_us = PHYT_PSTATE_TRANSITION_DELAY;
	policy->min = cppc_perf_to_khz(caps, caps->lowest_perf);
	policy->max = cppc_perf_to_khz(caps, caps->nominal_perf);
	policy->cpuinfo.min_freq = policy->min;
	policy->cpuinfo.max_freq = policy->max;
	policy->fast_switch_possible = false;
	policy->driver_data = cpudata;
	policy->policy = CPUFREQ_POLICY_POWERSAVE;

	/* It will be updated by governor */
	policy->cur = policy->cpuinfo.max_freq;

	perf_ctrls.max_perf = caps->nominal_perf;
	perf_ctrls.min_perf = caps->lowest_perf;
	ret = cppc_set_perf(cpudata->cpu, &perf_ctrls);
	if (ret)
		pr_debug("Failed to limit freq on CPU:%d. ret:%d\n",
			cpudata->cpu, ret);

	phyt_pstate_boost_init(cpudata);

	return 0;
err:
	kfree(cpudata);
	return ret;
}

static int phyt_pstate_cpu_exit(struct cpufreq_policy *policy)
{
	pr_debug("CPU %d exiting\n", policy->cpu);
	return 0;
}

static void phyt_pstate_update_limit(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	struct cppc_perf_ctrls perf_ctrls;
	struct cppc_req_cached req_epp_cached = cpudata->req_cached;

	u32 min_perf, max_perf;
	s16 epp;

	max_perf = cppc_khz_to_perf(&cpudata->perf_caps, policy->max);
	min_perf = cppc_khz_to_perf(&cpudata->perf_caps, policy->min);

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE) {
		min_perf = max_perf;
		policy->min = policy->max;
	}

	perf_ctrls.max_perf = max_perf;
	perf_ctrls.min_perf = min_perf;
	perf_ctrls.desired_perf = 0;
	cppc_set_perf(cpudata->cpu, &perf_ctrls);

	req_epp_cached.max_perf_cached = max_perf;
	req_epp_cached.min_perf_cached = min_perf;
	req_epp_cached.desired_perf_cached = 0;

	cpudata->epp_policy = cpudata->policy;

	/* Get epp value */
	epp = phyt_pstate_get_epp(cpudata);
	if (epp < 0)
		return;

	if (cpudata->policy == CPUFREQ_POLICY_PERFORMANCE)
		epp = 0;

	req_epp_cached.epp_cached = epp;
	phyt_pstate_set_epp(cpudata, epp);
}

static int phyt_pstate_set_policy(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;

	if (!policy->cpuinfo.max_freq)
		return -ENODEV;

	cpudata->policy = policy->policy;

	phyt_pstate_update_limit(policy);

	return 0;
}

static void update_cpu_perf(void *val)
{
	u64 prev_core_cnt, prev_const_cnt, core_cnt, const_cnt;
	u64 delta_core_cnt, delta_const_cnt;
	u64 perf;

	prev_core_cnt = read_corecnt();
	prev_const_cnt = read_constcnt();
	udelay(2);
	core_cnt = read_corecnt();
	const_cnt = read_constcnt();

	delta_core_cnt = core_cnt - prev_core_cnt;
	delta_const_cnt = const_cnt - prev_const_cnt;

	/*
	 *	         delta_core_cnt
	 * perf =  ----------------- * reference_perf
	 *	         delta_const_cnt
	 */
	perf = div64_u64(100 * delta_core_cnt, delta_const_cnt);
	perf = div64_u64(REFERENCE_PERF * perf, 100);

	*(u64 *)val = perf;
}

static inline int read_counters_on_cpu(int cpu, smp_call_func_t func, u64 *val)
{
	if (WARN_ON_ONCE(irqs_disabled()))
		return -EPERM;

	smp_call_function_single(cpu, func, val, 1);
	return 0;
}

static unsigned int phyt_pstate_get_rate(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
	struct phyt_cpudata *cpudata = policy->driver_data;
	u64 scale, cur_freq;

	read_counters_on_cpu(cpu, update_cpu_perf, &scale);

	cur_freq = cppc_perf_to_khz(&cpudata->perf_caps, scale);

	cpufreq_cpu_put(policy);
	return cur_freq;
}

static void phyt_pstate_epp_reenable(struct phyt_cpudata *cpudata)
{
	struct cppc_perf_ctrls perf_ctrls;
	struct cppc_req_cached req_epp_cached = cpudata->req_cached;
	u64 max_perf, min_perf;

	max_perf = req_epp_cached.max_perf_cached;
	min_perf = req_epp_cached.min_perf_cached;

	/* Set the maximum and minimum perf level to the cached value. */
	perf_ctrls.max_perf = max_perf;
	perf_ctrls.min_perf = min_perf;
	perf_ctrls.desired_perf = 0;
	cppc_set_perf(cpudata->cpu, &perf_ctrls);

	/* Re-enable auto_sel and write epp levels cached before suspend. */
	perf_ctrls.energy_perf = req_epp_cached.epp_cached;
	cppc_set_epp_perf(cpudata->cpu, &perf_ctrls, 1);
}

static int phyt_pstate_cpu_online(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;

	pr_debug("Phyt CPU Core %d going online\n", cpudata->cpu);

	if (driver_state == PHYT_PSTATE_ACTIVE)
		phyt_pstate_epp_reenable(cpudata);

	cpudata->suspended = false;
	return 0;
}

static int phyt_pstate_cpu_offline(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;

	pr_debug("Phyt CPU Core %d going offline\n", cpudata->cpu);

	if (cpudata->suspended)
		return 0;

	return 0;
}

static int phyt_pstate_verify_policy(struct cpufreq_policy_data *policy)
{
	cpufreq_verify_within_cpu_limits(policy);

	return 0;
}

static int phyt_pstate_suspend(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;

	cpudata->suspended = true;

	return 0;
}

static int phyt_pstate_resume(struct cpufreq_policy *policy)
{
	struct phyt_cpudata *cpudata = policy->driver_data;
	int ret;

	if (cpudata->suspended) {
		if (driver_state == PHYT_PSTATE_ACTIVE) {
			mutex_lock(&phyt_pstate_limits_lock);
			/* enable phyt pstate from suspend state*/
			phyt_pstate_epp_reenable(cpudata);
			mutex_unlock(&phyt_pstate_limits_lock);
		} else {
			cpudata->perf_ctrls.desired_perf = policy->cur;
			ret = cppc_set_perf(policy->cpu, &cpudata->perf_ctrls);
		}
		cpudata->suspended = false;
	}

	return 0;
}

static struct cpufreq_driver phyt_pstate_driver = {
	.flags		= CPUFREQ_CONST_LOOPS,
	.verify		= phyt_pstate_verify_policy,
	.setpolicy	= phyt_pstate_set_policy,
	.get		= phyt_pstate_get_rate,
	.init		= phyt_pstate_cpu_init,
	.exit		= phyt_pstate_cpu_exit,
	.offline	= phyt_pstate_cpu_offline,
	.online		= phyt_pstate_cpu_online,
	.suspend	= phyt_pstate_suspend,
	.resume		= phyt_pstate_resume,
	.set_boost	= phyt_cpufreq_set_boost,
	.name		= "phyt-pstate",
	.attr		= phyt_pstate_attr,
};

static struct cpufreq_driver phyt_cpufreq_driver = {
	.flags		= CPUFREQ_CONST_LOOPS | CPUFREQ_NEED_UPDATE_LIMITS,
	.verify		= phyt_cpufreq_verify,
	.target		= phyt_cpufreq_target,
	.get		= phyt_pstate_get_rate,
	.init		= phyt_cpufreq_cpu_init,
	.exit		= phyt_cpufreq_cpu_exit,
	.suspend	= phyt_pstate_suspend,
	.resume		= phyt_pstate_resume,
	.set_boost	= phyt_cpufreq_set_boost,
	.name		= "phyt-cpufreq",
	.attr		= phyt_cpufreq_attr,
};

static int __init phyt_pstate_set_driver(int mode_idx)
{
	if (mode_idx >= PHYT_PSTATE_DISABLE && mode_idx < PHYT_PSTATE_MAX) {
		driver_state = mode_idx;
		if (driver_state == PHYT_PSTATE_DISABLE)
			pr_info("driver is explicitly disabled\n");

		if (driver_state == PHYT_PSTATE_ACTIVE)
			current_pstate_driver = &phyt_pstate_driver;

		if (driver_state == PHYT_PSTATE_PASSIVE)
			current_pstate_driver = &phyt_cpufreq_driver;

		return 0;
	}

	return -EINVAL;
}

static int __init phyt_pstate_init(void)
{
	struct device *dev_root;
	int ret;

	if (!acpi_cpc_valid()) {
		pr_warn_once("_CPC_method is not available\n");
		return -ENODEV;
	}

	/* don't keep reloading if cpufreq_driver exists */
	if (cpufreq_get_current_driver())
		return -EEXIST;

	switch (driver_state) {
	case PHYT_PSTATE_UNDEFINED:
		ret = phyt_pstate_set_driver(CONFIG_PHYT_PSTATE_DEFAULT_MODE);
		if (ret)
			return ret;
		break;
	case PHYT_PSTATE_DISABLE:
		return -ENODEV;
	case PHYT_PSTATE_PASSIVE:
	case PHYT_PSTATE_ACTIVE:
		break;
	default:
		return -EINVAL;
	}

	ret = cpufreq_register_driver(current_pstate_driver);
	if (ret)
		pr_err("failed to register with return %d\n", ret);

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = sysfs_create_group(&dev_root->kobj,
				&phyt_pstate_global_attr_group);
		put_device(dev_root);
		if (ret) {
			pr_err("sysfs export failed %d\n", ret);
			goto global_attr_free;
		}
	}

	return ret;

global_attr_free:
	cpufreq_unregister_driver(current_pstate_driver);
	return ret;
}
device_initcall(phyt_pstate_init);

static int __init phyt_pstate_param(char *str)
{
	size_t size;
	int mode_idx;

	if (!str)
		return -EINVAL;

	size = strlen(str);
	mode_idx = get_mode_idx_from_str(str, size);

	return phyt_pstate_set_driver(mode_idx);
}
early_param("phyt_pstate", phyt_pstate_param);

MODULE_AUTHOR("Li Jiayi <lijiayi1493@phytium.com.cn>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phytium Processor P-state Frequency Driver");
