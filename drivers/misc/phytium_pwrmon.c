// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Phytium SoC Power Monitor Driver
 *
 * This driver provides a simple interface to read power monitor
 * data from Phytium SoC.
 *
 *  Copyright (c) 2021-2025 Phytium Technology Co., Ltd.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/version.h>

#undef pr_fmt
#define pr_fmt(fmt) "phytium_pwrmon: " fmt

#define ACPI_METHOD_PKG "PGET"
#define DEFAULT_BUFFER_SIZE (256)

static struct proc_dir_entry *pwrmon_dir;
static struct proc_dir_entry *proc_entry;

struct buffer_handler {
	char	*buf;
	size_t	sz;
	ssize_t	len;
};

struct pwrmon_drv_data {
	struct platform_device *pdev;
	struct kmem_cache *bh_cache;
};

/**
 * get_buffer_handler - Get a buffer handler from kmem_cache
 * @drv_data: Pointer to pwrmon_drv_data structure
 *
 * This function allocates a buffer handler from kmem_cache and initializes it.
 * The buffer handler contains a buffer, and the size of the buffer is set to
 * DEFAULT_BUFFER_SIZE. The buffer is allocated using kmalloc.
 *
 * Return: Pointer to buffer handler or NULL on failure
 */
static inline struct buffer_handler *get_buffer_handler(struct pwrmon_drv_data *drv_data)
{
	struct buffer_handler *bh;

	if (!drv_data || !drv_data->bh_cache)
		return NULL;

	bh = kmem_cache_zalloc(drv_data->bh_cache, GFP_KERNEL);
	if (bh) {
		bh->buf = kmalloc(DEFAULT_BUFFER_SIZE, GFP_KERNEL);
		if (bh->buf) {
			bh->sz = DEFAULT_BUFFER_SIZE;
			bh->len = 0;
		} else {
			kmem_cache_free(drv_data->bh_cache, bh);
			bh = NULL;
		}
	}
	return bh;
}

/**
 * put_buffer_handler - Put a buffer handler back to kmem_cache
 * @drv_data: Pointer to pwrmon_drv_data structure
 * @bh: Pointer to buffer handler
 *
 * This function puts a buffer handler back to kmem_cache. It first checks if
 * the parameters are valid, and then frees the buffer in the buffer handler
 * using kfree, and finally frees the buffer handler itself using
 * kmem_cache_free.
 *
 * Return: None
 */
static inline void put_buffer_handler(struct pwrmon_drv_data *drv_data, struct buffer_handler *bh)
{
	if (!bh || !drv_data || !drv_data->bh_cache)
		return;

	kfree(bh->buf);
	kmem_cache_free(drv_data->bh_cache, bh);
}

/**
 * free_buffer_handler - Free a kmem_cache used to allocate buffer handler
 * @drv_data: Pointer to pwrmon_drv_data structure
 *
 * This function frees a kmem_cache used to allocate buffer handler, and then
 * sets the pointer of the cache to NULL. It first checks if the parameters are
 * valid, i.e., if the pointer to pwrmon_drv_data structure is not NULL and if
 * the pointer to the cache is not NULL.
 *
 * Return: None
 */
static inline void free_buffer_handler(struct pwrmon_drv_data *drv_data)
{
	if (!drv_data || !drv_data->bh_cache)
		return;

	kmem_cache_destroy(drv_data->bh_cache);
	drv_data->bh_cache = NULL;
}

/**
 * shrink_buffer_handler - Shrink the buffer handler cache
 * @drv_data: Pointer to pwrmon_drv_data structure
 *
 * This function shrinks the buffer handler cache to release unused memory.
 * It checks if the input parameters are valid before proceeding with the
 * shrink operation. If either the driver data or the buffer handler cache
 * pointer is NULL, the function returns immediately without performing any
 * operation.
 *
 * Return: None
 */
static inline void shrink_buffer_handler(struct pwrmon_drv_data *drv_data)
{
	if (!drv_data || !drv_data->bh_cache)
		return;

	kmem_cache_shrink(drv_data->bh_cache);
}

/**
 * init_buffer_handler - Initialize a buffer handler cache
 * @drv_data: Pointer to pwrmon_drv_data structure
 *
 * This function creates a buffer handler cache to store buffer handlers.
 * The cache is created with the name "buffer_handler" and the size of a
 * buffer handler structure. The cache is created with the SLAB_HWCACHE_ALIGN
 * flag to ensure that the cache is aligned to the hardware cache size.
 * The cache is created with the NULL constructor and the NULL destructor.
 *
 * Return: 0 if the cache is created successfully, -ENOMEM if the cache
 * cannot be created.
 */
