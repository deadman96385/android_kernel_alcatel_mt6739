
#include "ufs.h"
#include <linux/hie.h>
#include <linux/nls.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/rpmb.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>

#include "ufshcd.h"
#include "ufshcd-pltfrm.h"
#include "unipro.h"
#include "ufs-mtk.h"
#include "ufs-mtk-block.h"
#include "ufs-mtk-platform.h"
#include "ufs-dbg.h"

#include <mt-plat/mtk_partition.h>
#include <mt-plat/mtk_secure_api.h>
#include <scsi/ufs/ufs-mtk-ioctl.h>

/* Query request retries */
#define QUERY_REQ_RETRIES 10

static struct ufs_dev_fix ufs_fixups[] = {
	/* UFS cards deviations table */
	UFS_FIX(UFS_VENDOR_TOSHIBA, UFS_ANY_MODEL, UFS_DEVICE_QUIRK_DELAY_BEFORE_DISABLE_REF_CLK),
	UFS_FIX(UFS_VENDOR_SKHYNIX, UFS_ANY_MODEL, UFS_DEVICE_QUIRK_LIMITED_RPMB_MAX_RW_SIZE),
	END_FIX
};

/* refer to ufs_mtk_init() for default value of these globals */
int  ufs_mtk_rpm_autosuspend_delay;    /* runtime PM: auto suspend delay */
bool ufs_mtk_rpm_enabled;              /* runtime PM: on/off */
u32  ufs_mtk_auto_hibern8_timer_ms;
bool ufs_mtk_auto_hibern8_enabled;
bool ufs_mtk_host_deep_stall_enable;
bool ufs_mtk_host_scramble_enable;
bool ufs_mtk_tr_cn_used;
struct ufs_hba *ufs_mtk_hba;

static bool ufs_mtk_is_data_cmd(char cmd_op);

