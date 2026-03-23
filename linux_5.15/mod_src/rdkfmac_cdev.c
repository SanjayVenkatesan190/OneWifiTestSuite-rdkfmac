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
static void *last_freed_spec = NULL;

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

    /* 🔥 Local snapshot variables (C90: declared at top) */
    wlan_emu_msg_type_t type;
    unsigned int frame_len = 0;
    void *frame_ptr = NULL;

    printk("SJY ENTER %s data=%p\n", __func__, data);

    /* ================================
     * 🔥 UAF DETECTION
     * ================================ */
    if (data == last_freed_spec) {
        printk("SJY_UAF_CONFIRMED: reused freed pointer=%p\n", data);
        return;
    }

    /* Validate input */
    if (!data) {
        printk("SJY ERROR: NULL data received\n");
        return;
    }

    /* ================================
     * 🔥 SAFE SNAPSHOT (avoid race/UAF)
     * ================================ */
    type = data->type;

    if (type == wlan_emu_msg_type_frm80211) {
        frame_len = data->u.frm80211.u.frame.frame_len;
        frame_ptr = data->u.frm80211.u.frame.frame;
    }

    printk("SJY_PUSH spec=%p type=%d frame_len=%u frame_ptr=%p\n",
           data, type, frame_len, frame_ptr);

    /* 🔥 POISON CHECK */
    if (type == 0xDEDEDEDE) {
        printk("SJY_POISON_HIT: data already freed %p\n", data);
        return;
    }

    /* Skip if nobody listening */
    if (g_char_device.num_inst == 0) {
        printk("SJY No listeners, dropping message\n");
        return;
    }

    /* Skip if emu closed */
    if (rdkfmac_emu80211_close == true) {
        printk("SJY emu80211 closed, dropping message\n");
        return;
    }

    /* ================================
     * ✅ Allocate entry
     * ================================ */
    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        printk("SJY ERROR: entry alloc failed\n");
        return;
    }

    /* ================================
     * ✅ Allocate spec
     * ================================ */
    spec = kmalloc(sizeof(*spec), GFP_ATOMIC);
    if (!spec) {
        printk("SJY ERROR: spec alloc failed\n");
        kfree(entry);
        return;
    }

    entry->spec = spec;

    /* ================================
     * ✅ STRUCT COPY
     * ================================ */
    memcpy(spec, data, sizeof(*spec));

    /* 🔥 CRITICAL: break shallow pointer */
    spec->u.frm80211.u.frame.frame = NULL;

    /* ================================
     * ✅ DEEP COPY FRAME
     * ================================ */
    if (type == wlan_emu_msg_type_frm80211 &&
        frame_ptr != NULL &&
        frame_len > 0) {

        if (frame_len > 4096) {
            printk("SJY ERROR: invalid frame length %u\n", frame_len);
            kfree(spec);
            kfree(entry);
            return;
        }

        spec->u.frm80211.u.frame.frame =
            kmalloc(frame_len, GFP_ATOMIC);

        if (!spec->u.frm80211.u.frame.frame) {
            printk("SJY ERROR: frame alloc failed\n");
            kfree(spec);
            kfree(entry);
            return;
        }

        memcpy(spec->u.frm80211.u.frame.frame,
               frame_ptr,
               frame_len);

        printk("SJY FRAME COPY orig=%p new=%p len=%u\n",
               frame_ptr,
               spec->u.frm80211.u.frame.frame,
               frame_len);
    } else {
        spec->u.frm80211.u.frame.frame = NULL;
        spec->u.frm80211.u.frame.frame_len = 0;
    }

    /* ================================
     * TYPE DEBUG
     * ================================ */
    switch (type) {
    case wlan_emu_msg_type_frm80211:
        strcpy(str_spec_type, "frm80211");
        break;
    case wlan_emu_msg_type_cfg80211:
        strcpy(str_spec_type, "cfg80211");
        break;
    case wlan_emu_msg_type_mac80211:
        strcpy(str_spec_type, "mac80211");
        break;
    default:
        strcpy(str_spec_type, "other");
        break;
    }

    printk("SJY Message type: %s\n", str_spec_type);

    /* ================================
     * 🔒 CRITICAL SECTION
     * ================================ */
    spin_lock_irqsave(&g_char_device_list_lock, flags);

    if (g_char_device.num_inst == 0 || rdkfmac_emu80211_close) {
        spin_unlock_irqrestore(&g_char_device_list_lock, flags);

        if (spec->u.frm80211.u.frame.frame)
            kfree(spec->u.frm80211.u.frame.frame);

        kfree(spec);
        kfree(entry);
        return;
    }

    list_add_tail(&entry->list_entry, &g_char_device.list_head);

    spin_unlock_irqrestore(&g_char_device_list_lock, flags);

    /* ================================
     * WAKEUP
     * ================================ */
    wake_up_interruptible(&rdkfmac_rq);

    printk("SJY EXIT %s success entry=%p spec=%p\n",
           __func__, entry, spec);
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
static void handle_frm80211_msg_w(char *read_buff, size_t size)
{
    wlan_emu_msg_data_t *frm80211_msg;
    struct ieee80211_hdr *hdr;
    unsigned int msg_ops_type;
    unsigned short fc, type, stype;
    const unsigned char rfc1042_hdr[ETH_ALEN] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    unsigned char *tmp_frame_buf;
    unsigned int data_header_len;
    unsigned int f_len;
    size_t rem_size;

    /* Init */
    msg_ops_type = 0;
    data_header_len = 0;
    rem_size = size;

    /* 🔍 ENTRY DEBUG */
    printk("SJY_PARSE ENTER size=%zu\n", size);

    /* 1. Validate minimum header */
    if (rem_size < (sizeof(wlan_emu_msg_type_t) +
                    sizeof(wlan_emu_frm80211_ops_type_t) +   /* ✅ FIXED */
                    sizeof(unsigned int) +
                    (ETH_ALEN * 2))) {

        printk("SJY ERROR: Buffer too small for frm80211 metadata\n");
        return;
    }

    frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (!frm80211_msg)
        return;

    /* 2. Copy TYPE */
    memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
    read_buff += sizeof(wlan_emu_msg_type_t);
    rem_size -= sizeof(wlan_emu_msg_type_t);

    if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
        printk("SJY ERROR: Invalid type %d\n", frm80211_msg->type);
        kfree(frm80211_msg);
        return;
    }

    /* 3. Skip frm80211 ops (correct struct) */
    read_buff += sizeof(wlan_emu_frm80211_ops_type_t);
    rem_size -= sizeof(wlan_emu_frm80211_ops_type_t);

    /* 🔍 DEBUG BEFORE frame_len */
    printk("SJY_PARSE BEFORE frame_len read rem_size=%zu\n", rem_size);

    /* 4. Read frame_len */
    memcpy(&frm80211_msg->u.frm80211.u.frame.frame_len,
           read_buff,
           sizeof(unsigned int));

    read_buff += sizeof(unsigned int);
    rem_size -= sizeof(unsigned int);

    f_len = frm80211_msg->u.frm80211.u.frame.frame_len;

    /* 🔍 DEBUG AFTER frame_len */
    printk("SJY_PARSE frame_len=%u rem_size=%zu read_ptr=%p\n",
           f_len, rem_size, read_buff);

    /* 5. Copy MAC addresses */
    memcpy(frm80211_msg->u.frm80211.u.frame.macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;
    rem_size -= ETH_ALEN;

    memcpy(frm80211_msg->u.frm80211.u.frame.client_macaddr, read_buff, ETH_ALEN);
    read_buff += ETH_ALEN;
    rem_size -= ETH_ALEN;

    /* 🚨 VALIDATE FRAME LENGTH */
    if (f_len == 0 || f_len > rem_size || f_len > 4096) {
        printk("SJY ERROR: Malformed frame_len=%u rem_size=%zu\n", f_len, rem_size);
        kfree(frm80211_msg);
        return;
    }

    /* 6. Allocate frame buffer */
    frm80211_msg->u.frm80211.u.frame.frame = kzalloc(f_len, GFP_KERNEL);
    if (!frm80211_msg->u.frm80211.u.frame.frame) {
        kfree(frm80211_msg);
        return;
    }

    /* 7. Copy frame payload */
    memcpy(frm80211_msg->u.frm80211.u.frame.frame, read_buff, f_len);

    /* 🚨 Ensure header is valid before accessing */
    if (f_len < sizeof(struct ieee80211_hdr)) {
        printk("SJY ERROR: Frame too small for ieee80211 header (%u)\n", f_len);
        kfree(frm80211_msg->u.frm80211.u.frame.frame);
        kfree(frm80211_msg);
        return;
    }

    /* 8. Protocol parsing */
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
            printk("SJY INFO: Unknown MGMT subtype %d\n", stype);
            break;
        }

    } else if (type == WLAN_FC_TYPE_DATA) {

        data_header_len = ieee80211_hdrlen(hdr->frame_control);

        if (f_len < data_header_len + sizeof(rfc1042_hdr) + 2) {
            printk("SJY ERROR: Invalid DATA frame length\n");
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }

        tmp_frame_buf = frm80211_msg->u.frm80211.u.frame.frame + data_header_len;

        if (memcmp(tmp_frame_buf, rfc1042_hdr, sizeof(rfc1042_hdr)) == 0) {
            tmp_frame_buf += sizeof(rfc1042_hdr);

            if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) == ETH_P_PAE) {
                msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
            }
        }
    }

    /* 🔍 FINAL DEBUG BEFORE PUSH */
    printk("SJY_PUSH_PRE spec=%p frame_len=%u frame_ptr=%p ops=%u\n",
           frm80211_msg,
           f_len,
           frm80211_msg->u.frm80211.u.frame.frame,
           msg_ops_type);

    /* 9. Assign ops and push */
    frm80211_msg->u.frm80211.ops = msg_ops_type;
    push_to_char_device(frm80211_msg);

    /* 10. Cleanup (push does deep copy) */
    kfree(frm80211_msg->u.frm80211.u.frame.frame);
    kfree(frm80211_msg);

    printk("SJY_PARSE EXIT\n");
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
        return -ENOMEM;
    }

    if (copy_from_user(read_buff, user_buffer, size)) {
        printk("%s:%d: copy_from_user error\n", __func__, __LINE__);
        kfree(pSpec);
        kfree(read_buff);
        return -EFAULT;
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
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %p len : %p \n", __func__, __LINE__, spec, s_tmp, len);
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
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %p len : %p \n", __func__, __LINE__, spec, s_tmp, len);
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
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %p len : %p \n", __func__, __LINE__, spec, s_tmp, len);
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
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %p len : %p \n", __func__, __LINE__, spec, s_tmp, len);
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

static void handle_frame(wlan_emu_msg_data_t *spec,
                         ssize_t *len,
                         u8 *s_tmp,
                         size_t max_size)
{
    unsigned int f_len;
    size_t total_needed;
    unsigned int f_len_copy;
    void *frame_ptr_copy;

    /* 🔥 UAF DETECTOR */
    if (spec == last_freed_spec) {
        printk("SJY_UAF_CONFIRMED: using freed spec=%p\n", spec);
        return;
    }

    /* 🔍 ENTRY LOG */
    printk("SJY_HANDLE_FRAME ENTER spec=%p type=%d frame_len=%u frame_ptr=%p len=%zd max=%zu\n",
           spec,
           spec->type,
           spec->u.frm80211.u.frame.frame_len,
           spec->u.frm80211.u.frame.frame,
           *len,
           max_size);

    /* 🔥 POISON DETECTION */
    if (spec->type == 0xDEDEDEDE) {
        printk("SJY_POISON_HIT: freed memory accessed spec=%p\n", spec);
        return;
    }

    f_len = spec->u.frm80211.u.frame.frame_len;

    /* 🚨 HARD CORRUPTION CHECK */
    if (f_len == 0 || f_len > 4096) {
        printk("SJY_CORRUPTION: Invalid frame_len=%u spec=%p frame_ptr=%p\n",
               f_len, spec, spec->u.frm80211.u.frame.frame);
        return;
    }

    /* 🚨 NULL POINTER CHECK */
    if (!spec->u.frm80211.u.frame.frame) {
        printk("SJY_CORRUPTION: NULL frame pointer spec=%p\n", spec);
        return;
    }

    /* 🚨 LOW ADDRESS POINTER CHECK */
    if ((unsigned long)spec->u.frm80211.u.frame.frame < 0x1000) {
        printk("SJY_BAD_PTR: suspicious frame pointer=%p spec=%p\n",
               spec->u.frm80211.u.frame.frame, spec);
        return;
    }

    /* 🧠 Save copies to detect mid-function corruption */
    f_len_copy = f_len;
    frame_ptr_copy = spec->u.frm80211.u.frame.frame;

    /* 1. Calculate required size */
    total_needed = sizeof(wlan_emu_msg_type_t) +
                   sizeof(wlan_emu_frm80211_ops_type_t) +
                   sizeof(unsigned int) +
                   f_len +
                   (ETH_ALEN * 2);

    printk("SJY_HANDLE_FRAME SIZE total_needed=%zu len=%zd max=%zu\n",
           total_needed, *len, max_size);

    /* 🚨 SAFE OVERFLOW CHECK */
    if (total_needed > max_size || *len > max_size - total_needed) {
        printk("SJY_OVERFLOW_BLOCK: needed=%zu len=%zd max=%zu\n",
               total_needed, *len, max_size);
        return;
    }

    /* 2. Copy type */
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t));
    s_tmp += sizeof(wlan_emu_msg_type_t);
    *len += sizeof(wlan_emu_msg_type_t);

    /* 3. Copy ops */
    memcpy(s_tmp,
           &spec->u.frm80211.ops,
           sizeof(wlan_emu_frm80211_ops_type_t));
    s_tmp += sizeof(wlan_emu_frm80211_ops_type_t);
    *len += sizeof(wlan_emu_frm80211_ops_type_t);

    printk("SJY_PROGRESS after header len=%zd\n", *len);

    /* 🔍 MID DEBUG */
    printk("SJY_HANDLE_FRAME MID frame_len=%u ops=%d\n",
           spec->u.frm80211.u.frame.frame_len,
           spec->u.frm80211.ops);

    /* 🚨 MID-FUNCTION CORRUPTION DETECTOR */
    if (f_len_copy != spec->u.frm80211.u.frame.frame_len ||
        frame_ptr_copy != spec->u.frm80211.u.frame.frame) {

        printk("SJY_CORRUPTION_MID: spec=%p len_before=%u len_now=%u ptr_before=%p ptr_now=%p\n",
               spec,
               f_len_copy,
               spec->u.frm80211.u.frame.frame_len,
               frame_ptr_copy,
               spec->u.frm80211.u.frame.frame);
        return;
    }

    /* 4. Copy frame_len */
    memcpy(s_tmp,
           &spec->u.frm80211.u.frame.frame_len,
           sizeof(unsigned int));
    s_tmp += sizeof(unsigned int);
    *len += sizeof(unsigned int);

    /* 5. Copy frame payload */
    memcpy(s_tmp,
           spec->u.frm80211.u.frame.frame,
           f_len);
    s_tmp += f_len;
    *len += f_len;

    /* 6. Copy MAC addresses */
    memcpy(s_tmp,
           spec->u.frm80211.u.frame.macaddr,
           ETH_ALEN);
    s_tmp += ETH_ALEN;
    *len += ETH_ALEN;

    memcpy(s_tmp,
           spec->u.frm80211.u.frame.client_macaddr,
           ETH_ALEN);
    s_tmp += ETH_ALEN;
    *len += ETH_ALEN;

    /* 🔍 EXIT LOG */
    printk("SJY_HANDLE_FRAME EXIT spec=%p final_len=%zd\n",
           spec, *len);
}

