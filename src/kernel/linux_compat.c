#include "linux_compat.h"
#include "linux_x11.h"
#include "display.h"
#include "filesystem.h"
#include "gdt.h"
#include "io.h"
#include "keyboard.h"
#include "multiboot1.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

#include <stddef.h>

#define PAGE 4096ULL
#define USER_LIMIT 0x0000800000000000ULL
#define MAP_BASE 0x0000006000000000ULL
#define STACK_TOP 0x00007ffffff00000ULL
#define STACK_SIZE (256 * 1024ULL)
#define FD_COUNT 64
#define PATH_SIZE 256
#define ARG_SIZE 1024
#define MAX_ARGS 48
#define MAX_MEMORY (192 * 1024 * 1024ULL)
#define AT_FDCWD (-100)

enum { EPERM=1, ENOENT=2, EINTR=4, EIO=5, EBADF=9, EAGAIN=11,
    ENOMEM=12, EACCES=13, EFAULT=14, EBUSY=16, EEXIST=17, ENODEV=19, ENOTDIR=20,
    EISDIR=21, EINVAL=22, EMFILE=24, ENOTTY=25, EFBIG=27, ENOSPC=28,
    ESPIPE=29, EROFS=30, EPIPE=32, EDEADLK=35, ENOSYS=38, ENOTSOCK=88,
    ENOPROTOOPT=92, EOPNOTSUPP=95, EAFNOSUPPORT=97, ENOTCONN=107,
    ETIMEDOUT=110, ECONNREFUSED=111 };
enum { FILE_REG=1, FILE_DIR, FILE_TTY, FILE_NULL, FILE_ZERO, FILE_RANDOM,
    FILE_SOCKET };

