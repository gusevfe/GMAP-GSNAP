static char rcsid[] = "$Id: chimera.c 34018 2011-01-21 20:23:10Z twu $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "chimera.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>		/* For sqrt */
#include "mem.h"
#include "maxent.h"


#define GBUFFERLEN 1024

/* Chimera assembly/bestpath matrix */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* Chimera detection */
#ifdef DEBUG1
#define debug1(x) x
#else
#define debug1(x)
#endif

/* Exon-exon boundary detection */
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif


#define T Chimera_T
struct T {
  int chimerapos;
  int equivpos;

  int cdna_direction;
  int exonexonpos;
  double donor_prob;
  double acceptor_prob;
};


int 
Chimera_pos (T this) {
  return this->chimerapos;
}

int
Chimera_equivpos (T this) {
  return this->equivpos;
}

int
Chimera_cdna_direction (T this) {
  return this->cdna_direction;
}

double
Chimera_donor_prob (T this) {
  if (this->exonexonpos < 0) {
    return 0.0;
  } else {
    return this->donor_prob;
  }
}

double
Chimera_acceptor_prob (T this) {
  if (this->exonexonpos < 0) {
    return 0.0;
  } else {
    return this->acceptor_prob;
  }
}


T
Chimera_new (int chimerapos, int chimeraequivpos, int exonexonpos, int cdna_direction,
	     double donor_prob, double acceptor_prob) {
  T new = (T) MALLOC(sizeof(*new));

  new->chimerapos = chimerapos;
  new->equivpos = chimeraequivpos;
  new->exonexonpos = exonexonpos;
  new->cdna_direction = cdna_direction;
  new->donor_prob = donor_prob;
  new->acceptor_prob = acceptor_prob;

  return new;
}

void
Chimera_free (T *old) {
  FREE(*old);
  return;
}


void
Chimera_print (FILE *fp, T this) {
  if (this->exonexonpos >= 0) {
    fprintf(fp," *** Possible chimera with exon-exon boundary");
    if (this->cdna_direction > 0) {
      fprintf(fp," (sense)");
    } else if (this->cdna_direction < 0) {
      fprintf(fp," (antisense)");
    }
    fprintf(fp," at %d (donor_prob = %.3f, acceptor_prob = %.3f)",
	   this->exonexonpos+1,this->donor_prob,this->acceptor_prob);
  } else if (this->equivpos == this->chimerapos) {
    fprintf(fp," *** Possible chimera with breakpoint at %d",this->chimerapos+1);
  } else {
    fprintf(fp," *** Possible chimera with breakpoint at %d..%d",this->chimerapos+1,this->equivpos+1);
  }

  return;
}


#define NPSEUDO 10.0

