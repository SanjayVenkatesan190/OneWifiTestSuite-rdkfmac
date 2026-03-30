/* Modifications Copyright 2025 Comcast Cable Communications Management, LLC
 * Licensed under the GPLv2.0 License
 */

#include "rdkfmac.h"
#include <linux/spinlock.h>

struct rdkfmac_device_data g_char_device;
static DECLARE_WAIT_QUEUE_HEAD(rdkfmac_rq);

/* BUG 2 FIX: Spinlock protecting g_char_device linked list.
 * Required: push (nl_recv_fu/write context) and pop (read context) run
 * concurrently; without this lock list_del() poisons ->prev to
 * LIST_POISON2 (0xdead000000000100) -> kernel panic. */
static DEFINE_SPINLOCK(g_char_device_lock);

static wlan_emu_msg_data_t *pop_from_char_device(void);
static unsigned int get_list_entries_count_in_char_device(void);
static bool rdkfmac_emu80211_close = true;

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
        default: break;
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
        default: break;
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
        default: break;
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
    char str_spec_type[32] = "";
    char str_ops[128] = "";
    unsigned long flags;

    if (g_char_device.num_inst == 0) return;
    if (rdkfmac_emu80211_close == true) return;

    /* BUG 1 FIX: GFP_ATOMIC and NULL Checks */
    entry = kmalloc(sizeof(wlan_emu_msg_data_entry_t), GFP_ATOMIC);
    if (entry == NULL) {
        printk(KERN_ERR "%s:%d: [DBG-PUSH] kmalloc failed for entry\n", __func__, __LINE__);
        return;
    }
    spec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_ATOMIC);
    if (spec == NULL) {
        printk(KERN_ERR "%s:%d: [DBG-PUSH] kmalloc failed for spec\n", __func__, __LINE__);
        kfree(entry);
        return;
    }
    entry->spec = spec;

    /* Shallow copy - ownership of deep pointers (if any) transfers to the list */
    memcpy(spec, data, sizeof(wlan_emu_msg_data_t));

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
        case wlan_emu_msg_type_agent:
            break;
        default:
            break;
    }

    /* BUG 4 FIX: Removed TOCTOU get_list_entries_count from printk */
    if ((spec->type != wlan_emu_msg_type_webconfig) && (spec->type != wlan_emu_msg_type_frm80211) &&
        (spec->type != wlan_emu_msg_type_agent)) {
        printk("%s:%d: pushing data to queue, type: %s ops: %s\n", __func__, __LINE__, str_spec_type, str_ops);
    }

    /* BUG 2 FIX: Protected List Addition using standard Kernel Macro */
    spin_lock_irqsave(&g_char_device_lock, flags);
    list_add_tail(&entry->list_entry, &g_char_device.list_head);
    spin_unlock_irqrestore(&g_char_device_lock, flags);

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
            if (cmd_buffer == NULL) return;
            
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
                heart_beat_data.rssi, heart_beat_data.noise, heart_beat_data.bitrate, heart_beat_data.mac);

            update_heartbeat_data(&heart_beat_data);
            kfree(cmd_buffer);
            break;
            
        case wlan_emu_emu80211_cmd_mac_update:
            buff_length = data->u.emu80211.u.cmd.buff_len;
            cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);
            if (cmd_buffer == NULL) return;
            
            memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
            memcpy(&mac_update.old_mac, &cmd_buffer[count], sizeof(mac_update.old_mac));
            count += sizeof(mac_update.old_mac);
            memcpy(&mac_update.new_mac, &cmd_buffer[count], sizeof(mac_update.new_mac));
            count += sizeof(mac_update.new_mac);
            memcpy(&mac_update.op_modes, &cmd_buffer[count], sizeof(mac_update.op_modes));
            count += sizeof(mac_update.op_modes);
            memcpy(&mac_update.bridge_name, &cmd_buffer[count], sizeof(mac_update.bridge_name));
            count += sizeof(mac_update.bridge_name);

            update_sta_new_mac(&mac_update);
            kfree(cmd_buffer);
            break;
            
        case wlan_emu_emu80211_cmd_frame_auth_req:
            cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);
            if (cmd_buffer == NULL) return;
            memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
            update_auth_req(cmd_buffer, data->u.emu80211.u.cmd.buff_len);
            kfree(cmd_buffer);
            break;
            
        case wlan_emu_emu80211_cmd_frame_assoc_req:
            cmd_buffer = kmalloc(sizeof(data->u.emu80211.u.cmd.cmd_buffer), GFP_KERNEL);
            if (cmd_buffer == NULL) return;
            memcpy(cmd_buffer, data->u.emu80211.u.cmd.cmd_buffer, sizeof(data->u.emu80211.u.cmd.cmd_buffer));
            update_assoc_req(cmd_buffer, data->u.emu80211.u.cmd.buff_len);
            kfree(cmd_buffer);
            break;
            
        default:
            break;
    }
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
}

