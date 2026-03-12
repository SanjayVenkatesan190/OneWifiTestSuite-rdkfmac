/* Modifications Copyright 2025 Comcast Cable Communications Management, LLC
 * Licensed under the GPLv2.0 License
 */

#include "rdkfmac.h"

struct rdkfmac_device_data g_char_device;
static DECLARE_WAIT_QUEUE_HEAD(rdkfmac_rq); 
static wlan_emu_msg_data_t *pop_from_char_device(void);
// static unsigned int get_list_entries_count_in_char_device(void);
static bool  rdkfmac_emu80211_close = true;

const char *rdkfmac_cfg80211_ops_type_to_string(wlan_emu_cfg80211_ops_type_t type)
{
#define CFG80211_TO_S(x) case x: return #x;
	switch (type) {
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_none)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_add_intf)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_del_intf)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_change_intf)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_start_ap)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_change_beacon)
		CFG80211_TO_S(wlan_emu_cfg80211_ops_type_stop_ap)
		default:
			break;
	}

	return "wlan_emu_cfg80211_ops_type_unknown";
}

const char *rdkfmac_mac80211_ops_type_to_string(wlan_emu_mac80211_ops_type_t type)
{
#define MAC80211_TO_S(x) case x: return #x;
	switch (type) {
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_none)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_tx)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_start)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_stop)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_add_intf)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_change_intf)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_remove_intf)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_config)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_bss_info_changed)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_start_ap)
		MAC80211_TO_S(wlan_emu_mac80211_ops_type_stop_ap)
		default:
			break;
	}

	return "wlan_emu_mac80211_ops_type_unknown";
}

const char *rdkfmac_emu80211_ops_type_to_string(wlan_emu_emu80211_ops_type_t type)
{
#define EMU80211_TO_S(x) case x: return #x;
	switch (type) {
		EMU80211_TO_S(wlan_emu_emu80211_ops_type_none)
		EMU80211_TO_S(wlan_emu_emu80211_ops_type_tctrl)
		EMU80211_TO_S(wlan_emu_emu80211_ops_type_close)
		default:
			break;
	}

	return "wlan_emu_emu80211_ops_type_unknown";
}

static __poll_t rdkfmac_poll(struct file *filp, struct poll_table_struct *wait)
{
	
    __poll_t mask = 0;
    unsigned long flags;
    printk("SJY Entering %s:%d\n", __func__, __LINE__);
    poll_wait(filp, &rdkfmac_rq, wait);

    spin_lock_irqsave(&g_char_device.lock, flags);
	printk("%s:%d SJY checking the list is empty or not\n", __func__, __LINE__);
    if (g_char_device.list_tail != &g_char_device.list_head) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    spin_unlock_irqrestore(&g_char_device.lock, flags);
    printk("SJY %s:%d: Exiting poll and returning mask : %u\n", __func__, __LINE__, mask);
    return mask;
}