int
Chimera_alignment_break (int *newstart, int *newend, Stage3_T stage3, int queryntlength, double fthreshold) {
  int breakpoint;
  int *matchscores;

  /* x signifies nmatches, y signifies nmismatches, x + y = n */
  int start, end, pos, x = 0, y, n, x_left, y_left, x_right, y_right, n_left, n_right;
  double theta, x_pseudo, theta_left, theta_right, rss, rss_left, rss_right, rss_sep;
  double fscore, min_rss_sep, best_pos = -1, best_theta_left, best_theta_right;

  /*
  start = Sequence_trim_start(queryseq);
  end = Sequence_trim_end(queryseq);
  */

  start = Stage3_querystart(stage3);
  end = Stage3_queryend(stage3);

  matchscores = Pair_matchscores(Stage3_pairarray(stage3),Stage3_npairs(stage3),queryntlength);

  x = 0;
  for (pos = start; pos < end; pos++) {
    x += matchscores[pos];
  }
  n = end - start;
  y = n - x;

  /* when rss_sep == rss, fscore == 0 */
  min_rss_sep = rss = (double) x * (double) y/(double) n;
  if (rss == 0.0) {
    FREE(matchscores);
    return 0;
  }

  theta = (double) x/(double) n;
  x_pseudo = NPSEUDO * theta;
  debug1(printf("%d %d %d %f\n",x,y,n,theta));
  
  x_left = y_left = n_left = 0;
  x_right = x;
  y_right = y;
  n_right = n;

  debug1(printf("%s %s %s %s %s %s %s %s %s %s %s %s %s\n",
		"pos","match","x.left","y.left","n.left","x.right","y.right","n.right",
		"theta.left","theta.right","rss.left","rss.right","fscore"));

  for (pos = start; pos < end-1; pos++) {
    if (matchscores[pos] == 1) {
      x_left++;
      x_right--;
    } else {
      y_left++;
      y_right--;
    }
    n_left++;
    n_right--;
    
    theta_left = ((double) x_left + x_pseudo)/((double) n_left + NPSEUDO);
    theta_right = ((double) x_right + x_pseudo)/((double) n_right + NPSEUDO);
    rss_left = x_left*(1.0-theta_left)*(1.0-theta_left) + y_left*theta_left*theta_left;
    rss_right = x_right*(1.0-theta_right)*(1.0-theta_right) + y_right*theta_right*theta_right;
    rss_sep = rss_left + rss_right;

    if (rss_sep == 0) {
      debug1(printf("%d %d %d %d %d %d %d %d %f %f %f %f NA\n",
		    pos,matchscores[pos],x_left,y_left,n_left,x_right,y_right,n_right,
		    theta_left,theta_right,rss_left,rss_right));
    } else {
      debug1(      
	     fscore = ((double) (n - 2))*(rss - rss_sep)/rss_sep;
	     printf("%d %d %d %d %d %d %d %d %f %f %f %f %f\n",
		    pos,matchscores[pos],x_left,y_left,n_left,x_right,y_right,n_right,
		    theta_left,theta_right,rss_left,rss_right,fscore);
	     );

      /* fscore = (n-2)*(rss - rss_sep)/rss_sep = (n-2)*(rss/rss_sep -
	 1) is maximized when rss_sep is minimized */

      if (rss_sep < min_rss_sep) {
	min_rss_sep = rss_sep;
	best_pos = pos;
	best_theta_left = theta_left;
	best_theta_right = theta_right;
      }
    }
  }
  FREE(matchscores);

  fscore = ((double) (n - 2))*(rss - min_rss_sep)/min_rss_sep;
  if (fscore < fthreshold) {
    return 0;
  } else {
    breakpoint = best_pos;
    debug1(printf("at %d, fscore = %f\n",breakpoint,fscore));
    if (best_theta_left < best_theta_right) {
      /* trim left */
      *newstart = breakpoint;
      *newend = end;
      return breakpoint - start;
    } else {
      /* trim right */
      *newstart = start;
      *newend = breakpoint;
      return end - breakpoint;
    }
  }
}


