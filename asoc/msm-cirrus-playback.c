/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/firmware.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/compat.h>
#include <linux/module.h>

#include <asoc/msm-cirrus-playback.h>

#define CIRRUS_SP_CONFIG_MAX_LEN_FILENAME 256

static atomic_t crus_sp_misc_usage_count;

static struct crus_single_data_t crus_enable;
static struct crus_sp_ioctl_header crus_sp_hdr;
static struct cirrus_cal_result_t crus_sp_cal_rslt;
static int32_t *crus_sp_get_buffer;
static atomic_t crus_sp_get_param_flag;
struct mutex crus_sp_get_param_lock;
struct mutex crus_sp_lock;
static int cirrus_sp_en;
static int cirrus_sp_case_ctrl;
static int cirrus_fb_port_ctl;
static int cirrus_fb_ext_sel;
static int cirrus_fb_port = AFE_PORT_ID_QUATERNARY_TDM_TX;
static int cirrus_ff_port = AFE_PORT_ID_QUATERNARY_TDM_RX;

static void *crus_gen_afe_get_header(int length, int port, int module,
				     int param)
{
	struct afe_custom_crus_get_config_t *config = NULL;
	int size = sizeof(struct afe_custom_crus_get_config_t);
	int index = afe_get_port_index(port);
	u16 payload_size = sizeof(struct afe_port_param_data_v2) + length;

	/* Allocate memory for the message */
	config = kzalloc(size, GFP_KERNEL);
	if (!config)
		return NULL;

	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					      APR_HDR_LEN(APR_HDR_SIZE),
					      APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_GET_PARAM_V2;

	/* Set param section */
	config->param.port_id = (uint16_t) port;
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;
	config->param.module_id = (uint32_t) module;
	config->param.param_id = (uint32_t) param;
	/* max data size of the param_ID/module_ID combination */
	config->param.payload_size = payload_size;

	/* Set data section */
	config->data.module_id = (uint32_t) module;
	config->data.param_id = (uint32_t) param;
	config->data.reserved = 0; /* Must be set to 0 */
	/* actual size of the data for the module_ID/param_ID pair */
	config->data.param_size = length;

	return (void *)config;
}

static void *crus_gen_afe_set_header(int length, int port, int module,
				     int param)
{
	struct afe_custom_crus_set_config_t *config = NULL;
	int size = sizeof(struct afe_custom_crus_set_config_t) + length;
	int index = afe_get_port_index(port);
	u16 payload_size = sizeof(struct afe_port_param_data_v2) + length;

	/* Allocate memory for the message */
	config = kzalloc(size, GFP_KERNEL);
	if (!config)
		return NULL;

	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					      APR_HDR_LEN(APR_HDR_SIZE),
					      APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_SET_PARAM_V2;

	/* Set param section */
	config->param.port_id = (uint16_t)port;
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;
	/* max data size of the param_ID/module_ID combination */
	config->param.payload_size = payload_size;

	/* Set data section */
	config->data.module_id = (uint32_t)module;
	config->data.param_id = (uint32_t)param;
	config->data.reserved = 0;	/* Must be set to 0 */
	/* actual size of the data for the module_ID/param_ID pair */
	config->data.param_size = length;

	return (void *)config;
}

static int crus_afe_get_param(int port, int module, int param, int length,
			      void *data)
{
	struct afe_custom_crus_get_config_t *config = NULL;
	int index = afe_get_port_index(port);
	int ret = 0;

	pr_info("%s: port = %d module = %d param = 0x%x length = %d\n",
		__func__, port, module, param, length);

	config = (struct afe_custom_crus_get_config_t *)
		 crus_gen_afe_get_header(length, port, module, param);
	if (!config) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}

	pr_info("%s: Preparing to send apr packet\n", __func__);

	mutex_lock(&crus_sp_get_param_lock);
	atomic_set(&crus_sp_get_param_flag, 0);
	crus_sp_get_buffer = kzalloc(config->param.payload_size + 16,
				     GFP_KERNEL);

	ret = afe_apr_send_pkt_crus(config, index, 0);

	if (ret)
		pr_err("%s: crus get_param for port %d failed with code %d\n",
		       __func__, port, ret);
	else
		pr_info("%s: crus get_param sent packet with param id 0x%08x to module 0x%08x.\n",
			__func__, param, module);

	/* Wait for afe callback to populate data */
	while (!atomic_read(&crus_sp_get_param_flag))
		usleep_range(1000, 2000);

	/* Copy from dynamic buffer to return buffer */
	memcpy((u8 *) data, &crus_sp_get_buffer[4], length);

	kfree(crus_sp_get_buffer);
	mutex_unlock(&crus_sp_get_param_lock);

	kfree(config);
	return ret;
}

