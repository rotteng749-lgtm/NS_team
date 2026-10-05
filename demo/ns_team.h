#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <android/log.h>
#include <sys/utsname.h>
#include <sys/prctl.h>

#ifndef LOG_TAG
#define LOG_TAG "ZeroLag_ESP_BRIDGE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,LOG_TAG,__VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)
#endif

#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <mutex>
#include <string>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <malloc.h>
#include <math.h>
#include <thread>
#include <iostream>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <locale>
#include <codecvt>
#include <dlfcn.h>

// ============================================================
//   proKpm — Stealth Syscall Backend (KPM/Syscall fallback)
// ============================================================
class proKpm {
private:
    struct Ditpro_uct_kpm {
        int pid;
        uintptr_t addr;
        void *buffer;
        size_t size;
        uint32_t mode;
    };
    enum {
        __NR_syscall_ = 18,
        __FLAGS = 616,
        __READMEM = 0x400,
        __WRITEMEM = 0x200,
        __PROCPID  = 0x50,
        __CALLFUNC_1 = 0x900,
    };
public:
    int pid = -1;
    float mode = 1.f;
    bool read(uintptr_t addr, void *buffer, size_t size) {
        struct Ditpro_uct_kpm ptr;
        ptr.addr = addr; ptr.buffer = buffer;
        ptr.pid  = this->pid; ptr.size = size;
        ptr.mode = (uint32_t)this->mode;
        return (syscall(__NR_syscall_, __FLAGS, &ptr, __READMEM) == 0);
    }
    bool write(uintptr_t addr, void *buffer, size_t size) {
        struct Ditpro_uct_kpm ptr;
        ptr.addr = addr; ptr.buffer = buffer;
        ptr.pid  = this->pid; ptr.size = size;
        ptr.mode = (uint32_t)this->mode;
        return (syscall(__NR_syscall_, __FLAGS, &ptr, __WRITEMEM) == 0);
    }
    int get_pid(const char *name) {
        return syscall(__NR_syscall_, __FLAGS, name, __PROCPID);
    }
    int tscape_input(const char *name) {
        return syscall(__NR_syscall_, __FLAGS, name, __CALLFUNC_1);
    }
    uintptr_t get_module_base(int tgt_pid, const char *module_name) {
        FILE *fp; uintptr_t addr = 0;
        char filename[64], line[1024];
        snprintf(filename, sizeof(filename), "/proc/%d/maps", tgt_pid);
        fp = fopen(filename, "r");
        if (fp != NULL) {
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, module_name)) {
                    char *pch = strtok(line, "-");
                    if (pch) { addr = (uintptr_t)strtoull(pch, NULL, 16); if (addr == 0x8000) addr = 0; }
                    break;
                }
            }
            fclose(fp);
        }
        return addr;
    }
};

// ============================================================
//   TB KPM — prctl memory access structs
// ============================================================
struct mem_operation {
    pid_t    target_pid;
    uint64_t addr;
    void    *buffer;
    uint64_t size;
};
#define PRCTL_MEM_READ  0x63687501
#define PRCTL_MEM_WRITE 0x63687502

class c_driver {
public:
  proKpm syscall_backend;            // Stealth syscall fallback
  bool use_syscall_fallback = false;  // Use proKpm instead of ioctl
  bool use_tb_kpm = false;            // Use prctl TB KPM mode
  int driver_mode = 0;
  char driver_path_str[128] = "None";
  char driver_mode_name[64] = "None";
  char pid_method[64] = "Searching...";
  char touch_method[64] = "Kernel uinput (/dev/uinput slot 9)";
private:
  int has_upper = 0;
  int has_lower = 0;
  int has_symbol = 0;
  int has_digit = 0;
  int fd = -1;
  pid_t pid = 0;

  typedef struct _COPY_MEMORY {
    pid_t pid;
    uintptr_t addr;
    void *buffer;
    size_t size;
  } COPY_MEMORY, *PCOPY_MEMORY;

  typedef struct _MODULE_BASE {
    pid_t pid;
    char *name;
    uintptr_t base;
  } MODULE_BASE, *PMODULE_BASE;


  typedef struct _GET_PID {
    char name[256];
    int32_t pid;
  } GET_PID, *PGET_PID;

  enum OPERATIONS {
    OP_INIT_KEY = 0x800,
    OP_READ_MEM = 0x801,
    OP_WRITE_MEM = 0x802,
    OP_MODULE_BASE = 0x803,
    OP_GET_PID = 0x804
  };

