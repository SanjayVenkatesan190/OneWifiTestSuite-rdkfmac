/* Modifications Copyright 2025 Comcast Cable Communications Management, LLC
 * Licensed under the GPLv2.0 License
 */

#include "rdkfmac.h"

struct rdkfmac_device_data g_char_device;
static DECLARE_WAIT_QUEUE_HEAD(rdkfmac_rq); 
static wlan_emu_msg_data_t *pop_from_char_device(void);
static unsigned int get_list_entries_count_in_char_device(void);
static spinlock_t g_char_device_list_lock;
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


static unsigned int rdkfmac_poll(struct file *filp, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(filp, &rdkfmac_rq, wait);
 
	if (get_list_entries_count_in_char_device() != 0) { 
			mask |= (POLLIN | POLLRDNORM);
	}

	return mask;
}

void push_to_char_device(wlan_emu_msg_data_t *data)
{
    wlan_emu_msg_data_entry_t *entry;
    wlan_emu_msg_data_t *spec;
    unsigned long flags;
    unsigned int q_count;

    printk("SJY_PUSH_ENTER: Received data=%p, type=%d\n", data, data ? data->type : -1);

    if (g_char_device.num_inst == 0) {
        printk("SJY_PUSH_DROP: No listeners (num_inst=0)\n");
        return;
    }

    if (rdkfmac_emu80211_close == true)  {
        printk("SJY_PUSH_DROP: Emulator is marked closed\n");
        return;
    }

    entry = kmalloc(sizeof(wlan_emu_msg_data_entry_t), GFP_ATOMIC);
    spec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_ATOMIC);
    
    if (!entry || !spec) {
        printk("SJY_PUSH_FATAL: GFP_ATOMIC Allocation Failed! entry=%p, spec=%p\n", entry, spec);
        if(entry) kfree(entry);
        if(spec) kfree(spec);
        return;
    }
    
    entry->spec = spec;
    memcpy(spec, data, sizeof(wlan_emu_msg_data_t));
    printk("SJY_PUSH_ALLOC: Allocated entry=%p, spec=%p\n", entry, spec);

    /* Deep Copy Shield */
    if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame_len > 0) {
        if (spec->u.frm80211.u.frame.frame_len > 4096) {
             printk("SJY_PUSH_ERROR: Frame too large (%u), dropping to prevent panic\n", spec->u.frm80211.u.frame.frame_len);
             kfree(entry); kfree(spec);
             return;
        }
        
        spec->u.frm80211.u.frame.frame = kmalloc(spec->u.frm80211.u.frame.frame_len, GFP_ATOMIC);
        if (spec->u.frm80211.u.frame.frame) {
            memcpy(spec->u.frm80211.u.frame.frame, data->u.frm80211.u.frame.frame, spec->u.frm80211.u.frame.frame_len);
            printk("SJY_PUSH_DEEPCOPY: Success. Copied %u bytes to new ptr=%p\n", 
                   spec->u.frm80211.u.frame.frame_len, spec->u.frm80211.u.frame.frame);
        } else {
            printk("SJY_PUSH_ERROR: Deep copy allocation failed\n");
            spec->u.frm80211.u.frame.frame_len = 0; 
        }
    }

    /* Queue Operation */
    spin_lock_irqsave(&g_char_device_list_lock, flags);
    printk("SJY_PUSH_LOCK: Acquired lock. Current tail=%p\n", g_char_device.list_tail);
    
    list_add(&entry->list_entry, g_char_device.list_tail);
    g_char_device.list_tail = &entry->list_entry;
    
    printk("SJY_PUSH_ENQUEUED: Updated tail to %p\n", g_char_device.list_tail);
    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    q_count = get_list_entries_count_in_char_device();
    printk("SJY_PUSH_WAKE: Waking wait queue. Total items in list=%u\n", q_count);
    
    wake_up_interruptible(&rdkfmac_rq);
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
			push_to_char_device(spec);
			break;
		case wlan_emu_emu80211_ops_type_close:
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

    printk("SJY_FRMW_ENTER: Parsing inbound frame of size %zu\n", size);

    frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (frm80211_msg == NULL) {
        printk("SJY_FRMW_ERROR: kzalloc failed\n");
        return;
    }

    memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
    read_buff += sizeof(wlan_emu_msg_type_t);

    if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
        printk("SJY_FRMW_ERROR: Type mismatch (expected %d, got %d)\n", wlan_emu_msg_type_frm80211, frm80211_msg->type);
        kfree(frm80211_msg);
        return;
    }

    /* Skip ops type in buffer */
    memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
    read_buff += sizeof(wlan_emu_cfg80211_ops_type_t);

    memcpy(&frm80211_msg->u.frm80211.u.frame.frame_len, read_buff, sizeof(unsigned int));
    read_buff += sizeof(unsigned int);

    printk("SJY_FRMW_META: Inbound frame_len=%u\n", frm80211_msg->u.frm80211.u.frame.frame_len);

    memcpy(&frm80211_msg->u.frm80211.u.frame.macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;

    memcpy(&frm80211_msg->u.frm80211.u.frame.client_macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;

    if (frm80211_msg->u.frm80211.u.frame.frame_len > 4096) {
        printk("SJY_FRMW_FATAL: Frame length %u exceeds safety limits\n", frm80211_msg->u.frm80211.u.frame.frame_len);
        kfree(frm80211_msg);
        return;
    }

    frm80211_msg->u.frm80211.u.frame.frame = kzalloc(frm80211_msg->u.frm80211.u.frame.frame_len, GFP_KERNEL);
    if (frm80211_msg->u.frm80211.u.frame.frame == NULL) {
        printk("SJY_FRMW_ERROR: Frame payload kzalloc failed\n");
        kfree(frm80211_msg);
        return;
    }

    memcpy(frm80211_msg->u.frm80211.u.frame.frame, read_buff, frm80211_msg->u.frm80211.u.frame.frame_len);
    read_buff += frm80211_msg->u.frm80211.u.frame.frame_len;

    hdr = (struct ieee80211_hdr *)frm80211_msg->u.frm80211.u.frame.frame;
    fc = le16_to_cpu(hdr->frame_control);
    type = WLAN_FC_GET_TYPE(fc);

    printk("SJY_FRMW_HDR: Parsed FC. Type=%d\n", type);

    if (type == WLAN_FC_TYPE_MGMT) {
        stype = WLAN_FC_GET_STYPE(fc);
        printk("SJY_FRMW_MGMT: Subtype=%d\n", stype);
        switch (stype) {
            case WLAN_FC_STYPE_PROBE_REQ: msg_ops_type = wlan_emu_frm80211_ops_type_prb_req; break;
            case WLAN_FC_STYPE_PROBE_RESP: msg_ops_type = wlan_emu_frm80211_ops_type_prb_resp; break;
            case WLAN_FC_STYPE_ASSOC_REQ: msg_ops_type = wlan_emu_frm80211_ops_type_assoc_req; break;
            case WLAN_FC_STYPE_ASSOC_RESP: msg_ops_type = wlan_emu_frm80211_ops_type_assoc_resp; break;
            case WLAN_FC_STYPE_AUTH: msg_ops_type = wlan_emu_frm80211_ops_type_auth; break;
            case WLAN_FC_STYPE_DEAUTH: msg_ops_type = wlan_emu_frm80211_ops_type_deauth; break;
            case WLAN_FC_STYPE_DISASSOC: msg_ops_type = wlan_emu_frm80211_ops_type_disassoc; break;
            case WLAN_FC_STYPE_ACTION: msg_ops_type = wlan_emu_frm80211_ops_type_action; break;
            case WLAN_FC_STYPE_REASSOC_REQ: msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_req; break;
            case WLAN_FC_STYPE_REASSOC_RESP: msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_resp; break;
            default:
                printk("SJY_FRMW_ERROR: Invalid mgmt subtype: %d\n", stype);
                kfree(frm80211_msg->u.frm80211.u.frame.frame);
                kfree(frm80211_msg);
                return;
        }
    } else if (type == WLAN_FC_TYPE_DATA) {
        printk("SJY_FRMW_DATA: Processing data frame\n");
        data_header_len = ieee80211_hdrlen(hdr->frame_control);
        if (frm80211_msg->u.frm80211.u.frame.frame_len < data_header_len + sizeof(rfc1042_hdr) + 2) {
            printk("SJY_FRMW_ERROR: Data frame too short for headers\n");
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }

        tmp_frame_buf =  (unsigned char *)frm80211_msg->u.frm80211.u.frame.frame;
        tmp_frame_buf += data_header_len;

        if (memcmp(tmp_frame_buf, rfc1042_hdr, sizeof(rfc1042_hdr)) != 0) {
            printk("SJY_FRMW_ERROR: RFC1042 header mismatch\n");
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }

        tmp_frame_buf += sizeof(rfc1042_hdr);
        if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) == ETH_P_PAE) {
            msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
        } else {
            printk("SJY_FRMW_ERROR: Not EAPOL payload\n");
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }
    }

    memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
    printk("SJY_FRMW_PUSH: Pushing valid frame to queue (ops=%d)\n", msg_ops_type);
    
    push_to_char_device(frm80211_msg);
    
    kfree(frm80211_msg->u.frm80211.u.frame.frame);
    kfree(frm80211_msg);
    
    printk("SJY_FRMW_EXIT: Complete\n");
}