static int crus_afe_set_param(int port, int module, int param, int length,
			      void *data_ptr)
{
	struct afe_custom_crus_set_config_t *config = NULL;

	int index = afe_get_port_index(port);
	int ret = 0;

	pr_info("%s: port = %d module = %d param = 0x%x length = %d\n",
		__func__, port, module, param, length);

	config = crus_gen_afe_set_header(length, port, module, param);
	if (!config) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}

	memcpy((u8 *)config + sizeof(struct afe_custom_crus_set_config_t),
	       (u8 *)data_ptr, length);

	pr_debug("%s: Preparing to send apr packet.\n", __func__);

	ret = afe_apr_send_pkt_crus(config, index, 1);
	if (ret) {
		pr_err("%s: crus set_param for port %d failed with code %d\n",
		       __func__, port, ret);
	} else {
		pr_debug("%s: crus set_param sent packet with param id 0x%08x to module 0x%08x.\n",
			 __func__, param, module);
	}

	kfree(config);
	return ret;
}

static int crus_afe_send_config(const char *data, int32_t length,
				s32 port, s32 module)
{
	struct afe_custom_crus_set_config_t *config = NULL;
	struct crus_external_config_t *payload = NULL;
	int size = sizeof(struct crus_external_config_t);
	int ret = 0;
	int index = afe_get_port_index(port);
	u32 param = 0;
	int mem_size = 0;
	int sent = 0;
	int chars_to_send = 0;

	pr_info("%s: called with module_id = %x, string length = %d\n",
		__func__, module, length);

	/* Destination settings for message */
	if (port == cirrus_ff_port) {
		param = CRUS_PARAM_RX_SET_EXT_CONFIG;
	} else if (port == cirrus_fb_port) {
		param = CRUS_PARAM_TX_SET_EXT_CONFIG;
	} else {
		pr_err("%s: Received invalid port parameter %d\n",
		       __func__, module);
		return -EINVAL;
	}

	if (length > APR_CHUNK_SIZE)
		mem_size = APR_CHUNK_SIZE;
	else
		mem_size = length;

	config = crus_gen_afe_set_header(size, port, module, param);
	if (!config) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}

	payload = (struct crus_external_config_t *)((u8 *)config +
			sizeof(struct afe_custom_crus_set_config_t));
	payload->total_size = (u32)length;
	payload->reserved = 0;
	payload->config = PAYLOAD_FOLLOWS_CONFIG;
	    /* ^ This tells the algorithm to expect array */
	    /*   immediately following the header */

	/* Send config string in chunks of APR_CHUNK_SIZE bytes */
	while (sent < length) {
		chars_to_send = length - sent;
		if (chars_to_send > APR_CHUNK_SIZE) {
			chars_to_send = APR_CHUNK_SIZE;
			payload->done = 0;
		} else {
			payload->done = 1;
		}

		/* Configure per message parameter settings */
		memcpy(payload->data, data + sent, chars_to_send);
		payload->chunk_size = chars_to_send;

		/* Send the actual message */
		pr_debug("%s: Preparing to send apr packet.\n", __func__);
		ret = afe_apr_send_pkt_crus(config, index, 1);

		if (ret)
			pr_err("%s: crus set_param for port %d failed with code %d\n",
			       __func__, port, ret);
		else
			pr_debug("%s: crus set_param sent packet with param id 0x%08x to module 0x%08x.\n",
				 __func__, param, module);

		sent += chars_to_send;
	}

	kfree(config);
	return ret;
}