void ufs_mtk_dump_reg(struct ufs_hba *hba)
{
	u32 reg;

	dev_err(hba->dev, "====== Dump Registers ======\n");

	reg = ufshcd_readl(hba, REG_AHIT);
	dev_err(hba->dev, "REG_AHIT (0x%x) = 0x%08x\n", REG_AHIT, reg);

	reg = ufshcd_readl(hba, REG_INTERRUPT_ENABLE);
	dev_err(hba->dev, "REG_INTERRUPT_ENABLE (0x%x) = 0x%08x\n", REG_INTERRUPT_ENABLE, reg);

	reg = ufshcd_readl(hba, REG_CONTROLLER_STATUS);
	dev_err(hba->dev, "REG_CONTROLLER_STATUS (0x%x) = 0x%08x\n", REG_CONTROLLER_STATUS, reg);

	reg = ufshcd_readl(hba, REG_CONTROLLER_ENABLE);
	dev_err(hba->dev, "REG_CONTROLLER_ENABLE (0x%x) = 0x%08x\n", REG_CONTROLLER_ENABLE, reg);

	reg = ufshcd_readl(hba, REG_UTP_TRANSFER_REQ_INT_AGG_CONTROL);
	dev_err(hba->dev, "REG_UTP_TRANSFER_REQ_INT_AGG_CONTROL (0x%x) = 0x%08x\n",
			REG_UTP_TRANSFER_REQ_INT_AGG_CONTROL, reg);

	reg = ufshcd_readl(hba, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	dev_err(hba->dev, "REG_UTP_TRANSFER_REQ_LIST_RUN_STOP (0x%x) = 0x%08x\n",
			REG_UTP_TRANSFER_REQ_LIST_RUN_STOP, reg);
}

static int ufs_mtk_query_desc(struct ufs_hba *hba, enum query_opcode opcode,
			enum desc_idn idn, u8 index, void *desc, int len)
{
	return ufshcd_query_descriptor(hba, opcode, idn, index, 0, desc, &len);
}

static int ufs_mtk_send_uic_command(struct ufs_hba *hba, u32 cmd, u32 arg1, u32 arg2, u32 *arg3, u8 *err_code)
{
	int result;
	struct uic_command uic_cmd = {
		.command = cmd,
		.argument1 = arg1,
		.argument2 = arg2,
		.argument3 = *arg3,
	};

	result = ufshcd_send_uic_cmd(hba, &uic_cmd);

	if (err_code)
		*err_code = uic_cmd.argument2 & MASK_UIC_COMMAND_RESULT;

	if (result) {
		dev_err(hba->dev, "UIC command error: %#x\n", result);
		return -EIO;
	}
	if (cmd == UIC_CMD_DME_GET || cmd == UIC_CMD_DME_PEER_GET)
		*arg3 = uic_cmd.argument3;

	return 0;
}

int ufs_mtk_run_batch_uic_cmd(struct ufs_hba *hba, struct uic_command *cmds, int ncmds)
{
	int i;
	int err = 0;

	for (i = 0; i < ncmds; i++) {

		err = ufshcd_send_uic_cmd(hba, &cmds[i]);

		if (err) {
			dev_err(hba->dev, "ufs_mtk_run_batch_uic_cmd fail, cmd: %x, arg1: %x\n",
				cmds->command, cmds->argument1);
			/* return err; */
		}
	}

	return err;
}

int ufs_mtk_cfg_unipro_cg(struct ufs_hba *hba, bool enable)
{
	u32 tmp;

	if (enable) {
		ufshcd_dme_get(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), &tmp);
		tmp = tmp | (1 << RX_SYMBOL_CLK_GATE_EN) |
		      (1 << SYS_CLK_GATE_EN) |
		      (1 << TX_CLK_GATE_EN);
		ufshcd_dme_set(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba, UIC_ARG_MIB(VENDOR_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp & ~(1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba, UIC_ARG_MIB(VENDOR_DEBUGCLOCKENABLE), tmp);
	} else {
		ufshcd_dme_get(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), &tmp);
		tmp = tmp & ~((1 << RX_SYMBOL_CLK_GATE_EN) |
			      (1 << SYS_CLK_GATE_EN) |
			      (1 << TX_CLK_GATE_EN));
		ufshcd_dme_set(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba, UIC_ARG_MIB(VENDOR_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp | (1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba, UIC_ARG_MIB(VENDOR_DEBUGCLOCKENABLE), tmp);
	}

	return 0;
}

static void ufs_mtk_advertise_hci_quirks(struct ufs_hba *hba)
{
#if defined(CONFIG_MTK_HW_FDE) && defined(UFS_MTK_PLATFORM_UFS_HCI_PERF_HEURISTIC)
	hba->quirks |= UFSHCD_QUIRK_UFS_HCI_PERF_HEURISTIC;
#endif

#if defined(UFS_MTK_PLATFORM_UFS_HCI_RST_DEV_FOR_LINKUP_FAIL)
	hba->quirks |= UFSHCD_QUIRK_UFS_HCI_DEV_RST_FOR_LINKUP_FAIL;
#endif

#if defined(UFS_MTK_PLATFORM_UFS_HCI_VENDOR_HOST_RST)
	hba->quirks |= UFSHCD_QUIRK_UFS_HCI_VENDOR_HOST_RST;
#endif

#if defined(UFS_MTK_PLATFORM_UFS_HCI_MANUALLY_DISABLE_AH8_BEFORE_RING_DOORBELL)
	hba->quirks |= UFSHCD_QUIRK_UFS_HCI_DISABLE_AH8_BEFORE_RING_DOORBELL;
#endif

	dev_info(hba->dev, "hci quirks: %#x\n", hba->quirks);
}

#ifdef CONFIG_MTK_HW_FDE
void ufs_mtk_hwfde_cfg_cmd(struct ufs_hba *hba,
	struct scsi_cmnd *cmd)
{
	u32 dunl, dunu, lba;
	unsigned long flags;
	int hwfde_key_idx_old;
	struct ufshcd_lrb *lrbp;

	lrbp = &hba->lrb[cmd->request->tag];

	/* for r/w request only */
	if (cmd->request->bio && cmd->request->bio->bi_hw_fde) {

		/* in case hw fde is enabled and key index is updated by dm-crypt */
		if (cmd->request->bio->bi_key_idx != hba->crypto_hwfde_key_idx) {

			/* acquire host lock to ensure atomicity during key change in atf */
			spin_lock_irqsave(hba->host->host_lock, flags);

			/* do key change */
			mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 3), 0, 0);

			hwfde_key_idx_old = hba->crypto_hwfde_key_idx;
			hba->crypto_hwfde_key_idx = cmd->request->bio->bi_key_idx;

			spin_unlock_irqrestore(hba->host->host_lock, flags);

			dev_info(hba->dev, "update hw-fde key, ver %d->%d\n", hwfde_key_idx_old,
				hba->crypto_hwfde_key_idx);
		}

		lba = ((cmd->cmnd[2]) << 24) | ((cmd->cmnd[3]) << 16) |
		((cmd->cmnd[4]) << 8) | (cmd->cmnd[5]);

		ufs_mtk_crypto_cal_dun(UFS_CRYPTO_ALGO_ESSIV_AES_CBC, lba, &dunl, &dunu);

		lrbp->crypto_cfgid = 0;
		lrbp->crypto_dunl = dunl;
		lrbp->crypto_dunu = dunu;
		lrbp->crypto_en = 1;

		/* mark data has ever gone through encryption/decryption path */
		hba->crypto_feature |= UFS_CRYPTO_HW_FDE_ENCRYPTED;

	} else {
		/* disable inline encryption */
		lrbp->crypto_en = 0;
	}
}

#else
void ufs_mtk_hwfde_cfg_cmd(struct ufs_hba *hba,
	struct scsi_cmnd *cmd) {};
#endif

int ufs_mtk_perf_heurisic_if_allow_cmd(struct ufs_hba *hba, struct scsi_cmnd *cmd)
{
	u32 tag;

	if (!(hba->quirks & UFSHCD_QUIRK_UFS_HCI_PERF_HEURISTIC))
		return 0;

	/* Check rw commands only and allow all other commands. */
	if (ufs_mtk_is_data_cmd(cmd->cmnd[0])) {

		tag = cmd->request->tag;

		if (hba->req_tag_map & (1 << tag)) {
			dev_info(hba->dev, "tag %d is already set\n", tag);
			return 0;
		}

		if (!hba->req_r_cnt && !hba->req_w_cnt) {

			/* Case: no on-going r or w commands. */

			if (ufs_mtk_is_data_write_cmd(cmd->cmnd[0]))
				hba->req_w_cnt++;
			else
				hba->req_r_cnt++;

		} else {

			/*
			 * Case: we have on-going r or w commands.
			 *
			 * Do not allow issueing w command if on-going commands are read.
			 * Do not allow issueing r command if on-going commands are write.
			 */

			if (ufs_mtk_is_data_write_cmd(cmd->cmnd[0])) {

				if (hba->req_r_cnt)
					return 1;

				hba->req_w_cnt++;

			} else {

				if (hba->req_w_cnt)
					return 1;

				hba->req_r_cnt++;
			}
		}

		__set_bit(tag, &hba->req_tag_map);
	}

	return 0;
}

void ufs_mtk_perf_heurisic_req_done(struct ufs_hba *hba, struct scsi_cmnd *cmd)
{
	u32 tag;

	if (!(hba->quirks & UFSHCD_QUIRK_UFS_HCI_PERF_HEURISTIC))
		return;

	if (ufs_mtk_is_data_cmd(cmd->cmnd[0])) {

		tag = cmd->request->tag;

		if (hba->req_tag_map & (1 << tag)) {

			if (ufs_mtk_is_data_write_cmd(cmd->cmnd[0]))
				hba->req_w_cnt--;
			else
				hba->req_r_cnt--;

			__clear_bit(tag, &hba->req_tag_map);

		} else
			dev_info(hba->dev, "tag %d is not set in req_tag_map\n", tag);
	}
}

static enum ufs_pm_level
ufs_mtk_get_desired_pm_lvl_for_dev_link_state(enum ufs_dev_pwr_mode dev_state,
					enum uic_link_state link_state)
{
	enum ufs_pm_level lvl;

	for (lvl = UFS_PM_LVL_0; lvl < UFS_PM_LVL_MAX; lvl++) {
		if ((ufs_pm_lvl_states[lvl].dev_state == dev_state) &&
			(ufs_pm_lvl_states[lvl].link_state == link_state))
			return lvl;
	}

	/* if no match found, return the level 0 */
	return UFS_PM_LVL_0;
}

static bool ufs_mtk_is_valid_pm_lvl(int lvl)
{
	if (lvl >= 0 && lvl < ufs_pm_lvl_states_size)
		return true;
	else
		return false;
}

static int ufs_mtk_init(struct ufs_hba *hba)
{
	/* initialize globals */
	ufs_mtk_rpm_autosuspend_delay = -1;
	ufs_mtk_rpm_enabled = false;
	ufs_mtk_auto_hibern8_timer_ms = 0;
	ufs_mtk_auto_hibern8_enabled = false;
	ufs_mtk_host_deep_stall_enable = 0;
	ufs_mtk_host_scramble_enable = 0;
	ufs_mtk_tr_cn_used = 0;
	ufs_mtk_hba = hba;
	hba->crypto_hwfde_key_idx = -1;

	ufs_mtk_pltfrm_init();

	ufs_mtk_pltfrm_parse_dt(hba);

	ufs_mtk_advertise_hci_quirks(hba);

	/* Get PM level from device tree */
	ufs_mtk_parse_pm_levels(hba);

	/*
	 * If rpm_lvl and and spm_lvl are not already set to valid levels,
	 * set the default power management level for UFS runtime and system
	 * suspend. Default power saving mode selected is keeping UFS link in
	 * Hibern8 state and UFS device in sleep.
	 */
	if (!ufs_mtk_is_valid_pm_lvl(hba->rpm_lvl))
		hba->rpm_lvl = ufs_mtk_get_desired_pm_lvl_for_dev_link_state(
							UFS_SLEEP_PWR_MODE,
							UIC_LINK_HIBERN8_STATE);
	if (!ufs_mtk_is_valid_pm_lvl(hba->spm_lvl))
		hba->spm_lvl = ufs_mtk_get_desired_pm_lvl_for_dev_link_state(
							UFS_SLEEP_PWR_MODE,
							UIC_LINK_HIBERN8_STATE);

	/* Get auto-hibern8 timeout from device tree */
	ufs_mtk_parse_auto_hibern8_timer(hba);

#ifdef CONFIG_MTK_UFS_BOOTING
	/* Rename device to unify device path for booting storage device */
	device_rename(hba->dev, "bootdevice");
#endif

	return 0;

}

static int ufs_mtk_pre_pwr_change(struct ufs_hba *hba,
			      struct ufs_pa_layer_attr *desired,
			      struct ufs_pa_layer_attr *final)
{
	struct ufs_descriptor desc;
	int err = 0;

	/* get manu ID for vendor specific configuration in the future */
	if (hba->manu_id == 0) {

		/* read device descriptor */
		desc.descriptor_idn = 0;
		desc.index = 0;

		err = ufs_mtk_query_desc(hba, UPIU_QUERY_FUNC_STANDARD_READ_REQUEST,
					      desc.descriptor_idn, desc.index,
					      desc.descriptor, sizeof(desc.descriptor));
		if (err)
			return err;

		/* get wManufacturerID */
		hba->manu_id = desc.descriptor[0x18] << 8 | desc.descriptor[0x19];
		dev_dbg(hba->dev, "wManufacturerID: 0x%x\n", hba->manu_id);
	}

#if 0 /* standard way: use the maximum speed supported by device */
	memcpy(final, desired, sizeof(struct ufs_pa_layer_attr));
#else /* for compatibility, use assigned speed temporarily */
/* HSG3B as default power mode, only use HSG1B at FPGA */
#ifndef CONFIG_FPGA_EARLY_PORTING
	final->gear_rx = 3;
	final->gear_tx = 3;
#else
	final->gear_rx = 1;
	final->gear_tx = 1;
#endif
	final->lane_rx = 1;
	final->lane_tx = 1;
	final->hs_rate = PA_HS_MODE_B;
	final->pwr_rx = FAST_MODE;
	final->pwr_tx = FAST_MODE;
#endif

	ufs_mtk_pltfrm_pwr_change_final_gear(hba, final);

	return err;
}

static int ufs_mtk_pwr_change_notify(struct ufs_hba *hba,
			      enum ufs_notify_change_status stage, struct ufs_pa_layer_attr *desired,
			      struct ufs_pa_layer_attr *final)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_pre_pwr_change(hba, desired, final);
		break;
	case POST_CHANGE:
		break;
	default:
		break;
	}

	return ret;
}

static int ufs_mtk_init_mphy(struct ufs_hba *hba)
{
	return 0;
}

static int ufs_mtk_reset_host(struct ufs_hba *hba)
{
	if (!(hba->quirks & UFSHCD_QUIRK_UFS_HCI_VENDOR_HOST_RST))
		return 0;

	/* avoid resetting host during resume flow or when link is not off */
	if (hba->pm_op_in_progress || !ufshcd_is_link_off(hba))
		return 0;

	dev_info(hba->dev, "reset host\n");

	/* do host sw reset */
	mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 6), 0, 0);

#ifdef CONFIG_MTK_HW_FDE

	/* restore HW FDE related settings by re-using resume operation */
	mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 2), 0, 0);

#endif

	return 0;
}

static int ufs_mtk_reset_device(struct ufs_hba *hba)
{
	dev_info(hba->dev, "reset device\n");

	/* do device hw reset */
	mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 5), 0, 0);

	return 0;
}

