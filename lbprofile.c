#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>	/* alloc_chrdev_region */
#include <linux/sched.h>

#include <asm/uaccess.h>	/* copy_from_user, copy_to_user */
#include <linux/cdev.h>	/* cdev_init */
#include <linux/ioctl.h>	/* _IO* */
#include <linux/cpumask.h>	/* cpumask_weight() */

#define MODNAME "lbprofile"
static char *lbprofile_log_prefix = "module[lbprofile]";
#define MINOR_COUNT 1 // num of minor number

static dev_t dev_id;  // device number
static struct cdev c_dev; // structure of charctor device

#define IO_MAGIC				'k'
#define IOC_USEREND_NOTIFY			_IO(IO_MAGIC, 0)	/* ユーザアプリ終了時 */
#define IOC_SIGRESET_REQUEST		_IO(IO_MAGIC, 1)	/* send_sig_argをリセット要求 */
#define IOC_SETSIGNO				_IO(IO_MAGIC, 2)	/* シグナル番号を設定 */
#define IOC_SETGRAN				_IO(IO_MAGIC, 3)	/* データの転送粒度を設定 */
#define IOC_SETPID				_IO(IO_MAGIC, 4)	/* PIDを設定 */

int fwd_gran;

enum signal_ready_status{
	PID_READY,
	SIGNO_READY,
	GRAN_READY,
	SIG_READY,
	SIGRESET_REQUEST,
	SIGRESET_ACCEPTED,
	MAX_STATUS
};

struct lb{
	pid_t pid;
	int src_cpu, dst_cpu;
};

#define NR_CELL	6

struct lb_cell{
	struct lb *cell;
	struct lb_cell *next;
	int nr_lb;	/* このセルに現在入っているlbの数(0 <= nr_lb < fwd_gran) */
};

struct ring_buf_ctl{
	int buflen;
	struct lb_cell *rbuf;	/* リングバッファの先頭アドレス */

	/*
	* w_curr:lbを記録していっているcellのアドレス
	* r_curr:lbを溜めているcellの先頭アドレス
	*/
	struct lb_cell *w_curr, *r_curr;
};

struct ring_buf_ctl ring_buf;

struct send_signal_arg {	/* ユーザ空間とシグナルで通信するための管理用構造体 */
	enum signal_ready_status sr_status;
	int signo;
	struct siginfo info;
	struct task_struct *t;
};


struct send_signal_arg lbprofile_arg;

static struct timer_list lbprofile_flush_timer;
#define LBPROFILE_FLUSH_PERIOD	2000	/* この周期でタイマーが設定される */

extern int send_sig_info(int sig, struct siginfo *info, struct task_struct *p);



/* read(2)待ちのCELLがいくつあるか返す関数 */
static int rwait_len(void)
{
	int r_idx, w_idx, len = 0;

	r_idx = (int)(ring_buf.r_curr - ring_buf.rbuf);
	w_idx = (int)(ring_buf.w_curr - ring_buf.rbuf);

	if(r_idx < w_idx){
		len = w_idx - r_idx;
	}
	else if(r_idx > w_idx){
		len = NR_CELL - (r_idx - w_idx);
	}
	else{	/* r_curr == w_curr 正常系か異常系かは関知していない */
		;
	}

	return len;
}

/* lbprofileリスナが終了する時 or カーネルモジュールのrmmmod時に呼び出される関数 リングバッファを再使用可能な状態にする */
static void restore_ring_buf(void)
{
	int i;

	for(i = 0; i < NR_CELL; i++){
		ring_buf.rbuf[i].nr_lb = 0;
	}

	ring_buf.buflen = 0;
	ring_buf.r_curr = &ring_buf.rbuf[0];
	ring_buf.w_curr = &ring_buf.rbuf[0];
}

/* メモリをallocしてリングバッファを構築する関数 */
static void build_ring_buf(void)
{
	int i;

	/* メモリを確保 */
	ring_buf.rbuf = kzalloc(sizeof(struct lb_cell) * NR_CELL, GFP_KERNEL);

	for(i = 0; i < NR_CELL; i++){
		ring_buf.rbuf[i].cell = kzalloc(sizeof(struct lb) * fwd_gran, GFP_KERNEL);
	}

	/* アドレスをリンクする */
	for(i = 0; i < NR_CELL; i++){
		if(i < NR_CELL - 1){
			ring_buf.rbuf[i].next = &ring_buf.rbuf[i + 1];
		}
		else{	/* 最後のcellは最初のcellにつなぐ */
			ring_buf.rbuf[i].next = &ring_buf.rbuf[0];
		}
		ring_buf.rbuf[i].nr_lb = 0;
	}

	/* 初期値を代入 */
	ring_buf.buflen = 0;
	ring_buf.w_curr = &ring_buf.rbuf[0];
	ring_buf.r_curr = &ring_buf.rbuf[0];
}