int crus_afe_callback(void *payload, int size)
{
	uint32_t *payload32 = payload;

	pr_debug("Cirrus AFE CALLBACK: size = %d\n", size);

	switch (payload32[1]) {
	case CIRRUS_SP:
		memcpy(crus_sp_get_buffer, payload32, size);
		atomic_set(&crus_sp_get_param_flag, 1);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int msm_routing_cirrus_fbport_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: cirrus_fb_port_ctl = %d", __func__, cirrus_fb_port_ctl);
	ucontrol->value.integer.value[0] = cirrus_fb_port_ctl;
	return 0;
}

int msm_routing_cirrus_fbport_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	cirrus_fb_port_ctl = ucontrol->value.integer.value[0];

	switch (cirrus_fb_port_ctl) {
	case 0:
		cirrus_fb_port = AFE_PORT_ID_PRIMARY_MI2S_TX;
		cirrus_ff_port = AFE_PORT_ID_PRIMARY_MI2S_RX;
		break;
	case 1:
		cirrus_fb_port = AFE_PORT_ID_SECONDARY_MI2S_TX;
		cirrus_ff_port = AFE_PORT_ID_SECONDARY_MI2S_RX;
		break;
	case 2:
		cirrus_fb_port = AFE_PORT_ID_TERTIARY_MI2S_TX;
		cirrus_ff_port = AFE_PORT_ID_TERTIARY_MI2S_RX;
		break;
	case 3:
		cirrus_fb_port = AFE_PORT_ID_QUATERNARY_MI2S_TX;
		cirrus_ff_port = AFE_PORT_ID_QUATERNARY_MI2S_RX;
		break;
	case 4:
		cirrus_fb_port = AFE_PORT_ID_PRIMARY_TDM_TX;
		cirrus_ff_port = AFE_PORT_ID_PRIMARY_TDM_RX;
		break;
	case 5:
		cirrus_fb_port = AFE_PORT_ID_SECONDARY_TDM_TX;
		cirrus_ff_port = AFE_PORT_ID_SECONDARY_TDM_RX;
		break;
	case 6:
		cirrus_fb_port = AFE_PORT_ID_TERTIARY_TDM_TX;
		cirrus_ff_port = AFE_PORT_ID_TERTIARY_TDM_RX;
		break;
	case 7:
		cirrus_fb_port = AFE_PORT_ID_QUATERNARY_TDM_TX;
		cirrus_ff_port = AFE_PORT_ID_QUATERNARY_TDM_RX;
		break;
	default:
		/* Default port to QUATERNARY */
		cirrus_fb_port_ctl = 7;
		cirrus_fb_port = AFE_PORT_ID_QUATERNARY_TDM_TX;
		cirrus_ff_port = AFE_PORT_ID_QUATERNARY_TDM_RX;
		break;
	}
	return 0;
}

static int msm_routing_crus_sp_enable_put(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	const int crus_set = ucontrol->value.integer.value[0];

	if (crus_set > 255) {
		pr_err("Cirrus SP Enable: Invalid entry; Enter 0 to DISABLE,");
		pr_err(" 1 to ENABLE; 2-255 are reserved for debug\n");
		return -EINVAL;
	}

	switch (crus_set) {
	case 0:		/* "Config SP Disable" */
		pr_info("Cirrus SP: Config DISABLE\n");
		crus_enable.value = 0;
		cirrus_sp_en = 0;
		break;
	case 1:		/* "Config SP Enable" */
		pr_info("Cirrus SP: Config ENABLE\n");
		crus_enable.value = 1;
		cirrus_sp_en = 1;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&crus_sp_lock);
	crus_afe_set_param(cirrus_ff_port, CIRRUS_SP,
			   CIRRUS_SP_ENABLE, sizeof(struct crus_single_data_t),
			   (void *)&crus_enable);
	mutex_unlock(&crus_sp_lock);

	mutex_lock(&crus_sp_lock);
	crus_afe_set_param(cirrus_fb_port, CIRRUS_SP,
			   CIRRUS_SP_ENABLE, sizeof(struct crus_single_data_t),
			   (void *)&crus_enable);
	mutex_unlock(&crus_sp_lock);

	return 0;
}

static int msm_routing_crus_sp_enable_get(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	pr_info("Starting Cirrus SP Enable Get function call : %d\n",
		cirrus_sp_en);
	ucontrol->value.integer.value[0] = cirrus_sp_en;
	return 0;
}

