/* -*- tab-width:8 -*- */
#define DEBUG 0
/*
 Copyright (C)  2011 Peter Zotov <whitequark@whitequark.org>
 Use of this source code is governed by a BSD-style
 license that can be found in the LICENSE file.
*/

#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <stlink-common.h>

#include "gdb-remote.h"

#define DEFAULT_LOGGING_LEVEL 50
#define DEFAULT_GDB_LISTEN_PORT 4242
#define PERSIST	 3

#define STRINGIFY_inner(name) #name
#define STRINGIFY(name) STRINGIFY_inner(name)

#define FLASH_BASE 0x08000000

//Allways update the FLASH_PAGE before each use, by calling stlink_calculate_pagesize
#define FLASH_PAGE (sl->flash_pgsz)

static const char hex[] = "0123456789abcdef";

static const char* current_memory_map = NULL;

typedef struct _st_state_t {
    // things from command line, bleh
    int stlink_version;
    // "/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FTE531X6-if00-port0" is only 58 chars
    char devicename[100];
    int logging_level;
    int perist_mode;
	int listen_port;
} st_state_t;


int serve(stlink_t *sl, int port);
char* make_memory_map(stlink_t *sl);


int parse_options(int argc, char** argv, st_state_t *st) {
    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", optional_argument, NULL, 'v'},
        {"enableP", no_argument, NULL, 'e'},
        {"device", required_argument, NULL, 'd'},
        {"stlink_version", required_argument, NULL, 's'},
        {"stlinkv1", no_argument, NULL, '1'},
		{"listen_port", required_argument, NULL, 'p'},
        {0, 0, 0, 0},
    };
	const char * help_str = "%s - usage:\n\n"
	"  -h, --help\t\tPrint this help\n"
	"  -e  --enableP puts the gdbserver in persitent mode\n"
	"  -vXX, --verbose=XX\tspecify a specific verbosity level (0..99)\n"
	"  -v, --verbose\tspecify generally verbose logging\n"
	"  -d <device>, --device=/dev/stlink2_1\n"
	"\t\t\tWhere is your stlink device connected?\n"
	"  -s X, --stlink_version=X\n"
	"\t\t\tChoose what version of stlink to use, (defaults to 2)\n"
	"  -1, --stlinkv1\tForce stlink version 1\n"
	"  -p 4242, --listen_port=1234\n"
	"\t\t\tSet the gdb server listen port. "
	"(default port: " STRINGIFY(DEFAULT_GDB_LISTEN_PORT) ")\n"
	;


    int option_index = 0;
    st->perist_mode = 0;
    int c;
    int q;
    while ((c = getopt_long(argc, argv, "hve::d:s:1p:", long_options, &option_index)) != -1) {
        switch (c) {
        case 0:
            printf("XXXXX Shouldn't really normally come here, only if there's no corresponding option\n");
            printf("option %s", long_options[option_index].name);
            if (optarg) {
                printf(" with arg %s", optarg);
            }
            printf("\n");
            break;
        case 'h':
            printf(help_str, argv[0]);
            exit(EXIT_SUCCESS);
            break;
        case 'v':
            if (optarg) {
                st->logging_level = atoi(optarg);
            } else {
                st->logging_level = DEFAULT_LOGGING_LEVEL;
            }
            break;
        case 'd':
            if (strlen(optarg) > sizeof (st->devicename)) {
                fprintf(stderr, "device name too long: %zd\n", strlen(optarg));
            } else {
                strcpy(st->devicename, optarg);
            }
            break;
		case '1':
			st->stlink_version = 1;
			break;
		case 's':
			sscanf(optarg, "%i", &q);
			if (q < 0 || q > 2) {
				fprintf(stderr, "stlink version %d unknown!\n", q);
				exit(EXIT_FAILURE);
			}
			st->stlink_version = q;
			break;
		case 'p':
			sscanf(optarg, "%i", &q);
			if (q < 0) {
				fprintf(stderr, "Can't use a negative port to listen on: %d\n", q);
				exit(EXIT_FAILURE);
			}
			st->listen_port = q;
			break;
		case 'e':
			st->perist_mode = PERSIST;
			printf("Running in PERSISTENT Mode\n");
			break;

        }
    }

    if (optind < argc) {
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
    }
    return 0;
}

