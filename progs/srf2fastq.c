/*
 * ======================================================================
 * This software has been created by Genome Research Limited (GRL).
 *
 * GRL hereby grants permission to use, copy, modify and distribute
 * this software and its documentation for non-commercial purposes
 * without fee at the user's own risk on the basis set out below.
 *
 * GRL neither undertakes nor accepts any duty whether contractual or
 * otherwise in connection with the software, its use or the use of
 * any derivative, and makes no representations or warranties, express
 * or implied, concerning the software, its suitability, fitness for
 * a particular purpose or non-infringement.
 *
 * In no event shall the authors of the software or GRL be responsible
 * or liable for any loss or damage whatsoever arising in any way
 * directly or indirectly out of the use of this software or its
 * derivatives, even if advised of the possibility of such damage.
 *
 * Our software can be freely distributed under the conditions set out
 * above, and must contain this copyright notice.
 * ======================================================================
 */

/*
 * This performs a linear (non-indexed) search for a trace in an SRF archive.
 *
 * It's not intended as a suitable production program or as a library of code
 * to use, but as a test and benchmark statistic.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>

#include <io_lib/Read.h>
#include <io_lib/misc.h>
#include <io_lib/ztr.h>
#include <io_lib/srf.h>
#include <io_lib/hash_table.h>

#define MAX_REGIONS   4

/* regn chunk */
typedef struct {
    char coord;
    char *region_names;
    int nregions;
    char *name[MAX_REGIONS];
    char code[MAX_REGIONS];
    int start[MAX_REGIONS];
    int length[MAX_REGIONS];
    int index[MAX_REGIONS];
    FILE *file[MAX_REGIONS];
    int count;
} regn_t;

static char qlookup[256];
void init_qlookup(void) {
    int i;
    for (i = -128; i < 128; i++) {
        qlookup[i+128] = '!' + (int)((10*log(1+pow(10, i/10.0))/log(10)+.499));
    }
}

/*
 * Parse the REGN chunk, add to regn HASH
 *
 * Returns corresponding HashItem * from regn Hash
 */
HashItem *parse_regn(ztr_t *z, ztr_chunk_t *chunk, HashTable *regn_hash) {
    char key[1024];
    char *name;
    HashItem *hi;
    regn_t *regn;
    size_t l;
    
    uncompress_chunk(z, chunk);

    /* the hash key is a combination of the region names and boundaries */
    name = ztr_lookup_mdata_value(z, chunk, "NAME");
    l = snprintf(key, sizeof(key), "names=%s", name);
    if( chunk->dlength ){
        int nbndy = (chunk->dlength-1)/4;
        uint4 *bndy = (uint4 *)(chunk->data+1);
        int ibndy;
	for (ibndy=0; ibndy<nbndy; ibndy++) {
            if( ibndy )
                l += snprintf(key + l, sizeof(key) - l,
			      ";%d", be_int4(bndy[ibndy]));
            else
                l += snprintf(key + l, sizeof(key) - l,
			      " boundaries=%d", be_int4(bndy[ibndy]));
        }
    }

    if (NULL == (hi = (HashTableSearch(regn_hash, key, strlen(key))))) {
        int iregion, nregions = 0, index = 1;
        char *coord;
	char *cp1;
        uint4 bndy[MAX_REGIONS];
        int ibndy, nbndy = 0;
        HashData hd;

        if( NULL == (regn = (regn_t *)malloc(sizeof(regn_t)))) {
	    return NULL;
	}

	coord = ztr_lookup_mdata_value(z, chunk, "COORD");
	regn->coord = (NULL == coord ? 'B' : *coord );

	regn->region_names = strdup(name);

        cp1 = strtok (regn->region_names,";");
        while(cp1) {
            char *cp2;
            if(NULL == (cp2 = strchr(cp1,':'))) {
                fprintf(stderr, "Invalid region name/code pair %s\n", cp1);
                return NULL;
            }
            *cp2++ = '\0';
            regn->name[nregions] = cp1;
            regn->code[nregions] = *cp2;
            nregions++;
            cp1 = strtok (NULL, ";");
        }

        regn->nregions = nregions;

	if( chunk->dlength ) {
            nbndy = (chunk->dlength-1)/4;
            memcpy(bndy, chunk->data+1, chunk->dlength-1);
	}

        for( iregion=0, ibndy=0; iregion<nregions; iregion++) {
            /* start = (start + length of previous region) or 0 if no previous region */
            /* length = (next boundary - start of region) or -1 if no next boundary */
            if( regn->code[iregion] == 'E' ){
                /* not in BASE chunk, no boundary, set length = 0 */
                regn->start[iregion] = (iregion ? (regn->start[iregion-1] + regn->length[iregion-1]) : 0);
                regn->length[iregion] = 0;
            } else {
                if( ibndy > nbndy ){
                    fprintf(stderr, "More name/code pairs than boundaries\n");
                    return NULL;
                }
                regn->start[iregion] = (iregion ? (regn->start[iregion-1] + regn->length[iregion-1]) : 0);
                regn->length[iregion] = (ibndy == nbndy ? -1 : (be_int4(bndy[ibndy])-regn->start[iregion]));
                regn->index[iregion] = index;
                ibndy++;
                index++;
            }
        }

        regn->count = 1;
            
	hd.p = regn;
	if (NULL == (hi = HashTableAdd(regn_hash, key, strlen(key), hd, NULL))) {
	    free(regn->region_names);
	    free(regn);
	    return NULL;
	}
    } else {
	regn = (regn_t *)(hi->data.p);
	regn->count++;
    }

    return hi;
}

