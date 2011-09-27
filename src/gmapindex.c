static char rcsid[] = "$Id: gmapindex.c 30937 2010-10-26 22:14:09Z twu $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <ctype.h>
#include <string.h>
#include <strings.h>		/* For rindex */
#include "bool.h"
#include "assert.h"
#include "mem.h"
#include "fopen.h"

#include "table.h"
#include "tableint.h"
#include "genomicpos.h"
#include "compress.h"
#include "chrom.h"
#include "segmentpos.h"
#include "iit-write.h"
#include "iit-read.h"
#include "genome-write.h"
#include "indexdb.h"
#include "intlist.h"

#define BUFFERSIZE 8192

#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif


/* Program variables */
typedef enum{NONE,AUXFILES,GENOME,COMPRESS,UNCOMPRESS,OFFSETS,POSITIONS} Action_T;
static Action_T action = NONE;
static char *sourcedir = ".";
static char *destdir = ".";
static char *fileroot = NULL;
static int index1interval = 3;	/* Interval for storing 12-mers */
static bool genome_lc_p = false;
static bool rawp = false;
static bool writefilep = false;
static bool sortchrp = true;
static int wraplength = 0;
static bool mask_lowercase_p = false;


#if 0
/************************************************************************
 *   Reading strain from file
 ************************************************************************/

static char *
read_strain_from_strainfile (char *strainfile) {
  FILE *fp;
  char *refstrain = NULL, Buffer[1024], strain[1024], straintype[1024];

  if (strainfile != NULL) {
    fp = fopen(strainfile,"r");
    if (fp == NULL) {
      fprintf(stderr,"Cannot open strain file %s\n",strainfile);
    } else {
      while (fgets(Buffer,1024,fp) != NULL) {
	if (Buffer[0] == '#') {
	  /* Skip */
	} else if (sscanf(Buffer,"%s %s",strain,straintype) == 2) {
	  if (!strcmp(straintype,"reference") || !strcmp(straintype,"Reference") || 
	      !strcmp(straintype,"REFERENCE")) {
	    if (refstrain != NULL) {
	      fprintf(stderr,"More than one reference strain seen in %s\n",strainfile);
	      exit(9);
	    }
	    refstrain = (char *) CALLOC(strlen(strain)+1,sizeof(char));
	    strcpy(refstrain,strain);
	  }
	}
      }

      fclose(fp);
    }
  }

  if (refstrain != NULL) {
    return refstrain;
  } else {
    refstrain = (char *) CALLOC(strlen("reference")+1,sizeof(char));
    strcpy(refstrain,"reference");
    return refstrain;
  }
}

static char *
read_strain_from_coordsfile (char *coordsfile) {
  FILE *fp;
  char *refstrain = NULL, Buffer[1024], strain[1024], *ptr;

  if (coordsfile != NULL) {
    fp = fopen(coordsfile,"r");
    if (fp == NULL) {
      fprintf(stderr,"Cannot open coords file %s\n",coordsfile);
    } else {
      while (fgets(Buffer,1024,fp) != NULL) {
	if (Buffer[0] == '#') {
	  if ((ptr = strstr(Buffer,"Reference strain:")) != NULL) {
	    if (sscanf(ptr,"Reference strain: %s",strain) == 1) {
	      if (refstrain != NULL) {
		fprintf(stderr,"More than one reference strain seen in %s\n",coordsfile);
		exit(9);
	      }
	      refstrain = (char *) CALLOC(strlen(strain)+1,sizeof(char));
	      strcpy(refstrain,strain);
	    }
	  }
	}	    
      }

      fclose(fp);
    }
  }

  if (refstrain != NULL) {
    return refstrain;
  } else {
    refstrain = (char *) CALLOC(strlen("reference")+1,sizeof(char));
    strcpy(refstrain,"reference");
    return refstrain;
  }
}
#endif




/************************************************************************
 *   Creating aux file
 ************************************************************************/

/* accsegmentpos_table: char *accession -> Segmentpos_T segmentpos
   chrlength_table:     Chrom_T chrom -> Genomicpos_T chrlength
*/

static void
chrlength_update (Tableint_T chrlength_table, Chrom_T chrom, Genomicpos_T segend) {
  Genomicpos_T oldsegend;

  if ((oldsegend = (Genomicpos_T) Tableint_get(chrlength_table,chrom)) == (Genomicpos_T) 0) {
    /* Initial entry for this chromosome */
    Tableint_put(chrlength_table,chrom,segend);

  } else if (segend > oldsegend) {
    /* Revise */
    Tableint_put(chrlength_table,chrom,segend);
  }
  return;
}