void
Chimera_bestpath (int *five_score, int *three_score, int *chimerapos, int *chimeraequivpos, int *bestfrom, int *bestto, 
		  Stage3_T *stage3array_sub1, int npaths_sub1, Stage3_T *stage3array_sub2, int npaths_sub2, 
		  int queryntlength) {
  int **matrix_sub1, **matrix_sub2, *from, *to, *bestscoreatpos, i, j, pos, score, 
    bestscore = -1000000;

  from = (int *) CALLOC(queryntlength,sizeof(int));
  to = (int *) CALLOC(queryntlength,sizeof(int));
  bestscoreatpos = (int *) CALLOC(queryntlength,sizeof(int));

  matrix_sub1 = (int **) CALLOC(npaths_sub1,sizeof(int *));
  for (i = 0; i < npaths_sub1; i++) {
    matrix_sub1[i] = (int *) CALLOC(queryntlength,sizeof(int));
    Stage3_pathscores(matrix_sub1[i],stage3array_sub1[i],queryntlength,FIVE);
  }

  matrix_sub2 = (int **) CALLOC(npaths_sub2,sizeof(int *));
  for (i = 0; i < npaths_sub2; i++) {
    matrix_sub2[i] = (int *) CALLOC(queryntlength,sizeof(int));
    Stage3_pathscores(matrix_sub2[i],stage3array_sub2[i],queryntlength,THREE);
  }

  for (pos = 0; pos < queryntlength; pos++) {
    bestscoreatpos[pos] = -100000;
  }
  debug(printf("npaths_sub1 = %d, npaths_sub2 = %d\n",npaths_sub1,npaths_sub2));
  for (pos = 0; pos < queryntlength; pos++) {
    for (i = 0; i < npaths_sub1; i++) {
      for (j = 0; j < npaths_sub2; j++) {
	/* Check for the same stage3 object on both lists */
	if (stage3array_sub1[i] != stage3array_sub2[j]) {
	  score = matrix_sub2[j][queryntlength-1] - matrix_sub2[j][pos] + matrix_sub1[i][pos] /* - 0 */;
	  if (score > bestscoreatpos[pos]) {
	    bestscoreatpos[pos] = score;
	    from[pos] = i;
	    to[pos] = j;
	  }
	}
      }
    }
  }

  for (pos = 0; pos < queryntlength; pos++) {
    if (bestscoreatpos[pos] > bestscore) {
      bestscore = bestscoreatpos[pos];
      *chimerapos = *chimeraequivpos = pos;
      *bestfrom = from[pos];
      *bestto = to[pos];
    } else if (bestscoreatpos[pos] == bestscore) {
      *chimeraequivpos = pos;
    }
  }
  *five_score = matrix_sub1[*bestfrom][*chimerapos] /* - 0 */;
  *three_score = matrix_sub2[*bestto][queryntlength-1] - matrix_sub2[*bestto][*chimerapos];

  debug(
	for (pos = 0; pos < queryntlength; pos++) {
	  printf("%d:",pos);
	  for (i = 0; i < npaths_sub1; i++) {
	    printf("\t%d",matrix_sub1[i][pos]);
	  }
	  printf("\t|");
	  for (i = 0; i < npaths_sub2; i++) {
	    printf("\t%d",matrix_sub2[i][pos]);
	  }
	  printf("\t||");
	  printf("%d (%d->%d)",bestscoreatpos[pos],from[pos],to[pos]);
	  if (pos >= *chimerapos && pos <= *chimeraequivpos) {
	    printf(" ** ");
	  }
	  printf("\n");
	}
	printf("From path %d to path %d at pos %d..%d.  5 score = %d, 3 score = %d\n",
	       *bestfrom,*bestto,*chimerapos,*chimeraequivpos,*five_score,*three_score);
	fflush(stdout);
	);

  for (i = 0; i < npaths_sub2; i++) {
    FREE(matrix_sub2[i]);
  }
  FREE(matrix_sub2);
  for (i = 0; i < npaths_sub1; i++) {
    FREE(matrix_sub1[i]);
  }
  FREE(matrix_sub1);

  FREE(bestscoreatpos);
  FREE(to);
  FREE(from);

  return;
}


