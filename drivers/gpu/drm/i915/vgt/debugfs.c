/*
 * Debugfs interfaces
 *
 * Copyright(c) 2011-2013 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of Version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* TODO: this file's code copied from arch/x86/xen/debugfs.c and
 * fs/debugfs/file.c. Can we clean up and/or minimize this file???
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include "fb_decoder.h"

#include "vgt.h"

/*
 * Dump buffer
 *
 * dump buffer provides users the ability to dump contents into a text
 * buffer first, so that the contents could be later printed by various
 * ways, like printk to stdout, or seq_printf to files.
 *
 * buffer overflow is handled inside.
 */
#define MAX_DUMP_BUFFER_SIZE 4096

int create_dump_buffer(struct dump_buffer *buf, int buf_size)
{
	buf->buffer = NULL;
	buf->buf_len = buf->buf_size = 0;

	if ((buf_size > MAX_DUMP_BUFFER_SIZE) || (buf_size <= 0)) {
		vgt_err ("Invalid dump buffer size!\n");
		return -EINVAL;
	}

	buf->buffer = vzalloc(buf_size);
	if (!buf->buffer) {
		vgt_err(
		"Buffer allocation failed for frame buffer format dump!\n");
		return -EINVAL;
	}

	buf->buf_size = buf_size;
	return 0;
}

void destroy_dump_buffer(struct dump_buffer *buf)
{
	if (buf->buffer)
		vfree(buf->buffer);

	buf->buffer = NULL;
	buf->buf_len = buf->buf_size = 0;
}

void dump_string(struct dump_buffer *buf, const char *fmt, ...)
{
	va_list args;
	int n;

	if (buf->buf_len >= buf->buf_size - 1) {
		vgt_warn("dump buffer is full! Contents will be ignored!\n");
		return;
	}

	va_start(args, fmt);
	n = vsnprintf(&buf->buffer[buf->buf_len],
			buf->buf_size - buf->buf_len, fmt, args);
	va_end(args);

	if (buf->buf_len + n >= buf->buf_size) {
		buf->buf_len = buf->buf_size - 1;
		vgt_warn("dump buffer is full! Content is truncated!\n");
	} else {
		buf->buf_len += n;
	}
}

/*************** end of dump buffer implementation **************/

/* Maximum lenth of stringlized integer is 10 */
#define MAX_VM_NAME_LEN (3 + 10)
enum vgt_debugfs_entry_t
{
	VGT_DEBUGFS_VIRTUAL_MMIO = 0,
	VGT_DEBUGFS_SHADOW_MMIO,
	VGT_DEBUGFS_FB_FORMAT,
	VGT_DEBUGFS_DPY_INFO,
	VGT_DEBUGFS_VIRTUAL_GTT,
	VGT_DEBUGFS_HA_CP,
	VGT_DEBUGFS_HA_GM_BITMAP,
	VGT_DEBUGFS_HA_STATE,
	VGT_DEBUGFS_HA_VGT_INFO,
	VGT_DEBUGFS_ENTRY_MAX
};

static debug_statistics_t  stat_info [] = {
	{ "context_switch_cycles", &context_switch_cost },
	{ "context_switch_num", &context_switch_num },
	{ "ring_idle_wait", &ring_idle_wait },
	{ "ring_0_busy", &ring_0_busy },
	{ "ring_0_idle", &ring_0_idle },
	{ "", NULL}
};

#define debugfs_create_u64_node(name, perm, parent, u64_ptr) \
	do { \
		struct dentry *__dentry = debugfs_create_u64( \
		(name),\
		(perm), \
		(parent), \
		(u64_ptr) \
		); \
		if (!__dentry) \
			printk(KERN_ERR "Failed to create debugfs node: %s\n", (name)); \
	} while (0)

static struct dentry *d_vgt_debug;
static struct dentry *d_per_vgt[VGT_MAX_VMS];
static struct dentry *d_debugfs_entry[VGT_MAX_VMS][VGT_DEBUGFS_ENTRY_MAX];
static char vm_dir_name[VGT_MAX_VMS][MAX_VM_NAME_LEN];

struct array_data
{
	void *array;
	unsigned elements;
};
struct array_data vgt_debugfs_data[VGT_MAX_VMS][VGT_DEBUGFS_ENTRY_MAX];

static int u32_array_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return nonseekable_open(inode, file);
}

/* This is generic function, used to format ring_buffer and etc. */
static size_t format_array(char *buf, size_t bufsize, const char *fmt,
				u32 *array, unsigned array_size)
{
	size_t ret = 0;
	unsigned i;

	for(i = 0; i < array_size; i++) {
		size_t len;

		if (i % 16 == 0) {
			len = snprintf(buf, bufsize, "0x%x:",i*4);
			ret += len;

			if (buf) {
				buf += len;
				bufsize -= len;
			}
		}

		len = snprintf(buf, bufsize, fmt, array[i]);
		len++;	/* ' ' or '\n' */
		ret += len;

		if (buf) {
			buf += len;
			bufsize -= len;
			buf[-1] = ((i + 1) % 16 == 0) ? '\n' : ' ';
		}
	}

	ret++;		/* \0 */
	if (buf)
		*buf = '\0';

	return ret;
}

static char *format_array_alloc(const char *fmt, u32 *array, unsigned array_size)
{
	/* very tricky way */
	size_t len = format_array(NULL, 0, fmt, array, array_size);
	char *ret;

	ret = vmalloc(len);
	if (ret == NULL) {
		vgt_err("failed to alloc memory!");
		return NULL;
	}

	format_array(ret, len, fmt, array, array_size);
	return ret;
}

/* data copied from kernel space to user space */
static ssize_t u32_array_read(struct file *file, char __user *buf, size_t len,
				loff_t *ppos)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct array_data *data = inode->i_private;
	size_t size;

	if (*ppos == 0) {
		if (file->private_data) {
			vfree(file->private_data);
			file->private_data = NULL;
		}

		file->private_data = format_array_alloc("%x", data->array, data->elements);
	}

	size = 0;
	if (file->private_data)
		size = strlen(file->private_data);

	return simple_read_from_buffer(buf, len, ppos, file->private_data, size);
}

static int vgt_array_release(struct inode *inode, struct file *file)
{
	vfree(file->private_data);
	return 0;
}

static const struct file_operations u32_array_fops = {
	.owner	= THIS_MODULE,
	.open	= u32_array_open,
	.release= vgt_array_release,
	.read	= u32_array_read,
	.llseek = no_llseek,
};

