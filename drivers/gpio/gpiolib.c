#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/idr.h>
#include <linux/slab.h>

#define CREATE_TRACE_POINTS
#include <trace/events/gpio.h>

/* Optional implementation infrastructure for GPIO interfaces.
 *
 * Platforms may want to use this if they tend to use very many GPIOs
 * that aren't part of a System-On-Chip core; or across I2C/SPI/etc.
 *
 * When kernel footprint or instruction count is an issue, simpler
 * implementations may be preferred.  The GPIO programming interface
 * allows for inlining speed-critical get/set operations for common
 * cases, so that access to SOC-integrated GPIOs can sometimes cost
 * only an instruction or two per bit.
 */


/* When debugging, extend minimal trust to callers and platform code.
 * Also emit diagnostic messages that may help initial bringup, when
 * board setup or driver bugs are most common.
 *
 * Otherwise, minimize overhead in what may be bitbanging codepaths.
 */
#ifdef	DEBUG
#define	extra_checks	1
#else
#define	extra_checks	0
#endif

/* gpio_lock prevents conflicts during gpio_desc[] table updates.
 * While any GPIO is requested, its gpio_chip is not removable;
 * each GPIO's "requested" flag serves as a lock and refcount.
 */
static DEFINE_SPINLOCK(gpio_lock);

/* 描述GPIO的结构，每个GPIO口对应一个 */
struct gpio_desc {
	struct gpio_chip	*chip;   /* 对应到平台相关的，是具体的操作函数等 */
	unsigned long		flags;   /* 标志位，如标记是否已申请，释放 */
/* flag symbols are bit numbers */
#define FLAG_REQUESTED	0       /* 是否已被 request  */
#define FLAG_IS_OUT	1           /* GPIO为 OUT 模式 */
#define FLAG_RESERVED	2       /* 标记该 desc 是否为预留 */
#define FLAG_EXPORT	3	/* protected by sysfs_lock */
#define FLAG_SYSFS	4	/* exported via /sys/class/gpio/control */
#define FLAG_TRIG_FALL	5	/* trigger on falling edge */
#define FLAG_TRIG_RISE	6	/* trigger on rising edge */
#define FLAG_ACTIVE_LOW	7	/* sysfs value has active low */
#define FLAG_OPEN_DRAIN	8	/* Gpio is open drain type */
#define FLAG_OPEN_SOURCE 9	/* Gpio is open source type */

#define ID_SHIFT	16	/* add new flags before this one */

#define GPIO_FLAGS_MASK		((1 << ID_SHIFT) - 1)
#define GPIO_TRIGGER_MASK	(BIT(FLAG_TRIG_FALL) | BIT(FLAG_TRIG_RISE))

#ifdef CONFIG_DEBUG_FS
	const char		*label; /* 调试用 */
#endif
};
/* 定义全局数组，管理所有GPIO
 * arch/arm/mach-exynos/include/mach/gpio.h */
static struct gpio_desc gpio_desc[ARCH_NR_GPIOS];

#ifdef CONFIG_GPIO_SYSFS
static DEFINE_IDR(dirent_idr);
#endif

/* 设置标签，调试用 */
static inline void desc_set_label(struct gpio_desc *d, const char *label)
{
#ifdef CONFIG_DEBUG_FS
	d->label = label;
#endif
}

/* Warn when drivers omit gpio_request() calls -- legal but ill-advised
 * when setting direction, and otherwise illegal.  Until board setup code
 * and drivers use explicit requests everywhere (which won't happen when
 * those calls have no teeth) we can't avoid autorequesting.  This nag
 * message should motivate switching to explicit requests... so should
 * the weaker cleanup after faults, compared to gpio_request().
 *
 * NOTE: the autorequest mechanism is going away; at this point it's
 * only "legal" in the sense that (old) code using it won't break yet,
 * but instead only triggers a WARN() stack dump.
 */
static int gpio_ensure_requested(struct gpio_desc *desc, unsigned offset)
{
	const struct gpio_chip *chip = desc->chip;
	const int gpio = chip->base + offset;

    /* 测试 desc 结构中 flags，判断是否已申请，未申请则自动申请 */
	if (WARN(test_and_set_bit(FLAG_REQUESTED, &desc->flags) == 0,
			"autorequest GPIO-%d\n", gpio)) {
		if (!try_module_get(chip->owner)) {
			pr_err("GPIO-%d: module can't be gotten \n", gpio);
			clear_bit(FLAG_REQUESTED, &desc->flags);
			/* lose */
			return -EIO;
		}
        /* 设置GPIO标签为  [auto] */
		desc_set_label(desc, "[auto]");
        /* 如果gpio对应的chip有request实现，则申请时必须调用 */
		/* caller must chip->request() w/o spinlock */
		if (chip->request)
			return 1;
	}
	return 0;
}

/* 反向查 gpio_chip ，直接在 gpio_desc 中索引即可 */
/* caller holds gpio_lock *OR* gpio is marked as requested */
struct gpio_chip *gpio_to_chip(unsigned gpio)
{
	return gpio_desc[gpio].chip;
}

/* 动态分配GPIO base(首个GPIO号)， 适用于热插拔的设备 */
/* dynamic allocation of GPIOs, e.g. on a hotplugged device */
static int gpiochip_find_base(int ngpio)
{
	int i;
	int spare = 0;
	int base = -ENOSPC;

    /* 从后面遍历所有的 gpio_desc 结构，找到满足数量要求的空闲结构 */
	for (i = ARCH_NR_GPIOS - 1; i >= 0 ; i--) {
		struct gpio_desc *desc = &gpio_desc[i];
		struct gpio_chip *chip = desc->chip;

        /* chip 为NULL 并且该 desc 并未占用，则计数 */
		if (!chip && !test_bit(FLAG_RESERVED, &desc->flags)) {
			spare++;
            /* 空闲的 desc满足数量要求，则使用这部分 desc */
			if (spare == ngpio) {
				base = i;
				break;
			}
		} else {
			spare = 0;
			if (chip)
				i -= chip->ngpio - 1;
		}
	}

	if (gpio_is_valid(base))
		pr_debug("%s: found new base at %d\n", __func__, base);
	return base;
}

/**
 * gpiochip_reserve() - reserve range of gpios to use with platform code only
 * @start: starting gpio number
 * @ngpio: number of gpios to reserve
 * Context: platform init, potentially before irqs or kmalloc will work
 *
 * Returns a negative errno if any gpio within the range is already reserved
 * or registered, else returns zero as a success code.  Use this function
 * to mark a range of gpios as unavailable for dynamic gpio number allocation,
 * for example because its driver support is not yet loaded.
 */
/* 保留一段 GPIO 号，被保留的部分，在动态分配时不会占用 */
int __init gpiochip_reserve(int start, int ngpio)
{
	int ret = 0;
	unsigned long flags;
	int i;

	if (!gpio_is_valid(start) || !gpio_is_valid(start + ngpio - 1))
		return -EINVAL;

	spin_lock_irqsave(&gpio_lock, flags);

    /* 遍历 gpio_desc 数组，如果已被使用(chip) 或 已被标记为占用
     * 则返回错误 
     * 这里需要注意，因为是遍历到一个未被占用 则会进行标记 
     * 所以，即使返回了错误，也有可能已经有被标记了的 */
	for (i = start; i < start + ngpio; i++) {
		struct gpio_desc *desc = &gpio_desc[i];

		if (desc->chip || test_bit(FLAG_RESERVED, &desc->flags)) {
			ret = -EBUSY;
			goto err;
		}

		set_bit(FLAG_RESERVED, &desc->flags);
	}

	pr_debug("%s: reserved gpios from %d to %d\n",
		 __func__, start, start + ngpio - 1);
err:
	spin_unlock_irqrestore(&gpio_lock, flags);

	return ret;
}