static void handle_agent_msg_w(wlan_emu_msg_data_t *spec) {
    switch (spec->u.agent_msg.ops) {
        case wlan_emu_msg_agent_ops_type_cmd:
            if (spec->u.agent_msg.u.cmd == wlan_emu_msg_agent_cmd_type_start) {
                rdkfmac_emu80211_close = false;
                push_to_char_device(spec);
            } else if (spec->u.agent_msg.u.cmd == wlan_emu_msg_agent_cmd_type_stop) {
                push_to_char_device(spec);
            }
            break;
        case wlan_emu_msg_agent_ops_type_data:
        case wlan_emu_msg_agent_ops_type_notification:
            push_to_char_device(spec);
            break;
        default:
            break;
    }
}

static void handle_frm80211_msg_w(char *read_buff, size_t size) {
    wlan_emu_msg_data_t *frm80211_msg;
    struct ieee80211_hdr *hdr;
    unsigned int msg_ops_type = 0;
    unsigned short fc, type, stype;
    const unsigned char rfc1042_hdr[ETH_ALEN] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    unsigned char *tmp_frame_buf;
    unsigned int data_header_len = 0;

    frm80211_msg = kzalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    if (frm80211_msg == NULL) return;

    memcpy(&frm80211_msg->type, read_buff, sizeof(wlan_emu_msg_type_t));
    read_buff += sizeof(wlan_emu_msg_type_t);

    if (frm80211_msg->type != wlan_emu_msg_type_frm80211) {
        kfree(frm80211_msg);
        return;
    }

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
        if (((tmp_frame_buf[0] << 8) | tmp_frame_buf[1]) == ETH_P_PAE) {
            msg_ops_type = wlan_emu_frm80211_ops_type_eapol;
        } else {
            kfree(frm80211_msg->u.frm80211.u.frame.frame);
            kfree(frm80211_msg);
            return;
        }
    }

    memcpy(&frm80211_msg->u.frm80211.ops, &msg_ops_type, sizeof(wlan_emu_cfg80211_ops_type_t));
    push_to_char_device(frm80211_msg);

    /* BUG 3 FIX: The ownership of frm80211_msg->u.frm80211.u.frame.frame 
     * is transferred to the list entry during the shallow copy inside 
     * push_to_char_device. We only free the outer container here.
     * rdkfmac_read will free the frame payload upon consumption. */
    kfree(frm80211_msg);
}

/* Missing function restored and patched to avoid Stack VLA panics */
static ssize_t rdkfmac_write(struct file *file, const char __user *user_buffer, size_t size, loff_t * offset)
{
    wlan_emu_msg_data_t *pSpec;
    ssize_t sz = 0;
    char *read_buff;

    if (size < sizeof(wlan_emu_msg_type_t)) return -EINVAL;

    pSpec = kmalloc(sizeof(wlan_emu_msg_data_t), GFP_KERNEL);
    read_buff = kzalloc(size, GFP_KERNEL);
    
    if (!pSpec || !read_buff) {
        if(pSpec) kfree(pSpec);
        if(read_buff) kfree(read_buff);
        return -ENOMEM;
    }

    if (copy_from_user(read_buff, user_buffer, size)) {
        printk("%s:%d: potential copy error\n", __func__, __LINE__);
        kfree(pSpec);
        kfree(read_buff);
        return -EFAULT;
    }

    memcpy(&pSpec->type, read_buff, sizeof(wlan_emu_msg_type_t));
    switch (pSpec->type) {
        case wlan_emu_msg_type_frm80211:
            handle_frm80211_msg_w(read_buff, size);
            sz = size;
            break;
        case wlan_emu_msg_type_emu80211:
            if (size >= sizeof(wlan_emu_msg_data_t)) {
                memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
                handle_emu80211_msg_w(pSpec);
                sz = sizeof(wlan_emu_msg_data_t);
            }
            break;
        case wlan_emu_msg_type_webconfig:
            if (size >= sizeof(wlan_emu_msg_data_t)) {
                memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
                push_to_char_device(pSpec);
                sz = sizeof(wlan_emu_msg_data_t);
            }
            break;
        case wlan_emu_msg_type_agent:
            if (size >= sizeof(wlan_emu_msg_data_t)) {
                memcpy(pSpec, read_buff, sizeof(wlan_emu_msg_data_t));
                handle_agent_msg_w(pSpec);
                sz = sizeof(wlan_emu_msg_data_t);
            }
            break;
        default:
            printk("%s:%d Invalid read operation\n", __func__, __LINE__);
            sz = -EINVAL;
            break;
    }

    kfree(pSpec);
    kfree(read_buff);
    return sz;
}

