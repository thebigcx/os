#qemu-system-x86_64 -L /usr/share/OVMF/x64 -bios /usr/share/OVMF/x64/OVMF_CODE.fd -cdrom bin/os.iso -drive id=disk,file=bin/sata.img,if=none -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -smp 2 -serial stdio -soundhw pcspk
qemu-system-x86_64 image.iso -serial stdio -m 512 -drive id=disk,file=bin/sata.img,if=none -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0
