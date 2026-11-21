#include "paging.h"

#include "pmm.h"

#include <stddef.h>

static void clear_page_table(uint64_t* table) {
    for (size_t index = 0; index < PAGING_PAGE_TABLE_ENTRIES; index++) {
        table[index] = 0;
    }
}

static bool page_table_is_empty(const uint64_t* table) {
    for (size_t index = 0; index < PAGING_PAGE_TABLE_ENTRIES; index++) {
        if (table[index]) return false;
    }
    return true;
}

static bool valid_page_address(uintptr_t address) {
    return (address & (PMM_PAGE_SIZE - 1)) == 0;
}

uintptr_t paging_address_space_create(void) {
    uint64_t* pml4 = (uint64_t*)pmm_alloc(1);
    uint64_t* p3 = (uint64_t*)pmm_alloc(1);
    uint64_t* p2 = (uint64_t*)pmm_alloc(1);
    if (!pml4 || !p3 || !p2) {
        if (pml4) pmm_free(pml4, 1);
        if (p3) pmm_free(p3, 1);
        if (p2) pmm_free(p2, 1);
        return 0;
    }

    clear_page_table(pml4);
    clear_page_table(p3);
    clear_page_table(p2);
    pml4[0] = (uintptr_t)p3 | PAGING_PAGE_TABLE_FLAGS;
    p3[0] = (uintptr_t)p2 | PAGING_PAGE_TABLE_FLAGS;
    for (size_t index = 0; index < PAGING_PAGE_TABLE_ENTRIES; index++) {
        p2[index] = (uint64_t)index * 0x200000ULL | PAGING_LARGE_PAGE_FLAGS;
    }
    return (uintptr_t)pml4;
}

bool paging_map_user_page(uintptr_t pml4_address, uintptr_t virtual_address,
    uintptr_t physical_address, bool writable) {
    extern char __kernel_start[], __kernel_end[];
    if (!pml4_address || !valid_page_address(pml4_address) ||
        !valid_page_address(virtual_address) || !valid_page_address(physical_address) ||
        physical_address >= PMM_IDENTITY_MAP_LIMIT ||
        virtual_address >= (1ULL << 47) ||
        (virtual_address < (uintptr_t)__kernel_end &&
         virtual_address + PMM_PAGE_SIZE > (uintptr_t)__kernel_start)) return false;
    uint64_t* table = (uint64_t*)pml4_address;
    const unsigned shifts[] = {39, 30, 21};
    for (unsigned level = 0; level < 3; level++) {
        size_t index = (virtual_address >> shifts[level]) & 511;
        uint64_t entry = table[index];
        if (!(entry & PAGING_PAGE_PRESENT)) {
            uint64_t* child = pmm_alloc(1);
            if (!child) return false;
            clear_page_table(child);
            table[index] = (uintptr_t)child | 7;
        } else if (entry & PAGING_PAGE_SIZE_FLAG) {
            if (level != 2) return false;
            uint64_t* child = pmm_alloc(1);
            if (!child) return false;
            uintptr_t base = entry & 0x000FFFFFFFE00000ULL;
            for (size_t page = 0; page < 512; page++)
                child[page] = (base + page * PMM_PAGE_SIZE) | (entry & 0x7F);
            table[index] = (uintptr_t)child | 7;
        }
        table[index] |= PAGING_PAGE_USER | PAGING_PAGE_WRITABLE;
        table = (uint64_t*)(uintptr_t)(table[index] & 0x000FFFFFFFFFF000ULL);
    }
    size_t index = (virtual_address >> 12) & 511;
    if (table[index] & PAGING_PAGE_USER) return false;
    table[index] = physical_address | 5 | (writable ? 2 : 0);
    return true;
}

bool paging_user_page(uintptr_t root, uintptr_t address, uintptr_t* physical,
    bool* writable) {
    if (!root || address >= (1ULL << 47)) return false;
    uint64_t* table = (uint64_t*)root;
    const unsigned shifts[] = {39, 30, 21, 12};
    bool write = true;
    for (unsigned level = 0; level < 4; level++) {
        uint64_t entry = table[(address >> shifts[level]) & 511];
        if ((entry & 5) != 5 || (level < 3 && (entry & 128))) return false;
        write = write && (entry & 2);
        if (level == 3) {
            if (physical) *physical = (entry & 0x000FFFFFFFFFF000ULL) | (address & 4095);
            if (writable) *writable = write;
            return true;
        }
        table = (uint64_t*)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
    }
    return false;
}

