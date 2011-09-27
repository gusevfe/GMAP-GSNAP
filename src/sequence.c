static char rcsid[] = "$Id: sequence.c,v 1.74 2006/12/15 12:00:58 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifndef HAVE_MEMCPY
# define memcpy(d,s,n) bcopy((s),(d),(n))
#endif
#ifndef HAVE_MEMMOVE
# define memmove(d,s,n) bcopy((s),(d),(n))
#endif

#include "sequence.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>		/* For rindex */
#include <ctype.h>		/* For iscntrl and isspace */
#include "mem.h"
#include "complement.h"
#include "intlist.h"
#include "md5.h"

/* Before setting DEBUG, may want to reduce MAXSEQLEN in sequence.h */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* Pointers for first half and second half */
#ifdef DEBUG1
#define debug1(x) x
#else
#define debug1(x)
#endif

/***********************************************************************
 *    Definitions:
 *
 *   TTTTTT ACGT ...... ACGT AAAAAA
 *          <- trimlength ->
 *   <-------- fulllength -------->
 *          ^trimstart
 *   ^contents
 *
 *   Trimming is determined by Oligoindex_set_inquery(), based on
 *   finding unique 8-mers on each end.
 ************************************************************************/


#define T Sequence_T
struct T {
  char *acc;			/* Accession */
  char *restofheader;		/* Rest of header */
  char *contents;		/* Original sequence, ends with '\0' */
  int trimstart;		/* Start of trim */
  int trimend;			/* End of trim */
  int fulllength;		/* Full length */
#ifdef PMAP
  int fulllength_given;		/* Full length minus implicit stop codon at end */
#endif
  int subseq_offset;		/* Used only for subsequences */
  int skiplength;		/* Used only for sequences longer than MAXSEQLEN */
  bool free_contents_p;
};

char *
Sequence_accession (T this) {
  return this->acc;
}

char *
Sequence_fullpointer (T this) {
  return this->contents;
}

char *
Sequence_trimpointer (T this) {
  return &(this->contents[this->trimstart]);
}

int
Sequence_ntlength (T this) {
#ifdef PMAP
  return 3*this->fulllength;
#else
  return this->fulllength;
#endif
}

int
Sequence_fulllength (T this) {
  return this->fulllength;
}

int
Sequence_fulllength_given (T this) {
#ifdef PMAP
  return this->fulllength_given;
#else
  return this->fulllength;
#endif
}

int
Sequence_trimlength (T this) {
  return this->trimend - this->trimstart;
}

void
Sequence_trim (T this, int trim_start, int trim_end) {
  this->trimstart = trim_start;
  this->trimend = trim_end;
  return;
}

int
Sequence_trim_start (T this) {
  return this->trimstart;
}

int
Sequence_trim_end (T this) {
  return this->trimend;
}

int
Sequence_subseq_offset (T this) {
  return this->subseq_offset;
}

int
Sequence_skiplength (T this) {
  return this->skiplength;
}

void
Sequence_free (T *old) {
  if (*old) {
    if ((*old)->restofheader != NULL) {
      FREE((*old)->restofheader);
    }
    if ((*old)->acc != NULL) {
      FREE((*old)->acc);
    }
    if ((*old)->free_contents_p == true) {
      FREE((*old)->contents);
    }
    FREE(*old);
  }
  return;
}

#ifdef PMAP
T
Sequence_convert_to_nucleotides (T this) {
  T new = (T) MALLOC(sizeof(*new));
  int i;

  new->acc = (char *) NULL;
  new->restofheader = (char *) NULL;
  new->fulllength = this->fulllength*3;
  new->fulllength_given = this->fulllength_given*3;
  new->contents = (char *) CALLOC(new->fulllength+1,sizeof(char));
  for (i = 0; i < new->fulllength; i++) {
    new->contents[i] = '?';
  }
  new->trimstart = 0;
  new->trimend = new->fulllength;
  new->free_contents_p = true;
  new->subseq_offset = 0;
  new->skiplength = this->skiplength;

  return new;
}
#endif

