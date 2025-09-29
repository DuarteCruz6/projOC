#include "tlb.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "constants.h"
#include "log.h"
#include "memory.h"
#include "page_table.h"

#define DRAM_PAGES (uint64_t)(1llu << (DRAM_ADDRESS_BITS - PAGE_SIZE_BITS))
#define PHYSICAL_PAGE_NUMBER_MASK (DRAM_PAGES - 1)

typedef struct {
  bool valid;
  bool dirty;
  uint64_t last_access;
  va_t virtual_page_number;
  pa_dram_t physical_page_number;
} tlb_entry_t;

tlb_entry_t tlb_l1[TLB_L1_SIZE];
tlb_entry_t tlb_l2[TLB_L2_SIZE];

uint64_t tlb_l1_hits = 0;
uint64_t tlb_l1_misses = 0;
uint64_t tlb_l1_invalidations = 0;

uint64_t tlb_l2_hits = 0;
uint64_t tlb_l2_misses = 0;
uint64_t tlb_l2_invalidations = 0;

uint64_t get_total_tlb_l1_hits() { return tlb_l1_hits; }
uint64_t get_total_tlb_l1_misses() { return tlb_l1_misses; }
uint64_t get_total_tlb_l1_invalidations() { return tlb_l1_invalidations; }

uint64_t get_total_tlb_l2_hits() { return tlb_l2_hits; }
uint64_t get_total_tlb_l2_misses() { return tlb_l2_misses; }
uint64_t get_total_tlb_l2_invalidations() { return tlb_l2_invalidations; }

void tlb_init() {
  memset(tlb_l1, 0, sizeof(tlb_l1));
  memset(tlb_l2, 0, sizeof(tlb_l2));
  tlb_l1_hits = 0;
  tlb_l1_misses = 0;
  tlb_l1_invalidations = 0;
  tlb_l2_hits = 0;
  tlb_l2_misses = 0;
  tlb_l2_invalidations = 0;
}

