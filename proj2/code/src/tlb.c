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

//returns the entry of tlb_l1 corresponding to the virtual page number
tlb_entry_t* search_in_tlb_l1(va_t virtual_page_number){
  increment_time(TLB_L1_LATENCY_NS);
  for(int index = 0; index < TLB_L1_SIZE; index++){
    if(tlb_l1[index].valid && tlb_l1[index].virtual_page_number==virtual_page_number){
      return &tlb_l1[index];
    }
  }
  return NULL;
}

//returns the 1st entry that is empty
tlb_entry_t* search_space_tlb_l1(){
  for(int index = 0; index < TLB_L1_SIZE; index++){
    if(!tlb_l1[index].valid){
      return &tlb_l1[index];
    }
  }
  return NULL;
}

//returns the LRU entry
tlb_entry_t* do_LRU_tlb_l1(){
  uint64_t min_access = UINT64_MAX;
  tlb_entry_t* entry = NULL;

  for(int index = 0; index < TLB_L1_SIZE; index++){
    if(tlb_l1[index].valid && tlb_l1[index].last_access<min_access){
      min_access = tlb_l1[index].last_access;
      entry = &tlb_l1[index];
    }
  }
  return entry;
}

//sets an entry
void set_tlb_entry(tlb_entry_t* entry, va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access) {
  entry -> valid = true;
  entry -> dirty = false;
  entry -> last_access = last_access;
  entry -> virtual_page_number = virtual_page_number;
  entry -> physical_page_number = physical_page_number;
}


void tlb_invalidate(va_t virtual_page_number) {
  increment_time(TLB_L1_LATENCY_NS);
  tlb_entry_t* tlb_entry = search_in_tlb_l1(virtual_page_number); //search for the entry

  if(tlb_entry){
    tlb_l1_invalidations++; 
    if(tlb_entry->dirty){
      //if its dirty, then it does write back
      va_t physical_address = (tlb_entry->virtual_page_number << PAGE_SIZE_BITS) & VIRTUAL_ADDRESS_MASK;
      write_back_tlb_entry(physical_address);
    }

    tlb_entry->valid = false;
    tlb_entry->dirty = false;
    log_dbg("Invalidated page %" PRIu64 "on Cache L1.", virtual_page_number);
  }

  return;
}


pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
  virtual_address &= VIRTUAL_ADDRESS_MASK;
  va_t virtual_page_offset = virtual_address & PAGE_OFFSET_MASK;
  va_t virtual_page_number = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;

  tlb_entry_t* tlb_entry = search_in_tlb_l1(virtual_page_number);

  pa_dram_t physical_address;

  if(tlb_entry){
    //found in tlb_l1
    tlb_l1_hits++;
    tlb_entry->last_access = tlb_l1_hits + tlb_l1_misses; //updates last_access for LRU implementation

    if(op == OP_WRITE){
      tlb_entry->dirty = true;
    }

    physical_address = (tlb_entry->physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset;

    log_dbg("Cache L1 found (VA=%" PRIx64 " VPN=%" PRIx64 " PA=%" PRIx64 ")",
            virtual_address, virtual_page_number, physical_address);

  }else{
    //not found in tlb_l1
    tlb_l1_misses++;

    physical_address = page_table_translate(virtual_address, op);
    pa_dram_t physical_page_number = (physical_address >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;

    //check for space to insert this in L1
    tlb_entry = search_space_tlb_l1();

    if(tlb_entry){
      //found space to insert
      set_tlb_entry(tlb_entry, virtual_page_number, physical_page_number, tlb_l1_hits + tlb_l1_misses);

    }else{
      //didnt find space, do LRU
      tlb_entry = do_LRU_tlb_l1();

      if (tlb_entry -> dirty) {
        //if it was changed while in tlb, do write back
        va_t replaced_virtual_address = (tlb_entry -> virtual_page_number << PAGE_SIZE_BITS) & VIRTUAL_ADDRESS_MASK;
        write_back_tlb_entry(replaced_virtual_address);
      }

      set_tlb_entry(tlb_entry, virtual_page_number, physical_page_number, tlb_l1_hits + tlb_l1_misses);
    }
  }

  return physical_address;
}