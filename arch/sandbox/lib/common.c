/*
 * common.c - common wrapper functions between U-Boot and the host
 *
 * Copyright (c) 2007 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * These are host includes. Never include any U-Boot header
 * files here...
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <libgen.h>
#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
/*
 * ...except the ones needed to connect with U-Boot
 */
#include <asm/arch/linux.h>
#include <asm/arch/hostfile.h>

static struct termios term_orig, term_vi;
static char erase_char;	/* the users erase character */

static void rawmode(void)
{
	tcgetattr(0, &term_orig);
	term_vi = term_orig;
	term_vi.c_lflag &= (~ICANON & ~ECHO & ~ISIG);
	term_vi.c_iflag &= (~IXON & ~ICRNL);
	term_vi.c_oflag |= (ONLCR);
	term_vi.c_cc[VMIN] = 1;
	term_vi.c_cc[VTIME] = 0;
	erase_char = term_vi.c_cc[VERASE];
	tcsetattr(0, TCSANOW, &term_vi);
}

static void cookmode(void)
{
	fflush(stdout);
	tcsetattr(0, TCSANOW, &term_orig);
}

void linux_putc(const char c)
{
	fputc(c, stdout);

	/* If \n, also do \r */
	if (c == '\n')
		linux_putc ('\r');

	fflush(stdout);
}

int linux_tstc(int fd)
{
	fd_set rfds;
	struct timeval tv;
	int ret;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	/*
	 * We set the timeout here to 100us, because otherwise
	 * U-Boot would eat all cpu resources while waiting
	 * for input. On the other hand this makes some
	 * things like networking slow, because U-Boot will
	 * poll this function very often.
	 */
	tv.tv_sec = 0;
	tv.tv_usec = 100;

	ret = select(fd + 1, &rfds, NULL, NULL, &tv);

	if (ret)
		return 1;

	return 0;
}

int linux_getc(void)
{
	char ret;

	read(0, &ret, 1);

	return ret;
}

uint64_t linux_get_time(void)
{
	struct timespec ts;
	uint64_t now;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	now = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;

	return now;
}

int reset_cpu(int unused)
{
	cookmode();
	exit(0);
}

void enable_interrupts(void)
{
}

void disable_interrupt(void)
{
}

int linux_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

int linux_read_nonblock(int fd, void *buf, size_t count)
{
	int oldflags, ret;

	oldflags = fcntl(fd, F_GETFL);
	if (oldflags == -1)
		goto err_out;

	if (fcntl(fd, F_SETFL, oldflags | O_NONBLOCK) == -1)
		goto err_out;

	ret = read(fd, buf, count);

	if (fcntl(fd, F_SETFL, oldflags) == -1)
		goto err_out;

	if (ret == -1)
		usleep(100);

	return ret;

err_out:
	perror("fcntl");
	return -1;
}

ssize_t linux_write(int fd, const void *buf, size_t count)
{
	return write(fd, buf, count);
}

off_t linux_lseek(int fd, off_t offset)
{
	return lseek(fd, offset, SEEK_SET);
}

void  flush_cache(unsigned long dummy1, unsigned long dummy2)
{
	/* why should we? */
}

extern void start_uboot(void);
extern void mem_malloc_init(void *start, void *end);

int add_image(char *str, char *name_template)
{
	char *file;
	int readonly = 0, map = 1;
	struct stat s;
	char *opt;
	int fd, ret;
	struct hf_platform_data *hf = malloc(sizeof(struct hf_platform_data));

	if (!hf)
		return -1;

	file = strtok(str, ",");
	while ((opt = strtok(NULL, ","))) {
		if (!strcmp(opt, "ro"))
			readonly = 1;
		if (!strcmp(opt, "map"))
			map = 1;
	}

	printf("add file %s(%s)\n", file, readonly ? "ro" : "");

	fd = open(file, readonly ? O_RDONLY : O_RDWR);
	hf->fd = fd;
	hf->filename = file;

	if (fd < 0) {
		perror("open");
		goto err_out;
	}

	if (fstat(fd, &s)) {
		perror("fstat");
		goto err_out;
	}

	hf->size = s.st_size;

	if (map) {
		hf->map_base = (unsigned long)mmap(0, hf->size,
				PROT_READ | (readonly ? 0 : PROT_WRITE),
				MAP_SHARED, fd, 0);
		if ((void *)hf->map_base == MAP_FAILED)
			printf("warning: mmapping %s failed\n", file);
	}


	ret = u_boot_register_filedev(hf, name_template);
	if (ret)
		goto err_out;
	return 0;

err_out:
	if (fd > 0)
		close(fd);
	free(hf);
	return -1;
}

static void print_usage(const char *prgname)
{
	printf(
"Usage: %s [OPTIONS]\n"
"Start U-Boot.\n"
"Options:\n"
"  -i <file>   Map a file to U-Boot. This option can be given multiple\n"
"              times. The files will show up as /dev/fd0 ... /dev/fdx\n"
"              under U-Boot.\n"
"  -e <file>   Map a file to U-Boot. With this option files are mapped as\n"
"              /dev/env0 ... /dev/envx and thus are used as default\n"
"              environment. An empty file generated with dd will do to get\n"
"              started wth an empty environment\n"
"  -O <file>   Register file as a console capable of doing stdout. File can\n"
"              be a regular file or a fifo.\n"
"  -I <file>   Register file as a console capable of doing stdin. File can\n"
"              be a regular file or a fifo.\n",
	prgname
	);
}

int main(int argc, char *argv[])
{
	void *ram;
	int opt, ret, fd;
	int malloc_size = 8 * 1024 * 1024;

	ram = malloc(malloc_size);
	if (!ram) {
		printf("unable to get malloc space\n");
		exit(1);
	}
	mem_malloc_init(ram, ram + malloc_size);

	while ((opt = getopt(argc, argv, "hi:e:I:O:")) != -1) {
		switch (opt) {
		case 'h':
			print_usage(basename(argv[0]));
			exit(0);
		case 'i':
			ret = add_image(optarg, "fd");
			if (ret)
				exit(1);
			break;
		case 'm':
			/* This option is broken. add_image needs malloc, so
			 * mem_alloc_init() has to be called before option
			 * parsing
			 */
			malloc_size = strtoul(optarg, NULL, 0);
			break;
		case 'e':
			ret = add_image(optarg, "env");
			if (ret)
				exit(1);
			break;
		case 'O':
			fd = open(optarg, O_WRONLY);
			if (fd < 0) {
				perror("open");
				exit(1);
			}

			u_boot_register_console("cout", -1, fd);
			break;
		case 'I':
			fd = open(optarg, O_RDWR);
			if (fd < 0) {
				perror("open");
				exit(1);
			}

			u_boot_register_console("cin", fd, -1);
			break;
		default:
			exit(1);
		}
	}

	u_boot_register_console("console", 1, 0);

	rawmode();
	start_uboot();

	/* never reached */
	return 0;
}