int ufs_mtk_linkup_fail_handler(struct ufs_hba *hba, int left_retry)
{
	if (!(hba->quirks & UFSHCD_QUIRK_UFS_HCI_DEV_RST_FOR_LINKUP_FAIL))
		return 0;

	if (left_retry <= 1)
		ufs_mtk_reset_device(hba);

	return 0;
}

static int ufs_mtk_hce_enable_notify(struct ufs_hba *hba,
	enum ufs_notify_change_status stage)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_reset_host(hba);
		break;
	case POST_CHANGE:
		break;
	default:
		break;
	}

	return ret;
}

static int ufs_mtk_pre_link(struct ufs_hba *hba)
{
	int ret = 0;
	u32 tmp;

	ufs_mtk_pltfrm_bootrom_deputy(hba);

	ufs_mtk_init_mphy(hba);

	/* ensure auto-hibern8 is disabled during hba probing */
	ufshcd_vops_auto_hibern8(hba, false);

	/* configure deep stall */
	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), &tmp);
	if (ret)
		return ret;

	if (ufs_mtk_host_deep_stall_enable)   /* enable deep stall */
		tmp |= (1 << 6);
	else
		tmp &= ~(1 << 6);   /* disable deep stall */

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VENDOR_SAVEPOWERCONTROL), tmp);
	if (ret)
		return ret;

	/* configure scrambling */
	if (ufs_mtk_host_scramble_enable)
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_SCRAMBLING), 1);
	else
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB(PA_SCRAMBLING), 0);

	return ret;
}

static int ufs_mtk_post_link(struct ufs_hba *hba)
{
	int ret = 0;
	u32 arg = 0;

	/* disable device LCC */
	ret = ufs_mtk_send_uic_command(hba, UIC_CMD_DME_SET, UIC_ARG_MIB(PA_LOCALTXLCCENABLE), 0, &arg, NULL);

	if (ret) {
		dev_err(hba->dev, "dme_setting_after_link fail\n");
		ret = 0;	/* skip error */
	}

#ifdef CONFIG_MTK_HW_FDE

	/* init HW FDE feature inlined in HCI */
	mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 0), 0, 0);

#endif

#ifdef CONFIG_HIE

	/* init ufs crypto IP for HIE */
	mt_secure_call(MTK_SIP_KERNEL_CRYPTO_HIE_INIT, 0, 0, 0);

#endif

	/* enable unipro clock gating feature */
	ufs_mtk_cfg_unipro_cg(hba, true);

	return ret;

}

static int ufs_mtk_link_startup_notify(struct ufs_hba *hba, enum ufs_notify_change_status stage)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_pre_link(hba);
		break;
	case POST_CHANGE:
		ret = ufs_mtk_post_link(hba);
		break;
	default:
		break;
	}

	return ret;
}

static int ufs_mtk_suspend(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	int ret = 0;

	if (ufshcd_is_link_hibern8(hba)) {
		/*
		 * HCI power-down flow with link in hibern8
		 * Enter vendor-specific power down mode to keep UniPro state
		 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VENDOR_UNIPROPOWERDOWNCONTROL, 0), 1);
		if (ret)
			return ret;

		ufs_mtk_pltfrm_suspend(hba);

#ifdef CONFIG_MTK_HW_FDE

		/* HW FDE related suspend operation */
		mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 1), 0, 0);

#endif

#ifdef CONFIG_HIE

		/* hie suspend handling: reset key hint */
		hie_kh_reset(ufs_mtk_hie_get_dev());

#endif
	}

	return ret;
}

static void ufs_mtk_dbg_register_dump(struct ufs_hba *hba)
{
	u32 val;

	/* read debugging register REG_UFS_MTK_PROBE */

	/*
	 * configure REG_UFS_MTK_DEBUG_SEL to direct debugging information
	 * to REG_UFS_MTK_PROBE.
	 *
	 * REG_UFS_MTK_DEBUG_SEL is not required to set back
	 * as 0x0 (default value) after dump.
	 */
	ufshcd_writel(hba, 0x20, REG_UFS_MTK_DEBUG_SEL);

	val = ufshcd_readl(hba, REG_UFS_MTK_PROBE);

	dev_info(hba->dev, "REG_UFS_MTK_PROBE: 0x%x\n", val);
}

static int ufs_mtk_resume(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	int ret = 0;

	if (ufshcd_is_link_hibern8(hba)) {

		ufs_mtk_pltfrm_resume(hba);

		/*
		 * HCI power-on flow with link in hibern8
		 *
		 * Enable UFSHCI
		 * NOTE: interrupt mask UFSHCD_UIC_MASK (UIC_COMMAND_COMPL | UFSHCD_UIC_PWR_MASK)
		 *       will be enabled inside.
		 */
		ret = ufshcd_hba_enable(hba);
		if (ret)
			return ret;

		/* Leave vendor-specific power down mode to resume UniPro state */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VENDOR_UNIPROPOWERDOWNCONTROL, 0), 0);
		if (ret)
			return ret;

		/*
		 * Leave hibern8 state
		 * NOTE: ufshcd_make_hba_operational will be ok only if link is in active state.
		 *
		 *       The reason is: ufshcd_make_hba_operational will check if REG_CONTROLLER_STATUS
		 *       is OK. In REG_CONTROLLER_STATUS, UTMRLRDY and UTRLRDY will be ready only if
		 *       DP is set. DP will be set only if link is in active state in MTK design.
		 */
		ret = ufshcd_uic_hibern8_exit(hba);
		if (!ret)
			ufshcd_set_link_active(hba);

		/* Re-start hba */
		ret = ufshcd_make_hba_operational(hba);
		if (ret)
			return ret;

#ifdef CONFIG_MTK_HW_FDE

		/* HW FDE related resume operation */
		mt_secure_call(MTK_SIP_KERNEL_HW_FDE_UFS_CTL, (1 << 2), 0, 0);

#endif
	}

	return ret;
}

/* replace non-printable or non-ASCII characters with spaces */
static inline void ufs_mtk_remove_non_printable(char *val)
{
	if (!val)
		return;

	if (*val < 0x20 || *val > 0x7e)
		*val = ' ';
}

static int ufs_mtk_read_string_desc(struct ufs_hba *hba, int desc_index, u8 *buf,
				    u32 size, bool ascii)
{
	int err = 0;

	err = ufshcd_read_desc(hba,
			       QUERY_DESC_IDN_STRING, desc_index, buf, size);

	if (err) {
		dev_err(hba->dev, "%s: reading String Desc failed after %d retries. err = %d\n",
			__func__, QUERY_REQ_RETRIES, err);
		goto out;
	}

	if (ascii) {
		int desc_len;
		int ascii_len;
		int i;
		char *buff_ascii;

		desc_len = buf[0];
		/* remove header and divide by 2 to move from UTF16 to UTF8 */
		ascii_len = (desc_len - QUERY_DESC_HDR_SIZE) / 2 + 1;
		if (size < ascii_len + QUERY_DESC_HDR_SIZE) {
			dev_err(hba->dev, "%s: buffer allocated size is too small\n",
				__func__);
			err = -ENOMEM;
			goto out;
		}

		buff_ascii = kmalloc(ascii_len, GFP_KERNEL);
		if (!buff_ascii) {
			err = -ENOMEM;
			goto out_free_buff;
		}

		/*
		 * the descriptor contains string in UTF16 format
		 * we need to convert to utf-8 so it can be displayed
		 */
		utf16s_to_utf8s((wchar_t *)&buf[QUERY_DESC_HDR_SIZE],
				desc_len - QUERY_DESC_HDR_SIZE,
				UTF16_BIG_ENDIAN, buff_ascii, ascii_len);

		/* replace non-printable or non-ASCII characters with spaces */
		for (i = 0; i < ascii_len; i++)
			ufs_mtk_remove_non_printable(&buff_ascii[i]);

		memset(buf + QUERY_DESC_HDR_SIZE, 0,
		       size - QUERY_DESC_HDR_SIZE);
		memcpy(buf + QUERY_DESC_HDR_SIZE, buff_ascii, ascii_len);
		buf[QUERY_DESC_LENGTH_OFFSET] = ascii_len + QUERY_DESC_HDR_SIZE;
out_free_buff:
		kfree(buff_ascii);
	}
out:
	return err;
}

static int ufs_mtk_get_device_info(struct ufs_hba *hba,
				   struct ufs_device_info *card_data)
{
	int err;
	u8 model_index;
	u8 str_desc_buf[QUERY_DESC_STRING_MAX_SIZE + 1] = {0};
	u8 desc_buf[QUERY_DESC_DEVICE_MAX_SIZE];

	err = ufshcd_read_desc(hba, QUERY_DESC_IDN_DEVICE, 0, desc_buf, QUERY_DESC_DEVICE_MAX_SIZE);

	if (err) {
		dev_err(hba->dev, "%s: Failed reading Device Desc. err = %d\n",
			__func__, err);
		goto out;
	}

	/*
	 * getting vendor (manufacturerID) and Bank Index in big endian
	 * format
	 */
	card_data->wmanufacturerid = desc_buf[DEVICE_DESC_PARAM_MANF_ID] << 8 |
				     desc_buf[DEVICE_DESC_PARAM_MANF_ID + 1];