void push_to_char_device(wlan_emu_msg_data_t *data)
{
    wlan_emu_msg_data_entry_t *entry;
    wlan_emu_msg_data_t *spec;
    unsigned long flags;
	char	str_spec_type[32] = {0};
	char	str_ops[128] = {0};

    printk("SJY Entering %s:%d\n", __func__, __LINE__);

    // 1. Pre-allocate memory (can sleep, so do it OUTSIDE the lock)
    entry = kmalloc(sizeof(wlan_emu_msg_data_entry_t), GFP_KERNEL);
    if (!entry){
		printk("%s:%d SJY kmalloc failed for entry\n", __func__, __LINE__);
		return;
	}

    spec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (!spec) {
		printk("%s:%d SJY kmalloc failed for spec\n", __func__, __LINE__);
        kfree(entry);
        return;
    }

    entry->spec = spec;
	printk("%s:%d: Memcpying data to spec, size of wlan_emu_msg_data_t is %zu\n", __func__, __LINE__, sizeof(wlan_emu_msg_data_t));
    memcpy(spec, data, sizeof(wlan_emu_msg_data_t));

    // 2. Lock and Update
	printk("SJY %s:%d: Acquiring lock to push data to char device\n", __func__, __LINE__);
    spin_lock_irqsave(&g_char_device.lock, flags);

	if (g_char_device.num_inst == 0 || rdkfmac_emu80211_close == true) {
		printk("%s:%d SJY Not pushing to char device, num_inst: %d, rdkfmac_emu80211_close: %d\n", __func__, __LINE__, g_char_device.num_inst, rdkfmac_emu80211_close);
		spin_unlock_irqrestore(&g_char_device.lock, flags);
		entry->spec = NULL;
		kfree(entry);
		kfree(spec);
        return;
    }
    printk("SJY %s:%d: the spec type is %d\n", __func__, __LINE__, spec->type);
		switch (spec->type) {
		case wlan_emu_msg_type_cfg80211:
			strcpy(str_spec_type, "cfg80211");
			strcpy(str_ops, rdkfmac_cfg80211_ops_type_to_string(spec->u.cfg80211.ops));
			break;

		case wlan_emu_msg_type_mac80211:
			strcpy(str_spec_type, "mac80211");
			strcpy(str_ops, rdkfmac_mac80211_ops_type_to_string(spec->u.mac80211.ops));
			break;

		case wlan_emu_msg_type_emu80211:
			strcpy(str_spec_type, "emu80211");
			strcpy(str_ops, rdkfmac_emu80211_ops_type_to_string(spec->u.emu80211.ops));
			break;

		case wlan_emu_msg_type_webconfig:
			strcpy(str_spec_type, "webconfig");
			strcpy(str_ops, "onewifi_webconfig");
			break;

		default:
			break;
	}

	if ((spec->type != wlan_emu_msg_type_webconfig) && (spec->type != wlan_emu_msg_type_frm80211)) {
		printk("SJY %s:%d: pushing data to queue, type: %s ops: %s\n", __func__, __LINE__,
			str_spec_type, str_ops);
	}

    // Link node to the current tail
	printk("SJY %s:%d: Adding the new entry to the list\n", __func__, __LINE__);
    list_add(&entry->list_entry, g_char_device.list_tail);
    // Update the tail pointer to the new node
    g_char_device.list_tail = &entry->list_entry;
	printk("SJY %s:%d: Updated tail pointer\n", __func__, __LINE__);

    spin_unlock_irqrestore(&g_char_device.lock, flags);
    printk("SJY %s:%d: Released lock after pushing data to char device and calling interrupt\n", __func__, __LINE__);
    // 3. Signal CCI
    wake_up_interruptible(&rdkfmac_rq);
	printk("SJY %s:%d: Exit\n", __func__, __LINE__);
	return;
}

void push_to_rdkfmac_device(wlan_emu_msg_data_t *data)
{
	unsigned char *cmd_buffer;
	unsigned int count = 0;
	unsigned int buff_length = 0;
	heart_beat_data_t heart_beat_data;
	mac_update_t mac_update;

	if (data->type != wlan_emu_msg_type_emu80211) {
		printk("%s:%d: received invalid control data\n", __func__, __LINE__);
		return;
	}

	if (data->u.emu80211.ops != wlan_emu_emu80211_ops_type_cmnd) {
		printk("%s:%d: received %d, invalid ops for emu80211\n", __func__, __LINE__, data->u.emu80211.ops);
		return;
	}

	switch (data->u.emu80211.u.cmd.type) {
		case wlan_emu_emu80211_cmd_radiotap:
			buff_length = data->u.emu80211.u.cmd.buff_len;
			cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);
			if (cmd_buffer == NULL) {
				return;
			}
			memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));

			memcpy(&heart_beat_data.mac, &cmd_buffer[count], sizeof(heart_beat_data.mac));
			count += sizeof(heart_beat_data.mac);

			memcpy(&heart_beat_data.rssi, &cmd_buffer[count], sizeof(heart_beat_data.rssi));
			count += sizeof(heart_beat_data.rssi);

			memcpy(&heart_beat_data.noise, &cmd_buffer[count], sizeof(heart_beat_data.noise));
			count += sizeof(heart_beat_data.noise);

			memcpy(&heart_beat_data.bitrate, &cmd_buffer[count], sizeof(heart_beat_data.bitrate));
			count += sizeof(heart_beat_data.bitrate);

			printk("%s:%d rssi : %d noise : %d bitrate : %d for MAC : %pM\n", __func__, __LINE__,
				heart_beat_data.rssi, heart_beat_data.noise,
				heart_beat_data.bitrate, heart_beat_data.mac);
/*
			for (count = 0; count < buff_length; count++ ) {
				printk(" %02X", cmd_buffer[count]);
			}
*/
			update_heartbeat_data(&heart_beat_data);
			kfree(cmd_buffer);
			break;
		case wlan_emu_emu80211_cmd_mac_update:
			buff_length = data->u.emu80211.u.cmd.buff_len;
			cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);
			if (cmd_buffer == NULL) {
				return;
			}
			memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
			memcpy(&mac_update.old_mac, &cmd_buffer[count], sizeof(mac_update.old_mac));
			count += sizeof(mac_update.old_mac);
			memcpy(&mac_update.new_mac, &cmd_buffer[count], sizeof(mac_update.new_mac));
			count += sizeof(mac_update.new_mac);
			memcpy(&mac_update.op_modes, &cmd_buffer[count], sizeof(mac_update.op_modes));
			count += sizeof(mac_update.op_modes);
			memcpy(&mac_update.bridge_name, &cmd_buffer[count], sizeof(mac_update.bridge_name));
			count += sizeof(mac_update.bridge_name);
