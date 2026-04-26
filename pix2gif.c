/* pix2gif.c V7/3
 *
 * MG1 to GIF format converter.
 *
 * Converts Infocom MG1/EG1 picture files to separate GIF files.
 *
 * usage: pix2gif picture-file
 *
 * Mark Howell 13 September 1992 howell_ma@movies.enet.dec.com
 *
 * History:
 *    Make stack allocated arrays static
 *    Fix lint warnings
 *    Fix 64KB MS-DOS problems
 *    Handle transparent colours properly
 *    Put transparency information into GIF (now version 89a)
 */

#include "pix2gif.h"

#include <string.h>

#ifdef __MSDOS__
#include <alloc.h>
#define malloc(n) farmalloc(n)
#define calloc(n,s) farcalloc(n,s)
#define free(p) farfree(p)
#endif

static short mask[16] = {
    0x0000, 0x0001, 0x0003, 0x0007,
    0x000f, 0x001f, 0x003f, 0x007f,
    0x00ff, 0x01ff, 0x03ff, 0x07ff,
    0x0fff, 0x1fff, 0x3fff, 0x7fff
};

static unsigned char ega_colourmap[16][3] = {
        {   0,  0,  0 },
        {   0,  0,170 },
        {   0,170,  0 },
        {   0,170,170 },
        { 170,  0,  0 },
        { 170,  0,170 },
        { 170,170,  0 },
        { 170,170,170 },
        {  85, 85, 85 },
        {  85, 85,255 },
        {  85,255, 85 },
        {  85,255,255 },
        { 255, 85, 85 },
        { 255, 85,255 },
        { 255,255, 85 },
        { 255,255,255 }
};

#if defined(AMIGA) && defined(_DCC)
__far
#endif
static nlist_t *hash_table[HASH_SIZE];
static unsigned char colourmap[16][3];
static unsigned char code_buffer[CODE_TABLE_SIZE];
static char file_name[FILENAME_MAX + 1];
static short code_table[CODE_TABLE_SIZE][2];
static unsigned char buffer[CODE_TABLE_SIZE];
static pchunk_t *adaptive_list;
static pchunk_t *resolution_list;
static header_t header;

static void process_image (FILE *, pdirectory_t *, unsigned char, unsigned char* );
static void decompress_image_lzw (FILE *, unsigned, unsigned, image_t *, char bw);
static void decompress_image_huffman (FILE *, unsigned char*, unsigned, unsigned, image_t *);
static short read_code (FILE *, compress_t *);
static void write_file (int, image_t *);
static void write_rectangle (int, image_t *);
static void write_screen (FILE *, image_t *);
static void write_graphic_control (FILE *, image_t *);
static void write_image (FILE *, image_t *);
static void compress_image (FILE *, image_t *);
static void write_code (FILE *, short, compress_t *);
static void insert_code (short, short, short);
static void push_chunk(pchunk_t **, short);
static short pop_chunk(pchunk_t **);
static void write_apal(pchunk_t *);
static void write_reln(unsigned short);
static void write_reso(pchunk_t *, int, int);
static short lookup (short, short);
static void clear_table (void);
static void delete_table (void);
static unsigned char read_byte (FILE *);
static void write_byte (FILE *, int);
static unsigned short read_word (FILE *);
static void write_word (FILE *, unsigned short);
static void read_bytes (FILE *, int, void *);
static void write_bytes (FILE *, int, const void *);