  int symbol_file(const char *filename) {
    // Reset state on every call to avoid stale results
    has_upper = 0; has_lower = 0; has_symbol = 0; has_digit = 0;
    int length = strlen(filename);
    for (int i = 0; i < length; i++) {
      if (islower(filename[i])) {
        has_lower = 1;
      } else if (isupper(filename[i])) {
        has_upper = 1;
      } else if (ispunct(filename[i])) {
        has_symbol = 1;
      } else if (isdigit(filename[i])) {
        has_digit = 1;
      }
    }
    return has_lower && !has_upper && !has_symbol && !has_digit;
  }


  char *driver_path() {
    struct dirent *de;
    DIR *dr = opendir("/proc");
    char *device_path = NULL;

    if (dr == NULL) {
      printf("Could not open /proc directory");
      return NULL;
    }



    while ((de = readdir(dr)) != NULL) {
      if (strlen(de->d_name) != 6 || strcmp(de->d_name, "NVTSPI") == 0 || strcmp(de->d_name, "ccci_log") == 0 || strcmp(de->d_name, "aputag") == 0 || strcmp(de->d_name, "asound") == 0 || strcmp(de->d_name, "clkdbg") == 0 || strcmp(de->d_name, "crypto") == 0 || strcmp(de->d_name, "modules") == 0 || strcmp(de->d_name, "mounts") == 0 || strcmp(de->d_name, "pidmap") == 0 || strcmp(de->d_name, "phoenix") == 0 || strcmp(de->d_name, "uptime") == 0 || strcmp(de->d_name, "vmstat") == 0) {
        continue;
      }
      int is_valid = 1;
      for (int i = 0; i < 6; i++) {
        if (!isalnum(de->d_name[i])) {
          is_valid = 0;
          break;
        }
      }
        if (is_valid) {
            device_path = (char*)malloc(11 + strlen(de->d_name));
            sprintf(device_path, "/proc/%s", de->d_name);
            struct stat sb;
            if (stat(device_path, &sb) == 0 && S_ISREG(sb.st_mode)) {
                break;
            } else {
                free(device_path);
                device_path = NULL;
            }
        }
    }
    if (device_path) puts(device_path);
    closedir(dr);
    return device_path;
  }