bool paging_protect_user_page(uintptr_t root, uintptr_t address, bool writable) {
    if (!paging_user_page(root, address, NULL, NULL)) return false;
    uint64_t* table = (uint64_t*)root;
    for (unsigned shift = 39; shift > 12; shift -= 9)
        table = (uint64_t*)(uintptr_t)(table[(address >> shift) & 511] & 0x000FFFFFFFFFF000ULL);
    uint64_t* entry = &table[(address >> 12) & 511];
    *entry = (*entry & ~2ULL) | (writable ? 2 : 0);
    return true;
}

bool paging_user_mapping(uintptr_t root, uintptr_t address, uintptr_t* physical,
    unsigned* protection) {
    if (!root || address >= (1ULL << 47)) return false;
    uint64_t* table = (uint64_t*)root;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t entry = table[(address >> shift) & 511];
        if ((entry & 5) != 5 || (entry & 128)) return false;
        table = (uint64_t*)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
    }
    uint64_t entry = table[(address >> 12) & 511];
    if (!(entry & 4)) return false;
    if (physical) *physical = (entry & 0x000FFFFFFFFFF000ULL) | (address & 4095);
    if (protection) *protection = !(entry & 1) ? 0 :
        1 | ((entry & 2) ? 2 : 0) | ((entry >> 63) ? 0 : 4);
    return true;
}

bool paging_set_user_protection(uintptr_t root, uintptr_t address,
    unsigned protection) {
    if (!paging_user_mapping(root, address, NULL, NULL)) return false;
    uint64_t* table = (uint64_t*)root;
    for (unsigned shift = 39; shift > 12; shift -= 9)
        table = (uint64_t*)(uintptr_t)(table[(address >> shift) & 511] & 0x000FFFFFFFFFF000ULL);
    uint64_t* entry = &table[(address >> 12) & 511];
    *entry = (*entry & 0x000FFFFFFFFFF000ULL) | 4 | 0x200 |
        (protection ? 1 : 0) | ((protection & 2) ? 2 : 0) |
        ((protection & 4) ? 0 : (1ULL << 63));
    return true;
}

bool paging_unmap_user_page(uintptr_t pml4_address, uintptr_t virtual_address,
    uintptr_t* physical_address) {
    if (!pml4_address || !valid_page_address(pml4_address) ||
        !valid_page_address(virtual_address) ||
        virtual_address >= (1ULL << 47) || !physical_address) {
        return false;
    }

    uint64_t* pml4 = (uint64_t*)pml4_address;
    size_t pml4_index = (virtual_address >> 39) & 0x1FF;
    size_t p3_index = (virtual_address >> 30) & 0x1FF;
    size_t p2_index = (virtual_address >> 21) & 0x1FF;
    size_t p1_index = (virtual_address >> 12) & 0x1FF;
    if (!(pml4[pml4_index] & PAGING_PAGE_PRESENT) ||
        (pml4[pml4_index] & PAGING_PAGE_SIZE_FLAG)) {
        return false;
    }

    uint64_t* p3 = (uint64_t*)(uintptr_t)(pml4[pml4_index] & ~0xFFFULL);
    if (!(p3[p3_index] & PAGING_PAGE_PRESENT) ||
        (p3[p3_index] & PAGING_PAGE_SIZE_FLAG)) {
        return false;
    }
    uint64_t* p2 = (uint64_t*)(uintptr_t)(p3[p3_index] & ~0xFFFULL);
    if (!(p2[p2_index] & PAGING_PAGE_PRESENT) ||
        (p2[p2_index] & PAGING_PAGE_SIZE_FLAG)) {
        return false;
    }
    uint64_t* p1 = (uint64_t*)(uintptr_t)(p2[p2_index] & ~0xFFFULL);
    uint64_t entry = p1[p1_index];
    if (!(entry & PAGING_PAGE_USER)) {
        return false;
    }

    *physical_address = (uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
    p1[p1_index] = 0;
    if (page_table_is_empty(p1)) {
        p2[p2_index] = 0;
        if (!pmm_free(p1, 1)) return false;
        if (page_table_is_empty(p2)) {
            p3[p3_index] = 0;
            if (!pmm_free(p2, 1)) return false;
        }
    }
    return true;
}

static void destroy_tables(uint64_t* table, unsigned level) {
    if (level > 1) {
        for (size_t index = 0; index < 512; index++) {
            uint64_t entry = table[index];
            if ((entry & 1) && !(entry & 128))
                destroy_tables((uint64_t*)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL), level - 1);
        }
    }
    pmm_free(table, 1);
}