int main (int argc, char *argv[])
{
    int i;
    FILE *fp;
    pdirectory_t *directory;
	unsigned char lookup_table[256];

    adaptive_list = NULL;
    resolution_list = NULL;

    if (argc != 2) {
        (void) fprintf (stderr, "usage: %s picture-file\n\n", argv[0]);
        (void) fprintf (stderr, "PIX2GIF version 7/2 - convert Infocom MG1/EG1 files to GIF. By Mark Howell\n");
        (void) fprintf (stderr, "Works with V6 Infocom games.\n");
        exit (EXIT_FAILURE);
    }

    if ((fp = fopen (argv[1], "rb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    /* Check the first pic number to see if we need to swap bytes when
     * reading words.  Then rewind back to the beginning.
     */
    fseek(fp, H_REVERSE, SEEK_SET);
    header.reverse = read_byte(fp);
    rewind(fp);

    header.part = read_byte (fp);
    header.flags = read_byte (fp);
    header.unknown1 = read_word (fp);
    header.local_count = read_word (fp);
    header.global_ptr = read_word (fp);
    header.entry_size = read_byte (fp);
    header.unknown3 = read_byte (fp);
    header.checksum = read_word (fp);
    header.unknown4 = read_word (fp);
    header.version = read_word (fp);

    (void) printf ("Total number of images is %d.\n", (int) header.local_count);

    /* Print contents of the header */
    printf("part: %d\n", header.part);
    printf("flags: 0x%02X\n", header.flags);
    printf("unknown1: %d, 0x%04X\n", header.unknown1, header.unknown1);
    printf("local_count: %d, 0x%04X\n", header.local_count, header.local_count);
    printf("global_ptr: %d, 0x%04X\n", header.global_ptr, header.global_ptr);
    printf("entry_size: %d, 0x%04X\n", header.entry_size, header.entry_size);
    printf("unknown3: %d, 0x%04X\n", header.unknown3, header.unknown3);
    printf("checksum: %d, 0x%04X\n", header.checksum, header.checksum);
    printf("unknown4: %d, 0x%04X\n", header.unknown4, header.unknown4);
    printf("version: %d, 0x%04X\n", header.version, header.version);
    printf("reverse: %d, 0x%04X\n", header.reverse, header.reverse);

    if ((directory = (pdirectory_t *) calloc ((size_t) header.local_count, sizeof (pdirectory_t))) == NULL) {
        (void) fprintf (stderr, "Insufficient memory\n");
        exit (EXIT_FAILURE);
    }

    for (i = 0; (unsigned int) i < (unsigned int) header.local_count; i++) {
        directory[i].image_number = read_word (fp);
        directory[i].image_width = read_word (fp);
        directory[i].image_height = read_word (fp);
        directory[i].image_flags = read_word (fp);
        directory[i].image_data_addr = (unsigned long) read_byte (fp) << 16;
        directory[i].image_data_addr += (unsigned long) read_byte (fp) << 8;
        directory[i].image_data_addr += (unsigned long) read_byte (fp);
        if ((unsigned int) header.entry_size == 14) {
            directory[i].image_cm_addr = (unsigned long) read_byte (fp) << 16;
            directory[i].image_cm_addr += (unsigned long) read_byte (fp) << 8;
            directory[i].image_cm_addr += (unsigned long) read_byte (fp);
        } else if ((unsigned int) header.entry_size == 16) {
            directory[i].image_cm_addr = (unsigned long) read_byte (fp) << 16;
            directory[i].image_cm_addr += (unsigned long) read_byte (fp) << 8;
            directory[i].image_cm_addr += (unsigned long) read_byte (fp);
            directory[i].image_lookup_addr = 2 * (unsigned long) read_word (fp);
        } else {
            directory[i].image_cm_addr = 0;
            (void) read_byte (fp);
        }
    }

	if (header.flags == 0xE)
		fread(lookup_table, 256, 1, fp);
	
    for (i = 0; (unsigned int) i < (unsigned int) header.local_count; i++) {
        process_image (fp, &directory[i], header.flags, lookup_table);
	}
    free (directory);
    (void) fclose (fp);

    write_reln(header.version);
    write_apal(adaptive_list);
    write_reso(resolution_list, 320, 200);

    exit (EXIT_SUCCESS);

    return (0);

}/* main */

static void process_image (FILE *fp, pdirectory_t *directory, unsigned char flags, unsigned char* lookup_table)
{
    int colours = 16, i;
    image_t image;
	unsigned char huffman_tree_data[256];

    for (i = 0; i < 16; i++) {
        colourmap[i][RED] = ega_colourmap[i][RED];
        colourmap[i][GREEN] = ega_colourmap[i][GREEN];
        colourmap[i][BLUE] = ega_colourmap[i][BLUE];
    }

    if (directory->image_cm_addr) {
        if (fseek (fp, directory->image_cm_addr, SEEK_SET) != 0) {
            perror ("fseek");
            exit (EXIT_FAILURE);
        }
        colours = read_byte (fp);

        /* Fix for some buggy _Arthur_ pictures. */
        if (colours > 14)
            colours = 14;
        read_bytes (fp, colours * 3, (void *) &colourmap[2][RED]);
        colours += 2;
    }
        
    if (directory->image_flags & 1) {
        colourmap[directory->image_flags >> 12][0] = 0;
        colourmap[directory->image_flags >> 12][1] = 0;
        colourmap[directory->image_flags >> 12][2] = 0;
    }

    /* Fix for CGA images that should be simply black and white. */
    if(directory->image_flags & 0x08){
        colourmap[2][0] = 255; colourmap[2][1] = 255; colourmap[2][2] = 255;
        colourmap[3][0] = 0; colourmap[3][1] = 0; colourmap[3][2] = 0;
    }

    (void) printf ("pic %03d   at %d (%d)  size %3d x %3d   %2d colours   colour map ",
            (int) directory->image_number,
            (int) directory->image_data_addr,
            (int) directory->image_lookup_addr,
            (int) directory->image_width, (int) directory->image_height,
            (int) colours);

    if (directory->image_cm_addr != 0)
        (void) printf ("$%05lx", (long) directory->image_cm_addr);
    else
        (void) printf ("------");

    if (directory->image_flags & 1) {
        image.transflag = 1;
        image.transpixel = (unsigned short) directory->image_flags >> 12;
        (void) printf ("   transparent is %u\n", image.transpixel);
    }
    else {
        image.transpixel = 0;
        image.transflag = 0;
        (void) printf ("\n");
    }

    image.width = directory->image_width;
    image.height = directory->image_height;

    if (directory->image_cm_addr == 0)
        push_chunk(&adaptive_list, directory->image_number);

    push_chunk(&resolution_list, directory->image_number);

    if (directory->image_data_addr == 0) {
        write_rectangle(directory->image_number, &image);
        return;
    }

    image.colours = colours;
    image.pixels = 0;
    if ((image.image = (unsigned char *) calloc ((size_t) directory->image_width, (size_t) directory->image_height)) == NULL) {
        (void) fprintf (stderr, "Insufficient memory\n");
        exit (EXIT_FAILURE);
    }
    image.colourmap = colourmap;
        
	if (flags == 0xE) {
		if (directory->image_lookup_addr) {
			if (fseek (fp, directory->image_lookup_addr, SEEK_SET) != 0) {
				perror ("fseek");
				exit (EXIT_FAILURE);
			}
			fread(huffman_tree_data, 1, 256, fp);
		}
		else {
			memcpy(huffman_tree_data, lookup_table, 256);
		}
	}

    if (fseek (fp, directory->image_data_addr, SEEK_SET) != 0) {
        perror ("fseek");
        exit (EXIT_FAILURE);
    }
	
	if (flags == 0xE)
		decompress_image_huffman (fp, huffman_tree_data, image.width, image.height, &image);
	else
		decompress_image_lzw (fp, image.width, image.height, &image, flags == 0x38);

    write_file ((int) directory->image_number, &image);

    free (image.image);

}/* process image */

static void decompress_image_huffman (FILE *fp, unsigned char* tree, unsigned width, unsigned height, image_t *image)
{
	unsigned char header[6];
	unsigned compressed_length;
	unsigned char* data;
    unsigned char* inStream;
    unsigned char* outStream;

	fread(header, 1, 6, fp);
	compressed_length = header[0] * 65536 + header[1] * 256 + header[2];
	data = malloc(compressed_length);
	fread(data, 1, compressed_length, fp);
    inStream = data;
    outStream = image->image;

	unsigned image_size = width * height;
    
    unsigned char decode_mask = 0;
    unsigned char current_byte = 0;
    unsigned char out_value = 0;
    unsigned char current_pixel_to_xor = 0;
    
    image->pixels = 0;

    // Compression method:
    //  Each row except the top one is replaced by a XOR of it with the previous row.
    //  The full stream is RLE compressed.
    //        0x00-0x0F = literal value
    //        0x10-0x7F = repeat last literal value: value = repeat count plus 0x0F
    //  The RLE compressed stream is then Huffman encoded, with a common tree for all
    //  the images, and with leaf nodes indicated by bit 7 set.
    
    while (image->pixels < image_size) {
        if (0 == out_value) {
            do {
                if (decode_mask == 0) {
                    current_byte = *inStream++;
                    decode_mask = 0x80;
                }
                if (current_byte & decode_mask)
                    out_value = tree[2*out_value+1];
                else
                    out_value = tree[2*out_value];
                decode_mask >>= 1;
            } while (! (out_value & 0x80) );
            
            if (out_value <= 0x90) {
                current_pixel_to_xor = out_value & 0x7F;
                out_value = 0; // do once
            }
            else {
                out_value -= 0x90; // repeat count
            }
        }
        else {
            out_value--;
        }
        if (image->pixels++ < width)
            *outStream++ = current_pixel_to_xor;
        else
            *outStream++ = outStream[-(int)width] ^ current_pixel_to_xor;
    }
    free(data);
}

static void decompress_image_lzw (FILE *fp, unsigned width, unsigned height, image_t *image, char bw)
{        
    (void)height;
    int i;
    short code, old = 0, first, clear_code;
    compress_t comp;

    clear_code = 1 << CODE_SIZE;
    comp.next_code = clear_code + 2;
    comp.slen = 0;
    comp.sptr = 0;
    comp.tlen = CODE_SIZE + 1;
    comp.tptr = 0;
    int x=0;

    for (i = 0; i < CODE_TABLE_SIZE; i++) {
        code_table[i][PREFIX] = CODE_TABLE_SIZE;
        code_table[i][PIXEL] = i;
    }

    for (;;) {
        if ((code = read_code (fp, &comp)) == (clear_code + 1)) 
            return;
        if (code == clear_code) {
            comp.tlen = CODE_SIZE + 1;
            comp.next_code = clear_code + 2;
            code = read_code (fp, &comp);
        } else {
            first = (code == comp.next_code) ? old : code;
            while (code_table[first][PREFIX] != CODE_TABLE_SIZE) {
                first = code_table[first][PREFIX];
            }
            code_table[comp.next_code][PREFIX] = old;
            code_table[comp.next_code++][PIXEL] = code_table[first][PIXEL];
        }
        old = code;
        i = 0;
        do
            buffer[i++] = (unsigned char) code_table[code][PIXEL];
        while ((code = code_table[code][PREFIX]) != CODE_TABLE_SIZE);
        do {
			unsigned char c = buffer[--i];
            if (bw && !image->transflag) {
                for (unsigned char m = 0x80 ; m ; m >>= 1) {
                    if (c & m)
                        image->image[image->pixels++] = 2;
                    else
                        image->image[image->pixels++] = 3;
                    x++;
                    if (x>=width) {
                        x = 0;
                        break;
                    }
                }
            }
            else {
                image->image[image->pixels++] = c;
            }
                    
		}
        while (i > 0);
    }
}/* decompress_image */

static short read_code (FILE *fp, compress_t *comp)
{
    short code, bsize, tlen, tptr;

    code = 0;
    tlen = comp->tlen;
    tptr = 0; 

    while (tlen) {
        if (comp->slen == 0) {
            if ((comp->slen = fread (code_buffer, 1, MAX_BIT, fp)) == 0) {
                perror ("fread");
                exit (EXIT_FAILURE);
            }
            comp->slen *= 8;
            comp->sptr = 0;
        }
        bsize = ((comp->sptr + 8) & ~7) - comp->sptr;
        bsize = (tlen > bsize) ? bsize : tlen;
        code |= (((unsigned int) code_buffer[comp->sptr >> 3] >> (comp->sptr & 7)) & mask[bsize]) << tptr;

        tlen -= bsize;
        tptr += bsize;
        comp->slen -= bsize;
        comp->sptr += bsize;
    }
    if ((comp->next_code == mask[comp->tlen]) && (comp->tlen < 12))
        comp->tlen++;

    return (code);

}/* read_code */

static void write_file (int image_number, image_t *image)
{
    FILE *fp;

    (void) sprintf (file_name, "pict_%03d.gif", (int) image_number);

    if ((fp = fopen (file_name, "wb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    write_bytes (fp, sig_k_bln, (const void *) CURRENT_VERSION);
    write_screen (fp, image);
    if (image->transflag) /* save 8 bytes if possible */
        write_graphic_control(fp, image);
    write_image (fp, image);
    compress_image (fp, image);
    write_byte (fp, ';');

    (void) fclose (fp);

}/* write_file */

static void write_rectangle (int image_number, image_t *image)
{
    FILE *fp;
    unsigned char width[4];
    unsigned char height[4];

    width[0] = (image->width >> 24) & 0xff;
    width[1] = (image->width >> 16) & 0xff;
    width[2] = (image->width >> 8) & 0xff;
    width[3] = image->width & 0xff;

    height[0] = (image->height >> 24) & 0xff;
    height[1] = (image->height >> 16) & 0xff;
    height[2] = (image->height >> 8) & 0xff;
    height[3] = image->height & 0xff;

    (void) sprintf (file_name, "pict_%03d.rec", (int) image_number);

    if ((fp = fopen (file_name, "wb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    write_bytes(fp, 4, (void *) width);
    write_bytes(fp, 4, (void *) height);

    fclose(fp);
} /* write_rectangle */

static void write_screen (FILE *fp, image_t *image)
{
    int i;

    for (i = 1; (image->colours - 1) >> i; i++)
        ;

    write_word (fp, (unsigned short) image->width);
    write_word (fp, (unsigned short) image->height);
    write_byte (fp, ((i - 1) & 7) | (((i - 1) & 7) << 4) | (1 << 7));
    write_byte (fp, 0);
    write_byte (fp, 0);

    write_bytes (fp, (1 << i) * 3, (const void *) image->colourmap);

}/* write_screen */

static void write_graphic_control (FILE *fp, image_t *image)
{
    write_byte(fp, '!');               /* Extension Introducer        */
    write_byte(fp, 0xF9);              /* Graphic Control Label       */
    write_byte(fp, 4);                 /* # bytes in block            */
    write_byte(fp, image->transflag);   /* bits 7-5: reserved          */
                                       /* bits 4-2: Disposal Method   */
                                       /* bit 1   : User Input Flag   */
                                       /* bit 0   : Transparency Flag */
    write_byte(fp, 0);                 /* Delay Time LSB              */
    write_byte(fp, 0);                 /* Delay Time MSB              */
    write_byte(fp, image->transpixel);  /* Transparent color index     */
    write_byte(fp, 0);                 /* Block terminator            */
}

static void write_image (FILE *fp, image_t *image)
{

    write_byte (fp, ',');
    write_word (fp, (unsigned short) 0);
    write_word (fp, (unsigned short) 0);
    write_word (fp, (unsigned short) image->width);
    write_word (fp, (unsigned short) image->height);
    write_byte (fp, 0);

}/* write_image */

static void compress_image (FILE *fp, image_t *image)
{
    int init_comp_size;
    long index;
    short code, clear_code, prefix, pixel;
    compress_t comp;

    clear_table ();

    for (init_comp_size = 1; (image->colours - 1) >> init_comp_size; init_comp_size++)
        ;

    clear_code = 1 << init_comp_size++;
    code_buffer[0] = 255;
    index = 0;

    comp.next_code = clear_code + 2;
    comp.slen = init_comp_size;
    comp.sptr = 0;
    comp.tlen = 255 * 8;
    comp.tptr = 8;

    write_byte (fp, init_comp_size - 1);
    write_code (fp, clear_code, &comp);
    prefix = image->image[index++];
    while (index < image->pixels) {
        pixel = image->image[index++];
        code = lookup (prefix, pixel);
        if (code) {
            prefix = code;
        } else {
            write_code (fp, prefix, &comp);
            if (comp.next_code == 4096) {
                delete_table ();
                comp.next_code = clear_code + 2;
                write_code (fp, clear_code, &comp);
                comp.slen = init_comp_size;
            } else
                insert_code (prefix, pixel, comp.next_code++);
            prefix = pixel;
        }
    }
    write_code (fp, prefix, &comp);
    write_code (fp, (short) (clear_code + 1), &comp);
    if (comp.tptr) {
        code_buffer[0] = (unsigned char) ((comp.tptr + 7) >> 3);
        write_bytes (fp, (int) code_buffer[0] + 1, (const void *) code_buffer);
    }
    write_byte (fp, 0);
    delete_table ();

}/* compress_image */

static void write_code (FILE *fp, short code, compress_t *comp)
{
    short bsize, slen, sptr;

    slen = comp->slen;
    sptr = 0;

    while (slen) {
        if (comp->tlen == 0) {
            write_bytes (fp, 256, (const void *) code_buffer);
            comp->tlen = 255 * 8;
            comp->tptr = 8;
        }
        bsize = ((comp->tptr + 8) & ~7) - comp->tptr;
        bsize = (slen > bsize) ? bsize : slen;

        code_buffer[comp->tptr >> 3] = (unsigned char) ((unsigned int) code_buffer[comp->tptr >> 3] & mask[comp->tptr & 7]);
        code_buffer[comp->tptr >> 3] = (unsigned char) ((unsigned int) code_buffer[comp->tptr >> 3] | ((code >> sptr) & mask[bsize]) << (comp->tptr & 7));

        slen -= bsize;
        sptr += bsize;
        comp->tlen -= bsize;
        comp->tptr += bsize;
    }
    if ((comp->next_code == (mask[comp->slen] + 1)) && (comp->slen < 12))
        comp->slen++;

}/* write_code */

static void insert_code (short prefix, short pixel, short code)
{
    short hashval;
    nlist_t *np;

    if ((np = (nlist_t *) malloc (sizeof (*np))) == NULL) {
        (void) fprintf (stderr, "Insufficient memory\n");
        exit (EXIT_FAILURE);
    }
    hashval = (short) hashfunc (prefix, pixel);
    np->next = (nlist_t *) hash_table[hashval];
    hash_table[hashval] = np;
    np->prefix = prefix;
    np->pixel = pixel;
    np->code = code;

}/* insert_code */

static short lookup (short prefix, short pixel)
{
    short hashval;
    nlist_t *np;

    hashval = (short) hashfunc (prefix, pixel);
    for (np = hash_table[hashval]; np != NULL; np = (nlist_t *) np->next)
        if ((np->prefix == prefix) && (np->pixel == pixel))
            return (np->code);

    return (0);

}/* lookup */

static void clear_table (void)
{
    int i;

    for (i = 0; i < HASH_SIZE; i++)
        hash_table[i] = NULL;

}/* clear_table */

static void delete_table (void)
{
    int i;
    nlist_t *np, *tp;

    for (i = 0; i < HASH_SIZE; i++) {
        for (np = hash_table[i]; np != NULL; np = tp) {
            tp = (nlist_t *) np->next;
            free (np);
        }
        hash_table[i] = NULL;
    }

}/* delete_table */

static unsigned char read_byte (FILE *fp)
{
    int c;

    if ((c = fgetc (fp)) == EOF) {
        fprintf(stderr, "read_byte()\n");
        perror ("fgetc");
        exit (EXIT_FAILURE);
    }

    return ((unsigned char) c);

}/* read_byte */

static void write_byte (FILE *fp, int c)
{
    if (fputc (c, fp) == EOF) {
        perror ("fputc");
        exit (EXIT_FAILURE);
    }

}/* write_byte */


/*
 * Read a word from the graphic file.
 *
 * These graphic files are little-endian no matter if it was intended
 * for use on an IBM PC (little-endian) or a Macintosh or Amiga
 * (big-endian).  At offset H_REVERSE is the beginning of the first
 * picture, numbered 001.  If this file is for a big-endian machine,
 * this will appear as 0x00 0x01 in a hex editor.  For a little-endian
 * machine, it will be 0x01 0x00.  So, this isn't really part of the
 * header, but instead a telltale for how to process the file.
 *   -- DG
 *
 */
static unsigned short read_word (FILE *fp)
{
    unsigned int w;

    w = (unsigned int) read_byte (fp);
    w += (unsigned int) read_byte (fp) << 8;

    if (!header.reverse)
        wordswap(w);

    return (w);

}/* read_word */

static void write_word (FILE *fp, unsigned short w)
{

    write_byte (fp, (int) w & 255);
    write_byte (fp, (int) w >> 8);

}/* write_word */

static void read_bytes (FILE *fp, int size, void *s)
{

    if (fread (s, (size_t) size, 1, fp) != 1) {
        perror ("fread");
        exit (EXIT_FAILURE);
    }

}/* read_bytes */

static void write_bytes (FILE *fp, int size, const void *s)
{

    if (fwrite (s, (size_t) size, 1, fp) != 1) {
        perror ("fwrite");
        exit (EXIT_FAILURE);
    }

}/* write_bytes */

static void push_chunk (pchunk_t **list, short image_number)
{
    pchunk_t *temp;

    temp = malloc(sizeof(pchunk_t));

    temp->image_number = image_number;
    temp->next = *list;
    *list = temp;

} /* push_chunk */

static short pop_chunk(pchunk_t **list)
{
    short result;
    pchunk_t *temp = *list;

    if (*list == NULL)
        return -1;

    result = temp->image_number;
    (*list) = (*list)->next;
    free(temp);

    return result;
} /* pop_chunk */

static void write_apal (pchunk_t *list)
{
    FILE *fp;
    pchunk_t *temp;
    short image_number;
    unsigned char entry[4];

    temp = NULL;

    (void) sprintf(file_name, "apal_0.bin");

    if ((fp = fopen (file_name, "wb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    while (-1 != (image_number = pop_chunk(&list)))
        push_chunk(&temp, image_number);

    while (-1 != (image_number = pop_chunk(&temp))) {
        entry[0] = (image_number >> 24) & 0xff;
        entry[1] = (image_number >> 16) & 0xff;
        entry[2] = (image_number >>  8) & 0xff;
        entry[3] = image_number & 0xff;
        write_bytes(fp, 4, (void *) entry);
    }
    fclose(fp);

} /* write_apal */

static void write_reln(unsigned short release_number)
{
    FILE *fp;
    unsigned char entry[2];

    (void) sprintf(file_name, "reln_0.bin");

    if ((fp = fopen (file_name, "wb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    entry[0] = (release_number >>  8) & 0xff;
    entry[1] = release_number & 0xff;
    write_bytes(fp, 2, (void *) entry);

    fclose(fp);

} /* write_reln */


/*
 * The Reso chunk contains information used to scale images.  For
 * Infocom games, all images are scalable.  See
 * http://ifarchive.org/if-archive/programming/blorb/blorb_format.txt
 *
 */
static void write_reso(pchunk_t *list, int width, int height)
{
    FILE *fp;
    pchunk_t *temp;
    short image_number;
    unsigned char entry[4];
    unsigned int number;

    temp = NULL;

    (void) sprintf(file_name, "reso_0.bin");

    if ((fp = fopen (file_name, "wb")) == NULL) {
        perror ("fopen");
        exit (EXIT_FAILURE);
    }

    while (-1 != (image_number = pop_chunk(&list)))
        push_chunk(&temp, image_number);

    /* standard window width */
    entry[0] = (width >> 24) & 0xff;
    entry[1] = (width >> 16) & 0xff;
    entry[2] = (width >>  8) & 0xff;
    entry[3] = width & 0xff;
    write_bytes(fp, 4, (void *) entry);

    /* standard window height */
    entry[0] = (height >> 24) & 0xff;
    entry[1] = (height >> 16) & 0xff;
    entry[2] = (height >>  8) & 0xff;
    entry[3] = height & 0xff;
    write_bytes(fp, 4, (void *) entry);

    /* minimum window width */
    entry[0] = 0;
    entry[1] = 0;
    entry[2] = 0;
    entry[3] = 0;
    write_bytes(fp, 4, (void *) entry);

    /* minimum window height */
    write_bytes(fp, 4, (void *) entry);

    /* maximum window width */
    write_bytes(fp, 4, (void *) entry);

    /* maximum window height */
    write_bytes(fp, 4, (void *) entry);

    while (-1 != (image_number = pop_chunk(&temp))) {

        /* image resource number */
        entry[0] = (image_number >> 24) & 0xff;
        entry[1] = (image_number >> 16) & 0xff;
        entry[2] = (image_number >>  8) & 0xff;
        entry[3] = image_number & 0xff;
        write_bytes(fp, 4, (void *) entry);

        /* numerator of standard ratio */
        entry[0] = 0;
        entry[1] = 0;
        entry[2] = 0;
        entry[3] = 1;
        write_bytes(fp, 4, (void *) entry);

        /* denominator of standard ratio */
        write_bytes(fp, 4, (void *) entry);

        entry[3] = 0;
        /* numerators and denominators of minimum and maximum ratios */
        write_bytes(fp, 4, (void *) entry);
        write_bytes(fp, 4, (void *) entry);
        write_bytes(fp, 4, (void *) entry);
        write_bytes(fp, 4, (void *) entry);
    }
    fclose(fp);

} /* write_reso */

