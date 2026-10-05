/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ns_team – KPatch-Next KPM kernel driver
 *
 * Rebranded and maintained by NS_team. Derived from the KPatch-Next
 * framework (https://github.com/bmax121/KPatch-Next) and the original
 * wanbai-driver by ALEX5402; distributed under GPL-2.0-or-later.
 *
 * ioctl interface matching ns_team.h:
 *   OP_INIT_KEY    (0x800)
 *   OP_READ_MEM    (0x801)
 *   OP_WRITE_MEM   (0x802)
 *   OP_MODULE_BASE (0x803)
 *
 * Uses manual typedefs (not kfunc_def) and manual struct layouts to avoid
 * KPatch-Next stripped-header limitations.
 */

#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>     /* compat_copy_to_user, compat_strncpy_from_user */
#include <ktypes.h>
#include <linux/printk.h>

KPM_NAME("ns_team");
KPM_VERSION("3.0.3");
KPM_LICENSE("GPL-2.0-or-later");
KPM_AUTHOR("NS_team");
KPM_DESCRIPTION("NS_team universal ioctl driver (/dev/ns_team): cross-process "
                "memory read/write, module base lookup and touch event "
                "injection for Android kernels 4.4+");

#define OP_INIT_KEY     0x800
#define OP_READ_MEM     0x801
#define OP_WRITE_MEM    0x802
#define OP_MODULE_BASE  0x803
#define OP_GET_PID      0x804
#define OP_TOUCH_INIT   0x805
#define OP_TOUCH_EVENT  0x806
#define OP_CALLFUNC_1   0x900 /* Matches ns_team.h tscape_input(__CALLFUNC_1) */

/* Linux input event types and codes */
#define KPM_EV_SYN              0x00
#define KPM_EV_KEY              0x01
#define KPM_EV_REL              0x02
#define KPM_EV_ABS              0x03
#define KPM_SYN_REPORT          0
#define KPM_ABS_MT_SLOT         0x2f
#define KPM_ABS_MT_TOUCH_MAJOR  0x30
#define KPM_ABS_MT_POSITION_X   0x35
#define KPM_ABS_MT_POSITION_Y   0x36
#define KPM_ABS_MT_TRACKING_ID  0x39
#define KPM_BTN_TOUCH           0x14a

/* Matches userspace COPY_MEMORY: pid_t(4) + pad(4) + uintptr_t(8) + void*(8) + size_t(8) */
typedef struct {
    int32_t   pid;
    uint32_t  _pad;
    uint64_t  addr;
    uint64_t  buffer;
    uint64_t  size;
} COPY_MEMORY;

/* Matches userspace MODULE_BASE: pid_t(4) + pad(4) + char*(8) + uintptr_t(8) */
typedef struct {
    int32_t   pid;
    uint32_t  _pad;
    uint64_t  name;   /* userspace char* pointer */
    uint64_t  base;
} MODULE_BASE;

/* Matches userspace GET_PID */
typedef struct {
    char      name[256];
    int32_t   pid;
} GET_PID;

/* Matches userspace TOUCH_EVENT */
typedef struct {
    int32_t   type;   /* EV_ABS, EV_SYN, EV_KEY */
    int32_t   code;   /* ABS_MT_POSITION_X, ABS_MT_POSITION_Y, etc. */
    int32_t   value;  /* Coordinate, tracking ID, or press state */
    int32_t   _pad;
} TOUCH_EVENT;


/* Opaque types */
struct task_struct;
struct pid;
struct mm_struct;
struct vm_area_struct;
struct iovec;
struct path;
struct file;
struct module;
struct device;

#define GFP_KERNEL  0xcc0u
#define GFP_ATOMIC  0x200u
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define PIDTYPE_PID 0
#define MISC_DYNAMIC_MINOR 255

/* -----------------------------------------------------------------------
 * file_operations: layout varies across kernel versions.
 *
 * The offset of unlocked_ioctl changes based on whether iopoll and iterate
 * are present:
 *   4.9-5.0  : no iopoll, has iterate   → ioctl at 0x48, open at 0x60
 *   5.1-5.8  : has iopoll, has iterate   → ioctl at 0x50, open at 0x70
 *   5.9-6.12 : has iopoll, no iterate    → ioctl at 0x48, open at 0x68
 *
 * We probe at runtime using def_chr_fops + chrdev_open to find the open
 * offset, then derive ioctl = open - 0x18 (pre-4.20) or open - 0x20 (4.20+).
 * We use a raw zeroed buffer and write function pointers at the probed offsets.
 * --------------------------------------------------------------------- */
static int fops_ioctl_offset;   /* detected at init */
static int fops_compat_offset;  /* ioctl_offset + 8 */
static int fops_probed;         /* 1 if the file_operations layout probe succeeded */

/* miscdevice ABI layout for arm64 Linux 4.x–6.x */
struct kpm_miscdevice {
    int                          minor;
    const char                  *name;
    const void                  *fops;
    struct { void *next; void *prev; } list;  /* list_head */
    struct device               *parent;
    struct device               *this_device;
    const void                  *groups;       /* attribute_group**, 5.x+ */
    const char                  *nodename;
    unsigned int                 mode;
};

/* -----------------------------------------------------------------------
 * Kernel function pointer typedefs
 * --------------------------------------------------------------------- */
typedef struct pid           *(*t_find_get_pid)(int32_t);
typedef struct task_struct   *(*t_get_pid_task)(struct pid *, int);
typedef struct task_struct   *(*t_pid_task)(struct pid *, int);
typedef void                  (*t_put_pid)(struct pid *);
typedef void                  (*t_put_task_struct)(struct task_struct *);
typedef struct mm_struct     *(*t_get_task_mm)(struct task_struct *);
typedef void                  (*t_mmput)(struct mm_struct *);
typedef int                   (*t_access_process_vm)(struct task_struct *,
                                                      unsigned long addr,
                                                      void *buf, int len,
                                                      unsigned int gup_flags);
typedef int                   (*t_access_remote_vm)(struct mm_struct *,
                                                     unsigned long addr,
                                                     void *buf, int len,
                                                     unsigned int gup_flags);
typedef void                  (*t_rcu_lock)(void);
typedef int                   (*t_misc_register)(struct kpm_miscdevice *);
typedef void                  (*t_misc_deregister)(struct kpm_miscdevice *);
typedef void                 *(*t_kmalloc)(uint64_t, uint32_t);
typedef void                  (*t_kfree)(const void *);
typedef char                 *(*t_d_path)(const struct path *, char *, int);
typedef long                  (*t_copy_from_user)(void *, const void *, uint64_t);

static t_find_get_pid     kp_find_get_pid;
static t_get_pid_task     kp_get_pid_task;
static t_pid_task         kp_pid_task;
static t_put_pid          kp_put_pid;
static t_put_task_struct  kp_put_task_struct;
static t_get_task_mm      kp_get_task_mm;
static t_mmput            kp_mmput;
static t_access_process_vm kp_access_process_vm;
static t_access_remote_vm  kp_access_remote_vm;
static t_rcu_lock         kp_rcu_read_lock_fn;
static t_rcu_lock         kp_rcu_read_unlock_fn;
static t_misc_register    kp_misc_register;
static t_misc_deregister  kp_misc_deregister;
static t_kmalloc          kp_kmalloc;
static t_kfree            kp_kfree;
static t_copy_from_user   kp_raw_copy_from_user;  /* Resolved function pointer */
static t_copy_from_user   kp_raw_copy_to_user;    /* Resolved function pointer */
/* Tracked per direction: __arch_copy_* is the raw LDTR copier and requires
 * PSTATE.PAN to be cleared around the call, while _copy_* handles PAN itself.
 * A single shared flag is wrong because a kernel can expose
 * __arch_copy_from_user but only _copy_to_user (or the reverse). */
static int                kp_arch_copy_from = 0;
static int                kp_arch_copy_to   = 0;
typedef long (*t_strncpy_from_user)(char *, const char *, long);
static t_strncpy_from_user kp_strncpy_from_user;

struct input_dev;
typedef void (*t_input_event)(struct input_dev *, unsigned int, unsigned int, int);
static t_input_event kp_input_event;
static struct input_dev *p_touch_dev = NULL;

/* RCU read-side critical section helpers */
static inline void kpm_rcu_read_lock(void)
{
    if (kp_rcu_read_lock_fn)
        kp_rcu_read_lock_fn();
    else
        __asm__ __volatile__("" ::: "memory");
}

static inline void kpm_rcu_read_unlock(void)
{
    if (kp_rcu_read_unlock_fn)
        kp_rcu_read_unlock_fn();
    else
        __asm__ __volatile__("" ::: "memory");
}

/* PAN-safe copy wrappers.
 * __arch_copy_from_user uses LDTR on some kernels but NOT all (KASAN/CFI builds
 * may instrument it differently). To be safe, we disable PAN before calling
 * __arch_copy and re-enable after. For _copy_from_user, PAN is already handled. */
static inline long kp_copy_from_user(void *to, const void *from, uint64_t n)
{
    long ret;
    if (kp_arch_copy_from) {
        /* Disable PAN: set PSTATE.PAN = 0
         * 0xd500409f = MSR PAN, #0  (encoded directly for assembler compat) */
        asm volatile(".inst 0xd500409f" ::: "memory");
        ret = kp_raw_copy_from_user(to, from, n);
        /* Re-enable PAN: 0xd500419f = MSR PAN, #1 */
        asm volatile(".inst 0xd500419f" ::: "memory");
    } else {
        ret = kp_raw_copy_from_user(to, from, n);
    }
    return ret;
}

static inline long kp_copy_to_user(void *to, const void *from, uint64_t n)
{
    long ret;
    if (kp_arch_copy_to) {
        asm volatile(".inst 0xd500409f" ::: "memory"); /* MSR PAN, #0 */
        ret = kp_raw_copy_to_user(to, from, n);
        asm volatile(".inst 0xd500419f" ::: "memory"); /* MSR PAN, #1 */
    } else {
        ret = kp_raw_copy_to_user(to, from, n);
    }
    return ret;
}

typedef int (*t_snprintf)(char *buf, size_t size, const char *fmt, ...);
static t_snprintf kp_snprintf;

typedef long (*t_probe_kernel_read)(void *dst, const void *src, size_t size);
static t_probe_kernel_read kp_probe_kernel_read;

typedef long (*t_copy_from_kernel_nofault)(void *dst, const void *src, size_t size);
static t_copy_from_kernel_nofault kp_copy_from_kernel_nofault;

typedef struct file *(*t_filp_open)(const char *filename, int flags, int mode);
static t_filp_open kp_filp_open;

typedef ssize_t (*t_kernel_read)(struct file *file, void *buf, size_t count, loff_t *pos);
static t_kernel_read kp_kernel_read;     /* 4.14+ public API */
static t_kernel_read kp___kernel_read;   /* 5.4+  kernel-buffer safe */

typedef ssize_t (*t_vfs_read)(struct file *file, char *buf, size_t count, loff_t *pos);
static t_vfs_read kp_vfs_read;

typedef int (*t_filp_close)(struct file *file, void *id);
static t_filp_close kp_filp_close;

/* -----------------------------------------------------------------------
 * iovec for process_vm_rw
 * --------------------------------------------------------------------- */
/* FOLL_FORCE value changed across kernel versions:
 *   Kernels <= 6.2:  FOLL_FORCE = 0x10 (BIT(4)), FOLL_TOUCH was at BIT(1)
 *   Kernels >= 6.3:  FOLL_FORCE = 0x08 (BIT(3)), FOLL_TOUCH removed, flags shifted
 *
 * Using the wrong value is catastrophic: 0x10 on 6.3+ maps to FOLL_NOWAIT
 * which causes access_process_vm to return 0 for pages needing fault-in.
 */
#define FOLL_FORCE_OLD  0x10  /* kernels <= 6.2 */
#define FOLL_FORCE_NEW  0x08  /* kernels >= 6.3 */
#define FOLL_WRITE      0x01
static unsigned int kp_foll_force = FOLL_FORCE_OLD;  /* detected at init */

/* -----------------------------------------------------------------------
 * VMA walk offsets (arm64)
 *
 * mm_struct.mmap       = 0x00  (first field, the VMA list head)
 * vm_area_struct layout:
 *   vm_start           = 0x00
 *   vm_end             = 0x08
 *   vm_next            = 0x10
 *   vm_prev            = 0x18
 *   vm_mm              = probed at runtime (typically 0x40)
 *   vm_file            = vm_mm + 0x60 (fixed distance on arm64 4.x-5.x)
 * --------------------------------------------------------------------- */
/* mm_struct.mmap offset — probed at runtime since it varies:
 *   Custom GKI1 5.10:  mmap is at 0x0 (first field)
 *   Stock GKI2 5.10:   mmap is elsewhere (CONFIG_SPECULATIVE_PAGE_FAULT shifts it)
 *   6.1+:              uses maple tree (mm_mt), layout differs again
 */
static int mm_mmap_offset = -1;    /* probed once at first use */
static int vma_vm_next_off = 0x10; /* vm_next offset, verified at probe time */

/* Safe memory read helper to prevent panics when reading unpinned memory */
static inline int kp_safe_read(void *dst, const void *src, size_t size)
{
    if (kp_copy_from_kernel_nofault)
        return kp_copy_from_kernel_nofault(dst, src, size);
    if (kp_probe_kernel_read)
        return kp_probe_kernel_read(dst, src, size);
    return -1;
}

/* Cached vm_file offset, probed once at first use */
static int vma_vm_file_off = 0;

/* Probe mm_struct.mmap offset by scanning for a VMA pointer.
 * A valid VMA has vm_start in userspace range and vm_end > vm_start.
 * We also verify the candidate VMA has a vm_mm back-pointer == mm. */
static int probe_mm_mmap_offset(struct mm_struct *mm)
{
    uint64_t mm_val = (uint64_t)mm;
    /* Scan mm_struct for the first VMA pointer */
    for (int off = 0; off <= 0x200; off += 8) {
        uint64_t candidate = 0;
        if (kp_safe_read(&candidate, (char *)mm + off, sizeof(candidate)))
            continue;
        if (!candidate || candidate < 0xffff000000000000ULL)
            continue;  /* must be a kernel pointer (arm64 kernel space) */
        /* Verify: read vm_start (offset 0) — should be a userspace addr */
        uint64_t vm_start = 0;
        if (kp_safe_read(&vm_start, (void *)candidate, sizeof(vm_start)))
            continue;
        if (vm_start == 0 || vm_start >= 0xffff000000000000ULL)
            continue;  /* vm_start must be userspace */
        /* Verify: read vm_end (offset 8) — should be > vm_start */
        uint64_t vm_end = 0;
        if (kp_safe_read(&vm_end, (void *)(candidate + 8), sizeof(vm_end)))
            continue;
        if (vm_end <= vm_start)
            continue;
        /* Verify: find vm_mm back-pointer in this VMA */
        for (int vmm_off = 0x20; vmm_off <= 0x80; vmm_off += 8) {
            uint64_t vmm = 0;
            if (kp_safe_read(&vmm, (void *)(candidate + vmm_off), sizeof(vmm)))
                continue;
            if (vmm == mm_val) {
                printk(KERN_INFO "ns_team: probed mm->mmap offset=0x%x (vma=%llx vm_start=%llx)\n",
                       off, candidate, vm_start);
                return off;
            }
        }
    }
    printk(KERN_ERR "ns_team: failed to probe mm->mmap offset, trying default 0x0\n");
    return 0;  /* fallback */
}

/* Probe vm_file offset by finding vm_mm back-pointer in the first VMA.
 * Every VMA has vma->vm_mm == mm.  We scan the VMA struct for a word
 * that equals mm, that gives us the vm_mm offset.  Then vm_file is at
 * a fixed distance from vm_mm:
 *   vm_page_prot(8) + vm_flags(8) + shared(32) + anon_vma_chain(16) +
 *   anon_vma(8) + vm_ops(8) + vm_pgoff(8) = 0x60
 * So vm_file = vm_mm_offset + 0x60 */
static int probe_vm_file_offset(struct vm_area_struct *vma, struct mm_struct *mm)
{
    uint64_t mm_val = (uint64_t)mm;
    /* Scan offsets 0x20..0x80 looking for vm_mm == mm */
    for (int off = 0x20; off <= 0x80; off += 8) {
        uint64_t val = 0;
        if (kp_safe_read(&val, (char *)vma + off, sizeof(val)))
            continue;
        if (val == mm_val) {
            int file_off = off + 0x60;
            return file_off;
        }
    }
    /* Fallback: try common offsets */
    return 0xa0;
}

/* Probe vm_next offset by checking candidate next pointers in a VMA.
 * vm_next is typically at 0x10 but can vary. The next VMA should also
 * have vm_mm == mm. */
static int probe_vm_next_offset(struct vm_area_struct *vma, struct mm_struct *mm)
{
    uint64_t mm_val = (uint64_t)mm;
    /* Try common offsets: 0x08, 0x10, 0x18 */
    int candidates[] = { 0x10, 0x08, 0x18 };
    for (int i = 0; i < 3; i++) {
        int off = candidates[i];
        uint64_t next = 0;
        if (kp_safe_read(&next, (char *)vma + off, sizeof(next)))
            continue;
        if (!next) continue;  /* end of list is OK for offset 0x10 */
        if (next < 0xffff000000000000ULL) continue;  /* must be kernel ptr */
        /* Verify next VMA has valid vm_start */
        uint64_t ns = 0;
        if (kp_safe_read(&ns, (void *)next, sizeof(ns)))
            continue;
        if (ns == 0 || ns >= 0xffff000000000000ULL)
            continue;
        /* Verify next VMA's vm_mm matches */
        for (int vmm_off = 0x20; vmm_off <= 0x80; vmm_off += 8) {
            uint64_t vmm = 0;
            if (kp_safe_read(&vmm, (void *)(next + vmm_off), sizeof(vmm)))
                continue;
            if (vmm == mm_val) {
                printk(KERN_INFO "ns_team: probed vm_next offset=0x%x\n", off);
                return off;
            }
        }
    }
    printk(KERN_INFO "ns_team: vm_next probe failed, using default 0x10\n");
    return 0x10;  /* default */
}

/* Inline char helpers (bare-metal gcc emits libcall otherwise) */
static char kpm_tolower_char(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}
static char *kpm_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (kpm_tolower_char(*haystack) == kpm_tolower_char(*needle)) {
            const char *h, *n;
            for (h = haystack, n = needle; *h && *n; h++, n++) {
                if (kpm_tolower_char(*h) != kpm_tolower_char(*n)) break;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}
static char *kpm_strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}
static size_t kpm_strlen(const char *s) {
    const char *sc = s;
    for (; *sc != '\0'; ++sc) /* nothing */;
    return sc - s;
}

/* Helper to check for error pointers */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline long IS_ERR(const void *ptr) {
    return IS_ERR_VALUE((unsigned long)ptr);
}

/* -----------------------------------------------------------------------
 * Safe task / mm lookup without refcount leaks.
 *
 * Instead of get_pid_task() (which increments t->usage refcount requiring
 * an inlined put_task_struct destructor), we use rcu_read_lock() + pid_task()
 * to borrow the task_struct reference just long enough to acquire mm_struct
 * via get_task_mm(). get_task_mm() safely increments mm->mm_users, which
 * is then released with kp_mmput(). This eliminates task refcount leaks.
 * --------------------------------------------------------------------- */
static struct mm_struct *get_mm_by_pid(int32_t pid)
{
    if (!kp_find_get_pid || !kp_put_pid || !kp_get_task_mm)
        return NULL;

    struct pid *p = kp_find_get_pid(pid);
    if (!p)
        return NULL;

    struct mm_struct *mm = NULL;
    if (kp_pid_task) {
        kpm_rcu_read_lock();
        struct task_struct *t = kp_pid_task(p, PIDTYPE_PID);
        if (t) {
            mm = kp_get_task_mm(t);
        }
        kpm_rcu_read_unlock();
    } else if (kp_get_pid_task) {
        /* Fallback if pid_task symbol is missing */
        struct task_struct *t = kp_get_pid_task(p, PIDTYPE_PID);
        if (t) {
            mm = kp_get_task_mm(t);
        }
    }
    kp_put_pid(p);
    return mm;
}

/* -----------------------------------------------------------------------
 * Cross-process memory helper using access_remote_vm / access_process_vm.
 * Works with kernel buffers directly and uses mm_struct to avoid task leaks.
 * --------------------------------------------------------------------- */
static long xmem(int32_t pid, uint64_t addr, void *buf, uint64_t sz, int wr)
{
    struct mm_struct *mm = get_mm_by_pid(pid);
    if (!mm) return -3;

    /* FOLL_FORCE is required: without it access_remote_vm refuses to fault in
     * pages and cannot write to read-only/COW mappings. Its numeric value
     * changed in 6.3, so we use the value detected at load time rather than a
     * hard-coded constant. */
    unsigned int flags = kp_foll_force | (wr ? FOLL_WRITE : 0);
    uint64_t done = 0;

    if (kp_access_remote_vm) {
        /* Loop: access_remote_vm can return a short count when the range
         * crosses a hole, so keep going until the buffer is filled. */
        while (done < sz) {
            uint64_t remaining = sz - done;
            int chunk = (remaining > 0x7fffffffULL) ? 0x7fffffff : (int)remaining;
            int n = kp_access_remote_vm(mm, (unsigned long)(addr + done),
                                        (char *)buf + done, chunk, flags);
            if (n <= 0)
                break;
            done += (uint64_t)n;
        }
    } else if (kp_access_process_vm && kp_get_pid_task) {
        /* Fallback: access_process_vm needs a task. Take a proper reference
         * with get_pid_task() and drop it afterwards instead of borrowing a
         * task pointer across the (sleepable) call. */
        struct pid *p = kp_find_get_pid(pid);
        if (!p) { kp_mmput(mm); return -3; }
        struct task_struct *t = kp_get_pid_task(p, PIDTYPE_PID);
        kp_put_pid(p);
        if (!t) { kp_mmput(mm); return -3; }

        while (done < sz) {
            uint64_t remaining = sz - done;
            int chunk = (remaining > 0x7fffffffULL) ? 0x7fffffff : (int)remaining;
            int n = kp_access_process_vm(t, (unsigned long)(addr + done),
                                         (char *)buf + done, chunk, flags);
            if (n <= 0)
                break;
            done += (uint64_t)n;
        }
        if (kp_put_task_struct)
            kp_put_task_struct(t);
    }

    kp_mmput(mm);
    return (done == sz) ? 0 : -5;
}

/* -----------------------------------------------------------------------
 * Module base via direct VMA walk.
 *
 * Reading /proc/<pid>/maps from kernel context fails on GKI 5.x+ kernels
 * because seq_file uses copy_to_user() internally, which breaks with
 * kernel buffers after set_fs() removal.
 *
 * Instead, we walk the VMA linked list directly:
 *   get_task_mm() → mm->mmap → iterate vm_next → check vm_file →
 *   read dentry name → match against requested module name.
 *
 * The vm_file offset is probed at runtime by finding the vm_mm
 * back-pointer in the first VMA (vma->vm_mm == mm), then adding 0x60.
 * --------------------------------------------------------------------- */

/* dentry.d_name is a struct qstr at offset 0x20 in dentry.
 * qstr layout: { union { u64 hash_len; struct { u32 hash; u32 len; }; }; const char *name; }
 * So dentry->d_name.name is at dentry + 0x20 + 0x08 = dentry + 0x28 */
#define DENTRY_D_NAME_NAME_OFF  0x28

/* file->f_path is at offset 0x10 in struct file on arm64,
 * f_path is { struct vfsmount *mnt; struct dentry *dentry; }
 * so file->f_path.dentry = file + 0x10 + 0x08 = file + 0x18 */
#define FILE_F_PATH_DENTRY_OFF  0x18

static const char *dentry_name_from_file(void *filp, char *name_buf, size_t buf_size)
{
    if (!filp) return NULL;
    void *dentry = NULL;
    if (kp_safe_read(&dentry, (char *)filp + FILE_F_PATH_DENTRY_OFF, sizeof(dentry)) || !dentry)
        return NULL;
    
    char *name_ptr = NULL;
    if (kp_safe_read(&name_ptr, (char *)dentry + DENTRY_D_NAME_NAME_OFF, sizeof(name_ptr)) || !name_ptr)
        return NULL;

    /* Safely read the string up to buf_size */
    for (size_t i = 0; i < buf_size - 1; i++) {
        char c;
        if (kp_safe_read(&c, name_ptr + i, 1) || c == '\0') {
            name_buf[i] = '\0';
            break;
        }
        name_buf[i] = c;
    }
    name_buf[buf_size - 1] = '\0';
    return name_buf;
}

/* Simple string comparison for matching just the basename */
static int kpm_str_ends_with(const char *str, const char *suffix)
{
    size_t slen = kpm_strlen(str);
    size_t sufflen = kpm_strlen(suffix);
    if (sufflen > slen) return 0;
    const char *p = str + slen - sufflen;
    while (*p && *suffix) {
        if (kpm_tolower_char(*p) != kpm_tolower_char(*suffix)) return 0;
        p++; suffix++;
    }
    return !*suffix;
}

static uint64_t module_base_vma(int32_t pid, const char *name)
{
    uint64_t base = 0;
    struct mm_struct *mm = get_mm_by_pid(pid);
    if (!mm) {
        return 0;
    }

    /* We MUST use safe reads (nofault) because we don't hold mmap_lock. */
    if (!kp_probe_kernel_read && !kp_copy_from_kernel_nofault) {
        printk(KERN_ERR "ns_team: VMA walk missing safe read functions!\n");
        kp_mmput(mm);
        return 0;
    }

    /* Probe mm->mmap offset on first call */
    if (mm_mmap_offset < 0) {
        mm_mmap_offset = probe_mm_mmap_offset(mm);
    }

    /* Safely read mm->mmap */
    struct vm_area_struct *vma = NULL;
    if (kp_safe_read(&vma, (char *)mm + mm_mmap_offset, sizeof(vma)) || !vma) {
        printk(KERN_ERR "ns_team: mm->mmap read failed at offset 0x%x\n", mm_mmap_offset);
        kp_mmput(mm);
        return 0;
    }

    /* Probe vm_file and vm_next offsets on first call */
    if (!vma_vm_file_off) {
        vma_vm_file_off = probe_vm_file_offset(vma, mm);
        vma_vm_next_off = probe_vm_next_offset(vma, mm);
        printk(KERN_INFO "ns_team: VMA offsets: mm_mmap=0x%x vm_file=0x%x vm_next=0x%x\n",
               mm_mmap_offset, vma_vm_file_off, vma_vm_next_off);
    }

    int count = 0;
    char fname_buf[128];

    while (vma && count < 100000) {
        count++;
        /* Read vm_file at probed offset */
        void *filp = NULL;
        kp_safe_read(&filp, (char *)vma + vma_vm_file_off, sizeof(filp));

#define DEBUG_VMA_WALK 0
        if (filp && !IS_ERR((void *)filp)) {
            const char *fname = dentry_name_from_file(filp, fname_buf, sizeof(fname_buf));
#if DEBUG_VMA_WALK
            if (count <= 10) {
                printk(KERN_INFO "ns_team: VMA[%d] start=%llx filp=%px fname='%s'\n",
                       count, *(uint64_t *)&vma, filp, fname ? fname : "(null)");
            }
#endif
            if (fname && kpm_str_ends_with(fname, name)) {
                uint64_t vs = 0;
                kp_safe_read(&vs, vma, sizeof(vs));
                base = vs;
                break;
            }
        }
        /* Read next VMA pointer at probed vm_next offset */
        void *next = NULL;
        if (kp_safe_read(&next, (char *)vma + vma_vm_next_off, sizeof(next)))
            break;
        vma = (struct vm_area_struct *)next;
    }

    kp_mmput(mm);
    return base;
}

/* Try VMA walk first, fall back to /proc/maps file reading */
static uint64_t module_base(int32_t pid, const char *name)
{
    uint64_t base = 0;

    /* Method 1: direct VMA walk (works on all kernels 4.9-6.x) */
    if (kp_get_task_mm && kp_mmput) {
        base = module_base_vma(pid, name);
        if (base) {
            return base;
        }
    }

    /* Method 2: /proc/maps reading (works on 4.x kernels with set_fs) */
    if (!kp_snprintf || !kp_filp_open || !kp_filp_close) {
        printk(KERN_ERR "ns_team: module_base: no file I/O funcs available\n");
        return 0;
    }

    char path[64];
    kp_snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    struct file *f = kp_filp_open(path, 0, 0);
    if (IS_ERR(f)) {
        printk(KERN_ERR "ns_team: failed to open %s err=%ld\n", path, (long)(f));
        return 0;
    }

    char *buf = kp_kmalloc(PAGE_SIZE + 1, GFP_KERNEL);
    if (!buf) { kp_filp_close(f, 0); return 0; }

    loff_t pos = 0;
    while (1) {
        ssize_t bytes = -1;
        if (bytes <= 0 && kp___kernel_read)
            bytes = kp___kernel_read(f, buf, PAGE_SIZE, &pos);
        if (bytes <= 0 && kp_kernel_read)
            bytes = kp_kernel_read(f, buf, PAGE_SIZE, &pos);
        if (bytes <= 0 && kp_vfs_read)
            bytes = kp_vfs_read(f, buf, PAGE_SIZE, &pos);
        if (bytes <= 0) {
            break;
        }
        buf[bytes] = '\0';

        char *line = buf;
        while (line && *line) {
            char *nl = kpm_strchr(line, '\n');
            if (nl) *nl = '\0';
            if (kpm_strcasestr(line, name)) {
                uint64_t val = 0;
                char *p2 = line;
                while (*p2) {
                    char c = *p2++;
                    if (c >= '0' && c <= '9') val = (val << 4) | (c - '0');
                    else if (c >= 'a' && c <= 'f') val = (val << 4) | (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') val = (val << 4) | (c - 'A' + 10);
                    else break;
                }
                base = val;
                break;
            }
            if (!nl) {
                if (bytes == PAGE_SIZE) {
                    int len = kpm_strlen(line);
                    if (len < PAGE_SIZE) pos -= len;
                }
                break;
            }
            line = nl + 1;
        }
        if (base) break;
    }

    kp_kfree(buf);
    kp_filp_close(f, 0);
    return base;
}

/* -----------------------------------------------------------------------
 * Dynamic task_struct.comm offset probing and PID resolution
 * --------------------------------------------------------------------- */
static int task_comm_offset = 0;

static void probe_task_comm_offset(void)
{
    char *init_task = (char *)kallsyms_lookup_name("init_task");
    if (!init_task) return;

    /* Scan init_task for "swapper" string (always present in init_task.comm) */
    for (int off = 0x100; off < 0x1400; off++) {
        if (init_task[off] == 's' && init_task[off+1] == 'w' &&
            init_task[off+2] == 'a' && init_task[off+3] == 'p' &&
            init_task[off+4] == 'p' && init_task[off+5] == 'e' &&
            init_task[off+6] == 'r') {
            task_comm_offset = off;
            printk(KERN_INFO "ns_team: probed task_struct.comm offset=0x%x\n", off);
            return;
        }
    }
    printk(KERN_WARNING "ns_team: task_struct.comm offset probe failed\n");
}

/* Read a task's comm (16 bytes) into a NUL-terminated buffer via safe reads. */
static int read_task_comm(struct task_struct *t, char *buf)
{
    if (task_comm_offset <= 0)
        return 0;
    for (int i = 0; i < 16; i++)
        kp_safe_read(&buf[i], (char *)t + task_comm_offset + i, 1);
    buf[16] = '\0';
    return buf[0] != '\0';
}

/* Match a task comm against the requested name, including the 15-char
 * truncation case (the kernel cuts TASK_COMM_LEN-1). */
static int comm_matches(const char *comm, const char *target, size_t target_len)
{
    if (kpm_strcasestr(comm, target) || kpm_strcasestr(target, comm))
        return 1;
    if (target_len >= 15) {
        for (int i = 0; i < 15 && comm[i]; i++) {
            if (kpm_tolower_char(comm[i]) != kpm_tolower_char(target[i]))
                return 0;
        }
        return 1;
    }
    return 0;
}

static void cache_name(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    for (; i < dst_size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Resolve a PID from a process name.
 *
 * Two passes: the common case (the process comm matches) uses only the cheap
 * per-PID comm check; the expensive per-PID VMA walk, needed when the caller
 * passes a package name that only appears in a mapped file path, is deferred
 * to a second pass so it no longer runs on every lookup. A one-entry cache
 * short-circuits repeated lookups while still verifying the cached PID is
 * alive and still matches. */
static int32_t find_pid_by_name(const char *target_name)
{
    if (!target_name || !target_name[0] || !kp_find_get_pid || !kp_put_pid)
        return -1;

    static char    cached_name[64];
    static int32_t cached_pid = -1;

    int max_pid = 65535;
    int *p_max = (int *)kallsyms_lookup_name("pid_max");
    if (p_max && *p_max > 0 && *p_max <= 4194304) {
        max_pid = *p_max;
        if (max_pid > 65535) max_pid = 65535;
    }

    size_t target_len = kpm_strlen(target_name);

    /* Fast path: validate and reuse the cached PID. */
    if (cached_pid > 0 && kpm_strlen(cached_name) == target_len) {
        int same = 1;
        for (size_t i = 0; i < target_len; i++) {
            if (cached_name[i] != target_name[i]) { same = 0; break; }
        }
        if (same) {
            struct pid *p = kp_find_get_pid(cached_pid);
            if (p) {
                int still = 0;
                if (task_comm_offset > 0 && kp_pid_task) {
                    kpm_rcu_read_lock();
                    struct task_struct *t = kp_pid_task(p, PIDTYPE_PID);
                    if (t) {
                        char comm[17];
                        if (read_task_comm(t, comm))
                            still = comm_matches(comm, target_name, target_len);
                    }
                    kpm_rcu_read_unlock();
                }
                kp_put_pid(p);
                if (still)
                    return cached_pid;
            }
            cached_pid = -1; /* stale; fall through to a full scan */
        }
    }

    /* Pass 1: cheap comm-only match. */
    if (task_comm_offset > 0 && kp_pid_task) {
        for (int pid = 1; pid <= max_pid; pid++) {
            struct pid *p = kp_find_get_pid(pid);
            if (!p) continue;

            int matched = 0;
            kpm_rcu_read_lock();
            struct task_struct *t = kp_pid_task(p, PIDTYPE_PID);
            if (t) {
                char comm[17];
                if (read_task_comm(t, comm))
                    matched = comm_matches(comm, target_name, target_len);
            }
            kpm_rcu_read_unlock();
            kp_put_pid(p);

            if (matched) {
                cache_name(cached_name, sizeof(cached_name), target_name);
                cached_pid = pid;
                return pid;
            }
        }
    }

    /* Pass 2: expensive VMA / package-name match, only on cache + comm miss. */
    if (target_len > 3) {
        for (int pid = 1; pid <= max_pid; pid++) {
            if (module_base_vma(pid, target_name) > 0) {
                cache_name(cached_name, sizeof(cached_name), target_name);
                cached_pid = pid;
                return pid;
            }
        }
    }

    return -1;
}

/* -----------------------------------------------------------------------
 * Dynamic touchscreen device discovery and input event injection
 * --------------------------------------------------------------------- */

/* Touchscreen name heuristic.
 * A bare "ts" substring is deliberately NOT used — it matches far too many
 * unrelated input devices ("sensors", "rotary", ...) and would send injected
 * events to the wrong device. Only distinctive vendor/"touch" tokens match. */
static int is_touchscreen_name(const char *name)
{
    static const char *const hints[] = {
        "touch", "synaptics", "goodix", "sec_touch", "focaltech",
        "novatek", "himax", "elan", "atmel", "stm_ts", "fts",
        "_ts", "ts_", "digitizer",
    };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (kpm_strcasestr(name, hints[i]))
            return 1;
    }
    return 0;
}

static struct input_dev *find_touchscreen_dev(const char *preferred_name)
{
    if (p_touch_dev && (!preferred_name || !preferred_name[0]))
        return p_touch_dev;

    struct list_head *dev_list = (struct list_head *)kallsyms_lookup_name("input_dev_list");
    if (!dev_list || !dev_list->next || dev_list->next == dev_list) {
        printk(KERN_WARNING "ns_team: input_dev_list empty or not found\n");
        return NULL;
    }

    struct input_dev *found = NULL;
    struct input_dev *fallback_touch = NULL;

    /* Scan candidate list_node offsets in struct input_dev (0x20..0x800) */
    for (int node_off = 0x20; node_off <= 0x800; node_off += 8) {
        struct list_head *curr = dev_list->next;
        int valid_list = 0;
        int count = 0;

        while (curr && curr != dev_list && count < 64) {
            count++;
            char *candidate_dev = (char *)curr - node_off;
            char *name_ptr = NULL;
            if (kp_safe_read(&name_ptr, candidate_dev, sizeof(name_ptr)) || !name_ptr)
                break;
            if ((uint64_t)name_ptr < 0xffff000000000000ULL)
                break;

            char dev_name[64];
            for (int i = 0; i < 63; i++) {
                char c = 0;
                if (kp_safe_read(&c, name_ptr + i, 1) || c == '\0') {
                    dev_name[i] = '\0';
                    break;
                }
                dev_name[i] = c;
            }
            dev_name[63] = '\0';

            if (dev_name[0] >= 0x20 && dev_name[0] <= 0x7e) {
                valid_list++;
                /* Check if this device matches touchscreen heuristics */
                if (preferred_name && preferred_name[0] && kpm_strcasestr(dev_name, preferred_name)) {
                    found = (struct input_dev *)candidate_dev;
                    printk(KERN_INFO "ns_team: found requested input device: '%s' at %px\n", dev_name, found);
                    break;
                }
                if (is_touchscreen_name(dev_name)) {
                    fallback_touch = (struct input_dev *)candidate_dev;
                    printk(KERN_INFO "ns_team: detected touchscreen device: '%s' at %px\n", dev_name, fallback_touch);
                }
            }

            struct list_head *next_node = NULL;
            if (kp_safe_read(&next_node, curr, sizeof(next_node)) || !next_node)
                break;
            curr = next_node;
        }

        if (found) break;
        if (valid_list >= 2 && fallback_touch) break;
    }

    if (found) {
        p_touch_dev = found;
        return found;
    }
    if (fallback_touch) {
        p_touch_dev = fallback_touch;
        return fallback_touch;
    }

    return NULL;
}

static int inject_touch_event(int type, int code, int value)
{
    if (!kp_input_event)
        return -22;

    struct input_dev *dev = p_touch_dev;
    if (!dev)
        dev = find_touchscreen_dev(NULL);

    if (!dev) {
        printk(KERN_ERR "ns_team: inject_touch_event: no touchscreen device found\n");
        return -19; /* -ENODEV */
    }

    kp_input_event(dev, (unsigned int)type, (unsigned int)code, value);
    return 0;
}

/* -----------------------------------------------------------------------
 * ioctl handler
 * --------------------------------------------------------------------- */
static long ns_team_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case OP_INIT_KEY: {
        char key[0x100];
        long ret = kp_copy_from_user(key, (void *)arg, sizeof(key));
        if (ret) {
            printk(KERN_ERR "ns_team: OP_INIT_KEY copy_from_user failed: %ld\n", ret);
            return -14;
        }
        return 0;
    }
    case OP_READ_MEM: {
        COPY_MEMORY cm;
        long cfu = kp_copy_from_user(&cm, (void *)arg, sizeof(cm));
        if (cfu) return -14;
        if (!cm.size || cm.size > 0x1000000ULL) return -22;
        void *tmp = kp_kmalloc(cm.size, GFP_KERNEL);
        if (!tmp) return -12;
        long r = xmem(cm.pid, cm.addr, tmp, cm.size, 0);
        if (r) {
            kp_kfree(tmp);
            return r;
        }
        if (kp_copy_to_user((void *)cm.buffer, tmp, cm.size)) {
            kp_kfree(tmp);
            return -14;
        }
        kp_kfree(tmp);
        return 0;
    }
    case OP_WRITE_MEM: {
        COPY_MEMORY cm;
        long cfu = kp_copy_from_user(&cm, (void *)arg, sizeof(cm));
        if (cfu) return -14;
        if (!cm.size || cm.size > 0x1000000ULL) return -22;
        void *tmp = kp_kmalloc(cm.size, GFP_KERNEL);
        if (!tmp) return -12;
        long cfu2 = kp_copy_from_user(tmp, (void *)cm.buffer, cm.size);
        if (cfu2) {
            kp_kfree(tmp);
            return -14;
        }
        long r = xmem(cm.pid, cm.addr, tmp, cm.size, 1);

        kp_kfree(tmp);
        return r;
    }
    case OP_MODULE_BASE: {
        MODULE_BASE mb = {0, 0, 0, 0};
        long cfu_ret = kp_copy_from_user(&mb, (void *)arg, sizeof(mb));
        if (cfu_ret) return -14;
        char nb[256];
        /* Try copy_from_user first (works on hardened GKI2 stock kernels
         * where strncpy_from_user fails with -EFAULT in hooked ioctl context).
         * Fall back to strncpy_from_user if copy_from_user fails. */
        long sfu_ret = -1;
        if (mb.name) {
            long cfu2 = kp_copy_from_user(nb, (char *)(uintptr_t)mb.name, sizeof(nb) - 1);
            if (cfu2 == 0) {
                nb[sizeof(nb) - 1] = '\0';
                /* Ensure null termination within the buffer */
                sfu_ret = kpm_strlen(nb);
            } else {
                /* Fallback to strncpy_from_user */
                sfu_ret = kp_strncpy_from_user(nb, (char *)(uintptr_t)mb.name, sizeof(nb));
            }
        }
        if (sfu_ret <= 0) return -14;
        nb[255] = '\0';
        mb.base = module_base(mb.pid, nb);
        long ctu_ret = kp_copy_to_user((void *)arg, &mb, sizeof(mb));
        if (ctu_ret) return -14;
        return 0;
    }
    case OP_GET_PID: {
        char raw_buf[288];
        long cfu = kp_copy_from_user(raw_buf, (void *)arg, sizeof(raw_buf));
        if (cfu) {
            printk(KERN_ERR "ns_team: OP_GET_PID copy_from_user failed: %ld\n", cfu);
            return -14;
        }

        char target_name[256];
        target_name[0] = '\0';

        /* Check if raw_buf[0] starts with printable ASCII chars */
        if (raw_buf[0] >= 0x20 && raw_buf[0] <= 0x7e) {
            for (int i = 0; i < 255; i++) {
                char c = raw_buf[i];
                if (c < 0x20 || c > 0x7e) { target_name[i] = '\0'; break; }
                target_name[i] = c;
            }
            target_name[255] = '\0';
        }

        /* Check if offset 8 is a userspace pointer */
        if (target_name[0] == '\0') {
            uint64_t ptr = *(uint64_t *)(raw_buf + 8);
            if (ptr > 0x1000 && ptr < 0x800000000000ULL) {
                if (kp_copy_from_user(target_name, (void *)(uintptr_t)ptr, sizeof(target_name) - 1) == 0) {
                    target_name[255] = '\0';
                }
            }
        }

        /* Check if offset 0 is a userspace pointer */
        if (target_name[0] == '\0') {
            uint64_t ptr = *(uint64_t *)raw_buf;
            if (ptr > 0x1000 && ptr < 0x800000000000ULL) {
                if (kp_copy_from_user(target_name, (void *)(uintptr_t)ptr, sizeof(target_name) - 1) == 0) {
                    target_name[255] = '\0';
                }
            }
        }

        /* Check if offset 8 starts with printable ASCII */
        if (target_name[0] == '\0' && raw_buf[8] >= 0x20 && raw_buf[8] <= 0x7e) {
            for (int i = 0; i < 255; i++) {
                char c = raw_buf[8 + i];
                if (c < 0x20 || c > 0x7e) { target_name[i] = '\0'; break; }
                target_name[i] = c;
            }
            target_name[255] = '\0';
        }

        if (target_name[0] == '\0') {
            printk(KERN_ERR "ns_team: OP_GET_PID: unable to parse target process name from userspace buffer\n");
            return -22;
        }

        printk(KERN_INFO "ns_team: OP_GET_PID looking up process: '%s'\n", target_name);

        int32_t found_pid = find_pid_by_name(target_name);
        if (found_pid <= 0) {
            printk(KERN_WARNING "ns_team: OP_GET_PID process '%s' NOT FOUND\n", target_name);
            return -3;
        }

        printk(KERN_INFO "ns_team: OP_GET_PID process '%s' FOUND => pid=%d\n", target_name, found_pid);

        /* Write PID back into common response offsets (0, 8, 256) */
        *(int32_t *)raw_buf = found_pid;
        *(int32_t *)(raw_buf + 8) = found_pid;
        *(int32_t *)(raw_buf + 256) = found_pid;

        long ctu = kp_copy_to_user((void *)arg, raw_buf, sizeof(raw_buf));
        if (ctu) {
            printk(KERN_ERR "ns_team: OP_GET_PID copy_to_user failed: %ld\n", ctu);
            return -14;
        }

        return 0;
    }
    case OP_TOUCH_INIT:
    case OP_CALLFUNC_1: {
        /* Handshake & auto-locate touchscreen device (supports tscape_input 0x900) */
        char dev_hint[64];
        dev_hint[0] = '\0';
        if (arg) {
            kp_copy_from_user(dev_hint, (void *)arg, sizeof(dev_hint) - 1);
            dev_hint[sizeof(dev_hint) - 1] = '\0';
        }
        find_touchscreen_dev(dev_hint[0] ? dev_hint : NULL);
        return 0;
    }
    case OP_TOUCH_EVENT: {
        TOUCH_EVENT te;
        long cfu = kp_copy_from_user(&te, (void *)arg, sizeof(te));
        if (cfu) return -14;

        int ret = inject_touch_event(te.type, te.code, te.value);
        return ret;
    }
    default:
        return -25;
    }
}

/* -----------------------------------------------------------------------
 * Device node pointers (dynamically allocated to avoid ABI overflows
 * and Read-Only .data permission panics on some kernels)
 * --------------------------------------------------------------------- */
static void      *p_ns_team_fops;   /* raw buffer, not a typed struct */
static struct kpm_miscdevice      *p_ns_team_dev;

/* -----------------------------------------------------------------------
 * KPM lifecycle
 * --------------------------------------------------------------------- */
/* Locate unlocked_ioctl without depending on local function symbol names.
 *
 * Android 16+/CFI kernels append a per-build hash to local symbols, e.g.
 * "chrdev_open$4083aaa7…" and "pipe_write$6c38da87…", so chrdev_open and
 * fifo_open can never be resolved by name and the open-based probe above
 * fails. Global symbols keep their plain names, so instead scan well-known
 * fops structs for a globally-named ioctl handler that occupies the
 * unlocked_ioctl slot:
 *   ptmx_fops.unlocked_ioctl  = tty_ioctl
 *   def_blk_fops.unlocked_ioctl = blkdev_ioctl
 * Returns the byte offset of the slot, or -1 if it could not be determined. */
static int probe_ioctl_offset_from_known_fops(void)
{
    static const char *const fops_names[] = { "ptmx_fops", "def_blk_fops" };
    /* Kernels built with CFI and canonical jump tables store the
     * "<fn>.cfi_jt" trampoline address in every function-pointer field, so
     * try both the plain symbol and its .cfi_jt alias. */
    static const char *const ioctl_names[][2] = {
        { "tty_ioctl",    "tty_ioctl.cfi_jt"    },
        { "blkdev_ioctl", "blkdev_ioctl.cfi_jt" },
    };

    for (int f = 0; f < 2; f++) {
        uint64_t *fops = (uint64_t *)kallsyms_lookup_name(fops_names[f]);
        if (!fops)
            continue;
        for (int c = 0; c < 2; c++) {
            uint64_t fn = kallsyms_lookup_name(ioctl_names[f][c]);
            if (!fn)
                continue;
            for (int slot = 2; slot < 40; slot++) {   /* skip owner/llseek */
                if (fops[slot] == fn)
                    return slot * 8;
            }
        }
    }
    return -1;
}

static long ns_team_init(const char *args, const char *event, void *__user rsv)
{
    if (!kallsyms_lookup_name) {
        printk(KERN_ERR "ns_team: kallsyms_lookup_name unavailable\n");
        return -2;
    }

    int missing = 0;

#define RESOLVE(var, sym) \
    var = (typeof(var))kallsyms_lookup_name(sym); \
    if (!var) { printk(KERN_ERR "ns_team: missing: " sym "\n"); missing++; }

    RESOLVE(kp_find_get_pid,    "find_get_pid");
    kp_pid_task = (t_pid_task)kallsyms_lookup_name("pid_task");
    kp_access_remote_vm = (t_access_remote_vm)kallsyms_lookup_name("access_remote_vm");
    if (!kp_access_remote_vm)
        kp_access_remote_vm = (t_access_remote_vm)kallsyms_lookup_name("__access_remote_vm");

    kp_rcu_read_lock_fn = (t_rcu_lock)kallsyms_lookup_name("__rcu_read_lock");
    if (!kp_rcu_read_lock_fn)
        kp_rcu_read_lock_fn = (t_rcu_lock)kallsyms_lookup_name("rcu_read_lock");

    kp_rcu_read_unlock_fn = (t_rcu_lock)kallsyms_lookup_name("__rcu_read_unlock");
    if (!kp_rcu_read_unlock_fn)
        kp_rcu_read_unlock_fn = (t_rcu_lock)kallsyms_lookup_name("rcu_read_unlock");

    RESOLVE(kp_get_pid_task,    "get_pid_task");
    RESOLVE(kp_put_pid,         "put_pid");
    kp_put_task_struct = (t_put_task_struct)kallsyms_lookup_name("__put_task_struct");
    RESOLVE(kp_get_task_mm,     "get_task_mm");
    RESOLVE(kp_mmput,           "mmput");
    RESOLVE(kp_access_process_vm, "access_process_vm");
    RESOLVE(kp_misc_register,   "misc_register");
    RESOLVE(kp_misc_deregister, "misc_deregister");

    kp_input_event = (t_input_event)kallsyms_lookup_name("input_event");
    if (!kp_input_event)
        kp_input_event = (t_input_event)kallsyms_lookup_name("input_handle_event");
    /* __kmalloc was renamed to __kmalloc_noprof in kernel 6.10+ (alloc_tag profiling) */
    kp_kmalloc = (t_kmalloc)kallsyms_lookup_name("__kmalloc");
    if (!kp_kmalloc) kp_kmalloc = (t_kmalloc)kallsyms_lookup_name("__kmalloc_noprof");
    if (!kp_kmalloc) { printk(KERN_ERR "ns_team: missing: __kmalloc / __kmalloc_noprof\n"); missing++; }

    RESOLVE(kp_kfree,           "kfree");

    kp_snprintf = (t_snprintf)kallsyms_lookup_name("snprintf");
    kp_filp_open = (t_filp_open)kallsyms_lookup_name("filp_open");
    kp_kernel_read = (t_kernel_read)kallsyms_lookup_name("kernel_read");
    kp___kernel_read = (t_kernel_read)kallsyms_lookup_name("__kernel_read");
    kp_vfs_read = (t_vfs_read)kallsyms_lookup_name("vfs_read");
    kp_filp_close = (t_filp_close)kallsyms_lookup_name("filp_close");
    kp_probe_kernel_read = (t_probe_kernel_read)kallsyms_lookup_name("probe_kernel_read");
    kp_copy_from_kernel_nofault = (t_copy_from_kernel_nofault)kallsyms_lookup_name("copy_from_kernel_nofault");

    /* copy_from_user: prefer __arch_copy_from_user (raw LDTR copier).
     * On GKI2 stock 5.10, _copy_from_user is broken (copies only first
     * few bytes but returns 0). __arch_copy uses LDTR, which requires PAN to
     * be cleared around the call — record that so the wrapper does it. */
    kp_raw_copy_from_user = (t_copy_from_user)kallsyms_lookup_name("__arch_copy_from_user");
    if (kp_raw_copy_from_user) {
        kp_arch_copy_from = 1;
        printk(KERN_INFO "ns_team: using __arch_copy_from_user (PAN toggled)\n");
    } else {
        kp_raw_copy_from_user = (t_copy_from_user)kallsyms_lookup_name("_copy_from_user");
        if (kp_raw_copy_from_user)
            printk(KERN_INFO "ns_team: using _copy_from_user (fallback)\n");
    }

    kp_raw_copy_to_user = (t_copy_from_user)kallsyms_lookup_name("__arch_copy_to_user");
    if (kp_raw_copy_to_user) {
        kp_arch_copy_to = 1;
        printk(KERN_INFO "ns_team: using __arch_copy_to_user (PAN toggled)\n");
    } else {
        kp_raw_copy_to_user = (t_copy_from_user)kallsyms_lookup_name("_copy_to_user");
    }

    kp_strncpy_from_user = (t_strncpy_from_user)kallsyms_lookup_name("strncpy_from_user");

    if (!kp_raw_copy_from_user || !kp_raw_copy_to_user) {
        printk(KERN_ERR "ns_team: missing: [__arch]_copy_from_user\n");
        missing++;
    }

    if (!kp_strncpy_from_user) {
        printk(KERN_ERR "ns_team: missing: strncpy_from_user\n");
        missing++;
    }

    if (missing > 0) {
        printk(KERN_ERR "ns_team: %d symbols missing, aborting\n", missing);
        return -2;
    }

    /* Probe task_struct.comm offset from init_task */
    probe_task_comm_offset();

    printk(KERN_INFO "ns_team: symbols OK\n");

    /* ------------------------------------------------------------------
     * Detect correct FOLL_FORCE value.
     *
     * In kernel 6.3, FOLL_TOUCH (0x02) was removed and the GUP flags
     * were renumbered:
     *   <= 6.2:  WRITE=0x01  TOUCH=0x02  GET=0x04  DUMP=0x08  FORCE=0x10
     *   >= 6.3:  WRITE=0x01            GET=0x02  DUMP=0x04  FORCE=0x08
     *
     * Passing old 0x10 on 6.3+ actually sets FOLL_NOWAIT, causing
     * access_process_vm to return 0 for any page needing fault-in.
     *
     * Detection: __kmalloc_noprof implies 6.10+ (definitely FORCE=0x08).
     * For 6.3-6.9, we probe for folio_alloc_noprof (introduced in 6.3).
     * If neither, assume old kernel with FORCE=0x10.
     * ------------------------------------------------------------------ */
    if (kallsyms_lookup_name("__kmalloc_noprof") ||
        kallsyms_lookup_name("folio_alloc_noprof")) {
        kp_foll_force = FOLL_FORCE_NEW;  /* 0x08, kernel >= 6.3 */
        printk(KERN_INFO "ns_team: detected kernel >= 6.3, FOLL_FORCE=0x%x\n",
               kp_foll_force);
    } else {
        kp_foll_force = FOLL_FORCE_OLD;  /* 0x10, kernel <= 6.2 */
        printk(KERN_INFO "ns_team: detected kernel <= 6.2, FOLL_FORCE=0x%x\n",
               kp_foll_force);
    }

    /* ------------------------------------------------------------------
     * Probe the unlocked_ioctl offset in file_operations.
     *
     * Strategy: look up def_chr_fops (the default char-device fops present
     * in all kernels).  Its 'open' field == chrdev_open.  Find chrdev_open
     * in the struct to determine the 'open' offset, then derive ioctl:
     *
     *   open at 0x60 → ioctl = 0x48  (4.9-4.19, no mmap_supported_flags)
     *   open at 0x68/0x70 → ioctl = 0x48  (4.20-5.0  or  5.9-6.12)
     *   open at 0x70 → ioctl = 0x50  (5.1-5.8)
     * ------------------------------------------------------------------ */
    {
        uint64_t *def_fops = (uint64_t *)kallsyms_lookup_name("def_chr_fops");
        uint64_t  chrdev_open_fn = kallsyms_lookup_name("chrdev_open");
        int open_off = -1;

        if (def_fops && chrdev_open_fn) {
            for (int i = 2; i < 30; i++) {  /* skip owner/llseek */
                if (def_fops[i] == chrdev_open_fn) {
                    open_off = i * 8;
                    break;
                }
            }
        }

        /* Fallback: try pipefifo_fops + pipe_write (available on most kernels) */
        if (open_off < 0) {
            uint64_t *pfops = (uint64_t *)kallsyms_lookup_name("pipefifo_fops");
            uint64_t  pw_fn = kallsyms_lookup_name("pipe_write");
            if (pfops && pw_fn) {
                /* pipe_write is at the 'write' field = offset 0x18 on all versions.
                 * write_iter is at 0x28. Check which slot has pipe_write. */
                for (int i = 2; i < 10; i++) {
                    if (pfops[i] == pw_fn) {
                        /* write is always at 0x18 (slot 3). If we found it at
                         * slot 3 => pre-field-insertion baseline matches.
                         * The open field can be derived: on all versions,
                         * open = write + (open_offset - 0x18).
                         * But easier: scan for pipe_fopen or pipefifo_open. */
                        uint64_t popen = kallsyms_lookup_name("fifo_open");
                        if (!popen) popen = kallsyms_lookup_name("pipefifo_open");
                        if (popen) {
                            for (int j = 5; j < 30; j++) {
                                if (pfops[j] == popen) {
                                    open_off = j * 8;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }

        if (open_off > 0) {
            /* open at 0x60 → no mmap_supported_flags → delta 0x18
             * open at 0x68/0x70 → has mmap_supported_flags → delta 0x20 */
            fops_ioctl_offset = (open_off <= 0x60)
                                ? open_off - 0x18
                                : open_off - 0x20;
            fops_probed = 1;
        } else {
            int io_off = probe_ioctl_offset_from_known_fops();
            if (io_off > 0) {
                fops_ioctl_offset = io_off;
                fops_probed = 1;
                printk(KERN_INFO "ns_team: probed unlocked_ioctl=0x%x via known fops\n",
                       io_off);
            } else {
                fops_ioctl_offset = 0x48; /* safe fallback for 4.14 */
                printk(KERN_WARNING "ns_team: fops probe failed, defaulting ioctl=0x48\n");
            }
        }
        fops_compat_offset = fops_ioctl_offset + 8;
    }

    /* Allocate 4096 bytes each to guarantee ABI overflow margin */
    p_ns_team_fops = kp_kmalloc(4096, GFP_KERNEL);
    p_ns_team_dev  = kp_kmalloc(4096, GFP_KERNEL);

    if (!p_ns_team_fops || !p_ns_team_dev) {
        printk(KERN_ERR "ns_team: kmalloc failed\n");
        if (p_ns_team_fops) kp_kfree(p_ns_team_fops);
        if (p_ns_team_dev)  kp_kfree(p_ns_team_dev);
        return -12; /* ENOMEM */
    }

    /* Zero out unconditionally (memset equivalent) */
    char *p1 = (char *)p_ns_team_fops;
    char *p2 = (char *)p_ns_team_dev;
    for (int i = 0; i < 4096; i++) { p1[i] = 0; p2[i] = 0; }

    /* Install the handler only in the probed unlocked_ioctl slot plus its
     * compat_ioctl sibling at +0x08. The previous code wrote the handler into
     * every slot from 0x38 to 0x58, which clobbered read/write/iterate_shared/
     * mmap — any read()/write() on the device then landed in our handler with
     * mismatched arguments. Only if the layout probe failed do we fall back to
     * the known arm64 candidate offsets. */
    if (fops_probed) {
        *(uint64_t *)(p1 + fops_ioctl_offset)  = (uint64_t)ns_team_ioctl;
        *(uint64_t *)(p1 + fops_compat_offset) = (uint64_t)ns_team_ioctl;
    } else {
        int fallback_offsets[] = { 0x40, 0x48, 0x50, 0x58 };
        int n_fallback = sizeof(fallback_offsets) / sizeof(fallback_offsets[0]);
        for (int i = 0; i < n_fallback; i++)
            *(uint64_t *)(p1 + fallback_offsets[i]) = (uint64_t)ns_team_ioctl;
    }
    printk(KERN_INFO "ns_team: ioctl handler installed at 0x%x (compat 0x%x, probed=%d)\n",
           fops_ioctl_offset, fops_compat_offset, fops_probed);

    /* Set up miscdevice */
    p_ns_team_dev->minor = MISC_DYNAMIC_MINOR;
    p_ns_team_dev->name  = "ns_team";
    p_ns_team_dev->fops  = p_ns_team_fops;
    p_ns_team_dev->mode  = 0600;

    int ret = kp_misc_register(p_ns_team_dev);
    if (ret) {
        printk(KERN_ERR "ns_team: misc_register failed: %d\n", ret);
        kp_kfree(p_ns_team_fops);
        kp_kfree(p_ns_team_dev);
        p_ns_team_fops = 0;
        p_ns_team_dev = 0;
    }
    return ret;
}

static long ns_team_exit(void *__user rsv)
{
    if (kp_misc_deregister && p_ns_team_dev) {
        kp_misc_deregister(p_ns_team_dev);
    }
    if (p_ns_team_dev)  { kp_kfree(p_ns_team_dev);  p_ns_team_dev = 0; }
    if (p_ns_team_fops) { kp_kfree(p_ns_team_fops); p_ns_team_fops = 0; }

    return 0;
}

static long ns_team_ctl0(const char *args, char *__user out, int outlen) { return 0; }
static long ns_team_ctl1(void *a1, void *a2, void *a3) { return 0; }

KPM_INIT(ns_team_init);
KPM_EXIT(ns_team_exit);
KPM_CTL0(ns_team_ctl0);
KPM_CTL1(ns_team_ctl1);