// TODO: 关于 sysfs 系统的 gpio 相关分析
#ifdef CONFIG_GPIO_SYSFS

/* lock protects against unexport_gpio() being called while
 * sysfs files are active.
 */
static DEFINE_MUTEX(sysfs_lock);

/*
 * /sys/class/gpio/gpioN... only for GPIOs that are exported
 *   /direction
 *      * MAY BE OMITTED if kernel won't allow direction changes
 *      * is read/write as "in" or "out"
 *      * may also be written as "high" or "low", initializing
 *        output value as specified ("out" implies "low")
 *   /value
 *      * always readable, subject to hardware behavior
 *      * may be writable, as zero/nonzero
 *   /edge
 *      * configures behavior of poll(2) on /value
 *      * available only if pin can generate IRQs on input
 *      * is read/write as "none", "falling", "rising", or "both"
 *   /active_low
 *      * configures polarity of /value
 *      * is read/write as zero/nonzero
 *      * also affects existing and subsequent "falling" and "rising"
 *        /edge configuration
 */

/* 输出 GPIO 的方向 out  in  */
static ssize_t gpio_direction_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	ssize_t			status;

	mutex_lock(&sysfs_lock);

    /* 测试是否已被 export  */
	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else
		status = sprintf(buf, "%s\n",
			test_bit(FLAG_IS_OUT, &desc->flags)
				? "out" : "in");

	mutex_unlock(&sysfs_lock);
	return status;
}

/* 设置 GPIO 的方向，可使用 High low 直接设置输出电平 */
static ssize_t gpio_direction_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	unsigned		gpio = desc - gpio_desc;
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else if (sysfs_streq(buf, "high"))
		status = gpio_direction_output(gpio, 1);
	else if (sysfs_streq(buf, "out") || sysfs_streq(buf, "low"))
		status = gpio_direction_output(gpio, 0);
	else if (sysfs_streq(buf, "in"))
		status = gpio_direction_input(gpio);
	else
		status = -EINVAL;

	mutex_unlock(&sysfs_lock);
	return status ? : size;
}

static /* const */ DEVICE_ATTR(direction, 0644,
		gpio_direction_show, gpio_direction_store);

static ssize_t gpio_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	unsigned		gpio = desc - gpio_desc;
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags)) {
		status = -EIO;
	} else {
		int value;

		value = !!gpio_get_value_cansleep(gpio);
		if (test_bit(FLAG_ACTIVE_LOW, &desc->flags))
			value = !value;

		status = sprintf(buf, "%d\n", value);
	}

	mutex_unlock(&sysfs_lock);
	return status;
}

static ssize_t gpio_value_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	unsigned		gpio = desc - gpio_desc;
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else if (!test_bit(FLAG_IS_OUT, &desc->flags))
		status = -EPERM;
	else {
		long		value;

		status = strict_strtol(buf, 0, &value);
		if (status == 0) {
			if (test_bit(FLAG_ACTIVE_LOW, &desc->flags))
				value = !value;
			gpio_set_value_cansleep(gpio, value != 0);
			status = size;
		}
	}

	mutex_unlock(&sysfs_lock);
	return status;
}

static const DEVICE_ATTR(value, 0644,
		gpio_value_show, gpio_value_store);

static irqreturn_t gpio_sysfs_irq(int irq, void *priv)
{
	struct sysfs_dirent	*value_sd = priv;

	sysfs_notify_dirent(value_sd);
	return IRQ_HANDLED;
}

static int gpio_setup_irq(struct gpio_desc *desc, struct device *dev,
		unsigned long gpio_flags)
{
	struct sysfs_dirent	*value_sd;
	unsigned long		irq_flags;
	int			ret, irq, id;

	if ((desc->flags & GPIO_TRIGGER_MASK) == gpio_flags)
		return 0;

	irq = gpio_to_irq(desc - gpio_desc);
	if (irq < 0)
		return -EIO;

	id = desc->flags >> ID_SHIFT;
	value_sd = idr_find(&dirent_idr, id);
	if (value_sd)
		free_irq(irq, value_sd);

	desc->flags &= ~GPIO_TRIGGER_MASK;

	if (!gpio_flags) {
		ret = 0;
		goto free_id;
	}

	irq_flags = IRQF_SHARED;
	if (test_bit(FLAG_TRIG_FALL, &gpio_flags))
		irq_flags |= test_bit(FLAG_ACTIVE_LOW, &desc->flags) ?
			IRQF_TRIGGER_RISING : IRQF_TRIGGER_FALLING;
	if (test_bit(FLAG_TRIG_RISE, &gpio_flags))
		irq_flags |= test_bit(FLAG_ACTIVE_LOW, &desc->flags) ?
			IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;

	if (!value_sd) {
		value_sd = sysfs_get_dirent(dev->kobj.sd, NULL, "value");
		if (!value_sd) {
			ret = -ENODEV;
			goto err_out;
		}

		do {
			ret = -ENOMEM;
			if (idr_pre_get(&dirent_idr, GFP_KERNEL))
				ret = idr_get_new_above(&dirent_idr, value_sd,
							1, &id);
		} while (ret == -EAGAIN);

		if (ret)
			goto free_sd;

		desc->flags &= GPIO_FLAGS_MASK;
		desc->flags |= (unsigned long)id << ID_SHIFT;

		if (desc->flags >> ID_SHIFT != id) {
			ret = -ERANGE;
			goto free_id;
		}
	}

	ret = request_any_context_irq(irq, gpio_sysfs_irq, irq_flags,
				"gpiolib", value_sd);
	if (ret < 0)
		goto free_id;

	desc->flags |= gpio_flags;
	return 0;

free_id:
	idr_remove(&dirent_idr, id);
	desc->flags &= GPIO_FLAGS_MASK;
free_sd:
	if (value_sd)
		sysfs_put(value_sd);
err_out:
	return ret;
}

static const struct {
	const char *name;
	unsigned long flags;
} trigger_types[] = {
	{ "none",    0 },
	{ "falling", BIT(FLAG_TRIG_FALL) },
	{ "rising",  BIT(FLAG_TRIG_RISE) },
	{ "both",    BIT(FLAG_TRIG_FALL) | BIT(FLAG_TRIG_RISE) },
};

static ssize_t gpio_edge_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else {
		int i;

		status = 0;
		for (i = 0; i < ARRAY_SIZE(trigger_types); i++)
			if ((desc->flags & GPIO_TRIGGER_MASK)
					== trigger_types[i].flags) {
				status = sprintf(buf, "%s\n",
						 trigger_types[i].name);
				break;
			}
	}

	mutex_unlock(&sysfs_lock);
	return status;
}

static ssize_t gpio_edge_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct gpio_desc	*desc = dev_get_drvdata(dev);
	ssize_t			status;
	int			i;

	for (i = 0; i < ARRAY_SIZE(trigger_types); i++)
		if (sysfs_streq(trigger_types[i].name, buf))
			goto found;
	return -EINVAL;

found:
	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else {
		status = gpio_setup_irq(desc, dev, trigger_types[i].flags);
		if (!status)
			status = size;
	}

	mutex_unlock(&sysfs_lock);

	return status;
}

static DEVICE_ATTR(edge, 0644, gpio_edge_show, gpio_edge_store);

