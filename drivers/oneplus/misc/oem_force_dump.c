/*
 * oem_force_dump.c
 *
 * drivers supporting debug functions for Oneplus device.
 *
 * hefaxi@filesystems, 2015/07/03.
 */
#include <linux/reboot.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/oem/oem_force_dump.h>
#include <linux/sched/signal.h>
#include <linux/syscalls.h>
#include <linux/sched/debug.h>
#include <linux/cred.h>

struct sock *nl_sk;
static int fd = -1;
static struct workqueue_struct *smg_workwq;
static struct work_struct smg_work;

#define MAX_MSGSIZE 1024
#define SIGNAL_DEBUGGER	(SIGRTMIN + 3)
static int message_state = -1;
static int selinux_switch;
enum key_stat_item pwr_status, vol_up_status;

static bool find_task_by_name(struct task_struct *t, char *name)
{
	const struct cred *tcred = __task_cred(t);

	if (!strncmp(t->comm, name, TASK_COMM_LEN))
		return true;
	if (!strncmp(t->comm, "Binder:", 7) && (t->group_leader->pid == t->pid)
			&& (tcred->uid.val == 1000) && (t->parent != 0 && !strcmp(t->parent->comm, "main")))
		return true;

	return false;
}

void send_sig_to_get_trace(char *name)
{
	struct task_struct *g, *t;

	for_each_process_thread(g, t) {
		if (find_task_by_name(t, name)) {
			do_send_sig_info(SIGQUIT, SEND_SIG_FORCED, t, PIDTYPE_TGID);
			msleep(500);
			return;
		}
	}
}


void send_sig_to_get_tombstone(char *name)
{
	struct task_struct *p;

	for_each_process(p) {
		if (!strncmp(p->comm, name, TASK_COMM_LEN)) {
			do_send_sig_info(SIGNAL_DEBUGGER, SEND_SIG_FORCED, p, PIDTYPE_TGID);
			msleep(500);
			break;
		}
	}
}

void get_init_sched_info(void)
{
	struct task_struct *p, *t;

	for_each_process(p) {
		if (p->pid == 1)
			break;
	}

	for_each_thread(p, t)
		sched_show_task(t);

}
void compound_key_to_get_trace(char *name)
{
	if (pwr_status == KEY_PRESSED && vol_up_status == KEY_PRESSED)
		send_sig_to_get_trace(name);
}

void compound_key_to_get_tombstone(char *name)
{
	if (pwr_status == KEY_PRESSED && vol_up_status == KEY_PRESSED)
		send_sig_to_get_tombstone(name);
}


/*
 * the way goto force dump:
 * 1. press the voluemup key and then relase it.
 * 2. press the volumedown key and then relase it.
 * 3. long press volumeup key, without release it.
 * 4. press twice power key, and release it.
 * 5. release the volumeup key.
 * 6. presss the volumeup key, without release it.
 * 7. press the power key.
 * after those step, the device will goto the force dump.
 */