static int msm_routing_crus_sp_usecase_put(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct crus_rx_run_case_ctrl_t case_ctrl;
	const int crus_set = ucontrol->value.integer.value[0];
	uint32_t option = 1;

	pr_debug("Starting Cirrus SP Config function call %d\n", crus_set);

	case_ctrl.status_l = 1;
	case_ctrl.status_r = 1;
	case_ctrl.z_l = crus_sp_cal_rslt.z_l;
	case_ctrl.z_r = crus_sp_cal_rslt.z_r;
	case_ctrl.checksum_l = crus_sp_cal_rslt.z_l + 1;
	case_ctrl.checksum_r = crus_sp_cal_rslt.z_r + 1;
	if (crus_sp_cal_rslt.atemp == 0)
		case_ctrl.atemp = 23;
	else
		case_ctrl.atemp = crus_sp_cal_rslt.atemp;
	pr_err("%s: atemp %d\n", __func__, case_ctrl.atemp);

	switch (crus_set) {
	case 0:		/* Playback */
		option = 0;
		case_ctrl.value = 0;
		cirrus_sp_case_ctrl = 0;
		pr_debug("Cirrus SP Config: Music Config\n");
		crus_afe_set_param(cirrus_fb_port, CIRRUS_SP,
				   CRUS_PARAM_TX_SET_USECASE, sizeof(option),
				   (void *)&option);
		crus_afe_set_param(cirrus_ff_port, CIRRUS_SP,
				   CRUS_PARAM_RX_SET_USECASE, sizeof(case_ctrl),
				   (void *)&case_ctrl);
		break;
	case 1:		/* Voice */
		option = 1;
		case_ctrl.value = 1;
		cirrus_sp_case_ctrl = 1;
		pr_debug("Cirrus SP Config: Voice Config\n");
		crus_afe_set_param(cirrus_fb_port, CIRRUS_SP,
				   CRUS_PARAM_TX_SET_USECASE, sizeof(option),
				   (void *)&option);
		crus_afe_set_param(cirrus_ff_port, CIRRUS_SP,
				   CRUS_PARAM_RX_SET_USECASE, sizeof(case_ctrl),
				   (void *)&case_ctrl);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int msm_routing_crus_sp_usecase_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("Starting Cirrus SP Config Get function call: %d\n",
		 cirrus_sp_case_ctrl);
	ucontrol->value.integer.value[0] = cirrus_sp_case_ctrl;
	return 0;
}

static int msm_routing_crus_ext_config(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	const int crus_set = ucontrol->value.integer.value[0];
	int length = 0, rc = 0;
	char *input = NULL;
	char *filename;
	const struct firmware *firmware;
	struct snd_soc_platform *platform = snd_soc_kcontrol_platform(kcontrol);
	struct msm_pcm_drv_data *pdata = snd_soc_platform_get_drvdata(platform);

	filename = kzalloc(CIRRUS_SP_CONFIG_MAX_LEN_FILENAME, GFP_KERNEL);
	if (!filename) {
		rc = -ENOMEM;
		goto out;
	}

	snprintf(filename, CIRRUS_SP_CONFIG_MAX_LEN_FILENAME,
		 "crus_sp_config_%s", pdata->config_name);

	pr_debug("Starting Cirrus SP EXT Config function call %d\n", crus_set);

	switch (crus_set) {
	case 0:		/* "Config RX Default" */
		strlcat(filename, "_rx.bin", CIRRUS_SP_CONFIG_MAX_LEN_FILENAME);
		if (request_firmware(&firmware, filename, platform->dev) != 0) {
			pr_err("%s: Request firmware failed\n", __func__);
		} else {
			length = firmware->size;
			input = kzalloc(length, GFP_KERNEL);
			pr_debug("%s: length = %d; dataptr = %lx\n", __func__,
				 length, (unsigned long)firmware->data);
			memcpy(input, firmware->data, length);
			pr_info("Cirrus SP EXT Config: Sending RX config\n");
			crus_afe_send_config(input, length, cirrus_ff_port,
					     CIRRUS_SP);
			cirrus_fb_ext_sel = crus_set;
			kfree(input);
		}
		release_firmware(firmware);
		break;
	case 1:		/* "Config TX Default" */
		strlcat(filename, "_tx.bin", CIRRUS_SP_CONFIG_MAX_LEN_FILENAME);
		if (request_firmware(&firmware, filename, platform->dev) != 0) {
			pr_err("%s: Request firmware failed\n", __func__);
		} else {
			length = firmware->size;
			input = kzalloc(length, GFP_KERNEL);
			pr_debug("%s: length = %d; dataptr = %lx\n", __func__,
				 length, (unsigned long)firmware->data);
			memcpy(input, firmware->data, length);
			pr_info("Cirrus SP EXT Config: Sending TX config\n");
			crus_afe_send_config(input, length, cirrus_fb_port,
					     CIRRUS_SP);
			cirrus_fb_ext_sel = crus_set;
			kfree(input);
		}
		release_firmware(firmware);
		break;
	default:
		rc = -EINVAL;
		break;
	}

out:
	kfree(filename);
	return rc;
}

static int msm_routing_crus_ext_config_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("Starting Cirrus SP EXT Config Get function call\n");
	ucontrol->value.integer.value[0] = cirrus_fb_ext_sel;

	return 0;
}

