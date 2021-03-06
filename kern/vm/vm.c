#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <uio.h>
#include <stat.h>
#include <vnode.h>
#include <vfs.h>
#include <bitmap.h>
#include <kern/fcntl.h>

static paddr_t lastpaddr, firstpaddr;
struct coremap_page *coremap_start;
static int coremap_used_counter;
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
static struct spinlock bitmaplock = SPINLOCK_INITIALIZER;
struct swap_disk swap;
static unsigned int roundrobin_counter;
//bool *clru = NULL;

static void as_zero_region(paddr_t paddr, unsigned npages)
{
        bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

void coremap_load() {
	
	lastpaddr = ram_getsize();
	firstpaddr = ram_getfirstfree();

	if(firstpaddr % PAGE_SIZE != 0) {
		firstpaddr = ((firstpaddr/PAGE_SIZE) + 1) * PAGE_SIZE;
	}

	unsigned int pages = lastpaddr/PAGE_SIZE;

	coremap_start = (struct coremap_page *)PADDR_TO_KVADDR(firstpaddr);

	unsigned int coremap_size = sizeof(struct coremap_page)*pages/PAGE_SIZE;

	if ((sizeof(struct coremap_page)*pages) % PAGE_SIZE != 0) {
		coremap_size = coremap_size + 1;
	}

	firstpaddr = firstpaddr + coremap_size * PAGE_SIZE;
	
	KASSERT(firstpaddr % PAGE_SIZE == 0);	

	for (unsigned int i = 0; i < firstpaddr/PAGE_SIZE; i++) {
		coremap_start[i].chunk_size = 0;
		coremap_start[i].state = fixed;
	}
	
	for (unsigned int i = firstpaddr/PAGE_SIZE; i < lastpaddr/PAGE_SIZE; i++){
		coremap_start[i].chunk_size = 0;
		coremap_start[i].state = free;
	}
	roundrobin_counter = firstpaddr/PAGE_SIZE;
	kprintf("\nFirst Paddr %d", firstpaddr);	
	kprintf("\nLast Paddr %d", lastpaddr);	
	kprintf("\nFirst value of roundrobin_counter %d", roundrobin_counter);	
}

void vm_bootstrap(void) {
	int err;
	struct vnode *v;
	struct stat disk_stat;
	char diskpath[] = "lhd0raw:";

	err = vfs_open(diskpath, O_RDWR, 0, &v);
	if (err) {
		swap.vnode = NULL;
//		swap.spinlock = NULL;
		swap.bitmap = NULL;
		swap.swap_disk_present = false;	
		return;
	}
	
	err = VOP_STAT(v, &disk_stat);
	if (err) {
		vfs_close(v);
		swap.vnode = NULL;
//		swap.spinlock = NULL;
		swap.bitmap = NULL;
		swap.swap_disk_present = false;
		return;
	}
	KASSERT(disk_stat.st_size % PAGE_SIZE == 0);
	kprintf("Disk Size : %llu", disk_stat.st_size);
	swap.bitmap = bitmap_create(disk_stat.st_size/PAGE_SIZE);
	kprintf("Bitmap size : %llu", disk_stat.st_size/PAGE_SIZE);
	KASSERT(swap.bitmap != NULL);
//	spinlock_init(swap.spinlock);
//	KASSERT(swap.spinlock != NULL);
	swap.swap_disk_present = true;
	swap.vnode = v;

//	clru = kmalloc(sizeof(bool) * ((lastpaddr-firstpaddr)/PAGE_SIZE));
}


static paddr_t getppages(unsigned long npages) {
	
	paddr_t addr = 0;
	unsigned int free_pages = 0;
	
	spinlock_acquire(&coremap_lock);

	for(unsigned int i = firstpaddr/PAGE_SIZE; i < lastpaddr/PAGE_SIZE; i++) {
		
		if(coremap_start[i].state == free) {
			free_pages++;
		}else {
			free_pages = 0;
		}

		if(free_pages == npages) {
			addr = (i - (npages - 1))*PAGE_SIZE;
			coremap_used_counter += npages;
			break;
		}
	}

	if(free_pages != npages) {
		if (swap.swap_disk_present) {
                        addr = swapout();
                } else {
                        spinlock_release(&coremap_lock);
                        return 0;
                }
	}
	
	for(unsigned int i = 0; i < npages; i++) {
		coremap_start[(addr/PAGE_SIZE) + i].state = fixed;
		coremap_start[(addr/PAGE_SIZE) + i].ref_bit = true;
		coremap_start[(addr/PAGE_SIZE) + i].owner_addrspace = NULL;
		coremap_start[(addr/PAGE_SIZE) + i].owner_vaddr = 0;
		if(i == 0) {
			coremap_start[(addr/PAGE_SIZE) + i].chunk_size = npages;
		} 
	}
	as_zero_region(addr, npages);
//	coremap_used_counter += npages;
	spinlock_release(&coremap_lock);

	return addr;
}

paddr_t getuserpage(unsigned long npages, struct addrspace *as, vaddr_t as_vpage, bool copy_call){
       
	KASSERT(npages == 1); 
	paddr_t addr = 0;
	unsigned int free_pages = 0;
	
	spinlock_acquire(&coremap_lock);

	for(unsigned int i = firstpaddr/PAGE_SIZE; i < lastpaddr/PAGE_SIZE; i++) {
		
		if(coremap_start[i].state == free) {
			free_pages++;
		}else {
			free_pages = 0;
		}

		if(free_pages == npages) {
			addr = (i - (npages - 1))*PAGE_SIZE;
			coremap_used_counter += npages;
			break;
		}
	}

	if(free_pages != npages) {
		if (swap.swap_disk_present) {
			addr = swapout();
		} else {
			spinlock_release(&coremap_lock);
			return 0;
		}
	}
	
	for(unsigned int i = 0; i < npages; i++) {
		coremap_start[(addr/PAGE_SIZE) + i].state = used;
		coremap_start[(addr/PAGE_SIZE) + i].owner_addrspace = as;
		coremap_start[(addr/PAGE_SIZE) + i].owner_vaddr = as_vpage;
		if (copy_call) {
			coremap_start[(addr/PAGE_SIZE) + i].ref_bit = false;
		} else {
			coremap_start[(addr/PAGE_SIZE) + i].ref_bit = true;
		}
		
		if(i == 0) {
			coremap_start[(addr/PAGE_SIZE) + i].chunk_size = npages;
		} 
	}
	as_zero_region(addr, npages);
//	coremap_used_counter += npages;
	spinlock_release(&coremap_lock);

	return addr;
}


vaddr_t alloc_kpages(unsigned npages) {

	paddr_t pages_paddr = getppages(npages);
	
	if(pages_paddr == 0) {
		return 0;
	}
	vaddr_t pages_vaddr = PADDR_TO_KVADDR(pages_paddr);
	
	return pages_vaddr;
}

void free_kpages (vaddr_t addr)
{
	paddr_t pages_paddr = addr - MIPS_KSEG0;

	spinlock_acquire(&coremap_lock);
	KASSERT(pages_paddr % PAGE_SIZE == 0);
	unsigned int page_index = pages_paddr/PAGE_SIZE;

	unsigned int chunks = coremap_start[page_index].chunk_size;

	for(unsigned int i = 0; i < chunks; i++) {
		coremap_start[page_index + i].state = free;
		coremap_start[page_index + i].chunk_size = 0; 
	}
	coremap_used_counter -= chunks;
	spinlock_release(&coremap_lock);
}

int free_ppages(paddr_t page_paddr) {
	spinlock_acquire(&coremap_lock);
	
	KASSERT(page_paddr % PAGE_SIZE == 0);

	unsigned int page_index = page_paddr/PAGE_SIZE;
	unsigned int chunks = coremap_start[page_index].chunk_size;
	
	KASSERT(chunks == 1);
	KASSERT(coremap_start[page_index].state != fixed);
	if(coremap_start[page_index].state == in_eviction) {
		spinlock_release(&coremap_lock);
		return 1;
	}else {
		coremap_start[page_index].state = free;
		coremap_start[page_index].chunk_size = 0;
		coremap_start[page_index].owner_addrspace = NULL;
		coremap_start[page_index].owner_vaddr = 0;
		coremap_used_counter -= chunks;	
		spinlock_release(&coremap_lock);
	}
	return 0; 	
}

void tlb_invalidate_entry(vaddr_t remove_vaddr) {

	//uint32_t ehi, elo;	
	int spl = splhigh();
	int index = -1;
	index = tlb_probe(remove_vaddr, 0);
	if (index >= 0) {
		tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
	}
        splx(spl);	
}

int vm_fault(int faulttype, vaddr_t faultaddress)
{
	struct addrspace *as;
	vaddr_t stackbase, stacktop;
	paddr_t ppage = 0;
	bool valid_addr = false;
	int i, spl;
	struct region *rg;
	struct page_table_entry *pte;
	struct page_table_entry *pte_prev = NULL;
	uint32_t ehi, elo;	
	(void)faulttype;
	faultaddress &= PAGE_FRAME;
	as = proc_getas();
	rg = as->start_region;
	int err;

	if (curproc == NULL) {
                /*
                 * No process. This is probably a kernel fault early
                 * in boot. Return EFAULT so as to panic instead of
                 * getting into an infinite faulting loop.
                 */
                return EFAULT;
        }
	
	if (as == NULL) {
		return EFAULT;
	}
	
	KASSERT(as->start_region != NULL);

	stacktop = USERSTACK;
	stackbase = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
	
	if(faultaddress >= stackbase && faultaddress < stacktop) {
		valid_addr = true;
	} else if (faultaddress >= as->heap_start && faultaddress < as->heap_end) {
		valid_addr = true;
	} else {
		while(rg != NULL) {
			if(faultaddress >= rg->start && faultaddress < (rg->start + rg->size)){
			//	if(faulttype == VM_FAULT_READ && rg->read == true) {
					valid_addr = true;
					break;
			/*	} else if (faulttype == VM_FAULT_WRITE && rg->write == true) {
					valid_addr = true;
					break;	
				} else if (faulttype == VM_FAULT_READONLY) {
					
				}
				valid_addr = false;
				break;
			*/	
			}
			rg = rg->next;
		}
	}
	if(valid_addr == false) {
		return EFAULT;
	}
	if(as->start_page_table == NULL) {
		
		as->start_page_table = kmalloc(sizeof(struct page_table_entry));
		if(as->start_page_table == NULL) {
			return ENOMEM;
		}
		pte = as->start_page_table;
		pte->lock = lock_create("PTE_Lock");
		lock_acquire(pte->lock);
		pte->as_vpage = faultaddress;
		pte->state = UNMAPPED;
		pte->vpage_permission = 0;
		pte->next = NULL; 	
		ppage  = getuserpage(1, as, faultaddress, false);
		if(ppage == 0) {
			lock_release(pte->lock);
			lock_destroy(pte->lock);
			kfree(pte);
			return ENOMEM;
		}
		pte->as_ppage = ppage;
		pte->state = MAPPED;
	} else {
		pte = as->start_page_table;
		while(pte != NULL) {
			if(pte->as_vpage == faultaddress) {
				lock_acquire(pte->lock);
				if(pte->state == SWAPPED) {
					ppage = getuserpage(1, as, pte->as_vpage, false);
					if(ppage == 0) {
						lock_release(pte->lock);
						return ENOMEM;
					}
					bool unmark = true;
					err = diskblock_read(ppage, pte->diskpage_location, unmark);
					if(err) {
						panic("unable to read from diskblock");
					}
					pte->as_ppage = ppage;
					pte->state = MAPPED;
				} else if (pte->state == MAPPED) {
					ppage = pte->as_ppage;
				} else {
					kprintf("Code should never reach here!");
				}
				break;
			}
			pte_prev = pte;
			pte = pte->next;
		}
		if (pte == NULL) {
                	pte_prev->next = kmalloc(sizeof(struct page_table_entry));
			if(pte_prev->next == NULL) {
				return ENOMEM;
			}
			pte = pte_prev->next;
			pte->lock = lock_create("PTE_Lock");
                	lock_acquire(pte->lock);
			pte->as_vpage = faultaddress;
			pte->state = UNMAPPED;
                	pte->vpage_permission = 0;
                	pte->next = NULL;
			ppage  = getuserpage(1, as, faultaddress, false);
			if(ppage == 0) {
				lock_release(pte->lock);
				lock_destroy(pte->lock);
				kfree(pte);
				return ENOMEM;
			}
                	pte->as_ppage = ppage;
			pte->state = MAPPED;
		}
	}
	
	KASSERT((ppage & PAGE_FRAME) == ppage);
	spl = splhigh();

        for (i=0; i<NUM_TLB; i++) {
                tlb_read(&ehi, &elo, i);
                if (elo & TLBLO_VALID) {
                        continue;
                }
                ehi = faultaddress;
                elo = ppage | TLBLO_DIRTY | TLBLO_VALID;
                tlb_write(ehi, elo, i);
                splx(spl);
		lock_release(pte->lock);
//		clru[(ppage-firstpaddr)/PAGE_SIZE] = true;
		coremap_start[ppage/PAGE_SIZE].ref_bit = true;
                return 0;
        }
	ehi = faultaddress;
        elo = ppage | TLBLO_DIRTY | TLBLO_VALID;
	tlb_random(ehi, elo);
        splx(spl);	 
	lock_release(pte->lock);
//	clru[(ppage-firstpaddr)/PAGE_SIZE] = true;
	coremap_start[ppage/PAGE_SIZE].ref_bit = true;
	return 0;
}

unsigned int coremap_used_bytes(void){
	//if(load_complete) {
	//	spinlock_acquire(&coremap_lock);
	//}
	
	return coremap_used_counter*PAGE_SIZE;
	
	//if(load_complete){
	//	spinlock_release(&coremap_lock);
	//}
}

void vm_tlbshootdown(const struct tlbshootdown * ts) {
	(void)ts;
}

int diskblock_read(paddr_t ppage_addr, unsigned int index, bool unmark) {
	
	struct iovec iov;
	struct uio kuio;
	
	uio_kinit(&iov, &kuio, (void *)PADDR_TO_KVADDR(ppage_addr), PAGE_SIZE, index * PAGE_SIZE, UIO_READ);
	KASSERT(bitmap_isset(swap.bitmap, index) != 0);

	int err = VOP_READ(swap.vnode, &kuio);
	if(err) {
		return err;
	}
	if (unmark) {
		spinlock_acquire(&bitmaplock);
		bitmap_unmark(swap.bitmap, index);
		spinlock_release(&bitmaplock);
	}
	return 0;
}

void bitmap_unmark_wrapper(unsigned int index) {
	spinlock_acquire(&bitmaplock);
	bitmap_unmark(swap.bitmap, index);
	spinlock_release(&bitmaplock);
}

int diskblock_write(paddr_t ppage_addr, unsigned int *index) {
	
	unsigned int block_index;
	struct iovec iov;
	struct uio kuio;
	
	spinlock_acquire(&bitmaplock);
	int err = bitmap_alloc(swap.bitmap, &block_index);
	spinlock_release(&bitmaplock);
	if(err) {
		kprintf("Problem in bitmap : %d", err);
		return err;
	}
	

	uio_kinit(&iov, &kuio, (void *)PADDR_TO_KVADDR(ppage_addr), PAGE_SIZE, block_index * PAGE_SIZE, UIO_WRITE);
	
	err = VOP_WRITE(swap.vnode, &kuio);
	if(err) {
		kprintf("Problem in VOP Write");
		return err;
	}

	*index = block_index;

	return 0;
}

paddr_t swapout(void) {
	
	struct addrspace *old_as;
	vaddr_t old_vaddr;
	paddr_t evicted_paddr;
	struct page_table_entry *pte;
	unsigned int diskblock_index;
	int err;

	KASSERT(spinlock_do_i_hold(&coremap_lock) == true);
	/*while (coremap_start[roundrobin_counter].state != used) {
		roundrobin_counter++;
		if (roundrobin_counter == lastpaddr/PAGE_SIZE + 1) {
			roundrobin_counter = firstpaddr/PAGE_SIZE;
		}
	}*/
	while (coremap_start[roundrobin_counter].ref_bit != false
			|| coremap_start[roundrobin_counter].state == in_eviction
			|| coremap_start[roundrobin_counter].state == fixed) {
		if (coremap_start[roundrobin_counter].state == used) {
			coremap_start[roundrobin_counter].ref_bit = false;
		}
		roundrobin_counter++;
		if (roundrobin_counter == lastpaddr/PAGE_SIZE) {
			roundrobin_counter = firstpaddr/PAGE_SIZE;
		}	
	}
//	kprintf("State : %d", coremap_start[roundrobin_counter].state);
	KASSERT(coremap_start[roundrobin_counter].state == used);
	old_as = coremap_start[roundrobin_counter].owner_addrspace;
	old_vaddr = coremap_start[roundrobin_counter].owner_vaddr;
	evicted_paddr = roundrobin_counter * PAGE_SIZE;
	coremap_start[roundrobin_counter].state = in_eviction;
	roundrobin_counter++;
	if (roundrobin_counter == lastpaddr/PAGE_SIZE) {
		roundrobin_counter = firstpaddr/PAGE_SIZE;
	}
	spinlock_release(&coremap_lock);
	
	KASSERT(old_as->start_page_table != NULL);
	pte = old_as->start_page_table;
	while(pte != NULL) {
		if(pte->as_vpage == old_vaddr) {
			lock_acquire(pte->lock);
			KASSERT(pte->as_ppage == evicted_paddr);
			KASSERT(pte->state == MAPPED);
			break;
		}
		pte = pte->next;
	}
	KASSERT(pte != NULL);
	tlb_invalidate_entry(old_vaddr);
	err = diskblock_write(evicted_paddr, &diskblock_index);
	if (err) {
		panic("Cannot write to Disk");
	}
	pte->diskpage_location = diskblock_index;
	pte->state = SWAPPED;
	lock_release(pte->lock);
	spinlock_acquire(&coremap_lock);
	
	KASSERT(coremap_start[evicted_paddr/PAGE_SIZE].owner_addrspace == old_as);
	KASSERT(coremap_start[evicted_paddr/PAGE_SIZE].owner_vaddr == old_vaddr);	
	return evicted_paddr;
}
