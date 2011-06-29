#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "lbprofile.h"

int dev;
int nr_cpus;
struct lb *hndlr_buf;
struct lb_hdr hdr;

FILE *flb;

/* 返り値　成功：0　失敗：1 */
int lbprofile_init(void)
{
	fseek(flb, (long)sizeof(struct lb_hdr), SEEK_SET);	/* ヘッダ分だけシークする */

	if(ioctl(dev, IOC_SETHNDLR, SIGUSR1) < 0){	/* カーネルモジュール側にシグナル番号を通知 */
		return 1;
	}

	if(ioctl(dev, IOC_SETPID, (int)getpid()) < 0){
		return 1;
	}

	return 0;
}

/* カーネルからのシグナルで実行される関数 */
void lbprofile_operator(void)
{
	int i;
	ssize_t s_read;

	if((s_read = read(dev, hndlr_buf, sizeof(struct lb) * GRAN_LB)) != sizeof(struct lb) * GRAN_LB){
		exit(EXIT_FAILURE);
	}

	for(i = 0; i < GRAN_LB; i++){
		printf("pid:%d, src_cpu:%d, dst_cpu:%d\n", hndlr_buf[i].pid, 
			hndlr_buf[i].src_cpu, hndlr_buf[i].dst_cpu);
	}

	if(fwrite(hndlr_buf, sizeof(struct lb), GRAN_LB, flb) != GRAN_LB){
		//lbprofile_free_resources();
		exit(EXIT_FAILURE);
	}

	hdr.nr_lb += GRAN_LB;

	lseek(dev, 0, SEEK_SET);
}

#if 1
void put_hdr(struct lb_hdr *hdr)
{
	long pos;

	pos = ftell(flb);	/* カレントオフセットを記録 */
	fseek(flb, 0L, SEEK_SET);

	if(fwrite(hdr, sizeof(struct lb_hdr), 1, flb) != 1){
		;
	}

	fseek(flb, pos, SEEK_SET);	/* 記録されたオフセットに戻す */
}

void lbprofile_cleanup()
{
	unsigned int i, piece;
	ssize_t r_size;

	if(ioctl(dev, IOC_USEREND_NOTIFY, &piece) < 0){
		exit(EXIT_FAILURE);
	}
	else{
		unsigned int nr_locked_be, tip;

		printf("piece = %d\n", piece);

		if((nr_locked_be = piece / GRAN_LB)){	/* nr_locked_be回だけread(2)を回さないといけない */
			unsigned int i;

			printf("nr_locked_be = %d\n", nr_locked_be);

			for(i = 0; i < nr_locked_be; i++){
				if((r_size = read(dev, hndlr_buf, sizeof(struct lb) * GRAN_LB)) != sizeof(struct lb) * GRAN_LB){
					printf("%s read(2) failed. lbentries was not loaded r_size is %d\n",
						log_err_prefix(lbprofile_cleanup), r_size);
				}

				if(fwrite(hndlr_buf, sizeof(struct lb), GRAN_LB, flb) != GRAN_LB){
					;
				}
				hdr.nr_lb += GRAN_LB;
			}

			tip = piece % GRAN_LB;
		}
		else{
			tip = piece;
		}

		/* 端数の分だけ追加でread(2)する */
		if((r_size = read(dev, hndlr_buf, sizeof(struct lb) * tip)) != sizeof(struct lb) * tip){
			;
		}

		for(i = 0; i < tip; i++){
			printf("pid:%d, src_cpu:%d, dst_cpu:%d", hndlr_buf[i].pid, 
				hndlr_buf[i].src_cpu, hndlr_buf[i].dst_cpu);
		}

		if(fwrite(hndlr_buf, sizeof(struct lb), tip, flb) != tip){
			;
		}
		hdr.nr_lb += tip;
	}

	hdr.nr_cpus = nr_cpus;

	put_hdr(&hdr);
}
#endif

int main(int argc, char *argv[])
{
	int signo;
	sigset_t ss;

	nr_cpus = sysconf(_SC_NPROCESSORS_CONF);

	lbprofile_init();

	/* シグナルハンドリングの準備 */
	sigemptyset(&ss);

	/* block SIGUSR1 */
	if(sigaddset(&ss, SIGUSR1) != 0){
		puts("sigaddset(3) error");
		return 1;
	}

	/* block SIGTERM */
	if(sigaddset(&ss, SIGTERM) != 0){
		puts("sigaddset(3) error");
		return 1;
	}

	if(!(hndlr_buf = calloc(GRAN_LB, sizeof(struct lb)))){
		return 1;
	}

	if(!(flb = fopen("lbprofile.lb", "w+"))){	/* output file */
		return 1;
	}

	/* デバイスファイルをオープン */
	if((dev = open("/dev/lbprofile", O_RDONLY)) < 0){
		return 1;
	}

	while(1){
		if(sigwait(&ss, &signo) == 0){
			if(signo == SIGUSR1){
				lbprofile_operator();
			}
			else if(signo == SIGTERM){
				//lbprofile_cleanup();
				;
			}
		}
	}

}

