/* Modifications Copyright 2025 Comcast Cable Communications Management, LLC
 * Licensed under the GPLv2.0 License
 */

#include "rdkfmac.h"

struct rdkfmac_device_data g_char_device;
static DECLARE_WAIT_QUEUE_HEAD(rdkfmac_rq); 
static wlan_emu_msg_data_t *pop_from_char_device(void);
static unsigned int get_list_entries_count_in_char_device(void);
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
	wlan_emu_msg_data_entry_t	*entry;
	wlan_emu_msg_data_t	*spec;
	char	str_spec_type[32] = {0};
	char	str_ops[128] = {0};
	unsigned int len = 0;

	if (!data) { 
		printk("SJY %s: ERR - Received NULL data\n", __func__); 
		return; 
	}

	printk("SJYC QUEUE push\n");
    printk("SJY: [CP 1] Entering %s. data_ptr=%p\n", __func__, data);

	// do not push to list if nobody is listening
    if (g_char_device.num_inst == 0) {
      printk("No listeners\n");

      if (data->type == wlan_emu_msg_type_frm80211)
        kfree(data->u.frm80211.u.frame.frame);

      kfree(data);
      return;
    }

    if (rdkfmac_emu80211_close == true) {
      printk("emu80211 closed\n");

      if (data->type == wlan_emu_msg_type_frm80211)
        kfree(data->u.frm80211.u.frame.frame);

      kfree(data);
      return;
    }

    printk("SJY: [CP 2] Allocating memory blocks\n");
	entry = kmalloc(sizeof(wlan_emu_msg_data_entry_t), GFP_KERNEL);
	if (entry == NULL) {
		printk("SJY kmalloc failed for entry\n");
		return;
	}
    printk("SJY: [CP 3] Allocated entry at %p\n", entry);

	spec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
	if (spec == NULL) {
		printk("SJY kmalloc failed for spec\n");
		kfree(entry);
		return;
	}
	printk("SJY: [CP 4] Setting entry->spec\n");
	entry->spec = spec;
    printk("SJY %s: [CP 5] Before main memcpy. src=%p dest=%p\n", __func__, data, spec);
	memcpy(spec, data, sizeof(wlan_emu_msg_data_t));
	spec->u.frm80211.u.frame.frame = NULL;

        printk("SJY DEBUG type=%d frame_ptr=%p frame_len=%u\n", data->type,
               data->u.frm80211.u.frame.frame,
               data->u.frm80211.u.frame.frame_len);

        /* ===== FIX: deep copy frame buffer ===== */
        if (spec->type == wlan_emu_msg_type_frm80211 &&
            data->u.frm80211.u.frame.frame != NULL &&
            data->u.frm80211.u.frame.frame_len > 0) {

           len = data->u.frm80211.u.frame.frame_len;

          if (len > 4096) {
            printk("SJY invalid frame length %u\n", len);
            kfree(spec);
            kfree(entry);
            return;
          }

          spec->u.frm80211.u.frame.frame = kmalloc(len, GFP_KERNEL);
          if (!spec->u.frm80211.u.frame.frame) {
            printk("SJY sep kmalloc failed for frame buffer\n");
            kfree(spec);
            kfree(entry);
            return;
          }

          memcpy(spec->u.frm80211.u.frame.frame, data->u.frm80211.u.frame.frame,
                 len);
        }

        printk("SJYC orig frame=%p copy frame=%p len=%u\n",
       data->u.frm80211.u.frame.frame,
       spec->u.frm80211.u.frame.frame,
       len);

        if (data->type == wlan_emu_msg_type_frm80211)
          kfree(data->u.frm80211.u.frame.frame);

        kfree(data);

        printk("SJY: [CP 6] memcpy success and spec->type is %d\n", spec->type);

        switch (spec->type) {
		case wlan_emu_msg_type_cfg80211:
			strcpy(str_spec_type, "cfg80211");
			printk("SJY cfg80211 ops: %d\n", spec->u.cfg80211.ops);
			strcpy(str_ops, rdkfmac_cfg80211_ops_type_to_string(spec->u.cfg80211.ops));
			break;

		case wlan_emu_msg_type_mac80211:
			strcpy(str_spec_type, "mac80211");
			printk("SJY mac80211 ops: %d\n", spec->u.mac80211.ops);
			strcpy(str_ops, rdkfmac_mac80211_ops_type_to_string(spec->u.mac80211.ops));
			break;

		case wlan_emu_msg_type_emu80211:
			strcpy(str_spec_type, "emu80211");
			printk("SJY emu80211 ops: %d\n", spec->u.emu80211.ops);
			strcpy(str_ops, rdkfmac_emu80211_ops_type_to_string(spec->u.emu80211.ops));
			break;

		case wlan_emu_msg_type_webconfig:
			strcpy(str_spec_type, "webconfig");
			strcpy(str_ops, "onewifi_webconfig");
			break;

		case wlan_emu_msg_type_agent:
			break;

		default:
			break;
	}
	printk("SJY: [CP 7] Switch complete. Type string: %s\n", str_spec_type);

	if ((spec->type != wlan_emu_msg_type_webconfig) && (spec->type != wlan_emu_msg_type_frm80211) &&
	(spec->type != wlan_emu_msg_type_agent)) {
		printk("%s:%d: pushing data to queue, type: %s ops: %s current size: %d\n", __func__, __LINE__,
			str_spec_type, str_ops, get_list_entries_count_in_char_device());
	}
	
	
	printk("SJY: [CP 8] Adding entry to list and the tail is %p\n", g_char_device.list_tail);
	list_add(&entry->list_entry, g_char_device.list_tail);

	printk("SJY: [CP 9] Adding the spec entry to global list tail\n");
	g_char_device.list_tail = &entry->list_entry;
    printk("SJY: [CP 10] List update success. Waking queue.\n");
	wake_up_interruptible(&rdkfmac_rq);

    printk("SJY: [CP 11] Exit successful.\n");
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
				printk("SJY Received emu80211 tstart control message and starting emu80211\n");
				rdkfmac_emu80211_close = false;
			} else if (spec->u.emu80211.u.ctrl.ctrl == wlan_emu_emu80211_ctrl_tstop) {
				printk("SJY Received emu80211 tstop control message and stopping emu80211\n");
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
	unsigned int f_len = 0;
    char *end_of_buff = read_buff + size; /* Safety boundary */

	printk("SJYC HANDLE FRM80211 start\n");
    /* [HND CP 1] Entry Check */
    printk("SJY: [HND CP 1] Entering %s. buff=%p, total_size=%zu\n", __func__, read_buff, size);

    frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (frm80211_msg == NULL) {
        printk("SJY: [HND ERR] kzalloc failed for frm80211_msg\n");
        return;
    }


    /* [HND CP 2] Type Header Extraction */
    if (read_buff + sizeof(wlan_emu_msg_type_t) > end_of_buff) goto oob_error;
    memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
    read_buff += sizeof(wlan_emu_msg_type_t);

    if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
        printk("SJY: [HND ERR] Invalid type %d\n", frm80211_msg->type);
        kfree(frm80211_msg);
        return;
    }

    /* [HND CP 3] Metadata Extraction (Ops, Len, MACs) */
    printk("SJY: [HND CP 3] Extracting frame metadata\n");
    
    // Skip ops dummy value
    if (read_buff + sizeof(wlan_emu_cfg80211_ops_type_t) > end_of_buff) goto oob_error;
    read_buff += sizeof(wlan_emu_cfg80211_ops_type_t);

    // Get Frame Len
    if (read_buff + sizeof(unsigned int) > end_of_buff) goto oob_error;
    memcpy(&frm80211_msg->u.frm80211.u.frame.frame_len, read_buff, sizeof(unsigned int));
    read_buff += sizeof(unsigned int);

    // Get MACs
    if (read_buff + (ETH_ALEN * 2) > end_of_buff) goto oob_error;
    memcpy(&frm80211_msg->u.frm80211.u.frame.macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;
    memcpy(&frm80211_msg->u.frm80211.u.frame.client_macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;

    /* [HND CP 4] Nested Allocation */
    f_len = frm80211_msg->u.frm80211.u.frame.frame_len;
    printk("SJY: [HND CP 4] Allocating frame buffer of size: %u\n", f_len);
    
    if (f_len == 0 || f_len > 4000) { /* Sane limit check */
        printk("SJY: [HND ERR] Insane frame length detected!\n");
        kfree(frm80211_msg);
        return;
    }

    frm80211_msg->u.frm80211.u.frame.frame = kzalloc(f_len, GFP_KERNEL);
    if (frm80211_msg->u.frm80211.u.frame.frame == NULL) {
        printk("SJY: [HND ERR] kzalloc failed for frame data\n");
        kfree(frm80211_msg);
        return;
    }

    /* [HND CP 5] Frame Data Copy */
    if (read_buff + f_len > end_of_buff) goto oob_error_nested;
    memcpy(frm80211_msg->u.frm80211.u.frame.frame, read_buff, f_len);
    read_buff += f_len;

    /* [HND CP 6] Protocol Analysis */
    printk("SJY: [HND CP 6] Analyzing 802.11 headers\n");
    hdr = (struct ieee80211_hdr *)frm80211_msg->u.frm80211.u.frame.frame;
    fc = le16_to_cpu(hdr->frame_control);
    type = WLAN_FC_GET_TYPE(fc);

    if (type == WLAN_FC_TYPE_MGMT) {
        stype = WLAN_FC_GET_STYPE(fc);
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
                printk("SJY: [HND ERR] Invalid fc subtype: %d\n", stype);
                goto cleanup_fail;
        }
    } else if (type == WLAN_FC_TYPE_DATA) {
        /* ... Data/EAPOL Logic ... */
        data_header_len = ieee80211_hdrlen(hdr->frame_control);
        if (f_len < data_header_len + sizeof(rfc1042_hdr) + 2) goto cleanup_fail;
        
        tmp_frame_buf = (unsigned char *)frm80211_msg->u.frm80211.u.frame.frame + data_header_len;
        if (memcmp(tmp_frame_buf, rfc1042_hdr, sizeof(rfc1042_hdr)) != 0) goto cleanup_fail;
        
        tmp_frame_buf += sizeof(rfc1042_hdr);
        if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) == ETH_P_PAE) {
            msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
        } else {
            goto cleanup_fail;
        }
    }

    /* [HND CP 7] Final Dispatch */
    printk("SJY: [HND CP 7] Final ops type %d. Calling push_to_char_device\n", msg_ops_type);
    memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
    printk("SJYC PUSH start frame_len from %s\n", __func__);
    push_to_char_device(frm80211_msg);

    /* [HND CP 8] Exit Cleanup */
    printk("SJY: [HND CP 8] Handler success. Cleaning up.\n");
    //kfree(frm80211_msg->u.frm80211.u.frame.frame);
    //kfree(frm80211_msg);
    return;