static const char * const cirrus_fb_port_text[] = {
	"PRI_MI2S_RX", "SEC_MI2S_RX",
	"TERT_MI2S_RX", "QUAT_MI2S_RX",
	"PRI_TDM_TX_0", "SEC_TDM_TX_0",
	"TERT_TDM_TX_0", "QUAT_TDM_TX_0"
};

static const char * const crus_en_text[] = { "Disable", "Enable" };

static const char * const crus_sp_usecase_text[] = { "Music", "Voice" };

static const char * const crus_ext_text[] = { "Config RX", "Config TX" };

static const struct soc_enum cirrus_fb_controls_enum[] = {
	SOC_ENUM_SINGLE_EXT(8, cirrus_fb_port_text),
};

static const struct soc_enum crus_en_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, crus_en_text),
};

static const struct soc_enum crus_sp_usecase_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, crus_sp_usecase_text),
};

static const struct soc_enum crus_ext_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, crus_ext_text),
};

static const struct snd_kcontrol_new crus_mixer_controls[] = {
	SOC_ENUM_EXT("Cirrus SP FBPort", cirrus_fb_controls_enum[0],
		     msm_routing_cirrus_fbport_get,
		     msm_routing_cirrus_fbport_put),
	SOC_ENUM_EXT("Cirrus SP", crus_en_enum[0],
		     msm_routing_crus_sp_enable_get,
		     msm_routing_crus_sp_enable_put),
	SOC_ENUM_EXT("Cirrus SP Usecase Config", crus_sp_usecase_enum[0],
		     msm_routing_crus_sp_usecase_get,
		     msm_routing_crus_sp_usecase_put),
	SOC_ENUM_EXT("Cirrus SP EXT Config", crus_ext_enum[0],
		     msm_routing_crus_ext_config_get,
		     msm_routing_crus_ext_config),
};

void msm_crus_pb_add_controls(struct snd_soc_platform *platform)
{
	snd_soc_add_platform_controls(platform, crus_mixer_controls,
				      ARRAY_SIZE(crus_mixer_controls));
}

static long crus_sp_shared_ioctl(struct file *f, unsigned int cmd,
				 void __user *arg)
{
	int result = 0, port;
	uint32_t bufsize = 0, size;
	uint32_t option = 0;
	void *io_data = NULL;

	pr_info("%s\n", __func__);

	if (copy_from_user(&size, (void *)arg, sizeof(size))) {
		pr_err("%s: copy_from_user (size) failed\n", __func__);
		result = -EFAULT;
		goto exit;
	}

	/* Copy IOCTL header from usermode */
	if (copy_from_user(&crus_sp_hdr, (void *)arg, size)) {
		pr_err("%s: copy_from_user (struct) failed\n", __func__);
		result = -EFAULT;
		goto exit;
	}

	bufsize = crus_sp_hdr.data_length;
	io_data = kzalloc(bufsize, GFP_KERNEL);

	switch (cmd) {
	case CRUS_SP_IOCTL_GET:
		switch (crus_sp_hdr.module_id) {
		case CRUS_MODULE_ID_TX:
			port = cirrus_fb_port;
			break;
		case CRUS_MODULE_ID_RX:
			port = cirrus_ff_port;
			break;
		default:
			port = cirrus_ff_port;
		}

		crus_afe_get_param(port, CIRRUS_SP, crus_sp_hdr.param_id,
				   bufsize, io_data);

		result = copy_to_user(crus_sp_hdr.data, io_data, bufsize);
		if (result) {
			pr_err("%s: copy_to_user failed (%d)\n", __func__,
			       result);
			result = -EFAULT;
		} else {
			result = bufsize;
		}
		break;
	case CRUS_SP_IOCTL_SET:
		result = copy_from_user(io_data, (void *)crus_sp_hdr.data,
					bufsize);
		if (result) {
			pr_err("%s: copy_from_user failed (%d)\n", __func__,
			       result);
			result = -EFAULT;
			goto exit_io;
		}

		switch (crus_sp_hdr.module_id) {
		case CRUS_MODULE_ID_TX:
			port = cirrus_fb_port;
			break;
		case CRUS_MODULE_ID_RX:
			port = cirrus_ff_port;
			break;
		default:
			port = cirrus_ff_port;
		}

		crus_afe_set_param(port, CIRRUS_SP, crus_sp_hdr.param_id,
				   bufsize, io_data);
		break;
	case CRUS_SP_IOCTL_GET_CALIB:
		if (copy_from_user(io_data,
				   (void *)crus_sp_hdr.data, bufsize)) {
			pr_err("%s: copy_from_user failed\n", __func__);
			result = -EFAULT;
			goto exit_io;
		}
		option = 1;
		crus_afe_set_param(cirrus_ff_port, CIRRUS_SP,
				   CRUS_PARAM_RX_SET_CALIB, sizeof(option),
				   (void *)&option);
		crus_afe_set_param(cirrus_fb_port, CIRRUS_SP,
				   CRUS_PARAM_TX_SET_CALIB, sizeof(option),
				   (void *)&option);
		msleep(2000);
		crus_afe_get_param(cirrus_fb_port, CIRRUS_SP,
				   CRUS_PARAM_TX_GET_TEMP_CAL, bufsize,
				   io_data);
		if (copy_to_user(crus_sp_hdr.data, io_data, bufsize)) {
			pr_err("%s: copy_to_user failed\n", __func__);
			result = -EFAULT;
		} else {
			result = bufsize;
		}

		break;
	case CRUS_SP_IOCTL_SET_CALIB:
		if (copy_from_user(io_data,
				   (void *)crus_sp_hdr.data, bufsize)) {
			pr_err("%s: copy_from_user failed\n", __func__);
			result = -EFAULT;
			goto exit_io;
		}
		memcpy(&crus_sp_cal_rslt, io_data, bufsize);
		break;
	default:
		pr_err("%s: Invalid IOCTL, command = %d!\n", __func__, cmd);
		result = -EINVAL;
	}

exit_io:
	kfree(io_data);
exit:
	return result;
}