static inline int init_buffer_handler(struct pwrmon_drv_data *drv_data)
{
	if (!drv_data)
		return -EINVAL;

	drv_data->bh_cache = kmem_cache_create("buffer_handler",
						sizeof(struct buffer_handler), 0,
						SLAB_HWCACHE_ALIGN, NULL);

	return drv_data->bh_cache ? 0 : -ENOMEM;
}

/**
 * enlarge_buffer - Enlarge the buffer size of a buffer handler
 * @bh: Pointer to buffer_handler structure
 *
 * This function doubles the size of the buffer in the given buffer handler
 * structure. If the buffer handler structure does not have a buffer allocated,
 * it allocates a new buffer with the default size.
 *
 * Return: 0 if the buffer is enlarged successfully, -ENOMEM if memory cannot
 * be allocated.
 */
static int enlarge_buffer(struct buffer_handler *bh)
{
	size_t next_sz;
	char *new_buf;

	if (!bh || !bh->buf)
		return -EFAULT;

	next_sz = bh->sz ? bh->sz * 2 : DEFAULT_BUFFER_SIZE;
	new_buf = krealloc(bh->buf, next_sz, GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;

	bh->buf = new_buf;
	bh->sz = next_sz;
	return 0;
}

static inline struct platform_device *get_pdev(struct inode *inode)
{
	return pde_data(inode);
}

static inline struct pwrmon_drv_data *get_drv_data(struct inode *node)
{
	struct platform_device *pdev = get_pdev(node);

	return pdev ? platform_get_drvdata(pdev) : NULL;
}

/**
 * prepare_pwrmon_data_acpi - Prepare power monitor data in ACPI mode.
 * @pdev: The platform device associated with the power monitor.
 * @bh: The buffer handler structure to store the prepared data.
 *
 * This function evaluates the ACPI method _PGET to get the power monitor data,
 * and stores the data in the buffer handler structure.
 *
 * Return: 0 if the data is prepared successfully, -ENOMEM if memory cannot be
 * allocated, -EINVAL if the ACPI handle is invalid or the method evaluation
 * fails.
 */
static int prepare_pwrmon_data_acpi(struct platform_device *pdev, struct buffer_handler *bh)
{
	acpi_status status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *package;
	int ret = 0, i, j;
	size_t remaining;

	if (!ACPI_HANDLE(&pdev->dev)) {
		pr_err("Invalid ACPI handle\n");
		return -EINVAL;
	}

	status = acpi_evaluate_object(ACPI_HANDLE(&pdev->dev), ACPI_METHOD_PKG, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		pr_err("ACPI method %s evaluation failed: %d\n", ACPI_METHOD_PKG, status);
		return -EINVAL;
	}

	package = buffer.pointer;
	if (!package || package->type != ACPI_TYPE_PACKAGE) {
		pr_err("ACPI method %s did not return a valid package\n", ACPI_METHOD_PKG);
		bh->len = 0;
		ret = -EINVAL;
		goto free_buffer;
	}

	for (i = 0; i < package->package.count; i++) {
		union acpi_object *element = &package->package.elements[i];

		if (element->type != ACPI_TYPE_PACKAGE || element->package.count < 2) {
			pr_warn_ratelimited("Element %d has %d elements, expected at least 2\n",
						i, element->package.count);
			continue;
		}
		for (j = 0; j < element->package.count; j++) {
			if (unlikely(element->package.elements[j].type != ACPI_TYPE_INTEGER)) {
				pr_warn_ratelimited("Element [%d, %d] is not an integer\n", i, j);
				continue;
			} else {
				/*
				 * We first check if the buffer is going to overflow.
				 * If so, we enlarge it.
				 */
				remaining = bh->sz - bh->len;
				if (remaining < 23) {
					ret = enlarge_buffer(bh);
					if (ret < 0) {
						pr_err("Failed to enlarge buffer\n");
						ret = -ENOMEM;
						goto free_buffer;
					}
				}

				/*
				 * We do not check for overflow here, because we have already
				 * reserved enough space above.
				 */
				remaining = bh->sz - bh->len;
				ret = scnprintf(bh->buf + bh->len, remaining, "%lld%s%s",
						element->package.elements[j].integer.value,
						j == 0 ? ": " : " ",
						j == element->package.count - 1 ? "\n" : "");
				if (ret < 0) {
					pr_err("Write error\n");
					goto free_buffer;
				}

				bh->len += ret;
			}
		}
	}

	ret = 0;
free_buffer:
	kfree(buffer.pointer);
	return ret;
}


/**
 * prepare_pwrmon_data_of - Prepare power monitor data from Device Tree
 * @pdev: Pointer to platform device structure
 * @bh: Pointer to buffer handler structure
 *
 * This function is not implemented yet.
 *
 * Return: -EOPNOTSUPP if the method is not supported.
 */
static int prepare_pwrmon_data_of(struct platform_device *pdev, struct buffer_handler *bh)
{
	/* TODO: Implement Device Tree support by reading from regmap */
	return -EOPNOTSUPP;
}

/**
 * prepare_pwrmon_data - Prepare power monitor data in the corresponding format.
 * @inode: Pointer to inode structure associated with the power monitor device.
 * @file: Pointer to file structure associated with the power monitor device.
 *
 * This function prepares the power monitor data in the corresponding format
 * (ACPI or Device Tree) and stores it in the buffer handler structure.
 *
 * Return: 0 if the data is prepared successfully, -EINVAL if the platform device
 * or the buffer handler is invalid, -EOPNOTSUPP if the method is not supported.
 */
static int prepare_pwrmon_data(struct inode *inode, struct file *file)
{
	struct platform_device *pdev = get_pdev(inode);
	struct buffer_handler *bh = file->private_data;

	if (!pdev || !bh)
		return -EINVAL;

	if (ACPI_HANDLE(&pdev->dev))
		return prepare_pwrmon_data_acpi(pdev, bh);
	else if (pdev->dev.of_node)
		return prepare_pwrmon_data_of(pdev, bh);

	return -EOPNOTSUPP;
}

/**
 * pwrmon_pm_suspend - Suspend power monitor device
 * @dev: Pointer to the device structure
 *
 * This function is called during the suspend phase of the power management
 * process. It attempts to shrink the buffer handler associated with the
 * power monitor device to release unused memory. If the driver data is
 * not available, it returns an error. The function is intended to be
 * expanded with additional suspend operations, such as suspending I/O
 * mappings, if needed.
 *
 * Return: 0 on success, -EINVAL if driver data is not available.
 */

static int pwrmon_pm_suspend(struct device *dev)
{
	struct pwrmon_drv_data *drv_data = dev_get_drvdata(dev);

	if (!drv_data)
		return -EINVAL;

	/* Try to shrink the buffer handler */
	shrink_buffer_handler(drv_data);

	/* TODO: suspend some iomap */
	return 0;
}

/**
 * pwrmon_pm_resume - Resume power monitor device
 * @dev: Pointer to the device structure
 *
 * This function is called during the resume phase of the power management
 * process. It attempts to resume I/O mappings associated with the
 * power monitor device. If the driver data is not available, it returns
 * an error.
 *
 * Return: 0 on success, -EINVAL if driver data is not available.
 */
static int pwrmon_pm_resume(struct device *dev)
{
	struct pwrmon_drv_data *drv_data = dev_get_drvdata(dev);

	if (!drv_data)
		return -EINVAL;

	/* TODO: resume some iomap */
	return 0;
}

static const struct dev_pm_ops pwrmon_pm_ops = {
	.suspend = pwrmon_pm_suspend,
	.resume = pwrmon_pm_resume,
};

/**
 * pwrmon_proc_open - Open a power monitor device for reading
 * @inode: Pointer to the inode structure
 * @file: Pointer to the file structure
 *
 * This function is called when the /proc interface is opened for reading.
 * It tries to allocate a buffer handler and prepare the power monitor
 * data for the user space to read. If the driver data is not available,
 * it returns -EINVAL. If the buffer handler cannot be allocated, it
 * returns -ENOMEM. If the power monitor data preparation fails, it
 * returns -EFAULT.
 *
 * Return: 0 on success, -EINVAL if driver data is not available,
 * -ENOMEM if buffer handler allocation fails, -EFAULT if power monitor
 * data preparation fails.
 */
static int pwrmon_proc_open(struct inode *inode, struct file *file)
{
	struct pwrmon_drv_data *drv_data = get_drv_data(inode);
	struct buffer_handler *bh;

	if (!drv_data)
		return -EINVAL;

	bh = get_buffer_handler(drv_data);
	if (!bh)
		return -ENOMEM;

	file->private_data = bh;

	if (prepare_pwrmon_data(inode, file)) {
		file->private_data = NULL;
		put_buffer_handler(drv_data, bh);
		return -EFAULT;
	}

	return 0;
}

/**
 * pwrmon_proc_release - Release a power monitor device after reading
 * @inode: Pointer to the inode structure
 * @file: Pointer to the file structure
 *
 * This function is called when the /proc interface is closed after reading.
 * It releases the buffer handler associated with the power monitor device.
 *
 * Return: 0 on success, -EINVAL if driver data is not available.
 */
static int pwrmon_proc_release(struct inode *inode, struct file *file)
{
	put_buffer_handler(get_drv_data(inode), file->private_data);
	return 0;
}

/**
 * pwrmon_proc_read - Read a power monitor device through the /proc interface
 * @file: Pointer to the file structure
 * @ubuf: Pointer to the user-space buffer
 * @count: Size of the user-space buffer
 * @ppos: Pointer to the file offset
 *
 * This function is called when the /proc interface is read. It returns the
 * power monitor data stored in the buffer handler associated with the power
 * monitor device.
 *
 * Return: The number of bytes read, -EINVAL if no buffer handler is available.
 */
static ssize_t pwrmon_proc_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct buffer_handler *bh = file->private_data;

	return bh ? simple_read_from_buffer(ubuf, count, ppos, bh->buf, bh->len) : -EINVAL;
}

