/*
 * Copyright 2012, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Adds support for merging and appending of Haiku/BeOS resource
 * data.
 */

#include <stdint.h>
#include <stdbool.h>

#define FATELF_UTILS 1
#include "fatelf-utils.h"

#include "fatelf-haiku.h"
#include "elf.h"

#define HAIKU_RSRC_HEADER_MAGIC     0x444f1000

#define HAIKU_ELF32_RSRC_ALIGN_MIN  32
// FATELF_REVIEW: Should we recommend this be changed to page alignment before
// the Haiku binary ABI is stabilized?
#define HAIKU_ELF64_RSRC_ALIGN      8
// FATELF_REVIEW: Should this be page aligned? We're just borrowing the
// alignment used by the existing Haiku ELF64 code
#define HAIKU_FAT_RSRC_ALIGN        8

#define ALIGN(v, a)     (((v + a - 1) / a) * a)

struct elf_table_layout {
    uint64_t offset;
    uint64_t header_size;
    uint32_t header_count;
};

struct elf_layout {
    uint32_t header_size;
    struct elf_table_layout prog;
    struct elf_table_layout sect;
};

// Byteswap handlers
static uint16_t swap16 (uint16_t v) { return xswap16(v); }
static uint16_t nswap16 (uint16_t v) { return v; }

static uint32_t swap32 (uint32_t v) { return xswap32(v); }
static uint32_t nswap32 (uint32_t v) { return v; }

static uint64_t swap64 (uint64_t v) { return xswap64(v); }
static uint64_t nswap64 (uint64_t v) { return v; }


// Determine the file position of the Haiku resources within a FatELF file. The
// returned offset may extend past the end of the file if no resources
// are available in the file.
static int haiku_fat_rsrc_offset(const char *fname, const int fd,
                                 const FATELF_header *header, uint64_t *offset)
{
    const int furthest = find_furthest_record(header);
    if (furthest < 0)
        return 0;

    const FATELF_record *rec = &header->records[furthest];
    const uint64_t edge = rec->offset + rec->size;

    *offset = ALIGN(edge, HAIKU_FAT_RSRC_ALIGN);
    return 1;
}

