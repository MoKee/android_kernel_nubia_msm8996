/*
 * drivers/thermal/msm_thermal_simple.c
 *
 * Copyright (C) 2014-2018, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "msm-thermal: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/slab.h>

#define CPU_MASK(cpu) (1U << (cpu))

/*
 * For MSM8996 (big.LITTLE). CPU0 and CPU1 are LITTLE CPUs; CPU2 and CPU3 are
 * big CPUs.
 */
#define LITTLE_CPU_MASK (CPU_MASK(0) | CPU_MASK(1))

#define UNTHROTTLE_ZONE (-1)

#define DEFAULT_SAMPLING_MS 3000

/* Max possible is currently 100 (0-99 => two digits) */
#define NR_THERMAL_ZONES 16

struct thermal_zone_sysfs {
	struct device_attribute dev_attr[NR_THERMAL_ZONES];
	struct attribute *attr[NR_THERMAL_ZONES + 1];
	struct attribute_group attr_group;
};

struct thermal_config {
	struct qpnp_vadc_chip *vadc_dev;
	enum qpnp_vadc_channels adc_chan;
	bool enabled;
	uint32_t sampling_ms;
};

struct thermal_zone {
	uint32_t freq[2];
	int64_t trip_degC;
	int64_t reset_degC;
};

struct thermal_policy {
	spinlock_t lock;
	struct delayed_work dwork;
	struct thermal_config conf;
	struct thermal_zone zone[NR_THERMAL_ZONES];
	struct workqueue_struct *wq;
	struct thermal_zone_sysfs zfs;
	bool throttle_active;
	int32_t curr_zone;
};

static struct thermal_policy *t_policy_g;

static void update_online_cpu_policy(void);
static uint32_t get_throttle_freq(struct thermal_policy *t,
		int32_t idx, uint32_t cpu);
static void set_throttle_freq(struct thermal_policy *t,
		int32_t idx, uint32_t cpu, uint32_t freq);
static bool validate_cpu_freq(struct cpufreq_frequency_table *pos,
		uint32_t *freq);

static void msm_thermal_main(struct work_struct *work)
{
	struct thermal_policy *t = container_of(work, typeof(*t), dwork.work);
	struct qpnp_vadc_result result;
	int32_t i, old_zone, ret;
	int64_t temp;

	ret = qpnp_vadc_read(t->conf.vadc_dev, t->conf.adc_chan, &result);
	if (ret) {
		pr_err("Unable to read ADC channel\n");
		goto reschedule;
	}

	temp = result.physical;

	spin_lock(&t->lock);

	old_zone = t->curr_zone;

	for (i = 0; i < NR_THERMAL_ZONES; i++) {
		if (!t->zone[i].freq[0]) {
			/*
			 * The current thermal zone is not configured, so use
			 * the previous one and exit.
			 */
			t->curr_zone = i - 1;
			break;
		}

		if (i == (NR_THERMAL_ZONES - 1)) {
			/* Highest zone has been reached, so use it and exit */
			t->curr_zone = i;
			break;
		}

		if (temp > t->zone[i].reset_degC) {
			/*
			 * If temp is less than the trip temp for the next
			 * thermal zone and is greater than or equal to the
			 * trip temp for the current zone, then exit here and
			 * use the current index as the thermal zone.
			 * Otherwise, keep iterating until this is true (or
			 * until we hit the highest thermal zone).
			 */
			if (temp < t->zone[i + 1].trip_degC &&
				(temp >= t->zone[i].trip_degC ||
				t->curr_zone != UNTHROTTLE_ZONE)) {
				t->curr_zone = i;
				break;
			} else if (!i && t->curr_zone == UNTHROTTLE_ZONE &&
				temp < t->zone[0].trip_degC) {
				/*
				 * Don't keep looping if the CPU is currently
				 * unthrottled and the temp is below the first
				 * zone's trip point.
				 */
				break;
			}
		} else if (!i) {
			/*
			 * Unthrottle CPU if temp is at or below the first
			 * zone's reset temp.
			 */
			t->curr_zone = UNTHROTTLE_ZONE;
			break;
		}
	}

	/*
	 * Set the throttle state to active once the current throttle zone is
	 * no longer set to the unthrottle zone.
	 */
	if (t->curr_zone != UNTHROTTLE_ZONE)
		t->throttle_active = true;

	spin_unlock(&t->lock);

	/* Only update CPU policy when the throttle zone changes */
	if (t->curr_zone != old_zone)
		update_online_cpu_policy();

reschedule:
	queue_delayed_work(t->wq, &t->dwork,
				msecs_to_jiffies(t->conf.sampling_ms));
}

