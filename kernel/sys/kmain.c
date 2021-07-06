#include <util/types.h>
#include <drivers/gfx/fb/fb.h>
#include <mem/paging.h>
#include <mem/kheap.h>
#include <intr/idt.h>
#include <cpu/gdt.h>
#include <drivers/input/mouse/ps2_mouse.h>
#include <drivers/input/keyboard/ps2_keyboard.h>
#include <sys/io.h>
#include <time/time.h>
#include <acpi/acpi.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci_ids.h>
#include <drivers/storage/ata/ahci/ahci.h>
#include <drivers/fs/vfs/vfs.h>
#include <sched/sched.h>
#include <intr/apic.h>
#include <sys/syscall/syscall.h>
#include <sys/console.h>
#include <util/rand.h>
#include <cpu/smp.h>
#include <intr/pic.h>
#include <util/bmp.h>
#include <util/stdlib.h>
#include <util/elf.h>
#include <drivers/tty/serial.h>
#include <drivers/storage/partmgr/gpt.h>

sem_t* testsem;

void kernel_proc()
{
    sem_acquire(testsem);
    serial_writestr("Ok\n");

    int x = 0;
    for (;;)
    {
        if (x > 500)
        {
            sem_release(testsem);
        }
        x += 1;

        for (int i = 0; i < 100; i++)
        for (int j = 0; j < 100; j++)
        {
            video_putpix(i + x, j, 255, 0, 0);
        }
    }
}

void kernel_proc_2()
{
    sem_acquire(testsem);
    int y = 0;
    for (;;)
    {
        y += 1;

        for (int i = 0; i < 100; i++)
        for (int j = 0; j < 100; j++)
        {
            video_putpix(i, j + y, 255, 255, 0);
        }
    }
}

// TODO: only load drivers for devices if they are present. This should
// take the form of kernel modules, and the initializing should not EXPECT
// a particular device to be present.

void kmain()
{
    serial_writestr("Enumerating PCI devices...");
    pci_enumerate();
    serial_writestr("Ok\n");

    serial_writestr("Initializing VFS...");
    vfs_init();
    serial_writestr("Ok\n");
    

//                  Everything here is temporary
// -----------------------------------------------------------------

    serial_writestr("Initializing AHCI controllers...");
    ahci_init(pci_devs);
    serial_writestr("Ok\n");

    serial_writestr("Mounting /dev/disk0 to /...");
    //vfs_node_t* dev = gpt_getpart(ahci_get_dev(0), "Root");
    vfs_node_t* dev = ahci_get_dev(0);
    vfs_mount(dev, "/dev/disk0"); // Mount first disk

    vfs_node_t* root = ext2_init(dev);
    vfs_mount(root, "/"); // Mount root file system

    console_init();
    video_init();

    serial_writestr("Ok\n");

// ---------------------------------------------------------------------

    serial_writestr("Initializing keyboard...");
    kb_init();
    serial_writestr("Ok\n");

    serial_writestr("Initializing mouse...");
    mouse_init();
    serial_writestr("Ok\n");

    serial_writestr("Initializing random number generator...");
    rand_seed(305640980);
    serial_writestr("Ok\n");
    
    serial_writestr("Creating kernel process...");
    sched_spawn_proc(mk_proc(kernel_proc));
    sched_spawn_proc(mk_proc(kernel_proc_2));
    serial_writestr("Ok\n");

    // TEMP
    list_t* args = list_create();
    list_push_back(args, "Hello");
    list_t* env = list_create();
    proc_t* elf = mkelfproc("/bin/test", args, env);
    sched_spawn_proc(elf);
    testsem = sem_create(1);

    serial_writestr("Intializing scheduler...");
    sched_init();

    for (;;);
}