static long crus_sp_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	pr_info("%s\n", __func__);

	return crus_sp_shared_ioctl(f, cmd, (void __user *)arg);
}

static long crus_sp_compat_ioctl(struct file *f,
				 unsigned int cmd, unsigned long arg)
{
	unsigned int cmd64;

	pr_info("%s\n", __func__);

	switch (cmd) {
	case CRUS_SP_IOCTL_GET32:
		cmd64 = CRUS_SP_IOCTL_GET;
		break;
	case CRUS_SP_IOCTL_SET32:
		cmd64 = CRUS_SP_IOCTL_SET;
		break;
	case CRUS_SP_IOCTL_GET_CALIB32:
		cmd64 = CRUS_SP_IOCTL_GET_CALIB;
		break;
	case CRUS_SP_IOCTL_SET_CALIB32:
		cmd64 = CRUS_SP_IOCTL_SET_CALIB;
		break;
	default:
		pr_err("%s: Invalid IOCTL, command = %d!\n", __func__, cmd);
		return -EINVAL;
	}

	return crus_sp_shared_ioctl(f, cmd64, compat_ptr(arg));
}

static int crus_sp_open(struct inode *inode, struct file *f)
{
	pr_info("%s\n", __func__);

	atomic_inc(&crus_sp_misc_usage_count);
	return 0;
}

static int crus_sp_release(struct inode *inode, struct file *f)
{
	pr_debug("%s\n", __func__);

	atomic_dec(&crus_sp_misc_usage_count);
	pr_debug("%s: ref count %d!\n", __func__,
		 atomic_read(&crus_sp_misc_usage_count));

	return 0;
}

static ssize_t
temperature_left_show(struct device *dev, struct device_attribute *a, char *buf)
{
	static const int material = 250;
	static const int scale_factor = 100000;
	int buffer[96];
	int out_cal0;
	int out_cal1;
	int z, r, t;
	int temp0;

	crus_afe_get_param(cirrus_ff_port, CIRRUS_SP, CRUS_PARAM_RX_GET_TEMP,
			   384, buffer);

	out_cal0 = buffer[12];
	out_cal1 = buffer[13];

	z = buffer[4];

	temp0 = buffer[10];

	if ((out_cal0 != 2) || (out_cal1 != 2))
		return sprintf(buf, "Calibration is not done\n");

	r = buffer[3];
	t = (material * scale_factor * (r - z) / z) + (temp0 * scale_factor);

	return sprintf(buf, "%d.%05dc\n", t / scale_factor, t % scale_factor);
}