static double
find_exonexon_fwd (int *exonexonpos, double *donor_prob, double *acceptor_prob,
		   Stage3_T left_part, Stage3_T right_part, Genome_T genome, IIT_T chromosome_iit,
		   int breakpoint_start, int breakpoint_end) {
  Sequence_T donor_genomicseg, acceptor_genomicseg;
  char gbuffer1[GBUFFERLEN], gbuffer2[GBUFFERLEN], gbuffer3[GBUFFERLEN], gbuffer4[GBUFFERLEN], 
    *gbuffer1aux, *gbuffer2aux, *gbuffer3aux, *gbuffer4aux, *donor_ptr, *acceptor_ptr;
  int i, j;
  Genomicpos_T left, donor_length, acceptor_length;
  bool revcomp, allocate12p, allocate34p;
  double donor_prob_1, acceptor_prob_1, bestproduct = 0.0, product;

  *exonexonpos = -1;

  donor_length = breakpoint_end - breakpoint_start + DONOR_MODEL_LEFT_MARGIN + DONOR_MODEL_RIGHT_MARGIN;
  left = Stage3_genomicpos(left_part,breakpoint_start,/*headp*/false);
  if (Stage3_watsonp(left_part) == true) {
    left -= DONOR_MODEL_LEFT_MARGIN;
    revcomp = false;
  } else {
    left += DONOR_MODEL_LEFT_MARGIN;
    left -= donor_length;
    revcomp = true;
  }

  if (donor_length + 2 > GBUFFERLEN) {
    allocate12p = true;
    gbuffer1aux = (char *) CALLOC(donor_length+2,sizeof(char));
    gbuffer2aux = (char *) CALLOC(donor_length+2,sizeof(char));
    donor_genomicseg = Genome_get_segment(genome,left,donor_length+1,chromosome_iit,revcomp,
					  gbuffer1aux,gbuffer2aux,donor_length+2);
  } else {
    allocate12p = false;
    donor_genomicseg = Genome_get_segment(genome,left,donor_length+1,chromosome_iit,revcomp,
					  gbuffer1,gbuffer2,/*gbufferlen*/GBUFFERLEN);
  }
  donor_ptr = Sequence_fullpointer(donor_genomicseg);
  debug2(
	 for (i = 0; i < ACCEPTOR_MODEL_LEFT_MARGIN - DONOR_MODEL_LEFT_MARGIN; i++) {
	   printf(" ");
	 }
	 Sequence_print(stdout,donor_genomicseg,/*uppercasep*/true,/*wraplength*/50,/*trimmedp*/false);
	 );

  acceptor_length = breakpoint_end - breakpoint_start + ACCEPTOR_MODEL_LEFT_MARGIN + ACCEPTOR_MODEL_RIGHT_MARGIN;
  left = Stage3_genomicpos(right_part,breakpoint_end+1,/*headp*/true);
  if (Stage3_watsonp(right_part) == true) {
    left += ACCEPTOR_MODEL_RIGHT_MARGIN;
    left -= acceptor_length;
    revcomp = false;
  } else {
    left -= ACCEPTOR_MODEL_RIGHT_MARGIN;
    revcomp = true;
  }

  if (acceptor_length + 2 > GBUFFERLEN) {
    allocate34p = true;
    gbuffer3aux = (char *) CALLOC(acceptor_length+2,sizeof(char));
    gbuffer4aux = (char *) CALLOC(acceptor_length+2,sizeof(char));
    acceptor_genomicseg = Genome_get_segment(genome,left,acceptor_length+1,chromosome_iit,revcomp,
					     gbuffer3aux,gbuffer4aux,acceptor_length+2);
  } else {
    allocate34p = false;
    acceptor_genomicseg = Genome_get_segment(genome,left,acceptor_length+1,chromosome_iit,revcomp,
					     gbuffer3,gbuffer4,/*gbufferlen*/GBUFFERLEN);
  }
  acceptor_ptr = Sequence_fullpointer(acceptor_genomicseg);
  debug2(
	 for (i = 0; i < DONOR_MODEL_LEFT_MARGIN - ACCEPTOR_MODEL_LEFT_MARGIN; i++) {
	   printf(" ");
	 }
	 Sequence_print(stdout,acceptor_genomicseg,/*uppercasep*/true,/*wraplength*/50,/*trimmedp*/false);
	 );

  *donor_prob = 0.0;
  *acceptor_prob = 0.0;
  for (i = DONOR_MODEL_LEFT_MARGIN, j = ACCEPTOR_MODEL_LEFT_MARGIN; 
       i <= donor_length - DONOR_MODEL_RIGHT_MARGIN && 
	 j <= acceptor_length - ACCEPTOR_MODEL_RIGHT_MARGIN;
       i++, j++) {
    debug2(printf("%c%c %c%c\n",donor_ptr[i+1],donor_ptr[i+2],acceptor_ptr[j-2],acceptor_ptr[j-1]));
    if (donor_ptr[i+1] == 'G' && donor_ptr[i+2] == 'T' &&
	acceptor_ptr[j-2] == 'A' && acceptor_ptr[j-1] == 'G') {
      donor_prob_1 = Maxent_donor_prob(&(donor_ptr[i+1-DONOR_MODEL_LEFT_MARGIN]));
      acceptor_prob_1 = Maxent_acceptor_prob(&(acceptor_ptr[j-ACCEPTOR_MODEL_LEFT_MARGIN]));
      if ((product = donor_prob_1*acceptor_prob_1) > bestproduct) {
	bestproduct = product;
	*donor_prob = donor_prob_1;
	*acceptor_prob = acceptor_prob_1;
	*exonexonpos = breakpoint_start - DONOR_MODEL_LEFT_MARGIN + i;
	debug2(printf("%f %f %d\n",donor_prob_1,acceptor_prob_1,*exonexonpos));
      }
    }
  }

  Sequence_free(&acceptor_genomicseg);
  Sequence_free(&donor_genomicseg);
  if (allocate34p == true) {
    FREE(gbuffer4aux);
    FREE(gbuffer3aux);
  }
  if (allocate12p == true) {
    FREE(gbuffer2aux);
    FREE(gbuffer1aux);
  }

  return bestproduct;
}