typedef struct {
    const unsigned char* data;
    uint64_t size;
    uint32_t mode;
    unsigned kind;
    char path[PATH_SIZE];
} guest_file_t;
typedef struct {
    guest_file_t file;
    uint64_t offset;
    unsigned flags;
    unsigned references;
    bool connected;
} open_file_t;
typedef struct { int handle; unsigned flags; } descriptor_t;
typedef struct { uint64_t handler, flags, restorer, mask; } signal_action_t;
typedef struct { int64_t sec, nsec; } linux_time_t;
typedef struct { uint64_t base, length; } linux_iovec_t;
typedef struct {
    uint64_t name;
    uint32_t name_length, pad0;
    uint64_t vectors, vector_count, control, control_length;
    uint32_t flags, pad1;
} linux_message_t;
typedef struct {
    uint64_t device, inode, links;
    uint32_t mode, uid, gid, pad;
    uint64_t rdevice;
    int64_t size, block_size, blocks;
    int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    int64_t reserved[3];
} linux_stat_t;
typedef struct {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf_program_t;
typedef struct {
    uint64_t entry, phdr, phnum, base, end;
    char interpreter[PATH_SIZE];
} loaded_elf_t;

uintptr_t linux_kernel_cr3;
uintptr_t linux_user_cr3;
uintptr_t linux_kernel_stack;
uintptr_t linux_user_stack;
static const unsigned char* root_archive;
static size_t root_archive_size;
static bool session_active;
static pid_t session_pid;
static char launch_path[PATH_SIZE], launch_arguments[ARG_SIZE];
static char current_directory[PATH_SIZE] = "/";
static open_file_t handles[FD_COUNT];
static descriptor_t descriptors[FD_COUNT];
static signal_action_t actions[64];
static uint64_t signal_mask;
static uint64_t memory_bytes, next_mapping, program_break, initial_break;
static uint64_t clear_tid;
static bool cpu_has_rdrand;
static uint64_t entropy_state;
extern char __kernel_start[], __kernel_end[];
extern void linux_syscall_entry(void);
extern void linux_enter(uintptr_t entry, uintptr_t stack, uintptr_t root)
    __attribute__((noreturn));

static void zero(void* data, size_t length) {
    unsigned char* p = data;
    for (size_t n = 0; n < length; n++) p[n] = 0;
}
static void copy(void* to, const void* from, size_t length) {
    unsigned char* d = to;
    const unsigned char* s = from;
    for (size_t n = 0; n < length; n++) d[n] = s[n];
}
static size_t text_length(const char* text) {
    size_t n = 0;
    while (text[n]) n++;
    return n;
}
static bool text_equal(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static bool text_copy(char* to, const char* from, size_t capacity) {
    size_t n = 0;
    while (from[n] && n + 1 < capacity) { to[n] = from[n]; n++; }
    to[n] = 0;
    return !from[n];
}
static uint64_t page_up(uint64_t value) { return (value + PAGE - 1) & ~(PAGE - 1); }
static uint64_t page_down(uint64_t value) { return value & ~(PAGE - 1); }
static uint64_t ms_ticks(uint64_t ms) { return ms / 10 + (ms % 10 != 0); }
static void log_text(const char* text) {
    serial_write_string(text);
    display_puts(text);
}
static void write_msr(uint32_t number, uint64_t value) {
    asm volatile("wrmsr" : : "c"(number), "a"((uint32_t)value),
        "d"((uint32_t)(value >> 32)) : "memory");
}
static uint64_t read_msr(uint32_t number) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(number));
    return ((uint64_t)high << 32) | low;
}
static bool entropy_word(uint64_t* output) {
    if (!cpu_has_rdrand) return false;
    for (unsigned n = 0; n < 10; n++) {
        unsigned char valid;
        asm volatile("rdrand %0; setc %1" : "=r"(*output), "=qm"(valid));
        if (valid) return true;
    }
    return false;
}
static bool fill_entropy(unsigned char* output, size_t size) {
    while (size) {
        uint64_t word;
        if (!entropy_word(&word)) return false;
        size_t chunk = size > sizeof(word) ? sizeof(word) : size;
        copy(output, &word, chunk);
        output += chunk;
        size -= chunk;
    }
    return true;
}
static uint64_t random_word(void) {
    uint64_t word;
    if (entropy_word(&word)) return word;
    /* Boot-stack diversification only; getrandom never reports this as entropy. */
    entropy_state ^= entropy_state << 13;
    entropy_state ^= entropy_state >> 7;
    entropy_state ^= entropy_state << 17;
    return entropy_state;
}
static bool valid_range(uint64_t address, uint64_t length) {
    if (address >= USER_LIMIT || length > USER_LIMIT - address) return false;
    return !length || (address >= 65536 &&
        !(address < (uintptr_t)__kernel_end && address + length > (uintptr_t)__kernel_start));
}
static bool user_valid(uint64_t address, uint64_t length, bool writing) {
    if (!valid_range(address, length)) return false;
    while (length) {
        uintptr_t physical;
        bool writable;
        if (!paging_user_page(linux_user_cr3, address, &physical, &writable) ||
            (writing && !writable)) return false;
        uint64_t chunk = PAGE - (address & (PAGE - 1));
        if (chunk > length) chunk = length;
        address += chunk;
        length -= chunk;
    }
    return true;
}
static bool user_copy(void* kernel, uint64_t user, size_t length, bool to_user) {
    if (!user_valid(user, length, to_user)) return false;
    unsigned char* bytes = kernel;
    while (length) {
        uintptr_t physical;
        if (!paging_user_page(linux_user_cr3, user, &physical, NULL)) return false;
        size_t chunk = PAGE - (user & (PAGE - 1));
        if (chunk > length) chunk = length;
        if (to_user) copy((void*)physical, bytes, chunk);
        else copy(bytes, (void*)physical, chunk);
        user += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return true;
}
static bool user_string(uint64_t user, char* output, size_t size) {
    for (size_t n = 0; n < size; n++) {
        if (!user_copy(&output[n], user + n, 1, false)) return false;
        if (!output[n]) return true;
    }
    return false;
}
static bool normalize_path(const char* path, char* output) {
    char input[PATH_SIZE * 2];
    size_t count = 0;
    if (*path != '/') {
        size_t cwd = text_length(current_directory);
        if (cwd + 1 >= sizeof(input)) return false;
        copy(input, current_directory, cwd);
        count = cwd;
        input[count++] = '/';
    }
    if (text_length(path) >= sizeof(input) - count) return false;
    text_copy(input + count, path, sizeof(input) - count);
    size_t position = 0, out = 1;
    output[0] = '/';
    while (input[position]) {
        while (input[position] == '/') position++;
        size_t start = position;
        while (input[position] && input[position] != '/') position++;
        size_t length = position - start;
        if (!length || (length == 1 && input[start] == '.')) continue;
        if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (out > 1) { out--; while (out > 1 && output[out - 1] != '/') out--; }
            continue;
        }
        if (out > 1 && output[out - 1] != '/') output[out++] = '/';
        if (out + length >= PATH_SIZE) return false;
        copy(output + out, input + start, length);
        out += length;
    }
    if (out > 1 && output[out - 1] == '/') out--;
    output[out] = 0;
    return true;
}
static uint64_t tar_number(const unsigned char* text, size_t length) {
    uint64_t number = 0;
    for (size_t n = 0; n < length; n++) {
        if (text[n] >= '0' && text[n] <= '7') number = number * 8 + text[n] - '0';
        else if (text[n] && text[n] != ' ') break;
    }
    return number;
}
static bool tar_name(const unsigned char* header, char* name) {
    char raw[PATH_SIZE];
    size_t out = 0;
    if (header[345]) {
        for (unsigned n = 0; n < 155 && header[345 + n]; n++) {
            if (out + 1 >= sizeof(raw)) return false;
            raw[out++] = header[345 + n];
        }
        raw[out++] = '/';
    }
    for (unsigned n = 0; n < 100 && header[n]; n++) {
        if (out + 1 >= sizeof(raw)) return false;
        raw[out++] = header[n];
    }
    raw[out] = 0;
    char absolute[PATH_SIZE];
    if (out + 2 > sizeof(absolute)) return false;
    absolute[0] = '/';
    text_copy(absolute + 1, raw, sizeof(absolute) - 1);
    return normalize_path(absolute, name);
}
static int find_file_depth(const char* path, guest_file_t* file, unsigned depth) {
    if (depth > 12) return -40;
    char normalized[PATH_SIZE];
    if (!normalize_path(path, normalized)) return -36;
    zero(file, sizeof(*file));
    text_copy(file->path, normalized, sizeof(file->path));
    if (text_equal(normalized, "/")) { file->kind = FILE_DIR; file->mode = 0040755; return 0; }
    if (text_equal(normalized, "/dev/null")) file->kind = FILE_NULL;
    else if (text_equal(normalized, "/dev/zero")) file->kind = FILE_ZERO;
    else if (text_equal(normalized, "/dev/urandom") || text_equal(normalized, "/dev/random")) file->kind = FILE_RANDOM;
    else if (text_equal(normalized, "/dev/tty") || text_equal(normalized, "/dev/console")) file->kind = FILE_TTY;
    if (file->kind) { file->mode = 0020666; return 0; }
    for (size_t offset = 0; root_archive && offset + 512 <= root_archive_size;) {
        const unsigned char* header = root_archive + offset;
        if (!header[0]) break;
        uint64_t size = tar_number(header + 124, 12);
        if (size > root_archive_size - offset - 512) return -EIO;
        char name[PATH_SIZE];
        if (tar_name(header, name) && text_equal(name, normalized)) {
            if (header[156] == '2' || header[156] == '1') {
                char target[PATH_SIZE], joined[PATH_SIZE];
                unsigned n = 0;
                while (n < 100 && header[157 + n]) { target[n] = header[157 + n]; n++; }
                target[n] = 0;
                if (target[0] == '/') return find_file_depth(target, file, depth + 1);
                if (header[156] == '1') {
                    joined[0] = '/';
                    if (!text_copy(joined + 1, target, sizeof(joined) - 1)) return -36;
                } else {
                    text_copy(joined, normalized, sizeof(joined));
                    size_t end = text_length(joined);
                    while (end && joined[end - 1] != '/') end--;
                    if (!text_copy(joined + end, target, sizeof(joined) - end)) return -36;
                }
                return find_file_depth(joined, file, depth + 1);
            }
            file->data = header + 512;
            file->size = size;
            file->kind = header[156] == '5' ? FILE_DIR : FILE_REG;
            file->mode = (file->kind == FILE_DIR ? 0040000 : 0100000) |
                (uint32_t)(tar_number(header + 100, 8) & 0777);
            return 0;
        }
        offset += 512 + ((size + 511) & ~511ULL);
    }
    filesystem_info_t info;
    if (filesystem_stat(normalized, &info) == FS_OK) {
        file->kind = info.type == FS_TYPE_DIR ? FILE_DIR : FILE_REG;
        file->size = info.size;
        file->mode = file->kind == FILE_DIR ? 0040755 : 0100644;
        return 0;
    }
    return -ENOENT;
}
static int find_file(const char* path, guest_file_t* file) { return find_file_depth(path, file, 0); }
static long file_read(const guest_file_t* file, uint64_t offset, void* data, size_t length) {
    if (file->kind == FILE_DIR) return -EISDIR;
    if (offset >= file->size) return 0;
    if (length > file->size - offset) length = file->size - offset;
    if (file->data) { copy(data, file->data + offset, length); return (long)length; }
    if (offset > UINT32_MAX || length > UINT32_MAX) return -EFBIG;
    uint32_t actual = 0;
    if (filesystem_read_at(file->path, offset, data, length, &actual) != FS_OK) return -EIO;
    return actual;
}
static open_file_t* fd_get(long fd) {
    if (fd < 0 || fd >= FD_COUNT || descriptors[fd].handle < 0) return NULL;
    return &handles[descriptors[fd].handle];
}
static long fd_allocate(const guest_file_t* file, unsigned flags, unsigned minimum) {
    unsigned handle, fd;
    for (handle = 0; handle < FD_COUNT && handles[handle].references; handle++);
    for (fd = minimum; fd < FD_COUNT && descriptors[fd].handle >= 0; fd++);
    if (handle == FD_COUNT || fd == FD_COUNT) return -EMFILE;
    zero(&handles[handle], sizeof(handles[handle]));
    handles[handle].file = *file;
    handles[handle].flags = flags;
    handles[handle].references = 1;
    descriptors[fd].handle = handle;
    descriptors[fd].flags = (flags & 02000000) ? 1 : 0;
    return fd;
}
static long fd_close(long fd) {
    open_file_t* file = fd_get(fd);
    if (!file) return -EBADF;
    file->references--;
    if (!file->references && file->file.kind == FILE_SOCKET && file->connected) linux_x11_reset();
    descriptors[fd].handle = -1;
    descriptors[fd].flags = 0;
    return 0;
}
static long open_path(long dirfd, uint64_t path_address, unsigned flags) {
    char path[PATH_SIZE];
    if (!user_string(path_address, path, sizeof(path))) return -EFAULT;
    if (path[0] != '/' && dirfd != AT_FDCWD) return -ENOTDIR;
    guest_file_t file;
    int result = find_file(path, &file);
    if (result < 0) return result;
    if ((flags & 0200000) && file.kind != FILE_DIR) return -ENOTDIR;
    if ((flags & 3) && (file.kind == FILE_REG || file.kind == FILE_DIR)) return -EROFS;
    if ((flags & 01000) && file.kind == FILE_REG) return -EROFS;
    return fd_allocate(&file, flags, 0);
}
static void fill_stat(const guest_file_t* file, linux_stat_t* stat) {
    zero(stat, sizeof(*stat));
    stat->device = 1;
    uint64_t inode = 5381;
    for (const char* p = file->path; *p; p++) inode = inode * 33 + (unsigned char)*p;
    stat->inode = inode;
    stat->links = file->kind == FILE_DIR ? 2 : 1;
    stat->mode = file->mode;
    stat->size = file->size;
    stat->block_size = PAGE;
    stat->blocks = (file->size + 511) / 512;
    if (file->kind == FILE_TTY) stat->rdevice = 0x0501;
}
static long stat_path(long dirfd, uint64_t path_address, uint64_t output, unsigned flags) {
    char path[PATH_SIZE];
    if (!user_string(path_address, path, sizeof(path))) return -EFAULT;
    guest_file_t file;
    if (!path[0] && (flags & 0x1000)) {
        open_file_t* opened = fd_get(dirfd);
        if (!opened) return -EBADF;
        file = opened->file;
    } else {
        if (path[0] != '/' && dirfd != AT_FDCWD) return -ENOTDIR;
        int result = find_file(path, &file);
        if (result < 0) return result;
    }
    linux_stat_t stat;
    fill_stat(&file, &stat);
    return user_copy(&stat, output, sizeof(stat), true) ? 0 : -EFAULT;
}
static bool mapping_free(uint64_t start, uint64_t length) {
    if (!valid_range(start, length)) return false;
    for (uint64_t address = start; address < start + length; address += PAGE)
        if (paging_user_mapping(linux_user_cr3, address, NULL, NULL)) return false;
    return true;
}
static long unmap_range(uint64_t start, uint64_t length) {
    if ((start & (PAGE - 1)) || !length || length > MAX_MEMORY ||
        !valid_range(start, page_up(length))) return -EINVAL;
    uint64_t end = start + page_up(length);
    for (uint64_t address = start; address < end; address += PAGE) {
        uintptr_t physical;
        if (paging_unmap_user_page(linux_user_cr3, address, &physical)) {
            pmm_free((void*)physical, 1);
            memory_bytes -= PAGE;
        }
    }
    return 0;
}
static long map_range(uint64_t start, uint64_t length, unsigned protection,
    unsigned flags, long fd, uint64_t offset) {
    if (!length || length > MAX_MEMORY || (offset & (PAGE - 1)) ||
        offset > INT64_MAX || (protection & ~7U) ||
        ((flags & 3) != 1 && (flags & 3) != 2)) return -EINVAL;
    length = page_up(length);
    open_file_t* opened = NULL;
    if (!(flags & 0x20)) {
        opened = fd_get(fd);
        if (!opened) return -EBADF;
        if (opened->file.kind != FILE_REG && opened->file.kind != FILE_ZERO) return -ENODEV;
        if ((flags & 1) && (protection & 2)) return -EACCES;
    }
    if (flags & (0x10 | 0x100000)) {
        if ((start & (PAGE - 1)) || !valid_range(start, length)) return -EINVAL;
        if ((flags & 0x100000) && !mapping_free(start, length)) return -EEXIST;
        if (!(flags & 0x100000)) unmap_range(start, length);
    } else {
        start = page_down(start);
        if (!start || !mapping_free(start, length)) {
            start = next_mapping;
            while (!mapping_free(start, length)) {
                if (start > USER_LIMIT - length - PAGE) return -ENOMEM;
                start += page_up(length);
            }
        }
    }
    if (memory_bytes + length > MAX_MEMORY) return -ENOMEM;
    uint64_t address;
    for (address = start; address < start + length; address += PAGE) {
        void* page = pmm_alloc(1);
        if (!page) break;
        zero(page, PAGE);
        if (opened && opened->file.kind == FILE_REG) {
            long result = file_read(&opened->file, offset + address - start, page, PAGE);
            if (result < 0) { pmm_free(page, 1); break; }
        }
        if (!paging_map_user_page(linux_user_cr3, address, (uintptr_t)page, (protection & 2) != 0)) {
            pmm_free(page, 1);
            break;
        }
        memory_bytes += PAGE;
        paging_set_user_protection(linux_user_cr3, address, protection);
    }
    if (address != start + length) {
        if (address > start) unmap_range(start, address - start);
        return -ENOMEM;
    }
    if (!(flags & (0x10 | 0x100000)) && start >= MAP_BASE) next_mapping = start + length + PAGE;
    return start;
}
static long remap_range(uint64_t old, uint64_t old_size, uint64_t new_size,
    unsigned flags, uint64_t requested) {
    if ((old & (PAGE - 1)) || !old_size || !new_size || old_size > MAX_MEMORY ||
        new_size > MAX_MEMORY || (flags & ~3U) || ((flags & 2) && !(flags & 1)))
        return -EINVAL;
    old_size = page_up(old_size);
    new_size = page_up(new_size);
    if (!valid_range(old, old_size)) return -EINVAL;
    unsigned protection = 0;
    for (uint64_t offset = 0; offset < old_size; offset += PAGE)
        if (!paging_user_mapping(linux_user_cr3, old + offset, NULL, &protection))
            return -EFAULT;
    if (!(flags & 2)) {
        if (new_size <= old_size) {
            if (new_size < old_size) unmap_range(old + new_size, old_size - new_size);
            return old;
        }
        if (mapping_free(old + old_size, new_size - old_size)) {
            long result = map_range(old + old_size, new_size - old_size,
                protection, 0x32, -1, 0);
            return result < 0 ? result : (long)old;
        }
        if (!(flags & 1)) return -ENOMEM;
    } else if ((requested & (PAGE - 1)) || !valid_range(requested, new_size) ||
        (requested < old + old_size && requested + new_size > old)) return -EINVAL;
    long result = map_range((flags & 2) ? requested : 0, new_size, 3,
        (flags & 2) ? 0x32 : 0x22, -1, 0);
    if (result < 0) return result;
    uint64_t destination = result, copied = old_size < new_size ? old_size : new_size;
    for (uint64_t offset = 0; offset < copied; offset += PAGE) {
        uintptr_t source, target;
        paging_user_mapping(linux_user_cr3, old + offset, &source, &protection);
        paging_user_mapping(linux_user_cr3, destination + offset, &target, NULL);
        copy((void*)target, (void*)source, PAGE);
        paging_set_user_protection(linux_user_cr3, destination + offset, protection);
    }
    for (uint64_t offset = copied; offset < new_size; offset += PAGE)
        paging_set_user_protection(linux_user_cr3, destination + offset, protection);
    unmap_range(old, old_size);
    return destination;
}