/* Quick fix to make Ctrl-C not lock up interface on OXS.  Needs global
 * variable refactoring out */
stlink_t *sl;
void catcher(int sig) {
	if(sl != NULL)
		stlink_close(sl);
	exit(1);
}

int main(int argc, char** argv) {

	sl = NULL;

	st_state_t state;
	memset(&state, 0, sizeof(state));
	// set defaults...
	state.stlink_version = 2;
	state.logging_level = DEFAULT_LOGGING_LEVEL;
	state.listen_port = DEFAULT_GDB_LISTEN_PORT;
	parse_options(argc, argv, &state);
	switch (state.stlink_version) {
	case 2:
		sl = stlink_open_usb(state.logging_level);
		if(sl == NULL) return 1;
		break;
	case 1:
		sl = stlink_v1_open(state.logging_level);
		if(sl == NULL) return 1;
		break;
    }
    
	signal(SIGINT, catcher);

	printf("Chip ID is %08x, Core ID is  %08x.\n", sl->chip_id, sl->core_id);

	sl->verbose=0;

	current_memory_map = make_memory_map(sl);

	while(serve(sl, state.listen_port) == state.perist_mode);

	/* Switch back to mass storage mode before closing. */
	stlink_reset(sl);
	stlink_run(sl);
	stlink_exit_debug_mode(sl);
	stlink_close(sl);

	return 0;
}

static const char* const memory_map_template_F4 =
  "<?xml version=\"1.0\"?>"
  "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map V1.0//EN\""
  "     \"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
  "<memory-map>"
  "  <memory type=\"rom\" start=\"0x00000000\" length=\"0x100000\"/>"       // code = sram, bootrom or flash; flash is bigger
  "  <memory type=\"ram\" start=\"0x20000000\" length=\"0x30000\"/>"        // sram
  "  <memory type=\"flash\" start=\"0x08000000\" length=\"0x10000\">"		//Sectors 0..3
  "    <property name=\"blocksize\">0x4000</property>"						//16kB
  "  </memory>"
  "  <memory type=\"flash\" start=\"0x08010000\" length=\"0x10000\">"		//Sector 4
  "    <property name=\"blocksize\">0x10000</property>"						//64kB
  "  </memory>"
  "  <memory type=\"flash\" start=\"0x08020000\" length=\"0x70000\">"		//Sectors 5..11
  "    <property name=\"blocksize\">0x20000</property>"						//128kB
  "  </memory>"
  "  <memory type=\"ram\" start=\"0x40000000\" length=\"0x1fffffff\"/>" 	// peripheral regs
  "  <memory type=\"ram\" start=\"0xe0000000\" length=\"0x1fffffff\"/>" 	// cortex regs
  "  <memory type=\"rom\" start=\"0x1fff0000\" length=\"0x7800\"/>"         // bootrom
  "  <memory type=\"rom\" start=\"0x1fffc000\" length=\"0x10\"/>"        	// option byte area
  "</memory-map>";

static const char* const memory_map_template =
  "<?xml version=\"1.0\"?>"
  "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map V1.0//EN\""
  "     \"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
  "<memory-map>"
  "  <memory type=\"rom\" start=\"0x00000000\" length=\"0x%x\"/>"       // code = sram, bootrom or flash; flash is bigger
  "  <memory type=\"ram\" start=\"0x20000000\" length=\"0x%x\"/>"       // sram 8k
  "  <memory type=\"flash\" start=\"0x08000000\" length=\"0x%x\">"
  "    <property name=\"blocksize\">0x%x</property>"
  "  </memory>"
  "  <memory type=\"ram\" start=\"0x40000000\" length=\"0x1fffffff\"/>" // peripheral regs
  "  <memory type=\"ram\" start=\"0xe0000000\" length=\"0x1fffffff\"/>" // cortex regs
  "  <memory type=\"rom\" start=\"0x%08x\" length=\"0x%x\"/>"           // bootrom
  "  <memory type=\"rom\" start=\"0x1ffff800\" length=\"0x8\"/>"        // option byte area
  "</memory-map>";