static const struct proc_ops pwrmon_ops = {
	.proc_open	=	pwrmon_proc_open,
	.proc_read	=	pwrmon_proc_read,
	.proc_release	=	pwrmon_proc_release,
};

/**
 * pwrmon_probe - Probe the power monitor device
 * @pdev: Pointer to the platform device structure
 *
 * This function is called when the power monitor device is probed. It allocates
 * the driver data structure, initializes the buffer handler, and sets up the
 * /proc interface.
 *
 * Return: 0 on success, error code on failure.
 */
static int pwrmon_probe(struct platform_device *pdev)
{
	struct pwrmon_drv_data *drv_data;

	if (!pdev)
		return -EINVAL;

	drv_data = devm_kzalloc(&pdev->dev, sizeof(struct pwrmon_drv_data), GFP_KERNEL);
	if (!drv_data)
		return -ENOMEM;

	drv_data->pdev = pdev;


	if (init_buffer_handler(drv_data)) {
		pr_err("Failed to init buffer handler\n");
		devm_kfree(&pdev->dev, drv_data);
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, drv_data);

	pwrmon_dir = proc_mkdir("phytium_pwrmon", NULL);
	if (!pwrmon_dir) {
		pr_err("Failed to create /proc/phytium_pwrmon dir\n");
		goto dir_create_error;
	}

	proc_entry = proc_create_data("data", 0444, pwrmon_dir, &pwrmon_ops, pdev);
	if (!proc_entry) {
		pr_err("Failed to create /proc/phytium_pwrmon/data entry\n");
		goto entry_create_error;
	}

	pr_notice("Phytium power monitor driver loaded\n");
	return 0;

entry_create_error:
	if (pwrmon_dir)
		proc_remove(pwrmon_dir);
dir_create_error:
	free_buffer_handler(drv_data);
	return -ENOMEM;
}