/*
	##### システムコールの実装 #####
*/

/* open(2) */
static int lbprofile_open(struct inode *inode, struct file *filp) 
{
	printk(KERN_INFO "%s : cdev_open\n", lbprofile_log_prefix);
	return 0;
}

/* close(2) */
static int lbprofile_release(struct inode* inode, struct file* filp)
{
	printk(KERN_INFO "%s : cdev_release\n", lbprofile_log_prefix);
	return 0;
}


/* read(2) */
static ssize_t lbprofile_read(struct file* filp, char* buf, size_t count, loff_t* offset)
{
	int len, rlen;

	if(lbprofile_arg.sr_status == SIGRESET_REQUEST){

		rlen = rwait_len();

		if(rlen > 0){	/* read(2)待ちがある場合 */
			if(copy_to_user(buf, ring_buf.r_curr->cell, sizeof(struct lb) * fwd_gran)){
				printk(KERN_WARNING "%s : copy_to_user failed\n", lbprofile_log_prefix);
				return -EFAULT;
			}
			printk(KERN_INFO "%s : rlen = %d\n", lbprofile_log_prefix, rlen);

			len = sizeof(struct lb) * fwd_gran;
		}
		else if(rlen == 0){	/* read(2)待ちが無い場合 */
			int i;

			if(copy_to_user(buf, ring_buf.w_curr->cell, sizeof(struct lb) * ring_buf.w_curr->nr_lb)){
				printk(KERN_WARNING "%s : copy_to_user failed\n", lbprofile_log_prefix);
				return -EFAULT;
			}
			printk(KERN_INFO "%s : ring_buffer read(2) complete\n", lbprofile_log_prefix);

			len = sizeof(struct lb) * ring_buf.w_curr->nr_lb;

			/* リングバッファメモリ領域の解放 */
			for(i = 0; i < NR_CELL; i++){
				kfree(ring_buf.rbuf[i].cell);
			}
			kfree(ring_buf.rbuf);

			lbprofile_arg.sr_status = SIGRESET_ACCEPTED;
		}
	}
	else if(lbprofile_arg.sr_status == SIG_READY){

		len = ring_buf.buflen;

		/* このコードはタイマルーチン経由 */
		printk(KERN_INFO "%s : cdev_read count = %d\n", lbprofile_log_prefix, (int)count);

		if(copy_to_user(buf, ring_buf.r_curr->cell, len)){
			printk(KERN_WARNING "%s : copy_to_user failed\n", lbprofile_log_prefix);
			return -EFAULT;
		}

		ring_buf.buflen = 0;
	}
	else{
		printk(KERN_WARNING "%s : invalid lbprofile_arg.sr_status\n", lbprofile_log_prefix);
		return 0;	/* error */
	}

	*offset += len;
	ring_buf.r_curr->nr_lb = 0;
	ring_buf.r_curr = ring_buf.r_curr->next;	/* read待ちのcellのアドレスの更新 */

	return len;
}

/*
	##### ここまで（システムコールの実装） #####
*/

/* this rutin may exit before */
static void lbprofile_flush(unsigned long __data)
{
	int len = rwait_len();

	if(lbprofile_arg.sr_status == SIGRESET_REQUEST){	/* USEREND_NOTIFYでread(2)待ちがある場合 */
		;
	}
	else if(lbprofile_arg.sr_status == SIG_READY){	/* SIG_READYである間はタイマは生きている */

		if(len > 0){	/* read(2)待ちが1以上であれば */
			printk(KERN_INFO "%s : rwait_len = %d, buflen = %d\n", lbprofile_log_prefix, len, ring_buf.buflen);

			if(ring_buf.buflen == 0){	/* rwait_len > 1の場合 */
				ring_buf.buflen = sizeof(struct lb) * fwd_gran;
			}

			send_sig_info(lbprofile_arg.signo, &lbprofile_arg.info, lbprofile_arg.t);
		}

		mod_timer(&lbprofile_flush_timer, jiffies + msecs_to_jiffies(LBPROFILE_FLUSH_PERIOD));	/* 次のタイマをセット */
	}
}