static int sysfs_set_active_low(struct gpio_desc *desc, struct device *dev,
				int value)
{
	int			status = 0;

	if (!!test_bit(FLAG_ACTIVE_LOW, &desc->flags) == !!value)
		return 0;

	if (value)
		set_bit(FLAG_ACTIVE_LOW, &desc->flags);
	else
		clear_bit(FLAG_ACTIVE_LOW, &desc->flags);

	/* reconfigure poll(2) support if enabled on one edge only */
	if (dev != NULL && (!!test_bit(FLAG_TRIG_RISE, &desc->flags) ^
				!!test_bit(FLAG_TRIG_FALL, &desc->flags))) {
		unsigned long trigger_flags = desc->flags & GPIO_TRIGGER_MASK;

		gpio_setup_irq(desc, dev, 0);
		status = gpio_setup_irq(desc, dev, trigger_flags);
	}

	return status;
}

static ssize_t gpio_active_low_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	const struct gpio_desc	*desc = dev_get_drvdata(dev);
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else
		status = sprintf(buf, "%d\n",
				!!test_bit(FLAG_ACTIVE_LOW, &desc->flags));

	mutex_unlock(&sysfs_lock);

	return status;
}

static ssize_t gpio_active_low_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct gpio_desc	*desc = dev_get_drvdata(dev);
	ssize_t			status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags)) {
		status = -EIO;
	} else {
		long		value;

		status = strict_strtol(buf, 0, &value);
		if (status == 0)
			status = sysfs_set_active_low(desc, dev, value != 0);
	}

	mutex_unlock(&sysfs_lock);

	return status ? : size;
}

static const DEVICE_ATTR(active_low, 0644,
		gpio_active_low_show, gpio_active_low_store);

static const struct attribute *gpio_attrs[] = {
	&dev_attr_value.attr,
	&dev_attr_active_low.attr,
	NULL,
};

static const struct attribute_group gpio_attr_group = {
	.attrs = (struct attribute **) gpio_attrs,
};

/*
 * /sys/class/gpio/gpiochipN/
 *   /base ... matching gpio_chip.base (N)
 *   /label ... matching gpio_chip.label
 *   /ngpio ... matching gpio_chip.ngpio
 */

static ssize_t chip_base_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	const struct gpio_chip	*chip = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", chip->base);
}
static DEVICE_ATTR(base, 0444, chip_base_show, NULL);

static ssize_t chip_label_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	const struct gpio_chip	*chip = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", chip->label ? : "");
}
static DEVICE_ATTR(label, 0444, chip_label_show, NULL);

static ssize_t chip_ngpio_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	const struct gpio_chip	*chip = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", chip->ngpio);
}
static DEVICE_ATTR(ngpio, 0444, chip_ngpio_show, NULL);

static const struct attribute *gpiochip_attrs[] = {
	&dev_attr_base.attr,
	&dev_attr_label.attr,
	&dev_attr_ngpio.attr,
	NULL,
};

static const struct attribute_group gpiochip_attr_group = {
	.attrs = (struct attribute **) gpiochip_attrs,
};

/*
 * /sys/class/gpio/export ... write-only
 *	integer N ... number of GPIO to export (full access)
 * /sys/class/gpio/unexport ... write-only
 *	integer N ... number of GPIO to unexport
 */
static ssize_t export_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t len)
{
	long	gpio;
	int	status;

	status = strict_strtol(buf, 0, &gpio);
	if (status < 0)
		goto done;

	/* No extra locking here; FLAG_SYSFS just signifies that the
	 * request and export were done by on behalf of userspace, so
	 * they may be undone on its behalf too.
	 */

	status = gpio_request(gpio, "sysfs");
	if (status < 0)
		goto done;

	status = gpio_export(gpio, true);
	if (status < 0)
		gpio_free(gpio);
	else
		set_bit(FLAG_SYSFS, &gpio_desc[gpio].flags);

done:
	if (status)
		pr_debug("%s: status %d\n", __func__, status);
	return status ? : len;
}

static ssize_t unexport_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t len)
{
	long	gpio;
	int	status;

	status = strict_strtol(buf, 0, &gpio);
	if (status < 0)
		goto done;

	status = -EINVAL;

	/* reject bogus commands (gpio_unexport ignores them) */
	if (!gpio_is_valid(gpio))
		goto done;

	/* No extra locking here; FLAG_SYSFS just signifies that the
	 * request and export were done by on behalf of userspace, so
	 * they may be undone on its behalf too.
	 */
	if (test_and_clear_bit(FLAG_SYSFS, &gpio_desc[gpio].flags)) {
		status = 0;
		gpio_free(gpio);
	}
done:
	if (status)
		pr_debug("%s: status %d\n", __func__, status);
	return status ? : len;
}

static struct class_attribute gpio_class_attrs[] = {
	__ATTR(export, 0200, NULL, export_store),
	__ATTR(unexport, 0200, NULL, unexport_store),
	__ATTR_NULL,
};

static struct class gpio_class = {
	.name =		"gpio",
	.owner =	THIS_MODULE,

	.class_attrs =	gpio_class_attrs,
};


/**
 * gpio_export - export a GPIO through sysfs
 * @gpio: gpio to make available, already requested
 * @direction_may_change: true if userspace may change gpio direction
 * Context: arch_initcall or later
 *
 * When drivers want to make a GPIO accessible to userspace after they
 * have requested it -- perhaps while debugging, or as part of their
 * public interface -- they may use this routine.  If the GPIO can
 * change direction (some can't) and the caller allows it, userspace
 * will see "direction" sysfs attribute which may be used to change
 * the gpio's direction.  A "value" attribute will always be provided.
 *
 * Returns zero on success, else an error.
 */
int gpio_export(unsigned gpio, bool direction_may_change)
{
	unsigned long		flags;
	struct gpio_desc	*desc;
	int			status = -EINVAL;
	const char		*ioname = NULL;

	/* can't export until sysfs is available ... */
	if (!gpio_class.p) {
		pr_debug("%s: called too early!\n", __func__);
		return -ENOENT;
	}

	if (!gpio_is_valid(gpio))
		goto done;

	mutex_lock(&sysfs_lock);

	spin_lock_irqsave(&gpio_lock, flags);
	desc = &gpio_desc[gpio];
	if (test_bit(FLAG_REQUESTED, &desc->flags)
			&& !test_bit(FLAG_EXPORT, &desc->flags)) {
		status = 0;
		if (!desc->chip->direction_input
				|| !desc->chip->direction_output)
			direction_may_change = false;
	}
	spin_unlock_irqrestore(&gpio_lock, flags);

	if (desc->chip->names && desc->chip->names[gpio - desc->chip->base])
		ioname = desc->chip->names[gpio - desc->chip->base];

	if (status == 0) {
		struct device	*dev;

		dev = device_create(&gpio_class, desc->chip->dev, MKDEV(0, 0),
				desc, ioname ? ioname : "gpio%u", gpio);
		if (!IS_ERR(dev)) {
			status = sysfs_create_group(&dev->kobj,
						&gpio_attr_group);

			if (!status && direction_may_change)
				status = device_create_file(dev,
						&dev_attr_direction);

			if (!status && gpio_to_irq(gpio) >= 0
					&& (direction_may_change
						|| !test_bit(FLAG_IS_OUT,
							&desc->flags)))
				status = device_create_file(dev,
						&dev_attr_edge);

			if (status != 0)
				device_unregister(dev);
		} else
			status = PTR_ERR(dev);
		if (status == 0)
			set_bit(FLAG_EXPORT, &desc->flags);
	}

	mutex_unlock(&sysfs_lock);

done:
	if (status)
		pr_debug("%s: gpio%d status %d\n", __func__, gpio, status);

	return status;
}
EXPORT_SYMBOL_GPL(gpio_export);

static int match_export(struct device *dev, void *data)
{
	return dev_get_drvdata(dev) == data;
}