   char *find_driver_path() {
    // Open directory
		const char *dev_path = "/dev";
		DIR *dir = opendir(dev_path);
		if (dir == NULL){
			printf("Unable to open /dev directory\n");
			return NULL;
		}

		const char *files[] = { "ns_team", "wanbai", "CheckMe", "Ckanri", "lanran", "video188" };
		struct dirent *entry;
		char *file_path = NULL;
		while ((entry = readdir(dir)) != NULL) {
			// Skip current and parent directories
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}

			size_t path_length = strlen(dev_path) + strlen(entry->d_name) + 2;
			file_path = (char *)malloc(path_length);
			snprintf(file_path, path_length, "%s/%s", dev_path, entry->d_name);
			const int n_files = (int)(sizeof(files) / sizeof(files[0]));
			for (int i = 0; i < n_files; i++) {
				if (strcmp(entry->d_name, files[i]) == 0) {
					printf("Driver file: %s\n", file_path);
					closedir(dir);
					return file_path;
				}
			}

			// Get file stat structure
			struct stat file_info;
			if (stat(file_path, &file_info) < 0) {
				free(file_path);
				file_path = NULL;
				continue;
			}


			// Skip gpio interface
			if (strstr(entry->d_name, "gpiochip") != NULL) {
						free(file_path);
						file_path = NULL;
						continue;
		   }

			// Skip gpio interface and wlan
			if (strstr(entry->d_name, "gpiochip") != NULL || 
      strcmp(entry->d_name, "wlan") == 0 ||
      strcmp(entry->d_name, "vedbinder") == 0 ||
      strcmp(entry->d_name, "vndbinder") == 0 ||
      strcmp(entry->d_name, "hwbinder") == 0 ||
      strcmp(entry->d_name, "binder") == 0 ||
      strcmp(entry->d_name, "accdet") == 0 ||
      strcmp(entry->d_name, "ptypa") == 0 ||
      strcmp(entry->d_name, "usip") == 0 ||
      strcmp(entry->d_name, "ptyp3") == 0 ||
      strcmp(entry->d_name, "ttyp0") == 0 ||
      strcmp(entry->d_name, "ptypc") == 0 ||
      strcmp(entry->d_name, "ptyp6") == 0 ||
      strcmp(entry->d_name, "console") == 0 ||
      strcmp(entry->d_name, "full") == 0 ||
      strcmp(entry->d_name, "ptype") == 0 ||
      strcmp(entry->d_name, "ttypb") == 0 ||
      
      
      strcmp(entry->d_name, "vcsa63") == 0 || 
      strcmp(entry->d_name, "ttyGS2") == 0 ||
      strcmp(entry->d_name, "ttyGS0") == 0 ||
      strcmp(entry->d_name, "ttyp7") == 0 ||
      strcmp(entry->d_name, "tty1") == 0 ||
      strcmp(entry->d_name, "tty2") == 0 ||
      strcmp(entry->d_name, "ptype") == 0 ||
      strcmp(entry->d_name, "ptyp2") == 0 ||
      strcmp(entry->d_name, "ptypd") == 0 ||
      strcmp(entry->d_name, "ptyp3") == 0 ||
      strcmp(entry->d_name, "ptyp1") == 0 ||
      strcmp(entry->d_name, "jonthv") == 0 ||  
      strcmp(entry->d_name, "JonthV") == 0 || 
      strcmp(entry->d_name, "btfmslim") == 0 ||


      /// ye wale ai ne add kiya
      strcmp(entry->d_name, "vboxdrv") == 0 || 
      strcmp(entry->d_name, "vboxnetflt") == 0 || 
      strcmp(entry->d_name, "vboxnetadp") == 0 || 
      strcmp(entry->d_name, "vboxpci") == 0 || 
      strcmp(entry->d_name, "vboxusb") == 0 || 
      strcmp(entry->d_name, "vboxguest") == 0 || 
      strcmp(entry->d_name, "vboxsf") == 0 || 
      strcmp(entry->d_name, "vboxvideo") == 0 || 
      strcmp(entry->d_name, "wlan0") == 0 || 
      strcmp(entry->d_name, "connfem") == 0) {
				free(file_path);
				file_path = NULL;
				continue;
			}

			// Check if it's a driver file
			if ((S_ISCHR(file_info.st_mode) || S_ISBLK(file_info.st_mode))
				&& strchr(entry->d_name, '_') == NULL && strchr(entry->d_name, '-') == NULL && strchr(entry->d_name, ':') == NULL) {
				// Filter standard input/output
				if (strcmp(entry->d_name, "stdin") == 0 || strcmp(entry->d_name, "stdout") == 0
					|| strcmp(entry->d_name, "stderr") == 0) {
					free(file_path);
					file_path = NULL;
					continue;
				}

				size_t file_name_length = strlen(entry->d_name);
				time_t current_time;
				time(&current_time);
				int current_year = localtime(&current_time)->tm_year + 1900;
				int file_year = localtime(&file_info.st_ctime)->tm_year + 1900;
				// Skip files before 1980
				if (file_year <= 1980) {
					free(file_path);
					file_path = NULL;
					continue;
				}

				time_t atime = file_info.st_atime;
				time_t ctime = file_info.st_ctime;
				// Check if access time and modification time match
				if (atime == ctime/* && symbol_file(entry->d_name)*/) {
					// Check if mode is S_IFREG (regular file), size is 0, gid/uid are 0 (root), and name length is 9 or less
					if ((file_info.st_mode & S_IFMT) == 8192 && file_info.st_size == 0
						&& file_info.st_gid == 0 && file_info.st_uid == 0 && file_name_length <= 9) {
						printf("Driver file: %s\n", file_path);
						closedir(dir);
						return file_path;
					}
				}
			}
			free(file_path);
			file_path = NULL;
		}
		closedir(dir);
		return NULL;
	}