	model_index = desc_buf[DEVICE_DESC_PARAM_PRDCT_NAME];

	err = ufs_mtk_read_string_desc(hba, model_index, str_desc_buf,
				       QUERY_DESC_STRING_MAX_SIZE, ASCII_STD);
	if (err) {
		dev_err(hba->dev, "%s: Failed reading Product Name. err = %d\n",
			__func__, err);
		goto out;
	}

	str_desc_buf[QUERY_DESC_STRING_MAX_SIZE] = '\0';
	strlcpy(card_data->model, (str_desc_buf + QUERY_DESC_HDR_SIZE),
		min_t(u8, str_desc_buf[QUERY_DESC_LENGTH_OFFSET],
		      MAX_MODEL_LEN));

	/* Null terminate the model string */
	card_data->model[MAX_MODEL_LEN] = '\0';

out:
	return err;
}

void ufs_mtk_advertise_fixup_device(struct ufs_hba *hba)
{
	int err;
	struct ufs_dev_fix *f;
	struct ufs_device_info card_data;

	card_data.wmanufacturerid = 0;

	err = ufs_mtk_get_device_info(hba, &card_data);
	if (err) {
		dev_err(hba->dev, "%s: Failed getting device info. err = %d\n",
			__func__, err);
		return;
	}

	for (f = ufs_fixups; f->quirk; f++) {
		if (((f->card.wmanufacturerid == card_data.wmanufacturerid) ||
			(f->card.wmanufacturerid == UFS_ANY_VENDOR)) &&
			(STR_PRFX_EQUAL(f->card.model, card_data.model) ||
			 !strcmp(f->card.model, UFS_ANY_MODEL)))
			hba->dev_quirks |= f->quirk;
	}

	dev_info(hba->dev, "dev quirks: %#x\n", hba->dev_quirks);
}

void ufs_mtk_parse_pm_levels(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;
	u32 val;

	if (np) {
		/* system PM */
		if (of_property_read_u32(np, "mediatek,spm-level", &hba->spm_lvl))
			hba->spm_lvl = -1;

		/* runtime PM */
		if (of_property_read_u32(np, "mediatek,rpm-level", &hba->rpm_lvl))
			hba->rpm_lvl = -1;

		if (!of_property_read_u32(np, "mediatek,rpm-enable", &val)) {
			if (val)
				ufs_mtk_rpm_enabled = true;
		}

		if (of_property_read_u32(np, "mediatek,rpm-autosuspend-delay", &ufs_mtk_rpm_autosuspend_delay))
			ufs_mtk_rpm_autosuspend_delay = -1;
	}
}

void ufs_mtk_parse_auto_hibern8_timer(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct device_node *np = dev->of_node;

	if (np) {
		if (of_property_read_u32(np, "mediatek,auto-hibern8-timer", &ufs_mtk_auto_hibern8_timer_ms))
			ufs_mtk_auto_hibern8_timer_ms = 0;
	}

	dev_info(hba->dev, "auto-hibern8 timer %d ms\n", ufs_mtk_auto_hibern8_timer_ms);
}

static int ufs_mtk_ffu_send_cmd(struct scsi_device *dev, struct ufs_ioctl_ffu_data *idata)
{
	struct ufs_hba *hba;
	unsigned char cmd[10];
	struct scsi_sense_hdr sshdr;
	unsigned long flags;
	int ret;

	if (dev) {
		hba = shost_priv(dev->host);
	} else {
		return -ENODEV;
	}

	spin_lock_irqsave(hba->host->host_lock, flags);

	ret = scsi_device_get(dev);
	if (!ret && !scsi_device_online(dev)) {
		ret = -ENODEV;
		scsi_device_put(dev);
	}


	spin_unlock_irqrestore(hba->host->host_lock, flags);

	if (ret)
		return ret;

	/*
	 * If scsi commands fail, the scsi mid-layer schedules scsi error-
	 * handling, which would wait for host to be resumed. Since we know
	 * we are functional while we are here, skip host resume in error
	 * handling context.
	 */
	hba->host->eh_noresume = 1;

	cmd[0] = WRITE_BUFFER;                   /* Opcode */
	cmd[1] = 0xE;                            /* 0xE: Download firmware */
	cmd[2] = 0;                              /* Buffer ID = 0 */
	cmd[3] = 0;                              /* Buffer Offset[23:16] = 0 */
	cmd[4] = 0;                              /* Buffer Offset[15:08] = 0 */
	cmd[5] = 0;                              /* Buffer Offset[07:00] = 0 */
	cmd[6] = (idata->buf_byte >> 16) & 0xff; /* Parameter List Length[23:16] */
	cmd[7] = (idata->buf_byte >> 8) & 0xff;  /* Parameter List Length[15:08] */
	cmd[8] = (idata->buf_byte) & 0xff;       /* Parameter List Length[07:00] */
	cmd[9] = 0x0;                            /* Control = 0 */

	/*
	 * Current function would be generally called from the power management
	 * callbacks hence set the REQ_PM flag so that it doesn't resume the
	 * already suspended children.
	 */
	ret = scsi_execute_req_flags(dev, cmd, DMA_TO_DEVICE,
					idata->buf_ptr, idata->buf_byte, &sshdr,
					msecs_to_jiffies(1000), 0, NULL, REQ_PM);

	if (ret) {
		sdev_printk(KERN_ERR, dev,
			  "WRITE BUFFER failed for firmware upgrade\n");
	}

	scsi_device_put(dev);
	hba->host->eh_noresume = 0;
	return ret;
}

int ufs_mtk_ioctl_get_fw_ver(struct scsi_device *dev, void __user *buf_user)
{
	struct ufs_hba *hba;
	struct ufs_ioctl_query_fw_ver_data *idata = NULL;
	int err;

	if (dev) {
		hba = shost_priv(dev->host);
	} else {
		return -ENODEV;
	}

	/* check scsi device instance */
	if (!dev->rev) {
		dev_err(hba->dev, "%s: scsi_device or rev is NULL\n", __func__);
		err = -ENOENT;
		goto out;
	}

	idata = kzalloc(sizeof(struct ufs_ioctl_query_fw_ver_data), GFP_KERNEL);

	if (!idata) {
		err = -ENOMEM;
		goto out;
	}

	/* extract params from user buffer */
	err = copy_from_user(idata, buf_user,
			sizeof(struct ufs_ioctl_query_fw_ver_data));

	if (err) {
		dev_err(hba->dev,
			"%s: failed copying buffer from user, err %d\n",
			__func__, err);
		goto out_release_mem;
	}

	idata->buf_byte = min_t(int, UFS_IOCTL_FFU_MAX_FW_VER_BYTES,
				idata->buf_byte);

	/*
	 * Copy firmware version string to user buffer
	 *
	 * We get firmware version from scsi_device->rev, which is ready in scsi_add_lun()
	 * during SCSI device probe process.
	 *
	 * If probe failed, rev will be NULL. We checked it in the beginning of this function.
	 */
	err = copy_to_user(idata->buf_ptr, dev->rev, idata->buf_byte);

	if (err) {
		dev_err(hba->dev, "%s: err %d copying back to user.\n",
				__func__, err);
		goto out_release_mem;
	}

out_release_mem:
	kfree(idata);
out:
	return 0;
}

int ufs_mtk_ioctl_ffu(struct scsi_device *dev, void __user *buf_user)
{
	struct ufs_hba *hba = shost_priv(dev->host);
	struct ufs_ioctl_ffu_data *idata = NULL;
	struct ufs_ioctl_ffu_data *idata_user = NULL;
	int err = 0;
	u32 attr = 0;

	idata = kzalloc(sizeof(struct ufs_ioctl_ffu_data), GFP_KERNEL);
	if (!idata) {
		err = -ENOMEM;
		goto out;
	}

	idata_user = kzalloc(sizeof(struct ufs_ioctl_ffu_data), GFP_KERNEL);
	if (!idata_user) {
		kfree(idata);
		err = -ENOMEM;
		goto out;
	}

	/* extract struct idata from user buffer */
	err = copy_from_user(idata_user, buf_user, sizeof(struct ufs_ioctl_ffu_data));

	if (err) {
		dev_err(hba->dev,
			"%s: failed copying buffer from user, err %d\n",
			__func__, err);
		goto out_release_mem;
	}

	memcpy(idata, idata_user, sizeof(struct ufs_ioctl_ffu_data));

	/* extract firmware from user buffer */
	if (idata->buf_byte > (u32)UFS_IOCTL_FFU_MAX_FW_SIZE_BYTES) {
		dev_err(hba->dev, "%s: idata->buf_byte:0x%x > max 0x%x bytes\n", __func__,
				idata->buf_byte, (u32)UFS_IOCTL_FFU_MAX_FW_SIZE_BYTES);
		err = -ENOMEM;
		goto out_release_mem;
	}
	idata->buf_ptr = kzalloc(idata->buf_byte, GFP_KERNEL);

	if (!idata->buf_ptr) {
		err = -ENOMEM;
		goto out_release_mem;
	}

	if (copy_from_user(idata->buf_ptr, (void __user *)idata_user->buf_ptr, idata->buf_byte)) {
		err = -EFAULT;
		goto out_release_mem;
	}

	/* do FFU */
	err = ufs_mtk_ffu_send_cmd(dev, idata);

	if (err)
		dev_err(hba->dev, "%s: ffu failed, err %d\n", __func__, err);
	else
		dev_err(hba->dev, "%s: ffu ok\n", __func__);

	/*
	 * Check bDeviceFFUStatus attribute
	 *
	 * For reference only since UFS spec. said the status is valid after
	 * device power cycle.
	 */

	err = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR, QUERY_ATTR_IDN_DEVICE_FFU_STATUS, 0, 0, &attr);

	if (err) {
		dev_err(hba->dev, "%s: query bDeviceFFUStatus failed, err %d\n", __func__, err);
		goto out_release_mem;
	}

	if (attr > UFS_FFU_STATUS_OK)
		dev_err(hba->dev, "%s: bDeviceFFUStatus shows fail %d (ref only)\n", __func__, attr);

