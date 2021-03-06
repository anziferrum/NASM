/* ----------------------------------------------------------------------- *
 *
 *   Copyright 1996-2016 The NASM Authors - All Rights Reserved
 *   See the file AUTHORS included with the NASM distribution for
 *   the specific copyright holders.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *     CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *     INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *     DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *     CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *     NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *     OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *     EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------- */

/*
 * outmacho.c	output routines for the Netwide Assembler to produce
 *		NeXTstep/OpenStep/Rhapsody/Darwin/MacOS X object files
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "nasm.h"
#include "nasmlib.h"
#include "saa.h"
#include "raa.h"
#include "rbtree.h"
#include "outform.h"
#include "outlib.h"

#if defined(OF_MACHO) || defined(OF_MACHO64)

/* Mach-O in-file header structure sizes */
#define MACHO_HEADER_SIZE		28
#define MACHO_SEGCMD_SIZE		56
#define MACHO_SECTCMD_SIZE		68
#define MACHO_SYMCMD_SIZE		24
#define MACHO_NLIST_SIZE		12
#define MACHO_RELINFO_SIZE		8

#define MACHO_HEADER64_SIZE		32
#define MACHO_SEGCMD64_SIZE		72
#define MACHO_SECTCMD64_SIZE		80
#define MACHO_NLIST64_SIZE		16

/* Mach-O file header values */
#define	MH_MAGIC		0xfeedface
#define	MH_MAGIC_64		0xfeedfacf
#define CPU_TYPE_I386		7		/* x86 platform */
#define CPU_TYPE_X86_64		0x01000007	/* x86-64 platform */
#define	CPU_SUBTYPE_I386_ALL	3		/* all-x86 compatible */
#define	MH_OBJECT		0x1		/* object file */

/* Mach-O load commands */
#define LC_SEGMENT		0x1		/* 32-bit segment load cmd */
#define LC_SEGMENT_64		0x19		/* 64-bit segment load cmd */
#define LC_SYMTAB		0x2		/* symbol table load command */

/* Mach-O relocations numbers */

/* Generic relocs, used by i386 Mach-O */
#define GENERIC_RELOC_VANILLA   0               /* Generic relocation */
#define GENERIC_RELOC_TLV	5		/* Thread local */

#define X86_64_RELOC_UNSIGNED   0               /* Absolute address */
#define X86_64_RELOC_SIGNED     1               /* Signed 32-bit disp */
#define X86_64_RELOC_BRANCH     2		/* CALL/JMP with 32-bit disp */
#define X86_64_RELOC_GOT_LOAD   3		/* MOVQ of GOT entry */
#define X86_64_RELOC_GOT        4		/* Different GOT entry */
#define X86_64_RELOC_SUBTRACTOR 5		/* Subtracting two symbols */
#define X86_64_RELOC_SIGNED_1   6		/* SIGNED with -1 addend */
#define X86_64_RELOC_SIGNED_2   7		/* SIGNED with -2 addend */
#define X86_64_RELOC_SIGNED_4   8		/* SIGNED with -4 addend */
#define X86_64_RELOC_TLV        9		/* Thread local */

/* Mach-O VM permission constants */
#define	VM_PROT_NONE	(0x00)
#define VM_PROT_READ	(0x01)
#define VM_PROT_WRITE	(0x02)
#define VM_PROT_EXECUTE	(0x04)

#define VM_PROT_DEFAULT	(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
#define VM_PROT_ALL	(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)

/* Our internal relocation types */
enum reltype {
    RL_ABS,			/* Absolute relocation */
    RL_REL,			/* Relative relocation */
    RL_TLV,			/* Thread local */
    RL_BRANCH,			/* Relative direct branch */
    RL_SUB,			/* X86_64_RELOC_SUBTRACT */
    RL_GOT,			/* X86_64_RELOC_GOT */
    RL_GOTLOAD			/* X86_64_RELOC_GOT_LOAD */
};
#define RL_MAX_32	RL_TLV
#define RL_MAX_64	RL_GOTLOAD

struct macho_fmt {
    uint32_t ptrsize;		/* Pointer size in bytes */
    uint32_t mh_magic;		/* Which magic number to use */
    uint32_t cpu_type;		/* Which CPU type */
    uint32_t lc_segment;	/* Which segment load command */
    uint32_t header_size;	/* Header size */
    uint32_t segcmd_size;	/* Segment command size */
    uint32_t sectcmd_size;	/* Section command size */
    uint32_t nlist_size;	/* Nlist (symbol) size */
    enum reltype maxreltype;	/* Maximum entry in enum reltype permitted */
    uint32_t reloc_abs;		/* Absolute relocation type */
    uint32_t reloc_rel;		/* Relative relocation type */
    uint32_t reloc_tlv;		/* Thread local relocation type */
};

static struct macho_fmt fmt;

static void fwriteptr(uint64_t data, FILE * fp)
{
    fwriteaddr(data, fmt.ptrsize, fp);
}

struct section {
    /* nasm internal data */
    struct section *next;
    struct SAA *data;
    int32_t index;
    int32_t fileindex;
    struct reloc *relocs;
    struct rbtree *gsyms;	/* Global symbols in section */
    int align;
    bool by_name;		/* This section was specified by full MachO name */

    /* data that goes into the file */
    char sectname[16];          /* what this section is called */
    char segname[16];           /* segment this section will be in */
    uint64_t addr;         /* in-memory address (subject to alignment) */
    uint64_t size;         /* in-memory and -file size  */
    uint64_t offset;	   /* in-file offset */
    uint32_t pad;          /* padding bytes before section */
    uint32_t nreloc;       /* relocation entry count */
    uint32_t flags;        /* type and attributes (masked) */
    uint32_t extreloc;     /* external relocations */
};

#define SECTION_TYPE	0x000000ff      /* section type mask */

#define	S_REGULAR	(0x0)   /* standard section */
#define	S_ZEROFILL	(0x1)   /* zerofill, in-memory only */

#define SECTION_ATTRIBUTES_SYS   0x00ffff00     /* system setable attributes */
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400     /* section contains some
                                                   machine instructions */
#define S_ATTR_EXT_RELOC         0x00000200     /* section has external
                                                   relocation entries */
#define S_ATTR_LOC_RELOC         0x00000100     /* section has local
                                                   relocation entries */
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000		/* section uses pure
												   machine instructions */

/* Fake section for absolute symbols, *not* part of the section linked list */
static struct section absolute_sect;