int
Sequence_count_bad (T this, int pos, int max, int direction) {
  int nbad = 0;

  if (direction > 0) {
    while (--max >= 0 && pos < this->fulllength) {
      if (this->contents[pos] == 'X') {
	nbad++;
      }
      pos++;
    }
  } else {
    while (--max >= 0 && pos >= 0) {
      if (this->contents[pos] == 'X') {
	nbad++;
      }
      pos--;
    }
  }

  return nbad;
}


#define HEADERLEN 512
#define DISCARDLEN 8192

static char Header[HEADERLEN];
static char Discard[DISCARDLEN];

static char Sequence[1+MAXSEQLEN+1]; /* Used by Sequence_read_unlimited */
static char Sequence1[HALFLEN+1]; /* 1 at end for '\0' */
static char Sequence2[HALFLEN+3]; /* 1 at end for '\0' and 2 extra in cyclic part for '\n' and '\0' */

static char *Firsthalf;
static char *Secondhalf;
static int Initc = '\0';


/* The first element of Sequence is always the null character, to mark
   the end of the string */

/* Returns '>' if FASTA file, first sequence char if not */
static int
input_init (FILE *fp) {
  int c;
  bool okayp = false;

  Header[0] = '\0';
  Sequence[0] = '\0';
  Firsthalf = &(Sequence1[0]);
  Secondhalf = &(Sequence2[0]);

  while (okayp == false && (c = fgetc(fp)) != EOF) {
    debug(printf("Read character %c\n",c));
    if (iscntrl(c)) {
    } else if (isspace(c)) {
    } else {
      okayp = true;
    }
  }

  debug(printf("Returning initial character %c\n",c));
  return c;
}

static void
blank_header (T this) {
  this->acc = (char *) CALLOC(strlen("NO_HEADER")+1,sizeof(char));
  strcpy(this->acc,"NO_HEADER");
  this->restofheader = (char *) CALLOC(1,sizeof(char));
  this->restofheader[0] = '\0';
  return;
}

static char *
input_header (FILE *fp, T this) {
  char *p;
  size_t length;

  if (feof(fp)) {
    return NULL;
  } else if (fgets(&(Header[0]),HEADERLEN,fp) == NULL) {
    /* File must terminate after > */
    return NULL;
  }

  if ((p = rindex(&(Header[0]),'\n')) != NULL) {
    *p = '\0';
  } else {
    /* Eliminate rest of header from input */
    while (fgets(&(Discard[0]),DISCARDLEN,fp) != NULL &&
	   rindex(&(Discard[0]),'\n') == NULL) ;
  }

  p = &(Header[0]);
  while (*p != '\0' && !isspace((int) *p)) {
    p++;
  }
  if (*p == '\0') {
    /* Accession only */
    length = (p - &(Header[0]))/sizeof(char);
    this->acc = (char *) CALLOC(length+1,sizeof(char));
    strcpy(this->acc,Header);
    this->restofheader = (char *) CALLOC(1,sizeof(char));
    this->restofheader[0] = '\0';
  } else {
    *p = '\0';
    length = (p - &(Header[0]))/sizeof(char);
    this->acc = (char *) CALLOC(length+1,sizeof(char));
    strcpy(this->acc,Header);
    p++;
    this->restofheader = (char *) CALLOC(strlen(p)+1,sizeof(char));
    strcpy(this->restofheader,p);
  }

  return this->acc;
} 

static void
print_contents (char *p, int length) {
  int i;
  FILE *fp = stdout;
	
  fprintf(fp,"\"");
  for (i = 0; i < length; i++) {
    if (*p == '\0') {
      fprintf(fp,"_");
    } else {
      fprintf(fp,"%c",*p);
    }
    p++;
  }
  fprintf(fp,"\"\n");
  return;
}


#define CONTROLM 13		/* From PC */
#define SPACE 32

static char *
find_bad_char (char *line) {
  char *p1, *p2;

  p1 = index(line,CONTROLM);
  p2 = index(line,SPACE);

  if (p1 == NULL && p2 == NULL) {
    return NULL;
  } else if (p1 == NULL) {
    return p2;
  } else if (p2 == NULL) {
    return p1;
  } else if (p1 < p2) {
    return p1;
  } else {
    return p2;
  }
}