void handle_cfg80211_msg_start_ap(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;

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
}

void handle_cfg80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    if (spec->u.cfg80211.ops == wlan_emu_cfg80211_ops_type_start_ap) {
        handle_cfg80211_msg_start_ap(spec, len, s_tmp);
    }
}

void handle_emu80211_msg_tctrl(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); s_tmp += sizeof(wlan_emu_msg_type_t); *len += sizeof(wlan_emu_msg_type_t);
    memcpy(s_tmp, &spec->u.emu80211.ops, sizeof(wlan_emu_emu80211_ops_type_t)); s_tmp += sizeof(wlan_emu_emu80211_ops_type_t); *len += sizeof(wlan_emu_emu80211_ops_type_t);
    memcpy(s_tmp, &spec->u.emu80211.u.ctrl.ctrl, sizeof(wlan_emu_emu80211_ctrl_type_t)); s_tmp += sizeof(wlan_emu_emu80211_ctrl_type_t); *len += sizeof(wlan_emu_emu80211_ctrl_type_t);
    memcpy(s_tmp, &spec->u.emu80211.u.ctrl.coverage, sizeof(wlan_emu_test_coverage_t)); s_tmp += sizeof(wlan_emu_test_coverage_t); *len += sizeof(wlan_emu_test_coverage_t);
    memcpy(s_tmp, &spec->u.emu80211.u.ctrl.type, sizeof(wlan_emu_test_type_t)); s_tmp += sizeof(wlan_emu_test_type_t); *len += sizeof(wlan_emu_test_type_t);
}

void handle_emu80211_msg_close(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL)) return;
    
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); s_tmp += sizeof(wlan_emu_msg_type_t); *len += sizeof(wlan_emu_msg_type_t);
    memcpy(s_tmp, &spec->u.emu80211.ops, sizeof(wlan_emu_emu80211_ops_type_t)); s_tmp += sizeof(wlan_emu_emu80211_ops_type_t); *len += sizeof(wlan_emu_emu80211_ops_type_t);
    memcpy(s_tmp, &spec->u.emu80211.u.close.fd, sizeof(int)); s_tmp += sizeof(int); *len += sizeof(int);
}

void handle_emu80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    switch(spec->u.emu80211.ops) {
        case wlan_emu_emu80211_ops_type_tctrl: handle_emu80211_msg_tctrl(spec, len, s_tmp); break;
        case wlan_emu_emu80211_ops_type_close: handle_emu80211_msg_close(spec, len, s_tmp); break;
        default: break;
    }
}

void handle_webconfig_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); s_tmp += sizeof(wlan_emu_msg_type_t); *len += sizeof(wlan_emu_msg_type_t);
    memcpy(s_tmp, &spec->u.ow_webconfig.subdoc_type, sizeof(webconfig_subdoc_type_t)); s_tmp += sizeof(webconfig_subdoc_type_t); *len += sizeof(webconfig_subdoc_type_t);
}