static double
find_exonexon_rev (int *exonexonpos, double *donor_prob, double *acceptor_prob,
		   Stage3_T left_part, Stage3_T right_part, Genome_T genome, IIT_T chromosome_iit,
		   int breakpoint_start, int breakpoint_end) {
  Sequence_T donor_genomicseg, acceptor_genomicseg;
  char gbuffer1[GBUFFERLEN], gbuffer2[GBUFFERLEN], gbuffer3[GBUFFERLEN], gbuffer4[GBUFFERLEN], 
    *gbuffer1aux, *gbuffer2aux, *gbuffer3aux, *gbuffer4aux, *donor_ptr, *acceptor_ptr;
  int i, j;
  Genomicpos_T left, donor_length, acceptor_length;
  bool revcomp, allocate12p, allocate34p;
  double donor_prob_1, acceptor_prob_1, bestproduct = 0.0, product;

  *exonexonpos = -1;

  donor_length = breakpoint_end - breakpoint_start + DONOR_MODEL_LEFT_MARGIN + DONOR_MODEL_RIGHT_MARGIN;
  left = Stage3_genomicpos(right_part,breakpoint_end+1,/*headp*/true);
  if (Stage3_watsonp(right_part) == true) {
    left += DONOR_MODEL_LEFT_MARGIN;
    left -= donor_length;
    revcomp = true;
  } else {
    left -= DONOR_MODEL_LEFT_MARGIN;
    revcomp = false;
  }

  if (donor_length + 2 > GBUFFERLEN) {
    allocate12p = true;
    gbuffer1aux = (char *) CALLOC(donor_length+2,sizeof(char));
    gbuffer2aux = (char *) CALLOC(donor_length+2,sizeof(char));
    donor_genomicseg = Genome_get_segment(genome,left,donor_length+1,chromosome_iit,revcomp,
					  gbuffer1aux,gbuffer2aux,donor_length+2);
  } else {
    allocate12p = false;
    donor_genomicseg = Genome_get_segment(genome,left,donor_length+1,chromosome_iit,revcomp,
					  gbuffer1,gbuffer2,/*gbufferlen*/GBUFFERLEN);
  }
  donor_ptr = Sequence_fullpointer(donor_genomicseg);
  debug2(
	 for (i = 0; i < ACCEPTOR_MODEL_LEFT_MARGIN - DONOR_MODEL_LEFT_MARGIN; i++) {
	   printf(" ");
	 }
	 Sequence_print(stdout,donor_genomicseg,/*uppercasep*/true,/*wraplength*/50,/*trimmedp*/false);
	 );

  acceptor_length = breakpoint_end - breakpoint_start + ACCEPTOR_MODEL_LEFT_MARGIN + ACCEPTOR_MODEL_RIGHT_MARGIN;
  left = Stage3_genomicpos(left_part,breakpoint_start,/*headp*/false);
  if (Stage3_watsonp(left_part) == true) {
    left -= ACCEPTOR_MODEL_RIGHT_MARGIN;
    revcomp = true;
  } else {
    left += ACCEPTOR_MODEL_RIGHT_MARGIN;
    left -= acceptor_length;
    revcomp = false;
  }

  if (acceptor_length + 2 > GBUFFERLEN) {
    allocate34p = true;
    gbuffer3aux = (char *) CALLOC(acceptor_length+2,sizeof(char));
    gbuffer4aux = (char *) CALLOC(acceptor_length+2,sizeof(char));
    acceptor_genomicseg = Genome_get_segment(genome,left,acceptor_length+1,chromosome_iit,revcomp,
					     gbuffer3aux,gbuffer4aux,acceptor_length+2);
  } else {
    allocate34p = false;
    acceptor_genomicseg = Genome_get_segment(genome,left,acceptor_length+1,chromosome_iit,revcomp,
					     gbuffer3,gbuffer4,/*gbufferlen*/GBUFFERLEN);
  }
  acceptor_ptr = Sequence_fullpointer(acceptor_genomicseg);
  debug2(
	 for (i = 0; i < DONOR_MODEL_LEFT_MARGIN - ACCEPTOR_MODEL_LEFT_MARGIN; i++) {
	   printf(" ");
	 }
	 Sequence_print(stdout,acceptor_genomicseg,/*uppercasep*/true,/*wraplength*/50,/*trimmedp*/false);
	 );

  *donor_prob = 0.0;
  *acceptor_prob = 0.0;
  for (i = DONOR_MODEL_LEFT_MARGIN, j = ACCEPTOR_MODEL_LEFT_MARGIN; 
       i <= donor_length - DONOR_MODEL_RIGHT_MARGIN && 
	 j <= acceptor_length - ACCEPTOR_MODEL_RIGHT_MARGIN;
       i++, j++) {
    debug2(printf("%c%c %c%c\n",donor_ptr[i+1],donor_ptr[i+2],acceptor_ptr[j-2],acceptor_ptr[j-1]));
    if (donor_ptr[i+1] == 'G' && donor_ptr[i+2] == 'T' &&
	acceptor_ptr[j-2] == 'A' && acceptor_ptr[j-1] == 'G') {
      donor_prob_1 = Maxent_donor_prob(&(donor_ptr[i+1-DONOR_MODEL_LEFT_MARGIN]));
      acceptor_prob_1 = Maxent_acceptor_prob(&(acceptor_ptr[j-ACCEPTOR_MODEL_LEFT_MARGIN]));
      if ((product = donor_prob_1*acceptor_prob_1) > bestproduct) {
	bestproduct = product;
	*donor_prob = donor_prob_1;
	*acceptor_prob = acceptor_prob_1;
	*exonexonpos = breakpoint_end + DONOR_MODEL_LEFT_MARGIN - i;
	debug2(printf("%f %f %d\n",donor_prob_1,acceptor_prob_1,*exonexonpos));
      }
    }
  }

  Sequence_free(&acceptor_genomicseg);
  Sequence_free(&donor_genomicseg);
  if (allocate34p == true) {
    FREE(gbuffer4aux);
    FREE(gbuffer3aux);
  }
  if (allocate12p == true) {
    FREE(gbuffer2aux);
    FREE(gbuffer1aux);
  }

  return bestproduct;
}


