
#ifndef _UFS_MTK_H
#define _UFS_MTK_H

#define CONFIG_MTK_UFS_DEBUG
/* #define CONFIG_MTK_UFS_DEBUG_QUEUECMD */
/* #define CONFIG_MTK_UFS_DEGUG_GPIO_TRIGGER */

#include <linux/of.h>
#include <linux/rpmb.h>
#include <linux/hie.h>
#include "ufshcd.h"

#define UPIU_COMMAND_CRYPTO_EN_OFFSET	23

#define UTP_TRANSFER_REQ_TIMEOUT (5 * HZ)   /* TODO: need fine-tune */

/* UFS device quirks */
#define UFS_DEVICE_QUIRK_DELAY_BEFORE_DISABLE_REF_CLK UFS_BIT(29)

#define UFS_DEVICE_QUIRK_LIMITED_RPMB_MAX_RW_SIZE UFS_BIT(30)

#define UFS_RPMB_DEV_MAX_RW_SIZE_LIMITATION (8)

struct ufs_crypto_map {
	unsigned char flag;
	unsigned char key;
};

#define UFS_MAX_LBA ((64 * 1024 * 1024) / 4)

#define UFS_CRYPTO_FLAG_READ          (0x01)
#define UFS_CRYPTO_FLAG_WRITE         (0x02)
#define UFS_CRYPTO_FLAG_UNMAP         (0x04)

#define UFS_CRYPTO_FLAG_NON_ENCRYPTED (0x10)
#define UFS_CRYPTO_FLAG_ENCRYPTED     (0x20)
#define UFS_CRYPTO_FLAG_VALID         (UFS_CRYPTO_FLAG_ENCRYPTED | UFS_CRYPTO_FLAG_NON_ENCRYPTED)

#define UFS_HIE_PARAM_OFS_CFG_ID         (24)
#define UFS_HIE_PARAM_OFS_MODE           (16)
#define UFS_HIE_PARAM_OFS_KEY_TOTAL_BYTE (8)
#define UFS_HIE_PARAM_OFS_KEY_START_BYTE (0)

enum ufs_crypto_type {
	UFS_CRYPTO_NON_ENCRYPTED,
	UFS_CRYPTO_ENCRYPT,
	UFS_CRYPTO_DECRYPT,
};

enum ufs_trace_event {
	UFS_TRACE_SEND,
	UFS_TRACE_COMPLETED,
	UFS_TRACE_DEV_SEND,
	UFS_TRACE_DEV_COMPLETED,
	UFS_TRACE_TM_SEND,
	UFS_TRACE_TM_COMPLETED,
	UFS_TRACE_ABORTING
};

enum {
	UNIPRO_CG_CFG_NATURE        = 0,    /* not force */
	UNIPRO_CG_CFG_FORCE_ENABLE  = 1,
	UNIPRO_CG_CFG_FORCE_DISABLE = 2,
};

enum {
	UFS_CRYPTO_ALGO_AES_XTS             = 0,
	UFS_CRYPTO_ALGO_BITLOCKER_AES_CBC   = 1,
	UFS_CRYPTO_ALGO_AES_ECB             = 2,
	UFS_CRYPTO_ALGO_ESSIV_AES_CBC       = 3,
};

enum {
	UFS_MTK_RESREQ_DMA_OP,      /* request resource for DMA operations, e.g., DRAM */
	UFS_MTK_RESREQ_MPHY_NON_H8  /* request resource for mphy not in H8, e.g., main PLL, 26 mhz clock */
};

enum {
	UFS_H8                      = 0x0,
	UFS_H8_SUSPEND              = 0x1,
};

struct ufs_cmd_str_struct {
	char str[32];
	char cmd;
};

struct ufs_mtk_trace_cmd_hlist_struct {
	enum ufs_trace_event event;
	u8 opcode;
	u8 lun;
	u32 tag;
	u32 transfer_len;
	sector_t lba;
	u64 time;
};

#ifdef MTK_UFS_HQA
#define UFS_CACHED_REGION_CNT (3)
#else
#define UFS_CACHED_REGION_CNT (2)
#endif

struct ufs_cached_region {
	char *name;
	sector_t start_sect;
	sector_t end_sect;
};

/* Hynix device need max 3 seconds to clear fDeviceInit, each fDeviceInit transaction takes */
/* around 1~2ms to get response from UFS. Max fDeviceInit clear time = 5000*(1~2)ms > 3seconds */
#define UFS_FDEVICEINIT_RETRIES    (5000)

#define ASCII_STD true

/* return true if s1 is a prefix of s2 */
#define STR_PRFX_EQUAL(s1, s2) !strncmp(s1, s2, strlen(s1))

#define UFS_ANY_VENDOR 0xFFFF
#define UFS_ANY_MODEL  "ANY_MODEL"

#define MAX_MODEL_LEN 16

#define UFS_VENDOR_TOSHIBA     0x198
#define UFS_VENDOR_SAMSUNG     0x1CE
#define UFS_VENDOR_SKHYNIX     0x1AD

struct ufs_device_info {
	u16 wmanufacturerid;
	char model[MAX_MODEL_LEN + 1];
};

#define UFS_DESCRIPTOR_SIZE (255)

struct ufs_descriptor {
	u8 descriptor_idn;
	u8 index;
	u8 descriptor[UFS_DESCRIPTOR_SIZE];

	u8 *qresp_upiu;
	u32 qresp_upiu_size;
};