/**
 * pwrmon_remove - Remove a power monitor device
 * @pdev: Pointer to the platform device structure
 *
 * This function is called when the power monitor device is removed. It removes
 * the /proc interface entries associated with the power monitor device and
 * releases the buffer handler.
 *
 * Return: 0 on success.
 */
static int pwrmon_remove(struct platform_device *pdev)
{
	if (proc_entry)
		proc_remove(proc_entry);

	if (pwrmon_dir)
		proc_remove(pwrmon_dir);

	free_buffer_handler(platform_get_drvdata(pdev));
	platform_set_drvdata(pdev, NULL);
	pr_notice("Phytium power monitor driver unloaded\n");
	return 0;
}

static const struct acpi_device_id acpi_ids[] = {
	{ "PHYT800B", 0 },
	{ "", 0 }
};
MODULE_DEVICE_TABLE(acpi, acpi_ids);

static struct platform_driver pwrmon_driver = {
	.probe = pwrmon_probe,
	.remove = pwrmon_remove,
	.driver = {
		.name = "pwrmon_driver",
		.acpi_match_table = ACPI_PTR(acpi_ids),
		.pm = &pwrmon_pm_ops,
	},
};

module_platform_driver(pwrmon_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huang Shaobo <huangshaobo2075@phytium.com.cn>");
MODULE_DESCRIPTION("Phytium power monitor driver");