void
Chimera_find_exonexon (T this, Stage3_T left_part, Stage3_T right_part,
		       Genome_T genome, IIT_T chromosome_iit) {
  int breakpoint_start, breakpoint_end, exonexonpos_fwd, exonexonpos_rev;
  double bestproduct_fwd, bestproduct_rev, donor_prob_fwd, donor_prob_rev, acceptor_prob_fwd, acceptor_prob_rev;
  int left_cdna_direction, right_cdna_direction, try_direction;

  breakpoint_start = this->chimerapos;
  breakpoint_end = this->equivpos;

  left_cdna_direction = Stage3_cdna_direction(left_part);
  right_cdna_direction = Stage3_cdna_direction(right_part);

  if (left_cdna_direction == 0 && right_cdna_direction == 0) {
    try_direction = 0;
  } else if (left_cdna_direction >= 0 && right_cdna_direction >= 0) {
    try_direction = +1;
  } else if (left_cdna_direction <= 0 && right_cdna_direction <= 0) {
    try_direction = -1;
  } else {
    try_direction = 0;
  }

  if (try_direction == +1) {
    this->cdna_direction = +1;
    find_exonexon_fwd(&this->exonexonpos,&this->donor_prob,&this->acceptor_prob,left_part,right_part,genome,
		      chromosome_iit,breakpoint_start,breakpoint_end);
  } else if (try_direction == -1) {
    this->cdna_direction = -1;
    find_exonexon_rev(&this->exonexonpos,&this->donor_prob,&this->acceptor_prob,left_part,right_part,genome,
		      chromosome_iit,breakpoint_start,breakpoint_end);
  } else {
    bestproduct_fwd = find_exonexon_fwd(&exonexonpos_fwd,&donor_prob_fwd,&acceptor_prob_fwd,
					left_part,right_part,genome,chromosome_iit,breakpoint_start,breakpoint_end);
    bestproduct_rev = find_exonexon_rev(&exonexonpos_rev,&donor_prob_rev,&acceptor_prob_rev,
					left_part,right_part,genome,chromosome_iit,breakpoint_start,breakpoint_end);
    if (bestproduct_fwd == 0.0 && bestproduct_rev == 0.0) {
      this->cdna_direction = 0;
      this->exonexonpos = -1;
      this->donor_prob = 0.0;
      this->acceptor_prob = 0.0;
    } else if (bestproduct_fwd >= bestproduct_rev) {
      this->cdna_direction = +1;
      this->exonexonpos = exonexonpos_fwd;
      this->donor_prob = donor_prob_fwd;
      this->acceptor_prob = acceptor_prob_fwd;
    } else {
      this->cdna_direction = -1;
      this->exonexonpos = exonexonpos_rev;
      this->donor_prob = donor_prob_rev;
      this->acceptor_prob = acceptor_prob_rev;
    }
  }

  if (this->exonexonpos >= 0) {
    this->chimerapos = this->equivpos = this->exonexonpos;
  }

  return;
}