/**
 * gpio_export_link - create a sysfs link to an exported GPIO node
 * @dev: device under which to create symlink
 * @name: name of the symlink
 * @gpio: gpio to create symlink to, already exported
 *
 * Set up a symlink from /sys/.../dev/name to /sys/class/gpio/gpioN
 * node. Caller is responsible for unlinking.
 *
 * Returns zero on success, else an error.
 */
int gpio_export_link(struct device *dev, const char *name, unsigned gpio)
{
	struct gpio_desc	*desc;
	int			status = -EINVAL;

	if (!gpio_is_valid(gpio))
		goto done;

	mutex_lock(&sysfs_lock);

	desc = &gpio_desc[gpio];

	if (test_bit(FLAG_EXPORT, &desc->flags)) {
		struct device *tdev;

		tdev = class_find_device(&gpio_class, NULL, desc, match_export);
		if (tdev != NULL) {
			status = sysfs_create_link(&dev->kobj, &tdev->kobj,
						name);
		} else {
			status = -ENODEV;
		}
	}

	mutex_unlock(&sysfs_lock);

done:
	if (status)
		pr_debug("%s: gpio%d status %d\n", __func__, gpio, status);

	return status;
}
EXPORT_SYMBOL_GPL(gpio_export_link);


/**
 * gpio_sysfs_set_active_low - set the polarity of gpio sysfs value
 * @gpio: gpio to change
 * @value: non-zero to use active low, i.e. inverted values
 *
 * Set the polarity of /sys/class/gpio/gpioN/value sysfs attribute.
 * The GPIO does not have to be exported yet.  If poll(2) support has
 * been enabled for either rising or falling edge, it will be
 * reconfigured to follow the new polarity.
 *
 * Returns zero on success, else an error.
 */
int gpio_sysfs_set_active_low(unsigned gpio, int value)
{
	struct gpio_desc	*desc;
	struct device		*dev = NULL;
	int			status = -EINVAL;

	if (!gpio_is_valid(gpio))
		goto done;

	mutex_lock(&sysfs_lock);

	desc = &gpio_desc[gpio];

	if (test_bit(FLAG_EXPORT, &desc->flags)) {
		dev = class_find_device(&gpio_class, NULL, desc, match_export);
		if (dev == NULL) {
			status = -ENODEV;
			goto unlock;
		}
	}

	status = sysfs_set_active_low(desc, dev, value);

unlock:
	mutex_unlock(&sysfs_lock);

done:
	if (status)
		pr_debug("%s: gpio%d status %d\n", __func__, gpio, status);

	return status;
}
EXPORT_SYMBOL_GPL(gpio_sysfs_set_active_low);

/**
 * gpio_unexport - reverse effect of gpio_export()
 * @gpio: gpio to make unavailable
 *
 * This is implicit on gpio_free().
 */
void gpio_unexport(unsigned gpio)
{
	struct gpio_desc	*desc;
	int			status = 0;
	struct device		*dev = NULL;

	if (!gpio_is_valid(gpio)) {
		status = -EINVAL;
		goto done;
	}

	mutex_lock(&sysfs_lock);

	desc = &gpio_desc[gpio];

	if (test_bit(FLAG_EXPORT, &desc->flags)) {

		dev = class_find_device(&gpio_class, NULL, desc, match_export);
		if (dev) {
			gpio_setup_irq(desc, dev, 0);
			clear_bit(FLAG_EXPORT, &desc->flags);
		} else
			status = -ENODEV;
	}

	mutex_unlock(&sysfs_lock);
	if (dev) {
		device_unregister(dev);
		put_device(dev);
	}
done:
	if (status)
		pr_debug("%s: gpio%d status %d\n", __func__, gpio, status);
}
EXPORT_SYMBOL_GPL(gpio_unexport);

static int gpiochip_export(struct gpio_chip *chip)
{
	int		status;
	struct device	*dev;

	/* Many systems register gpio chips for SOC support very early,
	 * before driver model support is available.  In those cases we
	 * export this later, in gpiolib_sysfs_init() ... here we just
	 * verify that _some_ field of gpio_class got initialized.
	 */
	if (!gpio_class.p)
		return 0;

	/* use chip->base for the ID; it's already known to be unique */
	mutex_lock(&sysfs_lock);
	dev = device_create(&gpio_class, chip->dev, MKDEV(0, 0), chip,
				"gpiochip%d", chip->base);
	if (!IS_ERR(dev)) {
		status = sysfs_create_group(&dev->kobj,
				&gpiochip_attr_group);
	} else
		status = PTR_ERR(dev);
	chip->exported = (status == 0);
	mutex_unlock(&sysfs_lock);

	if (status) {
		unsigned long	flags;
		unsigned	gpio;

		spin_lock_irqsave(&gpio_lock, flags);
		gpio = chip->base;
		while (gpio_desc[gpio].chip == chip)
			gpio_desc[gpio++].chip = NULL;
		spin_unlock_irqrestore(&gpio_lock, flags);

		pr_debug("%s: chip %s status %d\n", __func__,
				chip->label, status);
	}

	return status;
}

static void gpiochip_unexport(struct gpio_chip *chip)
{
	int			status;
	struct device		*dev;

	mutex_lock(&sysfs_lock);
	dev = class_find_device(&gpio_class, NULL, chip, match_export);
	if (dev) {
		put_device(dev);
		device_unregister(dev);
		chip->exported = 0;
		status = 0;
	} else
		status = -ENODEV;
	mutex_unlock(&sysfs_lock);

	if (status)
		pr_debug("%s: chip %s status %d\n", __func__,
				chip->label, status);
}

static int __init gpiolib_sysfs_init(void)
{
	int		status;
	unsigned long	flags;
	unsigned	gpio;

	status = class_register(&gpio_class);
	if (status < 0)
		return status;

	/* Scan and register the gpio_chips which registered very
	 * early (e.g. before the class_register above was called).
	 *
	 * We run before arch_initcall() so chip->dev nodes can have
	 * registered, and so arch_initcall() can always gpio_export().
	 */
	spin_lock_irqsave(&gpio_lock, flags);
	for (gpio = 0; gpio < ARCH_NR_GPIOS; gpio++) {
		struct gpio_chip	*chip;

		chip = gpio_desc[gpio].chip;
		if (!chip || chip->exported)
			continue;

		spin_unlock_irqrestore(&gpio_lock, flags);
		status = gpiochip_export(chip);
		spin_lock_irqsave(&gpio_lock, flags);
	}
	spin_unlock_irqrestore(&gpio_lock, flags);


	return status;
}
postcore_initcall(gpiolib_sysfs_init);

#else
static inline int gpiochip_export(struct gpio_chip *chip)
{
	return 0;
}

static inline void gpiochip_unexport(struct gpio_chip *chip)
{
}

#endif /* CONFIG_GPIO_SYSFS */

/**
 * gpiochip_add() - register a gpio_chip
 * @chip: the chip to register, with chip->base initialized
 * Context: potentially before irqs or kmalloc will work
 *
 * Returns a negative errno if the chip can't be registered, such as
 * because the chip->base is invalid or already associated with a
 * different chip.  Otherwise it returns zero as a success code.
 *
 * When gpiochip_add() is called very early during boot, so that GPIOs
 * can be freely used, the chip->dev device must be registered before
 * the gpio framework's arch_initcall().  Otherwise sysfs initialization
 * for GPIOs will fail rudely.
 *
 * If chip->base is negative, this requests dynamic assignment of
 * a range of valid GPIOs.
 */