static const struct sectmap {
    const char *nasmsect;
    const char *segname;
    const char *sectname;
    const int32_t flags;
} sectmap[] = {
    {".text", "__TEXT", "__text", S_REGULAR|S_ATTR_SOME_INSTRUCTIONS|S_ATTR_PURE_INSTRUCTIONS},
    {".data", "__DATA", "__data", S_REGULAR},
    {".rodata", "__DATA", "__const", S_REGULAR},
    {".bss", "__DATA", "__bss", S_ZEROFILL},
    {NULL, NULL, NULL, 0}
};

struct reloc {
    /* nasm internal data */
    struct reloc *next;

    /* data that goes into the file */
    int32_t addr;		/* op's offset in section */
    uint32_t snum:24,		/* contains symbol index if
				 ** ext otherwise in-file
				 ** section number */
	pcrel:1,                /* relative relocation */
	length:2,               /* 0=byte, 1=word, 2=int32_t, 3=int64_t */
	ext:1,                  /* external symbol referenced */
	type:4;                 /* reloc type */
};

#define	R_ABS		0       /* absolute relocation */
#define R_SCATTERED	0x80000000      /* reloc entry is scattered if
					** highest bit == 1 */

struct symbol {
    /* nasm internal data */
    struct rbtree symv;         /* Global symbol rbtree; "key" contains the
				   symbol offset. */
    struct symbol *next;	/* next symbol in the list */
    char *name;			/* name of this symbol */
    int32_t initial_snum;	/* symbol number used above in reloc */
    int32_t snum;		/* true snum for reloc */

    /* data that goes into the file */
    uint32_t strx;              /* string table index */
    uint8_t type;		/* symbol type */
    uint8_t sect;		/* NO_SECT or section number */
    uint16_t desc;		/* for stab debugging, 0 for us */
};

/* symbol type bits */
#define	N_EXT	0x01            /* global or external symbol */

#define	N_UNDF	0x0             /* undefined symbol | n_sect == */
#define	N_ABS	0x2             /* absolute symbol  |  NO_SECT */
#define	N_SECT	0xe             /* defined symbol, n_sect holds
				** section number */

#define	N_TYPE	0x0e            /* type bit mask */

#define DEFAULT_SECTION_ALIGNMENT 0 /* byte (i.e. no) alignment */

/* special section number values */
#define	NO_SECT		0       /* no section, invalid */
#define MAX_SECT	255     /* maximum number of sections */

static struct section *sects, **sectstail, **sectstab;
static struct symbol *syms, **symstail;
static uint32_t nsyms;

/* These variables are set by macho_layout_symbols() to organize
   the symbol table and string table in order the dynamic linker
   expects.  They are then used in macho_write() to put out the
   symbols and strings in that order.

   The order of the symbol table is:
     local symbols
     defined external symbols (sorted by name)
     undefined external symbols (sorted by name)

   The order of the string table is:
     strings for external symbols
     strings for local symbols
 */
static uint32_t ilocalsym = 0;
static uint32_t iextdefsym = 0;
static uint32_t iundefsym = 0;
static uint32_t nlocalsym;
static uint32_t nextdefsym;
static uint32_t nundefsym;
static struct symbol **extdefsyms = NULL;
static struct symbol **undefsyms = NULL;

static struct RAA *extsyms;
static struct SAA *strs;
static uint32_t strslen;

/* Global file information. This should be cleaned up into either
   a structure or as function arguments.  */
static uint32_t head_ncmds = 0;
static uint32_t head_sizeofcmds = 0;
static uint64_t seg_filesize = 0;
static uint64_t seg_vmsize = 0;
static uint32_t seg_nsects = 0;
static uint64_t rel_padcnt = 0;

#define xstrncpy(xdst, xsrc)						\
    memset(xdst, '\0', sizeof(xdst));	/* zero out whole buffer */	\
    strncpy(xdst, xsrc, sizeof(xdst));	/* copy over string */		\
    xdst[sizeof(xdst) - 1] = '\0';      /* proper null-termination */

#define alignint32_t(x)							\
    ALIGN(x, sizeof(int32_t))	/* align x to int32_t boundary */

#define alignint64_t(x)							\
    ALIGN(x, sizeof(int64_t))	/* align x to int64_t boundary */

#define alignptr(x) \
    ALIGN(x, fmt.ptrsize)	/* align x to output format width */

static struct section *get_section_by_name(const char *segname,
                                           const char *sectname)
{
    struct section *s;

    for (s = sects; s != NULL; s = s->next)
        if (!strcmp(s->segname, segname) && !strcmp(s->sectname, sectname))
            break;

    return s;
}

static struct section *get_section_by_index(const int32_t index)
{
    struct section *s;

    for (s = sects; s != NULL; s = s->next)
        if (index == s->index)
            break;

    return s;
}

/*
 * Special section numbers which are used to define Mach-O special
 * symbols, which can be used with WRT to provide PIC relocation
 * types.
 */
static int32_t macho_tlvp_sect;
static int32_t macho_gotpcrel_sect;

static void macho_init(void)
{
    sects = NULL;
    sectstail = &sects;

    /* Fake section for absolute symbols */
    absolute_sect.index = NO_SEG;

    syms = NULL;
    symstail = &syms;
    nsyms = 0;
    nlocalsym = 0;
    nextdefsym = 0;
    nundefsym = 0;

    extsyms = raa_init();
    strs = saa_init(1L);

    /* string table starts with a zero byte so index 0 is an empty string */
    saa_wbytes(strs, zero_buffer, 1);
    strslen = 1;

    /* add special symbol for TLVP */
    macho_tlvp_sect = seg_alloc() + 1;
    define_label("..tlvp", macho_tlvp_sect, 0L, NULL, false, false);

}

static void sect_write(struct section *sect,
                       const uint8_t *data, uint32_t len)
{
    saa_wbytes(sect->data, data, len);
    sect->size += len;
}

/*
 * Find a suitable global symbol for a ..gotpcrel or ..tlvp reference
 */
static struct symbol *macho_find_gsym(struct section *s,
				      uint64_t offset, bool exact)
{
    struct rbtree *srb;

    srb = rb_search(s->gsyms, offset);

    if (!srb || (exact && srb->key != offset)) {
        nasm_error(ERR_NONFATAL, "unable to find a suitable %s symbol"
		   " for this reference",
		   s == &absolute_sect ? "absolute" : "global");
        return NULL;
    }

    return container_of(srb, struct symbol, symv);
}

static int64_t add_reloc(struct section *sect, int32_t section,
			 int64_t offset,
			 enum reltype reltype, int bytes)
{
    struct reloc *r;
    struct section *s;
    int32_t fi;
    int64_t adjust;

    /* Double check this is a valid relocation type for this platform */
    nasm_assert(reltype <= fmt.maxreltype);

