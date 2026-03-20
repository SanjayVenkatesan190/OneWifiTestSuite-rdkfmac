/* Modifications Copyright 2025 Comcast Cable Communications Management, LLC
 * Licensed under the GPLv2.0 License
 */

#include "rdkfmac.h"

struct rdkfmac_device_data g_char_device;
static DECLARE_WAIT_QUEUE_HEAD(rdkfmac_rq); 
static wlan_emu_msg_data_t *pop_from_char_device(void);
static unsigned int get_list_entries_count_in_char_device(void);
static bool  rdkfmac_emu80211_close = true;
static spinlock_t g_char_device_list_lock;

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
    wlan_emu_msg_data_entry_t *entry = NULL;
    wlan_emu_msg_data_t *spec = NULL;
    char str_spec_type[32] = {0};
    char str_ops[128] = {0};
    u32 len = 0;
    unsigned long flags;

    printk("SJY ENTER %s data=%p\n", __func__, data);

    /* Validate input */
    if (!data) {
        printk("SJY ERROR: NULL data received\n");
        return;
    }

    printk("SJY DEBUG: type=%d frame=%p len=%u\n",
           data->type,
           data->u.frm80211.u.frame.frame,
           data->u.frm80211.u.frame.frame_len);

    printk("SJY DEBUG: listeners=%d emu_close=%d\n",
           g_char_device.num_inst,
           rdkfmac_emu80211_close);

    /* Skip if nobody listening (Early check) */
    if (g_char_device.num_inst == 0) {
        printk("SJY No listeners, dropping message\n");
        return;
    }

    /* Skip if emu closed (Early check) */
    if (rdkfmac_emu80211_close == true) {
        printk("SJY emu80211 closed, dropping message\n");
        return;
    }

    /* Allocate entry (Using GFP_ATOMIC for Interrupt Safety) */
    printk("SJY Allocating entry struct\n");
    entry = kmalloc(sizeof(wlan_emu_msg_data_entry_t), GFP_ATOMIC);
    if (!entry) {
        printk("SJY ERROR: kmalloc failed for entry\n");
        return;
    }
    printk("SJY entry allocated %p\n", entry);

    /* Allocate spec (Using GFP_ATOMIC for Interrupt Safety) */
    printk("SJY Allocating spec struct\n");
    spec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_ATOMIC);
    if (!spec) {
        printk("SJY ERROR: kmalloc failed for spec\n");
        kfree(entry);
        return;
    }
    printk("SJY spec allocated %p\n", spec);

    entry->spec = spec;

    printk("SJY Before memcpy src=%p dst=%p size=%zu\n",
           data, spec, sizeof(wlan_emu_msg_data_t));

    memcpy(spec, data, sizeof(wlan_emu_msg_data_t));

    /* prevent shallow pointer copy */
    spec->u.frm80211.u.frame.frame = NULL;

    printk("SJY After memcpy spec->type=%d\n", spec->type);

    /* ===== Deep copy frame buffer ===== */
    if (spec->type == wlan_emu_msg_type_frm80211 &&
        data->u.frm80211.u.frame.frame != NULL &&
        data->u.frm80211.u.frame.frame_len > 0) {

        len = data->u.frm80211.u.frame.frame_len;

        printk("SJY Deep copy triggered len=%u\n", len);

        /* Malformed length guard */
        if (len > 4096) {
            printk("SJY ERROR: invalid frame length %u\n", len);
            kfree(spec);
            kfree(entry);
            return;
        }

        spec->u.frm80211.u.frame.frame = kmalloc(len, GFP_ATOMIC);

        if (!spec->u.frm80211.u.frame.frame) {
            printk("SJY ERROR: frame buffer allocation failed\n");
            kfree(spec);
            kfree(entry);
            return;
        }

        printk("SJY frame buffer allocated %p\n",
               spec->u.frm80211.u.frame.frame);

        memcpy(spec->u.frm80211.u.frame.frame,
               data->u.frm80211.u.frame.frame,
               len);

        printk("SJY frame memcpy done\n");
    }
    else {
        printk("SJY Deep copy skipped type=%d frame=%p len=%u\n",
               spec->type,
               data->u.frm80211.u.frame.frame,
               data->u.frm80211.u.frame.frame_len);
    }

    printk("SJY Frame debug orig=%p copy=%p len=%u\n",
           data->u.frm80211.u.frame.frame,
           spec->u.frm80211.u.frame.frame,
           spec->u.frm80211.u.frame.frame_len);

    /* Identify message type */
    switch (spec->type) {

    case wlan_emu_msg_type_cfg80211:
        strcpy(str_spec_type, "cfg80211");
        printk("SJY cfg80211 ops=%d\n", spec->u.cfg80211.ops);
        strcpy(str_ops,
               rdkfmac_cfg80211_ops_type_to_string(spec->u.cfg80211.ops));
        break;

    case wlan_emu_msg_type_mac80211:
        strcpy(str_spec_type, "mac80211");
        printk("SJY mac80211 ops=%d\n", spec->u.mac80211.ops);
        strcpy(str_ops,
               rdkfmac_mac80211_ops_type_to_string(spec->u.mac80211.ops));
        break;

    case wlan_emu_msg_type_emu80211:
        strcpy(str_spec_type, "emu80211");
        printk("SJY emu80211 ops=%d\n", spec->u.emu80211.ops);
        strcpy(str_ops,
               rdkfmac_emu80211_ops_type_to_string(spec->u.emu80211.ops));
        break;

    case wlan_emu_msg_type_webconfig:
        strcpy(str_spec_type, "webconfig");
        strcpy(str_ops, "onewifi_webconfig");
        break;

    case wlan_emu_msg_type_agent:
        strcpy(str_spec_type, "agent");
        break;

    case wlan_emu_msg_type_frm80211:
        strcpy(str_spec_type, "frm80211");
        break;

    default:
        strcpy(str_spec_type, "unknown");
        break;
    }

    printk("SJY Message type resolved: %s ops=%s\n",
           str_spec_type, str_ops);

    /* * Print BEFORE lock to prevent deadlocking with get_list_entries_count.
     * We use list_head.prev as the standard API equivalent to the old list_tail.
     */
    printk("SJY Queue before insert count=%d tail=%p\n",
           get_list_entries_count_in_char_device(),
           g_char_device.list_head.prev);

    /* ======================================= */
    /* THE SHIELD: Enter Critical Section      */
    /* ======================================= */
    spin_lock_irqsave(&g_char_device_list_lock, flags);

    /* Re-check state inside the lock to prevent the "Phantom Push" race */
    if (g_char_device.num_inst == 0 || rdkfmac_emu80211_close) {
        spin_unlock_irqrestore(&g_char_device_list_lock, flags);
        if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame) {
            kfree(spec->u.frm80211.u.frame.frame);
        }
        kfree(spec);
        kfree(entry);
        return;
    }

    /* Safely add to standard list (This updates head.prev automatically) */
    list_add_tail(&entry->list_entry, &g_char_device.list_head);

    printk("SJY list_add done entry=%p\n", entry);

    /* Print the new tail (which is now correctly updated via list_add_tail) */
    printk("SJY list_tail updated to %p\n", g_char_device.list_head.prev);

    spin_unlock_irqrestore(&g_char_device_list_lock, flags);
    /* ======================================= */
    /* END Critical Section                    */
    /* ======================================= */

    /* Wake reader */
    wake_up_interruptible(&rdkfmac_rq);

    printk("SJY wake_up_interruptible called\n");
    printk("SJY EXIT %s success\n", __func__);
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
				printk("SJY %s:%d: Received emu80211 tstart control message\n", __func__, __LINE__);
				rdkfmac_emu80211_close = false;
			} else if (spec->u.emu80211.u.ctrl.ctrl == wlan_emu_emu80211_ctrl_tstop) {
				printk("SJY %s:%d: Received emu80211 tstop control message\n", __func__, __LINE__);
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
	unsigned int f_len = 0;
    size_t rem_size = size; /* TRACK REMAINING BYTES */

    /* 1. Header Validation */
    if (rem_size < (sizeof(wlan_emu_msg_type_t) + sizeof(wlan_emu_cfg80211_ops_type_t) + 
                    sizeof(unsigned int) + (ETH_ALEN * 2))) {
        printk("SJY ERROR: Buffer too small for frm80211 metadata\n");
        return;
    }

    frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (frm80211_msg == NULL) return;

    /* Safe Copy: Type */
    memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
    read_buff += sizeof(wlan_emu_msg_type_t);
    rem_size -= sizeof(wlan_emu_msg_type_t);

    if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
        kfree(frm80211_msg);
        return;
    }

    /* Safe Copy: Ops (dummy) and Frame Len */
    read_buff += sizeof(wlan_emu_cfg80211_ops_type_t);
    rem_size -= sizeof(wlan_emu_cfg80211_ops_type_t);

    memcpy(&frm80211_msg->u.frm80211.u.frame.frame_len, read_buff, sizeof(unsigned int));
    read_buff += sizeof(unsigned int);
    rem_size -= sizeof(unsigned int);

    /* Safe Copy: MAC Addresses */
    memcpy(&frm80211_msg->u.frm80211.u.frame.macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;
    rem_size -= ETH_ALEN;

    memcpy(&frm80211_msg->u.frm80211.u.frame.client_macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;
    rem_size -= ETH_ALEN;

    /* 2. CRITICAL FIX: Validate the internal Frame Length */
    f_len = frm80211_msg->u.frm80211.u.frame.frame_len;
    
    if (f_len == 0 || f_len > rem_size || f_len > 4096) {
        printk("SJY ERROR: Malformed frame length %u (Remaining buffer: %zu)\n", f_len, rem_size);
        kfree(frm80211_msg);
        return;
    }

    /* 3. Safe Allocation and Copy of the frame content */
    frm80211_msg->u.frm80211.u.frame.frame = kzalloc(f_len, GFP_KERNEL);
    if (frm80211_msg->u.frm80211.u.frame.frame == NULL) {
        kfree(frm80211_msg);
        return;
    }

    memcpy(frm80211_msg->u.frm80211.u.frame.frame, read_buff, f_len);

    /* 4. Protocol Analysis (remains same, but now on safe memory) */
    hdr = (struct ieee80211_hdr *)frm80211_msg->u.frm80211.u.frame.frame;
    fc = le16_to_cpu(hdr->frame_control);
    type = WLAN_FC_GET_TYPE(fc);

    if (type == WLAN_FC_TYPE_MGMT) {
        stype = WLAN_FC_GET_STYPE(fc);
        switch (stype) {
            case WLAN_FC_STYPE_PROBE_REQ:    msg_ops_type = wlan_emu_frm80211_ops_type_prb_req; break;
            case WLAN_FC_STYPE_PROBE_RESP:   msg_ops_type = wlan_emu_frm80211_ops_type_prb_resp; break;
            case WLAN_FC_STYPE_ASSOC_REQ:    msg_ops_type = wlan_emu_frm80211_ops_type_assoc_req; break;
            case WLAN_FC_STYPE_ASSOC_RESP:   msg_ops_type = wlan_emu_frm80211_ops_type_assoc_resp; break;
            case WLAN_FC_STYPE_AUTH:         msg_ops_type = wlan_emu_frm80211_ops_type_auth; break;
            case WLAN_FC_STYPE_DEAUTH:       msg_ops_type = wlan_emu_frm80211_ops_type_deauth; break;
            case WLAN_FC_STYPE_DISASSOC:     msg_ops_type = wlan_emu_frm80211_ops_type_disassoc; break;
            case WLAN_FC_STYPE_ACTION:       msg_ops_type = wlan_emu_frm80211_ops_type_action; break;
            case WLAN_FC_STYPE_REASSOC_REQ:  msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_req; break;
            case WLAN_FC_STYPE_REASSOC_RESP: msg_ops_type = wlan_emu_frm80211_ops_type_reassoc_resp; break;
            default:
                kfree(frm80211_msg->u.frm80211.u.frame.frame);
                kfree(frm80211_msg);
                return;
        }
    } else if (type == WLAN_FC_TYPE_DATA) {
        data_header_len = ieee80211_hdrlen(hdr->frame_control);
        if (f_len < data_header_len + sizeof(rfc1042_hdr) + 2) {
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }
        tmp_frame_buf = (unsigned char *)frm80211_msg->u.frm80211.u.frame.frame + data_header_len;
        if (memcmp(tmp_frame_buf, rfc1042_hdr, sizeof(rfc1042_hdr)) == 0) {
             tmp_frame_buf += sizeof(rfc1042_hdr);
             if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) == ETH_P_PAE) {
                 msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
             }
        }
    }

    /* 5. Final Dispatch */
    frm80211_msg->u.frm80211.ops = msg_ops_type;
    push_to_char_device(frm80211_msg);

    /* Cleanup: push_to_char_device performs its own deep copy, so we must free this local instance */
    kfree(frm80211_msg->u.frm80211.u.frame.frame);
    kfree(frm80211_msg);
}