out_release_mem:
	kfree(idata->buf_ptr);
	kfree(idata);
	kfree(idata_user);
out:
	return err;
}

int ufs_mtk_ioctl_query(struct ufs_hba *hba, u8 lun, void __user *buf_user)
{
	struct ufs_ioctl_query_data *idata;
	void __user *user_buf_ptr;
	int err = 0;
	int length = 0;
	void *data_ptr;
	bool flag;
	u32 att = 0;
	u8 *desc = NULL;
	u8 read_desc, read_attr, write_attr, read_flag;

	idata = kzalloc(sizeof(struct ufs_ioctl_query_data), GFP_KERNEL);
	if (!idata) {
		err = -ENOMEM;
		goto out;
	}

	/* extract params from user buffer */
	err = copy_from_user(idata, buf_user,
			sizeof(struct ufs_ioctl_query_data));
	if (err) {
		dev_err(hba->dev,
			"%s: failed copying buffer from user, err %d\n",
			__func__, err);
		goto out_release_mem;
	}

	user_buf_ptr = idata->buf_ptr;

	/*
	 * save idata->idn to specific local variable to avoid
	 * confusion by comapring idata->idn with different enums below.
	 */
	switch (idata->opcode) {
	case UPIU_QUERY_OPCODE_READ_DESC:
		read_desc = idata->idn;
		break;
	case UPIU_QUERY_OPCODE_READ_ATTR:
		read_attr = idata->idn;
		break;
	case UPIU_QUERY_OPCODE_WRITE_ATTR:
		write_attr = idata->idn;
		break;
	case UPIU_QUERY_OPCODE_READ_FLAG:
		read_flag = idata->idn;
		break;
	default:
		goto out_einval;
	}

	/* verify legal parameters & send query */
	switch (idata->opcode) {
	case UPIU_QUERY_OPCODE_READ_DESC:
		switch (read_desc) {
		case QUERY_DESC_IDN_DEVICE:
		case QUERY_DESC_IDN_STRING:
			break;
		default:
			goto out_einval;
		}

		length = min_t(int, QUERY_DESC_MAX_SIZE,
				idata->buf_byte);

		desc = kzalloc(length, GFP_KERNEL);

		if (!desc) {
			err = -ENOMEM;
			goto out_release_mem;
		}

		err = ufshcd_query_descriptor(hba, idata->opcode,
				read_desc, idata->idx, 0, desc, &length);
		break;
	case UPIU_QUERY_OPCODE_READ_ATTR:
		switch (read_attr) {
		case QUERY_ATTR_IDN_BOOT_LUN_EN:
			break;
		case QUERY_ATTR_IDN_DEVICE_FFU_STATUS:
			break;
		default:
			goto out_einval;
		}
		err = ufshcd_query_attr(hba, idata->opcode,
					read_attr, idata->idx, 0, &att);
		break;
	case UPIU_QUERY_OPCODE_WRITE_ATTR:
		switch (write_attr) {
		case QUERY_ATTR_IDN_BOOT_LUN_EN:
			break;
		default:
			goto out_einval;
		}

		length = min_t(int, sizeof(int), idata->buf_byte);

		if (copy_from_user(&att, (void __user *)(unsigned long)
					idata->buf_ptr, length)) {
			err = -EFAULT;
			goto out_release_mem;
		}

		err = ufshcd_query_attr(hba, idata->opcode,
					write_attr, idata->idx, 0, &att);
		break;
	case UPIU_QUERY_OPCODE_READ_FLAG:
		switch (read_flag) {
		case QUERY_FLAG_IDN_PERMANENTLY_DISABLE_FW_UPDATE:
			break;
		default:
			goto out_einval;
		}
		err = ufshcd_query_flag(hba, idata->opcode,
					read_flag, &flag);
		break;
	default:
		goto out_einval;
	}

	if (err) {
		dev_err(hba->dev, "%s: query for idn %d failed\n", __func__,
					idata->idn);
		goto out_release_mem;
	}

	/*
	 * copy response data
	 * As we might end up reading less data then what is specified in
	 * "ioct_data->buf_byte". So we are updating "ioct_data->
	 * buf_byte" to what exactly we have read.
	 */
	switch (idata->opcode) {
	case UPIU_QUERY_OPCODE_READ_DESC:
		idata->buf_byte = min_t(int, idata->buf_byte, length);
		data_ptr = desc;
		break;
	case UPIU_QUERY_OPCODE_READ_ATTR:
		idata->buf_byte = sizeof(att);
		data_ptr = &att;
		break;
	case UPIU_QUERY_OPCODE_READ_FLAG:
		idata->buf_byte = 1;
		data_ptr = &flag;
		break;
	case UPIU_QUERY_OPCODE_WRITE_ATTR:
		/* write attribute does not require coping response data */
		goto out_release_mem;
	default:
		goto out_einval;
	}

	/* copy to user */
	err = copy_to_user(buf_user, idata,
			sizeof(struct ufs_ioctl_query_data));
	if (err)
		dev_err(hba->dev, "%s: failed copying back to user.\n",
			__func__);

	err = copy_to_user(user_buf_ptr, data_ptr, idata->buf_byte);
	if (err)
		dev_err(hba->dev, "%s: err %d copying back to user.\n",
				__func__, err);
	goto out_release_mem;

out_einval:
	dev_err(hba->dev,
		"%s: illegal ufs query ioctl data, opcode 0x%x, idn 0x%x\n",
		__func__, idata->opcode, (unsigned int)idata->idn);
	err = -EINVAL;
out_release_mem:
	kfree(idata);
	kfree(desc);
out:
	return err;
}

static int ufs_mtk_scsi_dev_cfg(struct scsi_device *sdev, enum ufs_scsi_dev_cfg op)
{
	if (op == UFS_SCSI_DEV_SLAVE_CONFIGURE) {
		if (ufs_mtk_rpm_enabled) {
			sdev->use_rpm_auto = 1;
			sdev->autosuspend_delay = ufs_mtk_rpm_autosuspend_delay;
		} else {
			sdev->use_rpm_auto = 0;
			sdev->autosuspend_delay = -1;
		}
	}

	return 0;
}

void ufs_mtk_runtime_pm_init(struct scsi_device *sdev)
{
	/*
	 * If runtime PM is enabled for UFS device, use auto-suspend mechanism
	 * if assigned.
	 *
	 * This is only for those SCSI devices which runtime PM is not managed by
	 * block layer. Thus autosuspend of this device is not configured by sd_probe_async.
	 */
	if (ufs_mtk_rpm_enabled && sdev->autosuspend_delay >= 0) {
		pm_runtime_set_autosuspend_delay(&sdev->sdev_gendev, sdev->autosuspend_delay);
		pm_runtime_use_autosuspend(&sdev->sdev_gendev);
	}
}

static void ufs_mtk_auto_hibern8(struct ufs_hba *hba, bool enable)
{
	/* if auto-hibern8 is not enabled by device tree, return */
	if (!ufs_mtk_auto_hibern8_timer_ms)
		return;

	dev_dbg(hba->dev, "ah8: %d, t: %d ms\n", enable, ufs_mtk_auto_hibern8_timer_ms);

	/* if already enabled or disabled, return */
	if (!(ufs_mtk_auto_hibern8_enabled ^ enable))
		return;

	if (enable) {
		/*
		 * For UFSHCI 2.0 (in Elbrus), ensure hibernate enter/exit interrupts are disabled during
		 * auto hibern8.
		 *
		 * For UFSHCI 2.1 (in future projects), keep these 2 interrupts for auto-hibern8
		 * error handling.
		 */
		ufshcd_disable_intr(hba, (UIC_HIBERNATE_ENTER | UIC_HIBERNATE_EXIT));

		/* set timer scale as "ms" and timer */
		ufshcd_writel(hba, (0x03 << 10 | ufs_mtk_auto_hibern8_timer_ms), REG_AHIT);

		ufs_mtk_auto_hibern8_enabled = true;
	} else {
		/* disable auto-hibern8 */
		ufshcd_writel(hba, 0, REG_AHIT);

		/* ensure hibernate enter/exit interrupts are enabled for future manual-hibern8 */
		ufshcd_enable_intr(hba, (UIC_HIBERNATE_ENTER | UIC_HIBERNATE_EXIT));

		ufs_mtk_auto_hibern8_enabled = false;
	}
}