static DEVICE_ATTR_RO(temperature_left);

static ssize_t
temperature_right_show(struct device *dev, struct device_attribute *a,
		       char *buf)
{
	static const int material = 250;
	static const int scale_factor = 100000;
	int buffer[96];
	int out_cal0;
	int out_cal1;
	int z, r, t;
	int temp0;

	crus_afe_get_param(cirrus_ff_port, CIRRUS_SP, CRUS_PARAM_RX_GET_TEMP,
			   384, buffer);

	out_cal0 = buffer[14];
	out_cal1 = buffer[15];

	z = buffer[2];

	temp0 = buffer[10];

	if ((out_cal0 != 2) || (out_cal1 != 2))
		return sprintf(buf, "Calibration is not done\n");

	r = buffer[1];
	t = (material * scale_factor * (r - z) / z) + (temp0 * scale_factor);

	return sprintf(buf, "%d.%05dc\n", t / scale_factor, t % scale_factor);
}

static DEVICE_ATTR_RO(temperature_right);

static ssize_t
resistance_left_show(struct device *dev, struct device_attribute *a, char *buf)
{
	static const int scale_factor = 100000000;
	static const int amp_factor = 71498;
	int buffer[96];
	int out_cal0;
	int out_cal1;
	int r;

	crus_afe_get_param(cirrus_ff_port, CIRRUS_SP, CRUS_PARAM_RX_GET_TEMP,
			   384, buffer);

	out_cal0 = buffer[12];
	out_cal1 = buffer[13];

	if ((out_cal0 != 2) || (out_cal1 != 2))
		return sprintf(buf, "Calibration is not done\n");

	r = buffer[3] * amp_factor;

	return sprintf(buf, "%d.%08d ohms\n", r / scale_factor,
		       r % scale_factor);
}

static DEVICE_ATTR_RO(resistance_left);

static ssize_t
resistance_right_show(struct device *dev, struct device_attribute *a, char *buf)
{
	static const int scale_factor = 100000000;
	static const int amp_factor = 71498;
	int buffer[96];
	int out_cal0;
	int out_cal1;
	int r;

	crus_afe_get_param(cirrus_ff_port, CIRRUS_SP, CRUS_PARAM_RX_GET_TEMP,
			   384, buffer);

	out_cal0 = buffer[14];
	out_cal1 = buffer[15];

	if ((out_cal0 != 2) || (out_cal1 != 2))
		return sprintf(buf, "Calibration is not done\n");

	r = buffer[1] * amp_factor;

	return sprintf(buf, "%d.%08d ohms\n", r / scale_factor,
		       r % scale_factor);
}

static DEVICE_ATTR_RO(resistance_right);

static struct attribute *crus_sp_attrs[] = {
	&dev_attr_temperature_left.attr,
	&dev_attr_temperature_right.attr,
	&dev_attr_resistance_left.attr,
	&dev_attr_resistance_right.attr,
	NULL,
};

static const struct attribute_group crus_sp_group = {
	.attrs = crus_sp_attrs,
};

static const struct attribute_group *crus_sp_groups[] = {
	&crus_sp_group,
	NULL,
};

static const struct file_operations crus_sp_fops = {
	.owner = THIS_MODULE,
	.open = crus_sp_open,
	.release = crus_sp_release,
	.unlocked_ioctl = crus_sp_ioctl,
	.compat_ioctl = crus_sp_compat_ioctl,
};

struct miscdevice crus_sp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "msm_cirrus_playback",
	.fops = &crus_sp_fops,
};

static int __init crus_sp_init(void)
{
	int ret;

	pr_info("CRUS_SP_INIT: initializing misc device\n");
	atomic_set(&crus_sp_get_param_flag, 0);
	atomic_set(&crus_sp_misc_usage_count, 0);
	mutex_init(&crus_sp_get_param_lock);
	mutex_init(&crus_sp_lock);

	ret = misc_register(&crus_sp_misc);
	if (ret) {
		pr_err("Failed to register misc device\n");
		return ret;
	}

	ret = sysfs_create_groups(&crus_sp_misc.this_device->kobj,
				  crus_sp_groups);
	if (ret) {
		pr_err("%s: Could not create sysfs groups\n", __func__);
		return ret;
	}

	return 0;
}

module_init(crus_sp_init);