int gpiochip_add(struct gpio_chip *chip)
{
	unsigned long	flags;
	int		status = 0;
	unsigned	id;
	int		base = chip->base;

    /* 判断给定的 chip 中GPIO号是否合法
     * 需注意，base < 0 是合法的， 小于0表示由 gpiolib 动态分配 */
	if ((!gpio_is_valid(base) || !gpio_is_valid(base + chip->ngpio - 1))
			&& base >= 0) {
		status = -EINVAL;
		goto fail;
	}

    /* gpiolib 中的全局 spinlock  */
	spin_lock_irqsave(&gpio_lock, flags);

    /* base 小于0的情况，主要用于动态分配GPIO号，比如热插拔设备
     * 这里并未使用到 */
	if (base < 0) {
		base = gpiochip_find_base(chip->ngpio);
		if (base < 0) {
			status = base;
			goto unlock;
		}
        /* 将查找到的合理的首GPIO号给新注册的chip */
		chip->base = base;
	}

    /* 遍历已注册的 gpio_desc ，确保新注册的gpio_chip与之前的无冲突 */
	/* these GPIO numbers must not be managed by another gpio_chip */
	for (id = base; id < base + chip->ngpio; id++) {
		if (gpio_desc[id].chip != NULL) {
			status = -EBUSY;
			break;
		}
	}
	if (status == 0) {
        /* 将新注册的gpio_chip添加到全局 gpio_desc 中，
         * 从这里可以看出， 每个 gpio_desc 对应一个gpio端口
         * 一组 gpio 对应一个 gpio_chip ，具有相同的操作函数 */
		for (id = base; id < base + chip->ngpio; id++) {
			gpio_desc[id].chip = chip;

            /* 标记本组gpio， 如果没有设置 direction_input 回调，
             * 则认为改组gpio只有输出功能 */
			/* REVISIT:  most hardware initializes GPIOs as
			 * inputs (often with pullups enabled) so power
			 * usage is minimized.  Linux code should set the
			 * gpio direction first thing; but until it does,
			 * we may expose the wrong direction in sysfs.
			 */
			gpio_desc[id].flags = !chip->direction_input
				? (1 << FLAG_IS_OUT)
				: 0;
		}
	}

    /* 不支持设备树，无作用 */
	of_gpiochip_add(chip);

unlock:
	spin_unlock_irqrestore(&gpio_lock, flags);

	if (status)
		goto fail;

    /* sysfs 中导出gpio，该内核中未配置 sysfs */
	status = gpiochip_export(chip);
	if (status)
		goto fail;

	pr_debug("gpiochip_add: registered GPIOs %d to %d on device: %s\n",
		chip->base, chip->base + chip->ngpio - 1,
		chip->label ? : "generic");

	return 0;
fail:
	/* failures here can mean systems won't boot... */
	pr_err("gpiochip_add: gpios %d..%d (%s) failed to register\n",
		chip->base, chip->base + chip->ngpio - 1,
		chip->label ? : "generic");
	return status;
}
EXPORT_SYMBOL_GPL(gpiochip_add);

/* 卸载一个 gpio_chip
 * 该组 gpio_chip 中有任何 gpio 在使用时，则不会被移除 */
/**
 * gpiochip_remove() - unregister a gpio_chip
 * @chip: the chip to unregister
 *
 * A gpio_chip with any GPIOs still requested may not be removed.
 */
int gpiochip_remove(struct gpio_chip *chip)
{
	unsigned long	flags;
	int		status = 0;
	unsigned	id;

	spin_lock_irqsave(&gpio_lock, flags);

    /* 设备树相关，未使用 */
	of_gpiochip_remove(chip);

    /* 遍历需要卸载的 gpio_chip 对应的 gpio_desc 数组中的标志
     * 如果所需要卸载的gpio中仍有被使用的，则不能被卸载 */
	for (id = chip->base; id < chip->base + chip->ngpio; id++) {
		if (test_bit(FLAG_REQUESTED, &gpio_desc[id].flags)) {
			status = -EBUSY;
			break;
		}
	}
    /* 需卸载的 gpio_chip 对应的所有GPIO都未被使用，则逐个释放掉 */
	if (status == 0) {
		for (id = chip->base; id < chip->base + chip->ngpio; id++)
			gpio_desc[id].chip = NULL;
	}

	spin_unlock_irqrestore(&gpio_lock, flags);

    /* sysfs 相关，暂不分析 */
	if (status == 0)
		gpiochip_unexport(chip);

	return status;
}
EXPORT_SYMBOL_GPL(gpiochip_remove);

/**
 * gpiochip_find() - iterator for locating a specific gpio_chip
 * @data: data to pass to match function
 * @callback: Callback function to check gpio_chip
 *
 * Similar to bus_find_device.  It returns a reference to a gpio_chip as
 * determined by a user supplied @match callback.  The callback should return
 * 0 if the device doesn't match and non-zero if it does.  If the callback is
 * non-zero, this function will return to the caller and not iterate over any
 * more gpio_chips.
 */
/* 特定的查找 gpio_chip 
 * 提供一个匹配数据 data ，一个检查回调函数 match 
 * 本函数中将遍历所有的 gpio_chip 结构，并做为参数调用 match 
 * 如果 match 返回 非0， 则中止遍历，并将 gpio_chip 返回 */
struct gpio_chip *gpiochip_find(void *data,
				int (*match)(struct gpio_chip *chip,
					     void *data))
{
	struct gpio_chip *chip = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&gpio_lock, flags);
	for (i = 0; i < ARCH_NR_GPIOS; i++) {
        /* 仅遍历所有可用 gpio_chip */
		if (!gpio_desc[i].chip)
			continue;

        /* 调用提供的回调函数，如果返回非0，则中止遍历 */
		if (match(gpio_desc[i].chip, data)) {
			chip = gpio_desc[i].chip;
			break;
		}
	}
	spin_unlock_irqrestore(&gpio_lock, flags);

	return chip;
}
EXPORT_SYMBOL_GPL(gpiochip_find);

/* These "optional" allocation calls help prevent drivers from stomping
 * on each other, and help provide better diagnostics in debugfs.
 * They're called even less than the "set direction" calls.
 */
/* request 一个gpio */
int gpio_request(unsigned gpio, const char *label)
{
	struct gpio_desc	*desc;
	struct gpio_chip	*chip;
	int			status = -EINVAL;
	unsigned long		flags;

	spin_lock_irqsave(&gpio_lock, flags);

	if (!gpio_is_valid(gpio))
		goto done;
	desc = &gpio_desc[gpio];
	chip = desc->chip;
	if (chip == NULL)
		goto done;

	if (!try_module_get(chip->owner))
		goto done;

	/* NOTE:  gpio_request() can be called in early boot,
	 * before IRQs are enabled, for non-sleeping (SOC) GPIOs.
	 */

    /* 测试是否已被 request ，成功则设置 该标志 并设置 label */
	if (test_and_set_bit(FLAG_REQUESTED, &desc->flags) == 0) {
		desc_set_label(desc, label ? : "?");
		status = 0;
	} else {
		status = -EBUSY;
		module_put(chip->owner);
		goto done;
	}

    /* 如果 GPIO chip 中注册了 request 回调，则需要调用 
     * 在4412 的代码中，所有的 chip 都没有注册 request */
	if (chip->request) {
		/* chip->request may sleep */
		spin_unlock_irqrestore(&gpio_lock, flags);
		status = chip->request(chip, gpio - chip->base);
		spin_lock_irqsave(&gpio_lock, flags);

        /* request 回调失败，则恢复 label 和 request 标志 */
		if (status < 0) {
			desc_set_label(desc, NULL);
			module_put(chip->owner);
			clear_bit(FLAG_REQUESTED, &desc->flags);
		}
	}

done:
	if (status)
		pr_debug("gpio_request: gpio-%d (%s) status %d\n",
			gpio, label ? : "?", status);
	spin_unlock_irqrestore(&gpio_lock, flags);
	return status;
}
EXPORT_SYMBOL_GPL(gpio_request);