static ssize_t rdkfmac_write(struct file *file, const char __user *user_buffer, size_t size, loff_t * offset)
{
    wlan_emu_msg_data_t *pSpec;
    ssize_t sz = 0;
    char *read_buff;

    printk("SJY_WRITE_ENTER: Userspace requesting write of size=%zu\n", size);

    /* Safety Check to prevent out-of-bounds reading */
    if (size < sizeof(wlan_emu_msg_type_t)) {
        printk("SJY_WRITE_ERROR: Size %zu is too small for basic type header\n", size);
        return -EINVAL;
    }

    pSpec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    read_buff = kmalloc(size, GFP_KERNEL);
    
    if (!pSpec || !read_buff) {
        printk("SJY_WRITE_FATAL: kmalloc failed\n");
        if(pSpec) kfree(pSpec);
        if(read_buff) kfree(read_buff);
        return -ENOMEM;
    }

    memset(read_buff, 0, size);
    if (copy_from_user(read_buff, user_buffer, size)) {
        printk("SJY_WRITE_ERROR: copy_from_user failed!\n");
        kfree(pSpec); kfree(read_buff);
        return -EFAULT;
    }

    memcpy((char*)&pSpec->type, read_buff, sizeof(wlan_emu_msg_type_t));
    printk("SJY_WRITE_TYPE: Parsed msg_type=%d\n", pSpec->type);

    switch (pSpec->type) {
        case wlan_emu_msg_type_frm80211:
            printk("SJY_WRITE_DISPATCH: Routing to handle_frm80211_msg_w\n");
            handle_frm80211_msg_w(read_buff, size);
            sz = size;
            break;
        case wlan_emu_msg_type_emu80211:
        case wlan_emu_msg_type_webconfig:
        case wlan_emu_msg_type_agent:
            if (size >= sizeof(wlan_emu_msg_data_t)) {
                memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
                if (pSpec->type == wlan_emu_msg_type_emu80211) {
                    printk("SJY_WRITE_DISPATCH: Routing emu80211 msg\n");
                    handle_emu80211_msg_w(pSpec);
                } else if (pSpec->type == wlan_emu_msg_type_webconfig) {
                    printk("SJY_WRITE_DISPATCH: Pushing webconfig msg direct to queue\n");
                    push_to_char_device(pSpec);
                } else {
                    printk("SJY_WRITE_DISPATCH: Routing agent msg\n");
                    handle_agent_msg_w(pSpec);
                }
                sz = sizeof(wlan_emu_msg_data_t);
            } else {
                printk("SJY_WRITE_ERROR: Size %zu too small for full struct cast\n", size);
            }
            break;
        default:
            printk("SJY_WRITE_ERROR: Invalid message type %d\n", pSpec->type);
            sz = -EINVAL;
            break;
    }

    kfree(read_buff);
    kfree(pSpec);
    
    printk("SJY_WRITE_EXIT: Returning %zd\n", sz);
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

	printk("SJY_CFG_ENTER: ops=%d\n", spec->u.cfg80211.ops);

	switch(spec->u.cfg80211.ops) {
		case wlan_emu_cfg80211_ops_type_start_ap:
		    printk("SJY_CFG_DISPATCH: Routing to start_ap handler\n");
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

static void handle_frame(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp) {
    u8 *start_ptr = s_tmp;

    printk("SJY_HF_ENTER: Starting handle_frame. Initial len offset=%zd\n", *len);

    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); 
    s_tmp += sizeof(wlan_emu_msg_type_t); 
    *len += sizeof(wlan_emu_msg_type_t);
    
    memcpy(s_tmp, &spec->u.frm80211.ops, sizeof(wlan_emu_frm80211_ops_type_t)); 
    s_tmp += sizeof(wlan_emu_frm80211_ops_type_t); 
    *len += sizeof(wlan_emu_frm80211_ops_type_t);
    
    memcpy(s_tmp, &spec->u.frm80211.u.frame.frame_len, sizeof(unsigned int)); 
    s_tmp += sizeof(unsigned int); 
    *len += sizeof(unsigned int);
    
    printk("SJY_HF_META: Wrote Type/Ops/Len. frame_len=%u, ops=%d. Current offset=%zd\n", 
           spec->u.frm80211.u.frame.frame_len, spec->u.frm80211.ops, *len);
    
    if (spec->u.frm80211.u.frame.frame && spec->u.frm80211.u.frame.frame_len > 0) {
        printk("SJY_HF_PAYLOAD: Copying %u bytes of raw payload to %p\n", 
               spec->u.frm80211.u.frame.frame_len, s_tmp);
        memcpy(s_tmp, spec->u.frm80211.u.frame.frame, spec->u.frm80211.u.frame.frame_len);
        s_tmp += spec->u.frm80211.u.frame.frame_len;
        *len += spec->u.frm80211.u.frame.frame_len;
    } else {
        printk("SJY_HF_PAYLOAD: No payload to copy (len=0 or ptr=NULL)\n");
    }
    
    memcpy(s_tmp, spec->u.frm80211.u.frame.macaddr, ETH_ALEN); 
    s_tmp += ETH_ALEN; 
    *len += ETH_ALEN;
    
    memcpy(s_tmp, spec->u.frm80211.u.frame.client_macaddr, ETH_ALEN); 
    s_tmp += ETH_ALEN; 
    *len += ETH_ALEN;

    printk("SJY_HF_EXIT: Completed. Total bytes written this call=%zu. Final len=%zd\n", 
           (size_t)(s_tmp - start_ptr), *len);
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

static ssize_t rdkfmac_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
    wlan_emu_msg_data_t *spec;
    ssize_t return_len = 0;
    char *send_buff;
    u8 *s_tmp;
    unsigned int q_count;

    printk("SJY_READ_ENTER: User requested size=%zu\n", size);

    q_count = get_list_entries_count_in_char_device();
    printk("SJY_READ_WAIT_CHECK: Items in queue=%u\n", q_count);

    if (wait_event_interruptible(rdkfmac_rq, get_list_entries_count_in_char_device() != 0)) {
        printk("SJY_READ_INTERRUPT: Woken by signal (e.g., Ctrl+C)\n");
        return -ERESTARTSYS;
    }

    printk("SJY_READ_WAKE: Process awake, queue has data\n");

    spec = pop_from_char_device();
    if (spec == NULL) {
        printk("SJY_READ_EMPTY: pop() returned NULL despite wake event\n");
        return 0;
    }

    /* Allocate buffer (using user's safe size request) */
    send_buff = kzalloc(size, GFP_KERNEL);
    if (!send_buff) {
        printk("SJY_READ_ERROR: kzalloc failed for size %zu\n", size);
        if (spec->type == wlan_emu_msg_type_frm80211) kfree(spec->u.frm80211.u.frame.frame);
        kfree(spec);
        return -ENOMEM;
    }
    
    s_tmp = send_buff;
    printk("SJY_READ_DISPATCH: Dispatching spec type=%d to handlers\n", spec->type);

    switch (spec->type) {
        case wlan_emu_msg_type_cfg80211: handle_cfg80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_emu80211: handle_emu80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_frm80211: handle_frm80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_webconfig: handle_webconfig_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_agent: handle_agent_msg(spec, &return_len, s_tmp); break;
        default: break;
    }

    printk("SJY_READ_COPY: Handlers complete. Copying %zd bytes to user space\n", return_len);
    
    if (copy_to_user(user_buffer, send_buff, return_len)) {
        printk("SJY_READ_ERROR: copy_to_user failed!\n");
        return_len = -EFAULT;
    }

    if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame) {
        printk("SJY_READ_CLEANUP: Freeing deep-copied frame payload %p\n", spec->u.frm80211.u.frame.frame);
        kfree(spec->u.frm80211.u.frame.frame);
    }

    kfree(spec);
    kfree(send_buff);
    
    printk("SJY_READ_EXIT: Returning %zd to userspace\n", return_len);
    return return_len;
}
static int rdkfmac_open(struct inode *inode, struct file *file)
{
    unsigned long flags;
    
    spin_lock_irqsave(&g_char_device_list_lock, flags);
    g_char_device.num_inst++;
    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    printk(KERN_INFO "%s:%d Opened Instances: %d\n", __func__, __LINE__, g_char_device.num_inst);
    return 0;
}

