/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/kdbl.h>

#define DIV_RU(num, den) (((num) + (den) - 1) / (den))

#define PRINTF_BUF_LEN (1048576)
#define PRINTF_BUF_WIDTH (256)

struct trace {
	struct list_head tc_list;

	char *tc_program;
	char *tc_version;

	unsigned int tc_flags;
	unsigned char *tc_array;
};

struct profile_element {
	uint64_t pe_total_calls;
	uint64_t pe_total_micros;
	uint64_t pe_min_micros;
	uint64_t pe_max_micros;
};

struct profile {
	struct list_head pc_list;
	spinlock_t pc_spin;

	char *pc_program;
	char *pc_version;

	unsigned int pc_flags;
	struct profile_element *pc_array;
	struct profile_element *pc_array2;
};

static char printf_buf[2 * PRINTF_BUF_LEN];
static unsigned int printf_point;
static spinlock_t printf_spin;
static struct semaphore printf_mutex;

static struct list_head trace_list;
static struct semaphore trace_lock;

static struct list_head profile_list;
static struct semaphore profile_lock;

static spinlock_t req_lock;

/**
 * str2words - find the space-separated words in a string
 * @str: the string
 * @words: a pointer to an array of strings (the words)
 *
 * Warning: destroys @str
 * Warning: you must free @words
 *
 * Returns: the number of words found
 */

static int
str2words(char *str, char ***words)
{
	unsigned int n = 1;
	char *p;
	unsigned int x = 0;

	for (p = str; *p; p++)
		if (*p == ' ')
			n++;
      
	*words = kmalloc(n * sizeof(char *), GFP_KERNEL);
	if (!*words)
		return -ENOMEM;
      
	for ((*words)[0] = p = str; *p; p++)
		if (*p == ' ') {
			*p = 0;
			(*words)[++x] = p + 1;
		}
      
	return n;
}

/**
 * kdbl_printf - print to an incore buffer
 * @fmt:
 *
 * Returns: the number of characters printed
 */

int
kdbl_printf(const char *fmt, ...)
{
	va_list va;
	int count;

	spin_lock(&printf_spin);

	if (printf_point + PRINTF_BUF_WIDTH > PRINTF_BUF_LEN) {
		memset(printf_buf + printf_point,
		       0,
		       PRINTF_BUF_LEN - printf_point);
		printf_point = 0;
	}

	va_start(va, fmt);
	count = vsnprintf(printf_buf + printf_point, PRINTF_BUF_LEN, fmt, va);
	va_end(va);

	printf_point += count;

	spin_unlock(&printf_spin);

	return count;
}

EXPORT_SYMBOL(kdbl_printf);

/**
 * kdbl_printf_dump2console - dump the incore print buffer to the console
 *
 */

void
kdbl_printf_dump2console(void)
{
	unsigned int x;

	printk("\nKdbl log:\n\n");

	spin_lock(&printf_spin);

	for (x = printf_point; x < PRINTF_BUF_LEN; x++)
		if (printf_buf[x])
			printk("%c", printf_buf[x]);

	for (x = 0; x < printf_point; x++)
		if (printf_buf[x])
			printk("%c", printf_buf[x]);

	spin_unlock(&printf_spin);

	printk("\n");
}

EXPORT_SYMBOL(kdbl_printf_dump2console);

/**
 * printf_dump - copy the incore print buffer to userspace
 * @buf: the user buffer
 * @size: the size of the user buffer
 *
 * Returns: the number of bytes copied
 */

static int
printf_dump(char __user *buf, size_t size)
{
	char *tmp_buf = printf_buf + PRINTF_BUF_LEN;
	unsigned int point;
	int error = 0;

	if (size < PRINTF_BUF_LEN)
		return -ENOBUFS;

	down(&printf_mutex);

	spin_lock(&printf_spin);
	memcpy(tmp_buf, printf_buf, PRINTF_BUF_LEN);
	point = printf_point;
	memset(printf_buf, 0, PRINTF_BUF_LEN);
	printf_point = 0;
	spin_unlock(&printf_spin);

	if (copy_to_user(buf, tmp_buf + point, PRINTF_BUF_LEN - point))
		error = -EFAULT;
	else if (point &&
		 copy_to_user(buf + PRINTF_BUF_LEN - point, tmp_buf, point))
		error = -EFAULT;

	up(&printf_mutex);

	return (error) ? error : PRINTF_BUF_LEN;
}