static int
read_first_half (int *nextchar, bool *eolnp, FILE *fp) {
  int remainder, strlenp;
  char *ptr, *p = NULL;
  int c;

  ptr = &(Firsthalf[0]);
  if (Initc != '>') {
    *ptr++ = (char) Initc;
  }
  remainder = (&(Firsthalf[HALFLEN]) - ptr)/sizeof(char);

  while (1) {
    if (remainder <= 0) {
      debug(printf("remainder <= 0.  Returning false\n"));
      *nextchar = EOF;
      debug1(printf("read_first_half returning length1 of %d\n",(ptr - &(Firsthalf[0]))/sizeof(char)));
      return (ptr - &(Firsthalf[0]))/sizeof(char);

    } else if (feof(fp)) {
      /* EOF in middle of line */
      debug(printf("EOF.  Returning true\n"));
      *nextchar = EOF;
      debug1(printf("read_first_half returning length1 of %d\n",(ptr - &(Firsthalf[0]))/sizeof(char)));
      return (ptr - &(Firsthalf[0]))/sizeof(char);

    } else if (*eolnp == true) {
      if ((c = fgetc(fp)) == EOF || c == '>') {
	debug(printf("c == EOF or >.  Returning true\n"));
	*nextchar = c;
	return (ptr - &(Firsthalf[0]))/sizeof(char);
      } else if (iscntrl(c)) {
	debug(printf("c == control char.  Continuing\n"));
      } else if (isspace(c)) {
	*eolnp = true;
	debug(printf("c == NULL.  Continuing\n"));
      } else {
	*ptr++ = (char) c;
	remainder--;
	*eolnp = false;
	p = NULL;
	debug(printf("c == sth.  Continuing\n"));
      }

    } else {
      debug(printf("Trying to read remainder of %d\n",remainder));
      if (p != NULL) {
	strlenp = strlen(p);
	memmove(ptr,p,strlenp);
        ptr[strlenp] = '\0';
      } else {
	p = fgets(ptr,remainder+1,fp);
      }
      if (p == NULL) {
	debug(printf("line == NULL.  Returning true\n"));
	*nextchar = EOF;
	debug1(printf("read_first_half returning length1 of %d\n",(ptr - &(Firsthalf[0]))/sizeof(char)));
	return (ptr - &(Firsthalf[0]))/sizeof(char);
      } else {
	debug(printf("Read %s\n",ptr));
	if ((p = find_bad_char(ptr)) != NULL) {
	  /* Handle PC line feed ^M */
	  ptr = p++;
	  *eolnp = false;
	  debug(printf("Found control-M/space.  Advancing to %s\n",p));
	} else if ((p = index(ptr,'\n')) != NULL) {
	  ptr = p;
	  *eolnp = true;
	  debug(printf("line == EOLN.  Continuing\n"));
	} else {
	  ptr += strlen(ptr);
	  *eolnp = false;
	  p = NULL;
	  debug(printf("line != EOLN.  Continuing\n"));
	}
	remainder = (&(Firsthalf[HALFLEN]) - ptr)/sizeof(char);
      }
    }

    debug(print_contents(&(Firsthalf[0]),HALFLEN+1));
  }
}

