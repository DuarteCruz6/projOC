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

//sets an entry
void set_tlb_entry(tlb_entry_t* entry, va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {
  entry -> valid = true;
  entry -> dirty = is_dirty;
  entry -> last_access = last_access;
  entry -> virtual_page_number = virtual_page_number;
  entry -> physical_page_number = physical_page_number;
}

//searches for the entry in L1 -> returns the entry or NULL
tlb_entry_t* search_in_tlb_l1(va_t virtual_page_number){
  increment_time(TLB_L1_LATENCY_NS);
  for(int index = 0; index < TLB_L1_SIZE; index++){
    if(tlb_l1[index].valid && tlb_l1[index].virtual_page_number == virtual_page_number){
      //we found it
      return &tlb_l1[index];
    }
  }
  //we didnt find
  return NULL;
}

//searches for the entry in L2 -> returns the entry or NULL
tlb_entry_t* search_in_tlb_l2(va_t virtual_page_number, bool add_time){
  if (add_time) increment_time(TLB_L2_LATENCY_NS);
  for(int index = 0; index < TLB_L2_SIZE; index++){
    if(tlb_l2[index].valid && tlb_l2[index].virtual_page_number == virtual_page_number){
      //we found it
      return &tlb_l2[index];
    }
  }
  //we didnt find
  return NULL;
}

//invalidates an entry from L1 and L2 as well -> does writeback if dirty
void tlb_invalidate(va_t virtual_page_number) {
  bool dirty = false;
  tlb_entry_t* tlb_entry;
  pa_dram_t replaced_entry;

  tlb_entry = search_in_tlb_l1(virtual_page_number); //search for the entry in L1
  if(tlb_entry){
    //found in L1
    tlb_l1_invalidations++; 
    replaced_entry = ((tlb_entry->physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
    tlb_entry->valid = false;
    dirty = tlb_entry->dirty;
    tlb_entry->dirty = false;
  }

  bool add_time = true;
  tlb_entry = search_in_tlb_l2(virtual_page_number, add_time); //search for the entry in L2

  if(tlb_entry){
    //found in L2
    tlb_l2_invalidations++; 
    replaced_entry = ((tlb_entry->physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
    tlb_entry->valid = false;
    dirty = (tlb_entry->dirty || dirty);
    tlb_entry->dirty = false;
  }

  //if dirty, then it does write back
  if((dirty)){
    write_back_tlb_entry(replaced_entry);
    log_dbg("***** TLB L2 write back PPN=%" PRIu64 " VPN=%" PRIu64 " *****",tlb_entry->physical_page_number,tlb_entry->virtual_page_number);
  }

}

//adds an entry to the tlb
void add_entry_to_tlb(bool is_L1, tlb_entry_t* tlb_empty_entry, tlb_entry_t* tlb_LRU_entry,
                      va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool is_dirty) {

  if (tlb_empty_entry) {
    //there was an empty entry
    set_tlb_entry(tlb_empty_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
  else {
    //no empty space -> LRU

    if (tlb_LRU_entry -> dirty) {

      if (is_L1) {
        //LRU entry is dirty and from L1, so we need to update the L2 entry's dirty bit
        bool add_time = false;
        search_in_tlb_l2(tlb_LRU_entry->virtual_page_number, add_time)->dirty = true;

      } else {
        //LRU entry is dirty and from L2, so we need to write-back the entry
        pa_dram_t replaced_entry = ((tlb_LRU_entry -> physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
        write_back_tlb_entry(replaced_entry);
        log_dbg("***** TLB L2 write back PPN=%"PRIu64 " VPN=%" PRIu64 " *****",tlb_LRU_entry->physical_page_number,tlb_LRU_entry->virtual_page_number);

      }
    }

    set_tlb_entry(tlb_LRU_entry, virtual_page_number, physical_page_number, last_access, is_dirty);
  }
}

//gets the empty and LRU entry from L1 
void get_empty_and_LRU_l1(tlb_entry_t** tlb_l1_empty_entry, tlb_entry_t** tlb_l1_LRU_entry){
  for (size_t i = 0; i < TLB_L1_SIZE; i++){
    if (!*tlb_l1_empty_entry) {
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
}

//gets the empty and LRU entry from L2
void get_empty_and_LRU_l2(tlb_entry_t** tlb_l2_empty_entry, tlb_entry_t** tlb_l2_LRU_entry){
  for (size_t i = 0; i < TLB_L2_SIZE; i++){

    if (!*tlb_l2_empty_entry) {
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
}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
  virtual_address &= VIRTUAL_ADDRESS_MASK;
  va_t virtual_page_number = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  va_t virtual_page_offset = virtual_address & PAGE_OFFSET_MASK;

  tlb_entry_t* tlb_entry;
  pa_dram_t physical_address;
  pa_dram_t physical_add;
  pa_dram_t physical_page_number;
  bool is_dirty = (op == OP_WRITE);

  //search in L1
  tlb_entry = search_in_tlb_l1(virtual_page_number);

  uint64_t n_access_l1 = tlb_l1_hits + tlb_l1_misses + 1; // +1 due to the new miss or hit 

  if(tlb_entry){
    //we found it in L1
    tlb_l1_hits++;
    tlb_entry->last_access = n_access_l1;
    if(!tlb_entry->dirty){
      //updates the dirty bit if op=write and it wasnt dirty before
      tlb_entry->dirty = (op==OP_WRITE);
    }

    physical_address = (tlb_entry->physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset;
    return physical_address;
  }

  //not found in L1
  tlb_l1_misses++;

  tlb_entry_t* tlb_l1_empty_entry = NULL;
  tlb_entry_t* tlb_l1_LRU_entry = NULL;
  get_empty_and_LRU_l1(&tlb_l1_empty_entry, &tlb_l1_LRU_entry); //puts the pointers to the correct entries

  //search in L2
  bool add_time = true;
  tlb_entry = search_in_tlb_l2(virtual_page_number,add_time);
  uint64_t n_access_l2 = tlb_l2_hits + tlb_l2_misses + 1; //+1 due to the new miss or hit 

  bool is_l1;

  if(tlb_entry){
    //we found it in L2
    //log_clk("found in L2");
    tlb_l2_hits++;
    tlb_entry->last_access = n_access_l2;
    if(!tlb_entry->dirty){
      //updates the dirty bit if op=write and it wasnt dirty before
      tlb_entry->dirty = (op==OP_WRITE);
    }
    
    //put it on L1
    is_l1 = true;
    add_entry_to_tlb(is_l1, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, tlb_entry->physical_page_number, n_access_l1, is_dirty);

    physical_address = (tlb_entry->physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset;
    return physical_address;

  }

  //not found in L1 nor L2
  tlb_l2_misses++;

  tlb_entry_t* tlb_l2_empty_entry = NULL;
  tlb_entry_t* tlb_l2_LRU_entry = NULL;
  get_empty_and_LRU_l2(&tlb_l2_empty_entry, &tlb_l2_LRU_entry); //puts the pointers to the correct entries

  physical_add = page_table_translate(virtual_address, op) & DRAM_ADDRESS_MASK;
  physical_page_number = (physical_add >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;

  //adds the entry to L2
  is_l1 = false;
  add_entry_to_tlb(is_l1, tlb_l2_empty_entry, tlb_l2_LRU_entry, virtual_page_number, physical_page_number, n_access_l2, is_dirty);
  //adds the entry to L1
  is_l1 = true;
  add_entry_to_tlb(is_l1, tlb_l1_empty_entry, tlb_l1_LRU_entry, virtual_page_number, physical_page_number, n_access_l1, is_dirty);

  return physical_add;
}