static void handle_frm80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp, size_t max_size)
{
	if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) {
		printk(KERN_INFO "%s:%d: NULL Pointer spec : %p s_tmp : %p len : %p \n", __func__, __LINE__, spec, s_tmp, len);
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
			printk(KERN_INFO "%s:%d: Not Handling op type %d\n", __func__, __LINE__, spec->u.frm80211.ops);
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

    printk("SJY_READ spec=%p type=%d frame_len=%u frame_ptr=%p\n",
       spec,
       spec->type,
       spec->u.frm80211.u.frame.frame_len,
       spec->u.frm80211.u.frame.frame);

    if (spec->type == wlan_emu_msg_type_frm80211 &&
        spec->u.frm80211.u.frame.frame_len > 4096) {

      printk("SJY_CORRUPTION_DETECTED spec=%p frame_len=%u\n", spec,
             spec->u.frm80211.u.frame.frame_len);
    }

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
    printk("SJY_POP spec=%p type=%d frame_len=%u frame_ptr=%p\n", spec,
           spec->type,
           (spec->type == wlan_emu_msg_type_frm80211)
               ? spec->u.frm80211.u.frame.frame_len
               : 0,
           (spec->type == wlan_emu_msg_type_frm80211)
               ? spec->u.frm80211.u.frame.frame
               : NULL);
    kfree(entry);
    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
	return &g_char_device;
}