char* make_memory_map(stlink_t *sl) {
	/* This will be freed in serve() */
	char* map = malloc(4096);
	map[0] = '\0';

	if(sl->chip_id==STM32F4_CHIP_ID) {
    	strcpy(map, memory_map_template_F4);
    } else {
        snprintf(map, 4096, memory_map_template,
			sl->flash_size,
			sl->sram_size,
			sl->flash_size, sl->flash_pgsz,
			sl->sys_base, sl->sys_size);
    }
	return map;
}


/* 
 * DWT_COMP0     0xE0001020
 * DWT_MASK0     0xE0001024
 * DWT_FUNCTION0 0xE0001028
 * DWT_COMP1     0xE0001030
 * DWT_MASK1     0xE0001034
 * DWT_FUNCTION1 0xE0001038
 * DWT_COMP2     0xE0001040
 * DWT_MASK2     0xE0001044
 * DWT_FUNCTION2 0xE0001048
 * DWT_COMP3     0xE0001050
 * DWT_MASK3     0xE0001054
 * DWT_FUNCTION3 0xE0001058
 */

#define DATA_WATCH_NUM 4

enum watchfun { WATCHDISABLED = 0, WATCHREAD = 5, WATCHWRITE = 6, WATCHACCESS = 7 };

struct code_hw_watchpoint {
	stm32_addr_t addr;
	uint8_t mask;
	enum watchfun fun;
};

struct code_hw_watchpoint data_watches[DATA_WATCH_NUM];

static void init_data_watchpoints(stlink_t *sl) {
	#ifdef DEBUG
	printf("init watchpoints\n");
	#endif

	// set trcena in debug command to turn on dwt unit
	stlink_read_mem32(sl, 0xE000EDFC, 4);
	sl->q_buf[3] |= 1;
	stlink_write_mem32(sl, 0xE000EDFC, 4);

	// make sure all watchpoints are cleared
	memset(sl->q_buf, 0, 4);
	for(int i = 0; i < DATA_WATCH_NUM; i++) {
		data_watches[i].fun = WATCHDISABLED;
		stlink_write_mem32(sl, 0xe0001028 + i * 16, 4);
	}
}

static int add_data_watchpoint(stlink_t *sl, enum watchfun wf, stm32_addr_t addr, unsigned int len)
{
	int i = 0;
	uint32_t mask;

	// computer mask
	// find a free watchpoint
	// configure

	mask = -1;
	i = len;
	while(i) {
		i >>= 1;
		mask++;
	}

	if((mask != -1) && (mask < 16)) {
		for(i = 0; i < DATA_WATCH_NUM; i++) {
			// is this an empty slot ?
			if(data_watches[i].fun == WATCHDISABLED) {
				#ifdef DEBUG
				printf("insert watchpoint %d addr %x wf %u mask %u len %d\n", i, addr, wf, mask, len);
				#endif

				data_watches[i].fun = wf;
				data_watches[i].addr = addr;
				data_watches[i].mask = mask;

				// insert comparator address
				sl->q_buf[0] = (addr & 0xff);
				sl->q_buf[1] = ((addr >> 8) & 0xff);
				sl->q_buf[2] = ((addr >> 16) & 0xff);
				sl->q_buf[3] = ((addr >> 24)  & 0xff);

				stlink_write_mem32(sl, 0xE0001020 + i * 16, 4);

				// insert mask
				memset(sl->q_buf, 0, 4);
				sl->q_buf[0] = mask;
				stlink_write_mem32(sl, 0xE0001024 + i * 16, 4);

				// insert function
				memset(sl->q_buf, 0, 4);
				sl->q_buf[0] = wf;
				stlink_write_mem32(sl, 0xE0001028 + i * 16, 4);

				// just to make sure the matched bit is clear !
				stlink_read_mem32(sl,  0xE0001028 + i * 16, 4);
				return 0;
			}
		}
	}

	#ifdef DEBUG
	printf("failure: add watchpoints addr %x wf %u len %u\n", addr, wf, len);
	#endif
	return -1;
}