/*
			for (count = 0; count < buff_length; count++ ) {
				printk(" %02X", cmd_buffer[count]);
			}
*/
			update_sta_new_mac(&mac_update);
			kfree(cmd_buffer);
			break;
		case wlan_emu_emu80211_cmd_frame_auth_req:
			cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);

			if (cmd_buffer == NULL) {
				return;
			}

			memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
			update_auth_req(cmd_buffer, data->u.emu80211.u.cmd.buff_len);

			kfree(cmd_buffer);
			break;
		case wlan_emu_emu80211_cmd_frame_assoc_req:
			cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);

			if (cmd_buffer == NULL) {
				return;
			}

			memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
			update_assoc_req(cmd_buffer, data->u.emu80211.u.cmd.buff_len);

			kfree(cmd_buffer);
			break;
		default:
		break;
	}
	return;

}

static void handle_emu80211_msg_w(wlan_emu_msg_data_t *spec) {
	switch (spec->u.emu80211.ops) {
		case wlan_emu_emu80211_ops_type_tctrl:
			if (spec->u.emu80211.u.ctrl.ctrl == wlan_emu_emu80211_ctrl_tstart) {
				rdkfmac_emu80211_close = false;
			} else if (spec->u.emu80211.u.ctrl.ctrl == wlan_emu_emu80211_ctrl_tstop) {
				rdkfmac_emu80211_close = true;
			}
			printk("SJY Calling push_to_char_device from %s:%d for emu80211 tctrl ops\n", __func__, __LINE__);
			push_to_char_device(spec);
			break;
		case wlan_emu_emu80211_ops_type_close:
		    printk("SJY Calling push_to_char_device from %s:%d for emu80211 close ops\n", __func__, __LINE__);
			push_to_char_device(spec);
			break;
		case wlan_emu_emu80211_ops_type_cmnd:
			push_to_rdkfmac_device(spec);
			break;

		default:
			break;
	}
	return;
}

static void handle_agent_msg_w(wlan_emu_msg_data_t *spec) {
	switch (spec->u.agent_msg.ops) {
		case wlan_emu_msg_agent_ops_type_cmd:
			if (spec->u.agent_msg.u.cmd == wlan_emu_msg_agent_cmd_type_start) {
				rdkfmac_emu80211_close = false;
				push_to_char_device(spec);
			} else if (spec->u.agent_msg.u.cmd == wlan_emu_msg_agent_cmd_type_stop) {
				//rdkfmac_emu80211_close = true;
				push_to_char_device(spec);
			}
			break;
		case wlan_emu_msg_agent_ops_type_data:
			push_to_char_device(spec);
			break;
		case wlan_emu_msg_agent_ops_type_notification:
			push_to_char_device(spec);
			break;
		default:
			break;
	}
	return;
}

static void handle_frm80211_msg_w(char *read_buff, size_t size) {
	wlan_emu_msg_data_t *frm80211_msg;
	struct ieee80211_hdr *hdr;
	unsigned int msg_ops_type = 0;
	unsigned short fc, type, stype;
	const unsigned char rfc1042_hdr[ETH_ALEN] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
	unsigned char *tmp_frame_buf;
	unsigned int data_header_len = 0;

	printk("SJY Entering %s:%d\n", __func__, __LINE__);
	frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
	if (frm80211_msg == NULL) {
		printk("%s:%d NULL Pointer\n", __func__, __LINE__);
		return;
	}

	memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
	read_buff += sizeof(wlan_emu_msg_type_t);

	if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
		printk("%s:%d Invalid type\n", __func__, __LINE__);
		kfree(frm80211_msg);
		return;
	}