static int rdkfmac_release(struct inode *inode, struct file *file)
{
    unsigned long flags;
    
    spin_lock_irqsave(&g_char_device_list_lock, flags);
    if (g_char_device.num_inst > 0) {
        g_char_device.num_inst--;
    }
    
    /* Clean up the tail pointer if nobody is listening anymore */
    if (g_char_device.num_inst == 0) {
        g_char_device.list_tail = &g_char_device.list_head; 
    }
    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

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

	cdev_init(&g_char_device.cdev, &rdkfmac_fops);
	cdev_add(&g_char_device.cdev, MKDEV(RDKFMAC_MAJOR, 0), 1);
	g_char_device.class = class_create(THIS_MODULE, RDKFMAC_CLASS_NAME);
	if (IS_ERR(g_char_device.class)){
		printk(KERN_ALERT "cdrv : register device class failed\n");
		return PTR_ERR(g_char_device.class);
	}

	INIT_LIST_HEAD(&g_char_device.list_head);
	g_char_device.list_tail = &g_char_device.list_head;
	spin_lock_init(&g_char_device_list_lock);
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

unsigned int get_list_entries_count_in_char_device(void)
{
    unsigned count = 0;
    struct list_head *ptr;
    unsigned long flags;

    /* CRITICAL: Lock the list before walking it */
    spin_lock_irqsave(&g_char_device_list_lock, flags);
    for (ptr = &g_char_device.list_head; ptr != g_char_device.list_tail; ptr = ptr->next) {
        count++;
        if (count > 1000) break; /* Safety Break to prevent CPU hangs */
    }
    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    return count;
}
wlan_emu_msg_data_t *pop_from_char_device(void)
{
    wlan_emu_msg_data_t *spec = NULL;
    wlan_emu_msg_data_entry_t *entry = NULL;
    unsigned long flags;

    printk("SJY_POP_ENTER: Attempting to pop packet\n");

    spin_lock_irqsave(&g_char_device_list_lock, flags);
    printk("SJY_POP_LOCK: Acquired list lock\n");
    
    if (g_char_device.list_tail == &g_char_device.list_head) {
        spin_unlock_irqrestore(&g_char_device_list_lock, flags);
        printk("SJY_POP_EMPTY: list_tail == list_head (%p). Queue is empty.\n", &g_char_device.list_head);
        return NULL;
    }

    entry = list_entry(g_char_device.list_tail, wlan_emu_msg_data_entry_t, list_entry);
    printk("SJY_POP_NODE: Popping entry=%p from tail=%p\n", entry, g_char_device.list_tail);

    g_char_device.list_tail = g_char_device.list_tail->prev;
    list_del(&entry->list_entry);
    
    printk("SJY_POP_UPDATE: New list_tail=%p\n", g_char_device.list_tail);
    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    spec = entry->spec;
    kfree(entry);

    printk("SJY_POP_EXIT: Successfully popped spec=%p (type=%d)\n", spec, spec ? spec->type : -1);
    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
	return &g_char_device;
}