static struct dentry *vgt_debugfs_create_blob(const char *name, mode_t mode,
					struct dentry *parent,
					struct array_data *p)
{
	if (!p || !(p->array))
		return NULL;
	return debugfs_create_file(name, mode, parent, p, &u32_array_fops);
}

static inline char *reg_show_reg_owner(struct pgt_device *pdev, int i)
{
	char *str;
	switch (reg_get_owner(pdev, i)) {
		case VGT_OT_NONE:
			str = "NONE";
			break;
		case VGT_OT_RENDER:
			str = "Render";
			break;
		case VGT_OT_DISPLAY:
			str = "Display";
			break;
		case VGT_OT_CONFIG:
			str = "Config";
			break;
		default:
			str = "";
			break;
	}
	return str;
}

static inline char *reg_show_reg_type(struct pgt_device *pdev, int i)
{
	if (reg_get_owner(pdev, i) != VGT_OT_NONE)
		return "MPT";
	else if (reg_passthrough(pdev, i))
		return "PT";
	else if (reg_virt(pdev, i))
		return "Virt";
	else
		return "";
}

static int vgt_show_regs(struct seq_file *m, void *data)
{
	int i, tot;
	struct pgt_device *pdev = (struct pgt_device *)m->private;

	tot = 0;
	seq_printf(m, "------------------------------------------\n");
	seq_printf(m, "MGMT - Management context\n");
	seq_printf(m, "MPT - Mediated Pass-Through based on owner type\n");
	seq_printf(m, "PT - passthrough regs with special risk\n");
	seq_printf(m, "%8s: %8s (%-8s %-4s)\n",
			"Reg", "Flags", "Owner", "Type");
	for (i = 0; i < pdev->mmio_size; i +=  REG_SIZE) {
		if (!reg_is_accessed(pdev, i) && !reg_is_tracked(pdev, i))
			continue;

		tot++;
		seq_printf(m, "%8x: %8x (%-8s %-4s)\n",
			i, pdev->reg_info[REG_INDEX(i)],
			reg_show_reg_owner(pdev, i),
			reg_show_reg_type(pdev, i));
	}
	seq_printf(m, "------------------------------------------\n");
	seq_printf(m, "Total %d accessed registers are shown\n", tot);
	return 0;
}

static int vgt_reginfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_regs, inode->i_private);
}

static const struct file_operations reginfo_fops = {
	.open = vgt_reginfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * It's always dangerious to read from pReg directly, since some
 * read has side effect e.g. read-to-clear bit.
 *
 * So use it with caution only when debugging hard GPU hang problem
 */
static int vgt_show_pregs(struct seq_file *m, void *data)
{
	u64 i;
	struct pgt_device *pdev = (struct pgt_device *)m->private;

	seq_printf(m, "Use this interface with caution b/c side effect may be caused by reading hw status\n");
	for(i = 0; i < pdev->reg_num; i++) {
		if (!(i % 16))
			seq_printf(m, "\n%8llx:", i * REG_SIZE);
		seq_printf(m, " %x", VGT_MMIO_READ(pdev, i * REG_SIZE));
	}

	seq_printf(m, "\n");
	return 0;
}

static int vgt_preg_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_pregs, inode->i_private);
}

static const struct file_operations preg_fops = {
	.open = vgt_preg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_show_irqinfo(struct seq_file *m, void *data)
{
	struct pgt_device *pdev = (struct pgt_device *)m->private;
	struct vgt_device *vgt;
	struct pgt_statistics *pstat = &pdev->stat;
	struct vgt_statistics *vstat;
	int i, j;

	if (!pstat->irq_num) {
		seq_printf(m, "No irq logged\n");
		return 0;
	}
	seq_printf(m, "--------------------------\n");
	seq_printf(m, "Interrupt control status:\n");

	show_interrupt_regs(pdev, m);

	seq_printf(m, "Total %lld interrupts logged:\n", pstat->irq_num);
	seq_printf(m, "#	WARNING: precisely this is the number of vGT \n"
			"#	physical interrupt handler be called,\n"
			"#	each calling several events can be\n"
			"#	been handled, so usually this number\n"
			"#	is less than the total events number.\n");
	for (i = 0; i < EVENT_MAX; i++) {
		if (!pstat->events[i])
			continue;
		seq_printf(m, "\t%16lld: %s\n", pstat->events[i],
				vgt_irq_name[i]);
	}

	seq_printf(m, "%16lld: Last pirq\n", pstat->last_pirq);
	seq_printf(m, "%16lld: Last virq\n", pstat->last_virq);
	seq_printf(m, "%16lld: Average pirq cycles\n",
		pstat->pirq_cycles / pstat->irq_num);
	seq_printf(m, "%16lld: Average virq cycles\n",
		pstat->virq_cycles / pstat->irq_num);
	seq_printf(m, "%16lld: Average delay between pirq/virq handling\n",
		pstat->irq_delay_cycles / pstat->irq_num);
	/* TODO: hold lock */
	for (i = 0; i < VGT_MAX_VMS; i++) {
		if (!pdev->device[i])
			continue;

		seq_printf(m, "\n-->vgt-%d:\n", pdev->device[i]->vgt_id);
		vgt = pdev->device[i];
		vstat = &vgt->stat;

		show_virtual_interrupt_regs(vgt, m);

		seq_printf(m, "%16lld: Last injection\n",
			vstat->last_injection);

		if (!vstat->irq_num)
			continue;

		seq_printf(m, "Total %lld virtual irq injection:\n",
			vstat->irq_num);
		for (j = 0; j < EVENT_MAX; j++) {
			if (!vstat->events[j])
				continue;
			seq_printf(m, "\t%16lld: %s\n", vstat->events[j],
					vgt_irq_name[j]);
		}

		if (vstat->pending_events)
			seq_printf(m, "\t%16lld: %s\n", vstat->pending_events,
					"pending virt events");
	}
	return 0;
}

static int vgt_irqinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_irqinfo, inode->i_private);
}