    /* the current end of the section will be the symbol's address for
     ** now, might have to be fixed by macho_fixup_relocs() later on. make
     ** sure we don't make the symbol scattered by setting the highest
     ** bit by accident */
    r = nasm_malloc(sizeof(struct reloc));
    r->addr = sect->size & ~R_SCATTERED;
    r->ext = 1;
    adjust = bytes;

    /* match byte count 1, 2, 4, 8 to length codes 0, 1, 2, 3 respectively */
    r->length = ilog2_32(bytes);

    /* set default relocation values */
    r->type = fmt.reloc_abs;
    r->pcrel = 0;
    r->snum = R_ABS;

    s = NULL;
    if (section != NO_SEG)
	s = get_section_by_index(section);
    fi = s ? s->fileindex : NO_SECT;

    /* absolute relocation */
    switch (reltype) {
    case RL_ABS:
	if (section == NO_SEG) {
	    /* absolute (can this even happen?) */
	    r->ext = 0;
	    r->snum = R_ABS;
	} else if (fi == NO_SECT) {
	    /* external */
	    r->snum = raa_read(extsyms, section);
	} else {
	    /* local */
	    r->ext = 0;
	    r->snum = fi;
	    adjust = -sect->size;
	}
	break;

    case RL_REL:
    case RL_BRANCH:
	r->type = fmt.reloc_rel;
	r->pcrel = 1;
	if (section == NO_SEG) {
	    /* absolute - seems to produce garbage no matter what */
	    nasm_error(ERR_NONFATAL, "Mach-O does not support relative "
		       "references to absolute addresses");
	    goto bail;
#if 0
	    /* This "seems" to be how it ought to work... */

	    struct symbol *sym = macho_find_gsym(&absolute_sect,
						 offset, false);
	    if (!sym)
		goto bail;

	    sect->extreloc = 1;
	    r->snum = NO_SECT;
	    adjust = -sect->size;
#endif
	} else if (fi == NO_SECT) {
	    /* external */
	    sect->extreloc = 1;
	    r->snum = raa_read(extsyms, section);
	    if (reltype == RL_BRANCH)
		r->type = X86_64_RELOC_BRANCH;
	    else if (r->type == GENERIC_RELOC_VANILLA)
		adjust = -sect->size;
	} else {
	    /* local */
	    r->ext = 0;
	    r->snum = fi;
	    adjust = -sect->size;
	}
	break;

    case RL_SUB:
	r->pcrel = 0;
	r->type = X86_64_RELOC_SUBTRACTOR;
	break;

    case RL_GOT:
	r->type = X86_64_RELOC_GOT;
	goto needsym;

    case RL_GOTLOAD:
	r->type = X86_64_RELOC_GOT_LOAD;
	goto needsym;

    case RL_TLV:
	r->type = fmt.reloc_tlv;
	goto needsym;

    needsym:
	r->pcrel = 1;
	if (section == NO_SEG) {
	    nasm_error(ERR_NONFATAL, "Unsupported use of use of WRT");
	} else if (fi == NO_SECT) {
	    /* external */
	    r->snum = raa_read(extsyms, section);
	} else {
	    /* internal */
	    struct symbol *sym = macho_find_gsym(s, offset, reltype != RL_TLV);
	    if (!sym)
		goto bail;
	    r->snum = sym->initial_snum;
	}
	break;
    }

    /* NeXT as puts relocs in reversed order (address-wise) into the
     ** files, so we do the same, doesn't seem to make much of a
     ** difference either way */
    r->next = sect->relocs;
    sect->relocs = r;
    if (r->ext)
	sect->extreloc = 1;
    ++sect->nreloc;

    return adjust;

 bail:
    nasm_free(r);
    return 0;
}

static void macho_output(int32_t secto, const void *data,
			 enum out_type type, uint64_t size,
                         int32_t section, int32_t wrt)
{
    struct section *s;
    int64_t addr, offset;
    uint8_t mydata[16], *p;
    bool is_bss;
    enum reltype reltype;

    if (secto == NO_SEG) {
        if (type != OUT_RESERVE)
            nasm_error(ERR_NONFATAL, "attempt to assemble code in "
                  "[ABSOLUTE] space");
        return;
    }

    s = get_section_by_index(secto);

    if (s == NULL) {
        nasm_error(ERR_WARNING, "attempt to assemble code in"
              " section %d: defaulting to `.text'", secto);
        s = get_section_by_name("__TEXT", "__text");

        /* should never happen */
        if (s == NULL)
            nasm_panic(0, "text section not found");
    }

    is_bss = (s->flags & SECTION_TYPE) == S_ZEROFILL;

    if (is_bss && type != OUT_RESERVE) {
        nasm_error(ERR_WARNING, "attempt to initialize memory in "
              "BSS section: ignored");
        s->size += realsize(type, size);
        return;
    }

    memset(mydata, 0, sizeof(mydata));

    switch (type) {
    case OUT_RESERVE:
        if (!is_bss) {
            nasm_error(ERR_WARNING, "uninitialized space declared in"
		       " %s,%s section: zeroing", s->segname, s->sectname);

            sect_write(s, NULL, size);
        } else
            s->size += size;

        break;

    case OUT_RAWDATA:
        if (section != NO_SEG)
            nasm_panic(0, "OUT_RAWDATA with other than NO_SEG");

        sect_write(s, data, size);
        break;

    case OUT_ADDRESS:
    {
	int asize = abs((int)size);

        addr = *(int64_t *)data;
        if (section != NO_SEG) {
            if (section % 2) {
                nasm_error(ERR_NONFATAL, "Mach-O format does not support"
                      " section base references");
            } else if (wrt == NO_SEG) {
		if (fmt.ptrsize == 8 && asize != 8) {
		    nasm_error(ERR_NONFATAL,
			       "Mach-O 64-bit format does not support"
			       " 32-bit absolute addresses");
		} else {
		    add_reloc(s, section, addr, RL_ABS, asize);
		}
	    } else {
		nasm_error(ERR_NONFATAL, "Mach-O format does not support"
			   " this use of WRT");
	    }
	}

        p = mydata;
	WRITEADDR(p, addr, asize);
        sect_write(s, mydata, asize);
        break;
    }

    case OUT_REL2ADR:
	nasm_assert(section != secto);

        p = mydata;
	offset = *(int64_t *)data;
        addr = offset - size;

        if (section != NO_SEG && section % 2) {
            nasm_error(ERR_NONFATAL, "Mach-O format does not support"
		       " section base references");
	} else if (fmt.ptrsize == 8) {
	    nasm_error(ERR_NONFATAL, "Unsupported non-32-bit"
		       " Macho-O relocation [2]");
	} else if (wrt != NO_SEG) {
	    nasm_error(ERR_NONFATAL, "Mach-O format does not support"
		       " this use of WRT");
	    wrt = NO_SEG;	/* we can at least _try_ to continue */
	} else {
	    addr += add_reloc(s, section, addr+size, RL_REL, 2);
	}

        WRITESHORT(p, addr);
        sect_write(s, mydata, 2);
        break;

    case OUT_REL4ADR:
	nasm_assert(section != secto);

        p = mydata;
	offset = *(int64_t *)data;
        addr = offset - size;
	reltype = RL_REL;

        if (section != NO_SEG && section % 2) {
            nasm_error(ERR_NONFATAL, "Mach-O format does not support"
		       " section base references");
        } else if (wrt == NO_SEG) {
	    if (fmt.ptrsize == 8 &&
		(s->flags & S_ATTR_SOME_INSTRUCTIONS)) {
		uint8_t opcode[2];

		opcode[0] = opcode[1] = 0;

		/* HACK: Retrieve instruction opcode */
		if (likely(s->data->datalen >= 2)) {
		    saa_fread(s->data, s->data->datalen-2, opcode, 2);
		} else if (s->data->datalen == 1) {
		    saa_fread(s->data, 0, opcode+1, 1);
		}

		if ((opcode[0] != 0x0f && (opcode[1] & 0xfe) == 0xe8) ||
		    (opcode[0] == 0x0f && (opcode[1] & 0xf0) == 0x80)) {
		    /* Direct call, jmp, or jcc */
		    reltype = RL_BRANCH;
		}
	    }
	} else if (wrt == macho_gotpcrel_sect) {
	    reltype = RL_GOT;

	    if ((s->flags & S_ATTR_SOME_INSTRUCTIONS) &&
		s->data->datalen >= 3) {
		uint8_t gotload[3];

		/* HACK: Retrieve instruction opcode */
		saa_fread(s->data, s->data->datalen-3, gotload, 3);
		if ((gotload[0] & 0xf8) == 0x48 &&
		    gotload[1] == 0x8b &&
		    (gotload[2] & 0307) == 0005) {
		    /* movq <reg>,[rel sym wrt ..gotpcrel] */
		    reltype = RL_GOTLOAD;
		}
	    }
	} else if (wrt == macho_tlvp_sect) {
	    reltype = RL_TLV;
	} else {
	    nasm_error(ERR_NONFATAL, "Mach-O format does not support"
		       " this use of WRT");
	    /* continue with RL_REL */
	}

	addr += add_reloc(s, section, offset, reltype, 4);
        WRITELONG(p, addr);
        sect_write(s, mydata, 4);
        break;

    default:
        nasm_error(ERR_NONFATAL, "Unrepresentable relocation in Mach-O");
        break;
    }
}