/* add_lbentry()の処理を行う前のコンテキストチェック */
static int __add_lbprofile(struct task_struct *p)
{
	if(lbprofile_arg.sr_status == SIG_READY){	/* シグナルを送信できる状態かどうか */
		struct cpumask mask;

		cpumask_clear(&mask);
		cpumask_and(&mask, to_cpumask((const unsigned long *)p->cpus_allowed.bits), cpu_active_mask);

		/* affinityマスクが指定されているかどうかのチェック。指定されていればreturn 0 */
		if(!cpumask_equal(&mask, cpu_active_mask)){	/* !(cpu_active_mask == p->cpus_allowed.bits) */
			printk(KERN_WARNING "%s : __add_lbprofile() returns 0 with cpumask\n", lbprofile_log_prefix);
			return 0;
		}

		/* リングバッファの容量が足りているかどうかのチェック。NR_CELL==2だとここは常にreturn 0。なのでNR_CELLは3以上にすること */
		//if(ring_buf.w_curr->next == ring_buf.r_curr){	/* ENOMEM */
		//	printk(KERN_WARNING "%s : __add_lbprofile() returns 0 with w_curr->next == r_curr\n", lbprofile_log_prefix);
		//	return 0;
		//}
	}
	else{
		printk(KERN_WARNING "%s : signal is not ready, sr_status=%d\n", lbprofile_log_prefix, lbprofile_arg.sr_status);
		return 0;
	}

	return 1;
}

/* sched.cで呼び出される関数 */
int add_lbprofile(struct task_struct *p, struct rq *this_rq, int src_cpu, int this_cpu)
{
	struct lb *lb;

	if(__add_lbprofile(p) == 0){	/* 以降の処理を行うかどうかの分岐 */
		return 1;
	}

	lb = &ring_buf.w_curr->cell[ring_buf.w_curr->nr_lb];

	lb->pid = p->pid;
	lb->src_cpu = src_cpu;
	lb->dst_cpu = this_cpu;

	if(ring_buf.w_curr->nr_lb == fwd_gran - 1){
		ring_buf.buflen = sizeof(struct lb) * fwd_gran;
		ring_buf.w_curr = ring_buf.w_curr->next;	/* w_currのポインタを進める */
	}
	else{
		ring_buf.w_curr->nr_lb++;
	}

	return 1;
}

EXPORT_SYMBOL(add_lbprofile);

/* implement of ioctl(2) */
static int lbprofile_ioctl(struct inode *inode, struct file *flip, unsigned int cmd, unsigned long arg)
{
	int retval = -1;
	struct pid *p;
	struct task_struct *t;

	switch(cmd){
		case IOC_USEREND_NOTIFY:	/* USEREND_NOTIFYがioctl(2)される前にユーザ側でsleep(PERIOD)してくれている */
			/* signal送信を止める処理 */
			if(lbprofile_arg.sr_status == SIG_READY){
				lbprofile_arg.sr_status = SIGRESET_REQUEST;
				printk(KERN_INFO "%s : IOC_USEREND_NOTIFY recieved\n", lbprofile_log_prefix);

				/* ユーザに通知してユーザにread(2)してもらう */
				put_user((rwait_len() * fwd_gran) + ring_buf.w_curr->nr_lb, (unsigned int __user *)arg);
				retval = 1;
			}
			else{
				printk(KERN_INFO "%s : IOC_USEREND_NOTIFY was regarded\n", lbprofile_log_prefix);
				retval = -EPERM;
			}
			break;

		case IOC_SIGRESET_REQUEST:
			/* シグナルを止める処理 */
			if(lbprofile_arg.sr_status == SIG_READY){
				lbprofile_arg.sr_status = SIGRESET_REQUEST;
				printk(KERN_INFO "%s : IOC_SIGRESET_REQUES recieved\n", lbprofile_log_prefix);
				retval = 1;
			}
			else{
				printk(KERN_INFO "%s : IOC_SIGRESET_REQUEST was regarded\n", lbprofile_log_prefix);
				retval = -EPERM;
			}
			break;

		case IOC_SETSIGNO:
			printk(KERN_INFO "%s : IOC_SETSIGNO accepted\n", lbprofile_log_prefix);
			lbprofile_arg.signo = arg;
			lbprofile_arg.info.si_signo = arg;

			lbprofile_arg.sr_status = SIGNO_READY;

			retval = 1;
			break;

		case IOC_SETGRAN:
			printk(KERN_INFO "%s : IOC_SETGRAN accepted arg = %lu\n", lbprofile_log_prefix, arg);

			fwd_gran = arg;

			lbprofile_arg.sr_status = GRAN_READY;

			retval = 1;
			break;

		case IOC_SETPID:
			printk(KERN_INFO "%s : IOC_SETPID accepted\n", lbprofile_log_prefix);
			p = find_vpid(arg);	/* get struct pid* from arg */
			t = pid_task(p, PIDTYPE_PID);	/* get struct task_struct* from p */
			lbprofile_arg.t = t;
			lbprofile_arg.info.si_errno = 0;
			lbprofile_arg.info.si_code = SI_KERNEL;
			lbprofile_arg.info.si_pid = 0;
			lbprofile_arg.info.si_uid = 0;

			if(lbprofile_arg.sr_status == GRAN_READY){
				lbprofile_arg.sr_status = SIG_READY;
			}
			else{
				lbprofile_arg.sr_status = PID_READY;
			}

			retval = 1;
			break;
	}

	if(lbprofile_arg.sr_status == SIG_READY){	/* start timer */
		//restore_ring_buf();	/* リングバッファを使用可能な状態にする */
		build_ring_buf();	/* リングバッファを構築 */
		mod_timer(&lbprofile_flush_timer, jiffies + msecs_to_jiffies(LBPROFILE_FLUSH_PERIOD));
	}

	return retval;
}


