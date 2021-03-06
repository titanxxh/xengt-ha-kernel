/*
 * Instance life-cycle management
 *
 * Copyright(c) 2011-2013 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "vgt.h"
#include <linux/kthread.h>

/*
 * bitmap of allocated vgt_ids.
 * bit = 0 means free ID, =1 means allocated ID.
 */
static unsigned long vgt_id_alloc_bitmap;

struct vgt_device *vmid_2_vgt_device(int vmid)
{
	unsigned int bit;
	struct vgt_device *vgt;

	ASSERT(vgt_id_alloc_bitmap != ~0UL)
	for_each_set_bit(bit, &vgt_id_alloc_bitmap, VGT_MAX_VMS) {
		vgt = default_device.device[bit];
		if (vgt && vgt->vm_id == vmid)
			return vgt;
	}
	return NULL;
}

static int allocate_vgt_id(void)
{
	unsigned long bit_index;

	ASSERT(vgt_id_alloc_bitmap != ~0UL)
	do {
		bit_index = ffz (vgt_id_alloc_bitmap);
		if (bit_index >= VGT_MAX_VMS) {
			vgt_err("vGT: allocate_vgt_id() failed\n");
			return -ENOSPC;
		}
	} while (test_and_set_bit(bit_index, &vgt_id_alloc_bitmap) != 0);

	return bit_index;
}

static void free_vgt_id(int vgt_id)
{
	ASSERT(vgt_id >= 0 && vgt_id < VGT_MAX_VMS);
	ASSERT(vgt_id_alloc_bitmap & (1UL << vgt_id));
	clear_bit(vgt_id, &vgt_id_alloc_bitmap);
}

/*
 * Initialize the vgt state instance.
 * Return:	0: failed
 *		1: success
 *
 */
static int create_state_instance(struct vgt_device *vgt)
{
	vgt_state_t	*state;
	int i;

	vgt_dbg(VGT_DBG_GENERIC, "create_state_instance\n");
	state = &vgt->state;
	state->vReg = vzalloc(vgt->pdev->mmio_size);
	state->sReg = vzalloc(vgt->pdev->mmio_size);
	state->vReg_cp = vzalloc(vgt->pdev->mmio_size);
	state->sReg_cp = vzalloc(vgt->pdev->mmio_size);
	if ( state->vReg == NULL || state->sReg == NULL )
	{
		printk("VGT: insufficient memory allocation at %s\n", __FUNCTION__);
		if ( state->vReg )
			vfree (state->vReg);
		if ( state->sReg )
			vfree (state->sReg);
		state->sReg = state->vReg = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < I915_MAX_PIPES; i++) {
		vgt->pipe_mapping[i] = i;
	}

	for (i = 0; i < VGT_BAR_NUM; i++)
		state->bar_mapped[i] = 0;
	return 0;
}

int vgt_ha_thread(void *priv)
{
	struct vgt_device *vgt = (struct vgt_device *)priv;
	vgt_ha_t *ha = &(vgt->ha);
	vgt_info("XXH: vm %d ha thread start\n", vgt->vm_id);
	while (true)
	{
		msleep(ha->time);
		if (kthread_should_stop())
			return 0;
		if (!ha->enabled)
			continue;
		if (!ha->save_request)
			ha->save_request = 1;
	}
	return 0;
}

extern int vgt_ha_request_thread(void *);
extern void vgt_restore_vgt_state(struct vgt_device *vgt);
extern void vgt_restore_saved_instance(struct vgt_device *vgt, struct vgt_device *vgt_saved);

/*
 * priv: VCPU ?
 */