static void
store_accession (Table_T accsegmentpos_table, Tableint_T chrlength_table,
		 char *accession, char *chr_string, Genomicpos_T chrpos1, 
		 Genomicpos_T chrpos2, bool revcompp, Genomicpos_T seglength, 
		 int contigtype) {
  Chrom_T chrom;
  Segmentpos_T segmentpos;

  chrom = Chrom_from_string(chr_string);

  segmentpos = Segmentpos_new(chrom,chrpos1,chrpos2,revcompp,seglength,contigtype);
  Table_put(accsegmentpos_table,(void *) accession,(void *) segmentpos);

  /* Update chrlength */
  if (chrpos2 > chrpos1 + seglength) {
    chrlength_update(chrlength_table,chrom,chrpos2);
  } else {
    chrlength_update(chrlength_table,chrom,chrpos1+seglength);
  }

  return;
}


/* We assume that header has already been read.  We need to check each
   new line for a new header */
static Genomicpos_T
count_sequence () {
  Genomicpos_T seglength = 0U;
  int c;
  char Buffer[BUFFERSIZE], *p;
  bool newline = true;

  while (1) {
    /* Start of new line */
    if (newline == true) {
      if ((c = getc(stdin)) == EOF || c == '>') {
	return seglength;
      } else {
	seglength += 1U;
      }
    }

    if (fgets(Buffer,BUFFERSIZE,stdin) == NULL) {
      return seglength;
    } else {
      if ((p = rindex(Buffer,'\n')) != NULL) {
	*p = '\0';
	newline = true;
      } else {
	newline = false;
      }
      seglength += (Genomicpos_T) strlen(Buffer);
    }
  }
}

static void
skip_sequence (Genomicpos_T seglength) {
  int c;
  char Buffer[BUFFERSIZE];

  while (seglength > BUFFERSIZE) {
    if (fread(Buffer,sizeof(char),BUFFERSIZE,stdin) < BUFFERSIZE) {
      fprintf(stderr,"End of file reached.  Expecting %u more characters\n",seglength);
      exit(9);
    }
    seglength -= BUFFERSIZE;
  }

  if (seglength > 0U) {
    if (fread(Buffer,sizeof(char),seglength,stdin) < seglength) {
      fprintf(stderr,"End of file reached.  Expecting %u more characters\n",seglength);
      exit(9);
    }
  }

  if ((c = getchar()) != EOF && c != '\n') {
    fprintf(stderr,"Expecting linefeed at end of sequence.  Saw %d (%c) instead\n",c,c);
    exit(9);
  }

  if ((c = getchar()) != EOF && c != '>') {
    fprintf(stderr,"Expecting new FASTA line.  Saw %d (%c) instead\n",c,c);
    exit(9);
  }

  return;
}

static bool
process_sequence_aux (Table_T accsegmentpos_table, Tableint_T chrlength_table, char *fileroot,
		      int ncontigs) {
  char Buffer[BUFFERSIZE], accession_p[BUFFERSIZE], *accession, 
    chrpos_string[BUFFERSIZE], *chr_string, *coords;
  Genomicpos_T chrpos1, chrpos2, lower, upper, seglength;
  bool revcompp;

  /* Store sequence info */
  if (fgets(Buffer,BUFFERSIZE,stdin) == NULL) {
    return false;
  }

  if (sscanf(Buffer,"%s %s",accession_p,chrpos_string) < 2) {
    fprintf(stderr,"Can't parse line %s\n",Buffer);
    exit(1);
  } else {
    if (ncontigs < 100) {
      fprintf(stderr,"Logging contig %s at %s in genome %s\n",accession_p,chrpos_string,fileroot);
    } else if (ncontigs == 100) {
      fprintf(stderr,"More than 100 contigs.  Will stop printing messages\n");
    }

    if (!index(chrpos_string,':')) {
      fprintf(stderr,"Can't parse chromosomal coordinates %s\n",chrpos_string);
      exit(1);
    } else {
      chr_string = strtok(chrpos_string,":");
      coords = strtok(NULL,":");
      if (sscanf(coords,"%u..%u",&chrpos1,&chrpos2) == 2) {
	/* 1:3..5, one-based, inclusive => (2,5), zero-based, boundaries */
	if (chrpos1 <= chrpos2) {
	  chrpos1--;
	  revcompp = false;
	  lower = chrpos1;
	  upper = chrpos2;
	} else {
	  chrpos2--;
	  revcompp = true;
	  lower = chrpos2;
	  upper = chrpos1;
	}
      } else if (sscanf(coords,"%u",&chrpos1) == 1) {
	/* 1:3, one-based, inclusive => (3,3), zero-based, boundaries */
	revcompp = false;
	lower = upper = chrpos1;
      } else {
	fprintf(stderr,"Can't parse chromosomal coordinates %s\n",coords);
	exit(1);
      }
    }

#if 0
    /* No longer supporting strains/types */
    p = Buffer;
    while (*p != '\0' && !isspace((int) *p)) { p++; } /* Skip to first space */
    while (*p != '\0' && isspace((int) *p)) { p++; } /* Skip past first space */
    while (*p != '\0' && !isspace((int) *p)) { p++; } /* Skip to second space */
    while (*p != '\0' && isspace((int) *p)) { p++; } /* Skip past second space */

    if (*p == '\0') {
      contigtype = 0;		/* Empty type string */
    } else {
      if ((ptr = rindex(p,'\n')) != NULL) {
	while (isspace((int) *ptr)) { ptr--; } /* Erase empty space */
	ptr++;
	*ptr = '\0';
      }
      if ((contigtype = Tableint_get(contigtype_table,(void *) p)) == 0) {
	debug(printf("Storing type %s.\n",p));
	/* Store types as 1-based */
	contigtype = Tableint_length(contigtype_table) + 1;
	typestring = (char *) CALLOC(strlen(p)+1,sizeof(char));
	strcpy(typestring,p);
	Tableint_put(contigtype_table,(void *) typestring,contigtype);
	*contigtypelist = List_push(*contigtypelist,typestring);
      }
    }
#endif

    /* The '>' character was already stripped off by the last call to count_sequence() */
    accession = (char *) CALLOC(strlen(accession_p)+1,sizeof(char));
    strcpy(accession,accession_p);
  }

  if (rawp == true) {
    seglength = upper - lower;
    fprintf(stderr,"Skipping %u characters\n",seglength);
    skip_sequence(seglength);
  } else {
    seglength = count_sequence();
    if (seglength != upper - lower) {
      fprintf(stderr,"%s has expected sequence length %u-%u=%u but actual length %u\n",
	      accession,upper,lower,upper-lower,seglength);
    }
  }
  store_accession(accsegmentpos_table,chrlength_table,
		  accession,chr_string,lower,upper,revcompp,
		  seglength,/*contigtype*/0);

  return true;
}