static void release_user_pages(uint64_t* table, unsigned level) {
    for (unsigned n = 0; n < 512; n++) {
        uint64_t entry = table[n];
        if (entry & 128) continue;
        if (level > 1) {
            if (entry & 1) release_user_pages((uint64_t*)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL), level - 1);
        } else if (entry & 4) pmm_free((void*)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL), 1);
    }
}
static void finish(int status) __attribute__((noreturn));
static void finish(int status) {
    paging_restore_kernel_address_space();
    if (clear_tid) { uint32_t empty = 0; user_copy(&empty, clear_tid, sizeof(empty), true); }
    write_msr(0xC0000100, 0);
    write_msr(0xC0000101, 0);
    if (linux_user_cr3) {
        release_user_pages((uint64_t*)linux_user_cr3, 4);
        paging_address_space_destroy(linux_user_cr3);
    }
    linux_user_cr3 = 0;
    linux_x11_reset();
    session_active = false;
    session_pid = 0;
    serial_write_string("[linux] exit ");
    serial_write_dec((uint32_t)status);
    serial_write_string("\n");
    task_exit();
}
bool linux_compat_active(void) { return session_active; }
bool linux_compat_current(void) {
    task_t* task = task_get_current();
    return session_active && task && task->pid == session_pid;
}
void linux_compat_fault(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address) {
    serial_write_string("[linux] fault vector="); serial_write_dec(vector);
    serial_write_string(" error="); serial_write_hex(error);
    serial_write_string(" rip="); serial_write_hex(rip);
    serial_write_string(" address="); serial_write_hex(address);
    serial_write_string("\n");
    display_puts("[linux] application fault; see serial log.\n");
    finish(128 + (vector == 6 ? 4 : 11));
}
static bool load_elf(const guest_file_t* file, uint64_t base, loaded_elf_t* loaded) {
    elf_header_t header;
    zero(loaded, sizeof(*loaded));
    if (file_read(file, 0, &header, sizeof(header)) != sizeof(header) ||
        header.ident[0] != 0x7f || header.ident[1] != 'E' || header.ident[2] != 'L' ||
        header.ident[3] != 'F' || header.ident[4] != 2 || header.ident[5] != 1 ||
        header.machine != 62 || (header.type != 2 && header.type != 3) ||
        header.phentsize != sizeof(elf_program_t) || !header.phnum || header.phnum > 128 ||
        header.phoff > file->size || (uint64_t)header.phnum * header.phentsize > file->size - header.phoff) return false;
    if (header.type == 2) base = 0;
    loaded->base = base;
    loaded->entry = base + header.entry;
    loaded->phnum = header.phnum;
    for (unsigned n = 0; n < header.phnum; n++) {
        elf_program_t program;
        if (file_read(file, header.phoff + n * sizeof(program), &program, sizeof(program)) != sizeof(program)) return false;
        if (program.type == 3) {
            if (program.filesz < 2 || program.filesz > sizeof(loaded->interpreter) ||
                file_read(file, program.offset, loaded->interpreter, program.filesz) != (long)program.filesz ||
                loaded->interpreter[program.filesz - 1]) return false;
        }
        if (program.type == 6) loaded->phdr = base + program.vaddr;
        if (program.type != 1 || !program.memsz) continue;
        if (program.filesz > program.memsz || program.offset > file->size ||
            program.filesz > file->size - program.offset || program.vaddr > USER_LIMIT - base ||
            !valid_range(base + program.vaddr, program.memsz) || program.memsz > MAX_MEMORY ||
            (program.offset & (PAGE - 1)) != (program.vaddr & (PAGE - 1))) return false;
        uint64_t first = page_down(base + program.vaddr);
        uint64_t last = page_up(base + program.vaddr + program.memsz);
        for (uint64_t address = first; address < last; address += PAGE) {
            if (!paging_user_page(linux_user_cr3, address, NULL, NULL)) {
                if (map_range(address, PAGE, 3, 0x32, -1, 0) < 0) return false;
            } else paging_protect_user_page(linux_user_cr3, address, true);
        }
        unsigned char buffer[PAGE];
        for (uint64_t offset = 0; offset < program.filesz;) {
            size_t length = program.filesz - offset;
            if (length > sizeof(buffer)) length = sizeof(buffer);
            if (file_read(file, program.offset + offset, buffer, length) != (long)length ||
                !user_copy(buffer, base + program.vaddr + offset, length, true)) return false;
            offset += length;
        }
        if (header.phoff >= program.offset && header.phoff + header.phnum * sizeof(elf_program_t) <= program.offset + program.filesz)
            loaded->phdr = base + program.vaddr + header.phoff - program.offset;
        if (base + program.vaddr + program.memsz > loaded->end)
            loaded->end = base + program.vaddr + program.memsz;
    }
    if (!loaded->phdr || !user_valid(loaded->entry, 1, false)) return false;
    for (unsigned n = 0; n < header.phnum; n++) {
        elf_program_t program;
        if (file_read(file, header.phoff + n * sizeof(program), &program, sizeof(program)) != sizeof(program)) return false;
        if (program.type != 1 || !program.memsz) continue;
        unsigned protection = ((program.flags & 4) ? 1 : 0) |
            ((program.flags & 2) ? 2 : 0) | ((program.flags & 1) ? 4 : 0);
        for (uint64_t address = page_down(base + program.vaddr);
            address < page_up(base + program.vaddr + program.memsz); address += PAGE)
            paging_set_user_protection(linux_user_cr3, address, protection);
    }
    return true;
}
static bool stack_string(uint64_t* stack, const char* string, uint64_t* address) {
    size_t length = text_length(string) + 1;
    *stack -= length;
    *address = *stack;
    return user_copy((void*)string, *stack, length, true);
}
static bool initial_stack(const loaded_elf_t* main, uint64_t interpreter_base, uint64_t* stack) {
    if (map_range(STACK_TOP - STACK_SIZE, STACK_SIZE, 3, 0x32, -1, 0) < 0) return false;
    uint64_t pointer = STACK_TOP, argv[MAX_ARGS], envp[8], argc = 1, envc = 0, executable, random_address;
    if (!stack_string(&pointer, launch_path, &argv[0])) return false;
    executable = argv[0];
    char arguments[ARG_SIZE];
    text_copy(arguments, launch_arguments, sizeof(arguments));
    size_t position = 0;
    while (arguments[position]) {
        while (arguments[position] == ' ' || arguments[position] == '\t') position++;
        if (!arguments[position]) break;
        if (argc >= MAX_ARGS) return false;
        size_t start = position;
        while (arguments[position] && arguments[position] != ' ' && arguments[position] != '\t') position++;
        char separator = arguments[position];
        arguments[position] = 0;
        if (!stack_string(&pointer, arguments + start, &argv[argc++])) return false;
        if (!separator) break;
        position++;
    }
    const char* environment[] = {"DISPLAY=:0", "HOME=/root", "PATH=/bin:/usr/bin", "LANG=C", "LC_ALL=C", "USER=root"};
    for (unsigned n = 0; n < sizeof(environment) / sizeof(environment[0]); n++)
        if (!stack_string(&pointer, environment[n], &envp[envc++])) return false;
    pointer = (pointer - 16) & ~15ULL;
    random_address = pointer;
    uint64_t random[2] = {random_word(), random_word()};
    if (!user_copy(random, pointer, sizeof(random), true)) return false;
    const uint64_t auxiliary[][2] = {
        {3, main->phdr}, {4, sizeof(elf_program_t)}, {5, main->phnum},
        {6, PAGE}, {7, interpreter_base}, {8, 0}, {9, main->entry},
        {11, 0}, {12, 0}, {13, 0}, {14, 0}, {16, 0}, {17, 100},
        {23, 0}, {25, random_address}, {26, 0}, {31, executable}, {0, 0}
    };
    uint64_t words = 1 + argc + 1 + envc + 1 + 2 * (sizeof(auxiliary) / sizeof(auxiliary[0]));
    pointer = (pointer - words * 8) & ~15ULL;
    *stack = pointer;
    if (!user_copy(&argc, pointer, 8, true)) return false;
    pointer += 8;
    for (uint64_t n = 0; n < argc; n++, pointer += 8)
        if (!user_copy(&argv[n], pointer, 8, true)) return false;
    uint64_t empty = 0;
    if (!user_copy(&empty, pointer, 8, true)) return false;
    pointer += 8;
    for (uint64_t n = 0; n < envc; n++, pointer += 8)
        if (!user_copy(&envp[n], pointer, 8, true)) return false;
    if (!user_copy(&empty, pointer, 8, true)) return false;
    pointer += 8;
    return user_copy((void*)auxiliary, pointer, sizeof(auxiliary), true);
}
void linux_compat_init(uintptr_t boot_info_address) {
    linux_kernel_cr3 = paging_kernel_address_space();
    if (!boot_info_address) return;
    const multiboot1_info_t* info = (void*)boot_info_address;
    if (!(info->flags & MULTIBOOT1_INFO_MODULES)) return;
    const multiboot1_module_t* modules = (void*)(uintptr_t)info->mods_addr;
    for (uint32_t n = 0; n < info->mods_count; n++) {
        if (modules[n].mod_end <= modules[n].mod_start || !modules[n].string) continue;
        const char* label = (void*)(uintptr_t)modules[n].string;
        const char* expected = "REBANE_LINUX_ROOT";
        size_t i = 0;
        while (expected[i] && label[i] == expected[i]) i++;
        if (expected[i] || (label[i] && label[i] != ' ')) continue;
        root_archive = (void*)(uintptr_t)modules[n].mod_start;
        root_archive_size = modules[n].mod_end - modules[n].mod_start;
        serial_write_string("[linux] guest root archive mounted\n");
        break;
    }
}
bool linux_compat_launch(const char* path, const char* arguments) {
    if (session_active || !path || !arguments || text_length(path) >= sizeof(launch_path) ||
        text_length(arguments) >= sizeof(launch_arguments)) return false;
    guest_file_t executable;
    if (find_file(path, &executable) < 0 || executable.kind != FILE_REG) return false;
    text_copy(launch_path, path, sizeof(launch_path));
    text_copy(launch_arguments, arguments, sizeof(launch_arguments));
    session_active = true;
    task_t* task = task_create("linux", linux_compat_task);
    if (!task) { session_active = false; return false; }
    session_pid = task->pid;
    return true;
}
void linux_compat_task(void) {
    paging_restore_kernel_address_space();
    zero(handles, sizeof(handles));
    zero(actions, sizeof(actions));
    for (unsigned n = 0; n < FD_COUNT; n++) { descriptors[n].handle = -1; descriptors[n].flags = 0; }
    guest_file_t terminal;
    zero(&terminal, sizeof(terminal));
    terminal.kind = FILE_TTY;
    terminal.mode = 0020666;
    text_copy(terminal.path, "/dev/tty", sizeof(terminal.path));
    fd_allocate(&terminal, 0, 0);
    fd_allocate(&terminal, 1, 1);
    fd_allocate(&terminal, 1, 2);
    text_copy(current_directory, "/", sizeof(current_directory));
    memory_bytes = clear_tid = signal_mask = 0;
    next_mapping = MAP_BASE;
    linux_user_cr3 = paging_address_space_create();
    if (!linux_user_cr3) finish(127);
    task_t* task = task_get_current();
    linux_kernel_stack = (task->kernel_stack_base + task->kernel_stack_size) & ~15ULL;
    if (!gdt_set_kernel_stack(linux_kernel_stack)) finish(127);
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~((1ULL << 2) | (1ULL << 3))) | (1ULL << 1) | (1ULL << 5);
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    asm volatile("fninit");
    uint32_t mxcsr = 0x1f80;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    uint32_t a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    cpu_has_rdrand = (c & (1U << 30)) != 0;
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    entropy_state = ((uint64_t)high << 32) | low | 1;
    write_msr(0xC0000080, read_msr(0xC0000080) | 1 | (1ULL << 11));
    write_msr(0xC0000081, 0x0000000800000000ULL);
    write_msr(0xC0000082, (uintptr_t)linux_syscall_entry);
    write_msr(0xC0000084, (1ULL << 8) | (1ULL << 9) | (1ULL << 10) | (1ULL << 18));
    write_msr(0xC0000100, 0);
    write_msr(0xC0000101, 0);
    guest_file_t executable;
    loaded_elf_t main, interpreter;
    if (find_file(launch_path, &executable) < 0 || !load_elf(&executable, 0x40000000, &main)) {
        log_text("[linux] invalid or unmappable ELF executable\n"); finish(126);
    }
    uint64_t entry = main.entry, interpreter_base = 0, stack;
    initial_break = program_break = page_up(main.end);
    if (main.interpreter[0]) {
        guest_file_t loader;
        if (find_file(main.interpreter, &loader) < 0 || !load_elf(&loader, 0x80000000, &interpreter) ||
            interpreter.interpreter[0]) {
            log_text("[linux] ELF interpreter unavailable or invalid\n"); finish(126);
        }
        entry = interpreter.entry;
        interpreter_base = interpreter.base;
    }
    if (!initial_stack(&main, interpreter_base, &stack)) {
        log_text("[linux] cannot construct initial stack\n"); finish(126);
    }
    serial_write_string("[linux] entering "); serial_write_string(launch_path);
    serial_write_string(" entry="); serial_write_hex(entry); serial_write_string("\n");
    linux_enter(entry, stack, linux_user_cr3);
}