static const struct file_operations irqinfo_fops = {
	.open = vgt_irqinfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

int vgt_dump_fb_format(struct dump_buffer *buf, struct vgt_fb_format *fb);
static int vgt_show_fbinfo(struct seq_file *m, void *data)
{
	struct vgt_device *vgt =  (struct vgt_device *)m->private;
	struct vgt_fb_format fb;
	int rc;

	rc = vgt_decode_fb_format(vgt->vm_id, &fb);
	if (rc != 0) {
		seq_printf(m, "Failed to get frame buffer information!\n");
	} else {
		struct dump_buffer buf;
		if ((rc = create_dump_buffer(&buf, 2048) < 0))
			return rc;
		vgt_dump_fb_format(&buf, &fb);
		seq_printf(m, "-----------FB format (VM-%d)--------\n",
					vgt->vm_id);
		seq_printf(m, "%s", buf.buffer);
		destroy_dump_buffer(&buf);
	}

	return 0;
}

static int vgt_fbinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_fbinfo, inode->i_private);
}

static const struct file_operations fbinfo_fops = {
	.open = vgt_fbinfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static inline vgt_reg_t vgt_get_mmio_value(struct pgt_device *pdev,
		struct vgt_device *vgt, unsigned int reg)
{
	ASSERT(pdev || vgt);
	return (vgt ? __vreg(vgt, reg) : VGT_MMIO_READ(pdev, reg));
}

static void vgt_dump_dpy_mmio(struct seq_file *m, struct pgt_device *pdev,
		struct vgt_device *vgt)
{
	enum vgt_pipe pipe;
	enum vgt_port port;
	const char *str;
	unsigned int reg;
	vgt_reg_t val;
	bool enabled;

	seq_printf(m, "----General CTL:\n");

	reg = _REG_CPU_VGACNTRL;
	val = vgt_get_mmio_value(pdev, vgt, reg);
	enabled = !(val & _REGBIT_VGA_DISPLAY_DISABLE);
	seq_printf(m,"\tVGA_CONTROL(0x%x):0x%08x (VGA Mode %s)\n",
		reg, val, (enabled ? "enabled" : "disabled"));

	reg = _REG_HSW_FUSE_STRAP;
	val = vgt_get_mmio_value(pdev, vgt, reg);
	seq_printf(m,"\tFUSE_STRAP(0x%x):0x%08x(RO)\n", reg, val);

	reg = _REG_SHOTPLUG_CTL;
	val = vgt_get_mmio_value(pdev, vgt, reg);
	seq_printf(m,"\tSHOTPLUG_CTL(0x%x):0x%08x\n", reg, val);

	seq_printf(m, "\n");

	seq_printf(m, "----plane:\n");
	for (pipe = PIPE_A; pipe < I915_MAX_PIPES; ++ pipe) {
		char P = VGT_PIPE_CHAR(pipe);
		reg = VGT_DSPCNTR(pipe);
		val = vgt_get_mmio_value(pdev, vgt, reg);
		enabled = !!(val & _PRI_PLANE_ENABLE);
		seq_printf(m, "\tDSPCTL_%c(0x%x): 0x%08x (%s)\n",
			P, reg, val, (enabled ? "enabled" : "disabled"));
		if (enabled) {
			reg = VGT_DSPSURF(pipe);
			seq_printf(m, "\tDSPSURF_%c(0x%x): 0x%08x\n",
				P, reg, vgt_get_mmio_value(pdev, vgt, reg));
		}
		seq_printf(m, "\n");
	}

	seq_printf(m, "----pipe:\n");
	for (pipe = PIPE_A; pipe < I915_MAX_PIPES; ++ pipe) {
		char P = VGT_PIPE_CHAR(pipe);
		reg = VGT_PIPECONF(pipe);
		val = vgt_get_mmio_value(pdev, vgt, reg);
		enabled = !!(val & _REGBIT_PIPE_ENABLE);
		seq_printf(m, "\tPIPECONF_%c(0x%x): 0x%08x (%s)\n",
			P, reg, val, (enabled ? "enabled" : "disabled"));

		if (enabled) {
			reg = VGT_PIPESRC(pipe);
			val = vgt_get_mmio_value(pdev, vgt, reg);
			seq_printf(m, "\tPIPE_SRC_%c(0x%x): 0x%08x "
					"(width : %d, height: %d)\n",
				P, reg, val, ((val >> 16) & 0xfff) + 1,
						((val & 0xfff) + 1));
			reg = VGT_HTOTAL(pipe);
			val = vgt_get_mmio_value(pdev, vgt, reg);
			seq_printf(m, "\tPIPE_HTOTAL_%c(0x%x): 0x%08x (total: %d)\n",
				P, reg, val, (val & 0xfff) + 1);
			reg = VGT_VTOTAL(pipe);
			val = vgt_get_mmio_value(pdev, vgt, reg);
			seq_printf(m, "\tPIPE_VTOTAL_%c(0x%x): 0x%08x (total: %d)\n",
				P, reg, val, (val & 0xfff) + 1);
		}

		reg = _VGT_TRANS_DDI_FUNC_CTL(pipe);
		val = vgt_get_mmio_value(pdev, vgt, reg);
		enabled = !!(val & _REGBIT_TRANS_DDI_FUNC_ENABLE);
		seq_printf(m, "\tTRANS_DDI_FUNC_CTL_%c(0x%x): 0x%08x (%s)\n",
			P, reg, val, (enabled ? "enabled" : "disabled"));

		if (enabled) {
			vgt_reg_t ddi_select, mode_select;

			ddi_select = val & _REGBIT_TRANS_DDI_PORT_MASK;
			mode_select = val & _REGBIT_TRANS_DDI_MODE_SELECT_MASK;

			switch (ddi_select >> _TRANS_DDI_PORT_SHIFT) {
				case 0:
					str = "No Port Connected"; break;
				case 1:
					str = "DDI_B"; break;
				case 2:
					str = "DDI_C"; break;
				case 3:
					str = "DDI_D"; break;
				case 4:
					str = "DDI_E"; break;
				default:
					str = "Port INV";
			}
			seq_printf(m, "\t\tmapping to port: %s\n", str);

			switch (mode_select >> _TRANS_DDI_MODE_SELECT_HIFT) {
				case 0:
					str = "HDMI"; break;
				case 1:
					str = "DVI"; break;
				case 2:
					str = "DP SST"; break;
				case 3:
					str = "DP MST"; break;
				case 4:
					str = "FDI"; break;
				default:
					str = "Mode INV";
			}
			seq_printf(m, "\t\tMode type: %s\n", str);
		}
		seq_printf(m, "\n");
	}

	reg = _REG_PIPE_EDP_CONF;
	val = vgt_get_mmio_value(pdev, vgt, reg);
	enabled = !!(val & _REGBIT_PIPE_ENABLE);
	seq_printf(m, "\tPIPECONF_EDP(0x%x): 0x%08x (%s)\n",
		reg, val, (enabled ? "enabled" : "disabled"));

	if (enabled) {
		reg = _REG_HTOTAL_EDP;
		val = vgt_get_mmio_value(pdev, vgt, reg);
		seq_printf(m, "\tPIPE_HTOTAL_EDP(0x%x): 0x%08x (total: %d)\n",
			reg, val, (val & 0xfff) + 1);
		reg = _REG_VTOTAL_EDP;
		val = vgt_get_mmio_value(pdev, vgt, reg);
		seq_printf(m, "\tPIPE_VTOTAL_EDP(0x%x): 0x%08x (total: %d)\n",
			reg, val, (val & 0xfff) + 1);
	}

	reg = _REG_TRANS_DDI_FUNC_CTL_EDP;
	val = vgt_get_mmio_value(pdev, vgt, reg);
	enabled = !!(val & _REGBIT_TRANS_DDI_FUNC_ENABLE);
	seq_printf(m, "\tTRANS_DDI_FUNC_CTL_EDP(0x%x): 0x%08x (%s)\n",
		reg, val, (enabled ? "enabled" : "disabled"));
	if (enabled) {
		vgt_reg_t edp_input = val &_REGBIT_TRANS_DDI_EDP_INPUT_MASK;
		switch (edp_input >> _TRANS_DDI_EDP_INPUT_SHIFT) {
			case 0:
				str = "Plane A 0"; break;
			case 4:
				str = "Plane A 4"; break;
			case 5:
				str = "Plane B"; break;
			case 6:
				str = "Plane C"; break;
			default:
				str = "Plane INV";
		}
		seq_printf(m, "\t\teDP select: %s\n", str);
	}
	seq_printf(m, "\n");
	
	if (is_current_display_owner(vgt)) {
		return;
	}

	seq_printf(m, "---- virtual port:\n");

	for (port = PORT_A; port < I915_MAX_PORTS; ++ port) {
		if (!dpy_has_monitor_on_port(vgt, port))
			continue;

		seq_printf(m, "\t%s connected to monitors.\n",
			VGT_PORT_NAME(port));

		if (port == PORT_E) {
			reg = _REG_PCH_ADPA;
			val = vgt_get_mmio_value(pdev, vgt, reg);
			enabled = !!(val & _REGBIT_ADPA_DAC_ENABLE);
			seq_printf(m, "\tDAC_CTL(0x%x): 0x%08x (%s)\n",
				reg, val, (enabled ? "enabled" : "disabled"));
			if (enabled) {
				pipe = (val & PORT_TRANS_SEL_MASK)
						>> PORT_TRANS_SEL_SHIFT;
				seq_printf(m, "\t\t Transcoder %c selected.\n",
					VGT_PIPE_CHAR(pipe));
			}
			reg = _REG_TRANSACONF;
			val = vgt_get_mmio_value(pdev, vgt, reg);
			enabled = !!(val & _REGBIT_TRANS_ENABLE);
			seq_printf(m, "\tPCH TRANS_CONF(0x%x): 0x%08x (%s)\n",
				reg, val, (enabled ? "enabled" : "disabled"));
		}
	}
}

static int vgt_show_phys_dpyinfo(struct seq_file *m, void *data)
{
	struct pgt_device *pdev =  (struct pgt_device *)m->private;

	seq_printf(m, "----------Physical DPY info ----------\n");
	vgt_dump_dpy_mmio(m, pdev, NULL);
	seq_printf(m, "\n");

	return 0;
}

static int vgt_show_virt_dpyinfo(struct seq_file *m, void *data)
{
	struct vgt_device *vgt =  (struct vgt_device *)m->private;
	enum vgt_pipe pipe;

	seq_printf(m, "----------DPY info (VM-%d)----------\n", vgt->vm_id);
	vgt_dump_dpy_mmio(m, NULL, vgt);
	seq_printf(m, "\n");

	seq_printf(m, "---- physical/virtual mapping:\n");
	for (pipe = PIPE_A; pipe < I915_MAX_PIPES; ++ pipe) {
		enum vgt_pipe physical_pipe = vgt->pipe_mapping[pipe];
		if (physical_pipe == I915_MAX_PIPES) {
			seq_printf(m, "\t virtual pipe %d no mapping available yet\n", pipe);
		} else {
			seq_printf(m, "\t virtual pipe %d to physical pipe %d\n", pipe, physical_pipe);
		}
	}

	return 0;
}

static int vgt_phys_dpyinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_phys_dpyinfo, inode->i_private);
}

