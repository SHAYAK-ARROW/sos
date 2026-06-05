#include "../drivers/terminal.h"
#include "../drivers/keyboard.h"
#include "../drivers/disk.h"
#include "../fs/fs.h"
#include "../fs/fat16.h"
#include "../shell/shell.h"
#include "../mm/pmm.h"
#include "../mm/paging.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
void check_all_pci_buses(void);

void kernel_main(multiboot_info_t *mbd, uint32_t magic)
{terminal_init();
    
    // ??????? ???????? ?? ??? ??? (??? ??? ????? ??? - ?)
    terminal_setcolor(9, 0); 
    terminal_writeline("Checkpoint 1: Kernel entered");
    
    // GDT ??? IDT ??? ???? ??? ????? ?? (???? ???? - ?)
    terminal_setcolor(2, 0);
    gdt_init();
    terminal_writeline("Checkpoint 2: GDT loaded");
    
    idt_init();
    terminal_writeline("Checkpoint 3: IDT loaded");
    
    // ?????? ??? ???? ????? (???? - ?)
    terminal_setcolor(7, 0);
    /* 2. PMM */
    pmm_init(mbd, magic);
    terminal_writeline("DEBUG: pmm_init() passed");

    /* 3. Paging - ?? ???????? ?????? ?????? */
    terminal_writeline("DEBUG: About to call paging_init()");
    paging_init(); 
    terminal_writeline("DEBUG: paging_init() passed");
    terminal_writeline("DEBUG: About to enter PCI check");
   check_all_pci_buses();  // ????? ??? ???? ????
terminal_writeline("DEBUG: PCI check skipped, entering main loop");
 // ????? ????????? ???? ??? ???? ???? ????????? ?? ??
    /* 5. Disk */
    terminal_writeline("DEBUG: About to call disk_init()");
    disk_init();
    terminal_writeline("DEBUG: disk_init() passed");

    /* 6. Filesystem */
    if (fat_init() == 0) {
        terminal_writeline("DEBUG: fat_init() passed");
    } else {
        terminal_writeline("DEBUG: fat_init() FAILED (ignoring)");
    }

    /* 7. Keyboard */
    keyboard_init();
    terminal_writeline("DEBUG: keyboard_init() passed");

    /* 8. Shell */
    terminal_writeline("DEBUG: Entering shell_run()...");
    shell_run();
}