bool
Chimera_exonexon_p (int *exonexonpos, int *cdna_direction, double *donor_prob, double *acceptor_prob,
		    Stage3_T left_part, Stage3_T right_part, Genome_T genome, IIT_T chromosome_iit,int querylength) {
  int breakpoint_start, breakpoint_end, exonexonpos_fwd, exonexonpos_rev;
  double bestproduct_fwd, bestproduct_rev, donor_prob_fwd, donor_prob_rev, acceptor_prob_fwd, acceptor_prob_rev;
  int left_cdna_direction, right_cdna_direction, try_direction;

  debug2(printf("Starting Chimera_exonexon_p\n"));
  debug2(printf("left part covers query %d to %d\n",Stage3_querystart(left_part),Stage3_queryend(left_part)));
  debug2(printf("right part covers query %d to %d\n",Stage3_querystart(right_part),Stage3_queryend(right_part)));

  if (Stage3_queryend(left_part) < Stage3_querystart(right_part)) {
    breakpoint_start = Stage3_queryend(left_part);
    breakpoint_end = Stage3_querystart(right_part);
  } else {
    breakpoint_start = Stage3_querystart(right_part);
    breakpoint_end = Stage3_queryend(left_part);
  }
  breakpoint_start -= 10;
  if (breakpoint_start < 0) {
    breakpoint_start = 0;
  }
  breakpoint_end += 10;
  if (breakpoint_end > querylength-1) {
    breakpoint_end = querylength-1;
  }

  debug2(printf("Starting search for exon-exon boundary at breakpoint_start %d to breakpoint_end %d\n",
		breakpoint_start,breakpoint_end));

  left_cdna_direction = Stage3_cdna_direction(left_part);
  right_cdna_direction = Stage3_cdna_direction(right_part);

  if (left_cdna_direction == 0 && right_cdna_direction == 0) {
    try_direction = 0;
  } else if (left_cdna_direction >= 0 && right_cdna_direction >= 0) {
    try_direction = +1;
  } else if (left_cdna_direction <= 0 && right_cdna_direction <= 0) {
    try_direction = -1;
  } else {
    try_direction = 0;
  }

  if (try_direction == +1) {
    *cdna_direction = +1;
    find_exonexon_fwd(&(*exonexonpos),&(*donor_prob),&(*acceptor_prob),left_part,right_part,genome,
		      chromosome_iit,breakpoint_start,breakpoint_end);
  } else if (try_direction == -1) {
    *cdna_direction = -1;
    find_exonexon_rev(&(*exonexonpos),&(*donor_prob),&(*acceptor_prob),left_part,right_part,genome,
		      chromosome_iit,breakpoint_start,breakpoint_end);
  } else {
    bestproduct_fwd = find_exonexon_fwd(&exonexonpos_fwd,&donor_prob_fwd,&acceptor_prob_fwd,
					left_part,right_part,genome,chromosome_iit,breakpoint_start,breakpoint_end);
    bestproduct_rev = find_exonexon_rev(&exonexonpos_rev,&donor_prob_rev,&acceptor_prob_rev,
					left_part,right_part,genome,chromosome_iit,breakpoint_start,breakpoint_end);
    if (bestproduct_fwd == 0.0 && bestproduct_rev == 0.0) {
      *cdna_direction = 0;
      *exonexonpos = -1;
      *donor_prob = 0.0;
      *acceptor_prob = 0.0;
    } else if (bestproduct_fwd >= bestproduct_rev) {
      *cdna_direction = +1;
      *exonexonpos = exonexonpos_fwd;
      *donor_prob = donor_prob_fwd;
      *acceptor_prob = acceptor_prob_fwd;
    } else {
      *cdna_direction = -1;
      *exonexonpos = exonexonpos_rev;
      *donor_prob = donor_prob_rev;
      *acceptor_prob = acceptor_prob_rev;
    }
  }

  if (*exonexonpos >= 0) {
    return true;
  } else {
    return false;
  }
}