//writing dummy value == 0
	memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
	read_buff += sizeof(wlan_emu_cfg80211_ops_type_t);

	memcpy(&frm80211_msg->u.frm80211.u.frame.frame_len, read_buff, sizeof(unsigned int));
	read_buff += sizeof(unsigned int);

	memcpy(&frm80211_msg->u.frm80211.u.frame.macaddr, read_buff, ETH_ALEN);
	read_buff += ETH_ALEN;

	memcpy(&frm80211_msg->u.frm80211.u.frame.client_macaddr, read_buff, ETH_ALEN);
	read_buff += ETH_ALEN;

	frm80211_msg->u.frm80211.u.frame.frame = kzalloc(frm80211_msg->u.frm80211.u.frame.frame_len, GFP_KERNEL);
	if (frm80211_msg->u.frm80211.u.frame.frame == NULL) {
		printk("%s:%d NULL Pointer\n", __func__, __LINE__);
		kfree(frm80211_msg);
		return;
	}

	memcpy(frm80211_msg->u.frm80211.u.frame.frame, read_buff, frm80211_msg->u.frm80211.u.frame.frame_len);
	read_buff += frm80211_msg->u.frm80211.u.frame.frame_len;

	hdr = (struct ieee80211_hdr *)frm80211_msg->u.frm80211.u.frame.frame;
	fc = le16_to_cpu(hdr->frame_control);
	type = WLAN_FC_GET_TYPE(fc);

	if (type == WLAN_FC_TYPE_MGMT) {
		stype = WLAN_FC_GET_STYPE(fc);
		switch (stype) {
			case WLAN_FC_STYPE_PROBE_REQ:
				msg_ops_type = wlan_emu_frm80211_ops_type_prb_req;
				break;
			case WLAN_FC_STYPE_PROBE_RESP:
				msg_ops_type = wlan_emu_frm80211_ops_type_prb_resp;
				break;
			case WLAN_FC_STYPE_ASSOC_REQ:
				msg_ops_type = wlan_emu_frm80211_ops_type_assoc_req;
				break;
			case WLAN_FC_STYPE_ASSOC_RESP:
				msg_ops_type = wlan_emu_frm80211_ops_type_assoc_resp;
				break;
			case WLAN_FC_STYPE_AUTH:
				msg_ops_type = wlan_emu_frm80211_ops_type_auth;
				break;
			case WLAN_FC_STYPE_DEAUTH:
				msg_ops_type = wlan_emu_frm80211_ops_type_deauth;
				break;
			case WLAN_FC_STYPE_DISASSOC:
				msg_ops_type = wlan_emu_frm80211_ops_type_disassoc;
				break;
			case WLAN_FC_STYPE_ACTION:
				msg_ops_type = wlan_emu_frm80211_ops_type_action;
				break;
			case WLAN_FC_STYPE_REASSOC_REQ:
				msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_req;
				break;
			case WLAN_FC_STYPE_REASSOC_RESP:
				msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_resp;
				break;
			default:
				printk("%s:%d Invalid fc type : %d\n", __func__, __LINE__, stype);
				kfree(frm80211_msg->u.frm80211.u.frame.frame);
				kfree(frm80211_msg);
				return;
		}
	} else if (type == WLAN_FC_TYPE_DATA) {
		data_header_len = ieee80211_hdrlen(hdr->frame_control);
		if (frm80211_msg->u.frm80211.u.frame.frame_len < data_header_len + sizeof(rfc1042_hdr) + 2) {
			kfree(frm80211_msg->u.frm80211.u.frame.frame);
			kfree(frm80211_msg);
			return;
		}

		tmp_frame_buf =  (unsigned char *)frm80211_msg->u.frm80211.u.frame.frame;
		tmp_frame_buf += data_header_len;

		if (memcmp(tmp_frame_buf, rfc1042_hdr, sizeof(rfc1042_hdr)) != 0) {
			kfree(frm80211_msg->u.frm80211.u.frame.frame);
			kfree(frm80211_msg);
			return;
		}

		tmp_frame_buf += sizeof(rfc1042_hdr);
		if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) != ETH_P_PAE) {
			kfree(frm80211_msg->u.frm80211.u.frame.frame);
			kfree(frm80211_msg);
			return;
		} else {
			msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
		}
	}

	//updating the final correct value
	printk("SJY %s:%d: Memcpying the correct ops type %d to frm80211_msg\n", __func__, __LINE__, msg_ops_type);
	memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
    printk("SJY Calling push_to_char_device from %s:%d for frm80211 ops\n", __func__, __LINE__);
	push_to_char_device(frm80211_msg);
	kfree(frm80211_msg);
	return;
}