bool paging_address_space_destroy(uintptr_t pml4_address) {
    if (!pml4_address || !valid_page_address(pml4_address) ||
        pml4_address == paging_current_address_space() ||
        pml4_address == paging_kernel_address_space()) return false;
    destroy_tables((uint64_t*)pml4_address, 4);
    return true;
}

void paging_load_address_space(uintptr_t pml4_address) {
    if (!pml4_address || (pml4_address & (PMM_PAGE_SIZE - 1)) != 0) return;
    asm volatile("mov %0, %%cr3" : : "r"(pml4_address) : "memory");
}

uintptr_t paging_current_address_space(void) {
    uintptr_t address;
    asm volatile("mov %%cr3, %0" : "=r"(address));
    return address;
}

static uintptr_t kernel_address_space;
static uintptr_t kernel_mmio_next_virtual = 0xFFFF900000000000ULL;

void paging_init(void) {
    if (!kernel_address_space) {
        kernel_address_space = paging_current_address_space() & ~0xFFFULL;
    }
}

static bool kernel_get_or_create_table(uint64_t* parent, size_t index,
    uint64_t** child) {
    if (parent[index] & PAGING_PAGE_PRESENT) {
        if (parent[index] & PAGING_PAGE_SIZE_FLAG) return false;
        *child = (uint64_t*)(uintptr_t)(parent[index] & ~0xFFFULL);
        return true;
    }

    uint64_t* new_table = (uint64_t*)pmm_alloc(1);
    if (!new_table) return false;
    clear_page_table(new_table);
    parent[index] = (uintptr_t)new_table | PAGING_PAGE_TABLE_FLAGS;
    *child = new_table;
    return true;
}

bool paging_map_kernel_mmio(uintptr_t physical_address, size_t byte_count,
    uintptr_t* virtual_address) {
    if (!physical_address || !byte_count || !virtual_address ||
        physical_address >= (1ULL << 52) ||
        byte_count > (1ULL << 52) - physical_address) {
        return false;
    }

    uint64_t page_offset = physical_address & (PMM_PAGE_SIZE - 1);
    uint64_t physical_page = physical_address & ~(PMM_PAGE_SIZE - 1);
    if ((uint64_t)byte_count > UINT64_MAX - page_offset) return false;
    uint64_t mapped_bytes = page_offset + byte_count;
    uint64_t page_count = (mapped_bytes + PMM_PAGE_SIZE - 1) /
        PMM_PAGE_SIZE;
    uint64_t map_size = page_count * PMM_PAGE_SIZE;
    if (map_size > UINT64_MAX - kernel_mmio_next_virtual) return false;

    paging_init();
    uint64_t* pml4 = (uint64_t*)kernel_address_space;
    uintptr_t map_start = kernel_mmio_next_virtual;
    for (uint64_t page = 0; page < page_count; page++) {
        uintptr_t va = map_start + page * PMM_PAGE_SIZE;
        size_t pml4_index = (va >> 39) & 0x1FF;
        size_t p3_index = (va >> 30) & 0x1FF;
        size_t p2_index = (va >> 21) & 0x1FF;
        size_t p1_index = (va >> 12) & 0x1FF;
        uint64_t* p3;
        uint64_t* p2;
        uint64_t* p1;
        if (!kernel_get_or_create_table(pml4, pml4_index, &p3) ||
            !kernel_get_or_create_table(p3, p3_index, &p2) ||
            !kernel_get_or_create_table(p2, p2_index, &p1) ||
            (p1[p1_index] & PAGING_PAGE_PRESENT)) {
            return false;
        }

        p1[p1_index] = (physical_page + page * PMM_PAGE_SIZE) |
            PAGING_PAGE_PRESENT | PAGING_PAGE_WRITABLE | 0x018ULL;
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    }

    kernel_mmio_next_virtual += map_size;
    *virtual_address = map_start + page_offset;
    return true;
}

uintptr_t paging_kernel_address_space(void) {
    paging_init();
    return kernel_address_space;
}

void paging_restore_kernel_address_space(void) {
    paging_load_address_space(paging_kernel_address_space());
}