static ssize_t rdkfmac_write(struct file *file, const char __user *user_buffer,
                    size_t size, loff_t * offset)
{
    wlan_emu_msg_data_t *pSpec = NULL;
    ssize_t sz = 0;
    char *read_buff = NULL;

    /* 1. Basic Size Guard */
    if (size < sizeof(wlan_emu_msg_type_t)) {
        printk("SJY ERROR: Buffer too small to contain a message type (%zu bytes)\n", size);
        return -EINVAL;
    }

    pSpec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    read_buff = kmalloc(size, GFP_KERNEL);

    if (!pSpec || !read_buff) {
        printk("%s:%d: kmalloc failed\n", __func__, __LINE__);
        kfree(pSpec);
        kfree(read_buff);
        return 0;
    }

    if (copy_from_user(read_buff, user_buffer, size)) {
        printk("%s:%d: copy_from_user error\n", __func__, __LINE__);
        kfree(pSpec);
        kfree(read_buff);
        return 0;
    }

    /* Identify type safely */
    pSpec->type = *(wlan_emu_msg_type_t *)read_buff;
    
    switch (pSpec->type) {
        case wlan_emu_msg_type_frm80211:
            /* This handler parses read_buff itself */
            handle_frm80211_msg_w(read_buff, size);
            sz = size;
            break;

        case wlan_emu_msg_type_emu80211:
        case wlan_emu_msg_type_webconfig:
        case wlan_emu_msg_type_agent:
            /* CRITICAL FIX: Ensure user provided enough data for a full struct */
            /* Without this, memcpy reads into adjacent kernel memory (ASCII GHOSTS) */
            if (size < sizeof(wlan_emu_msg_data_t)) {
                printk("SJY ERROR: size %zu is too small for struct %zu\n", 
                       size, sizeof(wlan_emu_msg_data_t));
                sz = -EINVAL;
                break;
            }

            memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));

            if (pSpec->type == wlan_emu_msg_type_emu80211) {
                handle_emu80211_msg_w(pSpec);
            } else if (pSpec->type == wlan_emu_msg_type_webconfig) {
                push_to_char_device(pSpec);
            } else {
                handle_agent_msg_w(pSpec);
            }
            sz = sizeof(wlan_emu_msg_data_t);
            break;

        default:
            printk("SJY ERROR: Invalid message type %d\n", pSpec->type);
            sz = -EINVAL;
            break;
    }

    kfree(read_buff);
    kfree(pSpec);
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
		printk(KERN_INFO "SJY %s:%d: NULL Pointer \n", __func__, __LINE__);
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
		    printk("SJY %s:%d: Unknown emu80211 operation\n", __func__, __LINE__);
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