public:
  // ── GT Driver Detection (fast /dev scan) ──────────────────────────────────
  char *gtqwq() {
      LOGI("c_driver::gtqwq scanning /dev for GT driver nodes");
      const char *dev_path = "/dev";
      DIR *dir = opendir(dev_path);
      if (dir == NULL) return NULL;
      const char *files[] = {"msm_sdcc", "CheckMe", "Ckanri", "lanran", "video188"};
      struct dirent *entry; char *gtfile_path = NULL;
      while ((entry = readdir(dir)) != NULL) {
          if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
          size_t path_length = strlen(dev_path) + strlen(entry->d_name) + 2;
          gtfile_path = (char *)malloc(path_length);
          if (!gtfile_path) continue;
          snprintf(gtfile_path, path_length, "%s/%s", dev_path, entry->d_name);
          for (int i = 0; i < 5; i++) {
              if (strcmp(entry->d_name, files[i]) == 0) {
                  LOGI("GT driver match: %s", gtfile_path);
                  closedir(dir); return gtfile_path;
              }
          }
          struct stat file_info;
          if (stat(gtfile_path, &file_info) < 0) { free(gtfile_path); gtfile_path = NULL; continue; }
          if (strstr(entry->d_name, "gpiochip") != NULL) { free(gtfile_path); gtfile_path = NULL; continue; }
          if ((S_ISCHR(file_info.st_mode) || S_ISBLK(file_info.st_mode)) &&
              strchr(entry->d_name, '_') == NULL && strchr(entry->d_name, '-') == NULL && strchr(entry->d_name, ':') == NULL) {
              if (strcmp(entry->d_name, "stdin") == 0 || strcmp(entry->d_name, "stdout") == 0 || strcmp(entry->d_name, "stderr") == 0) {
                  free(gtfile_path); gtfile_path = NULL; continue;
              }
              size_t file_name_length = strlen(entry->d_name);
              int file_year = localtime(&file_info.st_ctime)->tm_year + 1900;
              if (file_year <= 1980) { free(gtfile_path); gtfile_path = NULL; continue; }
              time_t atime = file_info.st_atime; time_t ctime = file_info.st_ctime;
              if (atime == ctime) {
                  if ((file_info.st_mode & S_IFMT) == 8192 && file_info.st_size == 0 &&
                      file_info.st_gid == 0 && file_info.st_uid == 0 && file_name_length <= 9) {
                      LOGI("GT dynamic char device found: %s", gtfile_path);
                      closedir(dir); return gtfile_path;
                  }
              }
          }
          free(gtfile_path); gtfile_path = NULL;
      }
      closedir(dir); return NULL;
  }

  // ── QX Dynamic Node Discovery (shell-based) ───────────────────────────────
  char *get_dev() {
      LOGI("c_driver::get_dev scanning /proc for dynamic dev nodes");
      const char* command =
          "for dir in /proc/*/; do cmdline_file=\"cmdline\"; comm_file=\"comm\"; "
          "proclj=\"$dir$cmdline_file\"; proclj2=\"$dir$comm_file\"; "
          "if [[ -f \"$proclj\" && -f \"$proclj2\" ]]; then "
          "cmdline=$(head -n 1 \"$proclj\"); comm=$(head -n 1 \"$proclj2\"); "
          "if echo \"$cmdline\" | grep -qE '^/data/[a-z]{6}$'; then sbwj=$(echo \"$comm\"); "
          "open_file=\"\"; for file in \"$dir\"/fd/*; do link=$(readlink \"$file\"); "
          "if [[ \"$link\" == \"/dev/$sbwj (deleted)\" ]]; then open_file=\"$file\"; break; fi; done; "
          "if [[ -n \"$open_file\" ]]; then nhjd=$(echo \"$open_file\"); "
          "sbid=$(ls -L -l \"$nhjd\" | sed 's/\\([^,]*\\).*/\\1/' | sed 's/.*root //'); "
          "echo \"/dev/$sbwj\"; rm -Rf \"/dev/$sbwj\"; mknod \"/dev/$sbwj\" c \"$sbid\" 0; break; "
          "fi; fi; fi; done";
      FILE* file = popen(command, "r");
      if (file == NULL) return NULL;
      char result[512];
      if (fgets(result, sizeof(result), file) == NULL) { pclose(file); return NULL; }
      pclose(file);
      int len = strlen(result);
      if (len > 0 && result[len - 1] == '\n') result[len - 1] = '\0';
      return (len > 0) ? strdup(result) : NULL;
  }

  // ── Constructor: 6-Method Driver Fallback Chain ───────────────────────────
  c_driver() {
      LOGI("c_driver constructor: starting 6-method driver detection");

      // Method 1: Standard /dev known driver node
      char *device_name = find_driver_path();
      if (device_name) {
          fd = open(device_name, O_RDWR);
          if (fd > 0) {
              driver_mode = 1;
              snprintf(driver_mode_name, sizeof(driver_mode_name), "Standard /dev Node");
              snprintf(driver_path_str, sizeof(driver_path_str), "%s", device_name);
              snprintf(touch_method, sizeof(touch_method), "Kernel uinput (/dev/uinput slot 9)");
              LOGI("[+] Mode 1 Active: /dev Standard Driver (%s)", device_name);
              free(device_name);
              return;
          }
          free(device_name);
      }

      // Method 2: /proc-based driver file
      device_name = driver_path();
      if (device_name) {
          fd = open(device_name, O_RDWR);
          if (fd > 0) {
              driver_mode = 2;
              snprintf(driver_mode_name, sizeof(driver_mode_name), "/proc Dynamic Node");
              snprintf(driver_path_str, sizeof(driver_path_str), "%s", device_name);
              snprintf(touch_method, sizeof(touch_method), "Kernel uinput (/dev/uinput slot 9)");
              LOGI("[+] Mode 2 Active: /proc Driver (%s)", device_name);
              free(device_name);
              return;
          }
          free(device_name);
      }

      // Method 3: GT Driver detection
      device_name = gtqwq();
      if (device_name) {
          fd = open(device_name, O_RDWR);
          if (fd > 0) {
              driver_mode = 3;
              snprintf(driver_mode_name, sizeof(driver_mode_name), "GT Driver");
              snprintf(driver_path_str, sizeof(driver_path_str), "%s", device_name);
              snprintf(touch_method, sizeof(touch_method), "Kernel uinput (/dev/uinput slot 9)");
              LOGI("[+] Mode 3 Active: GT Driver (%s)", device_name);
              free(device_name);
              return;
          }
          free(device_name);
      }

      // Method 4: QX Dynamic Node (shell-based mknod recovery)
      char *dev_p = get_dev();
      if (dev_p) {
          char dev_path[64];
          strncpy(dev_path, dev_p, sizeof(dev_path) - 1);
          dev_path[sizeof(dev_path) - 1] = '\0';
          free(dev_p);
          fd = open(dev_path, O_RDWR);
          if (fd > 0) {
              unlink(dev_path);
              driver_mode = 4;
              snprintf(driver_mode_name, sizeof(driver_mode_name), "QX Dynamic Node");
              snprintf(driver_path_str, sizeof(driver_path_str), "%s", dev_path);
              snprintf(touch_method, sizeof(touch_method), "Kernel uinput (/dev/uinput slot 9)");
              LOGI("[+] Mode 4 Active: QX Dynamic Driver");
              return;
          }
      }

      // Method 5: TB KPM Mode via prctl
      int test_val = 0;
      pid_t self_pid = getpid();
      struct mem_operation test_op = {self_pid, (uint64_t)&test_val, &test_val, sizeof(test_val)};
      if (prctl(PRCTL_MEM_READ, (unsigned long)&test_op, 0, 0, 0) >= 0) {
          use_tb_kpm = true;
          driver_mode = 5;
          snprintf(driver_mode_name, sizeof(driver_mode_name), "TB KPM (prctl)");
          snprintf(driver_path_str, sizeof(driver_path_str), "prctl(PRCTL_MEM_READ)");
          snprintf(touch_method, sizeof(touch_method), "Kernel uinput (/dev/uinput slot 9)");
          LOGI("[+] Mode 5 Active: TB KPM (prctl Lag Free Mode)");
          return;
      }

      // Method 6: Stealth Syscall Fallback (proKpm)
      use_syscall_fallback = true;
      int ret = syscall_backend.tscape_input("com.pubg.imobile");
      if (ret >= 0) {
          driver_mode = 6;
          snprintf(driver_mode_name, sizeof(driver_mode_name), "Stealth Syscall (proKpm)");
          snprintf(driver_path_str, sizeof(driver_path_str), "syscall(__NR_syscall_=18)");
          snprintf(touch_method, sizeof(touch_method), "Kernel Stealth Touch (tscape_input)");
          LOGI("[+] Mode 6 Active: Stealth Syscall Fallback (proKpm)");
          return;
      }

      LOGE("[FATAL] No working driver found across all 6 methods!");
      snprintf(driver_mode_name, sizeof(driver_mode_name), "No Driver Found");
      snprintf(driver_path_str, sizeof(driver_path_str), "None");
      exit(1);
  }

  ~c_driver() {
      if (fd > 0) close(fd);
  }

  void initialize(pid_t target_pid) {
      this->pid = target_pid;
      if (use_syscall_fallback) syscall_backend.pid = target_pid;
      LOGI("c_driver::initialize PID=%d", target_pid);
  }

  bool init_key(char *key) {
      if (use_tb_kpm || use_syscall_fallback) return true;
      char buf[0x100];
      strcpy(buf, key);
      return (ioctl(fd, OP_INIT_KEY, buf) == 0);
  }

  bool read(uintptr_t addr, void *buffer, size_t size) {
      if (use_tb_kpm) {
          if (this->pid <= 0) return false;
          struct mem_operation op = {this->pid, addr, buffer, size};
          return prctl(PRCTL_MEM_READ, (unsigned long)&op, 0, 0, 0) >= 0;
      }
      if (use_syscall_fallback) return syscall_backend.read(addr, buffer, size);
      COPY_MEMORY cm;
      cm.pid = this->pid; cm.addr = addr; cm.buffer = buffer; cm.size = size;
      return (ioctl(fd, OP_READ_MEM, &cm) == 0);
  }

  bool write(uintptr_t addr, void *buffer, size_t size) {
      if (use_tb_kpm) {
          if (this->pid <= 0) return false;
          struct mem_operation op = {this->pid, addr, buffer, size};
          return prctl(PRCTL_MEM_WRITE, (unsigned long)&op, 0, 0, 0) >= 0;
      }
      if (use_syscall_fallback) return syscall_backend.write(addr, buffer, size);
      COPY_MEMORY cm;
      cm.pid = this->pid; cm.addr = addr; cm.buffer = buffer; cm.size = size;
      return (ioctl(fd, OP_WRITE_MEM, &cm) == 0);
  }

  template <typename T> T read(uintptr_t addr) {
      T res{};
      this->read(addr, &res, sizeof(T));
      return res;
  }

  template <typename T> bool write(uintptr_t addr, T value) {
      return this->write(addr, &value, sizeof(T));
  }

  pid_t get_pid(const char *package_name) {
      if (!package_name || !package_name[0]) return -1;
      // 1. Try kernel driver ioctl first (0x804 OP_GET_PID)
      if (!use_tb_kpm && !use_syscall_fallback && fd > 0) {
          GET_PID gp;
          memset(&gp, 0, sizeof(gp));
          strncpy(gp.name, package_name, sizeof(gp.name) - 1);
          if (ioctl(fd, OP_GET_PID, &gp) == 0 && gp.pid > 0) {
              snprintf(pid_method, sizeof(pid_method), "Kernel Driver (OP_GET_PID 0x804)");
              LOGI("[+] Resolved PID via Kernel Driver (OP_GET_PID): %d", gp.pid);
              return gp.pid;
          }
      }
      // 2. Try stealth syscall fallback if active
      if (use_syscall_fallback) {
          int sys_pid = syscall_backend.get_pid(package_name);
          if (sys_pid > 0) {
              snprintf(pid_method, sizeof(pid_method), "Kernel Syscall (__PROCPID)");
              return sys_pid;
          }
      }
      return -1;
  }

  uintptr_t get_module_base(char *name) {
      // 1. Try ioctl driver
      if (!use_tb_kpm && !use_syscall_fallback && fd > 0) {
          MODULE_BASE mb;
          char buf[0x100]; strcpy(buf, name);
          mb.pid = this->pid; mb.name = buf;
          if (ioctl(fd, OP_MODULE_BASE, &mb) == 0 && mb.base > 0)
              return mb.base;
      }
      // 2. Fallback: parse /proc/[pid]/maps
      uintptr_t addr = 0;
      char filename[64], line[1024];
      snprintf(filename, sizeof(filename), "/proc/%d/maps", this->pid);
      FILE *fp = fopen(filename, "r");
      if (fp != NULL) {
          while (fgets(line, sizeof(line), fp)) {
              if (strstr(line, name)) {
                  char *pch = strtok(line, "-");
                  if (pch) addr = (uintptr_t)strtoull(pch, NULL, 16);
                  break;
              }
          }
          fclose(fp);
      }
      return (addr == 0x8000) ? 0 : addr;
  }

};