static int do_cpu_throttle(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	struct thermal_policy *t = t_policy_g;
	bool active, ret;
	int32_t zone;
	uint32_t new_max;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	spin_lock(&t->lock);
	active = t->throttle_active;
	zone = t->curr_zone;
	spin_unlock(&t->lock);

	/* CPU throttling is not requested */
	if (!active)
		return NOTIFY_OK;

	if (zone == UNTHROTTLE_ZONE) {
		/* Restore original user maxfreq */
		policy->max = policy->user_policy.max;

		/* Thermal throttling is finished */
		spin_lock(&t->lock);
		t->throttle_active = false;
		spin_unlock(&t->lock);
	} else {
		new_max = get_throttle_freq(t, zone, policy->cpu);
		/*
		 * Throttle frequency must always be valid. If it's invalid
		 * (validate_cpu_freq() returns true), then update the
		 * throttle zone freq array with the validated frequency.
		 */
		ret = validate_cpu_freq(policy->freq_table, &new_max);
		if (ret)
			set_throttle_freq(t, zone, policy->cpu, new_max);
		if (policy->max > new_max)
			policy->max = new_max;
	}

	/* Validate the updated maxfreq */
	if (policy->min > policy->max)
		policy->min = policy->max;

	return NOTIFY_OK;
}

static struct notifier_block cpu_throttle_nb = {
	.notifier_call = do_cpu_throttle,
	.priority      = INT_MIN,
};

static void update_online_cpu_policy(void)
{
	uint32_t cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static uint32_t get_throttle_freq(struct thermal_policy *t,
		int32_t idx, uint32_t cpu)
{
	struct thermal_zone *zone = &t->zone[idx];
	uint32_t freq;

	/*
	 * The throttle frequency for a LITTLE CPU is stored at index 0 of
	 * the throttle freq array. The frequency for a big CPU is stored at
	 * index 1.
	 */
	spin_lock(&t->lock);
	freq = zone->freq[CPU_MASK(cpu) & LITTLE_CPU_MASK ? 0 : 1];
	spin_unlock(&t->lock);

	return freq;
}

static void set_throttle_freq(struct thermal_policy *t,
		int32_t idx, uint32_t cpu, uint32_t freq)
{
	struct thermal_zone *zone = &t->zone[idx];

	/*
	 * The throttle frequency for a LITTLE CPU is stored at index 0 of
	 * the throttle freq array. The frequency for a big CPU is stored at
	 * index 1.
	 */
	spin_lock(&t->lock);
	zone->freq[CPU_MASK(cpu) & LITTLE_CPU_MASK ? 0 : 1] = freq;
	spin_unlock(&t->lock);
}

static bool validate_cpu_freq(struct cpufreq_frequency_table *pos,
		uint32_t *freq)
{
	struct cpufreq_frequency_table *next;

	/* Set the cursor to the first valid freq */
	cpufreq_next_valid(&pos);

	/* Requested freq is below the lowest freq, so use the lowest freq */
	if (*freq < pos->frequency) {
		*freq = pos->frequency;
		return true;
	}

	while (1) {
		/* This freq exists in the table so it's definitely valid */
		if (*freq == pos->frequency)
			return false;

		next = pos + 1;

		/* We've gone past the highest freq, so use the highest freq */
		if (!cpufreq_next_valid(&next)) {
			*freq = pos->frequency;
			return true;
		}

		/* Target the next-highest freq */
		if (*freq > pos->frequency && *freq < next->frequency) {
			*freq = next->frequency;
			return true;
		}

		pos = next;
	}

	return false;
}

static uint32_t get_thermal_zone_number(const char *filename)
{
	uint32_t num;
	int ret;

	/* Thermal zone sysfs nodes are named as "zone##" */
	ret = sscanf(filename, "zone%u", &num);
	if (ret != 1)
		return 0;

	return num;
}

static ssize_t enabled_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct thermal_policy *t = t_policy_g;
	uint32_t data;
	int ret;

	ret = kstrtou32(buf, 10, &data);
	if (ret)
		return -EINVAL;

	/* t->conf.enabled is purely cosmetic; it's only used for sysfs */
	t->conf.enabled = data;

	cancel_delayed_work_sync(&t->dwork);

	if (data) {
		queue_delayed_work(t->wq, &t->dwork, 0);
	} else {
		/* Unthrottle all CPUS */
		spin_lock(&t->lock);
		t->curr_zone = UNTHROTTLE_ZONE;
		spin_unlock(&t->lock);
		update_online_cpu_policy();
	}

	return size;
}