/**
 * kdbl_trace_create_array - create an array of set-able flags
 * @program: the name of the array
 * @version: the version 
 * @flags: the number of flags needed
 * @array: returns the array of flags
 *
 * Returns: errno
 */

int
kdbl_trace_create_array(char *program, char *version,
			unsigned int flags, unsigned char **array)
{
	struct list_head *tmp, *head;
	struct trace *tc;
	unsigned int len;
	int error;

	down(&trace_lock);

	for (head = &trace_list, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		tc = list_entry(tmp, struct trace, tc_list);
		if (strcmp(tc->tc_program, program) == 0) {
			error = -EEXIST;
			goto fail;
		}
	}

	tc = kmalloc(sizeof(struct trace), GFP_KERNEL);
	if (!tc) {
		error = -ENOMEM;
		goto fail;
	}

	memset(tc, 0, sizeof(struct trace));

	tc->tc_program = program;
	tc->tc_version = version;
	tc->tc_flags = flags;

	len = DIV_RU(flags, 8);
	tc->tc_array = kmalloc(len, GFP_KERNEL);
	if (!tc->tc_array) {
		error = -ENOMEM;
		goto fail_free;
	}
	memset(tc->tc_array, 0, len);

	list_add(&tc->tc_list, &trace_list);
	*array = tc->tc_array;

	up(&trace_lock);

	return 0;

 fail_free:
	kfree(tc);

 fail:
	up(&trace_lock);

	return error;
}

EXPORT_SYMBOL(kdbl_trace_create_array);

/**
 * kdbl_trace_destroy_array - destroy an array of flags
 * @array:
 *
 */

void
kdbl_trace_destroy_array(unsigned char *array)
{
        struct list_head *tmp, *head;
	struct trace *tc = NULL;

	down(&trace_lock);

	for (head = &trace_list, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		tc = list_entry(tmp, struct trace, tc_list);
		if (tc->tc_array == array)
			break;
	}

	if (tmp != head) {
		list_del(&tc->tc_list);
		kfree(tc->tc_array);
		kfree(tc);
	} else
		printk("kdbl: kdbl_trace_destroy_array(): unknown array\n");

	up(&trace_lock);
}

EXPORT_SYMBOL(kdbl_trace_destroy_array);

/**
 * trace_change - change a particular flag
 * @command: the command to change the flag
 *
 * The format of @command is:
 * trace_change <program> <version> <flag> [on|off]
 *
 * Returns: errno
 */

static int
trace_change(char *command)
{
	char **words;
	int n;
        struct list_head *tmp, *head;
	struct trace *tc = NULL;
	unsigned int flag;
	int error;

	n = str2words(command, &words);
	if (n < 0)
		return n;
	if (n != 5) {
		kfree(words);
		return -EINVAL;
	}

	down(&trace_lock);

	for (head = &trace_list, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		tc = list_entry(tmp, struct trace, tc_list);
		if (strcmp(tc->tc_program, words[1]) == 0)
			break;
	}

	error = -ENOENT;
	if (tmp == head)
		goto out;

	error = -EINVAL;
	if (strcmp(tc->tc_version, words[2]) != 0)
		goto out;

	if (sscanf(words[3], "%u", &flag) != 1)
		goto out;
	if (flag >= tc->tc_flags)
		goto out;

	if (strcmp(words[4], "on") == 0)
		KDBL_TRACE_SET(tc->tc_array, flag);
	else if (strcmp(words[4], "off") == 0)
		KDBL_TRACE_CLEAR(tc->tc_array, flag);
	else
		goto out;

	error = 0;

 out:
	up(&trace_lock);

	kfree(words);

	return error;
}

/**
 * kdbl_profile_exit - call at the end of a function
 * @cookie: The cookie returned by kdbl_profile_create_array
 * @flag: the flag to credit this time to
 * @start: the start time of the function
 *
 */