static const struct file_operations phys_dpyinfo_fops = {
	.open = vgt_phys_dpyinfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t restore_vgt_info_write(struct file *file,
               const char __user *ubuf, size_t count, loff_t *ppos)
{
       if (vgt_prepared_for_restoring) {
               vgt_err("copy from user count %x\n", (unsigned int)count);
               if (!vgt_state_res) {
                       vgt_state_res = vzalloc(11 * SIZE_1MB);
               } else {
                       vgt_warn("XXH: flush last vgt instance state!\n");
               }
               return simple_write_to_buffer(vgt_state_res, 11 * SIZE_1MB, ppos, ubuf,
                               count);
       } else
               vgt_warn("XXH: no restoring is running!\n");
       return 0;
}

static const struct file_operations restore_vgt_info_fops = {
       .open = simple_open,
       .write = restore_vgt_info_write,
       .llseek = default_llseek,
};

static int vgt_virt_dpyinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_show_virt_dpyinfo, inode->i_private);
}

static const struct file_operations virt_dpyinfo_fops = {
	.open = vgt_virt_dpyinfo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_device_reset_show(struct seq_file *m, void *data)
{
	struct pgt_device *pdev = (struct pgt_device *)m->private;

	unsigned long flags;
	int i;

	seq_printf(m, "switch: ");
	seq_printf(m, enable_reset ? "enable" : "disable");
	seq_printf(m, "\n");

	seq_printf(m, "status: ");

	if (test_bit(RESET_INPROGRESS, &pdev->device_reset_flags))
		seq_printf(m, "resetting");
	else {
		if (get_seconds() - vgt_dom0->last_reset_time < 6)
			seq_printf(m, "hold");
		else
			seq_printf(m, "idle");
	}

	seq_printf(m, "\n");

	seq_printf(m, "waiting vm reset: ");

	spin_lock_irqsave(&pdev->lock, flags);

	for (i = 0; i < VGT_MAX_VMS; i++) {
		if (pdev->device[i]
			&& test_bit(WAIT_RESET, &pdev->device[i]->reset_flags))
		seq_printf(m, "%d ", pdev->device[i]->vm_id);
	}

	spin_unlock_irqrestore(&pdev->lock, flags);

	seq_printf(m, "\n");

	return 0;
}

static int vgt_device_reset_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_device_reset_show, inode->i_private);
}