static int delete_data_watchpoint(stlink_t *sl, stm32_addr_t addr)
{
	int i;

	for(i = 0 ; i < DATA_WATCH_NUM; i++) {
		if((data_watches[i].addr == addr) && (data_watches[i].fun != WATCHDISABLED)) {
			#ifdef DEBUG
			printf("delete watchpoint %d addr %x\n", i, addr);
			#endif

			memset(sl->q_buf, 0, 4);
			data_watches[i].fun = WATCHDISABLED;
			stlink_write_mem32(sl, 0xe0001028 + i * 16, 4);

			return 0;
		}
	}

	#ifdef DEBUG
	printf("failure: delete watchpoint addr %x\n", addr);
	#endif

	return -1;
}

#define CODE_BREAK_NUM	6
#define CODE_BREAK_LOW	0x01
#define CODE_BREAK_HIGH	0x02

struct code_hw_breakpoint {
	stm32_addr_t addr;
	int          type;
};

struct code_hw_breakpoint code_breaks[CODE_BREAK_NUM];

static void init_code_breakpoints(stlink_t *sl) {
	memset(sl->q_buf, 0, 4);
	sl->q_buf[0] = 0x03; // KEY | ENABLE
	stlink_write_mem32(sl, CM3_REG_FP_CTRL, 4);
        printf("KARL - should read back as 0x03, not 60 02 00 00\n");
        stlink_read_mem32(sl, CM3_REG_FP_CTRL, 4);

	memset(sl->q_buf, 0, 4);
	for(int i = 0; i < CODE_BREAK_NUM; i++) {
		code_breaks[i].type = 0;
		stlink_write_mem32(sl, CM3_REG_FP_COMP0 + i * 4, 4);
	}
}

static int update_code_breakpoint(stlink_t *sl, stm32_addr_t addr, int set) {
	stm32_addr_t fpb_addr = addr & ~0x3;
	int type = addr & 0x2 ? CODE_BREAK_HIGH : CODE_BREAK_LOW;

	if(addr & 1) {
		fprintf(stderr, "update_code_breakpoint: unaligned address %08x\n", addr);
		return -1;
	}

	int id = -1;
	for(int i = 0; i < CODE_BREAK_NUM; i++) {
		if(fpb_addr == code_breaks[i].addr ||
			(set && code_breaks[i].type == 0)) {
			id = i;
			break;
		}
	}

	if(id == -1) {
		if(set) return -1; // Free slot not found
		else	return 0;  // Breakpoint is already removed
	}

	struct code_hw_breakpoint* brk = &code_breaks[id];

	brk->addr = fpb_addr;

	if(set) brk->type |= type;
	else	brk->type &= ~type;

	memset(sl->q_buf, 0, 4);

	if(brk->type == 0) {
		#ifdef DEBUG
		printf("clearing hw break %d\n", id);
		#endif

		stlink_write_mem32(sl, 0xe0002008 + id * 4, 4);
	} else {
		sl->q_buf[0] = ( brk->addr        & 0xff) | 1;
		sl->q_buf[1] = ((brk->addr >> 8)  & 0xff);
		sl->q_buf[2] = ((brk->addr >> 16) & 0xff);
		sl->q_buf[3] = ((brk->addr >> 24) & 0xff) | (brk->type << 6);

		#ifdef DEBUG
		printf("setting hw break %d at %08x (%d)\n",
			id, brk->addr, brk->type);
		printf("reg %02x %02x %02x %02x\n",
			sl->q_buf[3], sl->q_buf[2], sl->q_buf[1], sl->q_buf[0]);
		#endif

		stlink_write_mem32(sl, 0xe0002008 + id * 4, 4);
	}

	return 0;
}


struct flash_block {
	stm32_addr_t addr;
	unsigned     length;
	uint8_t*     data;

	struct flash_block* next;
};

static struct flash_block* flash_root;