void
kdbl_profile_exit(void *cookie, unsigned int flag, uint64_t start)
{
	uint64_t stop = kdbl_profile_enter();
	struct profile *pc = (struct profile *)cookie;
	struct profile_element *pe = pc->pc_array + flag;
	uint64_t elapsed;

	if (stop > start)
		elapsed = stop - start;
	else
		elapsed = 0;

	spin_lock(&pc->pc_spin);

	pe->pe_total_calls++;
	pe->pe_total_micros += elapsed;
	if (pe->pe_min_micros > elapsed)
		pe->pe_min_micros = elapsed;
	if (pe->pe_max_micros < elapsed)
		pe->pe_max_micros = elapsed;

	spin_unlock(&pc->pc_spin);
}

EXPORT_SYMBOL(kdbl_profile_exit);

/**
 * kdbl_profile_create_array - create an array of profile statistics
 * @program: the name of the array
 * @version: the version of the array
 * @flags: the number of statistics to keep
 * @cookie: returns the pointer to the array
 *
 * Returns: errno
 */

int
kdbl_profile_create_array(char *program, char *version,
			  unsigned int flags, void **cookie)
{
	struct list_head *tmp, *head;
	struct profile *pc;
	unsigned int x;
	int error;

	down(&profile_lock);

	for (head = &profile_list, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		pc = list_entry(tmp, struct profile, pc_list);
		if (strcmp(pc->pc_program, program) == 0) {
			error = -EEXIST;
			goto fail;
		}
	}

	pc = kmalloc(sizeof(struct profile), GFP_KERNEL);
	if (!pc) {
		error = -ENOMEM;
		goto fail;
	}

	memset(pc, 0, sizeof(struct profile));

	spin_lock_init(&pc->pc_spin);

	pc->pc_program = program;
	pc->pc_version = version;
	pc->pc_flags = flags;

	pc->pc_array = kmalloc(pc->pc_flags * sizeof(struct profile_element),
			       GFP_KERNEL);
	if (!pc->pc_array)
	{
		error = -ENOMEM;
		goto fail_free;
	}
	memset(pc->pc_array, 0, pc->pc_flags * sizeof(struct profile_element));
	for (x = 0; x < pc->pc_flags; x++)
		pc->pc_array[x].pe_min_micros = (uint64_t)-1;

	pc->pc_array2 = kmalloc(pc->pc_flags * sizeof(struct profile_element),
				GFP_KERNEL);
	if (!pc->pc_array2)
	{
		error = -ENOMEM;
		goto fail_free2;
	}

	list_add(&pc->pc_list, &profile_list);
	*cookie = pc;

	up(&profile_lock);

	return 0;

 fail_free2:
	kfree(pc->pc_array);

 fail_free:
	kfree(pc);

 fail:
	up(&profile_lock);

	return error;
}

EXPORT_SYMBOL(kdbl_profile_create_array);

/**
 * kdbl_profile_destroy_array - destroy an array of profile statistics
 * @cookie: the array to destroy
 *
 */

void
kdbl_profile_destroy_array(void *cookie)
{
	struct profile *pc = (struct profile *)cookie;

	down(&profile_lock);

	list_del(&pc->pc_list);
	kfree(pc->pc_array2);
	kfree(pc->pc_array);
	kfree(pc);

	up(&profile_lock);
}

EXPORT_SYMBOL(kdbl_profile_destroy_array);

/**
 * profile_dump - return the current profiling statistics
 * @command: the description of which statistics to return
 *
 * The format of @command is:
 * profile_dump <program> <version>
 *
 * Returns: errno
 */

static int
profile_dump(char *command, char __user *buf, unsigned int size)
{
	char **words;
	int n;
        struct list_head *tmp, *head;
	struct profile *pc = NULL;
	unsigned int x;
	int error;

	n = str2words(command, &words);
	if (n < 0)
		return n;
	if (n != 3) {
		kfree(words);
		return -EINVAL;
	}

	down(&profile_lock);

	for (head = &profile_list, tmp = head->next;
	     tmp != head;
	     tmp = tmp->next) {
		pc = list_entry(tmp, struct profile, pc_list);
		if (strcmp(pc->pc_program, words[1]) == 0)
			break;
	}

	error = -ENOENT;
	if (tmp == head)
		goto out;

	error = -EINVAL;
	if (strcmp(pc->pc_version, words[2]) != 0)
		goto out;

	spin_lock(&pc->pc_spin);
	memcpy(pc->pc_array2, pc->pc_array, pc->pc_flags * sizeof(struct profile_element));
	memset(pc->pc_array, 0, pc->pc_flags * sizeof(struct profile_element));
	for (x = 0; x < pc->pc_flags; x++)
		pc->pc_array[x].pe_min_micros = (uint64_t)-1;
	spin_unlock(&pc->pc_spin);

	if (copy_to_user(buf, pc->pc_array2, pc->pc_flags * sizeof(struct profile_element)))
		error = -EFAULT;
	else
		error = pc->pc_flags * sizeof(struct profile_element);

 out:
	up(&profile_lock);

	kfree(words);

	return error;
}