int create_vgt_instance(struct pgt_device *pdev, struct vgt_device **ptr_vgt, vgt_params_t vp)
{
	int cpu;
	struct vgt_device *vgt;
	char *cfg_space;
	u16 *gmch_ctl;
	int rc = -ENOMEM;
	int i;
	struct task_struct *thread;
	vgt_ha_t *ha;

	vgt_info("vm_id=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, fence_sz=%d, vgt_primary=%d\n",
		vp.vm_id, vp.aperture_sz, vp.gm_sz-vp.aperture_sz, vp.fence_sz, vp.vgt_primary);

	vgt = vzalloc(sizeof(*vgt));
	if (vgt == NULL) {
		printk("Insufficient memory for vgt_device in %s\n", __FUNCTION__);
		return rc;
	}

	atomic_set(&vgt->crashing, 0);

	if ((rc = vgt->vgt_id = allocate_vgt_id()) < 0 )
		goto err2;

	vgt->vm_id = vp.vm_id;
	vgt->pdev = pdev;

	vgt->force_removal = 0;

	INIT_LIST_HEAD(&vgt->list);

	if ((rc = create_state_instance(vgt)) < 0)
		goto err2;

	for (i = 0; i < I915_MAX_PORTS; i++) {
		vgt->ports[i].type = VGT_PORT_MAX;
		vgt->ports[i].cache.type = VGT_PORT_MAX;
		vgt->ports[i].port_override = i;
		vgt->ports[i].cache.port_override = i;
		vgt->ports[i].physcal_port = i;
	}

	/* Hard code ballooning now. We can support non-ballooning too in the future */
	vgt->ballooning = 1;

	/* present aperture to the guest at the same host address */
	vgt->state.aperture_base = phys_aperture_base(pdev);

	/* init aperture/gm ranges allocated to this vgt */
	if ((rc = allocate_vm_aperture_gm_and_fence(vgt, vp)) < 0) {
		printk("vGT: %s: no enough available aperture/gm/fence!\n", __func__);
		goto err2;
	}

	vgt->aperture_offset = aperture_2_gm(pdev, vgt->aperture_base);
	vgt->aperture_base_va = phys_aperture_vbase(pdev) +
		vgt->aperture_offset;

	alloc_vm_rsvd_aperture(vgt);

	vgt->state.bar_size[0] = pdev->bar_size[0];	/* MMIOGTT */
	vgt->state.bar_size[1] =			/* Aperture */
		vgt->ballooning ? pdev->bar_size[1] : vgt_aperture_sz(vgt);
	vgt->state.bar_size[2] = pdev->bar_size[2];	/* PIO */
	vgt->state.bar_size[3] = pdev->bar_size[3];	/* ROM */
	for (i = 0; i < VGT_BAR_NUM; i++) {
		vgt_info("XXH: bar %d size %x\n", i, vgt->state.bar_size[i]);
	}

	/* Set initial configuration space and MMIO space registers. */
	cfg_space = &vgt->state.cfg_space[0];
	memcpy (cfg_space, pdev->initial_cfg_space, VGT_CFG_SPACE_SZ);
	cfg_space[VGT_REG_CFG_SPACE_MSAC] = vgt->state.bar_size[1];

	/* Show guest that there isn't any stolen memory.*/
	gmch_ctl = (u16 *)(cfg_space + _REG_GMCH_CONTRL);
	if (IS_PREBDW(pdev))
		*gmch_ctl &= ~(_REGBIT_SNB_GMCH_GMS_MASK << _REGBIT_SNB_GMCH_GMS_SHIFT);
	else
		*gmch_ctl &= ~(_REGBIT_BDW_GMCH_GMS_MASK << _REGBIT_BDW_GMCH_GMS_SHIFT);

	vgt_pci_bar_write_32(vgt, VGT_REG_CFG_SPACE_BAR1, phys_aperture_base(pdev) );

	/* mark HVM's GEN device's IO as Disabled. hvmloader will enable it */
	if (vgt->vm_id != 0) {
		cfg_space[VGT_REG_CFG_COMMAND] &= ~(_REGBIT_CFG_COMMAND_IO |
						_REGBIT_CFG_COMMAND_MEMORY |
						_REGBIT_CFG_COMMAND_MASTER);
	}

	vgt_info("aperture: [0x%llx, 0x%llx] guest [0x%llx, 0x%llx] "
		"va(0x%llx)\n",
		vgt_aperture_base(vgt),
		vgt_aperture_end(vgt),
		vgt_guest_aperture_base(vgt),
		vgt_guest_aperture_end(vgt),
		(uint64_t)vgt->aperture_base_va);

	vgt_info("GM: [0x%llx, 0x%llx], [0x%llx, 0x%llx], "
		"guest[0x%llx, 0x%llx], [0x%llx, 0x%llx]\n",
		vgt_visible_gm_base(vgt),
		vgt_visible_gm_end(vgt),
		vgt_hidden_gm_base(vgt),
		vgt_hidden_gm_end(vgt),
		vgt_guest_visible_gm_base(vgt),
		vgt_guest_visible_gm_end(vgt),
		vgt_guest_hidden_gm_base(vgt),
		vgt_guest_hidden_gm_end(vgt));

	/* If the user explicitly specified a value, use it; or, use the
	 * global vgt_primary.
	 */
	ASSERT(vgt->vm_id == 0 || (vp.vgt_primary >= -1 && vp.vgt_primary <= 1));
	if (vgt->vm_id != 0 &&
		(vp.vgt_primary == 0 || (vp.vgt_primary == -1 && !vgt_primary))) {
		/* Mark vgt device as non primary VGA */
		cfg_space[VGT_REG_CFG_CLASS_CODE] = VGT_PCI_CLASS_VGA;
		cfg_space[VGT_REG_CFG_SUB_CLASS_CODE] = VGT_PCI_CLASS_VGA_OTHER;
		cfg_space[VGT_REG_CFG_CLASS_PROG_IF] = VGT_PCI_CLASS_VGA_OTHER;
	}

	state_sreg_init(vgt);
	state_vreg_init(vgt);

	/* setup the ballooning information */
	if (vgt->ballooning) {
		__vreg64(vgt, vgt_info_off(magic)) = VGT_MAGIC;
		__vreg(vgt, vgt_info_off(version_major)) = 1;
		__vreg(vgt, vgt_info_off(version_minor)) = 0;
		__vreg(vgt, vgt_info_off(display_ready)) = 0;
		__vreg(vgt, vgt_info_off(vgt_id)) = vgt->vgt_id;
		__vreg(vgt, vgt_info_off(avail_rs.low_gmadr.my_base)) = vgt_visible_gm_base(vgt);
		__vreg(vgt, vgt_info_off(avail_rs.low_gmadr.my_size)) = vgt_aperture_sz(vgt);
		__vreg(vgt, vgt_info_off(avail_rs.high_gmadr.my_base)) = vgt_hidden_gm_base(vgt);
		__vreg(vgt, vgt_info_off(avail_rs.high_gmadr.my_size)) = vgt_hidden_gm_sz(vgt);

		__vreg(vgt, vgt_info_off(avail_rs.fence_num)) = vgt->fence_sz;
		vgt_info("filling VGT_PVINFO_PAGE for dom%d:\n"
			"   visable_gm_base=0x%llx, size=0x%llx\n"
			"   hidden_gm_base=0x%llx, size=0x%llx\n"
			"   fence_base=%d, num=%d\n",
			vgt->vm_id,
			vgt_visible_gm_base(vgt), vgt_aperture_sz(vgt),
			vgt_hidden_gm_base(vgt), vgt_hidden_gm_sz(vgt),
			vgt->fence_base, vgt->fence_sz);

		ASSERT(sizeof(struct vgt_if) == VGT_PVINFO_SIZE);
	}

	vgt->bypass_addr_check = bypass_dom0_addr_check && (vgt->vm_id == 0);

	vgt_lock_dev(pdev, cpu);

	pdev->device[vgt->vgt_id] = vgt;
	list_add(&vgt->list, &pdev->rendering_idleq_head);

	vgt_unlock_dev(pdev, cpu);

	if (!vgt_init_vgtt(vgt, &vgt->gtt)) {
		vgt_err("fail to initialize vgt vgtt.\n");
		goto err2;
	}
	/*if (vgt->vm_id != 0)
		if (!vgt_init_vgtt(vgt, &vgt->ha.gtt_saved)) {
			vgt_err("fail to initialize vgt gtt_saved.\n");
			goto err2;
		}*/

	if (vgt->vm_id != 0){
		/* HVM specific init */
		if ((rc = hypervisor_hvm_init(vgt)) < 0)
			goto err;
	}

	if (vgt->vm_id) {
		if (hvm_render_owner)
			current_render_owner(pdev) = vgt;

		if (hvm_display_owner)
			current_display_owner(pdev) = vgt;

		if (hvm_super_owner) {
			ASSERT(hvm_render_owner);
			ASSERT(hvm_display_owner);
			ASSERT(hvm_boot_foreground);
			current_config_owner(pdev) = vgt;
			current_foreground_vm(pdev) = vgt;
		}
	}
	bitmap_zero(vgt->enabled_rings, MAX_ENGINES);
	bitmap_zero(vgt->started_rings, MAX_ENGINES);

	for (i = 0; i < MAX_ENGINES; ++ i) {
		vgt->rb[i].csb_write_ptr = DEFAULT_INV_SR_PTR;
	}

	/* create debugfs per vgt */
	if ((rc = vgt_create_debugfs(vgt)) < 0) {
		vgt_err("failed to create debugfs for vgt-%d\n",
			vgt->vgt_id);
		goto err;
	}

	if ((rc = vgt_create_mmio_dev(vgt)) < 0) {
		vgt_err("failed to create mmio devnode for vgt-%d\n",
				vgt->vgt_id);
		goto err;
	}

	vgt_init_i2c_edid(vgt);

	*ptr_vgt = vgt;

	/* initialize context scheduler infor */
	if (event_based_qos)
		vgt_init_sched_info(vgt);

	if (shadow_tail_based_qos)
		vgt_init_rb_tailq(vgt);

	vgt->warn_untrack = 1;

	ha = &(vgt->ha);
	if (vgt->vm_id != 0) {
		ha->request = 0;
		ha->time = 1000;
		ha->checkpoint_id = ha->checkpoint_request = 0;
		ha->saving = ha->save_request = 0;
		ha->restoring = ha->restore_request = 0;
		ha->enabled = false;
		ha->logdirty_stop = false;
		ha->skip_bb_scan = false;
		ha->incremental = false;
		ha->gm_first_cached = false;
		ha->guest_gm_bitmap_size = SIZE_1MB;//=4G >> PAGE_SHIFT
		ha->guest_gm_bitmap_inited = false;
		ha->reg_track = true;
		//ha->guest_gm_bitmap_blob.data = ha->guest_gm_bitmap;
		ha->guest_gm_bitmap_blob.data = ha->dirty_gm_bitmap;
		ha->guest_gm_bitmap_blob.size = ha->guest_gm_bitmap_size / BITS_PER_BYTE;
		ha->vgt_info_blob.size = 11 * SIZE_1MB;
		if (vgt_prepared_for_restoring && vgt_state_res) {
			ha->vgt_info_blob.data = vgt_state_res;
			vgt_state_res = NULL;
		} else {
			ha->vgt_info_blob.data = vzalloc(ha->vgt_info_blob.size);
		}
		vgt_info("XXH: guest_gm_bitmap size %lx u32 %ld ul %ld\n", ha->guest_gm_bitmap_size, sizeof(u32), sizeof(unsigned long));
		ha->saved_context_save_area = vzalloc(SZ_CONTEXT_AREA_PER_RING * pdev->max_engines);
		ha->saved_gtt = vzalloc(vgt->gtt.ggtt_mm->page_table_entry_size);
		if (!ha->saved_context_save_area || !ha->saved_gtt)
			goto err;
		vgt_info("XXH: backup gtt size %llx context_area size %llx rb %llx state.sReg %llx\n",
				(unsigned long long)vgt->gtt.ggtt_mm->page_table_entry_size,
				(unsigned long long)SZ_CONTEXT_AREA_PER_RING * pdev->max_engines,
				(unsigned long long)sizeof(vgt_state_ring_t),
				(unsigned long long)pdev->mmio_size);
		/*ha->saved_gm_size = vp.gm_sz * SIZE_1MB;
		ha->saved_gm = vzalloc(vgt->ha.saved_gm_size);
		ha->saved_gm_bitmap = vzalloc(ha->saved_gm_size >> PAGE_SHIFT);
		ha->saved_gm_bitmap_swap = vzalloc(ha->saved_gm_size >> PAGE_SHIFT);
		ha->guest_pages_initialized = false;
		ha->guest_pages = vzalloc(sizeof(ha_guest_page_t) * (ha->saved_gm_size >> PAGE_SHIFT));
		ha->guest_page_cnt = 0;
		hash_init(ha->guest_page_hash_table);
		vgt_info("XXH: backup gm size %llx bitmap size %llx addr %llx\n",
				(unsigned long long)ha->saved_gm_size, (unsigned long long)ha->saved_gm_size >> PAGE_SHIFT,
				(unsigned long long)ha->saved_gm);
		if (!ha->saved_gm || !ha->saved_context_save_area || !ha->saved_gm_bitmap || !ha->guest_pages)
			goto err;*/
		vgt_info("XXH creating threads\n");
		thread = NULL;
		init_waitqueue_head(&ha->event_wq);
		thread = kthread_run(vgt_ha_request_thread, vgt, "vgt_ha_request:%d", vgt->vm_id);
		if(IS_ERR(thread))
		{
			vgt_info("XXH creating ha request thread failed\n");
			goto err;
		}
		ha->request_thread = thread;
		/*ha->thread = NULL;
		thread = kthread_run(vgt_ha_thread, vgt, "vgt_ha:%d", vgt->vm_id);
		if(IS_ERR(thread))
		{
			vgt_info("XXH creating ha thread failed\n");
			goto err;
		}
		ha->thread = thread;*/
		if (vgt_prepared_for_restoring) {
			vgt_restore_vgt_state(vgt);
			//vgt_restore_saved_instance(vgt, vgt_saved);
		}
	}
	return 0;
err:
	vgt_clean_vgtt(vgt);
err2:
	hypervisor_hvm_exit(vgt);
	if (vgt->aperture_base > 0)
		free_vm_aperture_gm_and_fence(vgt);
	vfree(vgt->state.vReg);
	vfree(vgt->state.sReg);
	vfree(vgt->state.vReg_cp);
	vfree(vgt->state.sReg_cp);
	if (vgt->ha.thread)
		kthread_stop(vgt->ha.thread);
	if (vgt->ha.request_thread)
		kthread_stop(vgt->ha.request_thread);
	if (vgt->vm_id != 0) {
		/*if (vgt->ha.saved_gm)
			vfree(vgt->ha.saved_gm);
		if (vgt->ha.saved_gm_bitmap)
			vfree(vgt->ha.saved_gm_bitmap);
		if (vgt->ha.saved_gm_bitmap_swap)
			vfree(vgt->ha.saved_gm_bitmap_swap);
		if (vgt->ha.guest_pages)
			vfree(vgt->ha.guest_pages);*/
		if (ha->saved_context_save_area)
			vfree(ha->saved_context_save_area);
		if (ha->saved_gtt)
			vfree(ha->saved_gtt);
		if (ha->vgt_info_blob.data)
			vfree(ha->vgt_info_blob.data);
	}
	if (vgt->vgt_id >= 0)
		free_vgt_id(vgt->vgt_id);
	vfree(vgt);
	return rc;
}