/************************************************************************
 *   Creating genome and related files
 ************************************************************************/

/* Modifies chrlength_table to store offsets, rather than chrlengths */
static void
write_chromosome_file (char *genomesubdir, char *fileroot, Tableint_T chrlength_table, bool sortchrp) {
  FILE *textfp, *chrsubsetfp;
  char *divstring, *textfile, *chrsubsetfile, *iitfile, *chr_string, emptystring[1];
  int n, i;
  Chrom_T *chroms;
  Genomicpos_T chroffset = 0, chrlength;
  List_T divlist = NULL;
  List_T intervallist = NULL, chrtypelist = NULL, labellist = NULL, annotlist = NULL, p;
  Table_T intervaltable, labeltable, annottable;
  Interval_T interval;

  emptystring[0] = '\0';

  if (sortchrp == true) {
    /* Get chromosomes in order */
    chroms = (Chrom_T *) Tableint_keys(chrlength_table,0U);
    n = Tableint_length(chrlength_table);
    qsort(chroms,n,sizeof(Chrom_T),Chrom_compare);
  } else {
    chroms = (Chrom_T *) Tableint_keys_by_timeindex(chrlength_table,0U);
    n = Tableint_length(chrlength_table);
  }

  /* Write chromosome text file and chrsubset file */
  textfile = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			     strlen(fileroot)+strlen(".chromosome")+1,sizeof(char));
  sprintf(textfile,"%s/%s.chromosome",genomesubdir,fileroot);
  /* Use binary, not text, so files are Unix-compatible */
  if ((textfp = FOPEN_WRITE_BINARY(textfile)) == NULL) {
    fprintf(stderr,"Can't write to file %s\n",textfile);
    exit(9);
  }
  FREE(textfile);

  chrsubsetfile = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
				  strlen(fileroot)+strlen(".chrsubset")+1,sizeof(char));
  sprintf(chrsubsetfile,"%s/%s.chrsubset",genomesubdir,fileroot);
  /* Use binary, not text, so files are Unix-compatible */
  if ((chrsubsetfp = FOPEN_WRITE_BINARY(chrsubsetfile)) == NULL) {
    fprintf(stderr,"Can't write to file %s\n",chrsubsetfile);
    exit(9);
  }
  FREE(chrsubsetfile);
  fprintf(chrsubsetfp,">all\n");
  fprintf(chrsubsetfp,"\n");

  chrtypelist = List_push(chrtypelist,"");
  for (i = 0; i < n; i++) {
    chrlength = (Genomicpos_T) Tableint_get(chrlength_table,chroms[i]);
    assert(chroffset <= chroffset+chrlength-1);
    chr_string = Chrom_string(chroms[i]);
    if (i < 100) {
      fprintf(stderr,"Chromosome %s has universal coordinates %u..%u\n",
	      chr_string,chroffset+1,chroffset+1+chrlength-1);
    } else if (i == 100) {
      fprintf(stderr,"More than 100 contigs.  Will stop printing messages\n");
    }
      
    if (n <= 100) {
      fprintf(chrsubsetfp,">%s\n",chr_string);
      fprintf(chrsubsetfp,"+%s\n",chr_string);
    }

    fprintf(textfp,"%s\t%u..%u\t%u\n",
	    chr_string,chroffset+1,chroffset+chrlength,chrlength);
    intervallist = List_push(intervallist,(void *) Interval_new(chroffset,chroffset+chrlength-1U,0));
    labellist = List_push(labellist,(void *) chr_string);
    annotlist = List_push(annotlist,(void *) emptystring); /* No annotations */
    Tableint_put(chrlength_table,chroms[i],chroffset);
    chroffset += chrlength;
  }
  FREE(chroms);
  intervallist = List_reverse(intervallist);
  labellist = List_reverse(labellist);

  fclose(chrsubsetfp);
  fclose(textfp);

  /* Write chromosome IIT file */
  divstring = (char *) CALLOC(1,sizeof(char));
  divstring[0] = '\0';
  divlist = List_push(NULL,divstring);

  intervaltable = Table_new(65522,Table_string_compare,Table_string_hash);
  labeltable = Table_new(65522,Table_string_compare,Table_string_hash);
  annottable = Table_new(65522,Table_string_compare,Table_string_hash);

  Table_put(intervaltable,(void *) divstring,intervallist);
  Table_put(labeltable,(void *) divstring,labellist);
  Table_put(annottable,(void *) divstring,annotlist);

  iitfile = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			    strlen(fileroot)+strlen(".chromosome.iit")+1,sizeof(char));
  sprintf(iitfile,"%s/%s.chromosome.iit",genomesubdir,fileroot);
  IIT_write(iitfile,divlist,chrtypelist,/*fieldlist*/(List_T) NULL,
	    intervaltable,labeltable,annottable,/*divsort*/NO_SORT,
	    /*version, use 1 for backward compatibility*/1,
	    /*label_pointers_8p*/false,/*annot_pointers_8p*/false);
  FREE(iitfile);

  List_free(&divlist);
  FREE(divstring);

  Table_free(&annottable);
  Table_free(&labeltable);
  Table_free(&intervaltable);

  List_free(&annotlist);

  /* Do not free strings in labellist, since they are not allocated */
  List_free(&labellist);

  /* chrtypelist has no dynamically allocated strings */
  List_free(&chrtypelist);

  for (p = intervallist; p != NULL; p = List_next(p)) {
    interval = (Interval_T) List_head(p);
    Interval_free(&interval);
  }
  List_free(&intervallist);

  return;
}

