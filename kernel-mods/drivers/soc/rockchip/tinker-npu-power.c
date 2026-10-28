// SPDX-License-Identifier: GPL-2.0
/*
 * Power sequencing of the RK3399Pro NPU on the ASUS Tinker Edge R.
 *
 * The NPU is a separate die with its own CPU: the host powers it up, then
 * downloads firmware over USB (or PCIe) and talks to it through
 * npu_transfer_proxy. The vendor 4.4 kernel left the sequencing to the ASUS
 * user space tool `npu_powerctrl`, which drives GPIOs through sysfs and
 * toggles a clock through a vendor-only debugfs file. This driver does the
 * same sequence from the device tree instead.
 *
 * Sequence (reconstructed from npu_powerctrl V1.1):
 *   32 kHz PMIC clock on, NPU reference clock off, all lines low,
 *   rail lines high one by one (2 ms apart), reference clock on,
 *   release lines high one by one (2 ms apart, 25 ms before the last).
 * Power down is the reverse of "all lines low".
 *
 * System sleep (reconstructed from `npu_powerctrl -s` / `-r`, which the vendor
 * system ran from its suspend hooks): the NPU die is a computer of its own and
 * sleeps by itself, keeping its memory and the models loaded on it.
 *   suspend: a 20 ms pulse on the wake line, then up to 1 s for the sleep-state
 *            line to go high; release lines 0 and 2 low, 32 kHz clock off.
 *   resume:  32 kHz clock on, release lines 0 and 2 high, 10 ms, a 20 ms pulse
 *            on the wake line, then up to 1 s for the sleep-state line to go low.
 * Without this the die loses its clocks in deep sleep and comes back in its
 * boot ROM, with the firmware and every model gone.
 *
 * Copyright (C) 2026 tinker-edge-r project
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct tinker_npu_power {
	struct device *dev;
	struct clk *ref_clk;	/* clk_wifi_pmu, reference clock of the NPU */
	struct clk *clk32k;	/* PMIC rk808-clkout2 */
	struct gpio_descs *rails;
	struct gpio_descs *release;
	struct gpio_desc *wake;	/* held low; pulsed only on suspend/resume */
	struct gpio_desc *sleep_state;	/* input: high while the NPU sleeps */
	struct mutex lock;
	bool on;
	bool asleep;		/* put to sleep by tinker_npu_power_suspend() */
};

/* the two release lines that the vendor tool drops while the NPU sleeps (GPIO1_C6, GPIO1_D0) */
static const int npu_sleep_release[] = { 0, 2 };

static void npu_wake_pulse(struct tinker_npu_power *p)
{
	gpiod_set_value_cansleep(p->wake, 1);
	msleep(20);
	gpiod_set_value_cansleep(p->wake, 0);
}

/* wait up to 1 s for the sleep-state line to read `level` */
static bool npu_wait_sleep_state(struct tinker_npu_power *p, int level)
{
	int i;

	for (i = 0; i < 100; i++) {
		if (gpiod_get_value_cansleep(p->sleep_state) == level)
			return true;
		msleep(10);
	}
	return gpiod_get_value_cansleep(p->sleep_state) == level;
}

static void npu_all_low(struct tinker_npu_power *p)
{
	int i;

	for (i = p->release->ndescs - 1; i >= 0; i--)
		gpiod_set_value_cansleep(p->release->desc[i], 0);
	for (i = p->rails->ndescs - 1; i >= 0; i--)
		gpiod_set_value_cansleep(p->rails->desc[i], 0);
	if (p->wake)
		gpiod_set_value_cansleep(p->wake, 0);
}

static int npu_power_up_locked(struct tinker_npu_power *p)
{
	int ret, i;

	if (p->on)
		return 0;

	ret = clk_prepare_enable(p->clk32k);
	if (ret)
		return ret;

	npu_all_low(p);
	usleep_range(2000, 3000);

	for (i = 0; i < p->rails->ndescs; i++) {
		gpiod_set_value_cansleep(p->rails->desc[i], 1);
		usleep_range(2000, 3000);
	}

	/*
	 * The NPU die (RK1808 family) takes this clock as its 24 MHz crystal input
	 * and derives its PLLs and USB PHY clock from it. The default of the clock
	 * tree is 26 MHz (Wi-Fi), which keeps the NPU from enumerating on USB.
	 */
	ret = clk_set_rate(p->ref_clk, 24000000);
	if (ret)
		dev_warn(p->dev, "cannot set the reference clock to 24 MHz: %d\n", ret);
	else
		dev_info(p->dev, "reference clock %lu Hz\n", clk_get_rate(p->ref_clk));

	ret = clk_prepare_enable(p->ref_clk);
	if (ret) {
		npu_all_low(p);
		clk_disable_unprepare(p->clk32k);
		return ret;
	}

	for (i = 0; i < p->release->ndescs; i++) {
		if (i == p->release->ndescs - 1)
			msleep(25);
		gpiod_set_value_cansleep(p->release->desc[i], 1);
		usleep_range(2000, 3000);
	}

	p->on = true;
	return 0;
}