/* returns skip length */
static int
read_second_half (int *nextchar, char **pointer2a, int *length2a, char **pointer2b, int *length2b,
		  bool eolnp, FILE *fp) {
  int skiplength, ncycles = 0, remainder, terminator, strlenp;
  char *ptr;
  char *p = NULL;
  int c;
  
  ptr = &(Secondhalf[0]);
  remainder = (&(Secondhalf[HALFLEN+2]) - ptr)/sizeof(char);

  while (1) {
    debug(printf("\nEnd: %d\n",remainder));

    if (feof(fp)) {
      debug(printf("EOF.  Returning\n"));
      *nextchar = EOF;
      break;

    } else if (remainder <= 0) {
      ptr = &(Secondhalf[0]);
      remainder = (&(Secondhalf[HALFLEN+2]) - ptr)/sizeof(char);
      ncycles++;
      debug(printf("remainder <= 0.  Cycling\n"));

    } else if (eolnp == true) {
      if ((c = fgetc(fp)) == EOF || c == '>') {
	debug(printf("c == EOF or >.  Returning\n"));
	*nextchar = c;
	break;
      } else if (iscntrl(c)) {
	debug(printf("c == control char.  Continuing\n"));
      } else if (isspace(c)) {
	debug(printf("c == NULL.  Continuing\n"));
      } else {
	*ptr++ = (char) c;
	remainder--;
	eolnp = false;
	p = NULL;
	debug(printf("c == sth.  Continuing\n"));
      }
      
    } else {
      if (p != NULL) {
        strlenp = strlen(p);
	memmove(ptr,p,strlenp);
        ptr[strlenp] = '\0';
      } else {
	p = fgets(ptr,remainder+1,fp);
      }
      if (p == NULL) {
	debug(printf("line == NULL.  Returning\n"));
	*nextchar = EOF;
	break;
      } else {
	debug(printf("Read %s\n",ptr));
	if ((p = find_bad_char(ptr)) != NULL) {
	  /* Handle PC line feed ^M */
	  ptr = p++;
	  eolnp = false;
	  debug(printf("Found control-M/space.  Advancing to %s\n",p));
	} else if ((p = index(ptr,'\n')) != NULL) {
	  ptr = p;
	  eolnp = true;
	  debug(printf("line == EOLN.  Continuing\n"));
	} else {
	  ptr += strlen(ptr);
	  eolnp = false;
	  p = NULL;
	  debug(printf("line != EOLN.  Continuing\n"));
	}
	remainder = (&(Secondhalf[HALFLEN+2]) - ptr)/sizeof(char);
      }
    }

    debug(print_contents(&(Secondhalf[0]),HALFLEN+3));
  }

  terminator = (ptr - &(Secondhalf[0]))/sizeof(char);
  debug(printf("ncycles = %d, terminator is %d\n",ncycles,terminator));
  if (ncycles == 0) {
    *length2a = 0;
    if (terminator < HALFLEN) {
      skiplength = 0;
    } else {
      skiplength = terminator-HALFLEN;
    }
  } else {
    *length2a = HALFLEN-terminator;
    skiplength = ncycles*(HALFLEN+2) + terminator-HALFLEN;
  }
  if (*length2a <= 0) {
    *length2a = 0;
    *pointer2a = (char *) NULL;
  } else {
    *pointer2a = &(Secondhalf[HALFLEN+2-(*length2a)]);
  }
  if (terminator == 0) {
    *length2b = 0;
    *pointer2b = (char *) NULL;
  } else if (terminator > HALFLEN) {
    *length2b = HALFLEN;
    *pointer2b = &(ptr[-(*length2b)]);
  } else {
    *length2b = terminator;
    *pointer2b = &(Secondhalf[0]);
  }

  return skiplength;
}

/* Returns sequence length */
static int
input_sequence (int *nextchar, char **pointer1, int *length1, char **pointer2a, int *length2a,
		char **pointer2b, int *length2b, int *skiplength, FILE *fp) {
  bool eolnp = true;

  *pointer1 = &(Firsthalf[0]);
  *pointer2a = (char *) NULL;
  *length2a = 0;
  *pointer2b = (char *) NULL;
  *length2b = 0;

  if ((*length1 = read_first_half(&(*nextchar),&eolnp,fp)) == 0) {
    *pointer1 = (char *) NULL;
    *skiplength = 0;
  } else if (*length1 < HALFLEN) {
    *skiplength = 0;
  } else {
    *skiplength = read_second_half(&(*nextchar),&(*pointer2a),&(*length2a),
				   &(*pointer2b),&(*length2b),eolnp,fp);
    debug1(printf("read_second_half returns skiplength of %d, length2a=%d, length2b=%d\n",
		  *skiplength,*length2a,*length2b));
  }

  debug1(printf("length1 = %d, length2a = %d, length2b = %d\n",
		*length1,*length2a,*length2b));

  return (*length1) + (*length2a) + (*length2b);
}

/* Used only by extern procedures (outside of this file).  Internal
   procedures have their own specialized creators. */