int ufs_mtk_auto_hiber8_quirk_handler(struct ufs_hba *hba, bool enable)
{
	/*
	 * do not toggle ah8 during suspend/resume callback since ah8 policy is always fixed
	 * as below,
	 *
	 * 1. suspend: ah8 is disabled before suspend flow starts.
	 * 2. resume: ah8 is enabled after resume flow is finished.
	 */
	if (hba->quirks & UFSHCD_QUIRK_UFS_HCI_DISABLE_AH8_BEFORE_RING_DOORBELL &&
		!hba->outstanding_reqs &&
		!hba->outstanding_tasks &&
		!hba->pm_op_in_progress) {

		ufshcd_vops_auto_hibern8(hba, enable);
	}

	return 0;
}

/* Notice: this function must be called in automic context */
/* Because it is not protected by ufs spin_lock or mutex, it access ufs host directly. */
int ufs_mtk_generic_read_dme(u32 uic_cmd, u16 mib_attribute, u16 gen_select_index, u32 *value, unsigned long retry_ms)
{
	u32 arg1;
	u32 ret;
	unsigned long elapsed_us = 0;

	if (ufs_mtk_hba->outstanding_reqs || ufs_mtk_hba->outstanding_tasks
		|| ufs_mtk_hba->active_uic_cmd || ufs_mtk_hba->pm_op_in_progress) {
		dev_dbg(ufs_mtk_hba->dev, "req: %lx, task %lx, pm %x\n", ufs_mtk_hba->outstanding_reqs
			, ufs_mtk_hba->outstanding_tasks, ufs_mtk_hba->pm_op_in_progress);
		if (ufs_mtk_hba->active_uic_cmd != NULL)
			dev_dbg(ufs_mtk_hba->dev, "uic not null\n");
		return -1;
	}

	arg1 = ((u32)mib_attribute << 16) | (u32)gen_select_index;
	ufshcd_writel(ufs_mtk_hba, arg1, REG_UIC_COMMAND_ARG_1);
	ufshcd_writel(ufs_mtk_hba, 0, REG_UIC_COMMAND_ARG_2);
	ufshcd_writel(ufs_mtk_hba, 0, REG_UIC_COMMAND_ARG_3);
	ufshcd_writel(ufs_mtk_hba, uic_cmd, REG_UIC_COMMAND);

	while ((ufshcd_readl(ufs_mtk_hba, REG_INTERRUPT_STATUS) & UIC_COMMAND_COMPL) != UIC_COMMAND_COMPL) {
		/* busy waiting 1us */
		udelay(1);
		elapsed_us += 1;
		if (elapsed_us > (retry_ms * 1000))
			return -ETIMEDOUT;
	}
	ufshcd_writel(ufs_mtk_hba, UIC_COMMAND_COMPL, REG_INTERRUPT_STATUS);

	ret = ufshcd_readl(ufs_mtk_hba, REG_UIC_COMMAND_ARG_2);
	if (ret & MASK_UIC_COMMAND_RESULT)
		return ret;

	*value = ufshcd_readl(ufs_mtk_hba, REG_UIC_COMMAND_ARG_3);

	return 0;
}

int ufs_mtk_deepidle_hibern8_check(void)
{
	return ufs_mtk_pltfrm_deepidle_check_h8();
}

void ufs_mtk_deepidle_leave(void)
{
	ufs_mtk_pltfrm_deepidle_leave();
}

static const char *ufs_mtk_uic_link_state_to_string(
			enum uic_link_state state)
{
	switch (state) {
	case UIC_LINK_OFF_STATE:     return "OFF";
	case UIC_LINK_ACTIVE_STATE:  return "ACTIVE";
	case UIC_LINK_HIBERN8_STATE: return "HIBERN8";
	default:                     return "UNKNOWN";
	}
}

static const char *ufs_mtk_dev_pwr_mode_to_string(
			enum ufs_dev_pwr_mode state)
{
	switch (state) {
	case UFS_ACTIVE_PWR_MODE:    return "ACTIVE";
	case UFS_SLEEP_PWR_MODE:     return "SLEEP";
	case UFS_POWERDOWN_PWR_MODE: return "POWERDOWN";
	default:                     return "UNKNOWN";
	}
}

static ssize_t ufs_mtk_rpm_info_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	int curr_len;

	curr_len = snprintf(buf, PAGE_SIZE,
			    "Runtime PM level = %d => dev_state [%s] link_state [%s]\n",
			    hba->rpm_lvl,
			    ufs_mtk_dev_pwr_mode_to_string(
				ufs_pm_lvl_states[hba->rpm_lvl].dev_state),
			    ufs_mtk_uic_link_state_to_string(
				ufs_pm_lvl_states[hba->rpm_lvl].link_state));

	curr_len += snprintf((buf + curr_len), (PAGE_SIZE - curr_len),
			     "Runtime PM enable = %d\nRuntime PM auto suspend delay = %d ms\n",
			     ufs_mtk_rpm_enabled,
			     ufs_mtk_rpm_autosuspend_delay);

	return curr_len;
}

static ssize_t ufs_mtk_rpm_info_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	/* disable store function */
	return 0;
}

static void ufs_mtk_add_rpm_info_sysfs_nodes(struct ufs_hba *hba)
{
	hba->rpm_info_attr.show = ufs_mtk_rpm_info_show;
	hba->rpm_info_attr.store = ufs_mtk_rpm_info_store;
	sysfs_attr_init(&hba->rpm_info_attr.attr);
	hba->rpm_info_attr.attr.name = "rpm_info";
	hba->rpm_info_attr.attr.mode = S_IRUGO;
	if (device_create_file(hba->dev, &hba->rpm_info_attr))
		dev_err(hba->dev, "Failed to create sysfs for rpm_info\n");
}

static ssize_t ufs_mtk_spm_info_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	int curr_len;

	curr_len = snprintf(buf, PAGE_SIZE,
			    "System PM level = %d => dev_state [%s] link_state [%s]\n",
			    hba->spm_lvl,
			    ufs_mtk_dev_pwr_mode_to_string(
				ufs_pm_lvl_states[hba->spm_lvl].dev_state),
			    ufs_mtk_uic_link_state_to_string(
				ufs_pm_lvl_states[hba->spm_lvl].link_state));

	return curr_len;
}

static ssize_t ufs_mtk_spm_info_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	/* disable store function */
	return 0;
}

static void ufs_mtk_add_spm_info_sysfs_nodes(struct ufs_hba *hba)
{
	hba->spm_info_attr.show = ufs_mtk_spm_info_show;
	hba->spm_info_attr.store = ufs_mtk_spm_info_store;
	sysfs_attr_init(&hba->spm_info_attr.attr);
	hba->spm_info_attr.attr.name = "spm_info";
	hba->spm_info_attr.attr.mode = S_IRUGO;
	if (device_create_file(hba->dev, &hba->spm_info_attr))
		dev_err(hba->dev, "Failed to create sysfs for spm_info\n");
}

void ufs_mtk_add_sysfs_nodes(struct ufs_hba *hba)
{
	ufs_mtk_add_rpm_info_sysfs_nodes(hba);
	ufs_mtk_add_spm_info_sysfs_nodes(hba);
}

struct rpmb_dev *ufs_mtk_rpmb_get_raw_dev(void)
{
	return ufs_mtk_hba->rawdev_ufs_rpmb;
}