static int32_t macho_section(char *name, int pass, int *bits)
{
    char *sectionAttributes;
    const struct sectmap *sm;
    struct section *s;
    const char *section, *segment;
    uint32_t flags;
    char *currentAttribute;
    char *comma;
    bool new_seg;

    (void)pass;

    /* Default to the appropriate number of bits. */
    if (!name) {
        *bits = fmt.ptrsize << 3;
        name = ".text";
        sectionAttributes = NULL;
    } else {
        sectionAttributes = name;
        name = nasm_strsep(&sectionAttributes, " \t");
    }

    section = segment = NULL;
    flags = 0;

    comma = strchr(name, ',');
    if (comma) {
	int len;

	*comma = '\0';
	segment = name;
	section = comma+1;

	len = strlen(segment);
	if (len == 0) {
	    nasm_error(ERR_NONFATAL, "empty segment name\n");
	} else if (len >= 16) {
	    nasm_error(ERR_NONFATAL, "segment name %s too long\n", segment);
	}

	len = strlen(section);
	if (len == 0) {
	    nasm_error(ERR_NONFATAL, "empty section name\n");
	} else if (len >= 16) {
	    nasm_error(ERR_NONFATAL, "section name %s too long\n", section);
	}

	if (!strcmp(section, "__text")) {
	    flags = S_REGULAR | S_ATTR_SOME_INSTRUCTIONS |
		S_ATTR_PURE_INSTRUCTIONS;
	} else if (!strcmp(section, "__bss")) {
	    flags = S_ZEROFILL;
	} else {
	    flags = S_REGULAR;
	}
    } else {
	for (sm = sectmap; sm->nasmsect != NULL; ++sm) {
	    /* make lookup into section name translation table */
	    if (!strcmp(name, sm->nasmsect)) {
		segment = sm->segname;
		section = sm->sectname;
		flags = sm->flags;
		goto found;
	    }
	}
	nasm_error(ERR_NONFATAL, "unknown section name\n");
	return NO_SEG;
    }

 found:
    /* try to find section with that name */
    s = get_section_by_name(segment, section);

    /* create it if it doesn't exist yet */
    if (!s) {
	new_seg = true;

	s = *sectstail = nasm_zalloc(sizeof(struct section));
	sectstail = &s->next;

	s->data = saa_init(1L);
	s->index = seg_alloc();
	s->fileindex = ++seg_nsects;
	s->align = -1;
	s->pad = -1;
	s->offset = -1;
	s->by_name = false;

	xstrncpy(s->segname, segment);
	xstrncpy(s->sectname, section);
	s->size = 0;
	s->nreloc = 0;
	s->flags = flags;
    } else {
	new_seg = false;
    }

    if (comma)
	*comma = ',';		/* Restore comma */

    s->by_name = s->by_name || comma; /* Was specified by name */

    flags = (uint32_t)-1;

    while ((NULL != sectionAttributes)
	   && (currentAttribute = nasm_strsep(&sectionAttributes, " \t"))) {
	if (0 != *currentAttribute) {
	    if (!nasm_strnicmp("align=", currentAttribute, 6)) {
		char *end;
		int newAlignment, value;

		value = strtoul(currentAttribute + 6, (char**)&end, 0);
		newAlignment = alignlog2_32(value);

		if (0 != *end) {
		    nasm_error(ERR_NONFATAL,
			       "unknown or missing alignment value \"%s\" "
			       "specified for section \"%s\"",
			       currentAttribute + 6,
			       name);
		} else if (0 > newAlignment) {
		    nasm_error(ERR_NONFATAL,
			       "alignment of %d (for section \"%s\") is not "
			       "a power of two",
			       value,
			       name);
		}

		if (s->align < newAlignment)
		    s->align = newAlignment;
	    } else if (!nasm_stricmp("data", currentAttribute)) {
		flags = S_REGULAR;
	    } else if (!nasm_stricmp("code", currentAttribute) ||
		       !nasm_stricmp("text", currentAttribute)) {
		flags = S_REGULAR | S_ATTR_SOME_INSTRUCTIONS |
		    S_ATTR_PURE_INSTRUCTIONS;
	    } else if (!nasm_stricmp("mixed", currentAttribute)) {
		flags = S_REGULAR | S_ATTR_SOME_INSTRUCTIONS;
	    } else if (!nasm_stricmp("bss", currentAttribute)) {
		flags = S_ZEROFILL;
	    } else {
		nasm_error(ERR_NONFATAL,
			   "unknown section attribute %s for section %s",
			   currentAttribute,
			   name);
	    }
	}

	if (flags != (uint32_t)-1) {
	    if (!new_seg && s->flags != flags) {
		nasm_error(ERR_NONFATAL,
			   "inconsistent section attributes for section %s\n",
			   name);
	    } else {
		s->flags = flags;
	    }
	}
    }

    return s->index;
}