extern c_driver *driver; // Defined once in Main64.cpp

/*--------------------------------------------------------------------------------------------------------*/

typedef char PACKAGENAME;	// Package Name
extern pid_t pid;	// Process ID

// ── Kernel Version (fast uname syscall, no shell fork) ──────────────────────
inline float Kernel_v() {
    struct utsname buf;
    if (uname(&buf) == 0) {
        LOGI("[ZL-CORE] Kernel: %s", buf.release);
        return atof(buf.release);
    }
    return 0.0f;
}

inline char *GetVersion(char* PackageName)
{
	char command[256];
	sprintf(command, "dumpsys package %s|grep versionName|sed 's/=/\\n/g'|tail -n 1", PackageName);
	FILE* file = popen(command, "r");
	if (file == NULL) {
		return NULL;
	}
	static char result[512];
	if (fgets(result, sizeof(result), file) == NULL) {
		return NULL;
	}
	pclose(file);
	result[strlen(result)-1] = '\0';
	return result;
}

inline uint64_t GetTime()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC,&ts);
	return (ts.tv_sec*1000 + ts.tv_nsec/(1000*1000));
}

inline char *getDirectory()
{
	static char buf[128];
	int rslt = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (rslt < 0 || (rslt >= sizeof(buf) - 1))
	{
		return NULL;
	}
	buf[rslt] = '\0';
	for (int i = rslt; i >= 0; i--)
	{
		if (buf[i] == '/')
		{
			buf[i] = '\0';
			break;
		}
	}
	return buf;
}