// Determine the file position of the Haiku resources within an ELF file. The
// returned offset may extend past the end of the file if no resources
// are available in the file.
static int haiku_elf_rsrc_offset(const char *fname, const int fd,
                                 uint64_t *offset)
{
    uint8_t ident[EI_NIDENT];

    xlseek(fname, fd, 0, SEEK_SET);
    xread(fname, fd, ident, sizeof(ident), 1);
    if (memcmp(ident, ELF_MAGIC, 4) != 0)
        return 0;

    uint64_t (*get64)(uint64_t v) = nswap64;
    uint32_t (*get32)(uint32_t v) = nswap32;
    uint16_t (*get16)(uint16_t v) = nswap16;

    if (ident[EI_DATA] != FATELF_HOST_ENDIAN) {
        get64 = swap64;
        get32 = swap32;
        get16 = swap16;
    }

    xlseek(fname, fd, 0, SEEK_SET);

    /* Parse the ELF header */
    struct elf_layout elfData;
    if (ident[EI_CLASS] == FATELF_32BITS) {
        struct Elf32_Ehdr ehdr;

        xread(fname, fd, &ehdr, sizeof(ehdr), 1);
        elfData.header_size = get16(ehdr.e_ehsize);

        elfData.prog.offset = get32(ehdr.e_phoff);
        elfData.prog.header_size = get16(ehdr.e_phentsize);
        elfData.prog.header_count = get16(ehdr.e_phnum);

        elfData.sect.offset = get32(ehdr.e_shoff);
        elfData.sect.header_size = get16(ehdr.e_shentsize);
        elfData.sect.header_count = get16(ehdr.e_shnum);

    } else if (ident[EI_CLASS] == FATELF_64BITS) {
        struct Elf64_Ehdr ehdr;

        xread(fname, fd, &ehdr, sizeof(ehdr), 1);
        elfData.header_size = get32(ehdr.e_ehsize);

        elfData.prog.offset = get64(ehdr.e_phoff);
        elfData.prog.header_size = get16(ehdr.e_phentsize);
        elfData.prog.header_count = get16(ehdr.e_phnum);

        elfData.sect.offset = get64(ehdr.e_shoff);
        elfData.sect.header_size = get16(ehdr.e_shentsize);
        elfData.sect.header_count = get16(ehdr.e_shnum);
    } else {
        xfail("'%s' has an invalid ELF EI_CLASS", fname);
    }

    /* Compute the offset to non-ELF data. For ELF files, this is based
     * on the offset to the end of the ELF data, plus either a fixed
     * alignment of 8 on ELF64, or on ELF32, the largest alignment value
     * specified in a Elf32_Phdr. */
    uint64_t rsrcOffset = 0;
    uint64_t rsrcAlign = 0;
    uint32_t i;

    if (elfData.prog.offset != 0) {
        uint64_t tableSize;
        uint64_t tableEnd;

        tableSize = elfData.prog.header_size * elfData.prog.header_count;
        tableEnd = elfData.prog.offset + tableSize;
        if (tableEnd > rsrcOffset)
            rsrcOffset = tableEnd;

        void *headers = xmalloc(tableSize);

        xread(fname, fd, headers, tableSize, 1);

        if (ident[EI_CLASS] == FATELF_32BITS) {
            struct Elf32_Phdr *phdrs = headers;
            for (i = 0; i < elfData.prog.header_count; i++) {
                struct Elf32_Phdr *phdr = phdrs + i;
                uint32_t type = get32(phdr->p_type);
                uint64_t offset = get32(phdr->p_offset);
                uint64_t size = get32(phdr->p_filesz);
                uint64_t alignment = get32(phdr->p_align);

                if (type == PT_NULL)
                    continue;

                uint64_t sectEnd = offset + size;
                if (sectEnd > rsrcOffset)
                    rsrcOffset = sectEnd;

                if (alignment > rsrcAlign)
                    rsrcAlign = alignment;
            }
        } else {
            struct Elf64_Phdr *phdrs = headers;
            for (i = 0; i < elfData.prog.header_count; i++) {
                struct Elf64_Phdr *phdr = phdrs + i;
                uint32_t type = get32(phdr->p_type);
                uint64_t offset = get64(phdr->p_offset);
                uint64_t size = get64(phdr->p_filesz);
                uint64_t alignment = get64(phdr->p_align);

                if (type == PT_NULL)
                    continue;

                uint64_t sectEnd = offset + size;
                if (sectEnd > rsrcOffset)
                    rsrcOffset = sectEnd;

                if (alignment > rsrcAlign)
                    rsrcAlign = alignment;
            }
        }
    }

    if (elfData.sect.offset != 0) {
        uint64_t tableSize;
        uint64_t tableEnd;

        tableSize = elfData.sect.header_size * elfData.sect.header_count;
        tableEnd = elfData.sect.offset + tableSize;
        if (tableEnd > rsrcOffset)
            rsrcOffset = tableEnd;

        void *headers = xmalloc(tableSize);
        xread(fname, fd, headers, tableSize, 1);
        if (ident[EI_CLASS] == FATELF_32BITS) {
            struct Elf32_Shdr *shdrs = headers;
            for (i = 0; i < elfData.sect.header_count; i++) {
                struct Elf32_Shdr *shdr = shdrs + i;
                uint32_t type = get32(shdr->sh_type);
                uint64_t offset = get32(shdr->sh_offset);
                uint64_t size = get32(shdr->sh_size);

                /* Skip sections that occupy no file space */
                if (type == SHT_NULL || type == SHT_NOBITS)
                    continue;

                uint64_t sectEnd = offset + size;
                if (sectEnd > rsrcOffset)
                    rsrcOffset = sectEnd;
            }
        } else {
            struct Elf64_Shdr *shdrs = headers;
            for (i = 0; i < elfData.sect.header_count; i++) {
                struct Elf64_Shdr *shdr = shdrs + i;
                uint32_t type = get32(shdr->sh_type);
                uint64_t offset = get64(shdr->sh_offset);
                uint64_t size = get64(shdr->sh_size);

                /* Skip sections that occupy no file space */
                if (type == SHT_NULL || type == SHT_NOBITS)
                    continue;

                uint64_t sectEnd = offset + size;
                if (sectEnd > rsrcOffset)
                    rsrcOffset = sectEnd;
            }
        }
    }

    // For 64-bit files, Haiku uses an 8 byte alignment for the resource header
    if (ident[EI_CLASS] == FATELF_64BITS)
        rsrcAlign = HAIKU_ELF64_RSRC_ALIGN;
    else if (rsrcAlign < HAIKU_ELF32_RSRC_ALIGN_MIN)
        rsrcAlign = HAIKU_ELF32_RSRC_ALIGN_MIN;

    *offset = ALIGN(rsrcOffset, rsrcAlign);

    return 1;
}

static bool haiku_parse_rsrc_header(const char *fname, const int fd,
                                    uint64_t offset, uint64_t *size)
{
    // TODO - compute actual resource size by reading the resource table
    uint64_t fileSize = xget_file_size(fname, fd);
    if (fileSize <= offset) {
        return false;
    }
    *size = fileSize - offset;

    uint32_t magic;
    xlseek(fname, fd, offset, SEEK_SET);
    xread(fname, fd, &magic, sizeof(magic), 1);

    if (magic != HAIKU_RSRC_HEADER_MAGIC &&
        xswap32(magic) != HAIKU_RSRC_HEADER_MAGIC)
    {
        return false;
    }

    return true;
}

int haiku_rsrc_offset(const char *fname, const int fd, uint64_t *offset)
{
    union {
        uint8_t elf[4];
        uint32_t fatelf;
    } magic;

    xlseek(fname, fd, 0, SEEK_SET);
    xread(fname, fd, &magic, sizeof(magic), 1);

    // ELF file
    if (memcmp(magic.elf, ELF_MAGIC, sizeof(magic.elf)) == 0)
        return haiku_elf_rsrc_offset(fname, fd, offset);

    // FatELF file
    if (FATELF_HOST_ENDIAN == FATELF_BIGENDIAN)
        magic.fatelf = xswap32(magic.fatelf);

    if (magic.fatelf == FATELF_MAGIC) {
        FATELF_header *header = xread_fatelf_header(fname, fd);
        int ret = haiku_fat_rsrc_offset(fname, fd, header, offset);
        free(header);

        return ret;
    }

    // Unknown file
    return 0;
}

int haiku_find_rsrc(const char *fname, const int fd, uint64_t *offset,
                    uint64_t *size)
{
    if (!haiku_rsrc_offset(fname, fd, offset))
        return 0;

    if (!haiku_parse_rsrc_header(fname, fd, *offset, size))
        return 0;

    return 1;
}