oob_error_nested:
    kfree(frm80211_msg->u.frm80211.u.frame.frame);
oob_error:
    printk("SJY: [HND ERR] Buffer Overflow prevented! read_buff exceeded size.\n");
    kfree(frm80211_msg);
    return;

cleanup_fail:
    kfree(frm80211_msg->u.frm80211.u.frame.frame);
    kfree(frm80211_msg);
}

static ssize_t rdkfmac_write(struct file *file, const char __user *user_buffer,
                    size_t size, loff_t * offset)
{
    wlan_emu_msg_data_t *pSpec = NULL;
    char *read_buff = NULL;
    ssize_t sz = 0;

	printk("SJYC WRITE START size=%zu\n", size);
    /* [WR CP 1] Entry Point */
    printk("SJY: [WR CP 1] Entering %s. User buffer: %p, size: %zu\n", __func__, user_buffer, size);

    /* [WR CP 2] Allocation Trace */
    pSpec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (pSpec == NULL) {
        printk("SJY: [WR ERR] kmalloc failed for pSpec\n");
        return 0; 
    }

    read_buff = kmalloc(size, GFP_KERNEL);
    if (read_buff == NULL) {
        printk("SJY: [WR ERR] kmalloc failed for read_buff\n");
        kfree(pSpec);
        return 0;
    }
    printk("SJY: [WR CP 2] Buffers allocated successfully. pSpec=%p, read_buff=%p\n", pSpec, read_buff);

    /* [WR CP 3] Memory Reset */
    memset(read_buff, 0, size);

    /* [WR CP 4] User-to-Kernel Copy (CRASH RISK) */
    /* If the user-space pointer is bad, the kernel will panic here */
    if (copy_from_user(read_buff, user_buffer, size)) {
        printk("SJY: [WR ERR] copy_from_user failed\n");
        kfree(pSpec);
        kfree(read_buff);
        return 0;
    }
    printk("SJY: [WR CP 4] copy_from_user success\n");

    /* [WR CP 5] Identifying Message Type */
    printk("SJY: [WR CP 5] Copying msg type (size %zu) from buffer\n", sizeof(wlan_emu_msg_type_t));
    memcpy((char*)&pSpec->type, read_buff, sizeof(wlan_emu_msg_type_t));
    printk("SJY: [WR CP 6] The pSpec->type is %d\n", pSpec->type);

    /* [WR CP 7] Branching Logic */
    switch (pSpec->type) {
        case wlan_emu_msg_type_frm80211:
            printk("SJY: [WR CP 7a] Calling handle_frm80211_msg_w\n");
            handle_frm80211_msg_w(read_buff, size);
            sz = size;
            break;

        case wlan_emu_msg_type_emu80211:
            printk("SJY: [WR CP 7b] Copying full struct and calling handle_emu80211_msg_w\n");
            memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
            handle_emu80211_msg_w(pSpec);
            sz = sizeof(wlan_emu_msg_data_t);
            break;

        case wlan_emu_msg_type_webconfig:
            printk("SJY: [WR CP 7c] Copying full struct and calling push_to_char_device\n");
            memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
            push_to_char_device(pSpec);
            sz = sizeof(wlan_emu_msg_data_t);
            break;

        case wlan_emu_msg_type_agent:
            printk("SJY: [WR CP 7d] Copying full struct and calling handle_agent_msg_w\n");
            memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
            handle_agent_msg_w(pSpec);
            sz = sizeof(wlan_emu_msg_data_t);
            break;

        default:
            printk("SJY: [WR ERR] Invalid operation type: %d\n", pSpec->type);
            sz = 0;
            break;
    }

    /* [WR CP 8] Cleanup */
    printk("SJY: [WR CP 8] Freeing local buffers\n");
    kfree(read_buff);
    kfree(pSpec);

    printk("SJY: [WR CP 9] Exit successfully with sz=%zd\n", sz);
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
	int list_count = 0;

	printk("SJYC READ called\n");
    /* [RD CP 1] Entry Check */
    printk("SJY: [RD CP 1] Entering %s. User buffer: %p, available size: %zu\n", __func__, user_buffer, size);
    
    /* [RD CP 2] Count Check - HIGH RISK 
       If it crashes here, the linked list is circular or corrupted. */
    printk("SJY: [RD CP 2] Calling count check\n");
    list_count = get_list_entries_count_in_char_device();
    printk("SJY: [RD CP 3] Current list size is %d\n", list_count);

    /* [RD CP 4] Popping Data - HIGH RISK 
       If it crashes here, list_tail is pointing to a bad address. */
    printk("SJY: [RD CP 4] Calling pop_from_char_device\n");
    spec = pop_from_char_device();
    
    if (spec == NULL) {
        printk("SJY: [RD CP 5] No data found in queue. Returning 0.\n");
        return 0;
    }

    /* [RD CP 6] Allocation Trace */
    printk("SJY: [RD CP 6] Popped spec at %p. Allocating send_buff of size %zu\n", spec, size);
    send_buff = kmalloc(size, GFP_KERNEL);
    if (send_buff == NULL) {
        printk("SJY: [RD ERR] kmalloc failed for send_buff\n");
        /* Preservation: We need to free the spec we just popped to avoid a leak */
        if (spec->type == wlan_emu_msg_type_frm80211) kfree(spec->u.frm80211.u.frame.frame);
        kfree(spec);
        return 0;
    }

    memset(send_buff, 0, size);
    s_tmp = send_buff;

    /* [RD CP 7] Type Dispatch */
    printk("SJY: [RD CP 7] Identified spec->type as %d. Entering switch.\n", spec->type);

    switch (spec->type) {
        case wlan_emu_msg_type_cfg80211:
            handle_cfg80211_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_emu80211:
		    printk("SJY Calling handle_emu80211_msg\n");
            handle_emu80211_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_frm80211:
            /* [RD CP 8a] Frame Check: Verify nested frame pointer before handler */
            printk("SJY: [RD CP 8a] Nested Frame Ptr: %p, Len: %u\n", 
                   spec->u.frm80211.u.frame.frame, spec->u.frm80211.u.frame.frame_len);
            handle_frm80211_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_webconfig:
            handle_webconfig_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_agent:
            handle_agent_msg(spec, &return_len, s_tmp);
            break;
        default:
            printk("SJY: [RD ERR] Unknown spec type popped: %d\n", spec->type);
            break;
    }

    /* [RD CP 9] User Copy - CRASH RISK if return_len is larger than size */
    printk("SJY: [RD CP 9] Preparing copy_to_user. return_len=%zd\n", return_len);
    if (copy_to_user(user_buffer, send_buff, return_len)) {
        printk("SJY: [RD ERR] copy_to_user failed\n");
        /* Return error code without crashing cleanup */
        return_len = -EFAULT;
    }

    /* [RD CP 10] Cleanup Sequence */
    printk("SJY: [RD CP 10] Starting cleanup.\n");
    if (spec->type == wlan_emu_msg_type_frm80211) {
        printk("SJY: [RD CP 11] Freeing nested frame at %p\n", spec->u.frm80211.u.frame.frame);
        kfree(spec->u.frm80211.u.frame.frame);
    }

    kfree(spec);
    kfree(send_buff);

    /* [RD CP 12] Final Exit */
    printk("SJY: [RD CP 12] Exit. Final return_len to user: %zd\n", return_len);
    return return_len;
}

