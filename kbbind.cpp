/* kbbind - Utility for remapping Linux keyboard inputs
 * Copyright (C) 2010 Daniel Collins <solemnwarning@solemnwarning.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the author nor the names of its contributors may
 *       be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <unistd.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <map>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <algorithm>
#include <signal.h>
#include <syslog.h>
#include <sstream>
#include <sys/wait.h>

#define streq(s1, s2) (strcmp(s1, s2) == 0)
#define S(s) std::string(s)

template <class T> std::string to_string(const T &t) {
	std::ostringstream ss;
	ss << t;
	return ss.str();
}

typedef std::vector<uint16_t> key_list;
typedef std::map<key_list,key_list> key_map;
typedef std::map<key_list,std::string> exec_map;
typedef std::map<std::string,key_list> alias_map;

static void load_config(const std::string &filename);
static char *next_arg(char *s);
static key_list parse_keys(const char *keys, const std::string &filename, unsigned int lnum);
static void uinput_open();
static void uinput_write(const char *buf, size_t size);
static input_event read_event();
static void die(const std::string &err);
static void error(const std::string &err);
static void exec_command(const std::string &command);
static void uinput_write_event(const input_event &event);

static bool sink_mode = false;
static key_map key_mappings;
static exec_map exec_mappings;
static alias_map key_aliases;
static std::string uinput_path = "/dev/input/uinput";
static std::string uinput_name = "kbbind virtual keyboard";
static bool fork_daemon = false;
static bool force_sync = true;

static int input_fd = -1;
static int uinput_fd = -1;
static key_list pressed_keys;
static bool running = true;

static void sighandler(int signum) {
	if(signum == SIGCHLD) {
		waitpid(-1, NULL, WNOHANG);
	}else{
		running = false;
	}
}

int main(int argc, char **argv) {
	int opt;
	
	bool dump_mode = false;
	std::string pidfile;
	
	while((opt = getopt(argc, argv, "shu:edp:vn:r")) != -1) {
		if(opt == 's') {
			sink_mode = true;
		}else if(opt == 'h') {
			std::cout << "Usage: " << argv[0] << " [-sdvr] [-p <file>] [-u <path>] [-n <name>] -e|<config file> <input device>" << std::endl << std::endl;
			std::cout << "-s\tSink mode - Trap all events and only forward mapped ones" << std::endl;
			std::cout << "-d\tDetach from console and run as a daemon, send messages to syslog" << std::endl;
			std::cout << "-v\tPrint version number and exit" << std::endl;
			std::cout << "-p\tWrite PID to file and delete on exit" << std::endl;
			std::cout << "-u\tOverride default uinput device path (/dev/input/uinput)" << std::endl;
			std::cout << "-n\tOverride default uinput device name (kbbind virtual keyboard)" << std::endl;
			std::cout << "-e\tPrint key presses to stdout and do nothing else" << std::endl;
			std::cout << "-r\tDon't append SYN_REPORT to every uinput event" << std::endl;
			
			return 0;
		}else if(opt == 'u') {
			uinput_path = optarg;
		}else if(opt == 'e') {
			dump_mode = true;
		}else if(opt == 'd') {
			fork_daemon = true;
		}else if(opt == 'p') {
			pidfile = optarg;
		}else if(opt == 'v') {
			std::cout << "kbbind " << VERSION << std::endl;
			return 0;
		}else if(opt == 'n') {
			uinput_name = optarg;
		}else if(opt == 'r') {
			force_sync = false;
		}else if(opt == '?') {
			std::cerr << "Run '" << argv[0] << " -h' for help" << std::endl;
			return 1;
		}
	}
	
	if(optind+2 == argc && dump_mode) {
		std::cerr << "Config file not expected with -e" << std::endl;
		std::cerr << "Run '" << argv[0] << " -h' for help" << std::endl;
		
		return 1;
	}
	
	if(dump_mode && fork_daemon) {
		std::cerr << "-d and -e options conflict" << std::endl;
		std::cerr << "Run '" << argv[0] << " -h' for help" << std::endl;
		
		return 1;
	}
	
	if(optind+1+(!dump_mode) != argc) {
		std::cerr << "Incorrect argument(s)" << std::endl;
		std::cerr << "Run '" << argv[0] << " -h' for help" << std::endl;
		
		return 1;
	}
	
	if(fork_daemon) {
		int p = fork();
		if(p == -1) {
			std::cerr << "Error forking daemon process: " << strerror(errno) << std::endl;
			return 1;
		}else if(p > 0) {
			return 0;
		}
		
		openlog("kbbind", LOG_PID, LOG_DAEMON);
		
		if(!pidfile.empty()) {
			FILE *pf = fopen(pidfile.c_str(), "w");
			if(pf) {
				fprintf(pf, "%d", getpid());
				fclose(pf);
			}else{
				error(S("Error opening PID file: ") + strerror(errno));
			}
		}
		
		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
	}
	
	std::string device = argv[optind+(!dump_mode)];
	
	if(!dump_mode) {
		load_config(argv[optind]);
	}
	
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	
	sa.sa_handler = &sighandler;
	sa.sa_flags = SA_NODEFER;
	
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	
	if((input_fd = open(device.c_str(), O_RDONLY)) == -1) {
		die(S("Error opening input device: ") + strerror(errno));
		return 1;
	}
	
	assert(fcntl(input_fd, F_SETFD, FD_CLOEXEC) == 0);
	
	if(sink_mode || !key_mappings.empty()) {
		if(ioctl(input_fd, EVIOCGRAB, 2) == -1) {
			die(S("Error grabbing input device: ") + strerror(errno));
			return 1;
		}
	}
	
	if(!key_mappings.empty()) {
		uinput_open();
	}
	
	while(running) {
		input_event event = read_event();
		
		if(event.type != EV_KEY) {
			continue;
		}
		
		if(dump_mode) {
			if(event.value == 1) {
				std::cout << "Key " << event.code << " pressed" << std::endl;
			}else if(event.value == 2) {
				std::cout << "Key " << event.code << " autorepeat" << std::endl;
			}else if(event.value == 0) {
				std::cout << "Key " << event.code << " released" << std::endl;
			}
			
			continue;
		}
		
		if(event.value == 1) {
			/* Press */
			
			key_map::iterator k = key_mappings.find(pressed_keys);
			if(k != key_mappings.end() && k->second.size() > 1) {
				/* Release the keys pressed by a multi-key mapping
				 * when an extra key is pressed, to prevent the
				 * keys being stuck down if one of the trigger
				 * keys is released before this key.
				*/
				
				for(key_list::reverse_iterator k2 = k->second.rbegin(); k2 != k->second.rend(); k2++) {
					input_event event2 = event;
					event2.code = *k2;
					
					uinput_write_event(event2);
				}
			}
			
			pressed_keys.push_back(event.code);
			
			exec_map::iterator e = exec_mappings.find(pressed_keys);
			if(e != exec_mappings.end()) {
				exec_command(e->second);
			}
		}else if(event.value == 2) {
			/* Autorepeat */
			
			exec_map::iterator e = exec_mappings.find(pressed_keys);
			if(e != exec_mappings.end()) {
				exec_command(e->second);
			}
		}
		
		key_map::iterator k = key_mappings.find(pressed_keys);
		
		if(k != key_mappings.end()) {
			/* The following code deals with forwarding key mappings
			 * using uinput.
			 *
			 * Mappings which resolve to multiple keys work by pressing
			 * all the keys when the trigger is pressed, repeating the
			 * final key when the trigger repeats and releasing all
			 * keys in reverse when the trigger is released.
			*/
			
			if(k->second.size() == 1) {
				input_event event2 = event;
				event2.code = *(k->second.begin());
				
				uinput_write_event(event2);
			}else if(event.value == 0) {
				/* Release */
				
				for(key_list::reverse_iterator k2 = k->second.rbegin(); k2 != k->second.rend(); k2++) {
					input_event event2 = event;
					event2.code = *k2;
					
					uinput_write_event(event2);
				}
			}else if(event.value == 1) {
				/* Press */
				
				for(key_list::iterator k2 = k->second.begin(); k2 != k->second.end(); k2++) {
					input_event event2 = event;
					event2.code = *k2;
					
					uinput_write_event(event2);
				}
			}else if(event.value == 2) {
				/* Autorepeat */
				
				input_event event2 = event;
				event2.code = *(k->second.rbegin());
				
				uinput_write_event(event2);
			}
		}else if(!key_mappings.empty() && !sink_mode) {
			uinput_write_event(event);
		}
		
		if(event.value == 0) {
			/* Release */
			
			key_list::iterator k = std::find(pressed_keys.begin(), pressed_keys.end(), event.code);
			pressed_keys.erase(k, pressed_keys.end());
		}
	}
	
	if(uinput_fd != -1) {
		ioctl(uinput_fd, UI_DEV_DESTROY);
		close(uinput_fd);
	}
	
	if(input_fd != -1) {
		ioctl(input_fd, EVIOCGRAB, 0);
		close(input_fd);
	}
	
	if(!pidfile.empty() && unlink(pidfile.c_str()) == -1) {
		error(S("Error deleting PID file: ") + strerror(errno));
	}
	
	return 0;
}