static Table_T current_accsegmentpos_table;

/* x is really a char ** */
static int
bysegmentpos_compare (const void *x, const void *y) {
  char *acc1 = * (char **) x;
  char *acc2 = * (char **) y;
  Segmentpos_T a = (Segmentpos_T) Table_get(current_accsegmentpos_table,(void *) acc1);
  Segmentpos_T b = (Segmentpos_T) Table_get(current_accsegmentpos_table,(void *) acc2);

  return Segmentpos_compare(&a,&b);
}

static void
write_contig_file (char *genomesubdir, char *fileroot, Table_T accsegmentpos_table, 
		   Tableint_T chrlength_table, List_T contigtypelist, bool sortchrp) {
  FILE *textfp;
  char *textfile, *iitfile, *annot;
  int naccessions, i;
  char **accessions, *divstring;
  Segmentpos_T segmentpos;
  Chrom_T chrom;
  Genomicpos_T chroffset, universalpos1, universalpos2;
  List_T divlist = NULL, intervallist = NULL, labellist = NULL, annotlist = NULL, p;
  Table_T intervaltable, labeltable, annottable;
  Interval_T interval;
#if 0
  void **keys;
  int *values, ntypes;
#endif
  
  if (sortchrp == true) {
    /* Get accessions in order */
    accessions = (char **) Table_keys(accsegmentpos_table,NULL);
    naccessions = Table_length(accsegmentpos_table);
    current_accsegmentpos_table = accsegmentpos_table;
    qsort(accessions,naccessions,sizeof(char *),bysegmentpos_compare);
  } else {
    accessions = (char **) Table_keys_by_timeindex(accsegmentpos_table,NULL);
    naccessions = Table_length(accsegmentpos_table);
  }

#if 0
  /* Get types in order */
  keys = Tableint_keys(contigtype_table,NULL);
  values = Tableint_values(contigtype_table,0);
  ntypes = Tableint_length(contigtype_table);
  contigtypes = (char **) CALLOC(ntypes+1,sizeof(char *)); /* Add 1 for type 0 */
  contigtypes[0] = "";
  for (j = 0; j < ntypes; j++) {
    contigtypes[values[j]] = keys[j];
  }
  FREE(values);
  FREE(keys);
#endif

  /* Write contig text file */
  textfile = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			     strlen(fileroot)+strlen(".contig")+1,sizeof(char));
  sprintf(textfile,"%s/%s.contig",genomesubdir,fileroot);
  /* Use binary, not text, so files are Unix-compatible */
  if ((textfp = FOPEN_WRITE_BINARY(textfile)) == NULL) {
    fprintf(stderr,"Can't write to file %s\n",textfile);
    exit(9);
  }
  FREE(textfile);

  for (i = 0; i < naccessions; i++) {
    segmentpos = (Segmentpos_T) Table_get(accsegmentpos_table,(void *) accessions[i]);
    chrom = Segmentpos_chrom(segmentpos);
    chroffset = (Genomicpos_T) Tableint_get(chrlength_table,chrom);
    universalpos1 = chroffset + Segmentpos_chrpos1(segmentpos);
    universalpos2 = chroffset + Segmentpos_chrpos2(segmentpos);

    /* Print as 1-based, inclusive [a,b] */
    if (Segmentpos_revcompp(segmentpos) == true) {
      fprintf(textfp,"%s\t%u..%u\t%s:%u..%u\t%u",
	      accessions[i],universalpos2+1U,universalpos1,
	      Chrom_string(chrom),Segmentpos_chrpos2(segmentpos)+1U,Segmentpos_chrpos1(segmentpos),
	      Segmentpos_length(segmentpos));
    } else {
      fprintf(textfp,"%s\t%u..%u\t%s:%u..%u\t%u",
	      accessions[i],universalpos1+1U,universalpos2,
	      Chrom_string(chrom),Segmentpos_chrpos1(segmentpos)+1U,Segmentpos_chrpos2(segmentpos),
	      Segmentpos_length(segmentpos));
    }
#if 0
    if (Segmentpos_type(segmentpos) > 0) {
      fprintf(textfp,"\t%s",contigtypes[Segmentpos_type(segmentpos)]);
    }
#endif

    fprintf(textfp,"\n");

    /* Store as 0-based, inclusive [a,b] */
    labellist = List_push(labellist,(void *) accessions[i]);
    if (Segmentpos_revcompp(segmentpos) == true) {
      /* The negative sign in the interval is the indication that the
	 contig was reverse complement */
      intervallist = List_push(intervallist, 
			       (void *) Interval_new(universalpos2-1U,universalpos1,
						     Segmentpos_type(segmentpos)));
    } else {
      intervallist = List_push(intervallist, 
			       (void *) Interval_new(universalpos1,universalpos2-1U,
						     Segmentpos_type(segmentpos)));
    }

#if 0
    /* IIT version 1 */
    sprintf(seglength,"%u",Segmentpos_length(segmentpos));
    annot = (char *) CALLOC(strlen(seglength)+1,sizeof(char));
    strcpy(annot,seglength);
#else
    /* IIT versions >= 2 */
    annot = (char *) CALLOC(1,sizeof(char));
    annot[0] = '\0';
#endif
    annotlist = List_push(annotlist,(void *) annot);

  }