static void npu_power_down_locked(struct tinker_npu_power *p)
{
	if (!p->on)
		return;

	npu_all_low(p);
	clk_disable_unprepare(p->ref_clk);
	clk_disable_unprepare(p->clk32k);
	p->on = false;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct tinker_npu_power *p = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", p->on);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct tinker_npu_power *p = dev_get_drvdata(dev);
	bool val;
	int ret = 0;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	mutex_lock(&p->lock);
	if (val)
		ret = npu_power_up_locked(p);
	else
		npu_power_down_locked(p);
	mutex_unlock(&p->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enable);

/* Write 1 to power-cycle the NPU (what `npu_powerctrl -i; -o` did). */
static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct tinker_npu_power *p = dev_get_drvdata(dev);
	bool val;
	int ret;

	if (kstrtobool(buf, &val) || !val)
		return -EINVAL;

	mutex_lock(&p->lock);
	npu_power_down_locked(p);
	msleep(100);
	ret = npu_power_up_locked(p);
	mutex_unlock(&p->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *tinker_npu_power_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_reset.attr,
	NULL
};
ATTRIBUTE_GROUPS(tinker_npu_power);

static int tinker_npu_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tinker_npu_power *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = dev;
	mutex_init(&p->lock);

	p->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(p->ref_clk))
		return dev_err_probe(dev, PTR_ERR(p->ref_clk), "ref clock\n");

	p->clk32k = devm_clk_get(dev, "clk32k");
	if (IS_ERR(p->clk32k))
		return dev_err_probe(dev, PTR_ERR(p->clk32k), "32k clock\n");

	p->rails = devm_gpiod_get_array(dev, "rail", GPIOD_OUT_LOW);
	if (IS_ERR(p->rails))
		return dev_err_probe(dev, PTR_ERR(p->rails), "rail gpios\n");

	p->release = devm_gpiod_get_array(dev, "release", GPIOD_OUT_LOW);
	if (IS_ERR(p->release))
		return dev_err_probe(dev, PTR_ERR(p->release), "release gpios\n");

	p->wake = devm_gpiod_get_optional(dev, "wake", GPIOD_OUT_LOW);
	if (IS_ERR(p->wake))
		return dev_err_probe(dev, PTR_ERR(p->wake), "wake gpio\n");

	p->sleep_state = devm_gpiod_get_optional(dev, "sleep-state", GPIOD_IN);
	if (IS_ERR(p->sleep_state))
		return dev_err_probe(dev, PTR_ERR(p->sleep_state), "sleep-state gpio\n");

	platform_set_drvdata(pdev, p);

	if (!of_property_read_bool(dev->of_node, "tinker,keep-off")) {
		mutex_lock(&p->lock);
		ret = npu_power_up_locked(p);
		mutex_unlock(&p->lock);
		if (ret)
			return dev_err_probe(dev, ret, "power up failed\n");
		dev_info(dev, "NPU powered up\n");
	}

	return 0;
}

static void tinker_npu_power_remove(struct platform_device *pdev)
{
	struct tinker_npu_power *p = platform_get_drvdata(pdev);

	mutex_lock(&p->lock);
	npu_power_down_locked(p);
	mutex_unlock(&p->lock);
}

static int tinker_npu_power_suspend(struct device *dev)
{
	struct tinker_npu_power *p = dev_get_drvdata(dev);
	unsigned int i;

	mutex_lock(&p->lock);
	if (!p->on || !p->wake || !p->sleep_state ||
	    p->release->ndescs <= npu_sleep_release[ARRAY_SIZE(npu_sleep_release) - 1])
		goto out;
	if (!gpiod_get_value_cansleep(p->sleep_state)) {
		npu_wake_pulse(p);
		if (!npu_wait_sleep_state(p, 1)) {
			dev_warn(dev, "the NPU did not go to sleep within 1 s: it may need its firmware again after resume\n");
			goto out;
		}
	}
	for (i = 0; i < ARRAY_SIZE(npu_sleep_release); i++)
		gpiod_set_value_cansleep(p->release->desc[npu_sleep_release[i]], 0);
	clk_disable_unprepare(p->clk32k);
	p->asleep = true;
	msleep(1000);		/* as the vendor tool */
	dev_info(dev, "NPU asleep\n");
out:
	mutex_unlock(&p->lock);
	return 0;
}

static int tinker_npu_power_resume(struct device *dev)
{
	struct tinker_npu_power *p = dev_get_drvdata(dev);
	unsigned int i;
	int ret;

	mutex_lock(&p->lock);
	if (!p->asleep)
		goto out;
	p->asleep = false;
	ret = clk_prepare_enable(p->clk32k);
	if (ret)
		dev_warn(dev, "cannot enable the 32 kHz clock: %d\n", ret);
	for (i = 0; i < ARRAY_SIZE(npu_sleep_release); i++)
		gpiod_set_value_cansleep(p->release->desc[npu_sleep_release[i]], 1);
	msleep(10);
	npu_wake_pulse(p);
	if (npu_wait_sleep_state(p, 0))
		dev_info(dev, "NPU awake\n");
	else
		dev_warn(dev, "the NPU did not wake up within 1 s: it may need its firmware again\n");
out:
	mutex_unlock(&p->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(tinker_npu_power_pm, tinker_npu_power_suspend, tinker_npu_power_resume);

static const struct of_device_id tinker_npu_power_of_match[] = {
	{ .compatible = "asus,tinker-edge-r-npu-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, tinker_npu_power_of_match);

static struct platform_driver tinker_npu_power_driver = {
	.probe = tinker_npu_power_probe,
	.remove = tinker_npu_power_remove,
	.driver = {
		.name = "tinker-npu-power",
		.of_match_table = tinker_npu_power_of_match,
		.dev_groups = tinker_npu_power_groups,
		.pm = pm_sleep_ptr(&tinker_npu_power_pm),
	},
};
module_platform_driver(tinker_npu_power_driver);

MODULE_DESCRIPTION("ASUS Tinker Edge R RK3399Pro NPU power sequencing");
MODULE_LICENSE("GPL");