/* 释放 gpio */
void gpio_free(unsigned gpio)
{
	unsigned long		flags;
	struct gpio_desc	*desc;
	struct gpio_chip	*chip;

	might_sleep();

	if (!gpio_is_valid(gpio)) {
		WARN_ON(extra_checks);
		return;
	}

	gpio_unexport(gpio);

	spin_lock_irqsave(&gpio_lock, flags);

    /* 检查制定的 gpio 对应的 chip ，确认是 request 的
     * 如果该 chip 有 free 回调，则需要先调用 */
	desc = &gpio_desc[gpio];
	chip = desc->chip;
	if (chip && test_bit(FLAG_REQUESTED, &desc->flags)) {
		if (chip->free) {
			spin_unlock_irqrestore(&gpio_lock, flags);
			might_sleep_if(chip->can_sleep);
			chip->free(chip, gpio - chip->base);
			spin_lock_irqsave(&gpio_lock, flags);
		}
        /* 清空 label ，并清除 gpio 对用的 desc 的标志 */
		desc_set_label(desc, NULL);
		module_put(desc->chip->owner);
		clear_bit(FLAG_ACTIVE_LOW, &desc->flags);
		clear_bit(FLAG_REQUESTED, &desc->flags);
		clear_bit(FLAG_OPEN_DRAIN, &desc->flags);
		clear_bit(FLAG_OPEN_SOURCE, &desc->flags);
	} else
		WARN_ON(extra_checks);

	spin_unlock_irqrestore(&gpio_lock, flags);
}
EXPORT_SYMBOL_GPL(gpio_free);

/**
 * gpio_request_one - request a single GPIO with initial configuration
 * @gpio:	the GPIO number
 * @flags:	GPIO configuration as specified by GPIOF_*
 * @label:	a literal description string of this GPIO
 */
/* request 一个gpio，带有初始化配置选项 如：GPIOF_DIR_IN  */
int gpio_request_one(unsigned gpio, unsigned long flags, const char *label)
{
	int err;

	err = gpio_request(gpio, label);
	if (err)
		return err;

	if (flags & GPIOF_OPEN_DRAIN)
		set_bit(FLAG_OPEN_DRAIN, &gpio_desc[gpio].flags);

	if (flags & GPIOF_OPEN_SOURCE)
		set_bit(FLAG_OPEN_SOURCE, &gpio_desc[gpio].flags);

	if (flags & GPIOF_DIR_IN)
		err = gpio_direction_input(gpio);
	else
		err = gpio_direction_output(gpio,
				(flags & GPIOF_INIT_HIGH) ? 1 : 0);

	if (err)
		goto free_gpio;

	if (flags & GPIOF_EXPORT) {
		err = gpio_export(gpio, flags & GPIOF_EXPORT_CHANGEABLE);
		if (err)
			goto free_gpio;
	}

	return 0;

 free_gpio:
	gpio_free(gpio);
	return err;
}
EXPORT_SYMBOL_GPL(gpio_request_one);

/**
 * gpio_request_array - request multiple GPIOs in a single call
 * @array:	array of the 'struct gpio'
 * @num:	how many GPIOs in the array
 */
/* request 一组 gpio ，由 struct gpio 指定 gpio 初始化参数 */
int gpio_request_array(const struct gpio *array, size_t num)
{
	int i, err;

	for (i = 0; i < num; i++, array++) {
		err = gpio_request_one(array->gpio, array->flags, array->label);
		if (err)
			goto err_free;
	}
	return 0;

err_free:
	while (i--)
		gpio_free((--array)->gpio);
	return err;
}
EXPORT_SYMBOL_GPL(gpio_request_array);

/**
 * gpio_free_array - release multiple GPIOs in a single call
 * @array:	array of the 'struct gpio'
 * @num:	how many GPIOs in the array
 */
/* 释放一组 gpio */
void gpio_free_array(const struct gpio *array, size_t num)
{
	while (num--)
		gpio_free((array++)->gpio);
}
EXPORT_SYMBOL_GPL(gpio_free_array);

/**
 * gpiochip_is_requested - return string iff signal was requested
 * @chip: controller managing the signal
 * @offset: of signal within controller's 0..(ngpio - 1) range
 *
 * Returns NULL if the GPIO is not currently requested, else a string.
 * If debugfs support is enabled, the string returned is the label passed
 * to gpio_request(); otherwise it is a meaningless constant.
 *
 * This function is for use by GPIO controller drivers.  The label can
 * help with diagnostics, and knowing that the signal is used as a GPIO
 * can help avoid accidentally multiplexing it to another controller.
 */
/* 判断 gpio_chip 中的 某个 gpio 是否被 request 
 * 如果正确 request ，该函数返回 label 或 固定的 ? 
 * 该函数不清楚有什么作用，传入的参数是 gpio_chip, 使用时还需要先
 * 查找 gpio_chip, 很是奇怪 */
const char *gpiochip_is_requested(struct gpio_chip *chip, unsigned offset)
{
	unsigned gpio = chip->base + offset;

	if (!gpio_is_valid(gpio) || gpio_desc[gpio].chip != chip)
		return NULL;
	if (test_bit(FLAG_REQUESTED, &gpio_desc[gpio].flags) == 0)
		return NULL;
#ifdef CONFIG_DEBUG_FS
	return gpio_desc[gpio].label;
#else
	return "?";
#endif
}
EXPORT_SYMBOL_GPL(gpiochip_is_requested);


/* Drivers MUST set GPIO direction before making get/set calls.  In
 * some cases this is done in early boot, before IRQs are enabled.
 *
 * As a rule these aren't called more than once (except for drivers
 * using the open-drain emulation idiom) so these are natural places
 * to accumulate extra debugging checks.  Note that we can't (yet)
 * rely on gpio_request() having been called beforehand.
 */
/* 设置指定 gpio 为 输入模式 */
int gpio_direction_input(unsigned gpio)
{
	unsigned long		flags;
	struct gpio_chip	*chip;
	struct gpio_desc	*desc = &gpio_desc[gpio];
	int			status = -EINVAL;

	spin_lock_irqsave(&gpio_lock, flags);

    /* 确认 gpio 编号有效
     * 该 gpio 有对应的 gpio_chip, 输入模式一定要有 get 回调 和 配置输入模式回调
     * */
	if (!gpio_is_valid(gpio))
		goto fail;
	chip = desc->chip;
	if (!chip || !chip->get || !chip->direction_input)
		goto fail;
    /* 计算 gpio 编号在该 gpio_chip 中的偏移，
     * 后面的操作都是以偏移操作的 */
	gpio -= chip->base;
	if (gpio >= chip->ngpio)
		goto fail;
    /* 确保 gpio 是 request 的，如果没有，则自动 request */
	status = gpio_ensure_requested(desc, gpio);
	if (status < 0)
		goto fail;

	/* now we know the gpio is valid and chip won't vanish */

	spin_unlock_irqrestore(&gpio_lock, flags);

	might_sleep_if(chip->can_sleep);

    /* 该操作与 gpio_ensure_requested 返回值有关
     * 如果 gpio_ensure_requested 返回 1 ，则表示有 request 回调
     * 这里就会调用，否则会直接跳过 */
	if (status) {
		status = chip->request(chip, gpio);
		if (status < 0) {
			pr_debug("GPIO-%d: chip request fail, %d\n",
				chip->base + gpio, status);
			/* and it's not available to anyone else ...
			 * gpio_request() is the fully clean solution.
			 */
			goto lose;
		}
	}

    /* 调用 direction_input 回调设置为输入模式
     * 该回调是在前面初始化 gpio_chip 是注册的，底层操作函数
     * 成功则清除 FLAG_IS_OUT 标志，标识该 gpio 为输入模式 */
	status = chip->direction_input(chip, gpio);
	if (status == 0)
		clear_bit(FLAG_IS_OUT, &desc->flags);

	trace_gpio_direction(chip->base + gpio, 1, status);
lose:
	return status;
fail:
	spin_unlock_irqrestore(&gpio_lock, flags);
	if (status)
		pr_debug("%s: gpio-%d status %d\n",
			__func__, gpio, status);
	return status;
}
EXPORT_SYMBOL_GPL(gpio_direction_input);