#define CONF_ERROR(s) \
	error(filename + ":" + to_string(lnum) + ": " + s); \
	cerror = true; \
	continue;

#define REQ_ARG(x) \
	if(!(x)) { \
		CONF_ERROR("Missing argument(s)"); \
	}

#define LAST_ARG(s) \
	if((s)[strcspn(s, "\t ")] != '\0') { \
		CONF_ERROR("Too many arguments"); \
	}

#define KL_CHECK(k) \
	if((k).empty()) { \
		cerror = true; \
		continue; \
	}

static void load_config(const std::string &filename) {
	FILE *fh = fopen(filename.c_str(), "r");
	if(!fh) {
		die(S("Error opening config file: ") + strerror(errno));
	}
	
	char linebuf[1024];
	unsigned int lnum = 0;
	bool cerror = false;
	
	while(fgets(linebuf, sizeof(linebuf), fh)) {
		lnum++;
		
		linebuf[strcspn(linebuf, "\n")] = '\0';
		
		char *name = linebuf+strspn(linebuf, "\t ");
		char *value = next_arg(name), *v2;
		
		if(name[0] == '#' || name[0] == '\0') {
			continue;
		}
		
		if(streq(name, "map")) {
			REQ_ARG(*(v2 = next_arg(value)));
			LAST_ARG(v2);
			
			key_list match = parse_keys(value, filename, lnum);
			key_list out = parse_keys(v2, filename, lnum);
			
			KL_CHECK(match);
			KL_CHECK(out);
			
			std::pair<key_map::iterator,bool> i = key_mappings.insert(std::make_pair(match, out));
			
			if(!i.second) {
				CONF_ERROR("Conflicting map/drop directives for key(s)");
			}
		}else if(streq(name, "exec")) {
			REQ_ARG(*(v2 = next_arg(value)));
			
			key_list match = parse_keys(value, filename, lnum);
			KL_CHECK(match);
			
			std::pair<exec_map::iterator,bool> i = exec_mappings.insert(std::make_pair(match, v2));
			
			if(!i.second) {
				CONF_ERROR("Conflicting exec directives for key(s)");
			}
		}else if(streq(name, "drop")) {
			REQ_ARG(*value);
			LAST_ARG(value);
			
			key_list match = parse_keys(value, filename, lnum);
			KL_CHECK(match);
			
			std::pair<key_map::iterator,bool> i = key_mappings.insert(std::make_pair(match, key_list()));
			
			if(!i.second) {
				CONF_ERROR("Conflicting map/drop directives for key(s)");
			}
		}else if(streq(name, "alias")) {
			REQ_ARG(*(v2 = next_arg(value)));
			LAST_ARG(v2);
			
			if(strchr(value, '+')) {
				CONF_ERROR("Alias names cannot contain '+'");
			}
			
			key_list keys = parse_keys(v2, filename, lnum);
			KL_CHECK(keys);
			
			key_aliases.erase(value);
			key_aliases.insert(std::make_pair(value, keys));
		}else{
			CONF_ERROR("Unknown directive '" + name + "'");
		}
	}
	
	if(cerror) {
		exit(1);
	}
	
	if(ferror(fh)) {
		die(S("Error reading config file: ") + strerror(errno));
	}
	
	fclose(fh);
}