void vgt_release_instance(struct vgt_device *vgt)
{
	int i;
	struct pgt_device *pdev = vgt->pdev;
	struct list_head *pos;
	struct vgt_device *v = NULL;
	int cpu;
	vgt_ha_t *ha = &(vgt->ha);
	/*struct hlist_node *n;
	ha_guest_page_t *gp;*/

	printk("prepare to destroy vgt (%d)\n", vgt->vgt_id);

	/* destroy vgt_mmio_device */
	vgt_destroy_mmio_dev(vgt);

	vgt_destroy_debugfs(vgt);

	vgt_lock_dev(pdev, cpu);

	if (vgt->ha.thread)
		kthread_stop(vgt->ha.thread);
	if (vgt->ha.request_thread)
		kthread_stop(vgt->ha.request_thread);
	if (vgt->vm_id != 0) {
		vfree(ha->saved_gtt);
		vfree(ha->saved_context_save_area);
		if (ha->vgt_info_blob.data)
			vfree(ha->vgt_info_blob.data);
	}

	printk("check render ownership...\n");
	list_for_each (pos, &pdev->rendering_runq_head) {
		v = list_entry (pos, struct vgt_device, list);
		if (v == vgt)
			break;
	}

	if (v != vgt)
		printk("vgt instance has been removed from run queue\n");
	else if (hvm_render_owner || current_render_owner(pdev) != vgt) {
		printk("remove vgt(%d) from runqueue safely\n",
			vgt->vgt_id);
		vgt_disable_render(vgt);
	} else {
		printk("vgt(%d) is current owner, request reschedule\n",
			vgt->vgt_id);
		vgt_request_force_removal(vgt);
	}

	printk("check display ownership...\n");
	if (!hvm_super_owner && (current_display_owner(pdev) == vgt)) {
		vgt_dbg(VGT_DBG_DPY, "switch display ownership back to dom0\n");
		current_display_owner(pdev) = vgt_dom0;
	}

	if (!hvm_super_owner && (current_foreground_vm(pdev) == vgt)) {
		vgt_dbg(VGT_DBG_DPY, "switch foreground vm back to dom0\n");
		pdev->next_foreground_vm = vgt_dom0;
		do_vgt_fast_display_switch(pdev);
	}

	vgt_unlock_dev(pdev, cpu);
	if (vgt->force_removal)
		/* wait for removal completion */
		wait_event(pdev->destroy_wq, !vgt->force_removal);

	printk("release display/render ownership... done\n");

	/* FIXME: any conflicts between destroy_wq ? */
	if (shadow_tail_based_qos)
		vgt_destroy_rb_tailq(vgt);

	vgt_clean_vgtt(vgt);
	hypervisor_hvm_exit(vgt);

	if (vgt->state.opregion_va) {
		vgt_hvm_opregion_map(vgt, 0);
		free_pages((unsigned long)vgt->state.opregion_va,
				VGT_OPREGION_PORDER);
	}

	vgt_lock_dev(pdev, cpu);

	vgt->pdev->device[vgt->vgt_id] = NULL;
	free_vgt_id(vgt->vgt_id);

	/* already idle */
	list_del(&vgt->list);

	vgt_unlock_dev(pdev, cpu);

	for (i = 0; i < I915_MAX_PORTS; i++) {
		if (vgt->ports[i].edid) {
			kfree(vgt->ports[i].edid);
			vgt->ports[i].edid = NULL;
		}

		if (vgt->ports[i].dpcd) {
			kfree(vgt->ports[i].dpcd);
			vgt->ports[i].dpcd = NULL;
		}

		if (vgt->ports[i].cache.edid) {
			kfree(vgt->ports[i].cache.edid);
			vgt->ports[i].cache.edid = NULL;
		}

		if (vgt->ports[i].kobj.state_initialized) {
			kobject_put(&vgt->ports[i].kobj);
		}
	}

	/* clear the gtt entries for GM of this vgt device */
	vgt_clear_gtt(vgt);

	free_vm_aperture_gm_and_fence(vgt);
	free_vm_rsvd_aperture(vgt);
	vfree(vgt->state.vReg);
	vfree(vgt->state.sReg);
	vfree(vgt->state.vReg_cp);
	vfree(vgt->state.sReg_cp);
	vfree(vgt);
	printk("vGT: vgt_release_instance done\n");
}