static ssize_t sampling_ms_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct thermal_policy *t = t_policy_g;
	uint32_t data;
	int ret;

	ret = kstrtou32(buf, 10, &data);
	if (ret)
		return -EINVAL;

	t->conf.sampling_ms = data;

	return size;
}

static ssize_t thermal_zone_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct thermal_policy *t = t_policy_g;
	uint32_t freq[2], idx;
	int64_t trip_degC, reset_degC;
	int ret;

	ret = sscanf(buf, "%u %u %lld %lld", &freq[0], &freq[1],
						&trip_degC, &reset_degC);
	if (ret != 4)
		return -EINVAL;

	idx = get_thermal_zone_number(attr->attr.name);

	spin_lock(&t->lock);
	/* freq[0] is assigned to LITTLE cluster, freq[1] to big cluster */
	t->zone[idx].freq[0] = freq[0];
	t->zone[idx].freq[1] = freq[1];
	t->zone[idx].trip_degC = trip_degC;
	t->zone[idx].reset_degC = reset_degC;
	spin_unlock(&t->lock);

	return size;
}

static ssize_t enabled_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct thermal_policy *t = t_policy_g;

	return snprintf(buf, PAGE_SIZE, "%d\n", t->conf.enabled);
}

static ssize_t sampling_ms_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct thermal_policy *t = t_policy_g;

	return snprintf(buf, PAGE_SIZE, "%u\n", t->conf.sampling_ms);
}

static ssize_t thermal_zone_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct thermal_policy *t = t_policy_g;
	uint32_t idx;

	idx = get_thermal_zone_number(attr->attr.name);

	return snprintf(buf, PAGE_SIZE, "%u %u %lld %lld\n",
			t->zone[idx].freq[0], t->zone[idx].freq[1],
			t->zone[idx].trip_degC, t->zone[idx].reset_degC);
}

static DEVICE_ATTR(enabled, 0644, enabled_read, enabled_write);
static DEVICE_ATTR(sampling_ms, 0644, sampling_ms_read, sampling_ms_write);

static struct attribute *msm_thermal_attr[] = {
	&dev_attr_enabled.attr,
	&dev_attr_sampling_ms.attr,
	NULL
};

static struct attribute_group msm_thermal_attr_group = {
	.attrs = msm_thermal_attr,
};