static char *next_arg(char *s) {
	s += strcspn(s, "\t ");
	
	if(*s) {
		s++[0] = '\0';
		s += strspn(s, "\t ");
	}
	
	return s;
}

static key_list parse_keys(const char *keys, const std::string &filename, unsigned int lnum) {
	key_list r;
	
	const char *f_keys = keys;
	
	while(*keys) {
		std::string key(keys, strcspn(keys, "+"));
		alias_map::iterator alias = key_aliases.find(key);
		
		if(alias != key_aliases.end()) {
			r.insert(r.end(), alias->second.begin(), alias->second.end());
		}else{
			char *endptr;
			r.push_back(strtoul(keys, &endptr, 10));
			
			if(*endptr && *endptr != '+') {
				error(filename + ":" + to_string(lnum) + ": Invalid key specified (" + f_keys + ")");
				return key_list();
			}
		}
		
		keys += key.length();
		keys += strspn(keys, "+");
	}
	
	return r;
}

static void uinput_open() {
	if((uinput_fd = open(uinput_path.c_str(), O_WRONLY)) == -1) {
		die(S("Error opening uinput device: ") + strerror(errno));
	}
	
	assert(fcntl(uinput_fd, F_SETFD, FD_CLOEXEC) == 0);
	
	assert(ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) == 0);
	assert(ioctl(uinput_fd, UI_SET_EVBIT, EV_REP) == 0);
	assert(ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) == 0);
	
	for(int key = 1; key <= KEY_MAX; key++) {
		assert(ioctl(uinput_fd, UI_SET_KEYBIT, key) == 0);
	}
	
	struct uinput_user_dev device;
	memset(&device, 0, sizeof(device));
	
	strcpy(device.name, uinput_name.c_str());
	device.id.bustype = BUS_VIRTUAL;
	
	uinput_write((char*)&device, sizeof(device));
	assert(ioctl(uinput_fd, UI_DEV_CREATE) == 0);
}