void handle_agent_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); s_tmp += sizeof(wlan_emu_msg_type_t); *len += sizeof(wlan_emu_msg_type_t);
    memcpy(s_tmp, &spec->u.agent_msg.ops, sizeof(wlan_emu_msg_agent_ops_t)); s_tmp += sizeof(wlan_emu_msg_agent_ops_t); *len += sizeof(wlan_emu_msg_agent_ops_t);

    if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_cmd) {
        memcpy(s_tmp, &spec->u.agent_msg.u.cmd, sizeof(wlan_emu_msg_agent_cmd_t)); s_tmp += sizeof(wlan_emu_msg_agent_cmd_t); *len += sizeof(wlan_emu_msg_agent_cmd_t);
    }
    if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_data) {
        memcpy(s_tmp, &spec->u.agent_msg.u.buf, sizeof(void *)); s_tmp += sizeof(void *); *len += sizeof(void *);
    }
    if (spec->u.agent_msg.ops == wlan_emu_msg_agent_ops_type_notification) {
        memcpy(s_tmp, &spec->u.agent_msg.u.agent_notif.sub_ops_type, sizeof(int)); s_tmp += sizeof(int); *len += sizeof(int);
        if (spec->u.agent_msg.u.agent_notif.sub_ops_type == wlan_msg_ext_agent_ops_sub_type_wifi_notification) {
            memcpy(s_tmp, &spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.sta_state, sizeof(int)); s_tmp += sizeof(int); *len += sizeof(int);
            memcpy(s_tmp, spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.sta_mac_addr, ETH_ALEN); s_tmp += ETH_ALEN; *len += ETH_ALEN;
            memcpy(s_tmp, spec->u.agent_msg.u.agent_notif.u.wifi_sta_notif.bssid_mac_addr, ETH_ALEN); s_tmp += ETH_ALEN; *len += ETH_ALEN;
        }
    }
}

static void handle_frame(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    memcpy(s_tmp, &spec->type, sizeof(wlan_emu_msg_type_t)); s_tmp += sizeof(wlan_emu_msg_type_t); *len += sizeof(wlan_emu_msg_type_t);
    memcpy(s_tmp, &spec->u.frm80211.ops, sizeof(wlan_emu_frm80211_ops_type_t)); s_tmp += sizeof(wlan_emu_frm80211_ops_type_t); *len += sizeof(wlan_emu_frm80211_ops_type_t);
    
    printk("%s:%d Frame len is %d ops is %d\n", __func__, __LINE__, spec->u.frm80211.u.frame.frame_len, spec->u.frm80211.ops);
    
    memcpy(s_tmp, &spec->u.frm80211.u.frame.frame_len, sizeof(unsigned int)); s_tmp += sizeof(unsigned int); *len += sizeof(unsigned int);
    memcpy(s_tmp, spec->u.frm80211.u.frame.frame, spec->u.frm80211.u.frame.frame_len); s_tmp += spec->u.frm80211.u.frame.frame_len; *len += spec->u.frm80211.u.frame.frame_len;
    memcpy(s_tmp, spec->u.frm80211.u.frame.macaddr, ETH_ALEN); s_tmp += ETH_ALEN; *len += ETH_ALEN;
    memcpy(s_tmp, spec->u.frm80211.u.frame.client_macaddr, ETH_ALEN); *len += ETH_ALEN;
}

static void handle_frm80211_msg(wlan_emu_msg_data_t *spec, ssize_t *len, u8 *s_tmp)
{
    if ((spec == NULL) || (s_tmp == NULL) || (len == NULL)) return;
    switch(spec->u.frm80211.ops) {
        case wlan_emu_frm80211_ops_type_prb_req: case wlan_emu_frm80211_ops_type_prb_resp:
        case wlan_emu_frm80211_ops_type_assoc_resp: case wlan_emu_frm80211_ops_type_assoc_req:
        case wlan_emu_frm80211_ops_type_auth: case wlan_emu_frm80211_ops_type_deauth:
        case wlan_emu_frm80211_ops_type_disassoc: case wlan_emu_frm80211_ops_type_eapol:
        case wlan_emu_frm80211_ops_type_reassoc_req: case wlan_emu_frm80211_ops_type_reassoc_resp:
        case wlan_emu_frm80211_ops_type_action:
            handle_frame(spec, len, s_tmp);
            break;
        default: break;
    }
}