#if 0
  FREE(contigtypes);
#endif
  FREE(accessions);
  intervallist = List_reverse(intervallist);
  /* contigtypelist = List_reverse(contigtypelist); -- Done by caller */ 
  labellist = List_reverse(labellist);
  annotlist = List_reverse(annotlist);

  /* Write contig IIT file */
  divstring = (char *) CALLOC(1,sizeof(char));
  divstring[0] = '\0';
  divlist = List_push(NULL,divstring);

  intervaltable = Table_new(65522,Table_string_compare,Table_string_hash);
  labeltable = Table_new(65522,Table_string_compare,Table_string_hash);
  annottable = Table_new(65522,Table_string_compare,Table_string_hash);

  Table_put(intervaltable,(void *) divstring,intervallist);
  Table_put(labeltable,(void *) divstring,labellist);
  Table_put(annottable,(void *) divstring,annotlist);

  iitfile = (char *) CALLOC(strlen(genomesubdir)+strlen("/")+
			    strlen(fileroot)+strlen(".contig.iit")+1,sizeof(char));
  sprintf(iitfile,"%s/%s.contig.iit",genomesubdir,fileroot);

#if 0
  debug(
	for (p = contigtypelist; p != NULL; p = List_next(p)) {
	  printf("Type %s\n",(char *) List_head(p));
	}
	);
#endif
  IIT_write(iitfile,divlist,contigtypelist,/*fieldlist*/(List_T) NULL,
	    intervaltable,labeltable,annottable,/*divsort*/NO_SORT,
	    /*version, use 1 for backward compatibility*/1,
	    /*label_pointers_8p*/false,/*annot_pointers_8p*/false);
  FREE(iitfile);

  List_free(&divlist);
  FREE(divstring);

  Table_free(&annottable);
  Table_free(&labeltable);
  Table_free(&intervaltable);

  for (p = annotlist; p != NULL; p = List_next(p)) {
    annot = (char *) List_head(p);
    FREE(annot);
  }
  List_free(&annotlist);

  /* Labels (accessions) are freed by accsegmentpos_table_gc */
  List_free(&labellist);

  for (p = intervallist; p != NULL; p = List_next(p)) {
    interval = (Interval_T) List_head(p);
    Interval_free(&interval);
  }
  List_free(&intervallist);

  return;
}

