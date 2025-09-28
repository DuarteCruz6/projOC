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

uint64_t calculate_last_access(){
  return tlb_l1_hits + tlb_l1_misses + tlb_l2_hits + tlb_l2_misses;
}

//sets an entry
void set_tlb_entry(tlb_entry_t* entry, va_t virtual_page_number, pa_dram_t physical_page_number, uint64_t last_access, bool dirty) {
  entry -> valid = true;
  entry -> dirty = dirty;
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
tlb_entry_t* search_in_tlb_l2(va_t virtual_page_number){
  increment_time(TLB_L2_LATENCY_NS);
  for(int index = 0; index < TLB_L2_SIZE; index++){
    if(tlb_l2[index].valid && tlb_l2[index].virtual_page_number == virtual_page_number){
      //we found it
      return &tlb_l2[index];
    }
  }
  //we didnt find
  return NULL;
}

//returns the 1st entry that is empty in L1
tlb_entry_t* search_space_tlb_l1(){
  for(int index = 0; index < TLB_L1_SIZE; index++){
    if(!tlb_l1[index].valid){
      return &tlb_l1[index];
    }
  }
  return NULL;
}

//returns the 1st entry that is empty in L2
tlb_entry_t* search_space_tlb_l2(){
  for(int index = 0; index < TLB_L2_SIZE; index++){
    if(!tlb_l2[index].valid){
      return &tlb_l2[index];
    }
  }
  return NULL;
}

//returns the LRU entry in L1
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

//returns the LRU entry in L2
tlb_entry_t* do_LRU_tlb_l2(){
  uint64_t min_access = UINT64_MAX;
  tlb_entry_t* entry = NULL;

  for(int index = 0; index < TLB_L2_SIZE; index++){
    if(tlb_l2[index].valid && tlb_l2[index].last_access<min_access){
      min_access = tlb_l2[index].last_access;
      entry = &tlb_l2[index];
    }
  }
  return entry;
}


//puts a new entry on L2
void put_on_tlb_l2(tlb_entry_t* entry){
  tlb_entry_t* entry_to_put_on = search_space_tlb_l2();
  
  if(entry_to_put_on){
    //there was space left
    set_tlb_entry(entry_to_put_on,entry->virtual_page_number,entry->physical_page_number,entry->last_access,entry->dirty);
    return;
  }

  //there was no space left -> do LRU
  entry_to_put_on = do_LRU_tlb_l2();

  //does write-back if dirty
  if(entry_to_put_on->dirty){
    pa_dram_t entry_old_address = ((entry_to_put_on->physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
    write_back_tlb_entry(entry_old_address);
  }

  //updates the entry with the new values
  set_tlb_entry(entry_to_put_on,entry->virtual_page_number,entry->physical_page_number,entry->last_access,entry->dirty);
  
}

//puts from L2 to L1
void pass_to_tlb_l1(tlb_entry_t* entry){
  tlb_entry_t* entry_to_put_on = search_space_tlb_l1();

  if(entry_to_put_on){
    //there was space left
    set_tlb_entry(entry_to_put_on,entry->virtual_page_number,entry->physical_page_number,entry->last_access,entry->dirty);
    return;
  }

  //there was no space left -> do LRU
  entry_to_put_on = do_LRU_tlb_l1();

  //updates the entry with the new values
  set_tlb_entry(entry_to_put_on,entry->virtual_page_number,entry->physical_page_number,entry->last_access,entry->dirty);

}

//creates a new entry on L1
tlb_entry_t* create_in_tlb_l1(op_t op, va_t virtual_page_number, pa_dram_t physical_page_number){
  tlb_entry_t* entry_to_put_on = search_space_tlb_l1();

  if(entry_to_put_on){
    //there was space left
    set_tlb_entry(entry_to_put_on,virtual_page_number,physical_page_number,calculate_last_access(),(op==OP_WRITE));
    return entry_to_put_on;
  }

  //there was no space left -> do LRU
  entry_to_put_on = do_LRU_tlb_l1();

  //updates the entry with the new values
  set_tlb_entry(entry_to_put_on,virtual_page_number,physical_page_number,calculate_last_access(),(op==OP_WRITE));
  
  return entry_to_put_on;
}

//invalidates an entry from L1 and L2 as well -> does writeback if dirty
void tlb_invalidate(va_t virtual_page_number) {
  bool dirty_l1 = false;
  tlb_entry_t* tlb_entry;

  tlb_entry = search_in_tlb_l1(virtual_page_number); //search for the entry in L1
  if(tlb_entry){
    //found in L1
    tlb_l1_invalidations++; 
    tlb_entry->valid = false;
    dirty_l1 = tlb_entry->dirty;
    tlb_entry->dirty = false;
  }

  tlb_entry = search_in_tlb_l2(virtual_page_number); //search for the entry in L2
  if(tlb_entry){
    //found in L2
    tlb_l2_invalidations++; 
    tlb_entry->valid = false;

    //if dirty, then it does write back
    if((tlb_entry->dirty || dirty_l1)){
      va_t replaced_entry = ((tlb_entry->physical_page_number) << PAGE_SIZE_BITS) & DRAM_ADDRESS_MASK;
      write_back_tlb_entry(replaced_entry);
    }

    tlb_entry->dirty = false;
  }

}

pa_dram_t tlb_translate(va_t virtual_address, op_t op) {
  virtual_address &= VIRTUAL_ADDRESS_MASK;
  va_t virtual_page_number = (virtual_address >> PAGE_SIZE_BITS) & PAGE_INDEX_MASK;
  va_t virtual_page_offset = virtual_address & PAGE_OFFSET_MASK;

  tlb_entry_t* tlb_entry;
  pa_dram_t physical_address;

  //search in L1
  tlb_entry = search_in_tlb_l1(virtual_page_number);

  if(tlb_entry){
    //we found it in L1
    tlb_l1_hits++;

    tlb_entry->last_access = calculate_last_access();
    if(!tlb_entry->dirty){
      //updates the dirty bit if op=write and it wasnt dirty before
      tlb_entry->dirty = (op==OP_WRITE);
    }

    physical_address = (tlb_entry->physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset;
    return physical_address;
  }

  //not found in L1
  tlb_l1_misses++;

  //search in L2
  tlb_entry = search_in_tlb_l2(virtual_page_number);

  if(tlb_entry){
    //we found it in L2
    tlb_l2_hits++;
    tlb_entry->last_access = calculate_last_access();
    
    //put it on L1
    pass_to_tlb_l1(tlb_entry);

    physical_address = (tlb_entry->physical_page_number << PAGE_SIZE_BITS) | virtual_page_offset;
    return physical_address;

  }

  //not found in L1 nor L2
  tlb_l2_misses++;

  //write in L1 and L2
  physical_address = page_table_translate(virtual_address, op);
  pa_dram_t physical_page_number = (physical_address >> PAGE_SIZE_BITS) & PHYSICAL_PAGE_NUMBER_MASK;

  tlb_entry = create_in_tlb_l1(op, virtual_page_number, physical_page_number); //put on L1
  put_on_tlb_l2(tlb_entry); //put on L2

  return physical_address;

}