struct ufs_dev_fix {
	struct ufs_device_info card;
	unsigned int quirk;
};

union ufs_cpt_cap {
	u32 cap_raw;
	struct {
		u8 cap_cnt;
		u8 cfg_cnt;
		u8 resv;
		u8 cfg_ptr;
	} cap;
};
union ufs_cpt_capx {
	u32 capx_raw;
	struct {
		u8 alg_id;
		u8 du_size;
		u8 key_size;
		u8 resv;
	} capx;
};
union ufs_cap_cfg {
	u32 cfgx_raw[32];
	struct {
		u32 key[16];
		u8 du_size;
		u8 cap_id;
		u16 resv0  : 15;
		u16 cfg_en : 1;
		u8 mu1ti_host;
		u8 resv1;
		u16 vsb;
		u32 resv2[14];
	} cfgx;
};
struct ufs_crypto {
	u32 cfg_id;
	u32 cap_id;
	union ufs_cpt_cap cap;
	union ufs_cpt_capx capx;
	union ufs_cap_cfg cfg;
};

struct ufs_crypt_info {
	struct ufs_hba *hba;
	struct scsi_cmnd *cmd;
};

#define END_FIX { { 0 }, 0 }

/* add specific device quirk */
#define UFS_FIX(_vendor, _model, _quirk) \
	       {                                         \
		       .card.wmanufacturerid = (_vendor),\
		       .card.model = (_model),           \
		       .quirk = (_quirk),                \
	       }

#define UFS_DEVICE_QUIRK_BROKEN_LCC (1 << 0)

#define UFS_DEVICE_NO_VCCQ (1 << 1)

#define UFS_DEVICE_QUIRK_RECOVERY_FROM_DL_NAC_ERRORS (1 << 2)

#define UFS_DEVICE_QUIRK_NO_LINK_OFF   (1 << 3)

#define UFS_DEVICE_QUIRK_PA_TACTIVATE  (1 << 4)

#define UFS_DEVICE_NO_FASTAUTO         (1 << 5)

/* Mediatek specific quirks */

#define UFS_DEVICE_QUIRK_AGGRESIVE_LINKUP    (1 << 31)

#define UFS_DEVICE_QUIRK_INCORRECT_PWM_BURST_CLOSURE_EXTENSION    (1 << 30)

extern u32							ufs_mtk_auto_hibern8_timer_ms;
extern enum ufs_dbg_lvl_t			ufs_mtk_dbg_lvl;
extern struct ufs_hba              *ufs_mtk_hba;
extern bool							ufs_mtk_host_deep_stall_enable;
extern bool							ufs_mtk_host_scramble_enable;
extern bool							ufs_mtk_tr_cn_used;
extern const struct of_device_id			ufs_of_match[];

void             ufs_mtk_add_sysfs_nodes(struct ufs_hba *hba);
void             ufs_mtk_advertise_fixup_device(struct ufs_hba *hba);
int              ufs_mtk_auto_hiber8_quirk_handler(struct ufs_hba *hba, bool enable);
void             ufs_mtk_cache_setup_cmd(struct scsi_cmnd *cmd);
void             ufs_mtk_crypto_cal_dun(u32 alg_id, u32 lba, u32 *dunl, u32 *dunu);
int              ufs_mtk_crypto_sanity_check(struct ufs_hba *hba, struct scsi_cmnd *cmd);
void             ufs_mtk_dbg_dump_scsi_cmd(struct ufs_hba *hba, struct scsi_cmnd *cmd, u32 flag);
int              ufs_mtk_deepidle_hibern8_check(void);
void             ufs_mtk_deepidle_leave(void);
int              ufs_mtk_generic_read_dme(u32 uic_cmd, u16 mib_attribute,
					u16 gen_select_index, u32 *value, unsigned long retry_ms);
void             ufs_mtk_hwfde_cfg_cmd(struct ufs_hba *hba,
					struct scsi_cmnd *cmd);
int              ufs_mtk_linkup_fail_handler(struct ufs_hba *hba, int left_retry);
void             ufs_mtk_parse_auto_hibern8_timer(struct ufs_hba *hba);
void             ufs_mtk_parse_hie(struct ufs_hba *hba);
void             ufs_mtk_parse_pm_levels(struct ufs_hba *hba);
int              ufs_mtk_perf_heurisic_if_allow_cmd(struct ufs_hba *hba, struct scsi_cmnd *cmd);
void             ufs_mtk_perf_heurisic_req_done(struct ufs_hba *hba, struct scsi_cmnd *cmd);
int              ufs_mtk_ioctl_ffu(struct scsi_device *dev, void __user *buf_user);
int              ufs_mtk_ioctl_get_fw_ver(struct scsi_device *dev, void __user *buf_user);
int              ufs_mtk_ioctl_query(struct ufs_hba *hba, u8 lun, void __user *buf_user);
bool             ufs_mtk_is_data_write_cmd(char cmd_op);
void             ufs_mtk_rpmb_dump_frame(struct scsi_device *sdev, u8 *data_frame, u32 cnt);
struct rpmb_dev *ufs_mtk_rpmb_get_raw_dev(void);
void             ufs_mtk_runtime_pm_init(struct scsi_device *sdev);

#ifdef CONFIG_HIE
struct hie_dev  *ufs_mtk_hie_get_dev(void);
#else
static inline
struct hie_dev  *ufs_mtk_hie_get_dev(void)
{
	return NULL;
}
#endif

#endif /* !_UFS_MTK_H */