static ssize_t rdkfmac_write(struct file *file, const char __user *user_buffer,
					size_t size, loff_t * offset)
{
	wlan_emu_msg_data_t *pSpec;
	ssize_t sz;
	char *read_buff;

	printk("SJY Entering %s:%d\n", __func__, __LINE__);
	pSpec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
	if (pSpec == NULL) {
		printk("SJY %s:%d kmalloc failed for pSpec\n", __func__, __LINE__);
		return 0;
	}
	read_buff = kmalloc(size, GFP_KERNEL);
	if (read_buff == NULL) {
		printk("SJY %s:%d kmalloc failed for read_buff\n", __func__, __LINE__);
		return 0;
	}
	memset(read_buff, 0, size);
	if (copy_from_user(read_buff, user_buffer, size)) {
		printk("%s:%d: potential copy error\n", __func__, __LINE__);
		kfree(pSpec);
		kfree(read_buff);
		return 0;
	}
    printk("SJY %s:%d: Calling memcpy and the size of wlan_emu_msg_type_t is %zu\n", __func__, __LINE__, sizeof(wlan_emu_msg_type_t));
	memcpy((char*)&pSpec->type, read_buff, sizeof(wlan_emu_msg_type_t));
	printk("SJY %s:%d: The pspec->type is %d\n", __func__, __LINE__, pSpec->type);
	switch (pSpec->type) {
		case wlan_emu_msg_type_frm80211:
		    printk("SJY %s:%d Calling handle_frm80211_msg_w for frm80211 ops and push would be called\n", __func__, __LINE__);
			handle_frm80211_msg_w(read_buff, size);
			sz = size;
			break;
		case wlan_emu_msg_type_emu80211:
			memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
			printk("SJY %s:%d: Calling handle_emu80211_msg_w for emu80211 ops and there push would be called\n", __func__, __LINE__);
			handle_emu80211_msg_w(pSpec);
			sz = sizeof(wlan_emu_msg_data_t);
			break;
		case wlan_emu_msg_type_webconfig:
			memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
			printk("SJY %s:%d: Calling push_to_char_device from %s:%d for webconfig ops\n", __func__, __LINE__, __func__, __LINE__);
			push_to_char_device(pSpec);
			sz = sizeof(wlan_emu_msg_data_t);
			break;
		case wlan_emu_msg_type_agent:
			memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
			handle_agent_msg_w(pSpec);
			sz = sizeof(wlan_emu_msg_data_t);
			break;
		default:
			printk("SJY %s:%d: Invalid read operation\n",__func__, __LINE__);
			sz = 0;
			break;
	}

	kfree(read_buff);
	kfree(pSpec);
	printk("SJY %s:%d: Exit\n", __func__, __LINE__);
	return sz;
}

void handle_cfg80211_msg_start_ap(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL))
	{
		printk(KERN_INFO "%s:%d: NULL Pointer \n", __func__, __LINE__);
		return;
	}

	memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	s_tmp += sizeof(wlan_emu_msg_type_t);
	*len += sizeof(wlan_emu_msg_type_t);

	memcpy(s_tmp, &spec->u.cfg80211.ops, sizeof(wlan_emu_cfg80211_ops_type_t));
	s_tmp += sizeof(wlan_emu_cfg80211_ops_type_t);
	*len += sizeof(wlan_emu_cfg80211_ops_type_t);

	memcpy(s_tmp, &(spec->u.cfg80211.u.start_ap.ifindex), sizeof(int));
	s_tmp += sizeof(int);
	*len += sizeof(int);

	memcpy(s_tmp, &(spec->u.cfg80211.u.start_ap.phy_index), sizeof(int));
	s_tmp += sizeof(int);
	*len += sizeof(int);

	memcpy(s_tmp, &(spec->u.cfg80211.u.start_ap.head_len), sizeof(size_t));
	s_tmp += sizeof(size_t);
	*len += sizeof(size_t);

	memcpy(s_tmp, &(spec->u.cfg80211.u.start_ap.tail_len), sizeof(size_t));
	s_tmp += sizeof(size_t);
	*len += sizeof(size_t);

	memcpy(s_tmp, spec->u.cfg80211.u.start_ap.beacon_head, spec->u.cfg80211.u.start_ap.head_len);
	s_tmp += spec->u.cfg80211.u.start_ap.head_len;
	*len += spec->u.cfg80211.u.start_ap.head_len;

	memcpy(s_tmp, spec->u.cfg80211.u.start_ap.beacon_tail, spec->u.cfg80211.u.start_ap.tail_len);
	s_tmp += spec->u.cfg80211.u.start_ap.tail_len;
	*len += spec->u.cfg80211.u.start_ap.tail_len;

	return;
}