static void handle_frame(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp, size_t max_size)
{
	unsigned int f_len;
	size_t total_needed = 0;
	
	f_len = spec->u.frm80211.u.frame.frame_len;
    
    /* 1. Calculate exactly how many bytes we need to write */
    total_needed = sizeof(wlan_emu_msg_type_t) + 
                          sizeof(wlan_emu_frm80211_ops_type_t) + 
                          sizeof(unsigned int) + 
                          f_len + 
                          (ETH_ALEN * 2);

    /* 2. THE SHIELD: Prevent Buffer Overflow right at the source */
    if (*len + total_needed > max_size) {
        printk("SJY FATAL: %s: Buffer overflow blocked! (Needed: %zu, Max: %zu)\n", 
               __func__, total_needed, max_size);
        return; /* Abort the copy to save the kernel */
    }
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

static void handle_frm80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp, size_t max_size)
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
			handle_frame(spec, len, s_tmp, max_size);
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
    int ret;
    size_t required_size = 0;

    printk("SJY ENTER %s: user requested size=%zu\n", __func__, size);

    /* 1. Safely wait for and pop a packet */
    while ((spec = pop_from_char_device()) == NULL) {
        if (file->f_flags & O_NONBLOCK) {
            printk("SJY %s: Queue empty, returning -EAGAIN (non-blocking)\n", __func__);
            return -EAGAIN;
        }
        
        printk("SJY %s: Queue empty, sleeping...\n", __func__);
        if (wait_event_interruptible(rdkfmac_rq, get_list_entries_count_in_char_device() != 0)) {
            printk("SJY %s: Woken up by signal (Ctrl+C), returning -ERESTARTSYS\n", __func__);
            return -ERESTARTSYS; 
        }
        printk("SJY %s: Woken up, attempting to pop again...\n", __func__);
    }

    printk("SJY %s: Successfully popped spec=%p, type=%d\n", __func__, spec, spec->type);

    /* 2. CRITICAL GUARD: Calculate how much space this specific packet requires */
    if (spec->type == wlan_emu_msg_type_frm80211) {
        required_size = sizeof(wlan_emu_msg_type_t) + 
                        sizeof(wlan_emu_frm80211_ops_type_t) + 
                        sizeof(unsigned int) + 
                        (ETH_ALEN * 2) + 
                        spec->u.frm80211.u.frame.frame_len;
                        
        printk("SJY %s: Calculated frm80211 required_size=%zu (frame_len=%u)\n", 
               __func__, required_size, spec->u.frm80211.u.frame.frame_len);
    } else {
        /* For all other types, the max possible size is the struct itself */
        required_size = sizeof(wlan_emu_msg_data_t);
        printk("SJY %s: Calculated standard required_size=%zu\n", __func__, required_size);
    }

    /* 3. Reject the read if user space didn't provide a large enough buffer */
    if (size < required_size) {
        printk("SJY ERROR: User buffer (%zu) too small for packet (%zu)\n", size, required_size);
        ret = -ENOBUFS; /* "No buffer space available" */
        goto cleanup_spec;
    }

    /* 4. Safe Allocation */
    printk("SJY %s: Allocating send_buff of size %zu\n", __func__, required_size);
    send_buff = kmalloc(required_size, GFP_KERNEL);
    if (!send_buff) {
        printk("SJY ERROR: %s: kmalloc failed for send_buff\n", __func__);
        ret = -ENOMEM;
        goto cleanup_spec;
    }

    memset(send_buff, 0, required_size);
    s_tmp = send_buff;

    /* 5. Process the specific message types */
    printk("SJY %s: Dispatching to handler for type %d\n", __func__, spec->type);
    switch (spec->type) {
        case wlan_emu_msg_type_cfg80211:
            handle_cfg80211_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_emu80211:
            handle_emu80211_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_frm80211:
            handle_frm80211_msg(spec, &return_len, s_tmp, size);
            break;
        case wlan_emu_msg_type_webconfig:
            handle_webconfig_msg(spec, &return_len, s_tmp);
            break;
        case wlan_emu_msg_type_agent:
            handle_agent_msg(spec, &return_len, s_tmp);
            break;
        default:
            printk("SJY WARN: %s: Unhandled spec type %d\n", __func__, spec->type);
            break;
    }

    /* 6. Copy to User Space */
    ret = return_len;
    printk("SJY %s: Handlers returned len=%zd, copying to user...\n", __func__, return_len);
    
    if (copy_to_user(user_buffer, send_buff, return_len)) {
        printk("SJY ERROR: %s: copy_to_user failed\n", __func__);
        ret = -EFAULT;
    } else {
        printk("SJY %s: copy_to_user success\n", __func__);
    }

    printk("SJY %s: Freeing send_buff=%p\n", __func__, send_buff);
    kfree(send_buff);