/* ------------------------------------------------------------------------ */
#define MAX_READ_LEN 10000
void ztr2fastq(ztr_t *z, char *name, int calibrated,
               int split, char *root, int numeric, int append, int explicit,
               HashTable *regn_hash, int *nfiles_open, char **filenames,
	       FILE **files) {
    int i, nc, seq_len, nfiles = *nfiles_open;
    char buf[MAX_READ_LEN*2 + 512 + 6];
    char *seq, *qual, *sdata, *qdata, *key;
    ztr_chunk_t **chunks;
    HashItem *hi;
    regn_t *regn;
    int logodds;

    if ( split || explicit ) {
        chunks = ztr_find_chunks(z, ZTR_TYPE_REGN, &nc);
        if (nc != 1) {
            fprintf(stderr, "Zero or greater than one REGN chunks found.\n");
            if (chunks)
                free(chunks);
            return;
        }
        if( NULL == (hi = parse_regn(z, chunks[0], regn_hash)) ){
            fprintf(stderr, "Invalid RGEN chunk\n");
            if (chunks)
                free(chunks);
            return;
        }
        regn = (regn_t *)(hi->data.p);
        if( regn->count == 1 ){
            int iregion;
            for (iregion=0; iregion<regn->nregions; iregion++) {
                if( regn->code[iregion] == 'E' ) {
                    /* not in BASE chunk, the name of region IS the sequence, set file = NULL */
                    regn->file[iregion] = NULL;
                } else if( split ){
                    char filename[FILENAME_MAX];
                    int ifile;
                    if( numeric ){
                        sprintf(filename, "%s_%d.fastq", root,
				regn->index[iregion]);
                    } else {
                        sprintf(filename, "%s_%s.fastq", root,
				regn->name[iregion]);
                    }
                    for (ifile=0; ifile<nfiles; ifile++) {
                        if( 0 == strcmp(filename,filenames[ifile]) ){
                            regn->file[iregion] = files[ifile];
                            break;
                        }
                    }
                    if( ifile == nfiles ){
                        FILE *fp;
                        if (nfiles == MAX_REGIONS) {
                            fprintf(stderr, "Too many regions.\n");
                            if (chunks)
                                free(chunks);
                            return;
                        }
                        printf("Opening file %s\n", filename);
                        filenames[nfiles] = strdup(filename);
                        if (NULL == (fp = fopen(filename, "wb+"))) {
                            perror(filename);
                            if (chunks)
                                free(chunks);
                            return;
                        }
                        files[nfiles++] = fp;
                        regn->file[iregion] = fp;
                    }
                } else {
                    regn->file[iregion] = stdout;
                }
            }
        }

        if (chunks)
            free(chunks);
    }

    /* Extract the sequence only */
    chunks = ztr_find_chunks(z, ZTR_TYPE_BASE, &nc);
    if (nc != 1) {
	fprintf(stderr, "Zero or greater than one BASE chunks found.\n");
	if (chunks)
	    free(chunks);
	return;
    }
    uncompress_chunk(z, chunks[0]);
    sdata = chunks[0]->data+1;
    seq_len = chunks[0]->dlength-1;

    /* Extract the quality */
    free(chunks);
    if (calibrated)
	chunks = ztr_find_chunks(z, ZTR_TYPE_CNF1, &nc);
    else
	chunks = ztr_find_chunks(z, ZTR_TYPE_CNF4, &nc);

    if (nc != 1) {
	fprintf(stderr, "Zero or greater than one CNF chunks found.\n");
	if (chunks)
	    free(chunks);
	return;
    }
    uncompress_chunk(z, chunks[0]);
    qdata = chunks[0]->data+1;
    key = ztr_lookup_mdata_value(z, chunks[0], "SCALE");
    logodds = (key && 0 == strcmp(key, "LO")) ? 1 : 0;

    /* Construct fastq entry */
    if( split ){
        int iregion;
        for (iregion=0; iregion<regn->nregions; iregion++) {
            char *cp = name;
            int start, length;

            if( regn->code[iregion] == 'E' ) {
                /*
		 * Not in BASE chunk, the sequence IS the name of the region
		 * which may be pre-pended to the next region
		 */
                continue;
            }
            

            start = regn->start[iregion];
            length = (regn->length[iregion] == -1
		      ? (seq_len-regn->start[iregion])
		      : regn->length[iregion]);

            seq = buf;
            *seq++ = '@';
            while (*cp)
                *seq++ = *cp++;
            if( append ){
                int n = sprintf(seq,"/%d", regn->index[iregion]);
                if( n < 0 ){
                    fprintf(stderr, "Unable to add index to read name\n");
                    if (chunks)
                        free(chunks);
                    return;
                }
                seq += n;
            }
            *seq++ = '\n';
            qual = seq + length;

            if( explicit && iregion && regn->code[iregion-1] == 'E' ) {
                /*
		 * previous region not in BASE chunk, the name of region
		 * IS the sequence which is pre-pended to this region
		 */
                qual += strlen(regn->name[iregion-1]);
            }

            *qual++ = '\n';
            *qual++ = '+';
            *qual++ = '\n';
            
            if( explicit && iregion && regn->code[iregion-1] == 'E' ){
                /*
		 * previous region not in BASE chunk, the name of region
		 * IS the sequence which is pre-pended to this region
		 */
                strcpy(seq, regn->name[iregion-1]);
                seq += strlen(regn->name[iregion-1]);
                strcpy(qual, regn->name[iregion-1]);
                qual += strlen(regn->name[iregion-1]);
            }
            
            for (i = 0; i < length; i++) {
                if (*sdata != '.') {
                    *seq++ = *sdata++;
                } else {
                    *seq++ = 'N';
                    sdata++;
                }
                *qual++ = logodds ? qlookup[*qdata++ + 128] : *qdata++ + '!';
            }
            *qual++ = '\n';

            fwrite(buf, 1, qual - buf, regn->file[iregion]);
        }
    } else {
        seq = buf;
        *seq++ = '@';
        while (*name)
            *seq++ = *name++;
        *seq++ = '\n';
        qual = seq + seq_len;

        if( explicit ){
            int iregion;
            for (iregion=0; iregion<regn->nregions; iregion++)
                if( regn->code[iregion] == 'E' ) {
                    /*
		     * region not in BASE chunk, the name of region IS
		     * the sequence
		     */
                    qual += strlen(regn->name[iregion]);
                }
        }

        *qual++ = '\n';
        *qual++ = '+';
        *qual++ = '\n';

        if( explicit ){
            int iregion;
            for (iregion=0; iregion<regn->nregions; iregion++) {
                int start, length;
                if( regn->code[iregion] == 'E' ){
                    /*
		     * region not in BASE chunk, the name of region IS
		     * the sequence
		     */
                    strcpy(seq, regn->name[iregion]);
                    seq += strlen(regn->name[iregion]);
                    strcpy(qual, regn->name[iregion]);
                    qual += strlen(regn->name[iregion]);
                } else {
                    start = regn->start[iregion];
                    length = (regn->length[iregion] == -1
			      ? (seq_len-regn->start[iregion])
			      : regn->length[iregion]);
                    for (i = 0; i < length; i++) {
                        if (*sdata != '.') {
                            *seq++ = *sdata++;
                        } else {
                            *seq++ = 'N';
                            sdata++;
                        }
                        *qual++ = logodds
			    ? qlookup[*qdata++ + 128]
			    : *qdata++ + '!';
                    }
                }
            }
        } else {
            for (i = 0; i < seq_len; i++) {
                if (*sdata != '.') {
                    *seq++ = *sdata++;
                } else {
                    *seq++ = 'N';
                    sdata++;
                }
                *qual++ = logodds ? qlookup[*qdata++ + 128] : *qdata++ + '!';
            }
        }

        *qual++ = '\n';

        fwrite(buf, 1, qual - buf, stdout);
    }
    
    *nfiles_open = nfiles;

    free(chunks);
}