// ファイルオペレーション構造体
// スペシャルファイルに対して読み書きなどを行ったときに呼び出す関数を登録する
static struct file_operations lbprofile_fops = {
	.owner   = THIS_MODULE,
	.open    = lbprofile_open,
	.release = lbprofile_release,
	.read    = lbprofile_read,
	.write   = NULL,
	.ioctl	= lbprofile_ioctl,
	//.unlocked_ioctl   = lbprofile_ioctl,	/* kernel 2.6.36以降はunlocked_ioctl */
};

// モジュール初期化
static int __init lbprofile_module_init(void)
{
	int ret;

	// キャラクタデバイス番号の動的取得
	ret = alloc_chrdev_region(&dev_id, // 最初のデバイス番号が入る
		0,  // マイナー番号の開始番号
		MINOR_COUNT, // 取得するマイナー番号数
		MODNAME // モジュール名
		);
	if(ret < 0){
		printk(KERN_WARNING "%s : alloc_chrdev_region failed\n", lbprofile_log_prefix);
		return ret;
	}

	// キャラクタデバイス初期化
	// ファイルオペレーション構造体の指定もする
	cdev_init(&c_dev, &lbprofile_fops);
	c_dev.owner = THIS_MODULE;

	// キャラクタデバイスの登録
	// MINOR_COUNT が 1 でマイナー番号の開始番号が 0 なので /dev/lbprofile0 が
	// 対応するスペシャルファイルになる
	ret = cdev_add(&c_dev, dev_id, MINOR_COUNT);

	if(ret < 0){
		printk(KERN_WARNING "%s : cdev_add failed\n", lbprofile_log_prefix);
		return ret;
	}

	setup_timer(&lbprofile_flush_timer, lbprofile_flush, 0);

	lbprofile_arg.sr_status = MAX_STATUS;

	printk(KERN_INFO "%s : lbprofile is loaded\n", lbprofile_log_prefix);
	printk(KERN_INFO "%s : lbprofile %d %d\n", lbprofile_log_prefix, IOC_SETSIGNO, IOC_SETPID);

	return 0;
}

//  exit operations
static void __exit lbprofile_module_exit(void)
{
	int i;

	cdev_del(&c_dev);	/* デバイスの削除 */

	del_timer_sync(&lbprofile_flush_timer);	/* タイマの終了 */

	/* リングバッファメモリ領域の解放 */
	for(i = 0; i < NR_CELL; i++){
		kfree(ring_buf.rbuf[i].cell);
	}
	kfree(ring_buf.rbuf);

	unregister_chrdev_region(dev_id, MINOR_COUNT);	/* メジャー番号の解放 */
	printk(KERN_INFO "%s : lbprofile is removed\n", lbprofile_log_prefix);
}

module_init(lbprofile_module_init);
module_exit(lbprofile_module_exit);

MODULE_DESCRIPTION("This module gives interface of lbprofile data to user space");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("K.Shimada");