/************************************************************************/


#if 0
static void
stringlist_gc (List_T *list) {
  List_T p;
  char *string;

  for (p = *list; p != NULL; p = List_next(p)) {
    string = (char *) List_head(p);
    FREE(string);
  }
  List_free(&(*list));
  return;
}
#endif


static void
chrlength_table_gc (Tableint_T *chrlength_table) {
  /* Don't free chrom entries in table, because they are freed by Segmentpos_free */
  Tableint_free(&(*chrlength_table));
  return;
}

static void
accsegmentpos_table_gc (Table_T *accsegmentpos_table) {
#if 0
  int n, i = 0;
  char *accession;
  Segmentpos_T segmentpos;
  void **keys, **values;
#endif

#if 0
  /* For some reason, this fails on some computers */
  n = Table_length(*accsegmentpos_table);
  keys = Table_keys(*accsegmentpos_table,NULL);
  values = Table_values(*accsegmentpos_table,NULL);
  for (i = 0; i < n; i++) {
    accession = (char *) keys[i];
    FREE(accession);
  }
  for (i = 0; i < n; i++) {
    segmentpos = (Segmentpos_T) values[i];
    Segmentpos_free(&segmentpos);
  }
  FREE(values);
  FREE(keys);
#endif

  Table_free(&(*accsegmentpos_table));
  return;
}

#if 0
static char *
remove_slashes (char *buffer) {
  char *copy, *p;

  copy = (char *) CALLOC(strlen(buffer)+1,sizeof(char));
  strcpy(copy,buffer);
  p = copy;
  while (*p != '\0') {
    if (*p == '/') {
      *p = '_';
    }
    p++;
  }
  return copy;
}
#endif


#ifdef __STRICT_ANSI__
int getopt (int argc, char *const argv[], const char *optstring);
#endif

