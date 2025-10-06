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

uint64_t tlb_access = 0;


/**
 * @brief Initializes all TLB entries (L1 and L2) and resets statistics.
 */
void tlb_init() {
  memset(tlb_l1, 0, sizeof(tlb_l1));
  memset(tlb_l2, 0, sizeof(tlb_l2));
  tlb_l1_hits = 0;
  tlb_l1_misses = 0;
  tlb_l1_invalidations = 0;
  tlb_l2_hits = 0;
  tlb_l2_misses = 0;
  tlb_l2_invalidations = 0;
  tlb_access = 0;
}


/**
 * @brief Sets the fields of a TLB entry with the given translation data.
 *
 * @param entry Pointer to the TLB entry to update
 * @param virtual_page_number Virtual page number of the translation
 * @param physical_page_number Physical page number of the translation
 * @param last_access Last access counter for LRU tracking
 * @param is_dirty True if the entry corresponds to a write, False otherwise
 */
void set_tlb_entry(tlb_entry_t* entry, va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {
  entry -> valid = true;
  entry -> dirty = is_dirty;
  entry -> last_access = last_access;
  entry -> virtual_page_number = virtual_page_number;
  entry -> physical_page_number = physical_page_number;
}


/**
 * @brief Searches for a valid entry in the TLB with the given virtual page number (VPN).
 *
 * @param tlb Array of TLB entries
 * @param size Number of entries in the TLB
 * @param virtual_page_number Virtual page number to search
 * @return Pointer to the TLB entry if found, NULL otherwise
 */
tlb_entry_t* get_entry(tlb_entry_t tlb[], uint64_t size, va_t virtual_page_number) {

  for (size_t i = 0; i < size; i++)
  {
    if (tlb[i].valid && tlb[i].virtual_page_number == virtual_page_number) {

      return &tlb[i];
    }
  }

  return NULL;
}


/**
 * @brief Invalidates an entry in L1 or L2 TLBs for the given VPN.
 *
 * - If found in L1:
 *   - Marks the entry as invalid
 *   - Increments @c tlb_l1_invalidations
 *   - If dirty, schedules a write-back
 *
 * - If found in L2:
 *   - Marks the entry as invalid
 *   - Increments @c tlb_l2_invalidations
 *   - If dirty, schedules a write-back
 *
 * @param virtual_page_number VPN of the entry to invalidate
 */
void tlb_invalidate(va_t virtual_page_number) {

  pa_dram_t replaced_entry;

  // Invalidate from cache L1
  increment_time(TLB_L1_LATENCY_NS);
  tlb_entry_t* l1_entry = get_entry(tlb_l1, TLB_L1_SIZE, virtual_page_number);
  
  if (l1_entry) {

    l1_entry -> valid = false;
    tlb_l1_invalidations++;
    
    if (l1_entry -> dirty) {
      // Write back
      replaced_entry = (l1_entry -> physical_page_number << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
      write_back_tlb_entry(replaced_entry);
    }
    
    log_dbg("Invalidated page %" PRIu64 " on Cache L1.", virtual_page_number);
    //return;
  }

  // Invalidate from cache L2
  increment_time(TLB_L2_LATENCY_NS);
  tlb_entry_t* l2_entry = get_entry(tlb_l2, TLB_L2_SIZE, virtual_page_number);

  if (l2_entry) {

    l2_entry -> valid = false;
    tlb_l2_invalidations++;

    if (l2_entry -> dirty) {
      // Write back
      replaced_entry = (l2_entry -> physical_page_number << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
      write_back_tlb_entry(replaced_entry);
    }

    log_dbg("Invalidated page %" PRIu64 " on Cache L2.", virtual_page_number);
    
  }
}

void get_to_replace_entry_tlb_l2(tlb_entry_t** tlb_l2_empty_entry, tlb_entry_t** tlb_l2_LRU_entry){
  // Search for empty entry and LRU
  for (size_t i = 0; i < TLB_L2_SIZE; i++)
  {
    // Get empty entry
    if (!tlb_l2[i].valid) {
      *tlb_l2_empty_entry = &tlb_l2[i];
      break;
    }
    
    // Get oldest access entry
    else if (!*tlb_l2_LRU_entry || tlb_l2[i].last_access < (*tlb_l2_LRU_entry) -> last_access) {
      *tlb_l2_LRU_entry = &tlb_l2[i];
    }
  }
}


/**
 * @brief Adds a new translation to the TLB.
 *
 * If an empty entry is available, it is used. Otherwise, the Least Recently Used (LRU) entry is replaced.
 * - If replacing an L1 entry, puts it to L2.
 * - If replacing an L2 entry that is dirty, writes back to memory.
 *
 * @param is_L1 True if adding to L1 TLB, False if adding to L2 TLB
 * @param tlb_empty_entry Pointer to an empty TLB entry, or NULL if none
 * @param tlb_LRU_entry Pointer to the LRU TLB entry
 * @param virtual_page_number VPN of the translation
 * @param physical_page_number PPN of the translation
 * @param last_access Last access counter for the entry
 * @param is_dirty True if the operation was a write, False if a read
 */
void add_entry_to_tlb(bool is_L1, tlb_entry_t* tlb_empty_entry, tlb_entry_t* tlb_LRU_entry,
                      va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {

  if (tlb_empty_entry) {
    set_tlb_entry(tlb_empty_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
  else {
    // Needs to replace LRU entry

    if(is_L1){
      //puts the L1 LRU entry into L2

      va_t old_vpn = tlb_LRU_entry->virtual_page_number;
      pa_dram_t old_ppn = tlb_LRU_entry->physical_page_number;
      uint64_t old_last_access = tlb_LRU_entry->last_access;
      bool old_dirty = tlb_LRU_entry->dirty;

      set_tlb_entry(tlb_LRU_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
      
      tlb_entry_t* tlb_l2_empty_entry = NULL;
      tlb_entry_t* tlb_l2_LRU_entry = NULL;
      get_to_replace_entry_tlb_l2(&tlb_l2_empty_entry,&tlb_l2_LRU_entry);

      add_entry_to_tlb(false, tlb_l2_empty_entry, tlb_l2_LRU_entry, old_vpn, 
                      old_ppn, old_last_access, old_dirty);

      log_dbg("Evicting TLB L1 entry to TLB L2 (VPN=%" PRIx64 " PPN=%" PRIx64 " dirty=%d)",
            tlb_LRU_entry->virtual_page_number, tlb_LRU_entry->physical_page_number, tlb_LRU_entry->dirty);

      return;


    }else{
      //checks if the L2 LRU entry is dirty and does write back if necessary

      log_dbg("Evicting TLB L2 entry (VPN=%" PRIx64 " PPN=%" PRIx64 " dirty=%d)",
            tlb_LRU_entry->virtual_page_number, tlb_LRU_entry->physical_page_number, tlb_LRU_entry->dirty);


      if(tlb_LRU_entry->dirty){
        pa_dram_t replaced_entry = ((tlb_LRU_entry -> physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
        write_back_tlb_entry(replaced_entry);
        log_dbg("***** TLB L2 write back *****")
      }
    }

    set_tlb_entry(tlb_LRU_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
}


/**
 * @brief Searches for an entry in the L1 TLB matching the given VPN.
 *
 * - If found:
 *   - Increments @c tlb_l1_hits
 *   - Updates the entry's last access counter
 *   - Sets the dirty bit if the operation is a write
 *   - Returns the translated physical address
 *
 * - If not found:
 *   - Increments @c tlb_l1_misses
 *   - Identifies an empty entry (if any) and the LRU entry
 *
 * @param virtual_address Full virtual address to translate
 * @param virtual_page_number VPN of the translation
 * @param virtual_page_offset Offset within the page
 * @param op Operation type (Read or Write)
 * @param n_access Current access counter
 * @param tlb_l1_empty_entry Output pointer to an empty entry, or NULL if none
 * @param tlb_l1_LRU_entry Output pointer to the LRU entry
 * @param success Output flag, true if found, false otherwise
 * @return Translated physical address if found, 0 otherwise
 */
pa_dram_t search_tlb_l1(va_t virtual_address, va_t virtual_page_number, va_t virtual_page_offset, op_t op, uint64_t n_access,
                        tlb_entry_t** tlb_l1_empty_entry, tlb_entry_t** tlb_l1_LRU_entry, bool* success) {

  increment_time(TLB_L1_LATENCY_NS);
  tlb_entry_t* l1_entry = get_entry(tlb_l1, TLB_L1_SIZE, virtual_page_number);

  // If found in TLB
  if (l1_entry) {

    tlb_l1_hits++;
    l1_entry -> last_access = n_access;

    if (op == OP_WRITE) {
      l1_entry -> dirty = true;
    }

    pa_dram_t translated_address = ((l1_entry -> physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset) & DRAM_ADDRESS_MASK;
    log_dbg("Cache L1 found (VA=%" PRIx64 " VPN=%" PRIx64 " PA=%" PRIx64 ")",
            virtual_address, virtual_page_number, translated_address);

    *success = true;
    return translated_address;
  }

  // Search for empty entry and LRU
  for (size_t i = 0; i < TLB_L1_SIZE; i++)
  {
    // Get empty entry
    if (!tlb_l1[i].valid) {
      *tlb_l1_empty_entry = &tlb_l1[i];
      break;
    }

    // Get oldest access entry
    else if (!*tlb_l1_LRU_entry || tlb_l1[i].last_access < (*tlb_l1_LRU_entry) -> last_access) {
      *tlb_l1_LRU_entry = &tlb_l1[i];
    }
  }

  tlb_l1_misses++;
  *success = false;
  return 0;
}

/**
 * @brief Searches for an entry in the L2 TLB matching the given VPN.
 *
 * - If found:
 *   - Increments @c tlb_l2_hits
 *   - Removes the entry from TLB L2
 *   - Returns the translated physical address
 *
 * - If not found:
 *   - Increments @c tlb_l2_misses
 *   - Identifies an empty entry (if any) and the LRU entry
 *
 * @param virtual_address Full virtual address to translate
 * @param virtual_page_number VPN of the translation
 * @param virtual_page_offset Offset within the page
 * @param op Operation type (Read or Write)
 * @param n_access Current access counter
 * @param tlb_l2_empty_entry Output pointer to an empty entry, or NULL if none
 * @param tlb_l2_LRU_entry Output pointer to the LRU entry
 * @param success Output flag, true if found, false otherwise
 * @param is_dirty Output flag, true if the found entry is dirty
 * @return Translated physical address if found, 0 otherwise
 */
pa_dram_t search_tlb_l2(va_t virtual_address, va_t virtual_page_number, va_t virtual_page_offset, op_t op, uint64_t n_access,
                        tlb_entry_t** tlb_l2_empty_entry, tlb_entry_t** tlb_l2_LRU_entry, bool* success, bool* is_dirty) {

  increment_time(TLB_L2_LATENCY_NS);
  tlb_entry_t* l2_entry = get_entry(tlb_l2, TLB_L2_SIZE, virtual_page_number);

  // If found in TLB
  if (l2_entry) {

    tlb_l2_hits++;
    l2_entry -> last_access = n_access;

    if (op == OP_WRITE) {
      l2_entry -> dirty = true;
    }

    pa_dram_t translated_address = ((l2_entry -> physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset) & DRAM_ADDRESS_MASK;
    log_dbg("Cache L2 found (VA=%" PRIx64 " VPN=%" PRIx64 " PA=%" PRIx64 ")",
            virtual_address, virtual_page_number, translated_address);

    *success = true;
    *is_dirty = l2_entry -> dirty;
    l2_entry->valid = false; //removes the entry from L2
    return translated_address;
  }

  // Search for empty entry and LRU
  for (size_t i = 0; i < TLB_L2_SIZE; i++)
  {
    // Get empty entry
    if (!tlb_l2[i].valid) {
      *tlb_l2_empty_entry = &tlb_l2[i];
      break;
    }
    
    // Get oldest access entry
    else if (!*tlb_l2_LRU_entry || tlb_l2[i].last_access < (*tlb_l2_LRU_entry) -> last_access) {
      *tlb_l2_LRU_entry = &tlb_l2[i];
    }
  }

  tlb_l2_misses++;
  *success = false;
  return 0;
}


/**
 * @brief Translates a virtual address through the TLB hierarchy (L1 → L2 → Page Table).
 *
 * - If found in L1:
 *   - Returns the physical address immediately
 *
 * - If found in L2 but not L1:
 *   - Promotes the entry to L1
 *   - Removes from L2
 *   - Returns the physical address
 *
 * - If not found in either:
 *   - Translates using the page table
 *   - Inserts the new entry into L1
 *   - Returns the physical address
 *
 * If the operation is a write, the dirty bit is set in the corresponding entry.
 *
 * @param virtual_address Virtual address to translate
 * @param op Operation type (Read or Write)
 * @return Translated physical address
 */
pa_dram_t tlb_translate(va_t virtual_address, op_t op) {

  pa_dram_t physical_add;
  pa_dram_t physical_page_number;

  virtual_address &= VIRTUAL_ADDRESS_MASK;
  va_t virtual_page_offset = virtual_address & PAGE_OFFSET_MASK;
  va_t virtual_page_number = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  bool success = false;
  bool is_dirty = (op == OP_WRITE);

  // Check in Cache L1

  uint64_t n_access = tlb_l1_hits + tlb_l1_misses + tlb_l2_hits + tlb_l2_misses + 1;
  tlb_entry_t* tlb_l1_empty_entry = NULL;
  tlb_entry_t* tlb_l1_LRU_entry = NULL;

  physical_add = search_tlb_l1(virtual_address, virtual_page_number, virtual_page_offset, 
    op, n_access, &tlb_l1_empty_entry, &tlb_l1_LRU_entry, &success);

  if (success)
    return physical_add;

  // Check in Cache L2

  n_access += 1;
  tlb_entry_t* tlb_l2_empty_entry = NULL;
  tlb_entry_t* tlb_l2_LRU_entry = NULL;

  physical_add = search_tlb_l2(virtual_address, virtual_page_number, virtual_page_offset, 
    op, n_access, &tlb_l2_empty_entry, &tlb_l2_LRU_entry, &success, &is_dirty);

  if (success) {
    // If there is a hit on L2 but a miss on L1, we add the entry to L1 and we already removed it from L2
    physical_page_number = (physical_add >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;
    add_entry_to_tlb(true, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, physical_page_number, n_access, is_dirty);

    return physical_add;
  }

  // Search in Page Table and add to both caches

  physical_add = page_table_translate(virtual_address, op) & DRAM_ADDRESS_MASK;
  physical_page_number = (physical_add >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;

  //add_entry_to_tlb(false, tlb_l2_empty_entry, tlb_l2_LRU_entry, virtual_page_number, physical_page_number, n_access, is_dirty);  
  add_entry_to_tlb(true, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, physical_page_number, n_access, is_dirty);
  

  return physical_add;
}