cleanup_spec:
    /* 7. Guaranteed Cleanup of the popped packet */
    if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame) {
        printk("SJY %s: Freeing deep-copied frame buffer=%p\n", __func__, spec->u.frm80211.u.frame.frame);
        kfree(spec->u.frm80211.u.frame.frame);
    }
    
    printk("SJY %s: Freeing base spec struct=%p\n", __func__, spec);
    kfree(spec);

    printk("SJY EXIT %s returning %d\n", __func__, ret);
    return ret;
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
	spin_lock_init(&g_char_device_list_lock); // Initialize spinlock
	printk(KERN_INFO "%s:%d: registered successfully\n", __func__, __LINE__);
	g_char_device.tdev = MKDEV(RDKFMAC_MAJOR, 0);
	g_char_device.dev = device_create(g_char_device.class, NULL,
				g_char_device.tdev, NULL, RDKFMAC_DEVICE_NAME);

	return 0;
}

void cleanup_rdkfmac_cdev(void)
{
    wlan_emu_msg_data_t *spec;
    unsigned int drained_count = 0;

    printk(KERN_INFO "SJY ENTER %s: Draining remaining packets...\n", __func__);

    /* 1. DRAIN THE QUEUE to prevent Slab Memory Leaks */
    while ((spec = pop_from_char_device()) != NULL) {
        /* Free the deep-copied frame buffer if it exists */
        if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame != NULL) {
            kfree(spec->u.frm80211.u.frame.frame);
        }
        /* Free the spec struct itself */
        kfree(spec);
        drained_count++;
    }

    if (drained_count > 0) {
        printk(KERN_INFO "SJY %s: Successfully freed %u orphaned packets.\n", __func__, drained_count);
    }

    /* 2. Standard Kernel Teardown */
    device_destroy(g_char_device.class, g_char_device.tdev);
    class_destroy(g_char_device.class);
    cdev_del(&g_char_device.cdev);
    unregister_chrdev_region(MKDEV(RDKFMAC_MAJOR, 0), 1);

    printk(KERN_INFO "%s:%d: unregistered successfully\n", __func__, __LINE__);
}

unsigned int get_list_entries_count_in_char_device(void)
{
    unsigned int count = 0;
    struct list_head *ptr;
    unsigned long flags;

    spin_lock_irqsave(&g_char_device_list_lock, flags);

    /* list_for_each is a safe kernel macro that auto-terminates */
    list_for_each(ptr, &g_char_device.list_head) {
        count++;
    }

    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    return count;
}

wlan_emu_msg_data_t* pop_from_char_device(void)
{
    wlan_emu_msg_data_t *spec = NULL;
    wlan_emu_msg_data_entry_t *entry = NULL;
    unsigned long flags;

    spin_lock_irqsave(&g_char_device_list_lock, flags);

    if (list_empty(&g_char_device.list_head)) {
        spin_unlock_irqrestore(&g_char_device_list_lock, flags);
        return NULL;
    }

    /* GET OLDEST (First) entry - FIFO ordering */
    entry = list_first_entry(&g_char_device.list_head, wlan_emu_msg_data_entry_t, list_entry);
    list_del(&entry->list_entry);

    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    spec = entry->spec;
    kfree(entry);
    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
	return &g_char_device;
}