int 
main (int argc, char *argv[]) {
  int ncontigs;
  Table_T accsegmentpos_table;
  Tableint_T chrlength_table;
  List_T contigtypelist = NULL;
  IIT_T chromosome_iit, contig_iit;
  char *typestring;
  unsigned int genomelength;
  char *chromosomefile, *iitfile, *offsetsfile, *positionsfile, interval_char;
  FILE *offsets_fp, *fp;

  int c;
  extern int optind;
  extern char *optarg;

  while ((c = getopt(argc,argv,"F:D:d:ArlGCUOPWw:Sq:m")) != -1) {
    switch (c) {
    case 'F': sourcedir = optarg; break;
    case 'D': destdir = optarg; break;
    case 'd': fileroot = optarg; break;
    case 'A': action = AUXFILES; break;
    case 'r': rawp = true; break;
    case 'l': genome_lc_p = true; break;
    case 'G': action = GENOME; break;
    case 'C': action = COMPRESS; break;
    case 'U': action = UNCOMPRESS; break;
    case 'O': action = OFFSETS; break;
    case 'P': action = POSITIONS; break;
    case 'W': writefilep = true; break;
    case 'w': wraplength = atoi(optarg); break;
    case 'S': sortchrp = false; break;
    case 'q': index1interval = atoi(optarg); break;
    case 'm': mask_lowercase_p = true; break;
    }
  }
  argc -= (optind - 1);
  argv += (optind - 1);

  if (index1interval == 6) {
    interval_char = '6';
  } else if (index1interval == 3) {
    interval_char = '3';
  } else if (index1interval == 1) {
    interval_char = '1';
  } else {
    fprintf(stderr,"Selected indexing interval %d is not allowed.  Only values allowed are 6, 3, or 1\n",index1interval);
    exit(9);
  }

  if (action != COMPRESS && action != UNCOMPRESS) {
    if (fileroot == NULL) {
      fprintf(stderr,"Missing name of genome database.  Must specify with -d flag.\n");
      exit(9);
    }
  }

  if (action == AUXFILES) {
    /* Usage: cat <fastafile> | gmapindex [-F <sourcedir>] [-D <destdir>] -d <dbname> -A
       Requires <fastafile> in appropriate format
       Writes <destdir>/<dbname>.chromosome and <destdir>/<dbname>.contig files 
       and corresponding .iit files */

    if (getc(stdin) != '>') {
      fprintf(stderr,"Expected file to start with '>'\n");
      exit(9);
    }

    /* Holds contigs.  keys are strings; values are structs. */
    accsegmentpos_table = Table_new(65522,Table_string_compare,Table_string_hash);
    /* Hold chromosomes.  keys are Chrom_Ts; values are uints. */
    chrlength_table = Tableint_new(65522,Chrom_compare_table,Chrom_hash_table);

#if 0
    /* No longer supporting strains */
    /* keys are strings; values are ints */
    contigtype_table = Tableint_new(100,Table_string_compare,Table_string_hash);

    refstrain = read_strain_from_coordsfile(coordsfile);
    fprintf(stderr,"Reference strain is %s\n",refstrain);
    contigtypelist = List_push(NULL,refstrain);
#endif
    /* The zeroth type is empty */
    typestring = (char *) CALLOC(1,sizeof(char));
    typestring[0] = '\0';
    contigtypelist = List_push(NULL,typestring);

    ncontigs = 0;
    while (process_sequence_aux(accsegmentpos_table,chrlength_table,fileroot,ncontigs) == true) {
      ncontigs++;
    }
    if (ncontigs == 0) {
      fprintf(stderr,"No contig information was provided to gmapindex\n");
      exit(9);
    }

    write_chromosome_file(destdir,fileroot,chrlength_table,sortchrp);
    write_contig_file(destdir,fileroot,accsegmentpos_table,chrlength_table,contigtypelist,
		      sortchrp);

    chrlength_table_gc(&chrlength_table);
    accsegmentpos_table_gc(&accsegmentpos_table);

  } else if (action == GENOME) {
    /* Usage: cat <fastafile> | gmapindex [-F <sourcedir>] [-D <destdir>] -d <dbname> -G
       Requires <fastafile> in appropriate format and <sourcedir>/<dbname>.chromosome.iit 
       and <sourcedir>/<dbname>.contig.iit files.
       Creates <destdir>/<dbname>.genome */

    chromosomefile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+
				     strlen(fileroot)+strlen(".chromosome.iit")+1,sizeof(char));
    sprintf(chromosomefile,"%s/%s.chromosome.iit",sourcedir,fileroot);
    if ((chromosome_iit = IIT_read(chromosomefile,/*name*/NULL,/*readonlyp*/true,
				   /*divread*/READ_ALL,/*divstring*/NULL,/*add_iit_p*/false,
				   /*labels_read_p*/true)) == NULL) {
      fprintf(stderr,"IIT file %s is not valid\n",chromosomefile);
      exit(9);
    }
    genomelength = IIT_totallength(chromosome_iit);
    FREE(chromosomefile);

    iitfile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+
			      strlen(fileroot)+strlen(".contig.iit")+1,sizeof(char));
    sprintf(iitfile,"%s/%s.contig.iit",sourcedir,fileroot);
    if ((contig_iit = IIT_read(iitfile,/*name*/NULL,/*readonlyp*/true,/*divread*/READ_ALL,
			       /*divstring*/NULL,/*add_iit_p*/false,/*labels_read_p*/true)) == NULL) {
      fprintf(stderr,"IIT file %s is not valid\n",iitfile);
      exit(9);
    }
    FREE(iitfile);
    
    if (IIT_ntypes(contig_iit) == 1) {
      Genome_write(destdir,fileroot,stdin,contig_iit,NULL,genome_lc_p,rawp,writefilep,genomelength);
    } else if (IIT_ntypes(contig_iit) > 1) {
      fprintf(stderr,"GMAPINDEX no longer supports alternate strains\n");
      abort();
    }

    IIT_free(&contig_iit);

  } else if (action == COMPRESS) {
    /* Usage: cat <genomefile> | gmapindex -C > <genomecompfile>, or
              gmapindex -C <genomefile> > <genomecompfile> */

    if (argc > 1) {
      fp = FOPEN_READ_BINARY(argv[1]);
      Compress_compress(fp);
      fclose(fp);
    } else {
      Compress_compress(stdin);
    }

  } else if (action == UNCOMPRESS) {
    /* Usage: cat <genomecompfile> | gmapindex -U [-w <wraplength>] > <genomefile>, or
              gmapindex -U [-w <wraplength>] <genomecompfile> > <genomefile> */
    
    if (argc > 1) {
      fp = FOPEN_READ_BINARY(argv[1]);
      Compress_uncompress(fp,wraplength);
      fclose(fp);
    } else {
      Compress_uncompress(stdin,wraplength);
    }

  } else if (action == OFFSETS) {
    /* Usage: cat <genomefile> | gmapindex [-F <sourcedir>] [-D <destdir>] -d <dbname> -O
       Creates <destdir>/<dbname>.idxoffsets */

    chromosomefile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+
				     strlen(fileroot)+strlen(".chromosome.iit")+1,sizeof(char));
    sprintf(chromosomefile,"%s/%s.chromosome.iit",sourcedir,fileroot);
    if ((chromosome_iit = IIT_read(chromosomefile,/*name*/NULL,/*readonlyp*/true,
				   /*divread*/READ_ALL,/*divstring*/NULL,/*add_iit_p*/false,
				   /*labels_read_p*/false)) == NULL) {
      fprintf(stderr,"IIT file %s is not valid\n",chromosomefile);
      exit(9);
    }
    FREE(chromosomefile);

    /* Reference strain */
    if (mask_lowercase_p == true) {
      offsetsfile = (char *) CALLOC(strlen(destdir)+strlen("/")+strlen(fileroot)+
				    strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval_char*/1+
				    strlen(OFFSETS_FILESUFFIX)+strlen(".masked")+1,sizeof(char));
      sprintf(offsetsfile,"%s/%s.%s%c%s.masked",destdir,fileroot,IDX_FILESUFFIX,interval_char,OFFSETS_FILESUFFIX);
    } else {
      offsetsfile = (char *) CALLOC(strlen(destdir)+strlen("/")+strlen(fileroot)+
				    strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval_char*/1+
				    strlen(OFFSETS_FILESUFFIX)+1,sizeof(char));
      sprintf(offsetsfile,"%s/%s.%s%c%s",destdir,fileroot,IDX_FILESUFFIX,interval_char,OFFSETS_FILESUFFIX);
    }
    if ((offsets_fp = FOPEN_WRITE_BINARY(offsetsfile)) == NULL) {
      fprintf(stderr,"Can't write to file %s\n",offsetsfile);
      exit(9);
    }

    Indexdb_write_offsets(offsets_fp,stdin,chromosome_iit,index1interval,
			  genome_lc_p,fileroot,mask_lowercase_p);

    fclose(offsets_fp);
    FREE(offsetsfile);
    IIT_free(&chromosome_iit);

  } else if (action == POSITIONS) {
    /* Usage: cat <genomefile> | gmapindex [-F <sourcedir>] [-D <destdir>] -d <dbname> -P
       Requires <sourcedir>/<dbname>.idxoffsets.
       Creates <destdir>/<dbname>.idxpositions */

    chromosomefile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+
				     strlen(fileroot)+strlen(".chromosome.iit")+1,sizeof(char));
    sprintf(chromosomefile,"%s/%s.chromosome.iit",sourcedir,fileroot);
    if ((chromosome_iit = IIT_read(chromosomefile,/*name*/NULL,/*readonlyp*/true,
				   /*divread*/READ_ALL,/*divstring*/NULL,/*add_iit_p*/false,
				   /*labels_read_p*/false)) == NULL) {
      fprintf(stderr,"IIT file %s is not valid\n",chromosomefile);
      exit(9);
    }
    FREE(chromosomefile);

    /* Reference strain */
    if (mask_lowercase_p == true) {
      offsetsfile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+strlen(fileroot)+
				    strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval char*/1+
				    strlen(OFFSETS_FILESUFFIX)+strlen(".masked")+1,sizeof(char));
      sprintf(offsetsfile,"%s/%s.%s%c%s.masked",sourcedir,fileroot,IDX_FILESUFFIX,interval_char,OFFSETS_FILESUFFIX);
    } else {
      offsetsfile = (char *) CALLOC(strlen(sourcedir)+strlen("/")+strlen(fileroot)+
				    strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval char*/1+
				    strlen(OFFSETS_FILESUFFIX)+1,sizeof(char));
      sprintf(offsetsfile,"%s/%s.%s%c%s",sourcedir,fileroot,IDX_FILESUFFIX,interval_char,OFFSETS_FILESUFFIX);
    }
    if ((offsets_fp = FOPEN_READ_BINARY(offsetsfile)) == NULL) {
      fprintf(stderr,"Can't open file %s\n",offsetsfile);
      exit(9);
    }

    if (mask_lowercase_p == true) {
      positionsfile = (char *) CALLOC(strlen(destdir)+strlen("/")+strlen(fileroot)+
				      strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval char*/1+
				      strlen(POSITIONS_FILESUFFIX)+strlen(".masked")+1,sizeof(char));
      sprintf(positionsfile,"%s/%s.%s%c%s.masked",destdir,fileroot,IDX_FILESUFFIX,interval_char,POSITIONS_FILESUFFIX);
    } else {
      positionsfile = (char *) CALLOC(strlen(destdir)+strlen("/")+strlen(fileroot)+
				      strlen(".")+strlen(IDX_FILESUFFIX)+/*for interval char*/1+
				      strlen(POSITIONS_FILESUFFIX)+1,sizeof(char));
      sprintf(positionsfile,"%s/%s.%s%c%s",destdir,fileroot,IDX_FILESUFFIX,interval_char,POSITIONS_FILESUFFIX);
    }
    
    Indexdb_write_positions(positionsfile,offsets_fp,stdin,chromosome_iit,index1interval,
			    genome_lc_p,writefilep,fileroot,mask_lowercase_p);
    fclose(offsets_fp);
    FREE(positionsfile);
    FREE(offsetsfile);
    IIT_free(&chromosome_iit);
  }

  return 0;
}