void vgt_reset_ppgtt(struct vgt_device *vgt, unsigned long ring_bitmap)
{
	struct vgt_mm *mm;
	int bit;

	if (!vgt->pdev->enable_ppgtt || !vgt->gtt.active_ppgtt_mm_bitmap)
		return;

	if (ring_bitmap == 0xff)
		vgt_info("VM %d: Reset full virtual PPGTT state.\n", vgt->vm_id);

	for_each_set_bit(bit, &ring_bitmap, sizeof(ring_bitmap)) {
		if (bit >= vgt->pdev->max_engines)
			break;

		if (!test_bit(bit, &vgt->gtt.active_ppgtt_mm_bitmap))
			continue;

		mm = vgt->rb[bit].active_ppgtt_mm;

		vgt_info("VM %d: Reset ring %d PPGTT state.\n", vgt->vm_id, bit);

		vgt->rb[bit].has_ppgtt_mode_enabled = 0;
		vgt->rb[bit].has_ppgtt_base_set = 0;
		vgt->rb[bit].ppgtt_page_table_level = 0;
		vgt->rb[bit].ppgtt_root_pointer_type = GTT_TYPE_INVALID;

		vgt_destroy_mm(mm);

		vgt->rb[bit].active_ppgtt_mm = NULL;
		clear_bit(bit, &vgt->gtt.active_ppgtt_mm_bitmap);
	}

	return;
}