void ufs_mtk_rpmb_dump_frame(struct scsi_device *sdev, u8 *data_frame, u32 cnt)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		pr_debug("\n[ufs-rpmb] frame #%d\n", i);
		pr_debug("[ufs-rpmb] mac, frame[196] = 0x%x\n", data_frame[196]);
		pr_debug("[ufs-rpmb] mac, frame[197] = 0x%x\n", data_frame[197]);
		pr_debug("[ufs-rpmb] mac, frame[198] = 0x%x\n", data_frame[198]);
		pr_debug("[ufs-rpmb] data,frame[228] = 0x%x\n", data_frame[228]);
		pr_debug("[ufs-rpmb] data,frame[229] = 0x%x\n", data_frame[229]);
		pr_debug("[ufs-rpmb] nonce, frame[484] = 0x%x\n", data_frame[484]);
		pr_debug("[ufs-rpmb] nonce, frame[485] = 0x%x\n", data_frame[485]);
		pr_debug("[ufs-rpmb] nonce, frame[486] = 0x%x\n", data_frame[486]);
		pr_debug("[ufs-rpmb] nonce, frame[487] = 0x%x\n", data_frame[487]);
		pr_debug("[ufs-rpmb] wc, frame[500] = 0x%x\n", data_frame[500]);
		pr_debug("[ufs-rpmb] wc, frame[501] = 0x%x\n", data_frame[501]);
		pr_debug("[ufs-rpmb] wc, frame[502] = 0x%x\n", data_frame[502]);
		pr_debug("[ufs-rpmb] wc, frame[503] = 0x%x\n", data_frame[503]);
		pr_debug("[ufs-rpmb] addr, frame[504] = 0x%x\n", data_frame[504]);
		pr_debug("[ufs-rpmb] addr, frame[505] = 0x%x\n", data_frame[505]);
		pr_debug("[ufs-rpmb] blkcnt,frame[506] = 0x%x\n", data_frame[506]);
		pr_debug("[ufs-rpmb] blkcnt,frame[507] = 0x%x\n", data_frame[507]);
		pr_debug("[ufs-rpmb] result, frame[508] = 0x%x\n", data_frame[508]);
		pr_debug("[ufs-rpmb] result, frame[509] = 0x%x\n", data_frame[509]);
		pr_debug("[ufs-rpmb] type, frame[510] = 0x%x\n", data_frame[510]);
		pr_debug("[ufs-rpmb] type, frame[511] = 0x%x\n", data_frame[511]);

		data_frame += 512;
	}
}

static struct ufs_cached_region ufs_mtk_cached_region[UFS_CACHED_REGION_CNT] = {
	{"cache", 0, 0},
	{"userdata", 0, 0},
#ifdef MTK_UFS_HQA
	/* For HQA project, skip cache policy change in testing partiton */
	{"mvg_test", 0, 0},
#endif
};

void ufs_mtk_cache_setup_cmd(struct scsi_cmnd *cmd)
{
	int i;
	sector_t start_sect;
	sector_t end_sect;

	/*
	 * In consideration of data robustness, we only change
	 * cache policy for write command if required.
	 *
	 * Normally read commands always enable cache by default for
	 * better performance.
	 */

	if (!ufs_mtk_is_data_write_cmd(cmd->cmnd[0]))
		return;

	start_sect = blk_rq_pos(cmd->request);
	end_sect = start_sect + blk_rq_sectors(cmd->request);

	for (i = 0; i < UFS_CACHED_REGION_CNT; i++) {

		if (!ufs_mtk_cached_region[i].start_sect &&
			!ufs_mtk_cached_region[i].end_sect)
			continue;

		if (start_sect >= ufs_mtk_cached_region[i].start_sect &&
			end_sect <= ufs_mtk_cached_region[i].end_sect) {

			/* Cached or skipped. Skip any cache policy change and just return */
			return;
		}
	}

	/* Non-cached, try to add FUA flag in WRITE command block */

	if (likely(cmd->cmnd[0] != WRITE_6)) {
		/*
		 * Use magic number 0x8 here since kernel scsi layer does
		 * not define proper macro for flag FUA in command block.
		 */
		cmd->cmnd[1] |= 0x8;
	} else {
		/*
		 * FUA is enabled in WRITE_6 command, this is illegal
		 * since WRITE_6 has no FUA field in command block.
		 */
		return;
	}
}

void ufs_mtk_cache_get_region(struct work_struct *work)
{
	struct hd_struct *hd = NULL;
	int i;

	for (i = 0; i < UFS_CACHED_REGION_CNT; i++) {

		hd = get_part(ufs_mtk_cached_region[i].name);

		/* note that sector size is 512 bytes in hd_struct */

		if (likely(hd)) {
			ufs_mtk_cached_region[i].start_sect = hd->start_sect;
			ufs_mtk_cached_region[i].end_sect = hd->start_sect + hd->nr_sects;
			put_part(hd);

			pr_err("ufs cached part: %s, sect 0x%llx - 0x%llx\n",
				ufs_mtk_cached_region[i].name,
				(u64)ufs_mtk_cached_region[i].start_sect,
				(u64)ufs_mtk_cached_region[i].end_sect);
		} else {
			pr_err("ufs cached part: %s: No info.\n",
				ufs_mtk_cached_region[i].name);
		}

		hd = NULL;
	}
}

static struct delayed_work ufs_mtk_cache_get_info_work;
static int __init ufs_mtk_cache_init_work(void)
{
	INIT_DELAYED_WORK(&ufs_mtk_cache_get_info_work, ufs_mtk_cache_get_region);
	schedule_delayed_work(&ufs_mtk_cache_get_info_work, 100);
	return 0;
}

#ifdef CONFIG_MTK_UFS_DEBUG
void ufs_mtk_dump_asc_ascq(struct ufs_hba *hba, u8 asc, u8 ascq)
{
	dev_info(hba->dev, "Sense Data: ASC=%#04x, ASCQ=%#04x\n", asc, ascq);

	if (asc == 0x25) {
		if (ascq == 0x00)
			dev_err(hba->dev, "Logical unit not supported!\n");
	} else if (asc == 0x29) {
		if (ascq == 0x00)
			dev_err(hba->dev, "Power on, reset, or bus device reset occupied\n");
	}
}
#endif

void ufs_mtk_crypto_cal_dun(u32 alg_id, u32 lba, u32 *dunl, u32 *dunu)
{
	if (alg_id != UFS_CRYPTO_ALGO_BITLOCKER_AES_CBC) {
		*dunl = lba;
		*dunu = 0;
	} else {                             /* bitlocker dun use byte address */
		*dunl = (lba & 0x7FFFF) << 12;   /* byte address for lower 32 bit */
		*dunu = (lba >> (32 - 12)) << 12;  /* byte address for higher 32 bit */
	}
}

bool ufs_mtk_is_data_write_cmd(char cmd_op)
{
	if (cmd_op == WRITE_10 || cmd_op == WRITE_16 || cmd_op == WRITE_6)
		return true;

	return false;
}

static bool ufs_mtk_is_data_cmd(char cmd_op)
{
	if (cmd_op == WRITE_10 || cmd_op == READ_10 ||
	    cmd_op == WRITE_16 || cmd_op == READ_16 ||
	    cmd_op == WRITE_6 || cmd_op == READ_6)
		return true;

	return false;
}

static struct ufs_cmd_str_struct ufs_mtk_cmd_str_tbl[] = {
	{"TEST_UNIT_READY",        0x00},
	{"REQUEST_SENSE",          0x03},
	{"FORMAT_UNIT",            0x04},
	{"READ_BLOCK_LIMITS",      0x05},
	{"INQUIRY",                0x12},
	{"RECOVER_BUFFERED_DATA",  0x14},
	{"MODE_SENSE",             0x1a},
	{"START_STOP",             0x1b},
	{"SEND_DIAGNOSTIC",        0x1d},
	{"READ_FORMAT_CAPACITIES", 0x23},
	{"READ_CAPACITY",          0x25},
	{"READ_10",                0x28},
	{"WRITE_10",               0x2a},
	{"PRE_FETCH",              0x34},
	{"SYNCHRONIZE_CACHE",      0x35},
	{"WRITE_BUFFER",           0x3b},
	{"READ_BUFFER",            0x3c},
	{"UNMAP",                  0x42},
	{"MODE_SELECT_10",         0x55},
	{"MODE_SENSE_10",          0x5a},
	{"REPORT_LUNS",            0xa0},
	{"READ_CAPACITY_16",       0x9e},
	{"SECURITY_PROTOCOL_IN",   0xa2},
	{"MAINTENANCE_IN",         0xa3},
	{"MAINTENANCE_OUT",        0xa4},
	{"SECURITY_PROTOCOL_OUT",  0xb5},
	{"UNKNOWN",                0xFF}
};

static int ufs_mtk_get_cmd_str_idx(char cmd)
{
	int i;

	for (i = 0; ufs_mtk_cmd_str_tbl[i].cmd != 0xFF; i++) {
		if (ufs_mtk_cmd_str_tbl[i].cmd == cmd)
			return i;
	}

	return i;
}