T
Sequence_genomic_new (char *contents, int length) {
  T new = (T) MALLOC(sizeof(*new));

  new->acc = (char *) NULL;
  new->restofheader = (char *) NULL;
  new->contents = contents;
  new->trimstart = 0;
  new->trimend = new->fulllength = length;
#ifdef PMAP
  new->fulllength_given = length;
#endif
  new->free_contents_p = false;	/* Called only by Genome_get_segment, which provides
				   its own buffer */
  new->subseq_offset = 0;
  new->skiplength = 0;
  return new;
}

static char complCode[128] = COMPLEMENT_LC;

static char *
make_complement (char *sequence, unsigned int length) {
  char *complement;
  int i, j;

  complement = (char *) CALLOC(length+1,sizeof(char));
  for (i = length-1, j = 0; i >= 0; i--, j++) {
    complement[j] = complCode[sequence[i]];
  }
  complement[length] = '\0';
  return complement;
}

static void
make_complement_buffered (char *complement, char *sequence, unsigned int length) {
  int i, j;

  /* complement = (char *) CALLOC(length+1,sizeof(char)); */
  for (i = length-1, j = 0; i >= 0; i--, j++) {
    complement[j] = complCode[sequence[i]];
  }
  complement[length] = '\0';
  return;
}

/************************************************************************
 *  Original:
 *   TTTTTT ACGT ...... ACGT AAAAAA
 *          ^trimstart     ^trimend
 *   ^contents
 ************************************************************************
 *  Subsequence:
 *       ^start                ^end
 *          ^trimstart     ^trimend
 *       ^contents
 ************************************************************************/

T
Sequence_subsequence (T this, int start, int end) {
  T new;

#ifdef PMAP
  start /= 3;
  end /= 3;
#endif

  if (start < 0) {
    start = 0;
  }
  if (end > this->fulllength) {
    end = this->fulllength;
  }

  if (end <= start) {
    return NULL;
  } else {
    new = (T) MALLOC(sizeof(*new));

    new->acc = (char *) NULL;
    new->restofheader = (char *) NULL;
    new->contents = &(this->contents[start]); 
    new->fulllength = end - start;
#ifdef PMAP
    new->fulllength_given = new->fulllength;
#endif
    if ((new->trimstart = this->trimstart - start) < 0) {
      new->trimstart = 0;
    }
    if ((new->trimend = this->trimend - start) > new->fulllength) {
      new->trimend = new->fulllength;
    }
    new->free_contents_p = false;
    new->subseq_offset = start;
    new->skiplength = this->skiplength;
    return new;
  }
}


T
Sequence_revcomp (T this) {
  T new = (T) MALLOC(sizeof(*new));

  new->acc = (char *) NULL;
  new->restofheader = (char *) NULL;
  new->contents = make_complement(this->contents,this->fulllength);
  new->fulllength = this->fulllength;
#ifdef PMAP
  new->fulllength_given = this->fulllength_given;
#endif
  new->trimstart = this->trimstart;
  new->trimend = this->trimend;
  new->free_contents_p = true;
  new->subseq_offset = 0;	/* Not sure if this is right */
  new->skiplength = this->skiplength;
  return new;
}

static char *
make_uppercase (char *sequence, unsigned int length) {
  char *uppercase;
#ifdef PMAP
  char uppercaseCode[128] = UPPERCASE_STD;
#else
  char uppercaseCode[128] = UPPERCASE_U2T;
#endif
  int i;

  uppercase = (char *) CALLOC(length+1,sizeof(char));
  for (i = 0; i < length; i++) {
    uppercase[i] = uppercaseCode[(int) sequence[i]];
  }
  uppercase[length] = '\0';
  return uppercase;
}


T
Sequence_uppercase (T this) {
  T new = (T) MALLOC(sizeof(*new));

  new->acc = (char *) NULL;
  new->restofheader = (char *) NULL;
  new->contents = make_uppercase(this->contents,this->fulllength);
  new->fulllength = this->fulllength;
#ifdef PMAP
  new->fulllength_given = this->fulllength_given;
#endif
  new->trimstart = this->trimstart;
  new->trimend = this->trimend;
  new->free_contents_p = true;
  new->subseq_offset = this->subseq_offset;
  new->skiplength = this->skiplength;
  return new;
}