static void macho_symdef(char *name, int32_t section, int64_t offset,
                         int is_global, char *special)
{
    struct symbol *sym;

    if (special) {
        nasm_error(ERR_NONFATAL, "The Mach-O output format does "
              "not support any special symbol types");
        return;
    }

    if (is_global == 3) {
        nasm_error(ERR_NONFATAL, "The Mach-O format does not "
              "(yet) support forward reference fixups.");
        return;
    }

    if (name[0] == '.' && name[1] == '.' && name[2] != '@') {
	/*
	 * This is a NASM special symbol. We never allow it into
	 * the Macho-O symbol table, even if it's a valid one. If it
	 * _isn't_ a valid one, we should barf immediately.
	 */
	if (strcmp(name, "..gotpcrel") && strcmp(name, "..tlvp"))
            nasm_error(ERR_NONFATAL, "unrecognized special symbol `%s'", name);
	return;
    }

    sym = *symstail = nasm_zalloc(sizeof(struct symbol));
    sym->next = NULL;
    symstail = &sym->next;

    sym->name = name;
    sym->strx = strslen;
    sym->type = 0;
    sym->desc = 0;
    sym->symv.key = offset;
    sym->initial_snum = -1;

    /* external and common symbols get N_EXT */
    if (is_global != 0) {
        sym->type |= N_EXT;
    }

    if (section == NO_SEG) {
        /* symbols in no section get absolute */
        sym->type |= N_ABS;
        sym->sect = NO_SECT;

	/* all absolute symbols are available to use as references */
	absolute_sect.gsyms = rb_insert(absolute_sect.gsyms, &sym->symv);
    } else {
	struct section *s = get_section_by_index(section);

        sym->type |= N_SECT;

        /* get the in-file index of the section the symbol was defined in */
        sym->sect = s ? s->fileindex : NO_SECT;

	/* track the initially allocated symbol number for use in future fix-ups */
	sym->initial_snum = nsyms;

        if (!s) {
            /* remember symbol number of references to external
             ** symbols, this works because every external symbol gets
             ** its own section number allocated internally by nasm and
             ** can so be used as a key */
	    extsyms = raa_write(extsyms, section, nsyms);

            switch (is_global) {
            case 1:
            case 2:
                /* there isn't actually a difference between global
                 ** and common symbols, both even have their size in
                 ** sym->symv.key */
                sym->type = N_EXT;
                break;

            default:
                /* give an error on unfound section if it's not an
                 ** external or common symbol (assemble_file() does a
                 ** seg_alloc() on every call for them) */
                nasm_panic(0, "in-file index for section %d not found, is_global = %d", section, is_global);
		break;
            }
	} else if (is_global) {
	    s->gsyms = rb_insert(s->gsyms, &sym->symv);
	}
    }
    ++nsyms;
}

static void macho_sectalign(int32_t seg, unsigned int value)
{
    struct section *s;
    int align;

    nasm_assert(!(seg & 1));

    s = get_section_by_index(seg);

    if (!s || !is_power2(value))
        return;

    align = alignlog2_32(value);
    if (s->align < align)
        s->align = align;
}

static int32_t macho_segbase(int32_t section)
{
    return section;
}

static void macho_filename(char *inname, char *outname)
{
    standard_extension(inname, outname, ".o");
}

extern macros_t macho_stdmac[];

/* Comparison function for qsort symbol layout.  */
static int layout_compare (const struct symbol **s1,
			   const struct symbol **s2)
{
    return (strcmp ((*s1)->name, (*s2)->name));
}

/* The native assembler does a few things in a similar function

	* Remove temporary labels
	* Sort symbols according to local, external, undefined (by name)
	* Order the string table

   We do not remove temporary labels right now.

   numsyms is the total number of symbols we have. strtabsize is the
   number entries in the string table.  */

static void macho_layout_symbols (uint32_t *numsyms,
				  uint32_t *strtabsize)
{
    struct symbol *sym, **symp;
    uint32_t i,j;

    *numsyms = 0;
    *strtabsize = sizeof (char);

    symp = &syms;

    while ((sym = *symp)) {
	/* Undefined symbols are now external.  */
	if (sym->type == N_UNDF)
	    sym->type |= N_EXT;

	if ((sym->type & N_EXT) == 0) {
	    sym->snum = *numsyms;
	    *numsyms = *numsyms + 1;
	    nlocalsym++;
	}
	else {
		if ((sym->type & N_TYPE) != N_UNDF) {
			nextdefsym++;
	    } else {
			nundefsym++;
		}

	    /* If we handle debug info we'll want
	       to check for it here instead of just
	       adding the symbol to the string table.  */
	    sym->strx = *strtabsize;
	    saa_wbytes (strs, sym->name, (int32_t)(strlen(sym->name) + 1));
	    *strtabsize += strlen(sym->name) + 1;
	}
	symp = &(sym->next);
    }

    /* Next, sort the symbols.  Most of this code is a direct translation from
       the Apple cctools symbol layout. We need to keep compatibility with that.  */
    /* Set the indexes for symbol groups into the symbol table */
    ilocalsym = 0;
    iextdefsym = nlocalsym;
    iundefsym = nlocalsym + nextdefsym;

    /* allocate arrays for sorting externals by name */
    extdefsyms = nasm_malloc(nextdefsym * sizeof(struct symbol *));
    undefsyms = nasm_malloc(nundefsym * sizeof(struct symbol *));

    i = 0;
    j = 0;

    symp = &syms;

    while ((sym = *symp)) {

	if((sym->type & N_EXT) == 0) {
	    sym->strx = *strtabsize;
	    saa_wbytes (strs, sym->name, (int32_t)(strlen (sym->name) + 1));
	    *strtabsize += strlen(sym->name) + 1;
	}
	else {
		if((sym->type & N_TYPE) != N_UNDF) {
			extdefsyms[i++] = sym;
	    } else {
			undefsyms[j++] = sym;
		}
	}
	symp = &(sym->next);
    }

    qsort(extdefsyms, nextdefsym, sizeof(struct symbol *),
	  (int (*)(const void *, const void *))layout_compare);
    qsort(undefsyms, nundefsym, sizeof(struct symbol *),
	  (int (*)(const void *, const void *))layout_compare);

    for(i = 0; i < nextdefsym; i++) {
	extdefsyms[i]->snum = *numsyms;
	*numsyms += 1;
    }
    for(j = 0; j < nundefsym; j++) {
	undefsyms[j]->snum = *numsyms;
	*numsyms += 1;
    }
}

