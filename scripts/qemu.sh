qemu-system-x86_64 -L /usr/share/OVMF/x64 -bios /usr/share/OVMF/x64/OVMF_CODE.fd -cdrom bin/os.iso -drive id=disk,file=bin/sata.img,if=none -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -monitor stdio