static int flash_add_block(stm32_addr_t addr, unsigned length, stlink_t *sl) {

	if(addr < FLASH_BASE || addr + length > FLASH_BASE + sl->flash_size) {
		fprintf(stderr, "flash_add_block: incorrect bounds\n");
		return -1;
	}

	stlink_calculate_pagesize(sl, addr);
	if(addr % FLASH_PAGE != 0 || length % FLASH_PAGE != 0) {
		fprintf(stderr, "flash_add_block: unaligned block\n");
		return -1;
	}

	struct flash_block* new = malloc(sizeof(struct flash_block));
	new->next = flash_root;

	new->addr   = addr;
	new->length = length;
	new->data   = calloc(length, 1);

	flash_root = new;

	return 0;
}

static int flash_populate(stm32_addr_t addr, uint8_t* data, unsigned length) {
	int fit_blocks = 0, fit_length = 0;

	for(struct flash_block* fb = flash_root; fb; fb = fb->next) {
		/* Block: ------X------Y--------
		 * Data:            a-----b
		 *                a--b
		 *            a-----------b
		 * Block intersects with data, if:
		 *  a < Y && b > x
		 */

		unsigned X = fb->addr, Y = fb->addr + fb->length;
		unsigned a = addr, b = addr + length;
		if(a < Y && b > X) {
			// from start of the block
			unsigned start = (a > X ? a : X) - X;
			unsigned end   = (b > Y ? Y : b) - X;

			memcpy(fb->data + start, data, end - start);

			fit_blocks++;
			fit_length += end - start;
		}
	}

	if(fit_blocks == 0) {
		fprintf(stderr, "Unfit data block %08x -> %04x\n", addr, length);
		return -1;
	}

	if(fit_length != length) {
		fprintf(stderr, "warning: data block %08x -> %04x truncated to %04x\n",
			addr, length, fit_length);
		fprintf(stderr, "(this is not an error, just a GDB glitch)\n");
	}

	return 0;
}

static int flash_go(stlink_t *sl) {
	int error = -1;

	// Some kinds of clock settings do not allow writing to flash.
	stlink_reset(sl);

	for(struct flash_block* fb = flash_root; fb; fb = fb->next) {
		#ifdef DEBUG
		printf("flash_do: block %08x -> %04x\n", fb->addr, fb->length);
		#endif

		unsigned length = fb->length;
		for(stm32_addr_t page = fb->addr; page < fb->addr + fb->length; page += FLASH_PAGE) {

			//Update FLASH_PAGE
			stlink_calculate_pagesize(sl, page);

			#ifdef DEBUG
			printf("flash_do: page %08x\n", page);
			#endif

			if(stlink_write_flash(sl, page, fb->data + (page - fb->addr),
					length > FLASH_PAGE ? FLASH_PAGE : length) < 0)
				goto error;
			}
	}

	stlink_reset(sl);

	error = 0;

error:
	for(struct flash_block* fb = flash_root, *next; fb; fb = next) {
		next = fb->next;
		free(fb->data);
		free(fb);
	}

	flash_root = NULL;

	return error;
}