/**
 * kdbl_proc_write - take a command from userspace
 * @file:
 * @buf:
 * @size:
 * @offset:
 *
 * Returns: -errno or the number of bytes taken
 */

static ssize_t
kdbl_proc_write(struct file *file,
		const char __user *buf, size_t size,
		loff_t *offset)
{
        char *p;

        spin_lock(&req_lock);
        p = file->private_data;
        file->private_data = NULL;
        spin_unlock(&req_lock);

        if (p)
                kfree(p);

        if (!size)
                return -EINVAL;

        p = kmalloc(size + 1, GFP_KERNEL);
        if (!p)
                return -ENOMEM;
        p[size] = 0;

        if (copy_from_user(p, buf, size)) {
                kfree(p);
                return -EFAULT;
        }

        spin_lock(&req_lock);
        file->private_data = p;
        spin_unlock(&req_lock);

        return size;
}
/**
 * kdbl_proc_read - return the results of a command
 * @file:
 * @buf:
 * @size:
 * @offset:
 *
 * Returns: -errno or the number of bytes returned
 */

static ssize_t
kdbl_proc_read(struct file *file,
	       char __user *buf, size_t size,
	       loff_t *offset)
{
        char *p;
        int error;

        spin_lock(&req_lock);
        p = file->private_data;
        file->private_data = NULL;
        spin_unlock(&req_lock);

        if (!p)
                return -ENOENT;

        if (!size) {
                kfree(p);
                return -EINVAL;
        }

        if (strcmp(p, "printf_dump") == 0)
                error = printf_dump(buf, size);
	else if (strncmp(p, "trace_change", 12) == 0)
		error = trace_change(p);
	else if (strncmp(p, "profile_dump", 12) == 0)
		error = profile_dump(p, buf, size);
	else
		error = -ENOSYS;

        kfree(p);

        return error;
}

/**
 * kdbl_proc_close - free any mismatches writes
 * @inode:
 * @file:
 *
 * Returns: 0
 */

static int
kdbl_proc_close(struct inode *inode, struct file *file)
{
        if (file->private_data)
                kfree(file->private_data);
        return 0;
}

static struct file_operations
kdbl_proc_fops = {
        .owner = THIS_MODULE,
        .write = kdbl_proc_write,
        .read = kdbl_proc_read,
        .release = kdbl_proc_close,
};

/**
 * kdbl_init - initialize the module
 *
 * Returns: errno
 */

int __init
kdbl_init(void)
{
        struct proc_dir_entry *pde;

	memset(printf_buf, 0, PRINTF_BUF_LEN);
	printf_point = 0;
	spin_lock_init(&printf_spin);
	init_MUTEX(&printf_mutex);

	INIT_LIST_HEAD(&trace_list);
	init_MUTEX(&trace_lock);

	INIT_LIST_HEAD(&profile_list);
	init_MUTEX(&profile_lock);

	spin_lock_init(&req_lock);

        pde = create_proc_entry("kdbl", S_IFREG | 0600, NULL);
        if (!pde)
		return -EINVAL;

	pde->owner = THIS_MODULE;
	pde->proc_fops = &kdbl_proc_fops;

	printk("Kdbl (built %s %s) installed\n",
	       __DATE__, __TIME__);

	return 0;
}

/**
 * kdbl_exit - clean up the module
 *
 */

void __exit
kdbl_exit(void)
{
        remove_proc_entry("kdbl", NULL);
}

MODULE_DESCRIPTION("kdbl");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");

module_init(kdbl_init);
module_exit(kdbl_exit);