/* Calculate some values we'll need for writing later.  */

static void macho_calculate_sizes (void)
{
    struct section *s;
    int fi;

    /* count sections and calculate in-memory and in-file offsets */
    for (s = sects; s != NULL; s = s->next) {
        uint64_t newaddr;

        /* recalculate segment address based on alignment and vm size */
        s->addr = seg_vmsize;

        /* we need section alignment to calculate final section address */
        if (s->align == -1)
            s->align = DEFAULT_SECTION_ALIGNMENT;

	newaddr = ALIGN(s->addr, 1 << s->align);
        s->addr = newaddr;

        seg_vmsize = newaddr + s->size;

        /* zerofill sections aren't actually written to the file */
        if ((s->flags & SECTION_TYPE) != S_ZEROFILL) {
	    /*
	     * LLVM/Xcode as always aligns the section data to 4
	     * bytes; there is a comment in the LLVM source code that
	     * perhaps aligning to pointer size would be better.
	     */
	    s->pad = ALIGN(seg_filesize, 4) - seg_filesize;
	    s->offset = seg_filesize + s->pad;
            seg_filesize += s->size + s->pad;
	}
    }

    /* calculate size of all headers, load commands and sections to
    ** get a pointer to the start of all the raw data */
    if (seg_nsects > 0) {
        ++head_ncmds;
        head_sizeofcmds += fmt.segcmd_size  + seg_nsects * fmt.sectcmd_size;
    }

    if (nsyms > 0) {
	++head_ncmds;
	head_sizeofcmds += MACHO_SYMCMD_SIZE;
    }

    if (seg_nsects > MAX_SECT) {
	nasm_fatal(0, "MachO output is limited to %d sections\n",
		   MAX_SECT);
    }

    /* Create a table of sections by file index to avoid linear search */
    sectstab = nasm_malloc((seg_nsects + 1) * sizeof(*sectstab));
    sectstab[NO_SECT] = &absolute_sect;
    for (s = sects, fi = 1; s != NULL; s = s->next, fi++)
	sectstab[fi] = s;
}

/* Write out the header information for the file.  */

static void macho_write_header (void)
{
    fwriteint32_t(fmt.mh_magic, ofile);	/* magic */
    fwriteint32_t(fmt.cpu_type, ofile);	/* CPU type */
    fwriteint32_t(CPU_SUBTYPE_I386_ALL, ofile);	/* CPU subtype */
    fwriteint32_t(MH_OBJECT, ofile);	/* Mach-O file type */
    fwriteint32_t(head_ncmds, ofile);	/* number of load commands */
    fwriteint32_t(head_sizeofcmds, ofile);	/* size of load commands */
    fwriteint32_t(0, ofile);			/* no flags */
    fwritezero(fmt.header_size - 7*4, ofile);	/* reserved fields */
}

/* Write out the segment load command at offset.  */

static uint32_t macho_write_segment (uint64_t offset)
{
    uint64_t rel_base = alignptr(offset + seg_filesize);
    uint32_t s_reloff = 0;
    struct section *s;

    fwriteint32_t(fmt.lc_segment, ofile);        /* cmd == LC_SEGMENT_64 */

    /* size of load command including section load commands */
    fwriteint32_t(fmt.segcmd_size + seg_nsects * fmt.sectcmd_size,
		  ofile);

    /* in an MH_OBJECT file all sections are in one unnamed (name
    ** all zeros) segment */
    fwritezero(16, ofile);
    fwriteptr(0, ofile);		     /* in-memory offset */
    fwriteptr(seg_vmsize, ofile);	     /* in-memory size */
    fwriteptr(offset, ofile);	             /* in-file offset to data */
    fwriteptr(seg_filesize, ofile);	     /* in-file size */
    fwriteint32_t(VM_PROT_DEFAULT, ofile);   /* maximum vm protection */
    fwriteint32_t(VM_PROT_DEFAULT, ofile);   /* initial vm protection */
    fwriteint32_t(seg_nsects, ofile);        /* number of sections */
    fwriteint32_t(0, ofile);		     /* no flags */

    /* emit section headers */
    for (s = sects; s != NULL; s = s->next) {
	if (s->nreloc) {
	    nasm_assert((s->flags & SECTION_TYPE) != S_ZEROFILL);
	    s->flags |= S_ATTR_LOC_RELOC;
	    if (s->extreloc)
		s->flags |= S_ATTR_EXT_RELOC;
	} else if (!strcmp(s->segname, "__DATA") &&
		   !strcmp(s->sectname, "__const") &&
		   !s->by_name &&
		   !get_section_by_name("__TEXT", "__const")) {
	    /*
	     * The MachO equivalent to .rodata can be either
	     * __DATA,__const or __TEXT,__const; the latter only if
	     * there are no relocations.  However, when mixed it is
	     * better to specify the segments explicitly.
	     */
	    xstrncpy(s->segname, "__TEXT");
	}

        nasm_write(s->sectname, sizeof(s->sectname), ofile);
        nasm_write(s->segname, sizeof(s->segname), ofile);
        fwriteptr(s->addr, ofile);
        fwriteptr(s->size, ofile);

        /* dummy data for zerofill sections or proper values */
        if ((s->flags & SECTION_TYPE) != S_ZEROFILL) {
	    nasm_assert(s->pad != (uint32_t)-1);
	    offset += s->pad;
            fwriteint32_t(offset, ofile);
	    offset += s->size;
            /* Write out section alignment, as a power of two.
            e.g. 32-bit word alignment would be 2 (2^2 = 4).  */
            fwriteint32_t(s->align, ofile);
            /* To be compatible with cctools as we emit
            a zero reloff if we have no relocations.  */
            fwriteint32_t(s->nreloc ? rel_base + s_reloff : 0, ofile);
            fwriteint32_t(s->nreloc, ofile);

            s_reloff += s->nreloc * MACHO_RELINFO_SIZE;
        } else {
            fwriteint32_t(0, ofile);
            fwriteint32_t(s->align, ofile);
            fwriteint32_t(0, ofile);
            fwriteint32_t(0, ofile);
        }

        fwriteint32_t(s->flags, ofile);      /* flags */
        fwriteint32_t(0, ofile);	     /* reserved */
        fwriteptr(0, ofile);		     /* reserved */
    }

    rel_padcnt = rel_base - offset;
    offset = rel_base + s_reloff;

    return offset;
}