int serve(stlink_t *sl, int port) {

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0) {
		perror("socket");
		return 1;
	}

	unsigned int val = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	struct sockaddr_in serv_addr = {0};
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	serv_addr.sin_port = htons(port);

	if(bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		perror("bind");
		return 1;
	}

	if(listen(sock, 5) < 0) {
		perror("listen");
		return 1;
	}

	stlink_force_debug(sl);
	stlink_reset(sl);
	init_code_breakpoints(sl);
	init_data_watchpoints(sl);

	printf("Listening at *:%d...\n", port);

	int client = accept(sock, NULL, NULL);
	signal (SIGINT, SIG_DFL);
	if(client < 0) {
		perror("accept");
		return 1;
	}

	close(sock);

	printf("GDB connected.\n");

	/*
	 * To allow resetting the chip from GDB it is required to
	 * emulate attaching and detaching to target.
	 */
	unsigned int attached = 1;

	while(1) {
		char* packet;

		int status = gdb_recv_packet(client, &packet);
		if(status < 0) {
			fprintf(stderr, "cannot recv: %d\n", status);
			return 1;
		}

		#ifdef DEBUG
		printf("recv: %s\n", packet);
		#endif

		char* reply = NULL;
                reg regp;

		switch(packet[0]) {
		case 'q': {
			if(packet[1] == 'P' || packet[1] == 'C' || packet[1] == 'L') {
				reply = strdup("");
				break;
			}

			char *separator = strstr(packet, ":"), *params = "";
			if(separator == NULL) {
				separator = packet + strlen(packet);
			} else {
				params = separator + 1;
			}

			unsigned queryNameLength = (separator - &packet[1]);
			char* queryName = calloc(queryNameLength + 1, 1);
			strncpy(queryName, &packet[1], queryNameLength);

			#ifdef DEBUG
			printf("query: %s;%s\n", queryName, params);
			#endif

			if(!strcmp(queryName, "Supported")) {
				reply = strdup("PacketSize=3fff;qXfer:memory-map:read+");
			} else if(!strcmp(queryName, "Xfer")) {
				char *type, *op, *s_addr, *s_length;
				char *tok = params;
				char *annex __attribute__((unused));

				type     = strsep(&tok, ":");
				op       = strsep(&tok, ":");
				annex    = strsep(&tok, ":");
				s_addr   = strsep(&tok, ",");
				s_length = tok;

				unsigned addr = strtoul(s_addr, NULL, 16),
				       length = strtoul(s_length, NULL, 16);

				#ifdef DEBUG
				printf("Xfer: type:%s;op:%s;annex:%s;addr:%d;length:%d\n",
					type, op, annex, addr, length);
				#endif

				const char* data = NULL;

				if(!strcmp(type, "memory-map") && !strcmp(op, "read"))
					data = current_memory_map;

				if(data) {
					unsigned data_length = strlen(data);
					if(addr + length > data_length)
						length = data_length - addr;

					if(length == 0) {
						reply = strdup("l");
					} else {
						reply = calloc(length + 2, 1);
						reply[0] = 'm';
						strncpy(&reply[1], data, length);
					}
				}
			} else if(!strncmp(queryName, "Rcmd,",4)) {
				// Rcmd uses the wrong separator
				char *separator = strstr(packet, ","), *params = "";
				if(separator == NULL) {
					separator = packet + strlen(packet);
				} else {
					params = separator + 1;
				}
				

				if (!strncmp(params,"72657375",8)) {// resume
#ifdef DEBUG
					printf("Rcmd: resume\n");
#endif
					stlink_run(sl);

					reply = strdup("OK");
				} else if (!strncmp(params,"6861",4)) { //half
					reply = strdup("OK");
					
					stlink_force_debug(sl);

#ifdef DEBUG
					printf("Rcmd: halt\n");
#endif
				} else if (!strncmp(params,"72657365",8)) { //reset
					reply = strdup("OK");
					
					stlink_force_debug(sl);
					stlink_reset(sl);
					init_code_breakpoints(sl);
					init_data_watchpoints(sl);
					
#ifdef DEBUG
					printf("Rcmd: reset\n");
#endif
				} else {
#ifdef DEBUG
					printf("Rcmd: %s\n", params);
#endif

				}
				
			}

			if(reply == NULL)
				reply = strdup("");

			free(queryName);

			break;
		}

		case 'v': {
			char *params = NULL;
			char *cmdName = strtok_r(packet, ":;", &params);

			cmdName++; // vCommand -> Command

			if(!strcmp(cmdName, "FlashErase")) {
				char *s_addr, *s_length;
				char *tok = params;

				s_addr   = strsep(&tok, ",");
				s_length = tok;

				unsigned addr = strtoul(s_addr, NULL, 16),
				       length = strtoul(s_length, NULL, 16);

				#ifdef DEBUG
				printf("FlashErase: addr:%08x,len:%04x\n",
					addr, length);
				#endif

				if(flash_add_block(addr, length, sl) < 0) {
					reply = strdup("E00");
				} else {
					reply = strdup("OK");
				}
			} else if(!strcmp(cmdName, "FlashWrite")) {
				char *s_addr, *data;
				char *tok = params;

				s_addr = strsep(&tok, ":");
				data   = tok;

				unsigned addr = strtoul(s_addr, NULL, 16);
				unsigned data_length = status - (data - packet);

				// Length of decoded data cannot be more than
				// encoded, as escapes are removed.
				// Additional byte is reserved for alignment fix.
				uint8_t *decoded = calloc(data_length + 1, 1);
				unsigned dec_index = 0;
				for(int i = 0; i < data_length; i++) {
					if(data[i] == 0x7d) {
						i++;
						decoded[dec_index++] = data[i] ^ 0x20;
					} else {
						decoded[dec_index++] = data[i];
					}
				}

				// Fix alignment
				if(dec_index % 2 != 0)
					dec_index++;

				#ifdef DEBUG
				printf("binary packet %d -> %d\n", data_length, dec_index);
				#endif

				if(flash_populate(addr, decoded, dec_index) < 0) {
					reply = strdup("E00");
				} else {
					reply = strdup("OK");
				}
			} else if(!strcmp(cmdName, "FlashDone")) {
				if(flash_go(sl) < 0) {
					reply = strdup("E00");
				} else {
					reply = strdup("OK");
				}
			} else if(!strcmp(cmdName, "Kill")) {
				attached = 0;

				reply = strdup("OK");
			}

			if(reply == NULL)
				reply = strdup("");

			break;
		}

		case 'c':
			stlink_run(sl);

			while(1) {
				int status = gdb_check_for_interrupt(client);
				if(status < 0) {
					fprintf(stderr, "cannot check for int: %d\n", status);
					return 1;
				}

				if(status == 1) {
					stlink_force_debug(sl);
					break;
				}

				stlink_status(sl);
				if(sl->core_stat == STLINK_CORE_HALTED) {
					break;
				}

				usleep(100000);
			}

			reply = strdup("S05"); // TRAP
			break;

		case 's':
			stlink_step(sl);

			reply = strdup("S05"); // TRAP
			break;

		case '?':
			if(attached) {
				reply = strdup("S05"); // TRAP
			} else {
				/* Stub shall reply OK if not attached. */
				reply = strdup("OK");
			}
			break;

		case 'g':
			stlink_read_all_regs(sl, &regp);

			reply = calloc(8 * 16 + 1, 1);
			for(int i = 0; i < 16; i++)
				sprintf(&reply[i * 8], "%08x", htonl(regp.r[i]));

			break;

		case 'p': {
			unsigned id = strtoul(&packet[1], NULL, 16);
                        unsigned myreg = 0xDEADDEAD;

			if(id < 16) {
				stlink_read_reg(sl, id, &regp);
				myreg = htonl(regp.r[id]);
			} else if(id == 0x19) {
				stlink_read_reg(sl, 16, &regp);
				myreg = htonl(regp.xpsr);
			} else {
				reply = strdup("E00");
			}

			reply = calloc(8 + 1, 1);
			sprintf(reply, "%08x", myreg);

			break;
		}

		case 'P': {
			char* s_reg = &packet[1];
			char* s_value = strstr(&packet[1], "=") + 1;

			unsigned reg   = strtoul(s_reg,   NULL, 16);
			unsigned value = strtoul(s_value, NULL, 16);

			if(reg < 16) {
				stlink_write_reg(sl, ntohl(value), reg);
			} else if(reg == 0x19) {
				stlink_write_reg(sl, ntohl(value), 16);
			} else {
				reply = strdup("E00");
			}

			if(!reply) {
				reply = strdup("OK");
			}

			break;
		}

		case 'G':
			for(int i = 0; i < 16; i++) {
				char str[9] = {0};
				strncpy(str, &packet[1 + i * 8], 8);
				uint32_t reg = strtoul(str, NULL, 16);
				stlink_write_reg(sl, ntohl(reg), i);
			}

			reply = strdup("OK");
			break;

		case 'm': {
			char* s_start = &packet[1];
			char* s_count = strstr(&packet[1], ",") + 1;

			stm32_addr_t start = strtoul(s_start, NULL, 16);
			unsigned     count = strtoul(s_count, NULL, 16);

			unsigned adj_start = start % 4;

			stlink_read_mem32(sl, start - adj_start, (count % 4 == 0) ?
						count : count + 4 - (count % 4));

			reply = calloc(count * 2 + 1, 1);
			for(int i = 0; i < count; i++) {
				reply[i * 2 + 0] = hex[sl->q_buf[i + adj_start] >> 4];
				reply[i * 2 + 1] = hex[sl->q_buf[i + adj_start] & 0xf];
			}

			break;
		}

		case 'M': {
			char* s_start = &packet[1];
			char* s_count = strstr(&packet[1], ",") + 1;
			char* hexdata = strstr(packet, ":") + 1;

			stm32_addr_t start = strtoul(s_start, NULL, 16);
			unsigned     count = strtoul(s_count, NULL, 16);

			for(int i = 0; i < count; i ++) {
				char hex[3] = { hexdata[i*2], hexdata[i*2+1], 0 };
				uint8_t byte = strtoul(hex, NULL, 16);
				sl->q_buf[i] = byte;
			}

			if((count % 4) == 0 && (start % 4) == 0) {
				stlink_write_mem32(sl, start, count);
			} else {
				stlink_write_mem8(sl, start, count);
			}

			reply = strdup("OK");

			break;
		}

		case 'Z': {
			char *endptr;
			stm32_addr_t addr = strtoul(&packet[3], &endptr, 16);
			stm32_addr_t len  = strtoul(&endptr[1], NULL, 16);

			switch (packet[1]) {
				case '1':
				if(update_code_breakpoint(sl, addr, 1) < 0) {
					reply = strdup("E00");
				} else {
					reply = strdup("OK");
				}
				break;

				case '2':   // insert write watchpoint
				case '3':   // insert read  watchpoint
				case '4':   // insert access watchpoint
		    		{
					enum watchfun wf;
		 			if(packet[1] == '2') {
						wf = WATCHWRITE;
					} else if(packet[1] == '3') {
						wf = WATCHREAD;
					} else {
						wf = WATCHACCESS;
						if(add_data_watchpoint(sl, wf, addr, len) < 0) {
							reply = strdup("E00");
						} else {
							reply = strdup("OK");
							break;
						}
					}
				}

				default:
				reply = strdup("");
			}
			break;
		}
		case 'z': {
			char *endptr;
			stm32_addr_t addr = strtoul(&packet[3], &endptr, 16);
			//stm32_addr_t len  = strtoul(&endptr[1], NULL, 16);

			switch (packet[1]) {
				case '1': // remove breakpoint
				update_code_breakpoint(sl, addr, 0);
				reply = strdup("OK");
				break;

				case '2' : // remove write watchpoint
				case '3' : // remove read watchpoint
				case '4' : // remove access watchpoint
				if(delete_data_watchpoint(sl, addr) < 0) {
					reply = strdup("E00");
				} else {
					reply = strdup("OK");
					break;
				}

				default:
				reply = strdup("");
			}
			break;
		}

		case '!': {
			/*
			 * Enter extended mode which allows restarting.
			 * We do support that always.
			 */

			reply = strdup("OK");

			break;
		}

		case 'R': {
			/* Reset the core. */

			stlink_reset(sl);
			init_code_breakpoints(sl);
			init_data_watchpoints(sl);

			attached = 1;

			reply = strdup("OK");

			break;
		}
		case 'k': {
			//recieved kill from gdb
			reply = 0;
			printf("GDB Client Disconnected\n");
			return PERSIST;
		}

		default:
			reply = strdup("");
		}

		if(reply) {
			#ifdef DEBUG
			printf("send: %s\n", reply);
			#endif

			int result = gdb_send_packet(client, reply);
			if(result != 0) {
				fprintf(stderr, "cannot send: %d\n", result);
				return 1;
			}

			free(reply);
		}

		free(packet);
	}

	return 0;
}