T
Sequence_alias (T this) {
  T new = (T) MALLOC(sizeof(*new));

  new->acc = (char *) NULL;
  new->restofheader = (char *) NULL;
  new->contents = this->contents;
  new->fulllength = this->fulllength;
#ifdef PMAP
  new->fulllength_given = this->fulllength_given;
#endif
  new->trimstart = this->trimstart;
  new->trimend = this->trimend;
  new->free_contents_p = false;
  new->subseq_offset = this->subseq_offset;
  return new;
}


/*
void
Sequence_endstream () {
  Initc = '\0';
  return;
}
*/

T
Sequence_read (int *nextchar, FILE *input, bool maponlyp) {
  T new;
  int fulllength, skiplength;
  char *pointer1, *pointer2a, *pointer2b;
  int length1, length2a, length2b;
#ifdef PMAP
  char lastchar = '*';
#endif

  if (feof(input)) {
    *nextchar = EOF;
    return NULL;
  }

  new = (T) MALLOC(sizeof(*new));

  if (Initc == '\0') {
    if ((Initc = input_init(input)) == EOF) {
      *nextchar = EOF;
      return NULL;
    }
  }
  if (Initc != '>') {
    blank_header(new);
  } else if (input_header(input,new) == NULL) {
    /* File ends after >.  Don't process. */
      *nextchar = EOF;
     return NULL;
  } 
  if ((fulllength = input_sequence(&(*nextchar),&pointer1,&length1,&pointer2a,&length2a,
				   &pointer2b,&length2b,&skiplength,input)) == 0) {
    /* File ends during header.  Continue with a sequence of length 0. */
    /* fprintf(stderr,"File ends after header\n"); */
  }
  Initc = *nextchar;

  if (skiplength > 0) {
    if (maponlyp == false) {
      fprintf(stderr,"Warning: cDNA sequence length of %d exceeds maximum length of %d.  Truncating %d chars in middle.\n",
	      fulllength+skiplength,MAXSEQLEN,skiplength);
      fprintf(stderr,"  (For long sequences, perhaps you want maponly mode, by providing the '-1' flag.)\n");
    }
  }

#ifdef PMAP
  if (length1 > 0) {
    lastchar = pointer1[length1-1];
    if (length2a > 0) {
      lastchar = pointer2a[length2a-1];
    }
    if (length2b > 0) {
      lastchar = pointer2b[length2b-1];
    }
  }

  new->fulllength_given = fulllength;
  if (lastchar != '*') {
    debug(printf("Sequence does not end with *, so adding it\n"));
    fulllength++;
  }
#endif

  debug(printf("fulllength = %d\n",fulllength));
  new->fulllength = fulllength;
  new->skiplength = skiplength;

  new->trimstart = 0;
  new->trimend = fulllength;

  new->contents = (char *) CALLOC(fulllength+1,sizeof(char));
  if (length1 > 0) {
    strncpy(new->contents,pointer1,length1);
    if (length2a > 0) {
      strncpy(&(new->contents[length1]),pointer2a,length2a);
    }
    if (length2b > 0) {
      strncpy(&(new->contents[length1+length2a]),pointer2b,length2b);
    }
  }
#ifdef PMAP
  if (lastchar != '*') {
    new->contents[fulllength-1] = '*';
  }
#endif
  new->free_contents_p = true;
  new->subseq_offset = 0;

  debug(printf("Final query sequence is:\n"));
  debug(Sequence_print(new,/*uppercasep*/false,/*wraplength*/60,/*trimmedp*/false));
  return new;
}