/* For a given chain of relocs r, write out the entire relocation
   chain to the object file.  */

static void macho_write_relocs (struct reloc *r)
{
    while (r) {
	uint32_t word2;

	fwriteint32_t(r->addr, ofile); /* reloc offset */

	word2 = r->snum;
	word2 |= r->pcrel << 24;
	word2 |= r->length << 25;
	word2 |= r->ext << 27;
	word2 |= r->type << 28;
	fwriteint32_t(word2, ofile); /* reloc data */
	r = r->next;
    }
}

/* Write out the section data.  */
static void macho_write_section (void)
{
    struct section *s;
    struct reloc *r;
    uint8_t *p;
    int32_t len;
    int64_t l;
    union offset {
	uint64_t val;
	uint8_t buf[8];
    } blk;

    for (s = sects; s != NULL; s = s->next) {
	if ((s->flags & SECTION_TYPE) == S_ZEROFILL)
	    continue;

	/* Like a.out Mach-O references things in the data or bss
	 * sections by addresses which are actually relative to the
	 * start of the _text_ section, in the _file_. See outaout.c
	 * for more information. */
	saa_rewind(s->data);
	for (r = s->relocs; r != NULL; r = r->next) {
	    len = (uint32_t)1 << r->length;
	    if (len > 4)	/* Can this ever be an issue?! */
		len = 8;
	    blk.val = 0;
	    saa_fread(s->data, r->addr, blk.buf, len);

	    /* get offset based on relocation type */
#ifdef WORDS_LITTLEENDIAN
	    l = blk.val;
#else
	    l  = blk.buf[0];
	    l += ((int64_t)blk.buf[1]) << 8;
	    l += ((int64_t)blk.buf[2]) << 16;
	    l += ((int64_t)blk.buf[3]) << 24;
	    l += ((int64_t)blk.buf[4]) << 32;
	    l += ((int64_t)blk.buf[5]) << 40;
	    l += ((int64_t)blk.buf[6]) << 48;
	    l += ((int64_t)blk.buf[7]) << 56;
#endif

	    /* If the relocation is internal add to the current section
	       offset. Otherwise the only value we need is the symbol
	       offset which we already have. The linker takes care
	       of the rest of the address.  */
	    if (!r->ext) {
		/* generate final address by section address and offset */
		nasm_assert(r->snum <= seg_nsects);
		l += sectstab[r->snum]->addr;
		if (r->pcrel)
		    l -= s->addr;
	    } else if (r->pcrel && r->type == GENERIC_RELOC_VANILLA) {
		l -= s->addr;
	    }

	    /* write new offset back */
	    p = blk.buf;
	    WRITEDLONG(p, l);
	    saa_fwrite(s->data, r->addr, blk.buf, len);
	}

	/* dump the section data to file */
	fwritezero(s->pad, ofile);
	saa_fpwrite(s->data, ofile);
    }

    /* pad last section up to reloc entries on pointer boundary */
    fwritezero(rel_padcnt, ofile);

    /* emit relocation entries */
    for (s = sects; s != NULL; s = s->next)
	macho_write_relocs (s->relocs);
}

/* Write out the symbol table. We should already have sorted this
   before now.  */
static void macho_write_symtab (void)
{
    struct symbol *sym;
    uint64_t i;

    /* we don't need to pad here since MACHO_RELINFO_SIZE == 8 */

    for (sym = syms; sym != NULL; sym = sym->next) {
	if ((sym->type & N_EXT) == 0) {
	    fwriteint32_t(sym->strx, ofile);		/* string table entry number */
	    nasm_write(&sym->type, 1, ofile);		/* symbol type */
	    nasm_write(&sym->sect, 1, ofile);		/* section */
	    fwriteint16_t(sym->desc, ofile);		/* description */

	    /* Fix up the symbol value now that we know the final section
	       sizes.  */
	    if (((sym->type & N_TYPE) == N_SECT) && (sym->sect != NO_SECT)) {
		nasm_assert(sym->sect <= seg_nsects);
		sym->symv.key += sectstab[sym->sect]->addr;
	    }

	    fwriteptr(sym->symv.key, ofile);	/* value (i.e. offset) */
	}
    }

    for (i = 0; i < nextdefsym; i++) {
	sym = extdefsyms[i];
	fwriteint32_t(sym->strx, ofile);
	nasm_write(&sym->type, 1, ofile);	/* symbol type */
	nasm_write(&sym->sect, 1, ofile);	/* section */
	fwriteint16_t(sym->desc, ofile);	/* description */

	/* Fix up the symbol value now that we know the final section
	   sizes.  */
	if (((sym->type & N_TYPE) == N_SECT) && (sym->sect != NO_SECT)) {
	    nasm_assert(sym->sect <= seg_nsects);
	    sym->symv.key += sectstab[sym->sect]->addr;
	}

	fwriteptr(sym->symv.key, ofile); /* value (i.e. offset) */
    }

     for (i = 0; i < nundefsym; i++) {
	 sym = undefsyms[i];
	 fwriteint32_t(sym->strx, ofile);
	 nasm_write(&sym->type, 1, ofile);	/* symbol type */
	 nasm_write(&sym->sect, 1, ofile);	/* section */
	 fwriteint16_t(sym->desc, ofile);	/* description */

	/* Fix up the symbol value now that we know the final section
	   sizes.  */
	 if (((sym->type & N_TYPE) == N_SECT) && (sym->sect != NO_SECT)) {
	    nasm_assert(sym->sect <= seg_nsects);
	    sym->symv.key += sectstab[sym->sect]->addr;
	 }

	 fwriteptr(sym->symv.key, ofile); /* value (i.e. offset) */
     }

}

/* Fixup the snum in the relocation entries, we should be
   doing this only for externally referenced symbols. */
static void macho_fixup_relocs (struct reloc *r)
{
    struct symbol *sym;

    while (r != NULL) {
	if (r->ext) {
	    for (sym = syms; sym != NULL; sym = sym->next) {
		if (sym->initial_snum == r->snum) {
		    r->snum = sym->snum;
		    break;
		}
	    }
	}
	r = r->next;
    }
}

/* Write out the object file.  */