/* ------------------------------------------------------------------------ */
void usage(void) {
    fprintf(stderr, "Usage: srf2fastq [-c] [-C] [-s root] [-n] [-p] archive_name ...\n");
    fprintf(stderr, "                                                               \n");
    fprintf(stderr, "       -c       use calibrated quality values (CNF1)           \n");
    fprintf(stderr, "       -C       ignore bad reads                               \n");
    fprintf(stderr, "                                                               \n");
    fprintf(stderr, "       -s root  split the fastq files, one for each region     \n");
    fprintf(stderr, "                in the REGN chunk. The files are named         \n");
    fprintf(stderr, "                root_ + the name of the region                 \n");
    fprintf(stderr, "       -n       ignore REGN names, use region index.           \n");
    fprintf(stderr, "                i.e. root_1, root_2 etc.                       \n");
    fprintf(stderr, "       -a       append region index to name                    \n");
    fprintf(stderr, "                i.e. name/1, name/2 etc.                       \n");
    fprintf(stderr, "       -e       include explicit sequence, the names of the    \n");
    fprintf(stderr, "                regions of type 'E'                            \n");
    exit(0);
}

int main(int argc, char **argv) {
    int calibrated = 0;
    int mask = 0, i;
    int split = 0;
    int numeric = 0;
    int append = 0;
    int explicit = 0;
    char root[FILENAME_MAX];
    int nfiles_open = 0;
    char *filenames[MAX_REGIONS];
    FILE *files[MAX_REGIONS];
    
    /* Parse args */
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
	if (!strcmp(argv[i], "-")) {
	    break;
	} else if (!strcmp(argv[i], "-C")) {
	    mask = SRF_READ_FLAG_BAD_MASK;
	} else if (!strcmp(argv[i], "-c")) {
	    calibrated = 1;
	} else if (!strcmp(argv[i], "-s")) {
            split = 1;
            strcpy(root, argv[++i]);
	} else if (!strcmp(argv[i], "-n")) {
            numeric = 1;
	} else if (!strcmp(argv[i], "-a")) {
            append = 1;
	} else if (!strcmp(argv[i], "-e")) {
            explicit = 1;
	} else {
	    usage();
	}
    }    

    if (i == argc) {
	usage();
    }

    read_sections(READ_BASES);
    init_qlookup();

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (; i < argc; i++) {
	char *ar_name;
	srf_t *srf;
        HashTable *regn_hash;
	char name[512];
	ztr_t *ztr;

	ar_name = argv[i];

	if (NULL == (srf = srf_open(ar_name, "r"))) {
	    perror(ar_name);
	    return 4;
	}

        if (NULL == (regn_hash = HashTableCreate(0,HASH_DYNAMIC_SIZE|HASH_FUNC_JENKINS3))) {
	    return 1;
        }
    
	while (NULL != (ztr = srf_next_ztr(srf, name, mask))) {
            ztr2fastq(ztr, name, calibrated, split, root, numeric, append,
		      explicit, regn_hash, &nfiles_open, filenames, files);
	    delete_ztr(ztr);
	}

	srf_destroy(srf, 1);
    }

    return 0;
}