static long read_descriptor(long fd, uint64_t output, uint64_t length, bool positioned, uint64_t offset) {
    open_file_t* file = fd_get(fd);
    if (!file) return -EBADF;
    if ((file->flags & 3) == 1) return -EBADF;
    if (length > 0x7ffff000) length = 0x7ffff000;
    if (!user_valid(output, length, true)) return -EFAULT;
    if (!length) return 0;
    unsigned char buffer[PAGE];
    uint64_t total = 0;
    while (total < length) {
        size_t chunk = length - total;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        long result;
        switch (file->file.kind) {
        case FILE_REG:
            result = file_read(&file->file, positioned ? offset + total : file->offset, buffer, chunk);
            break;
        case FILE_NULL: return total;
        case FILE_ZERO: zero(buffer, chunk); result = chunk; break;
        case FILE_RANDOM:
            if (!cpu_has_rdrand) return total ? (long)total : -ENOSYS;
            if (!fill_entropy(buffer, chunk)) return total ? (long)total : -EAGAIN;
            result = chunk;
            break;
        case FILE_SOCKET:
            if (!file->connected) return -ENOTCONN;
            result = linux_x11_read(buffer, chunk);
            while (result == -EAGAIN && !total && !(file->flags & 04000)) {
                task_sleep_ticks(1);
                result = linux_x11_read(buffer, chunk);
            }
            break;
        case FILE_TTY: {
            input_key_event_t key;
            result = 0;
            while (!result) {
                while (keyboard_get_event(&key)) {
                    if (key.is_pressed && key.ascii) { buffer[result++] = key.ascii == '\r' ? '\n' : key.ascii; break; }
                }
                if (result || total || (file->flags & 04000)) break;
                task_sleep_ticks(1);
            }
            if (!result) return total ? (long)total : -EAGAIN;
            break;
        }
        default: return -EISDIR;
        }
        if (result < 0) return total ? (long)total : result;
        if (!result) return total;
        if (!user_copy(buffer, output + total, result, true)) return -EFAULT;
        total += result;
        if (!positioned && file->file.kind == FILE_REG) file->offset += result;
        if ((size_t)result < chunk || file->file.kind == FILE_SOCKET || file->file.kind == FILE_TTY) break;
    }
    return total;
}
static long write_descriptor(long fd, uint64_t input, uint64_t length) {
    open_file_t* file = fd_get(fd);
    if (!file) return -EBADF;
    if ((file->flags & 3) == 0 && file->file.kind != FILE_SOCKET) return -EBADF;
    if (length > 0x7ffff000) length = 0x7ffff000;
    if (!user_valid(input, length, false)) return -EFAULT;
    if (file->file.kind == FILE_REG || file->file.kind == FILE_DIR) return -EROFS;
    if (file->file.kind == FILE_NULL || file->file.kind == FILE_ZERO || file->file.kind == FILE_RANDOM) return length;
    char buffer[PAGE];
    uint64_t total = 0;
    while (total < length) {
        size_t chunk = length - total;
        if (chunk > sizeof(buffer) - 1) chunk = sizeof(buffer) - 1;
        if (!user_copy(buffer, input + total, chunk, false)) return -EFAULT;
        long result;
        if (file->file.kind == FILE_SOCKET) {
            if (!file->connected) return -ENOTCONN;
            result = linux_x11_write(buffer, chunk);
        } else {
            for (size_t n = 0; n < chunk; n++) display_putchar(buffer[n]);
            buffer[chunk] = 0;
            serial_write_string(buffer);
            result = chunk;
        }
        if (result < 0) return total ? (long)total : result;
        total += result;
        if ((size_t)result < chunk) break;
    }
    return total;
}
static long vector_io(long fd, uint64_t vectors, uint64_t count, bool writing) {
    if (count > 1024) return -EINVAL;
    if (!user_valid(vectors, count * sizeof(linux_iovec_t), false)) return -EFAULT;
    uint64_t total = 0;
    for (uint64_t n = 0; n < count; n++) {
        linux_iovec_t vector;
        if (!user_copy(&vector, vectors + n * sizeof(vector), sizeof(vector), false)) return -EFAULT;
        if (vector.length > 0x7ffff000 - total) return total ? (long)total : -EINVAL;
        long result = writing ? write_descriptor(fd, vector.base, vector.length) :
            read_descriptor(fd, vector.base, vector.length, false, 0);
        if (result < 0) return total ? (long)total : result;
        total += result;
        if ((uint64_t)result < vector.length) break;
        if (!writing && total) {
            open_file_t* file = fd_get(fd);
            if (file && file->file.kind == FILE_SOCKET && !linux_x11_available()) break;
        }
    }
    return total;
}
static long poll_descriptors(uint64_t array, uint64_t count, int64_t timeout_ms) {
    struct poll_descriptor { int32_t fd; int16_t events, revents; } item;
    if (count > FD_COUNT || !user_valid(array, count * sizeof(item), true)) return -EFAULT;
    uint64_t start = timer_get_ticks();
    for (;;) {
        long ready = 0;
        for (uint64_t n = 0; n < count; n++) {
            user_copy(&item, array + n * sizeof(item), sizeof(item), false);
            item.revents = 0;
            open_file_t* file = fd_get(item.fd);
            if (item.fd >= 0 && !file) item.revents = 0x20;
            else if (file) {
                if (file->file.kind == FILE_SOCKET) {
                    if (!file->connected) item.revents = 0x10;
                    else {
                        if ((item.events & 1) && linux_x11_available()) item.revents |= 1;
                        if (item.events & 4) item.revents |= 4;
                    }
                } else {
                    if ((item.events & 1) && file->file.kind != FILE_TTY) item.revents |= 1;
                    if (item.events & 4) item.revents |= 4;
                }
            }
            if (item.revents) ready++;
            user_copy(&item, array + n * sizeof(item), sizeof(item), true);
        }
        if (ready || !timeout_ms || (timeout_ms > 0 && timer_get_ticks() - start >= ms_ticks(timeout_ms))) return ready;
        task_sleep_ticks(1);
    }
}
static long select_descriptors(uint64_t count, uint64_t read_address,
    uint64_t write_address, uint64_t except_address, uint64_t timeout_address,
    bool nanoseconds) {
    if (count > 1024) return -EINVAL;
    uint64_t reads[16], writes[16], exceptions[16];
    size_t bytes = ((count + 63) / 64) * 8;
    zero(reads, sizeof(reads)); zero(writes, sizeof(writes));
    zero(exceptions, sizeof(exceptions));
    if ((read_address && (!user_copy(reads, read_address, bytes, false) ||
            !user_valid(read_address, bytes, true))) ||
        (write_address && (!user_copy(writes, write_address, bytes, false) ||
            !user_valid(write_address, bytes, true))) ||
        (except_address && (!user_copy(exceptions, except_address, bytes, false) ||
            !user_valid(except_address, bytes, true)))) return -EFAULT;
    int64_t duration = -1;
    if (timeout_address) {
        linux_time_t time;
        if (!user_copy(&time, timeout_address, sizeof(time), false) ||
            !user_valid(timeout_address, sizeof(time), true)) return -EFAULT;
        if (time.sec < 0 || time.nsec < 0 ||
            time.nsec >= (nanoseconds ? 1000000000 : 1000000)) return -EINVAL;
        duration = time.sec * 100 +
            (time.nsec + (nanoseconds ? 9999999 : 9999)) /
            (nanoseconds ? 10000000 : 10000);
    }
    uint64_t start = timer_get_ticks();
    for (;;) {
        uint64_t ready_reads[16], ready_writes[16], ready_exceptions[16];
        zero(ready_reads, sizeof(ready_reads)); zero(ready_writes, sizeof(ready_writes));
        zero(ready_exceptions, sizeof(ready_exceptions));
        long ready = 0;
        for (uint64_t fd = 0; fd < count; fd++) {
            uint64_t bit = 1ULL << (fd & 63);
            unsigned word = fd / 64;
            if (!((reads[word] | writes[word] | exceptions[word]) & bit)) continue;
            open_file_t* file = fd_get(fd);
            if (!file) return -EBADF;
            if ((reads[word] & bit) && (file->file.kind == FILE_SOCKET ?
                    (!file->connected || linux_x11_available()) :
                    file->file.kind != FILE_TTY)) {
                ready_reads[word] |= bit; ready++;
            }
            if (writes[word] & bit) { ready_writes[word] |= bit; ready++; }
        }
        uint64_t elapsed = timer_get_ticks() - start;
        if (ready || !duration || (duration > 0 && elapsed >= (uint64_t)duration)) {
            if ((read_address && !user_copy(ready_reads, read_address, bytes, true)) ||
                (write_address && !user_copy(ready_writes, write_address, bytes, true)) ||
                (except_address && !user_copy(ready_exceptions, except_address, bytes, true)))
                return -EFAULT;
            if (timeout_address) {
                uint64_t left = duration > (int64_t)elapsed ? duration - elapsed : 0;
                linux_time_t remaining = {left / 100,
                    (left % 100) * (nanoseconds ? 10000000 : 10000)};
                user_copy(&remaining, timeout_address, sizeof(remaining), true);
            }
            return ready;
        }
        task_sleep_ticks(1);
    }
}