static int rdkfmac_open(struct inode *inode, struct file *file)
{
    /* [OPN CP 1] Entry */
    printk("SJY: [OPN CP 1] Entering %s\n", __func__);

    g_char_device.num_inst++;
    
    /* [OPN CP 2] Verification */
    printk("SJY: [OPN CP 2] Open Success. Current Instances: %d\n", g_char_device.num_inst);

    return 0;
}

static int rdkfmac_release(struct inode *inode, struct file *file)
{
    /* [REL CP 1] Entry */
    printk("SJY: [REL CP 1] Entering %s. Instances before dec: %d\n", __func__, g_char_device.num_inst);

    if (g_char_device.num_inst > 0) {
        g_char_device.num_inst--;
    } else {
        /* [REL ERR] Underflow Guard */
        printk("SJY: [REL ERR] Attempted to release with 0 instances!\n");
    }

    /* [REL CP 2] Logic Check */
    if (g_char_device.num_inst == 0) {
        rdkfmac_emu80211_close = true; 
        printk("SJY: [REL CP 3] Last instance closed. rdkfmac_emu80211_close set to TRUE\n");
    }

    /* [REL CP 4] Final Exit */
    printk("SJY: [REL CP 4] Release successful. Final Count: %d\n", g_char_device.num_inst);
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
	struct list_head *ptr = &g_char_device.list_head;

	for (ptr = &g_char_device.list_head; ptr != g_char_device.list_tail; ptr = ptr->next) {
		count++;
		if (count > 5000) { 
            printk("SJY: [CNT ERR] List count exceeded 5000! Possible circular corruption.\n");
            break; 
        }
	}
	printk("SJY: [CNT CP 3] Final count identified: %u\n", count);
	return count;
}