static void uinput_write(const char *buf, size_t size) {
	while(size) {
		int i = write(uinput_fd, buf, size);
		if(i == -1) {
			die(S("Error writing to uinput device: ") + strerror(errno));
		}
		
		size -= i;
		buf += i;
	}
}

static input_event read_event() {
	input_event event;
	
	int r = read(input_fd, &event, sizeof(event));
	if(r != sizeof(event)) {
		if(r == -1 && errno == EINTR) {
			/* Return an invalid EV_REL event when interrupted by
			 * a signal, causing the main loop to repeat and finish
			 * if 'running' has been set to false by a signal.
			*/
			
			event.type = EV_REL;
			return event;
		}
		
		die(S("Error reading from input device: ") + strerror(errno));
	}
	
	return event;
}

static void die(const std::string &err) {
	error(err);
	exit(1);
}

static void error(const std::string &err) {
	if(fork_daemon) {
		syslog(LOG_ERR, "%s", err.c_str());
	}else{
		std::cerr << err << std::endl;
	}
}

static void exec_command(const std::string &command) {
	static const char *env_shell = getenv("SHELL");
	const char *shell = env_shell ? env_shell : "/bin/sh";
	
	pid_t pid = fork();
	
	if(pid == -1) {
		error(S("Error forking child: ") + strerror(errno));
	}else if(pid == 0) {
		const char *argv[] = {shell, "-c", command.c_str(), NULL};
		execv(shell, (char**)argv);
		
		error(S("Error executing shell: ") + strerror(errno));
		exit(1);
	}
}

static void uinput_write_event(const input_event &event) {
	uinput_write((char*)&event, sizeof(event));
	
	if(force_sync) {
		input_event sync_event;
		memset(&sync_event, 0, sizeof(sync_event));
		
		sync_event.type = EV_SYN;
		sync_event.code = SYN_REPORT;
		
		uinput_write((char*)&sync_event, sizeof(sync_event));
	}
}