static long socket_message(long fd, uint64_t address, unsigned flags, bool writing) {
    linux_message_t message;
    if (!user_copy(&message, address, sizeof(message), false)) return -EFAULT;
    open_file_t* file = fd_get(fd);
    if (!file) return -EBADF;
    if (file->file.kind != FILE_SOCKET) return -ENOTSOCK;
    if (message.control_length && writing) return -EOPNOTSUPP;
    unsigned old_flags = file->flags;
    if (flags & 0x40) file->flags |= 04000;
    long result = vector_io(fd, message.vectors, message.vector_count, writing);
    file->flags = old_flags;
    if (!writing && result >= 0) {
        message.name_length = message.control_length = message.flags = 0;
        if (!user_copy(&message, address, sizeof(message), true)) return -EFAULT;
    }
    return result;
}
static long ioctl_descriptor(long fd, uint64_t request, uint64_t argument) {
    open_file_t* file = fd_get(fd);
    if (!file) return -EBADF;
    if (request == 0x541b) {
        int32_t available = file->file.kind == FILE_SOCKET ? linux_x11_available() : 0;
        if (file->file.kind == FILE_REG) available = file->file.size - file->offset;
        return user_copy(&available, argument, sizeof(available), true) ? 0 : -EFAULT;
    }
    if (request == 0x5421) {
        int32_t enabled;
        if (!user_copy(&enabled, argument, sizeof(enabled), false)) return -EFAULT;
        if (enabled) file->flags |= 04000; else file->flags &= ~04000U;
        return 0;
    }
    if (file->file.kind != FILE_TTY) return -ENOTTY;
    if (request == 0x5413) {
        uint16_t size[4] = {48, 128, 1024, 768};
        return user_copy(size, argument, sizeof(size), true) ? 0 : -EFAULT;
    }
    if (request == 0x5401) {
        struct { uint32_t input, output, control, local; unsigned char line, chars[19]; } term;
        zero(&term, sizeof(term));
        term.input = 0x500; term.output = 5; term.control = 0xbf; term.local = 0x8a3b;
        term.chars[0] = 3; term.chars[1] = 28; term.chars[2] = 127; term.chars[3] = 21;
        term.chars[4] = 4; term.chars[6] = 1;
        return user_copy(&term, argument, sizeof(term), true) ? 0 : -EFAULT;
    }
    if (request == 0x540f) {
        int32_t group = session_pid;
        return user_copy(&group, argument, sizeof(group), true) ? 0 : -EFAULT;
    }
    return -ENOTTY;
}
static long duplicate_fd(long source, long destination, bool exact, unsigned flags) {
    open_file_t* file = fd_get(source);
    if (!file) return -EBADF;
    if (destination < 0 || destination >= FD_COUNT) return exact ? -EBADF : -EINVAL;
    if (!exact) while (destination < FD_COUNT && fd_get(destination)) destination++;
    if (destination >= FD_COUNT) return -EMFILE;
    if (source == destination) return destination;
    if (fd_get(destination)) fd_close(destination);
    descriptors[destination].handle = descriptors[source].handle;
    descriptors[destination].flags = flags;
    file->references++;
    return destination;
}
static long get_time(uint64_t output, bool timeval) {
    uint64_t ticks = timer_get_ticks();
    linux_time_t time = {(int64_t)(ticks / 100), (int64_t)((ticks % 100) * (timeval ? 10000 : 10000000))};
    return user_copy(&time, output, sizeof(time), true) ? 0 : -EFAULT;
}
static long read_link(long dirfd, uint64_t address, uint64_t output, uint64_t capacity) {
    char path[PATH_SIZE], normalized[PATH_SIZE];
    if (!capacity) return -EINVAL;
    if (!user_string(address, path, sizeof(path))) return -EFAULT;
    if (path[0] != '/' && dirfd != AT_FDCWD) return -ENOTDIR;
    if (!normalize_path(path, normalized)) return -36;
    const char* target = NULL;
    char buffer[PATH_SIZE];
    if (text_equal(normalized, "/proc/self/exe")) target = launch_path;
    for (size_t offset = 0; !target && root_archive && offset + 512 <= root_archive_size;) {
        const unsigned char* header = root_archive + offset;
        if (!header[0]) break;
        uint64_t length = tar_number(header + 124, 12);
        if (length > root_archive_size - offset - 512) return -EIO;
        char name[PATH_SIZE];
        if (tar_name(header, name) && text_equal(name, normalized)) {
            if (header[156] != '2') return -EINVAL;
            unsigned n = 0;
            while (n < 100 && header[157 + n]) { buffer[n] = header[157 + n]; n++; }
            buffer[n] = 0;
            target = buffer;
        }
        offset += 512 + ((length + 511) & ~511ULL);
    }
    if (!target) return -ENOENT;
    size_t length = text_length(target);
    if (length > capacity) length = capacity;
    return user_copy((void*)target, output, length, true) ? (long)length : -EFAULT;
}
static long syscall_dispatch(uint64_t number, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f) {
    switch (number) {
    case 0: return read_descriptor(a, b, c, false, 0);
    case 1: return write_descriptor(a, b, c);
    case 2: return open_path(AT_FDCWD, a, b);
    case 3: return fd_close(a);
    case 4: case 6: return stat_path(AT_FDCWD, a, b, 0);
    case 5: {
        open_file_t* file = fd_get(a);
        if (!file) return -EBADF;
        linux_stat_t stat;
        fill_stat(&file->file, &stat);
        return user_copy(&stat, b, sizeof(stat), true) ? 0 : -EFAULT;
    }
    case 7: return poll_descriptors(a, b, (int32_t)c);
    case 8: {
        open_file_t* file = fd_get(a);
        if (!file) return -EBADF;
        if (file->file.kind != FILE_REG && file->file.kind != FILE_DIR) return -ESPIPE;
        int64_t offset = (int64_t)b;
        if (c == 1) offset += file->offset;
        else if (c == 2) offset += file->file.size;
        else if (c != 0) return -EINVAL;
        if (offset < 0) return -EINVAL;
        file->offset = offset;
        return offset;
    }
    case 9: return map_range(a, b, c, d, e, f);
    case 10: {
        if ((a & (PAGE - 1)) || !valid_range(a, page_up(b)) || b > MAX_MEMORY || (c & ~7ULL)) return -EINVAL;
        for (uint64_t address = a; address < a + page_up(b); address += PAGE)
            if (!paging_user_mapping(linux_user_cr3, address, NULL, NULL)) return -ENOMEM;
        for (uint64_t address = a; address < a + page_up(b); address += PAGE)
            paging_set_user_protection(linux_user_cr3, address, c);
        return 0;
    }
    case 11: return unmap_range(a, b);
    case 12:
        if (!a) return program_break;
        if (a < initial_break || a - initial_break > MAX_MEMORY || !valid_range(initial_break, a - initial_break)) return program_break;
        if (page_up(a) > page_up(program_break)) {
            uint64_t start = page_up(program_break), length = page_up(a) - start;
            if (!mapping_free(start, length) || map_range(start, length, 3, 0x32, -1, 0) < 0) return program_break;
        } else if (page_up(a) < page_up(program_break)) unmap_range(page_up(a), page_up(program_break) - page_up(a));
        program_break = a;
        return program_break;
    case 13:
        if (!a || a > 64 || a == 9 || a == 19 || d != 8) return -EINVAL;
        if (c && !user_copy(&actions[a - 1], c, sizeof(signal_action_t), true)) return -EFAULT;
        if (b && !user_copy(&actions[a - 1], b, sizeof(signal_action_t), false)) return -EFAULT;
        return 0;
    case 14: {
        if (d != 8) return -EINVAL;
        if (c && !user_copy(&signal_mask, c, 8, true)) return -EFAULT;
        uint64_t mask;
        if (b) {
            if (!user_copy(&mask, b, 8, false)) return -EFAULT;
            if (a == 0) signal_mask |= mask;
            else if (a == 1) signal_mask &= ~mask;
            else if (a == 2) signal_mask = mask;
            else return -EINVAL;
            signal_mask &= ~((1ULL << 8) | (1ULL << 18));
        }
        return 0;
    }
    case 16: return ioctl_descriptor(a, b, c);
    case 17: return read_descriptor(a, b, c, true, d);
    case 19: return vector_io(a, b, c, false);
    case 20: return vector_io(a, b, c, true);
    case 21: case 269: {
        char path[PATH_SIZE]; guest_file_t file;
        uint64_t pointer = number == 21 ? a : b;
        unsigned mode = number == 21 ? b : c;
        if (!user_string(pointer, path, sizeof(path))) return -EFAULT;
        int result = find_file(path, &file);
        if (result < 0) return result;
        if (mode & ~7U) return -EINVAL;
        if ((mode & 2) && (file.kind == FILE_REG || file.kind == FILE_DIR)) return -EROFS;
        if ((mode & 1) && !(file.mode & 0111)) return -EACCES;
        return 0;
    }
    case 23: return select_descriptors(a, b, c, d, e, false);
    case 24: task_yield(); return 0;
    case 25: return remap_range(a, b, c, d, e);
    case 28:
        if ((a & (PAGE - 1)) || !valid_range(a, b)) return -EINVAL;
        if (c == 0 || c == 1 || c == 2 || c == 3 || c == 14 || c == 15) return 0;
        if (c == 4) {
            unsigned char empty[PAGE]; zero(empty, sizeof(empty));
            for (uint64_t offset = 0; offset < b; offset += PAGE) {
                size_t chunk = b - offset > PAGE ? PAGE : b - offset;
                if (!user_copy(empty, a + offset, chunk, true)) return -ENOMEM;
            }
            return 0;
        }
        return -EINVAL;
    case 32: return duplicate_fd(a, 0, false, 0);
    case 33: return duplicate_fd(a, b, true, 0);
    case 35: case 230: {
        linux_time_t time;
        uint64_t pointer = number == 35 ? a : c;
        if (!user_copy(&time, pointer, sizeof(time), false)) return -EFAULT;
        if (time.sec < 0 || time.nsec < 0 || time.nsec >= 1000000000) return -EINVAL;
        uint64_t ticks = time.sec * 100 + (time.nsec + 9999999) / 10000000;
        if (number == 230 && (b & 1)) {
            uint64_t now = timer_get_ticks();
            ticks = ticks > now ? ticks - now : 0;
        }
        if (ticks) task_sleep_ticks(ticks);
        return 0;
    }
    case 39: case 186: return session_pid;
    case 41: {
        if (a != 1) return -EAFNOSUPPORT;
        if ((b & 15) != 1 || c != 0) return -EOPNOTSUPP;
        guest_file_t file; zero(&file, sizeof(file));
        file.kind = FILE_SOCKET; file.mode = 0140777;
        return fd_allocate(&file, 2 | ((b & 0x800) ? 04000 : 0) | ((b & 0x80000) ? 02000000 : 0), 0);
    }
    case 42: {
        open_file_t* file = fd_get(a);
        if (!file) return -EBADF;
        if (file->file.kind != FILE_SOCKET) return -ENOTSOCK;
        if (c < 3 || c > 110) return -EINVAL;
        unsigned char address[110];
        if (!user_copy(address, b, c, false)) return -EFAULT;
        if (file->connected) return -106;
        int result = linux_x11_connect(address, c);
        if (!result) file->connected = true;
        return result;
    }
    case 44:
        if (e || f) return -EOPNOTSUPP;
        return write_descriptor(a, b, c);
    case 45: {
        if (e || f || (d & ~0x40ULL)) return -EOPNOTSUPP;
        open_file_t* file = fd_get(a); if (!file) return -EBADF;
        unsigned flags = file->flags;
        if (d & 0x40) file->flags |= 04000;
        long result = read_descriptor(a, b, c, false, 0);
        file->flags = flags;
        return result;
    }
    case 46: return socket_message(a, b, c, true);
    case 47: return socket_message(a, b, c, false);
    case 48: {
        open_file_t* file = fd_get(a); if (!file) return -EBADF;
        if (file->file.kind != FILE_SOCKET) return -ENOTSOCK;
        if (b > 2) return -EINVAL;
        file->connected = false; linux_x11_reset(); return 0;
    }
    case 51: case 52: {
        open_file_t* file = fd_get(a); if (!file) return -EBADF;
        if (file->file.kind != FILE_SOCKET) return -ENOTSOCK;
        if (number == 52 && !file->connected) return -ENOTCONN;
        uint32_t length;
        if (!user_copy(&length, c, 4, false)) return -EFAULT;
        unsigned char name[2] = {1, 0};
        uint32_t used = length > 2 ? 2 : length;
        if (!user_copy(name, b, used, true)) return -EFAULT;
        length = 2;
        return user_copy(&length, c, 4, true) ? 0 : -EFAULT;
    }
    case 54: return -ENOPROTOOPT;
    case 55: {
        open_file_t* file = fd_get(a); if (!file) return -EBADF;
        if (file->file.kind != FILE_SOCKET) return -ENOTSOCK;
        if (b != 1) return -ENOPROTOOPT;
        int32_t value;
        if (c == 4) value = 0;
        else if (c == 3) value = 1;
        else return -ENOPROTOOPT;
        uint32_t length;
        if (!user_copy(&length, e, 4, false) || length < 4) return -EFAULT;
        if (!user_copy(&value, d, 4, true)) return -EFAULT;
        length = 4;
        return user_copy(&length, e, 4, true) ? 0 : -EFAULT;
    }
    case 60: case 231: finish(a & 255);
    case 62: case 234: {
        uint64_t signal = number == 62 ? b : c;
        uint64_t pid = number == 62 ? a : b;
        if (pid != session_pid && pid != 0 && (int64_t)pid != -1) return -3;
        if (signal > 64) return -EINVAL;
        if (!signal) return 0;
        if (actions[signal - 1].handler == 1) return 0;
        if (signal == 9 || actions[signal - 1].handler == 0) finish(128 + signal);
        return -EOPNOTSUPP;
    }
    case 63: {
        char name[6][65]; zero(name, sizeof(name));
        text_copy(name[0], "Linux", 65); text_copy(name[1], "rebane", 65);
        text_copy(name[2], "6.1.0-rebane", 65); text_copy(name[3], "RebaneOS Linux ABI", 65);
        text_copy(name[4], "x86_64", 65); text_copy(name[5], "localdomain", 65);
        return user_copy(name, a, sizeof(name), true) ? 0 : -EFAULT;
    }
    case 72: {
        open_file_t* file = fd_get(a); if (!file) return -EBADF;
        if (b == 0 || b == 1030) return duplicate_fd(a, c, false, b == 1030);
        if (b == 1) return descriptors[a].flags;
        if (b == 2) { descriptors[a].flags = c & 1; return 0; }
        if (b == 3) return file->flags;
        if (b == 4) { file->flags = (file->flags & ~06000U) | (c & 06000); return 0; }
        return -EINVAL;
    }
    case 74: case 75: {
        open_file_t* file = fd_get(a);
        if (!file) return -EBADF;
        return file->file.kind == FILE_REG || file->file.kind == FILE_DIR ? 0 : -EINVAL;
    }
    case 79: {
        size_t length = text_length(current_directory) + 1;
        if (b < length) return -34;
        return user_copy(current_directory, a, length, true) ? (long)length : -EFAULT;
    }
    case 80: {
        char path[PATH_SIZE], normalized[PATH_SIZE]; guest_file_t file;
        if (!user_string(a, path, sizeof(path))) return -EFAULT;
        int result = find_file(path, &file); if (result < 0) return result;
        if (file.kind != FILE_DIR) return -ENOTDIR;
        if (!normalize_path(path, normalized)) return -36;
        text_copy(current_directory, normalized, sizeof(current_directory)); return 0;
    }
    case 89: return read_link(AT_FDCWD, a, b, c);
    case 96:
        if (a && get_time(a, true) < 0) return -EFAULT;
        if (b) { int32_t timezone[2] = {0, 0}; if (!user_copy(timezone, b, sizeof(timezone), true)) return -EFAULT; }
        return 0;
    case 97: case 302: {
        uint64_t resource = number == 97 ? a : b;
        uint64_t output = number == 97 ? b : d;
        if (number == 302 && a && a != session_pid) return -3;
        if (number == 302 && c) return -EPERM;
        if (resource > 15) return -EINVAL;
        uint64_t limit[2] = {UINT64_MAX, UINT64_MAX};
        if (resource == 3) limit[0] = limit[1] = STACK_SIZE;
        if (resource == 7) limit[0] = limit[1] = FD_COUNT;
        if (resource == 9) limit[0] = limit[1] = MAX_MEMORY;
        if (output && !user_copy(limit, output, sizeof(limit), true)) return -EFAULT;
        return 0;
    }
    case 102: case 104: case 107: case 108: return 0;
    case 110: return 1;
    case 111: return session_pid;
    case 121: return session_pid;
    case 158:
        if (a == 0x1002 || a == 0x1001) {
            if (b >= USER_LIMIT) return -EPERM;
            write_msr(a == 0x1002 ? 0xC0000100 : 0xC0000101, b); return 0;
        }
        if (a == 0x1003 || a == 0x1004) {
            uint64_t value = read_msr(a == 0x1003 ? 0xC0000100 : 0xC0000101);
            return user_copy(&value, b, 8, true) ? 0 : -EFAULT;
        }
        return -EINVAL;
    case 201: {
        int64_t seconds = timer_get_ticks() / 100;
        if (a && !user_copy(&seconds, a, 8, true)) return -EFAULT;
        return seconds;
    }
    case 202: {
        unsigned operation = b & 127;
        uint32_t value;
        if (!user_copy(&value, a, 4, false)) return -EFAULT;
        if (operation == 1 || operation == 10) return 0;
        if (operation != 0 && operation != 9) return -ENOSYS;
        if (value != (uint32_t)c) return -EAGAIN;
        if ((a & 3) || (operation == 9 && !f)) return -EINVAL;
        uint64_t deadline = UINT64_MAX;
        if (d) {
            linux_time_t timeout;
            if (!user_copy(&timeout, d, sizeof(timeout), false)) return -EFAULT;
            if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000) return -EINVAL;
            uint64_t ticks = timeout.sec * 100 + (timeout.nsec + 9999999) / 10000000;
            deadline = operation == 9 ? ticks : timer_get_ticks() + ticks;
        }
        for (;;) {
            if (timer_get_ticks() >= deadline) return -ETIMEDOUT;
            task_sleep_ticks(1);
            if (!user_copy(&value, a, 4, false)) return -EFAULT;
            if (value != (uint32_t)c) return 0;
        }
    }
    case 204: {
        if (a && a != session_pid) return -3;
        if (b < 8) return -EINVAL;
        uint64_t mask = 1;
        if (!user_copy(&mask, c, 8, true)) return -EFAULT;
        return 8;
    }
    case 218: clear_tid = a; return session_pid;
    case 228:
        if (a != 0 && a != 1 && a != 4 && a != 5 && a != 6 && a != 7) return -EINVAL;
        return get_time(b, false);
    case 229: {
        if (a > 11) return -EINVAL;
        linux_time_t resolution = {0, 10000000};
        return b ? (user_copy(&resolution, b, sizeof(resolution), true) ? 0 : -EFAULT) : 0;
    }
    case 257: return open_path((long)a, b, c);
    case 262: return stat_path((long)a, b, c, d);
    case 267: return read_link((long)a, b, c, d);
    case 270: return select_descriptors(a, b, c, d, e, true);
    case 271: {
        int64_t milliseconds = -1;
        if (c) {
            linux_time_t timeout;
            if (!user_copy(&timeout, c, sizeof(timeout), false)) return -EFAULT;
            if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000) return -EINVAL;
            milliseconds = timeout.sec * 1000 + (timeout.nsec + 999999) / 1000000;
        }
        return poll_descriptors(a, b, milliseconds);
    }
    case 273: return -ENOSYS;
    case 292:
        if (a == b || (c & ~02000000ULL)) return -EINVAL;
        return duplicate_fd(a, b, true, (c & 02000000) != 0);
    case 318: {
        if (c & ~7ULL) return -EINVAL;
        if (!cpu_has_rdrand) return -ENOSYS;
        if (!user_valid(a, b, true)) return -EFAULT;
        unsigned char buffer[256]; uint64_t total = 0;
        while (total < b) {
            size_t chunk = b - total > sizeof(buffer) ? sizeof(buffer) : b - total;
            if (!fill_entropy(buffer, chunk)) return total ? (long)total : -EAGAIN;
            if (!user_copy(buffer, a + total, chunk, true)) return -EFAULT;
            total += chunk;
        }
        return total;
    }
    case 334: return -ENOSYS;
    default:
        serial_write_string("[linux] unsupported syscall "); serial_write_dec(number);
        serial_write_string("\n");
        return -ENOSYS;
    }
}
void linux_compat_syscall(linux_registers_t* registers) {
    if (!linux_compat_current()) { registers->rax = (uint64_t)-ENOSYS; return; }
    registers->rax = syscall_dispatch(registers->rax, registers->rdi, registers->rsi,
        registers->rdx, registers->r10, registers->r8, registers->r9);
}