T
Sequence_read_unlimited (FILE *input) {
  T new;
  Intlist_T intlist = NULL;
  char *p;
  int length, startpos = 1, maxseqlen = MAXSEQLEN;
  bool eolnp;

  if (feof(input)) {
    return NULL;
  }

  new = (T) MALLOC(sizeof(*new));

  if (Initc == '\0') {
    if ((Initc = input_init(input)) == EOF) {
      return NULL;
    }
  }
  if (Initc != '>') {
    blank_header(new);
    Sequence[startpos++] = Initc;
    maxseqlen--;
  } else if (input_header(input,new) == NULL) {
    /* File ends after >.  Don't process. */
     return NULL;
  } 
  /* Don't touch Sequence[0], because subsequent calls to
     Sequence_read depend on it being '\0'. */
  eolnp = true;
  while (fgets(&(Sequence[startpos]),maxseqlen,input) != NULL &&
	 (eolnp == false || Sequence[1] != '>')) {
    for (p = &(Sequence[1]); *p != '\n' && *p != '\0'; p++) {
      if (!iscntrl((int) *p)) {
	intlist = Intlist_push(intlist,(int) *p);
      }
    }
    if (*p == '\n') {
      eolnp = true;
    } else {
      eolnp = false;
    }
    startpos = 1;
    maxseqlen = MAXSEQLEN;
  }
  intlist = Intlist_reverse(intlist);
  new->contents = Intlist_to_char_array(&length,intlist);
  Intlist_free(&intlist);

  if (length == 0) {
    return NULL;
  } else {
    new->fulllength = new->trimend = length;
#ifdef PMAP
    new->fulllength_given = length;
#endif
    new->trimstart = 0;

    new->free_contents_p = true;
    new->subseq_offset = 0;
    new->skiplength = 0;

    /* Important to initialize for subsequent cDNA reads */
    Initc = '\0';

    return new;
  }
}

void
Sequence_print_digest (T this) {
  unsigned char *digest;

  digest = MD5_compute((unsigned char *) this->contents,this->fulllength);
  MD5_print(digest);
  FREE(digest);
  return;
}

/* Calling procedure needs to print the initial ">", if desired */
void
Sequence_print_header (T this, bool checksump) {

#if 0
#ifdef PMAP
  printf("%s (%d aa) %s",this->acc,this->fulllength_given+this->skiplength,this->restofheader);
#else
  printf("%s (%d bp) %s",this->acc,this->fulllength+this->skiplength,this->restofheader);
#endif
#else
  printf("%s %s",this->acc,this->restofheader);
#endif
  if (checksump == true) {
    printf(" md5:");
    Sequence_print_digest(this);
  }
  printf("\n");
  return;
}

void
Sequence_print (T this, bool uppercasep, int wraplength, bool trimmedp) {
  int i = 0, pos, start, end;
  char uppercaseCode[128] = UPPERCASE_STD;

  if (trimmedp == true) {
    start = this->trimstart;
    end = this->trimend;
  } else {
    start = 0;
    end = this->fulllength;
  }

  if (uppercasep == true) {
    for (pos = start; pos < end; pos++, i++) {
      printf("%c",uppercaseCode[this->contents[i]]);
      if ((i+1) % wraplength == 0) {
	printf("\n");
      }
    }
  } else {
    for (pos = start; pos < end; pos++, i++) {
      printf("%c",this->contents[i]);
      if ((i+1) % wraplength == 0) {
	printf("\n");
      }
    }
  }
  if (i % wraplength != 0) {
    printf("\n");
  }
  return;
}

void
Sequence_print_raw (T this) {
  int i = 0, pos, start, end;

  start = 0;
  end = this->fulllength;

  for (pos = start; pos < end; pos++, i++) {
    printf("%d\n",(int) this->contents[i]);
  }
  return;
}


T
Sequence_substring (T usersegment, unsigned int left, unsigned int length, 
		    bool revcomp, char *gbuffer1, char *gbuffer2, int gbufferlen) {
  if (length > gbufferlen) {
    fprintf(stderr,"Didn't allocate enough space for gbufferlen (%d < %u)\n",
	    gbufferlen,length);
    abort();
  }

  memcpy(gbuffer1,&(usersegment->contents[left]),length*sizeof(char));
  gbuffer1[length] = '\0';

  if (revcomp == true) {
    make_complement_buffered(gbuffer2,gbuffer1,length);
    debug(fprintf(stderr,"Got sequence at %u with length %u, revcomp\n",left,length));
    return Sequence_genomic_new(gbuffer2,length);
  } else {
    debug(fprintf(stderr,"Got sequence at %u with length %u, forward\n",left,length));
    return Sequence_genomic_new(gbuffer1,length);
  }
}