static ssize_t vgt_device_reset_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct pgt_device *pdev = (struct pgt_device *)s->private;
	struct vgt_device *vgt;
	struct list_head *pos, *n;
	unsigned long flags;
	char buf[32];

	if (*ppos && count > sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;

	if (!enable_reset) {
		vgt_err("VGT device reset is not enabled.\n");
		return -ENODEV;
	}

	/*
	 * Prevent the protection logic bites ourself.
	 */
	if (get_seconds() - vgt_dom0->last_reset_time < 6)
		return -EAGAIN;

	if (!strncmp(buf, "normal", 6)) {
		vgt_info("Trigger device reset under normal situation.\n");

		vgt_raise_request(pdev, VGT_REQUEST_DEVICE_RESET);
	} else if (!strncmp(buf, "invalid head", 12)) {
		spin_lock_irqsave(&pdev->lock, flags);

		list_for_each_safe(pos, n, &pdev->rendering_runq_head) {
			vgt = list_entry(pos, struct vgt_device, list);

			if (vgt != current_render_owner(pdev)) {
				vgt_info("Inject an invalid RCS ring head pointer to VM: %d.\n",
						vgt->vm_id);

				vgt->rb[0].sring.head = 0xdeadbeef;
			}
		}

		spin_unlock_irqrestore(&pdev->lock, flags);
	}

	return count;
}

static const struct file_operations vgt_device_reset_fops = {
	.open = vgt_device_reset_open,
	.read = seq_read,
	.write = vgt_device_reset_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_debug_show(struct seq_file *m, void *data)
{
	struct pgt_device *pdev = (struct pgt_device *)m->private;
	unsigned long flags;

	spin_lock_irqsave(&pdev->lock, flags);
	show_debug(pdev);
	spin_unlock_irqrestore(&pdev->lock, flags);

	seq_printf(m, "\n");

	return 0;
}

static int vgt_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_debug_show, inode->i_private);
}

static const struct file_operations vgt_debug_fops = {
	.open = vgt_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_el_status_show(struct seq_file *m, void *data)
{
	struct pgt_device *pdev = (struct pgt_device *)m->private;
	unsigned long flags;

	spin_lock_irqsave(&pdev->lock, flags);
	dump_el_status(pdev);
	spin_unlock_irqrestore(&pdev->lock, flags);

	seq_printf(m, "\n");

	return 0;
}

static int vgt_el_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_el_status_show, inode->i_private);
}

static const struct file_operations vgt_el_status_fops = {
	.open = vgt_el_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_el_context_show(struct seq_file *m, void *data)
{
	struct pgt_device *pdev = (struct pgt_device *)m->private;
	unsigned long flags;

	spin_lock_irqsave(&pdev->lock, flags);
	dump_all_el_contexts(pdev);
	spin_unlock_irqrestore(&pdev->lock, flags);

	seq_printf(m, "\n");

	return 0;
}

static int vgt_el_context_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_el_context_show, inode->i_private);
}

static const struct file_operations vgt_el_context_fops = {
	.open = vgt_el_context_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_ha_checkpoint_show(struct seq_file *m, void *data)
{
	struct vgt_device *vgt =  (struct vgt_device *)m->private;
	vgt_ha_t *ha = &(vgt->ha);
	//struct vgt_mm *mm = vgt->gtt.ggtt_mm;
	int pos, cnt = 0, i = 0;

	/*seq_printf(m, "XXH test vmid=%d\n", vgt->vm_id);
	seq_printf(m, "gtt wp pages %d\n", atomic_read(&vgt->gtt.n_write_protected_guest_page));
	seq_printf(m, "ha wp pages %d\n", atomic_read(&ha->n_write_protected_guest_page));
	seq_printf(m, "guest pages %ld\n", ha->guest_page_cnt);
	seq_printf(m, "last_changed_pages_cnt=%lu\n", ha->last_changed_pages_cnt);
	seq_printf(m, "gtt_changed_entries_cnt=%lu\n", ha->gtt_changed_entries_cnt);*/
	for_each_set_bit(pos, ha->guest_gm_bitmap, ha->guest_gm_bitmap_size)
	{
		cnt++;
	}
	for_each_set_bit(pos, ha->dirty_gm_bitmap, ha->guest_gm_bitmap_size)
	{
		i++;
	}
	seq_printf(m, "\nbitmap bits total %d changed %ld dirty %d\n", cnt, ha->gtt_changed_entries_cnt, i);
	/*for (i = 0; i < mm->page_table_entry_size/sizeof(uint32_t); i ++) {
		seq_printf(m, "%x ", ((uint32_t *)(vgt->ha.saved_gtt))[i]);
		if (i % 0x100 == 0)
			seq_printf(m, "\n");
	}*/
	/*seq_printf(m, "ha enabled %s\n", ha->enabled ? "yes" : "no");
	seq_printf(m, "inc enabled %s\n", ha->incremental ? "yes" : "no");
	seq_printf(m, "pdev ppgtt enabled %s\n", vgt->pdev->enable_ppgtt ? "yes" : "no");*/
	return 0;
}

static int vgt_ha_checkpoint_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_ha_checkpoint_show, inode->i_private);
}