static ssize_t rdkfmac_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
    wlan_emu_msg_data_t *spec;
    ssize_t return_len = 0;
    char *send_buff;
    u8 *s_tmp;

    /* PUT CPU TO SLEEP UNTIL DATA ARRIVES */
    if (wait_event_interruptible(rdkfmac_rq, get_list_entries_count_in_char_device() != 0)) {
        return -ERESTARTSYS;
    }

    spec = pop_from_char_device();
    if (spec == NULL) {
        return 0;
    }

    send_buff = kzalloc(size, GFP_KERNEL);
    if (!send_buff) {
        if (spec->type == wlan_emu_msg_type_frm80211) kfree(spec->u.frm80211.u.frame.frame);
        kfree(spec);
        return -ENOMEM;
    }
    
    s_tmp = send_buff;

    switch (spec->type) {
        case wlan_emu_msg_type_cfg80211: handle_cfg80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_emu80211: handle_emu80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_frm80211: handle_frm80211_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_webconfig: handle_webconfig_msg(spec, &return_len, s_tmp); break;
        case wlan_emu_msg_type_agent: handle_agent_msg(spec, &return_len, s_tmp); break;
        default: break;
    }

    if (copy_to_user(user_buffer, send_buff, return_len)) {
        return_len = -EFAULT;
    }

    if (spec->type == wlan_emu_msg_type_frm80211 && spec->u.frm80211.u.frame.frame) {
        kfree(spec->u.frm80211.u.frame.frame);
    }

    kfree(spec);
    kfree(send_buff);
    return return_len;
}

static int rdkfmac_open(struct inode *inode, struct file *file)
{
    unsigned long flags;
    spin_lock_irqsave(&g_char_device_lock, flags);
    g_char_device.num_inst++;
    spin_unlock_irqrestore(&g_char_device_lock, flags);
    
    printk(KERN_INFO "%s:%d Opened Instances: %d\n", __func__, __LINE__, g_char_device.num_inst);
    return 0;
}

static int rdkfmac_release(struct inode *inode, struct file *file)
{
    unsigned long flags;
    spin_lock_irqsave(&g_char_device_lock, flags);
    if (g_char_device.num_inst > 0) {
        g_char_device.num_inst--;
    }
    spin_unlock_irqrestore(&g_char_device_lock, flags);

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
    if (ret_val != 0) return ret_val;

    memset(&g_char_device, 0, sizeof(rdkfmac_device_data_t));
    
    /* INITIALIZATION FIX: Lists initialized BEFORE cdev_add */
    INIT_LIST_HEAD(&g_char_device.list_head);

    cdev_init(&g_char_device.cdev, &rdkfmac_fops);
    cdev_add(&g_char_device.cdev, MKDEV(RDKFMAC_MAJOR, 0), 1);
    
    g_char_device.class = class_create(THIS_MODULE, RDKFMAC_CLASS_NAME);
    if (IS_ERR(g_char_device.class)){
        cdev_del(&g_char_device.cdev);
        unregister_chrdev_region(MKDEV(RDKFMAC_MAJOR, 0), 1);
        return PTR_ERR(g_char_device.class);
    }
    
    g_char_device.tdev = MKDEV(RDKFMAC_MAJOR, 0);
    g_char_device.dev = device_create(g_char_device.class, NULL, g_char_device.tdev, NULL, RDKFMAC_DEVICE_NAME);

    return 0;
}

void cleanup_rdkfmac_cdev(void)
{
    device_destroy(g_char_device.class, g_char_device.tdev);
    class_destroy(g_char_device.class);
    cdev_del(&g_char_device.cdev);
    unregister_chrdev_region(MKDEV(RDKFMAC_MAJOR, 0), 1);
}

unsigned int get_list_entries_count_in_char_device(void)
{
    unsigned count = 0;
    struct list_head *ptr;
    unsigned long flags;

    spin_lock_irqsave(&g_char_device_lock, flags);
    list_for_each(ptr, &g_char_device.list_head) {
        count++;
        if (count > 2000) break; /* Safety Break */
    }
    spin_unlock_irqrestore(&g_char_device_lock, flags);

    return count;
}

wlan_emu_msg_data_t *pop_from_char_device(void)
{
    wlan_emu_msg_data_t *spec = NULL;
    wlan_emu_msg_data_entry_t *entry = NULL;
    unsigned long flags;

    spin_lock_irqsave(&g_char_device_lock, flags);
    
    /* BUG 2 FIX: Use safe Linux macros instead of manual tail management */
    if (list_empty(&g_char_device.list_head)) {
        spin_unlock_irqrestore(&g_char_device_lock, flags);
        return NULL;
    }

    entry = list_first_entry(&g_char_device.list_head, wlan_emu_msg_data_entry_t, list_entry);
    list_del(&entry->list_entry);
    
    spin_unlock_irqrestore(&g_char_device_lock, flags);

    spec = entry->spec;
    kfree(entry);

    return spec;
}

struct rdkfmac_device_data *get_char_device_data(void)
{
    return &g_char_device;
}