wlan_emu_msg_data_t* pop_from_char_device(void)
{
    wlan_emu_msg_data_t *spec = NULL;
    wlan_emu_msg_data_entry_t *entry = NULL;

    /* [CP 1] Entry Check */
    printk("SJY: [POP CP 1] Entering %s\n", __func__);

    /* [CP 2] Empty Check - High Risk if tail is corrupted */
    printk("SJY: [POP CP 2] Checking if list is empty. Tail: %p, Head: %p\n", 
           g_char_device.list_tail, &g_char_device.list_head);
    
    if (g_char_device.list_tail == &g_char_device.list_head) {
        printk("SJY: [POP CP 3] List is empty, returning NULL\n");
        return NULL;
    }

    /* [CP 4] Entry Retrieval - CRASH RISK if list structure is broken */
    printk("SJY: [POP CP 4] Retrieving entry from tail %p\n", g_char_device.list_tail);
    entry = list_entry(g_char_device.list_tail, wlan_emu_msg_data_entry_t, list_entry);
    
    if (!entry) {
        printk("SJY: [POP ERR] entry is NULL after list_entry macro!\n");
        return NULL;
    }

    /* [CP 5] Pointer Update - CRASH RISK if prev is a poisoned address */
    printk("SJY: [POP CP 5] Updating tail to prev: %p\n", g_char_device.list_tail->prev);
    g_char_device.list_tail = g_char_device.list_tail->prev;

    /* [CP 6] Node Deletion */
    printk("SJY: [POP CP 6] Deleting entry from list\n");
    list_del(&entry->list_entry);

    /* [CP 7] Extraction and Free */
    spec = entry->spec;
    printk("SJY: [POP CP 7] Popped spec: %p. Freeing entry node: %p\n", spec, entry);
    kfree(entry);

    printk("SJY: [POP CP 8] Success. Returning spec.\n");
    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
	return &g_char_device;
}