static void macho_write (void)
{
    uint64_t offset = 0;

    /* mach-o object file structure:
    **
    ** mach header
    **  uint32_t magic
    **  int   cpu type
    **  int   cpu subtype
    **  uint32_t mach file type
    **  uint32_t number of load commands
    **  uint32_t size of all load commands
    **   (includes section struct size of segment command)
    **  uint32_t flags
    **
    ** segment command
    **  uint32_t command type == LC_SEGMENT[_64]
    **  uint32_t size of load command
    **   (including section load commands)
    **  char[16] segment name
    **  pointer  in-memory offset
    **  pointer  in-memory size
    **  pointer  in-file offset to data area
    **  pointer  in-file size
    **   (in-memory size excluding zerofill sections)
    **  int   maximum vm protection
    **  int   initial vm protection
    **  uint32_t number of sections
    **  uint32_t flags
    **
    ** section commands
    **   char[16] section name
    **   char[16] segment name
    **   pointer  in-memory offset
    **   pointer  in-memory size
    **   uint32_t in-file offset
    **   uint32_t alignment
    **    (irrelevant in MH_OBJECT)
    **   uint32_t in-file offset of relocation entires
    **   uint32_t number of relocations
    **   uint32_t flags
    **   uint32_t reserved
    **   uint32_t reserved
    **
    ** symbol table command
    **  uint32_t command type == LC_SYMTAB
    **  uint32_t size of load command
    **  uint32_t symbol table offset
    **  uint32_t number of symbol table entries
    **  uint32_t string table offset
    **  uint32_t string table size
    **
    ** raw section data
    **
    ** padding to pointer boundary
    **
    ** relocation data (struct reloc)
    ** int32_t offset
    **  uint data (symbolnum, pcrel, length, extern, type)
    **
    ** symbol table data (struct nlist)
    **  int32_t  string table entry number
    **  uint8_t type
    **   (extern, absolute, defined in section)
    **  uint8_t section
    **   (0 for global symbols, section number of definition (>= 1, <=
    **   254) for local symbols, size of variable for common symbols
    **   [type == extern])
    **  int16_t description
    **   (for stab debugging format)
    **  pointer value (i.e. file offset) of symbol or stab offset
    **
    ** string table data
    **  list of null-terminated strings
    */

    /* Emit the Mach-O header.  */
    macho_write_header();

    offset = fmt.header_size + head_sizeofcmds;

    /* emit the segment load command */
    if (seg_nsects > 0)
	offset = macho_write_segment (offset);
    else
        nasm_error(ERR_WARNING, "no sections?");

    if (nsyms > 0) {
        /* write out symbol command */
        fwriteint32_t(LC_SYMTAB, ofile); /* cmd == LC_SYMTAB */
        fwriteint32_t(MACHO_SYMCMD_SIZE, ofile); /* size of load command */
        fwriteint32_t(offset, ofile);    /* symbol table offset */
        fwriteint32_t(nsyms, ofile);     /* number of symbol
                                         ** table entries */
        offset += nsyms * fmt.nlist_size;
        fwriteint32_t(offset, ofile);    /* string table offset */
        fwriteint32_t(strslen, ofile);   /* string table size */
    }

    /* emit section data */
    if (seg_nsects > 0)
	macho_write_section ();

    /* emit symbol table if we have symbols */
    if (nsyms > 0)
	macho_write_symtab ();

    /* we don't need to pad here, we are already aligned */

    /* emit string table */
    saa_fpwrite(strs, ofile);
}
/* We do quite a bit here, starting with finalizing all of the data
   for the object file, writing, and then freeing all of the data from
   the file.  */

static void macho_cleanup(void)
{
    struct section *s;
    struct reloc *r;
    struct symbol *sym;

    /* Sort all symbols.  */
    macho_layout_symbols (&nsyms, &strslen);

    /* Fixup relocation entries */
    for (s = sects; s != NULL; s = s->next) {
	macho_fixup_relocs (s->relocs);
    }

    /* First calculate and finalize needed values.  */
    macho_calculate_sizes();
    macho_write();

    /* free up everything */
    while (sects->next) {
        s = sects;
        sects = sects->next;

        saa_free(s->data);
        while (s->relocs != NULL) {
            r = s->relocs;
            s->relocs = s->relocs->next;
            nasm_free(r);
        }

        nasm_free(s);
    }

    saa_free(strs);
    raa_free(extsyms);

    while (syms) {
       sym = syms;
       syms = syms->next;
       nasm_free (sym);
    }

    nasm_free(extdefsyms);
    nasm_free(undefsyms);
    nasm_free(sectstab);
}

#ifdef OF_MACHO32
static const struct macho_fmt macho32_fmt = {
    4,
    MH_MAGIC,
    CPU_TYPE_I386,
    LC_SEGMENT,
    MACHO_HEADER_SIZE,
    MACHO_SEGCMD_SIZE,
    MACHO_SECTCMD_SIZE,
    MACHO_NLIST_SIZE,
    RL_MAX_32,
    GENERIC_RELOC_VANILLA,
    GENERIC_RELOC_VANILLA,
    GENERIC_RELOC_TLV
};

static void macho32_init(void)
{
    fmt = macho32_fmt;
    macho_init();

    macho_gotpcrel_sect = NO_SEG;
}

const struct ofmt of_macho32 = {
    "NeXTstep/OpenStep/Rhapsody/Darwin/MacOS X (i386) object files",
    "macho32",
    0,
    32,
    null_debug_arr,
    &null_debug_form,
    macho_stdmac,
    macho32_init,
    null_setinfo,
    macho_output,
    macho_symdef,
    macho_section,
    macho_sectalign,
    macho_segbase,
    null_directive,
    macho_filename,
    macho_cleanup
};
#endif

#ifdef OF_MACHO64
static const struct macho_fmt macho64_fmt = {
    8,
    MH_MAGIC_64,
    CPU_TYPE_X86_64,
    LC_SEGMENT_64,
    MACHO_HEADER64_SIZE,
    MACHO_SEGCMD64_SIZE,
    MACHO_SECTCMD64_SIZE,
    MACHO_NLIST64_SIZE,
    RL_MAX_64,
    X86_64_RELOC_UNSIGNED,
    X86_64_RELOC_SIGNED,
    X86_64_RELOC_TLV
};

static void macho64_init(void)
{
    fmt = macho64_fmt;
    macho_init();

    /* add special symbol for ..gotpcrel */
    macho_gotpcrel_sect = seg_alloc() + 1;
    define_label("..gotpcrel", macho_gotpcrel_sect, 0L, NULL, false, false);
}

const struct ofmt of_macho64 = {
    "NeXTstep/OpenStep/Rhapsody/Darwin/MacOS X (x86_64) object files",
    "macho64",
    0,
    64,
    null_debug_arr,
    &null_debug_form,
    macho_stdmac,
    macho64_init,
    null_setinfo,
    macho_output,
    macho_symdef,
    macho_section,
    macho_sectalign,
    macho_segbase,
    null_directive,
    macho_filename,
    macho_cleanup
};
#endif

#endif

/*
 * Local Variables:
 * mode:c
 * c-basic-offset:4
 * End:
 *
 * end of file */