void handle_cfg80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n", __func__, __LINE__, spec, s_tmp, len);
		return;
	}

	switch(spec->u.cfg80211.ops) {
		case wlan_emu_cfg80211_ops_type_start_ap:
			handle_cfg80211_msg_start_ap(spec, len, s_tmp);
			break;
		default:
			break;
	}

	return;
}

 void handle_emu80211_msg_tctrl(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
 {
	 if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		 printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n",
				 __func__, __LINE__, spec, s_tmp, len);
		 return;
	 }
	 memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	 s_tmp += sizeof(wlan_emu_msg_type_t);
	 *len += sizeof(wlan_emu_msg_type_t);

	 memcpy(s_tmp, &spec->u.emu80211.ops, sizeof(wlan_emu_emu80211_ops_type_t));
	 s_tmp += sizeof(wlan_emu_emu80211_ops_type_t);
	 *len += sizeof(wlan_emu_emu80211_ops_type_t);

	 memcpy(s_tmp, &spec->u.emu80211.u.ctrl.ctrl, sizeof(wlan_emu_emu80211_ctrl_type_t));
	 s_tmp += sizeof(wlan_emu_emu80211_ctrl_type_t);
	 *len += sizeof(wlan_emu_emu80211_ctrl_type_t);

	 memcpy(s_tmp, &spec->u.emu80211.u.ctrl.coverage, sizeof(wlan_emu_test_coverage_t));
	 s_tmp += sizeof(wlan_emu_test_coverage_t);
	 *len += sizeof(wlan_emu_test_coverage_t);

	 memcpy(s_tmp, &spec->u.emu80211.u.ctrl.type, sizeof(wlan_emu_test_type_t));
	 s_tmp += sizeof(wlan_emu_test_type_t);
	 *len += sizeof(wlan_emu_test_type_t);

	 return;
 }

void handle_emu80211_msg_close(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer \n", __func__, __LINE__);
		return;
	}

	memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	s_tmp += sizeof(wlan_emu_msg_type_t);
	*len += sizeof(wlan_emu_msg_type_t);

	memcpy(s_tmp, &spec->u.emu80211.ops, sizeof(wlan_emu_emu80211_ops_type_t));
	s_tmp += sizeof(wlan_emu_emu80211_ops_type_t);
	*len += sizeof(wlan_emu_emu80211_ops_type_t);

	memcpy(s_tmp, &spec->u.emu80211.u.close.fd, sizeof(int));
	s_tmp += sizeof(int);
	*len += sizeof(int);

	return;
}


void handle_emu80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n", __func__, __LINE__, spec, s_tmp, len);
		return;
	}

	switch(spec->u.emu80211.ops) {
		case wlan_emu_emu80211_ops_type_tctrl:
			handle_emu80211_msg_tctrl(spec, len, s_tmp);
		break;
		case wlan_emu_emu80211_ops_type_close:
			handle_emu80211_msg_close(spec, len, s_tmp);
		break;
		default:
		break;
	}

	return;
}


void handle_webconfig_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n", __func__, __LINE__, spec, s_tmp, len);
		return;
	}

	memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	s_tmp += sizeof(wlan_emu_msg_type_t);
	*len += sizeof(wlan_emu_msg_type_t);

	memcpy(s_tmp, &spec->u.ow_webconfig.subdoc_type, sizeof(webconfig_subdoc_type_t));
	s_tmp += sizeof(webconfig_subdoc_type_t);
	*len += sizeof(webconfig_subdoc_type_t);

    return;
}

void handle_agent_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n", __func__, __LINE__, spec, s_tmp, len);
		return;
	}

	memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	s_tmp += sizeof(wlan_emu_msg_type_t);
	*len += sizeof(wlan_emu_msg_type_t);

	memcpy(s_tmp, &spec->u.agent_msg.ops, sizeof(wlan_emu_msg_agent_ops_t));
	s_tmp += sizeof(wlan_emu_msg_agent_ops_t);
	*len += sizeof(wlan_emu_msg_agent_ops_t);


	if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_cmd) {
		memcpy(s_tmp, &spec->u.agent_msg.u.cmd, sizeof(wlan_emu_msg_agent_cmd_t));
		s_tmp += sizeof(wlan_emu_msg_agent_cmd_t);
		*len += sizeof(wlan_emu_msg_agent_cmd_t);
	}

	if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_data) {
		memcpy(s_tmp, &spec->u.agent_msg.u.buf, sizeof(void *));
		s_tmp += sizeof(void *);
		*len += sizeof(void *);
	}

	if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_notification) {
		memcpy(s_tmp, &spec->u.agent_msg.u.agent_notif.sub_ops_type, sizeof(int));
		s_tmp += sizeof(int);
		*len += sizeof(int);

		if (spec->u.agent_msg.u.agent_notif.sub_ops_type == wlan_msg_ext_agent_ops_sub_type_wifi_notification) {
			memcpy(s_tmp, &spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.sta_state, sizeof(int));
			s_tmp += sizeof(int);
			*len += sizeof(int);

			memcpy(s_tmp, spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.sta_mac_addr, ETH_ALEN);
			s_tmp += ETH_ALEN;
			*len += ETH_ALEN;

			memcpy(s_tmp, spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.bssid_mac_addr, ETH_ALEN);
			s_tmp += ETH_ALEN;
			*len += ETH_ALEN;
		}
	}

	return;
}

