qemu-system-x86_64 dist/image.iso -serial stdio -m 512 -drive id=disk,file=dist/disk.img,if=none -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -s -S -smp 2