void tlb_invalidate(va_t virtual_page_number) {

  bool is_dirty = false;
  pa_dram_t replaced_entry;

  // Invalidate from cache L1
  increment_time(TLB_L1_LATENCY_NS);

  // search for the entry with the correct virtual page number
  for (size_t i = 0; i < TLB_L1_SIZE; i++)
  {
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {

      tlb_l1[i].valid = false;
      tlb_l1_invalidations++;
      
      if (tlb_l1[i].dirty) {

        is_dirty = true;
        replaced_entry = (tlb_l1[i].physical_page_number << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
      }
      
      log_dbg("Invalidated page %" PRIu64 " on Cache L1.", virtual_page_number);
      break;
    }
  }

  // Invalidate from cache L2
  increment_time(TLB_L2_LATENCY_NS);

  // search for the entry with the correct virtual page number
  for (size_t i = 0; i < TLB_L2_SIZE; i++)
  {
    if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == virtual_page_number) {

      tlb_l2[i].valid = false;
      tlb_l2_invalidations++;
      
      if (tlb_l2[i].dirty && !is_dirty) {

        is_dirty = true;
        replaced_entry = (tlb_l2[i].physical_page_number << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
      }
      
      log_dbg("Invalidated page %" PRIu64 " on Cache L2.", virtual_page_number);
      break;
    }
  }
  
  if (is_dirty)
    write_back_tlb_entry(replaced_entry);
}

void set_tlb_entry(tlb_entry_t* entry, va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {
  entry -> valid = true;
  entry -> dirty = is_dirty;
  entry -> last_access = last_access;
  entry -> virtual_page_number = virtual_page_number;
  entry -> physical_page_number = physical_page_number;
}

void add_entry_to_tlb(bool is_L1, tlb_entry_t* tlb_empty_entry, tlb_entry_t* tlb_LRU_entry,
                      va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {

  if (tlb_empty_entry) {
    set_tlb_entry(tlb_empty_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
  else {

    if (tlb_LRU_entry -> dirty) {
      
      if (is_L1) {

        for (size_t i = 0; i < TLB_L2_SIZE; i++)
        {
          if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == tlb_LRU_entry -> virtual_page_number) {

            tlb_l2[i].dirty = true;
            break;
          }
        }
      } else {
        
        pa_dram_t replaced_entry = ((tlb_LRU_entry -> physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
        write_back_tlb_entry(replaced_entry);
      }
    }

    set_tlb_entry(tlb_LRU_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
}

pa_dram_t search_tlb_l1(va_t virtual_address, va_t virtual_page_number, va_t virtual_page_offset, op_t op, uint64_t tlb_l1_n_access,
                        tlb_entry_t** tlb_l1_empty_entry, tlb_entry_t** tlb_l1_LRU_entry, bool* success) {

  increment_time(TLB_L1_LATENCY_NS);

  for (size_t i = 0; i < TLB_L1_SIZE; i++)
  {
    // Found page in TLB
    if (tlb_l1[i].valid && tlb_l1[i].virtual_page_number == virtual_page_number) {

      tlb_l1_hits++;
      tlb_l1[i].last_access = tlb_l1_n_access;

      if (op == OP_WRITE) {
        tlb_l1[i].dirty = true;
      }

      pa_dram_t translated_address = ((tlb_l1[i].physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset) & DRAM_ADDRESS_MASK;
      log_dbg("Cache L1 found (VA=%" PRIx64 " VPN=%" PRIx64 " PA=%" PRIx64 ")",
              virtual_address, virtual_page_number, translated_address);

      *success = true;
      return translated_address;
    }

    else if (!*tlb_l1_empty_entry) {
      // Get empty entry
      if (!tlb_l1[i].valid) {
        *tlb_l1_empty_entry = &tlb_l1[i];
      }
      // Get oldest access entry
      else if (!*tlb_l1_LRU_entry || tlb_l1[i].last_access < (*tlb_l1_LRU_entry) -> last_access) {
        *tlb_l1_LRU_entry = &tlb_l1[i];
      }
    }
  }

  tlb_l1_misses++;
  *success = false;
  return 0;
}

pa_dram_t search_tlb_l2(va_t virtual_address, va_t virtual_page_number, va_t virtual_page_offset, op_t op, uint64_t tlb_l2_n_access,
                        tlb_entry_t** tlb_l2_empty_entry, tlb_entry_t** tlb_l2_LRU_entry, bool* success, bool* is_dirty) {

  increment_time(TLB_L2_LATENCY_NS);

  for (size_t i = 0; i < TLB_L2_SIZE; i++)
  {
    // Found page in TLB
    if (tlb_l2[i].valid && tlb_l2[i].virtual_page_number == virtual_page_number) {

      tlb_l2_hits++;
      tlb_l2[i].last_access = tlb_l2_n_access;

      if (op == OP_WRITE) {
        tlb_l2[i].dirty = true;
      }

      pa_dram_t translated_address = ((tlb_l2[i].physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset) & DRAM_ADDRESS_MASK;
      log_dbg("Cache L2 found (VA=%" PRIx64 " VPN=%" PRIx64 " PA=%" PRIx64 ")",
              virtual_address, virtual_page_number, translated_address);

      *success = true;
      *is_dirty = tlb_l2[i].dirty;
      return translated_address;
    }

    else if (!*tlb_l2_empty_entry) {
      // Get empty entry
      if (!tlb_l2[i].valid) {
        *tlb_l2_empty_entry = &tlb_l2[i];
      }
      // Get oldest access entry
      else if (!*tlb_l2_LRU_entry || tlb_l2[i].last_access < (*tlb_l2_LRU_entry) -> last_access) {
        *tlb_l2_LRU_entry = &tlb_l2[i];
      }
    }
  }

  tlb_l2_misses++;
  *success = false;
  return 0;
}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {

  pa_dram_t physical_add;
  pa_dram_t physical_page_number;

  virtual_address &= VIRTUAL_ADDRESS_MASK;
  va_t virtual_page_offset = virtual_address & PAGE_OFFSET_MASK;
  va_t virtual_page_number = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  bool success = false;
  bool is_dirty = (op == OP_WRITE);

  // Check in Cache L1

  uint64_t tlb_l1_n_access = tlb_l1_hits + tlb_l1_misses + 1;
  tlb_entry_t* tlb_l1_empty_entry = NULL;
  tlb_entry_t* tlb_l1_LRU_entry = NULL;

  physical_add = search_tlb_l1(virtual_address, virtual_page_number, virtual_page_offset, 
    op, tlb_l1_n_access, &tlb_l1_empty_entry, &tlb_l1_LRU_entry, &success);

  if (success)
    return physical_add;

  // Check in Cache L2

  uint64_t tlb_l2_n_access = tlb_l2_hits + tlb_l2_misses + 1;
  tlb_entry_t* tlb_l2_empty_entry = NULL;
  tlb_entry_t* tlb_l2_LRU_entry = NULL;

  physical_add = search_tlb_l2(virtual_address, virtual_page_number, virtual_page_offset, 
    op, tlb_l2_n_access, &tlb_l2_empty_entry, &tlb_l2_LRU_entry, &success, &is_dirty);

  if (success) {
    // If there is a hit on L2 but a miss on L1, we add the entry to L1
    physical_page_number = (physical_add >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;
    add_entry_to_tlb(true, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, physical_page_number, tlb_l1_n_access, is_dirty);

    return physical_add;
  }

  // Search in Page Table and add to both caches

  physical_add = page_table_translate(virtual_address, op) & DRAM_ADDRESS_MASK;
  physical_page_number = (physical_add >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;

  add_entry_to_tlb(false, tlb_l2_empty_entry, tlb_l2_LRU_entry, virtual_page_number, physical_page_number, tlb_l2_n_access, is_dirty);
  add_entry_to_tlb(true, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, physical_page_number, tlb_l1_n_access, is_dirty);

  return physical_add;
}