static void handle_frame(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
	s_tmp += sizeof(wlan_emu_msg_type_t);
	*len += sizeof(wlan_emu_msg_type_t);

	memcpy(s_tmp, &spec->u.frm80211.ops, sizeof(wlan_emu_frm80211_ops_type_t));
	s_tmp += sizeof(wlan_emu_frm80211_ops_type_t);
	*len += sizeof(wlan_emu_frm80211_ops_type_t);

	printk("%s:%d Frame len is %d ops is %d\n", __func__, __LINE__, spec->u.frm80211.u.frame.frame_len, spec->u.frm80211.ops);
	memcpy(s_tmp, &spec->u.frm80211.u.frame.frame_len, sizeof(unsigned int));
	s_tmp += sizeof(unsigned int);
	*len += sizeof(unsigned int);

	memcpy(s_tmp, spec->u.frm80211.u.frame.frame, spec->u.frm80211.u.frame.frame_len);
	s_tmp += spec->u.frm80211.u.frame.frame_len;
	*len += spec->u.frm80211.u.frame.frame_len;

	memcpy(s_tmp, spec->u.frm80211.u.frame.macaddr, ETH_ALEN);
	s_tmp += ETH_ALEN;
	*len += ETH_ALEN;

	memcpy(s_tmp, spec->u.frm80211.u.frame.client_macaddr, ETH_ALEN);
	*len += ETH_ALEN;

	return;
}

static void handle_frm80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %s len : %p \n", __func__, __LINE__, spec, s_tmp, len);
		return;
	}

	switch(spec->u.frm80211.ops) {
		case wlan_emu_frm80211_ops_type_prb_req:
		case wlan_emu_frm80211_ops_type_prb_resp:
		case wlan_emu_frm80211_ops_type_assoc_resp:
		case wlan_emu_frm80211_ops_type_assoc_req:
		case wlan_emu_frm80211_ops_type_auth:
		case wlan_emu_frm80211_ops_type_deauth:
		case wlan_emu_frm80211_ops_type_disassoc:
		case wlan_emu_frm80211_ops_type_eapol:
		case wlan_emu_frm80211_ops_type_reassoc_req:
		case wlan_emu_frm80211_ops_type_reassoc_resp:
		case wlan_emu_frm80211_ops_type_action:
			handle_frame(spec, len, s_tmp);
			break;
		default:
			printk(KERN_INFO "%s:%d: Not Handling op type %d\n", __func__, __LINE__, spec->u.emu80211.ops);
			break;
	}

	return;
}

