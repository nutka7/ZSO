#include <sys/ptrace.h>
#include <sys/mman.h>
#include <linux/elf.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include "alienos.h"
#include "emuerr.h"

#define PAGE_SIZE 4096


static void saferead(int fd, void *buf, size_t count, off_t offset)
{
    if (pread(fd, buf, count, offset) < count)
        EMUERR("saferead");
}

static int convflags(uint32_t flags)
{
    int prot = 0;
    if (flags & PF_X) prot |= PROT_EXEC;
    if (flags & PF_W) prot |= PROT_WRITE;
    if (flags & PF_R) prot |= PROT_READ;

    return prot;
}

Elf64_Addr loadelf(int argc, char *argv[])
{
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
    int fd;
    int i, j;
    off_t offset;
    void *addr;
    int padding;
    void *pageaddr;
    size_t memsz;
    int r;

    if (argc == 1) EMUERR("No program to emulate.");

    fd = open(argv[1], O_RDONLY);
    SYSERR(fd, "open");

    saferead(fd, &ehdr, (sizeof ehdr), 0);

    for (i = 0; i < ehdr.e_phnum; ++i)
    {
        offset = ehdr.e_phoff + i * (sizeof phdr);
        saferead(fd, &phdr, (sizeof phdr), offset);
        
        /* mmap & mprotect address must be aligned to a page boundary */
        padding = phdr.p_vaddr % PAGE_SIZE;
        pageaddr = (void *) (phdr.p_vaddr - padding);
        memsz = phdr.p_memsz + padding;

        addr = mmap(pageaddr, memsz, PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                    -1, 0);
        SYS2ERR(addr, "mmap");

        switch (phdr.p_type) {
        case PT_LOAD:
            saferead(fd, (void *) phdr.p_vaddr, phdr.p_filesz, phdr.p_offset);
            break;
        
        case PT_PARAMS:
            if (argc - 2 != phdr.p_memsz / 4)
                EMUERR("Invalid argnum for the alien program");

            for (j = 0; j < phdr.p_memsz / 4; ++j)
                /* x86_64 is little endian and sizeof (int) == 4 */
                *((int *) phdr.p_vaddr + j) = atoi(argv[j+2]);
            break;

        default:
            EMUERR("Unexpected segment type");
            break;
        }

        r = mprotect(pageaddr, memsz, convflags(phdr.p_flags));
        SYSERR(r, "mprotect");
    }

    return ehdr.e_entry;
}

int main(int argc, char *argv[])
{
    long r;
    Elf64_Addr addr;

    addr = loadelf(argc, argv);

    r = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    SYSERR(r, "PTRACE_TRACEME");
    
    /* Ensure the parent has a chance to start tracing. */
    raise(SIGSTOP);

    goto *addr;
}