/* 设置指定 gpio 为 输出模式, 并设置初始输出值 */
int gpio_direction_output(unsigned gpio, int value)
{
	unsigned long		flags;
	struct gpio_chip	*chip;
	struct gpio_desc	*desc = &gpio_desc[gpio];
	int			status = -EINVAL;

	/* Open drain pin should not be driven to 1 */
	if (value && test_bit(FLAG_OPEN_DRAIN,  &desc->flags))
		return gpio_direction_input(gpio);

	/* Open source pin should not be driven to 0 */
	if (!value && test_bit(FLAG_OPEN_SOURCE,  &desc->flags))
		return gpio_direction_input(gpio);

	spin_lock_irqsave(&gpio_lock, flags);

    /* 判断有效性 */
	if (!gpio_is_valid(gpio))
		goto fail;
	chip = desc->chip;
	if (!chip || !chip->set || !chip->direction_output)
		goto fail;
	gpio -= chip->base;
	if (gpio >= chip->ngpio)
		goto fail;
    /* 确认已被 request */
	status = gpio_ensure_requested(desc, gpio);
	if (status < 0)
		goto fail;

	/* now we know the gpio is valid and chip won't vanish */

	spin_unlock_irqrestore(&gpio_lock, flags);

	might_sleep_if(chip->can_sleep);

    /* 调用 request 回调 */
	if (status) {
		status = chip->request(chip, gpio);
		if (status < 0) {
			pr_debug("GPIO-%d: chip request fail, %d\n",
				chip->base + gpio, status);
			/* and it's not available to anyone else ...
			 * gpio_request() is the fully clean solution.
			 */
			goto lose;
		}
	}

    /* 调用底层回调，设置为输出模式，并设置初值
     * 成功则设置该 gpio 为输出标志 */
	status = chip->direction_output(chip, gpio, value);
	if (status == 0)
		set_bit(FLAG_IS_OUT, &desc->flags);
	trace_gpio_value(chip->base + gpio, 0, value);
	trace_gpio_direction(chip->base + gpio, 0, status);
lose:
	return status;
fail:
	spin_unlock_irqrestore(&gpio_lock, flags);
	if (status)
		pr_debug("%s: gpio-%d status %d\n",
			__func__, gpio, status);
	return status;
}
EXPORT_SYMBOL_GPL(gpio_direction_output);

/**
 * gpio_set_debounce - sets @debounce time for a @gpio
 * @gpio: the gpio to set debounce time
 * @debounce: debounce time is microseconds
 */
/* 设置 gpio 消抖时间
 * 4412 中并未实现该函数
 * 该函数与其他函数过程基本一致，确认有效性，调用底层回调 */
int gpio_set_debounce(unsigned gpio, unsigned debounce)
{
	unsigned long		flags;
	struct gpio_chip	*chip;
	struct gpio_desc	*desc = &gpio_desc[gpio];
	int			status = -EINVAL;

	spin_lock_irqsave(&gpio_lock, flags);

	if (!gpio_is_valid(gpio))
		goto fail;
	chip = desc->chip;
	if (!chip || !chip->set || !chip->set_debounce)
		goto fail;
	gpio -= chip->base;
	if (gpio >= chip->ngpio)
		goto fail;
	status = gpio_ensure_requested(desc, gpio);
	if (status < 0)
		goto fail;

	/* now we know the gpio is valid and chip won't vanish */

	spin_unlock_irqrestore(&gpio_lock, flags);

	might_sleep_if(chip->can_sleep);

	return chip->set_debounce(chip, gpio, debounce);

fail:
	spin_unlock_irqrestore(&gpio_lock, flags);
	if (status)
		pr_debug("%s: gpio-%d status %d\n",
			__func__, gpio, status);

	return status;
}
EXPORT_SYMBOL_GPL(gpio_set_debounce);

/* I/O calls are only valid after configuration completed; the relevant
 * "is this a valid GPIO" error checks should already have been done.
 * I/O调用只有在配置完成后才有效；相关的“这是一个有效的GPIO”错误检查应该已经完成。
 *
 * "Get" operations are often inlinable as reading a pin value register,
 * and masking the relevant bit in that register.
 * “Get”操作通常可以内联为读取引脚值寄存器，并屏蔽该寄存器中的相关位。
 *
 * When "set" operations are inlinable, they involve writing that mask to
 * one register to set a low value, or a different register to set it high.
 * Otherwise locking is needed, so there may be little value to inlining.
 * 当“set”操作是可内联的时，它们涉及将掩码写入一个寄存器以设置低值，或写入另一个
 * 寄存器将其设置为高值。否则就需要锁定，所以内联可能没有什么价值。
 *
 *------------------------------------------------------------------------
 *
 * IMPORTANT!!!  The hot paths -- get/set value -- assume that callers
 * have requested the GPIO.  That can include implicit requesting by
 * a direction setting call.  Marking a gpio as requested locks its chip
 * in memory, guaranteeing that these table lookups need no more locking
 * and that gpiochip_remove() will fail.
 *
 * REVISIT when debugging, consider adding some instrumentation to ensure
 * that the GPIO was actually requested.
 */

/**
 * __gpio_get_value() - return a gpio's value
 * @gpio: gpio whose value will be returned
 * Context: any
 *
 * This is used directly or indirectly to implement gpio_get_value().
 * It returns the zero or nonzero value provided by the associated
 * gpio_chip.get() method; or zero if no such method is provided.
 */
/* 获取 gpio 端口状态值
 * 该接口中并未做有效性检查，
 * 所以调用时一定要确保参数有效性，并且该gpio是 request 的*/
int __gpio_get_value(unsigned gpio)
{
	struct gpio_chip	*chip;
	int value;

	chip = gpio_to_chip(gpio);
	/* Should be using gpio_get_value_cansleep() */
	WARN_ON(chip->can_sleep);
	value = chip->get ? chip->get(chip, gpio - chip->base) : 0;
	trace_gpio_value(gpio, 1, value);
	return value;
}
EXPORT_SYMBOL_GPL(__gpio_get_value);

/*
 *  _gpio_set_open_drain_value() - Set the open drain gpio's value.
 * @gpio: Gpio whose state need to be set.
 * @chip: Gpio chip.
 * @value: Non-zero for setting it HIGH otherise it will set to LOW.
 */
static void _gpio_set_open_drain_value(unsigned gpio,
			struct gpio_chip *chip, int value)
{
	int err = 0;
	if (value) {
		err = chip->direction_input(chip, gpio - chip->base);
		if (!err)
			clear_bit(FLAG_IS_OUT, &gpio_desc[gpio].flags);
	} else {
		err = chip->direction_output(chip, gpio - chip->base, 0);
		if (!err)
			set_bit(FLAG_IS_OUT, &gpio_desc[gpio].flags);
	}
	trace_gpio_direction(gpio, value, err);
	if (err < 0)
		pr_err("%s: Error in set_value for open drain gpio%d err %d\n",
					__func__, gpio, err);
}

/*
 *  _gpio_set_open_source() - Set the open source gpio's value.
 * @gpio: Gpio whose state need to be set.
 * @chip: Gpio chip.
 * @value: Non-zero for setting it HIGH otherise it will set to LOW.
 */