static ssize_t vgt_ha_checkpoint_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct vgt_device *vgt =  (struct vgt_device *)s->private;
	char buf[32];

	if (*ppos && count > sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;

	if (!strncmp(buf, "save", 4)) {
		vgt_raise_ha_request(vgt, VGT_HA_REQUEST_SAVE);
		vgt_info("XXH: ha save request set\n");
	} else if (!strncmp(buf, "prepare_restore", 15)) {
		vgt_info("XXH: ha prepare_restore\n");
	} else if (!strncmp(buf, "create", 6)) {
		vgt_raise_ha_request(vgt, VGT_HA_REQUEST_CREATE);
	} else if (!strncmp(buf, "continue", 8)) {
		vgt_raise_ha_request(vgt, VGT_HA_REQUEST_CONTINUE);
	} else if (!strncmp(buf, "ioreq", 5)) {
		hvm_toggle_iorequest_server(vgt, 1);
	} else if (!strncmp(buf, "enable", 6)) {
		vgt->ha.enabled = 1;
		vgt_info("ha enabled\n");
	} else if (!strncmp(buf, "disable", 7)) {
		vgt->ha.enabled = 0;
		vgt_info("ha disabled\n");
	} else if (!strncmp(buf, "scan", 4)) {
		vgt->ha.skip_bb_scan = !vgt->ha.skip_bb_scan;
		vgt_info("skip_bb_scan %d\n", vgt->ha.skip_bb_scan);
	} else if (!strncmp(buf, "logdirty_stop", 13)) {
		vgt->ha.logdirty_stop = 1;
		vgt_info("logdirty will stop\n");
	/*} else if (!strncmp(buf, "enable", 6)) {
		vgt->ha.enabled = !vgt->ha.enabled;
		vgt_info("XXH: ha enabled status %d for vgt %d\n", vgt->ha.enabled, vgt->vm_id);
		if (!vgt->ha.enabled) {
			vgt->ha.incremental = false;
		}
		else {
			vgt->ha.incremental = false;
			vgt->ha.gm_first_cached = false;
		}
		vgt->ha.guest_pages_initialized = false;*/
	} else if (!strncmp(buf, "inc", 3)) {
		if (!vgt->ha.enabled) {
			vgt_info("please enable ha first!\n");
		}
		else {
			vgt->ha.incremental = !vgt->ha.incremental;
			vgt_info("XXH: ha incremental status %d for vgt %d\n", vgt->ha.incremental, vgt->vm_id);
			if (vgt->ha.incremental)
				vgt->ha.gm_first_cached = false;
		}
	} else if (!strncmp(buf, "restore", 7)) {
		vgt_raise_ha_request(vgt, VGT_HA_REQUEST_RESTORE);
		vgt_info("XXH: ha restore request set\n");
		vgt_info("XXH: run queue count: %d\n", vgt_nr_in_runq(vgt->pdev));
	/*} else if (!strncmp(buf, "restore", 7)) {
		vgt->ha.restore_request = 1;
		vgt_info("XXH: ha restore request set\n");*/
	} else if (!strncmp(buf, "nrrunq", 6)) {
		struct vgt_device *vgt2;
		struct list_head *pos, *n;
		vgt_info("run queue count: %d\n", vgt_nr_in_runq(vgt->pdev));
		list_for_each_safe(pos, n, &vgt->pdev->rendering_runq_head) {
			vgt2 = list_entry(pos, struct vgt_device, list);
			vgt_info("vm %d is in run queue\n", vgt2->vm_id);
		}
		vgt_info("idle queue count: %d\n", vgt_nr_in_idleq(vgt->pdev));
	} else if (!strncmp(buf, "vrings", 6)) {
		int i;
		for (i = 0; i < vgt->pdev->max_engines; i++) {
			vgt_info("vm %d ring %d startgma %x size %lx\n", vgt->vm_id, i, vgt->rb[i].vring.start, _RING_CTL_BUF_SIZE(vgt->rb[i].vring.ctl));
		}
	} else {
		vgt_info("XXH: unknown cmd %s\n", buf);
		vgt_info("XXH: accepted cmd:\ncreate\nenable\nrestore\n");
	}

	return count;
}

static const struct file_operations ha_checkpoint_fops = {
	.open = vgt_ha_checkpoint_open,
	.read = seq_read,
	.write = vgt_ha_checkpoint_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vgt_ha_state_show(struct seq_file *m, void *data)
{
	struct vgt_device *vgt =  (struct vgt_device *)m->private;
	vgt_ha_t *ha = &(vgt->ha);
	int state = ha->enabled << 0 |
		    ha->saving << 1 |
		    ha->logdirty_stop << 2;

	ha->logdirty_stop = 0;
	//vgt_info("ha state %x\n", state);
	seq_printf(m, "%u\n", state);
	return 0;
}

static int vgt_ha_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgt_ha_state_show, inode->i_private);
}

static ssize_t vgt_ha_state_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	//struct seq_file *s = file->private_data;
	//struct vgt_device *vgt =  (struct vgt_device *)s->private;
	char buf[32];

	if (*ppos && count > sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	return count;
}