static void vgt_reset_ringbuffer(struct vgt_device *vgt, unsigned long ring_bitmap)
{
	vgt_state_ring_t *rb;
	int bit;

	for_each_set_bit(bit, &ring_bitmap, sizeof(ring_bitmap)) {
		int i;
		if (bit >= vgt->pdev->max_engines)
			break;

		rb = &vgt->rb[bit];

		/* Drop all submitted commands. */
		vgt_init_cmd_info(rb);

		rb->uhptr = 0;
		rb->request_id = rb->uhptr_id = 0;

		rb->el_slots_head = rb->el_slots_tail = 0;
		for (i = 0; i < EL_QUEUE_SLOT_NUM; ++ i)
			memset(&rb->execlist_slots[i], 0,
				sizeof(struct vgt_exec_list));

		memset(&rb->vring, 0, sizeof(vgt_ringbuffer_t));
		memset(&rb->sring, 0, sizeof(vgt_ringbuffer_t));
		rb->csb_write_ptr = DEFAULT_INV_SR_PTR;

		vgt_disable_ring(vgt, bit);

		if (bit == RING_BUFFER_RCS) {
			struct pgt_device *pdev = vgt->pdev;
			struct vgt_rsvd_ring *ring = &pdev->ring_buffer[bit];

			memcpy((char *)v_aperture(pdev, rb->context_save_area),
					(char *)v_aperture(pdev, ring->null_context),
					SZ_CONTEXT_AREA_PER_RING);

			vgt->has_context = rb->active_vm_context = 0;
		}
	}

	return;
}

void vgt_reset_virtual_states(struct vgt_device *vgt, unsigned long ring_bitmap)
{
	ASSERT(spin_is_locked(&vgt->pdev->lock));

	vgt_reset_ringbuffer(vgt, ring_bitmap);

	vgt_reset_ppgtt(vgt, ring_bitmap);

	return;
}