void oem_check_force_dump_key(unsigned int code, int value)
{
	static enum { NONE, STEP1, STEP2, STEP3, STEP4, STEP5,
	STEP6, STEP7, STEP8, STEP9, STEP10, STEP11, STEP_DEBUG1} state = NONE;

	switch (state) {
	case NONE:
		if (code == KEY_VOLUMEUP && value)
			state = STEP1;
		else
			state = NONE;
		break;
	case STEP1:
		if (code == KEY_VOLUMEUP && !value)
			state = STEP2;
		else
			state = NONE;
		break;
	case STEP2:
		if (code == KEY_VOLUMEDOWN && value)
			state = STEP3;
		else
			state = NONE;
		break;
	case STEP3:
		if (code == KEY_VOLUMEDOWN && !value)
			state = STEP4;
		else
			state = NONE;
		break;
	case STEP4:
		if (code == KEY_VOLUMEUP && value)
			state = STEP5;
		else
			state = NONE;
		break;
	case STEP5:
		if (code == KEY_POWER && value)
			state = STEP6;
		else
			state = NONE;
		break;
	case STEP6:
		if (code == KEY_POWER && !value)
			state = STEP7;
		else
			state = NONE;
		break;
	case STEP7:
		if (code == KEY_POWER && value)
			state = STEP8;
		else
			state = NONE;
		break;
	case STEP8:
		if (code == KEY_POWER && !value)
			state = STEP9;
		else
			state = NONE;
		break;
	case STEP9:
		if (code == KEY_VOLUMEUP && !value)
			state = STEP10;
		else
			state = NONE;
		break;
	case STEP10:
		if (code == KEY_VOLUMEUP && value)
			state = STEP11;
		else if (code == KEY_VOLUMEDOWN && value)
			state = STEP_DEBUG1;
		else
			state = NONE;
		break;
	case STEP11:
		if (code == KEY_POWER && value) {
			if (oem_get_download_mode())
				panic("Force Dump");
		} else
			state = NONE;
		break;

	case STEP_DEBUG1:
		if (code == KEY_POWER && value) {
			set_oem_selinux_state(1);
			message_state = 1;
			queue_work(smg_workwq, &smg_work);
			state = NONE;
		} else if (code == KEY_VOLUMEDOWN && !value) {
			message_state = 2;
			queue_work(smg_workwq, &smg_work);
			state = NONE;
		} else
			state = NONE;
		break;
	}
}
int  set_oem_selinux_state(int state)
{
	selinux_switch = state;
	return 0;
}
int get_oem_selinux_state(void)
{
	return selinux_switch;
}

static void send_msg_worker(struct work_struct *work)
{
	if (message_state == 1)
		send_msg("Enable DEBUG!");
	else if (message_state == 2) {
		pr_info("force oem serial\n");
		msm_serial_oem_init();
		send_msg("ENABLE_OEM_FORCE_SERIAL");
	}
	message_state = 0;
}

void send_msg_sync_mdm_dump(void)
{
	send_msg("FORCE_MDM_DUMP_SYNC");
}

void send_msg(char *message)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	int len = NLMSG_SPACE(MAX_MSGSIZE);

	pr_info("%s,%s\n",__func__,message);

	if (!message || !nl_sk)
		return;

	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, MAX_MSGSIZE, 0);
	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	strlcpy(NLMSG_DATA(nlh), message, MAX_MSGSIZE);
	netlink_unicast(nl_sk, skb, fd, MSG_DONTWAIT);
}

void recv_nlmsg(struct sk_buff *skb)
{
	struct nlmsghdr *nlh = nlmsg_hdr(skb);

	if (nlh->nlmsg_len < NLMSG_HDRLEN || skb->len < nlh->nlmsg_len)
		return;
	fd = nlh->nlmsg_pid;
	pr_err("received:%s %d\n", (char *)NLMSG_DATA(nlh), fd);
}

struct netlink_kernel_cfg nl_kernel_cfg = {
	.groups = 0,
	.flags = 0,
	.input = recv_nlmsg,
	.cb_mutex = NULL,
	.bind = NULL,
	.compare = NULL,
};

int op_netlink_init(void)
{
	nl_sk = netlink_kernel_create(&init_net, NETLINK_ADB, &nl_kernel_cfg);
	if (!nl_sk) {
		pr_err("%s: Create netlink socket error.\n", __func__);
		return 1;
	}
	smg_workwq = create_singlethread_workqueue("oem_key_dump");
	if (!smg_workwq) {
		pr_err("%s: Create oem_key_dump error.\n", __func__);
		return 1;
	}
	INIT_WORK(&smg_work, send_msg_worker);
	pr_err("%s\n", __func__);
	return 0;
}

static void op_netlink_exit(void)
{
	if (nl_sk != NULL)
		sock_release(nl_sk->sk_socket);
	if (smg_workwq != NULL)
		destroy_workqueue(smg_workwq);
	pr_err("%s\n", __func__);
}

module_init(op_netlink_init);
module_exit(op_netlink_exit);
MODULE_LICENSE("GPL v2");