static ssize_t rdkfmac_read(struct file *file, char __user *user_buffer,
		size_t size, loff_t *offset)
{
	wlan_emu_msg_data_t *spec;
	ssize_t return_len = 0;
	char *send_buff;
	u8 *s_tmp;

	printk("SJY Entering %s:%d \n", __func__, __LINE__);
	spec = pop_from_char_device();
	if (spec == NULL) {
		printk("SJY %s:%d: No data to read\n", __func__, __LINE__);
		return 0;
	}
	send_buff = kmalloc(size, GFP_KERNEL);
    if(send_buff == NULL){
		printk("SJY %s:%d send_buff failed\n,", __func__, __LINE__);
		return 0;
	}
	memset(send_buff, 0, size);
	s_tmp = send_buff;
    printk("SJY %s:%d: the spec->type is %d\n", __func__, __LINE__, spec->type);
	switch (spec->type) {
		case wlan_emu_msg_type_cfg80211:
			handle_cfg80211_msg(spec, &return_len, s_tmp);
		break;
		case wlan_emu_msg_type_emu80211:
			handle_emu80211_msg(spec, &return_len, s_tmp);
		break;
		case wlan_emu_msg_type_frm80211:
			handle_frm80211_msg(spec, &return_len, s_tmp);
			break;
		case wlan_emu_msg_type_webconfig:
			handle_webconfig_msg(spec, &return_len, s_tmp);
			break;
		case wlan_emu_msg_type_agent:
			handle_agent_msg(spec, &return_len, s_tmp);
			break;
		default:
			break;
	}
    printk("SJY Calling copy to user\n");
	if (copy_to_user(user_buffer, send_buff, return_len)) {
		printk("%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}

	if (spec->type == wlan_emu_msg_type_frm80211) {
		kfree(spec->u.frm80211.u.frame.frame);
	}


	kfree(spec);
	kfree(send_buff);
    printk("SJY Exiting and returning from rdkfmac_read\n");
	return return_len;
}

static int rdkfmac_open(struct inode *inode, struct file *file)
{
    unsigned long flags;

    spin_lock_irqsave(&g_char_device.lock, flags);
    g_char_device.num_inst++;
    spin_unlock_irqrestore(&g_char_device.lock, flags);

	printk(KERN_INFO "%s:%d Opened Instances: %d\n", __func__, __LINE__, g_char_device.num_inst);
    return 0;
}

static int rdkfmac_release(struct inode *inode, struct file *file)
{
    unsigned long flags;

    spin_lock_irqsave(&g_char_device.lock, flags);
    if (g_char_device.num_inst > 0) {
        g_char_device.num_inst--;
    }
    spin_unlock_irqrestore(&g_char_device.lock, flags);

	printk(KERN_INFO "%s:%d Opened Instances: %d\n", __func__, __LINE__, g_char_device.num_inst);
    return 0;
}

const struct file_operations rdkfmac_fops = {
	.owner = THIS_MODULE,
	.open = rdkfmac_open,
	.read = rdkfmac_read,
	.write = rdkfmac_write,
	.release = rdkfmac_release,
	.poll = rdkfmac_poll
};

int init_rdkfmac_cdev(void)
{
	int ret_val;

	printk(KERN_INFO "%s:%d\n", __func__, __LINE__);
	ret_val = register_chrdev_region(MKDEV(RDKFMAC_MAJOR, 0), 1, RDKFMAC_DEVICE_DRIVER_NAME);
	if (ret_val != 0) {
			printk(KERN_INFO "%s:%d: register_chrdev_region():failed with error code:%d\n", __func__, __LINE__, ret_val);
		return ret_val;
	}

	memset(&g_char_device, 0, sizeof(rdkfmac_device_data_t));
    spin_lock_init(&g_char_device.lock);
	cdev_init(&g_char_device.cdev, &rdkfmac_fops);
	cdev_add(&g_char_device.cdev, MKDEV(RDKFMAC_MAJOR, 0), 1);
	g_char_device.class = class_create(THIS_MODULE, RDKFMAC_CLASS_NAME);
	if (IS_ERR(g_char_device.class)){
		printk(KERN_ALERT "cdrv : register device class failed\n");
		return PTR_ERR(g_char_device.class);
	}

	INIT_LIST_HEAD(&g_char_device.list_head);
	g_char_device.list_tail = &g_char_device.list_head;
	printk(KERN_INFO "%s:%d: registered successfully\n", __func__, __LINE__);
	g_char_device.tdev = MKDEV(RDKFMAC_MAJOR, 0);
	g_char_device.dev = device_create(g_char_device.class, NULL,
				g_char_device.tdev, NULL, RDKFMAC_DEVICE_NAME);

	return 0;
}

void cleanup_rdkfmac_cdev(void)
{
	device_destroy(g_char_device.class, g_char_device.tdev);
	class_destroy(g_char_device.class);
	cdev_del(&g_char_device.cdev);
	unregister_chrdev_region(MKDEV(RDKFMAC_MAJOR, 0), 1);

	printk(KERN_INFO "%s:%d: unregistered successfully\n", __func__, __LINE__);
}

/*(void)
{
	unsigned count = 0;
	struct list_head *ptr = &g_char_device.list_head;
	for (ptr = &g_char_device.list_head; ptr != g_char_device.list_tail; ptr = ptr->next) {
		count++;
	}
	return count;
} */

wlan_emu_msg_data_t* pop_from_char_device(void)
{
    wlan_emu_msg_data_t *spec = NULL;
    wlan_emu_msg_data_entry_t *entry = NULL;
    unsigned long flags;
    printk("SJY Entering %s:%d\n", __func__, __LINE__);
    spin_lock_irqsave(&g_char_device.lock, flags);

    // If tail points to head, list is empty
    if (g_char_device.list_tail == &g_char_device.list_head) {
		printk("SJY %s:%d list is empty\n", __func__, __LINE__);
        spin_unlock_irqrestore(&g_char_device.lock, flags);
        return NULL;
    }

    // Get the current tail entry
    entry = list_entry(g_char_device.list_tail, wlan_emu_msg_data_entry_t, list_entry);
    
    // Move tail back to the previous node
    g_char_device.list_tail = g_char_device.list_tail->prev;
    
    // Unlink the node (kernel poisons the pointers here safely under lock)
	printk("SJY %s:%d deleting the list entry\n", __func__, __LINE__);
    list_del(&entry->list_entry);

    spin_unlock_irqrestore(&g_char_device.lock, flags);

    spec = entry->spec;
    kfree(entry);
    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
	return &g_char_device;
}