void ufs_mtk_dbg_dump_scsi_cmd(struct ufs_hba *hba, struct scsi_cmnd *cmd, u32 flag)
{
	u32 lba = 0;
	u32 blk_cnt;
	u32 fua, flush;

	if (!(flag & hba->ufshcd_dbg_print))
		return;

	if (ufs_mtk_is_data_cmd(cmd->cmnd[0])) {

		lba = cmd->cmnd[5] | (cmd->cmnd[4] << 8) | (cmd->cmnd[3] << 16) | (cmd->cmnd[2] << 24);
		blk_cnt = cmd->cmnd[8] | (cmd->cmnd[7] << 8);
		fua = (cmd->cmnd[1] & 0x8) ? 1 : 0;
		flush = (cmd->request->cmd_flags & REQ_FUA) ? 1 : 0;

#ifdef CONFIG_MTK_HW_FDE
		if (cmd->request->bio && cmd->request->bio->bi_hw_fde) {
			dev_info(hba->dev, "QCMD(C),L:%x,T:%d,0x%x,%s,LBA:%d,BCNT:%d,FUA:%d,FLUSH:%d\n",
				ufshcd_scsi_to_upiu_lun(cmd->device->lun), cmd->request->tag, cmd->cmnd[0],
				ufs_mtk_cmd_str_tbl[ufs_mtk_get_cmd_str_idx(cmd->cmnd[0])].str,
				lba, blk_cnt, fua, flush);

		} else
#endif
		{
			if (hie_request_crypted(cmd->request)) {
				dev_info(hba->dev, "QCMD(H),L:%x,T:%d,0x%x,%s,LBA:%d,BCNT:%d,FUA:%d,FLUSH:%d\n",
					ufshcd_scsi_to_upiu_lun(cmd->device->lun), cmd->request->tag, cmd->cmnd[0],
					ufs_mtk_cmd_str_tbl[ufs_mtk_get_cmd_str_idx(cmd->cmnd[0])].str,
					lba, blk_cnt, fua, flush);
			} else {
				dev_info(hba->dev, "QCMD,L:%x,T:%d,0x%x,%s,LBA:%d,BCNT:%d,FUA:%d,FLUSH:%d\n",
					ufshcd_scsi_to_upiu_lun(cmd->device->lun), cmd->request->tag, cmd->cmnd[0],
					ufs_mtk_cmd_str_tbl[ufs_mtk_get_cmd_str_idx(cmd->cmnd[0])].str,
					lba, blk_cnt, fua, flush);
			}
		}
	} else {
		dev_info(hba->dev, "QCMD,L:%x,T:%d,0x%x,%s\n",
		ufshcd_scsi_to_upiu_lun(cmd->device->lun),
			cmd->request->tag, cmd->cmnd[0],
			ufs_mtk_cmd_str_tbl[ufs_mtk_get_cmd_str_idx(cmd->cmnd[0])].str);
	}
}

static struct ufs_hba_variant_ops ufs_hba_mtk_vops = {
	"mediatek.ufshci",  /* name */
	ufs_mtk_init,    /* init */
	NULL,            /* exit */
	NULL,            /* get_ufs_hci_version */
	NULL,            /* clk_scale_notify */
	NULL,            /* setup_clocks */
	NULL,            /* setup_regulators */
	ufs_mtk_hce_enable_notify,    /* hce_enable_notify */
	ufs_mtk_link_startup_notify,  /* link_startup_notify */
	ufs_mtk_pwr_change_notify,    /* pwr_change_notify */
	ufs_mtk_suspend,              /* suspend */
	ufs_mtk_resume,               /* resume */
	ufs_mtk_dbg_register_dump,    /* dbg_register_dump */
	ufs_mtk_auto_hibern8,         /* auto_hibern8 */
	ufs_mtk_pltfrm_deepidle_resource_req, /* deepidle_resource_req */
	ufs_mtk_pltfrm_deepidle_lock, /* deepidle_lock */
	ufs_mtk_scsi_dev_cfg,         /* scsi_dev_cfg */
};

#ifdef CONFIG_HIE
/* configure request for HIE */
static int ufs_mtk_hie_cfg_request(unsigned int mode, const char *key, int len, struct request *req, void *priv)
{
	struct ufs_crypt_info *info = (struct ufs_crypt_info *)priv;
	struct ufshcd_lrb *lrbp;
	u32 hie_para, i;
	u32 *key_ptr;
	unsigned long flags;
	u32 lba, dunl, dunu;
	struct scsi_cmnd *cmd;
	int need_update = 1;
	int key_idx;

	spin_lock_irqsave(info->hba->host->host_lock, flags);

	/* get key index from key hint, or install new key to key hint */
	key_idx = hie_kh_get_hint(ufs_mtk_hie_get_dev(), key, &need_update);

	spin_unlock_irqrestore(info->hba->host->host_lock, flags);

	if (need_update || (key_idx < 0)) {

		if (key_idx < 0)
			key_idx = 0;

		key_ptr = (u32 *)key;

		hie_para = ((key_idx & 0xFF) << UFS_HIE_PARAM_OFS_CFG_ID) |
					((mode & 0xFF) << UFS_HIE_PARAM_OFS_MODE) |
					((len & 0xFF) << UFS_HIE_PARAM_OFS_KEY_TOTAL_BYTE);

		spin_lock_irqsave(info->hba->host->host_lock, flags);

		/* init ufs crypto IP for HIE and program key by first 8 bytes */
		mt_secure_call(MTK_SIP_KERNEL_CRYPTO_HIE_CFG_REQUEST,
			hie_para, key_ptr[0], key_ptr[1]);

		/* program remaining key */
		for (i = 2; i < len / sizeof(u32); i += 3) {
			mt_secure_call(MTK_SIP_KERNEL_CRYPTO_HIE_CFG_REQUEST,
				key_ptr[i], key_ptr[i + 1], key_ptr[i + 2]);
		}

		spin_unlock_irqrestore(info->hba->host->host_lock, flags);
	}

	cmd = info->cmd;
	lba = ((cmd->cmnd[2]) << 24) | ((cmd->cmnd[3]) << 16) |
			((cmd->cmnd[4]) << 8) | (cmd->cmnd[5]);

	ufs_mtk_crypto_cal_dun(UFS_CRYPTO_ALGO_AES_XTS, lba, &dunl, &dunu);

	/* setup LRB for UTPRD's crypto fields */
	lrbp = &info->hba->lrb[req->tag];
	lrbp->crypto_en = 1;
	lrbp->crypto_cfgid = key_idx;

	/* remember to give "tweak" for AES-XTS */
	lrbp->crypto_dunl = dunl;
	lrbp->crypto_dunu = dunu;

	/* mark data has ever gone through encryption/decryption path */
	info->hba->crypto_feature |= UFS_CRYPTO_HW_FBE_ENCRYPTED;

	return 0;
}

struct hie_dev ufs_hie_dev = {
	.name = "ufs",
	.mode = (BC_AES_256_XTS | BC_AES_128_XTS),
	.encrypt = ufs_mtk_hie_cfg_request,
	.decrypt = ufs_mtk_hie_cfg_request,
	.priv = NULL,
};

struct hie_dev *ufs_mtk_hie_get_dev(void)
{
	return &ufs_hie_dev;
}
#endif

static int ufs_mtk_probe(struct platform_device *pdev)
{
	int err;
	struct ufs_hba *hba;
	struct device *dev = &pdev->dev;

	ufs_mtk_biolog_init();

	/* perform generic probe */
	err = ufshcd_pltfrm_init(pdev, &ufs_hba_mtk_vops);
	if (err) {
		dev_err(dev, "ufshcd_pltfrm_init() failed %d\n", err);
		goto out;
	}

	hba = platform_get_drvdata(pdev);

#ifdef CONFIG_HIE

	hie_register_device(&ufs_hie_dev);

	/*
	 * enable hie key hint feature
	 *
	 * key_bits = 512 bits for all possible FBE crypto algorithms
	 * key_slot = 16 crypto configuration slots
	 */
	hie_kh_register(&ufs_hie_dev, 512, 16);

	hba->crypto_feature |= UFS_CRYPTO_HW_FBE;

#endif

#if defined(CONFIG_MTK_HW_FDE)

	hba->crypto_feature |= UFS_CRYPTO_HW_FDE;

#endif

out:
	return err;
}

static int ufs_mtk_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba =  platform_get_drvdata(pdev);

	pm_runtime_get_sync(&(pdev)->dev);
	ufshcd_remove(hba);
	ufs_mtk_biolog_exit();
	return 0;
}

const struct of_device_id ufs_mtk_of_match[] = {
	{ .compatible = "mediatek,ufshci"},
	{},
};

static const struct dev_pm_ops ufs_mtk_pm_ops = {
	.suspend	= ufshcd_pltfrm_suspend,
	.resume		= ufshcd_pltfrm_resume,
	.runtime_suspend = ufshcd_pltfrm_runtime_suspend,
	.runtime_resume  = ufshcd_pltfrm_runtime_resume,
	.runtime_idle    = ufshcd_pltfrm_runtime_idle,
};

static struct platform_driver ufs_mtk_pltform = {
	.probe	= ufs_mtk_probe,
	.remove	= ufs_mtk_remove,
	.shutdown = ufshcd_pltfrm_shutdown,
	.driver	= {
		.name	= "ufshcd",
		.owner	= THIS_MODULE,
		.pm	= &ufs_mtk_pm_ops,
		.of_match_table = ufs_mtk_of_match,
	},
};

module_platform_driver(ufs_mtk_pltform);
late_initcall_sync(ufs_mtk_cache_init_work);