static const struct file_operations ha_state_fops = {
	.open = vgt_ha_state_open,
	.read = seq_read,
	.write = vgt_ha_state_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t ha_gm_bitmap_read(struct file *file, char __user *user_buf,
                             size_t count, loff_t *ppos)
{
	struct vgt_device *vgt = file->private_data;
	vgt_ha_t *ha = &(vgt->ha);
	struct debugfs_blob_wrapper *blob = &ha->guest_gm_bitmap_blob;
	// XXH: if not ha enabled then it is in migration.
	if (!vgt->ha.enabled) {
		memset(ha->dirty_gm_bitmap, 0, ha->guest_gm_bitmap_size / BITS_PER_BYTE);
	}
	return simple_read_from_buffer(user_buf, count, ppos, blob->data,
			blob->size);
}

static const struct file_operations ha_gm_bitmap_fops = {
       .open = simple_open,
       .read = ha_gm_bitmap_read,
       .llseek = default_llseek,
};

static ssize_t ha_vgt_info_read(struct file *file, char __user *user_buf,
                             size_t count, loff_t *ppos)
{
       struct vgt_device *vgt = file->private_data;
       struct debugfs_blob_wrapper *blob = &vgt->ha.vgt_info_blob;
       struct pgt_device *pdev = vgt->pdev;
       struct vgt_mm *mm = vgt->gtt.ggtt_mm;
       int i = 0;
       int restore_magic = 0x37;
       memcpy((char *)blob->data + i, vgt->ha.saved_gtt, mm->page_table_entry_size);
       i += mm->page_table_entry_size;
       memcpy((char *)blob->data + i, vgt->ha.saved_context_save_area, SZ_CONTEXT_AREA_PER_RING * pdev->max_engines);
       i += SZ_CONTEXT_AREA_PER_RING * pdev->max_engines;
       memcpy((char *)blob->data + i, vgt->rb_cp, sizeof(vgt_state_ring_t) * pdev->max_engines);
       i += sizeof(vgt_state_ring_t) * pdev->max_engines;
       memcpy((char *)blob->data + i, vgt->state.sReg_cp, pdev->mmio_size);
       i += pdev->mmio_size;
       memcpy((char *)blob->data + i, vgt->state.vReg_cp, pdev->mmio_size);
       i += pdev->mmio_size;
       memcpy((char *)blob->data + i, &restore_magic, sizeof(int));
       vgt_err("XXH: size of vgt info %x magic %x\n", i, restore_magic);
       return simple_read_from_buffer(user_buf, count, ppos, blob->data,
                       blob->size);
}

static ssize_t ha_vgt_info_write(struct file *file,
               const char __user *ubuf, size_t count, loff_t *ppos)
{
       struct vgt_device *vgt = file->private_data;
       struct debugfs_blob_wrapper *blob = &vgt->ha.vgt_info_blob;
       return simple_write_to_buffer(blob->data, blob->size, ppos, ubuf,
                       count);
}

static const struct file_operations ha_vgt_info_fops = {
       .open = simple_open,
       .read = ha_vgt_info_read,
       .write = ha_vgt_info_write,
       .llseek = default_llseek,
};

/* initialize vGT debufs top directory */
struct dentry *vgt_init_debugfs(struct pgt_device *pdev)
{
	struct dentry *temp_d;
	int   i;

	if (!d_vgt_debug) {
		d_vgt_debug = debugfs_create_dir("vgt", NULL);

		if (!d_vgt_debug) {
			pr_warning("Could not create 'vgt' debugfs directory\n");
			return NULL;
		}
	}

	temp_d = debugfs_create_file("device_reset", 0444, d_vgt_debug,
		pdev, &vgt_device_reset_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("show_debug", 0444, d_vgt_debug,
		pdev, &vgt_debug_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("show_el_status", 0444, d_vgt_debug,
		pdev, &vgt_el_status_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("show_el_context", 0444, d_vgt_debug,
		pdev, &vgt_el_context_fops);
	if (!temp_d)
		return NULL;

	for ( i = 0; stat_info[i].stat != NULL; i++ ) {
		temp_d = debugfs_create_u64(stat_info[i].node_name,
			0444,
			d_vgt_debug,
			stat_info[i].stat);
		if (!temp_d)
			printk(KERN_ERR "Failed to create debugfs node %s\n",
				stat_info[i].node_name);
	}

	temp_d = debugfs_create_file("reginfo", 0444, d_vgt_debug,
		pdev, &reginfo_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("preg", 0444, d_vgt_debug,
		pdev, &preg_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("irqinfo", 0444, d_vgt_debug,
		pdev, &irqinfo_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("dpyinfo", 0444, d_vgt_debug,
		pdev, &phys_dpyinfo_fops);
	if (!temp_d)
		return NULL;

	temp_d = debugfs_create_file("restore_vgt_info", 0666, d_vgt_debug,
			pdev, &restore_vgt_info_fops);
	if (!temp_d)
		return NULL;

	return d_vgt_debug;
}

static void vgt_create_cmdstat_per_ring(struct vgt_device *vgt, int ring_id, struct dentry *parent)
{
	char *ring_name;
	struct dentry *ring_dir_entry;
	switch (ring_id) {
		case RING_BUFFER_RCS:
			ring_name = "render";
			break;
		case RING_BUFFER_VCS:
			ring_name = "video";
			break;
		case RING_BUFFER_BCS:
			ring_name = "blitter";
			break;
		case RING_BUFFER_VECS:
			ring_name = "ve";
			break;
		default:
			return;
	}
	ring_dir_entry = debugfs_create_dir(ring_name, parent);
	if (!ring_dir_entry)
		printk(KERN_ERR "vGT(%d): failed to create debugfs directory: %s\n", vgt->vgt_id, ring_name);
	else {
		debugfs_create_u64_node("cmd_nr", 0444, ring_dir_entry, &(vgt->rb[ring_id].cmd_nr));
	}
}

int vgt_create_debugfs(struct vgt_device *vgt)
{
	int retval,i;
	struct array_data *p;
	int vgt_id;
	struct pgt_device *pdev;
	struct dentry *perf_dir_entry, *cmdstat_dir_entry;

	if (!vgt || !d_vgt_debug)
		return -EINVAL;

	vgt_id = vgt->vgt_id;
	pdev = vgt->pdev;

	retval = sprintf(vm_dir_name[vgt_id], "vm%d", vgt->vm_id);
	if (retval <= 0) {
		printk(KERN_ERR "vGT: failed to generating dirname:  vm%d\n", vgt->vm_id);
		return -EINVAL;
	}
	/* create vm directory */
	d_per_vgt[vgt_id] = debugfs_create_dir(vm_dir_name[vgt_id], d_vgt_debug);
	if (d_per_vgt[vgt_id] == NULL) {
		printk(KERN_ERR "vGT: creation faiure for debugfs directory: vm%d\n", vgt->vm_id);
		return -EINVAL;
	}

	/* virtual mmio space dump */
	p = &vgt_debugfs_data[vgt_id][VGT_DEBUGFS_VIRTUAL_MMIO];
	p->array = (u32 *)(vgt->state.vReg);
	p->elements = pdev->reg_num;
	d_debugfs_entry[vgt_id][VGT_DEBUGFS_VIRTUAL_MMIO] = vgt_debugfs_create_blob("virtual_mmio_space",
			0444,
			d_per_vgt[vgt_id],
			p);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_VIRTUAL_MMIO])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: virtual_mmio_space\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: virtual_mmio_space\n", vgt_id);

	p = &vgt_debugfs_data[vgt_id][VGT_DEBUGFS_SHADOW_MMIO];
	p->array = (u32 *)(vgt->state.sReg);
	p->elements = pdev->reg_num;
	d_debugfs_entry[vgt_id][VGT_DEBUGFS_SHADOW_MMIO] = vgt_debugfs_create_blob("shadow_mmio_space",
			0444,
			d_per_vgt[vgt_id],
			p);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_SHADOW_MMIO])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: shadow_mmio_space\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: shadow_mmio_space\n", vgt_id);

	/* virtual gtt space dump */
	p = &vgt_debugfs_data[vgt_id][VGT_DEBUGFS_VIRTUAL_GTT];
	p->array = (u32 *)(vgt->gtt.ggtt_mm->virtual_page_table);
	p->elements = 2* SIZE_1MB;
	d_debugfs_entry[vgt_id][VGT_DEBUGFS_VIRTUAL_GTT] =
		vgt_debugfs_create_blob("virtual_gtt_space",
			0444,
			d_per_vgt[vgt_id],
			p);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_VIRTUAL_GTT])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: "
				"virtual_mmio_space\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: virtual_mmio_space\n", vgt_id);
	/* end of virtual gtt space dump */

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_FB_FORMAT] = debugfs_create_file("frame_buffer_format",
			0444, d_per_vgt[vgt_id], vgt, &fbinfo_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_FB_FORMAT])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: frame_buffer_format\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: frame_buffer_format\n", vgt_id);

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_DPY_INFO] = debugfs_create_file("dpyinfo",
			0444, d_per_vgt[vgt_id], vgt, &virt_dpyinfo_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_FB_FORMAT])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: frame_buffer_format\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: frame_buffer_format\n", vgt_id);

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_CP] = debugfs_create_file("ha_checkpoint",
			0444, d_per_vgt[vgt_id], vgt, &ha_checkpoint_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_CP])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: ha_checkpoint\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: ha_checkpoint\n", vgt_id);

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_GM_BITMAP] = debugfs_create_file("ha_gm_bitmap",
			0444,
			d_per_vgt[vgt_id],
			vgt,
			&ha_gm_bitmap_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_GM_BITMAP])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: ha_gm_bitmap\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: ha_gm_bitmap\n", vgt_id);

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_STATE] = debugfs_create_file("ha_state",
			0444, d_per_vgt[vgt_id], vgt, &ha_state_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_STATE])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: ha_state\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: ha_state\n", vgt_id);

	d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_VGT_INFO] = debugfs_create_file("ha_vgt_info",
			0444, d_per_vgt[vgt_id], vgt, &ha_vgt_info_fops);

	if (!d_debugfs_entry[vgt_id][VGT_DEBUGFS_HA_VGT_INFO])
		printk(KERN_ERR "vGT(%d): failed to create debugfs node: ha_vgt_info\n", vgt_id);
	else
		printk("vGT(%d): create debugfs node: ha_vgt_info\n", vgt_id);

	/* perf vm perfermance statistics */
	perf_dir_entry = debugfs_create_dir("perf", d_per_vgt[vgt_id]);
	if (!perf_dir_entry)
		printk(KERN_ERR "vGT(%d): failed to create debugfs directory: perf\n", vgt_id);
	else {
		debugfs_create_u64_node ("schedule_in_time", 0444, perf_dir_entry, &(vgt->stat.schedule_in_time));
		debugfs_create_u64_node ("allocated_cycles", 0444, perf_dir_entry, &(vgt->stat.allocated_cycles));
		//debugfs_create_u64_node ("used_cycles", 0444, perf_dir_entry, &(vgt->stat.used_cycles));

		debugfs_create_u64_node ("gtt_mmio_rcnt", 0444, perf_dir_entry, &(vgt->stat.gtt_mmio_rcnt));
		debugfs_create_u64_node ("gtt_mmio_wcnt", 0444, perf_dir_entry, &(vgt->stat.gtt_mmio_wcnt));
		debugfs_create_u64_node ("gtt_mmio_wcycles", 0444, perf_dir_entry, &(vgt->stat.gtt_mmio_wcycles));
		debugfs_create_u64_node ("gtt_mmio_rcycles", 0444, perf_dir_entry, &(vgt->stat.gtt_mmio_rcycles));
		debugfs_create_u64_node ("mmio_rcnt", 0444, perf_dir_entry, &(vgt->stat.mmio_rcnt));
		debugfs_create_u64_node ("mmio_wcnt", 0444, perf_dir_entry, &(vgt->stat.mmio_wcnt));
		debugfs_create_u64_node ("mmio_wcycles", 0444, perf_dir_entry, &(vgt->stat.mmio_wcycles));
		debugfs_create_u64_node ("mmio_rcycles", 0444, perf_dir_entry, &(vgt->stat.mmio_rcycles));
		debugfs_create_u64_node ("ring_mmio_rcnt", 0444, perf_dir_entry, &(vgt->stat.ring_mmio_rcnt));
		debugfs_create_u64_node ("ring_mmio_wcnt", 0444, perf_dir_entry, &(vgt->stat.ring_mmio_wcnt));
		debugfs_create_u64_node ("ring_tail_mmio_wcnt", 0444, perf_dir_entry, &(vgt->stat.ring_tail_mmio_wcnt));
		debugfs_create_u64_node ("ring_tail_mmio_wcycles", 0444, perf_dir_entry, &(vgt->stat.ring_tail_mmio_wcycles));
		debugfs_create_u64_node ("total_cmds", 0444, perf_dir_entry, &(vgt->total_cmds));
		debugfs_create_u64_node ("vring_scan_cnt", 0444, perf_dir_entry, &(vgt->stat.vring_scan_cnt));
		debugfs_create_u64_node ("vring_scan_cycles", 0444, perf_dir_entry, &(vgt->stat.vring_scan_cycles));
		debugfs_create_u64_node ("ppgtt_wp_cnt", 0444, perf_dir_entry, &(vgt->stat.ppgtt_wp_cnt));
		debugfs_create_u64_node ("ppgtt_wp_cycles", 0444, perf_dir_entry, &(vgt->stat.ppgtt_wp_cycles));
		debugfs_create_u64_node ("skip_bb_cnt", 0444, perf_dir_entry, &(vgt->stat.skip_bb_cnt));

		/* cmd statistics for ring/batch buffers */
		cmdstat_dir_entry = debugfs_create_dir("ring", perf_dir_entry);
		if (!cmdstat_dir_entry)
			printk(KERN_ERR "vGT(%d): failed to create debugfs directory: ringbuffer\n", vgt_id);
		else
			/* for each ring */
			for (i = 0; i < pdev->max_engines; i++)
				vgt_create_cmdstat_per_ring(vgt, i, cmdstat_dir_entry);
	}

	return 0;
}

/* debugfs_remove_recursive has no return value, this fuction
 * also return nothing */
void vgt_destroy_debugfs(struct vgt_device *vgt)
{
	int vgt_id = vgt->vgt_id;

	if(!d_per_vgt[vgt_id])
		return;

	debugfs_remove_recursive(d_per_vgt[vgt_id]);
	d_per_vgt[vgt_id] = NULL;
}

void vgt_release_debugfs(void)
{
	if (!d_vgt_debug)
		return;

	debugfs_remove_recursive(d_vgt_debug);
}