static void _gpio_set_open_source_value(unsigned gpio,
			struct gpio_chip *chip, int value)
{
	int err = 0;
	if (value) {
		err = chip->direction_output(chip, gpio - chip->base, 1);
		if (!err)
			set_bit(FLAG_IS_OUT, &gpio_desc[gpio].flags);
	} else {
		err = chip->direction_input(chip, gpio - chip->base);
		if (!err)
			clear_bit(FLAG_IS_OUT, &gpio_desc[gpio].flags);
	}
	trace_gpio_direction(gpio, !value, err);
	if (err < 0)
		pr_err("%s: Error in set_value for open source gpio%d err %d\n",
					__func__, gpio, err);
}


/**
 * __gpio_set_value() - assign a gpio's value
 * @gpio: gpio whose value will be assigned
 * @value: value to assign
 * Context: any
 *
 * This is used directly or indirectly to implement gpio_set_value().
 * It invokes the associated gpio_chip.set() method.
 */
/* 设置 gpio 端口输出值
 * 该接口中并未做有效性检查
 * 所以调用时一定要确保参数有效性，并且该gpio是 request 的 */
void __gpio_set_value(unsigned gpio, int value)
{
	struct gpio_chip	*chip;

	chip = gpio_to_chip(gpio);
	/* Should be using gpio_set_value_cansleep() */
	WARN_ON(chip->can_sleep);
	trace_gpio_value(gpio, 0, value);
	if (test_bit(FLAG_OPEN_DRAIN,  &gpio_desc[gpio].flags))
		_gpio_set_open_drain_value(gpio, chip, value);
	else if (test_bit(FLAG_OPEN_SOURCE,  &gpio_desc[gpio].flags))
		_gpio_set_open_source_value(gpio, chip, value);
	else
		chip->set(chip, gpio - chip->base, value);
}
EXPORT_SYMBOL_GPL(__gpio_set_value);

/**
 * __gpio_cansleep() - report whether gpio value access will sleep
 * @gpio: gpio in question
 * Context: any
 *
 * This is used directly or indirectly to implement gpio_cansleep().  It
 * returns nonzero if access reading or writing the GPIO value can sleep.
 */
/* 检查 gpio 访问时是否会休眠
 * 检查 can_sleep 标志，该标志在初始化的时候一定要正确设置 */
int __gpio_cansleep(unsigned gpio)
{
	struct gpio_chip	*chip;

	/* only call this on GPIOs that are valid! */
	chip = gpio_to_chip(gpio);

	return chip->can_sleep;
}
EXPORT_SYMBOL_GPL(__gpio_cansleep);

/**
 * __gpio_to_irq() - return the IRQ corresponding to a GPIO
 * @gpio: gpio whose IRQ will be returned (already requested)
 * Context: any
 *
 * This is used directly or indirectly to implement gpio_to_irq().
 * It returns the number of the IRQ signaled by this (input) GPIO,
 * or a negative errno.
 */
/* gpio 转换为 irq 号
 * 不是所有的 gpio都具有 irq功能的 
 * 这里只有在有 to_irq 回调时才会正确返回，
 * 无该回调，则返回 -ENXIO 错误 */
int __gpio_to_irq(unsigned gpio)
{
	struct gpio_chip	*chip;

	chip = gpio_to_chip(gpio);
	return chip->to_irq ? chip->to_irq(chip, gpio - chip->base) : -ENXIO;
}
EXPORT_SYMBOL_GPL(__gpio_to_irq);



/* There's no value in making it easy to inline GPIO calls that may sleep.
 * Common examples include ones connected to I2C or SPI chips.
 */
/* 可休眠的 读 gpio 状态函数 */
int gpio_get_value_cansleep(unsigned gpio)
{
	struct gpio_chip	*chip;
	int value;

	might_sleep_if(extra_checks);
	chip = gpio_to_chip(gpio);
	value = chip->get ? chip->get(chip, gpio - chip->base) : 0;
	trace_gpio_value(gpio, 1, value);
	return value;
}
EXPORT_SYMBOL_GPL(gpio_get_value_cansleep);

/* 可休眠的 设置 gpio 输出状态函数 */
void gpio_set_value_cansleep(unsigned gpio, int value)
{
	struct gpio_chip	*chip;

	might_sleep_if(extra_checks);
	chip = gpio_to_chip(gpio);
	trace_gpio_value(gpio, 0, value);
	if (test_bit(FLAG_OPEN_DRAIN,  &gpio_desc[gpio].flags))
		_gpio_set_open_drain_value(gpio, chip, value);
	else if (test_bit(FLAG_OPEN_SOURCE,  &gpio_desc[gpio].flags))
		_gpio_set_open_source_value(gpio, chip, value);
	else
		chip->set(chip, gpio - chip->base, value);
}
EXPORT_SYMBOL_GPL(gpio_set_value_cansleep);


/* debugfs 调试支持 */
#ifdef CONFIG_DEBUG_FS

static void gpiolib_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
	unsigned		i;
	unsigned		gpio = chip->base;
	struct gpio_desc	*gdesc = &gpio_desc[gpio];
	int			is_out;

    /* 遍历某 gpio_chip 下所有的 gpio 
     * 只有被 request 的 gpio 才会输出信息 */
	for (i = 0; i < chip->ngpio; i++, gpio++, gdesc++) {
		if (!test_bit(FLAG_REQUESTED, &gdesc->flags))
			continue;

        /* 输出配置信息， gpio编号， label， 输入/输出配置， IO端口值 */
		is_out = test_bit(FLAG_IS_OUT, &gdesc->flags);
		seq_printf(s, " gpio-%-3d (%-20.20s) %s %s",
			gpio, gdesc->label,
			is_out ? "out" : "in ",
			chip->get
				? (chip->get(chip, i) ? "hi" : "lo")
				: "?  ");
		seq_printf(s, "\n");
	}
}

static int gpiolib_show(struct seq_file *s, void *unused)
{
	struct gpio_chip	*chip = NULL;
	unsigned		gpio;
	int			started = 0;

	/* REVISIT this isn't locked against gpio_chip removal ... */

    /* 遍历所有的 gpio  */
	for (gpio = 0; gpio_is_valid(gpio); gpio++) {
		struct device *dev;

        /* 连续相同 gpio_chip 的仅遍历一次，这些gpio同属于一个控制器
         * 打印的信息是按照控制器分组的 */
		if (chip == gpio_desc[gpio].chip)
			continue;
		chip = gpio_desc[gpio].chip;
		if (!chip)
			continue;

        /* 输出控制器基本信息，起始 gpio号，数量，设备信息，label */
		seq_printf(s, "%sGPIOs %d-%d",
				started ? "\n" : "",
				chip->base, chip->base + chip->ngpio - 1);
		dev = chip->dev;
		if (dev)
			seq_printf(s, ", %s/%s",
				dev->bus ? dev->bus->name : "no-bus",
				dev_name(dev));
		if (chip->label)
			seq_printf(s, ", %s", chip->label);
		if (chip->can_sleep)
			seq_printf(s, ", can sleep");
		seq_printf(s, ":\n");

		started = 1;
		if (chip->dbg_show)
			chip->dbg_show(s, chip);
		else
			gpiolib_dbg_show(s, chip);
	}
	return 0;
}

static int gpiolib_open(struct inode *inode, struct file *file)
{
	return single_open(file, gpiolib_show, NULL);
}

static const struct file_operations gpiolib_operations = {
	.open		= gpiolib_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init gpiolib_debugfs_init(void)
{
	/* /sys/kernel/debug/gpio */
	(void) debugfs_create_file("gpio", S_IFREG | S_IRUGO,
				NULL, NULL, &gpiolib_operations);
	return 0;
}
subsys_initcall(gpiolib_debugfs_init);

#endif	/* DEBUG_FS */