pid_t getPid_robust(char * name);

inline int getPID(const char* PackageName)
{
    if (driver && driver->use_syscall_fallback) {
        int kpid = driver->syscall_backend.get_pid(PackageName);
        if (kpid > 0) {
            pid = kpid;
            snprintf(driver->pid_method, sizeof(driver->pid_method), "Kernel Syscall (OP_GET_PID)");
            LOGI("[ZL-CORE] Found PID for %s via syscall: %d", PackageName, pid);
            driver->initialize(pid);
            return pid;
        }
    }

    // Use the robust PID finder from support.h
    pid = getPid_robust((char*)PackageName);
    
	if (pid > 0)
	{
        LOGI("[ZL-CORE] Found PID for %s: %d", PackageName, pid);
		driver->initialize(pid);
	} else {
        if (driver) snprintf(driver->pid_method, sizeof(driver->pid_method), "Not Found");
        LOGE("[ZL-CORE] Failed to find PID for %s", PackageName);
    }
    return pid;
}

inline bool PidExamIne()
{
	char path[128];
	sprintf(path, "/proc/%d",pid);
	if (access(path,F_OK) != 0)
	{
		printf("\033[31;1m");
		puts("Failed to get process PID!");
		exit(1);
	}
	return true;
}

inline long GetModuleBaseAddr(char* module_name)
{
    long addr = 0;
    char filename[32];
    char line[1024];
    if (pid < 0)
    {
        snprintf(filename, sizeof(filename), "/proc/self/maps");
    }
    else
    {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    FILE *fp = fopen(filename, "r");
    if (fp != NULL)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, module_name))
            {
				sscanf(line,"%lx-%*lx",&addr);
                break;
            }
        }
        fclose(fp);
    }
    return addr;
}

inline long getModuleBase(char* module_name)
{
	uintptr_t base=0;
	if (Kernel_v() >= 6.0)
		base = GetModuleBaseAddr(module_name);
	else
		base = driver->get_module_base(module_name);
	return base;
}

inline long ReadValue(long addr)
{
	long he=0;
	if (addr < 0xFFFFFFFF){
		driver->read(addr, &he, 4);
	}else{
		driver->read(addr, &he, 8);
		he=he&0xFFFFFFFFFFFF;
	}
	return he;
}

inline long ReadDword(long addr)
{
	long he=0;
	driver->read(addr, &he, 4);
	return he;
}

inline float ReadFloat(long addr)
{
	float he=0;
	driver->read(addr, &he, 4);
	return he;
}

// NOTE: Caller is responsible for free()-ing the returned pointer.
inline int *ReadArray(long addr)
{
	int *he = (int *) malloc(12);
	if (!he) return nullptr;
	driver->read(addr, he, 12);
	return he;
}

inline int WriteDword(long int addr, int value)
{
	driver->write(addr, &value, 4);
	return 0;
}

inline int WriteFloat(long int addr, float value)
{
	driver->write(addr, &value, 4);
	return 0;
}