static int sysfs_zone_attr_init(struct thermal_policy *t)
{
	char zone_name[7]; /* "zone##" */
	int i;

	/*
	 * All thermal zones use the same read/write functions, so initialize
	 * all of the zones dynamically using a loop.
	 */
	for (i = 0; i < NR_THERMAL_ZONES; i++) {
		snprintf(zone_name, sizeof(zone_name), "zone%d", i);
		t->zfs.dev_attr[i].attr.name = kstrdup(zone_name, GFP_KERNEL);
		if (!t->zfs.dev_attr[i].attr.name)
			goto free_name;
		t->zfs.dev_attr[i].attr.mode = VERIFY_OCTAL_PERMISSIONS(0644);
		t->zfs.dev_attr[i].show = thermal_zone_read;
		t->zfs.dev_attr[i].store = thermal_zone_write;
		t->zfs.attr[i] = &t->zfs.dev_attr[i].attr;
	}

	/* Last element in the attribute array must be NULL */
	t->zfs.attr[NR_THERMAL_ZONES] = NULL;
	t->zfs.attr_group.attrs = t->zfs.attr;

	return 0;

free_name:
	while (i--)
		kfree(t->zfs.dev_attr[i].attr.name);
	return -ENOMEM;
}

static int sysfs_thermal_init(struct thermal_policy *t)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("msm_thermal", kernel_kobj);
	if (!kobj) {
		pr_err("Failed to create kobject\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(kobj, &msm_thermal_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs interface, ret=%d\n", ret);
		goto put_kobj;
	}

	ret = sysfs_zone_attr_init(t);
	if (ret) {
		pr_err("Failed to init thermal zone attrs, ret=%d\n", ret);
		goto put_kobj;
	}

	ret = sysfs_create_group(kobj, &t->zfs.attr_group);
	if (ret) {
		pr_err("Failed to create thermal zone sysfs, ret=%d\n", ret);
		goto put_kobj;
	}

	return 0;

put_kobj:
	kobject_put(kobj);
	return ret;
}

static int msm_thermal_parse_dt(struct platform_device *pdev,
			struct thermal_policy *t)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	t->conf.vadc_dev = qpnp_get_vadc(&pdev->dev, "thermal");
	if (IS_ERR(t->conf.vadc_dev)) {
		ret = PTR_ERR(t->conf.vadc_dev);
		if (ret != -EPROBE_DEFER)
			pr_err("VADC property missing\n");
		return ret;
	}

	ret = of_property_read_u32(np, "qcom,adc-channel", &t->conf.adc_chan);
	if (ret)
		pr_err("ADC-channel property missing\n");

	return ret;
}

static struct thermal_policy *alloc_thermal_policy(void)
{
	struct thermal_policy *t;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	t->wq = alloc_workqueue("msm_thermal_wq", WQ_HIGHPRI, 0);
	if (!t->wq) {
		pr_err("Failed to allocate workqueue\n");
		goto free_t;
	}

	return t;

free_t:
	kfree(t);
	return NULL;
}

static int msm_thermal_probe(struct platform_device *pdev)
{
	struct thermal_policy *t;
	int ret;

	t = alloc_thermal_policy();
	if (!t) {
		pr_err("Failed to allocate thermal policy\n");
		return -ENOMEM;
	}

	ret = msm_thermal_parse_dt(pdev, t);
	if (ret)
		goto free_mem;

	t->conf.sampling_ms = DEFAULT_SAMPLING_MS;

	spin_lock_init(&t->lock);

	/* Allow global thermal policy access */
	t_policy_g = t;

	INIT_DELAYED_WORK(&t->dwork, msm_thermal_main);

	ret = sysfs_thermal_init(t);
	if (ret)
		goto free_mem;

	cpufreq_register_notifier(&cpu_throttle_nb, CPUFREQ_POLICY_NOTIFIER);

	return 0;

free_mem:
	kfree(t);
	return ret;
}

static const struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal-simple"},
	{ },
};

static struct platform_driver msm_thermal_device = {
	.probe = msm_thermal_probe,
	.driver = {
		.name = "msm-thermal-simple",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

static int __init msm_thermal_init(void)
{
	return platform_driver_register(&msm_thermal_device);
}
device_initcall(msm_thermal_init);
