/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*  Copyright (c) 2002-2023 Zuse Institute Berlin (ZIB)                      */
/*                                                                           */
/*  Licensed under the Apache License, Version 2.0 (the "License");          */
/*  you may not use this file except in compliance with the License.         */
/*  You may obtain a copy of the License at                                  */
/*                                                                           */
/*      http://www.apache.org/licenses/LICENSE-2.0                           */
/*                                                                           */
/*  Unless required by applicable law or agreed to in writing, software      */
/*  distributed under the License is distributed on an "AS IS" BASIS,        */
/*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. */
/*  See the License for the specific language governing permissions and      */
/*  limitations under the License.                                           */
/*                                                                           */
/*  You should have received a copy of the Apache-2.0 license                */
/*  along with SCIP; see the file LICENSE. If not visit scipopt.org.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

//#define SCIP_DEBUG
/**@file   heur_dks.c
 * @ingroup DEFPLUGINS_HEUR
 * @brief  dks primal heuristic
 * @author Adrian Göß
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "scip/pub_lp.h"

#include "blockmemshell/memory.h"
#include "scip/cons_linear.h"
#include "scip/debug.h"
#include "scip/heuristics.h"
#include "scip/pub_cons.h"
#include "scip/pub_event.h"
#include "scip/pub_fileio.h"
#include "scip/pub_tree.h"
#include "scip/pub_heur.h"
#include "scip/pub_message.h"
#include "scip/pub_misc.h"
#include "scip/pub_misc_select.h"
#include "scip/pub_sol.h"
#include "scip/pub_var.h"
#include "scip/scipdefplugins.h"
#include "scip/scip_branch.h"
#include "scip/scip_cons.h"
#include "scip/scip_copy.h"
#include "scip/scip_dcmp.h"
#include "scip/scip_event.h"
#include "scip/scip_general.h"
#include "scip/scip_heur.h"
#include "scip/scip_lp.h"
#include "scip/scip_mem.h"
#include "scip/scip_message.h"
#include "scip/scip_nodesel.h"
#include "scip/scip_numerics.h"
#include "scip/scip_param.h"
#include "scip/scip_prob.h"
#include "scip/scip_randnumgen.h"
#include "scip/scip_sol.h"
#include "scip/scip_solve.h"
#include "scip/scip_solvingstats.h"
#include "scip/scip_table.h"
#include "scip/scip_timing.h"
#include "scip/scip_tree.h"
#include "scip/scip_var.h"
#include "scip/sol.h"

#include "scip/heur_dks.h"


#define HEUR_NAME             "dks"
#define HEUR_DESC             "decomposition kernel search"
#define HEUR_DISPCHAR         'D'
#define HEUR_PRIORITY         -1102500
#define HEUR_FREQ             0
#define HEUR_FREQOFS          0
#define HEUR_MAXDEPTH         0
#define HEUR_TIMING           SCIP_HEURTIMING_AFTERLPNODE
#define HEUR_USESSUBSCIP      TRUE  /**< does the heuristic use a secondary SCIP instance? */

#define DEFAULT_ADDUSECONSS      TRUE  /**< default value to add a use constraint */
#define DEFAULT_LINKBUCKSIZE     TRUE  /**< default value to respect kernel linking variables in the calculation of the bucket size */
#define DEFAULT_TRANSLBKERNEL    TRUE  /**< default value to respect the difference of the variable lower bound in trans and orig prob */
#define DEFAULT_LESSLOCKSKERNEL  FALSE /**< default value to respect <= 1 up- and downlock in the kernel construction */
#define DEFAULT_USETRANSPROB     TRUE  /**< default value to use the original or transformed problem **/
#define DEFAULT_MAX_TIME         0.1   /**< default time limit of heuristic in seconds */
#define DEFAULT_USETWOLEVEL      TRUE  /**< default value to use a two level structure for the buckets */
#define DEFAULT_USEDECOMP        TRUE  /**< default value to use the decomp if given */
#define DEFAULT_USEBESTSOL       TRUE  /**< default value to use the best existing solution or the lp solution alternatively */
#define DEFAULT_REDCOSTSORT      TRUE  /**< default value to sort the non kernel variables before dividing into buckets */
#define DEFAULT_DECKERNEL        TRUE  /**< default value to decrease the kernel if a feasible better solution is found */
#define DEFAULT_PRIMALONLY       FALSE /**< default value to kill dks after the first primal solution is found */
#define DEFAULT_REDCOSTLOGSORT   TRUE  /**< default value to sort the non kernel variables logarithmically by reduced costs */
#define DEFAULT_OBJCUTOFF        TRUE  /**< default value to add an objective cutoff */

/*
 * Data structures
 */

/** data related to one bucket list, details see below **/
typedef struct Bucketlist BUCKETLIST;

/** data related to one bucket **/
typedef struct Bucket
{
   BUCKETLIST*             bucketlist;       /** <the bucketlist the bucket belongs to **/
   SCIP*                   subscip;          /** <scip structure to solve smaller MIPs later **/
   int                     number;           /** <component number **/
   SCIP_VAR**              contbucketvars;   /** <continuous variables for this bucket **/
   SCIP_VAR**              bucketvars;       /** <variables of this bucket, just binary if 2-level bucket **/
   SCIP_VAR**              intbucketvars;    /** <just integer variables if 2-level bucket **/
   int                     ncontbucketvars;  /** <amount of continuous variables in this bucket **/
   int                     nbucketvars;      /** <amount of variables in this bucket **/
   int                     nintbucketvars;   /** <amount of integer variables in a 2-level bucket **/
   SCIP_Bool               twolevel;         /** <is the current bucket a 2-level one **/
   SCIP_VAR**              sub2scip;         /** <mapping the variables to the original ones **/
   SCIP_VAR**              scip2sub;
} BUCKET;

/** data related to one whole list of buckets **/
struct Bucketlist
{
   SCIP*             scip;                   /** <scip instance this bucketlist belongs to **/
   BUCKET*           buckets;                /** <buckets in this bucketlist **/ 
   int               nbuckets;               /** <amount of buckets in this bucketlist **/
};

/** primal heuristic data */
struct SCIP_HeurData
{
   int                  maxbucks;                  /** <maximum amount of buckets that are investigated**/
   SCIP_Real            kernelsizefactor;          /** <factor with which initial kernel can grow max */
   SCIP_Bool            addUseConss;               /** <add a constraint that ensures a use of the bucket variables or not */
   SCIP_Bool            linkbucksize;              /** <respect the kernel linking variables in the calculation of the initial bucket size */
   SCIP_Bool            translbkernel;             /** <respect the lower bound of the variable in the transformed problem at the kernel construction */
   SCIP_Bool            lesslockskernel;           /** <respect variables with <= 1 up and downlock at the kernel construction */
   SCIP_Bool            usetransprob;              /** <use the transformed problem instead of the original one **/
   SCIP_Real            buckmaxgap;                /** <set an upper bound for the maximum mip gap of each bucket */
   SCIP_Real            maxlinkscore;              /** <set an upper bound for the linking score a decomp can have to get solved by dks */
   SCIP_Real            maxtimeshare;              /** <set time share for the heuristic */
   SCIP_Bool            usetwolevel;               /** <use two level structure for the buckets if possible */
   SCIP_Bool            usedecomp;                 /** <use decomp if given */
   SCIP_Bool            usebestsol;                /** <use best solution or alt. LP sol */
   SCIP_Bool            redcostsort;               /** <sort the non kernel variables by reduced costs */
   SCIP_Bool            deckernel;                 /** <decrease the kernel to non-lower-bound variables when a solution is found */
   SCIP_Bool            primalonly;                /** <terminate after the first found primal solution */
   SCIP_Bool            redcostlogsort;
   SCIP_Bool            objcutoff;                 /** <add an objective cutoff for the current best sol */
   int                  ncalls;                    /** <amount of calls of the heuristic */
};


/*
 * Local methods
 */

static
SCIP_Real getMaxReal(
   SCIP*                scip,                /**< main SCIP data structure */     
   SCIP_Real*           array,               /**< array to return the maximum from */
   int                  length               /**< length of the array */
)
{
   int i;
   SCIP_Real maximum= array[0];

   assert(length > 0);

   for ( i = 1; i < length; i++ )
      if ( maximum < array[i] )
         maximum = array[i];

   return maximum;
}

/** calculate the linking score of a given decomposition */
static
SCIP_RETCODE getLinkingScoreAndBlocklabels(
   SCIP*                scip,                /**< main SCIP data structure */
   int**                blocklabels,         /**< int array to store the different block labels */
   int*                 varlabels,           /**< array of variable labels */
   int*                 conslabels,          /**< array of constraint labels */
   SCIP_Real*           linkscore,           /**< linking score to return */
   int*                 nblocklabels,        /**< number of block labels to return */
   int                  nblocks,             /**< number of blocks */
   int                  nvars,               /**< number of variables */
   int                  nconss               /**< number of constraints */
)
{
   /* initialization of variables */
   /* counters */
   int v;                           
   int b;

   SCIP_Bool newlabel = TRUE;       /*< indication of finding a new label */
   int nlinkscoreconss = 0;         /*< number of linking conss for calculation */
   int nlinkscorevars = 0;          /*< number of linking vars for calculation */
   *nblocklabels = 0;               /*< number of distinct block labels */


   for ( v = 0; v < nvars; v++ )
   {
      /* counting of linking variables */
      if ( varlabels[v] == SCIP_DECOMP_LINKVAR )
         nlinkscorevars++;
      /* fill an array for the existing distinct block labels that are not linking variables */
      else if ( *nblocklabels < nblocks && blocklabels != NULL )
      {
         newlabel = TRUE;
         /* check the current label for novelty */
         for ( b = 0; b < *nblocklabels; b++ )
         {
            if ( (*blocklabels)[b] == varlabels[v])
            {
               newlabel = FALSE;
               break;
            }
         }

         /* add unseen labels */
         if ( newlabel )
            (*blocklabels)[(*nblocklabels)++] = varlabels[v];
      }
   }

   /* counting of linking constraints */
   for ( v = 0; v < nconss; v++ )
   {
      if ( conslabels[v] == SCIP_DECOMP_LINKCONS )
         nlinkscoreconss++;
   }

   /* linking score calculation */
   *linkscore = ( (SCIP_Real)nlinkscorevars*(SCIP_Real)nconss + (SCIP_Real)nlinkscoreconss*(SCIP_Real)nvars - 
                (SCIP_Real)nlinkscorevars*(SCIP_Real)nlinkscoreconss ) / ((SCIP_Real)nconss*(SCIP_Real)nvars);

   return SCIP_OKAY; 
}

/** count of potential kernel variables for one level or two level structure */
static
SCIP_RETCODE countKernelVariables(
   SCIP*                scip,                   /**< main SCIP data structure */
   SCIP_VAR**           vars,                   /**< array of variables */
   SCIP_SOL*            bestcurrsol,            /**< best current solution */
   SCIP_HASHMAP*        lbvarmap,               /**< original lower bound of transformed variables */
   SCIP_Bool            twolevel,               /**< usage of one or two level structure */
   SCIP_Bool            usebestsol,             /**< usage of best or lp solution */
   SCIP_Bool            usetransprob,           /**< usage of transformed or original problem */
   SCIP_Bool            usetranslb,             /**< usage of transformed lb in comparison to original lb */
   int**                bw_ncontkernelvars,     /**< blockwise number of continuous kernel variables */
   int**                bw_ncontnonkernelvars,  /**< blockwise number of continuous non-kernel variables */
   int**                bw_nkernelvars,         /**< blockwise number of (binary) kernel variables */
   int**                bw_nnonkernelvars,      /**< blockwise number of (binary) non-kernel variables */
   int**                bw_nintkernelvars,      /**< blockwise number of integer kernel variables */
   int**                bw_nintnonkernelvars,   /**< blockwise number of integer non-kernel variables */
   int*                 ncontkernelvars,        /**< number of continuous kernel variables */
   int*                 ncontnonkernelvars,     /**< number of continuous non-kernel variables */
   int*                 nkernelvars,            /**< number of (binary) kernel variables */
   int*                 nnonkernelvars,         /**< number of (binary) non-kernel variables */
   int*                 nintkernelvars,         /**< number of integer kernel variables */
   int*                 nintnonkernelvars,      /**< number of integer non-kernel variables */
   int*                 block2index,            /**< mapping of block labels to block index */
   int*                 varlabels,              /**< array of variable labels */
   int                  blklbl_offset,          /**< optional offset for the blocklabels, if it exists a block 0 */
   int                  nvars                   /**< number of variables */
)
{
   /* initialization of variables */
   SCIP_Real lpval;              /*< variable value in LP solution */
   SCIP_Real lb;                 /*< variable lower bound */
   SCIP_Real lborig;             /*< variable lower bound in original problem */
   int i;                        /*< counter */
   int block;                    /*< block of variable */

   /* count all possible kernel variables dependent on their type blockwise and overall */
   for ( i = 0; i < nvars; i++ )
   {
      /* calculate the variable's LP solution value, the lower bound in the transformed and original problem */
      lpval = usebestsol ? SCIPgetSolVal(scip, bestcurrsol, vars[i]) : SCIPvarGetLPSol(vars[i]);
      lb = SCIPvarGetLbGlobal(vars[i]);
      lborig = usetransprob ? SCIPhashmapGetImageReal(lbvarmap, vars[i]) : SCIPvarGetLbOriginal(vars[i]);

      /* definition of the variable's block (SCIP_DECOMP_LINKVAR = -1, but is stored as 0) */
      block = block2index[MAX(varlabels[i] + blklbl_offset, 0)];

      switch ( SCIPvarGetType(vars[i]) )
      {
      /* compare binaries only to the lower bound of 0.0 and count as kernel or non-kernel variable */
      case SCIP_VARTYPE_BINARY:
         if ( !SCIPisEQ(scip, lpval, 0.0) )
         {
            (*nkernelvars)++;
            (*bw_nkernelvars)[block]++;
         }
         else
         {
            (*nnonkernelvars)++;
            (*bw_nnonkernelvars)[block]++;
         }
         break;
      /* LP value > lb -> count integer as kernel variable else not */
      /* count separatly if binaries and integers are present */
      case SCIP_VARTYPE_INTEGER:
         if (  (!SCIPisEQ(scip, lpval, 0.0) && !SCIPisEQ(scip, lpval, lb)) 
            || (usetranslb && SCIPisGT(scip, lb, lborig)) )
         {
            if ( twolevel )
            {
               (*nintkernelvars)++;
               (*bw_nintkernelvars)[block]++;
            }
            else
            {
               (*nkernelvars)++;
               (*bw_nkernelvars)[block]++;
            }
         }
         else
         {
            if ( twolevel )
            {
               (*nintnonkernelvars)++;
               (*bw_nintnonkernelvars)[block]++;
            }
            else
            {
               (*nnonkernelvars)++;
               (*bw_nnonkernelvars)[block]++;
            }
         }
         break;
      /* LP value > lower bound -> potential kernel variable else not for continuous vars */
      default:
         if (  (!SCIPisEQ(scip, lpval, 0.0) && !SCIPisEQ(scip, lpval, lb) )
            || (usetranslb && SCIPisGT(scip, lb, lborig)) )
         {
            (*ncontkernelvars)++;
            (*bw_ncontkernelvars)[block]++;
         }
         else
         {
            (*ncontnonkernelvars)++;
            (*bw_ncontnonkernelvars)[block]++;
         }
         break;
      }
   }
   
   return SCIP_OKAY;
}

/** fill the blockwise kernels with the respective variables */
static
SCIP_RETCODE fillKernels(
   SCIP*                scip,                   /**< main SCIP data structure */
   SCIP_VAR**           vars,                   /**< array of variables */
   SCIP_VAR***          binintvars,             /**< array of binary and integer variables */
   SCIP_VAR****         bw_contkernelvars,      /**< blockwise array of continuous kernel variables */
   SCIP_VAR****         bw_contnonkernelvars,   /**< blockwise array of continuous non-kernel variables */
   SCIP_VAR****         bw_kernelvars,          /**< blockwise array of (binary) kernel variables */
   SCIP_VAR****         bw_nonkernelvars,       /**< blockwise array of (binary) non-kernel variables */
   SCIP_VAR****         bw_intkernelvars,       /**< blockwise array of integer kernel variables */
   SCIP_VAR****         bw_intnonkernelvars,    /**< blockwise array of integer non-kernel variables */
   SCIP_SOL*            bestcurrsol,            /**< best current solution */
   SCIP_HASHMAP*        lbvarmap,               /**< original lower bound of transformed variables */
   SCIP_Bool            twolevel,               /**< usage of one or two level structure */
   SCIP_Bool            usebestsol,             /**< usage of best or lp solution */
   SCIP_Bool            usetransprob,           /**< usage of transformed or original problem */
   SCIP_Bool            usetranslb,             /**< usage of transformed lb in comparison to original lb */
   int**                bw_contkernelcount,     /**< blockwise counter of continuous kernel variables */
   int**                bw_contnonkernelcount,  /**< blockwise counter of continuous non-kernel variables */
   int**                bw_kernelcount,         /**< blockwise counter of (binary) kernel variables */
   int**                bw_nonkernelcount,      /**< blockwise counter of (binary) non-kernel variables */
   int**                bw_intkernelcount,      /**< blockwise counter of integer kernel variables */
   int**                bw_intnonkernelcount,   /**< blockwise counter of integer non-kernel variables */
   int*                 block2index,            /**< mapping of block labels to block index */
   int*                 varlabels,              /**< array of variable labels */
   int                  nblocks,                /**< number of blocks >= 1 */
   int                  blklbl_offset,          /**< optional offset for the blocklabels, if it exists a block 0 */
   int                  nvars                   /**< number of variables */
)
{
   /* initialization of variables */
   SCIP_Real lpval;              /*< variable value in LP solution */
   SCIP_Real lb;                 /*< variable lower bound */
   SCIP_Real lborig;             /*< variable lower bound in original problem */
   int i;                        /*< variable counter */
   int j = 0;                    /*< integer and binary variable counter */
   int m;                        /*< temporary integer variable index */
   int n;                        /*< temporary (binary) variable index */
   int l;                        /*< temporary continuous variable index */
   int block_index;              /*< index of block of variable */

   /* assign all variables dependent on their type blockwise to a kernel or a non-kernel */
   for ( i = 0; i < nvars; i++ )
   {
      /* calculate the variable's LP solution value, the lower bound in the transformed and original problem */
      lpval = usebestsol ? SCIPgetSolVal(scip, bestcurrsol, vars[i]) : SCIPvarGetLPSol(vars[i]);
      lb = SCIPvarGetLbGlobal(vars[i]);
      lborig = usetransprob ? SCIPhashmapGetImageReal(lbvarmap, vars[i]) : SCIPvarGetLbOriginal(vars[i]);

      /* definition of the variable's block index (SCIP_DECOMP_LINKVAR = -1, but is stored as 0 in block2index) */
      block_index = nblocks == 0 ? 0 : block2index[MAX(varlabels[i] + blklbl_offset, 0)];

      switch ( SCIPvarGetType(vars[i]) )
      {
      /* compare binaries only to the lower bound of 0.0 and add to kernel or non-kernel variables */
      case SCIP_VARTYPE_BINARY:
         /* adding the variable to the binary and integer variable array */
         (*binintvars)[j++] = vars[i];

         if ( !SCIPisEQ(scip, lpval, 0.0) )
         {
            n = (*bw_kernelcount)[block_index];
            (*bw_kernelvars)[block_index][n] = vars[i];
            ((*bw_kernelcount)[block_index])++;
         }
         else
         {
            n = (*bw_nonkernelcount)[block_index];
            (*bw_nonkernelvars)[block_index][n] = vars[i];
            ((*bw_nonkernelcount)[block_index])++;
         }
         break;
      /* LP value > lb -> integer kernel variable else non-kernel variable */
      /* count separatly if binaries and integers are present */
      case SCIP_VARTYPE_INTEGER:
         /* adding the variable to the binary and integer variable array */
         (*binintvars)[j++] = vars[i];

         if (  (!SCIPisEQ(scip, lpval, 0.0) && !SCIPisEQ(scip, lpval, lb)) 
            || (usetranslb && SCIPisGT(scip, lb, lborig)) )
         {
            if ( twolevel )
            {
               m = (*bw_intkernelcount)[block_index];
               (*bw_intkernelvars)[block_index][m] = vars[i];
               ((*bw_intkernelcount)[block_index])++;
            }
            else
            {
               m = (*bw_kernelcount)[block_index];
               (*bw_kernelvars)[block_index][m] = vars[i];
               ((*bw_kernelcount)[block_index])++;
            }
         }
         else
         {
            if ( twolevel )
            {
               m = (*bw_intnonkernelcount)[block_index];
               (*bw_intnonkernelvars)[block_index][m] = vars[i];
               ((*bw_intnonkernelcount)[block_index])++;
            }
            else
            {
               m = (*bw_nonkernelcount)[block_index];
               (*bw_nonkernelvars)[block_index][m] = vars[i];
               ((*bw_nonkernelcount)[block_index])++;
            }
         }
         break;
      /* LP value > lower bound -> continuous kernel variable else non-kernel variable */
      default:
         if (  (!SCIPisEQ(scip, lpval, 0.0) && !SCIPisEQ(scip, lpval, lb) )
            || (usetranslb && SCIPisGT(scip, lb, lborig)) )
         {
            l = (*bw_contkernelcount)[block_index];
            (*bw_contkernelvars)[block_index][l] = vars[i];
            ((*bw_contkernelcount)[block_index])++;
         }
         else
         {
            l = (*bw_contnonkernelcount)[block_index];
            (*bw_contnonkernelvars)[block_index][l] = vars[i];
            ((*bw_contnonkernelcount)[block_index])++;
         }
         break;
      }
   }

   return SCIP_OKAY;
}

/** calculation of reduced costs and non-decreasing sorting **/
static
SCIP_RETCODE reducedCostSort(
   SCIP*                scip,                   /**< main SCIP data structure */
   SCIP_VAR****         bw_contnonkernelvars,   /**< array pointer of continuous, non-kernel variables */
   SCIP_VAR****         bw_nonkernelvars,       /**< array pointer of (binary,) non-kernel variables */
   SCIP_VAR****         bw_intnonkernelvars,    /**< array pointer of integer, non-kernel variables */
   SCIP_Real***         bw_cont_redcost,        /**< array pointer with reduced costs for continuous variables */
   SCIP_Real***         bw_redcost,             /**< array pointer with reduced costs for (binary) variables */
   SCIP_Real***         bw_int_redcost,         /**< array pointer with reduced costs for integer variables */
   int*                 bw_ncontnonkernelvars,  /**< blockwise number of continuous, non-kernel variables */
   int*                 bw_nnonkernelvars,      /**< blockwise number of (binary,) non-kernel variables */
   int*                 bw_nintnonkernelvars,   /**< blockwise number of integer, non-kernel variables */
   SCIP_Bool            twolevel,               /**< usage of one or two level structure */
   int                  nblocks                 /**< number of blocks */
)
{
   /** initialization of variables */
   int b;               /**< block counter */
   int i;               /**< variable counter */

   /* blockwise initialization of block arrays for the reduced costs */
   SCIP_CALL( SCIPallocBufferArray(scip, bw_cont_redcost, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, bw_redcost, nblocks + 1) );
   if ( twolevel )
      SCIP_CALL( SCIPallocBufferArray(scip, bw_int_redcost, nblocks + 1) );

   /* blockwise and type-wise extraction of reduced costs and sorting in non-decreasing order */
   for (b = 0; b < nblocks + 1; b++ )
   {
      SCIP_CALL( SCIPallocBufferArray(scip, &((*bw_cont_redcost)[b]), bw_ncontnonkernelvars[b]) );
      SCIP_CALL( SCIPallocBufferArray(scip, &((*bw_redcost)[b]), bw_nnonkernelvars[b]) );
      
      for ( i = 0; i < bw_ncontnonkernelvars[b]; i++ )
         (*bw_cont_redcost)[b][i] = SCIPgetVarRedcost(scip, (*bw_contnonkernelvars)[b][i]);

      for ( i = 0; i < bw_nnonkernelvars[b]; i++ )
      {
         (*bw_redcost)[b][i] = SCIPgetVarRedcost(scip, (*bw_nonkernelvars)[b][i]);
         //assert(SCIPisGE(scip, (*bw_redcost)[b][i], 0.0));
      }

      SCIPsortRealPtr((*bw_cont_redcost)[b], (void**)((*bw_contnonkernelvars)[b]), bw_ncontnonkernelvars[b]);
      SCIPsortRealPtr((*bw_redcost)[b], (void**)((*bw_nonkernelvars)[b]), bw_nnonkernelvars[b]);

      if ( twolevel )
      {
         SCIP_CALL( SCIPallocBufferArray(scip, &((*bw_int_redcost)[b]), bw_nintnonkernelvars[b]) );

         for ( i = 0; i < bw_nintnonkernelvars[b]; i++ )
            (*bw_int_redcost)[b][i] = SCIPgetVarRedcost(scip, (*bw_intnonkernelvars)[b][i]);
         
         SCIPsortRealPtr((*bw_int_redcost)[b], (void**)((*bw_intnonkernelvars)[b]), bw_nintnonkernelvars[b]);
      }
   }

   return SCIP_OKAY;
}

static
SCIP_RETCODE freeRedcostArrays(
   SCIP*                scip,                   /**< main SCIP data structure */
   SCIP_Real***         bw_cont_redcost,        /**< array pointer with reduced costs for continuous variables */
   SCIP_Real***         bw_redcost,             /**< array pointer with reduced costs for (binary) variables */
   SCIP_Real***         bw_int_redcost,         /**< array pointer with reduced costs for integer variables */
   int                  nblocks                 /**< number of blocks */
)
{
   /** initialization of variables */
   int b;               /**< block counter */

   /** type-wise and blockwise freeing of reduced cost arrays */
   /* continuous case */
   if ( *bw_cont_redcost != NULL )
   {
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( (*bw_cont_redcost)[b] != NULL )
            SCIPfreeBufferArray(scip, &((*bw_cont_redcost)[b]));
      }
      SCIPfreeBufferArray(scip, bw_cont_redcost);
   }

   /* (binary) case */
   if ( *bw_redcost != NULL )
   {
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( (*bw_redcost)[b] != NULL )
            SCIPfreeBufferArray(scip, &((*bw_redcost)[b]));
      }
      SCIPfreeBufferArray(scip, bw_redcost);
   }

   /* integer case */
   if ( *bw_int_redcost != NULL )
   {
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( (*bw_int_redcost)[b] != NULL )
            SCIPfreeBufferArray(scip, &((*bw_int_redcost)[b]));
      }
      SCIPfreeBufferArray(scip, bw_int_redcost);
   }

   return SCIP_OKAY;
}

static
SCIP_RETCODE fillBuckets(
   SCIP*                scip,                   /**< main SCIP data structure */
   BUCKETLIST**         bucketlist,             /**< array pointer of buckets to fill */
   SCIP_VAR***          bw_contnonkernelvars,   /**< array of continuous, non-kernel variables */
   SCIP_VAR***          bw_nonkernelvars,       /**< array of (binary,) non-kernel variables */
   SCIP_VAR***          bw_intnonkernelvars,    /**< array of integer, non-kernel variables */
   int*                 bw_ncontnonkernelvars,  /**< blockwise number of continuous, non-kernel variables */
   int*                 bw_nnonkernelvars,      /**< blockwise number of (binary,) non-kernel variables */
   int*                 bw_nintnonkernelvars,   /**< blockwise number of integer, non-kernel variables */
   SCIP_Real**          bw_cont_redcost,        /**< blockwise reduced costs of continuous, non-kernel variables */
   SCIP_Real**          bw_redcost,             /**< blockwise reduced costs of (binary,) non-kernel variables */ 
   SCIP_Real**          bw_int_redcost,         /**< blockwise reduced costs of integer, non-kernel variables */                
   SCIP_Bool            twolevel,               /**< usage of one or two level structure */
   SCIP_Bool            redcostlogsort,         /**< filling the buckets by logarithmically reduced cost sort */
   int                  iters,                  /**< number of solving iterations */
   int                  nbuckets,               /**< number of buckets */
   int                  nblocks                 /**< number of blocks */
)
{
   /** initialization of variables */
   BUCKET* bucket;                  /**< temporary bucket */
   int contbucklength;              /**< temporary length of the continuous bucket */
   int bucklength;                  /**< temporary length of the (binary) bucket */
   int intbucklength;               /**< temporary length of the integer bucket */
   int fromcontvars;                /**< temporary start index for the variables of a continuous bucket */
   int tocontvars;                  /**< temporary end index for the variables of a continuous bucket */
   int fromvars;                    /**< temporary start index for the variables of a (binary) bucket */
   int tovars;                      /**< temporary end index for the variables of a (binary) bucket */
   int fromintvars;                 /**< temporary start index for the variables of a integer bucket */
   int tointvars;                   /**< temporary end index for the variables of a integer bucket */
   int k;                           /**< bucket counter */
   int b;                           /**< block counter */
   int l;                           /**< variable counter */
   int j;                           /**< temporary continuous variable counter */
   int n;                           /**< temporary (binary) variable counter */
   int m;                           /**< temporary integer variable counter */
   SCIP_Real* contbases;            /**< array to store blockwise the nbuckets-th-root of the maximal reduced costs of continuous variables */
   SCIP_Real* bases;                /**< array to store blockwise the nbuckets-th-root of the maximal reduced costs of (binary) variables */
   SCIP_Real* intbases;             /**< array to store blockwise the nbuckets-th-root of the maximal reduced costs of integer variables */
   SCIP_Real redcostmin;            /**< temporary lower bound for the variable reduced cost to be assigned to the current bucket */
   SCIP_Real redcostmax;            /**< temporary upper bound for the variable reduced cost to be assigned to the current bucket */

   /* when sorting logarithmically by reduced costs, extract the maximal reduced cost per block and its nbuckets-th-root */
   if ( redcostlogsort )
   {
      SCIP_Real tmp_max;

      SCIP_CALL( SCIPallocBufferArray(scip, &contbases, nblocks + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &bases, nblocks + 1) );
      if ( twolevel )
         SCIP_CALL( SCIPallocBufferArray(scip, &intbases, nblocks + 1) );
      
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( bw_ncontnonkernelvars[b] > 0 )
         {
            /* calculate the maximum, safe its root */
            tmp_max = getMaxReal(scip, bw_cont_redcost[b], bw_ncontnonkernelvars[b]);
            if ( nbuckets == 0 || SCIPisLE(scip, tmp_max, 0.0) )
               contbases[b] = tmp_max;
            else
               contbases[b] = (SCIP_Real) pow(tmp_max, 1.0/nbuckets);
         }
         else
            /* safe -inf as the maximum */
            contbases[b] = -SCIPinfinity(scip);

         if ( bw_nnonkernelvars[b] > 0 )
         {
            tmp_max = getMaxReal(scip, bw_redcost[b], bw_nnonkernelvars[b]);
            if ( nbuckets == 0 || SCIPisLE(scip, tmp_max, 0.0) )
               bases[b] = tmp_max;
            else
               bases[b] = (SCIP_Real) pow(tmp_max, 1.0/nbuckets);
         }
         else
            bases[b] = -SCIPinfinity(scip);

         if ( twolevel )
         {
            if ( bw_nintnonkernelvars[b] > 0 )
            {
               tmp_max = getMaxReal(scip, bw_int_redcost[b], bw_nintnonkernelvars[b]);
               if ( nbuckets == 0 || SCIPisLE(scip, tmp_max, 0.0) )
                  intbases[b] = tmp_max;
               else
                  intbases[b] = (SCIP_Real) pow(tmp_max, 1.0/nbuckets);
            }
            else
               intbases[b] = -SCIPinfinity(scip);
         }
            
      }
   }

   /* iteration over all buckets to fill */
   for ( k = 1; k < iters; k++ )
   {
      bucket = &(*bucketlist)->buckets[k];
      
      contbucklength = 0;
      bucklength = 0;
      intbucklength = 0;

      /* calculate the length of the variable arrays for the current bucket typewise */
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( redcostlogsort )
         {
            /* calculation of the variable array length for each type */
            redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(contbases[b], (double)(k - 1));
            redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(contbases[b], (double)k);
            for ( l = 0; l < bw_ncontnonkernelvars[b]; l++ )
               if ( SCIPisGT(scip, bw_cont_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_cont_redcost[b][l], redcostmax) )
                  contbucklength++;
            
            redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(bases[b], (double)(k - 1));
            redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(bases[b], (double)k);
            for ( l = 0; l < bw_nnonkernelvars[b]; l++ )
               if ( SCIPisGT(scip, bw_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_redcost[b][l], redcostmax) )
                  bucklength++;

            if ( twolevel )
            {
               redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(intbases[b], (double)(k - 1));
               redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(intbases[b], (double)k);
               for ( l = 0; l < bw_nintnonkernelvars[b]; l++ )
                  if ( SCIPisGT(scip, bw_int_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_int_redcost[b][l], redcostmax) )
                     intbucklength++;
            }
         }
         else
         {
            /* initialize the start and end indices to split the non-kernel variables typewise and calculate the bucket length */
            fromcontvars = SCIPceil(scip, (bw_ncontnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
            tocontvars = SCIPceil(scip, (bw_ncontnonkernelvars[b] / (SCIP_Real)nbuckets) * k );
            fromvars = SCIPceil(scip, (bw_nnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
            tovars = SCIPceil(scip, (bw_nnonkernelvars[b] / (SCIP_Real)nbuckets) * k );
         

            contbucklength += tocontvars - fromcontvars;
            bucklength += tovars - fromvars;

            if ( twolevel )
            {
               fromintvars = SCIPceil(scip, (bw_nintnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
               tointvars = SCIPceil(scip, (bw_nintnonkernelvars[b] / (SCIP_Real)nbuckets) * k );

               intbucklength += tointvars - fromintvars;
            }
         }

      }

      /* initialize all buffer arrays for the continuous, binary/integer and (if necessary) integer bucket variables */
      SCIP_CALL( SCIPallocBufferArray(scip, &(bucket->contbucketvars), contbucklength) );
      bucket->ncontbucketvars = contbucklength;
      SCIP_CALL( SCIPallocBufferArray(scip, &(bucket->bucketvars), bucklength) );
      bucket->nbucketvars = bucklength;
      if ( twolevel )
      {
         SCIP_CALL( SCIPallocBufferArray(scip, &(bucket->intbucketvars), intbucklength) );
         bucket->nintbucketvars = intbucklength;
      }

      /* fill the initialized arrays with the respective variables */
      j = 0;
      n = 0;
      m = 0;
      for ( b = 0; b < nblocks + 1; b++ )
      {
         if ( redcostlogsort )
         {
            /* assignment of the variables to the respective bucket variable arrays for each type */
            redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(contbases[b], (double)(k - 1));
            redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(contbases[b], (double)k);

            for ( l = 0; l < bw_ncontnonkernelvars[b]; l++ )
               if ( SCIPisGT(scip, bw_cont_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_cont_redcost[b][l], redcostmax) )
                  bucket->contbucketvars[j++] = bw_contnonkernelvars[b][l];
            
            redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(bases[b], (double)(k - 1));
            redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(bases[b], (double)k);
            for ( l = 0; l < bw_nnonkernelvars[b]; l++ )
               if ( SCIPisGT(scip, bw_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_redcost[b][l], redcostmax) )
                  bucket->bucketvars[n++] = bw_nonkernelvars[b][l];

            if ( twolevel )
            {
               redcostmin = k == 1 ? -SCIPinfinity(scip) : pow(intbases[b], (double)(k - 1));
               redcostmax = k == nbuckets ? SCIPinfinity(scip) : pow(intbases[b], (double)k);
               for ( l = 0; l < bw_nintnonkernelvars[b]; l++ )
                  if ( SCIPisGT(scip, bw_int_redcost[b][l], redcostmin) && SCIPisLE(scip, bw_int_redcost[b][l], redcostmax) )
                     bucket->intbucketvars[m++] = bw_intnonkernelvars[b][l];
            }
         }
         else
         {
            /* calculate again the necessary start and end indices to split the non-kernel variables typewise */
            fromcontvars = SCIPceil(scip, (bw_ncontnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
            tocontvars = SCIPceil(scip, (bw_ncontnonkernelvars[b] / (SCIP_Real)nbuckets) * k );
            fromvars = SCIPceil(scip, (bw_nnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
            tovars = SCIPceil(scip, (bw_nnonkernelvars[b] / (SCIP_Real)nbuckets) * k );

            /* fill the variable arrays */
            for ( l = 0; l < tocontvars - fromcontvars; l++ )
               bucket->contbucketvars[j++] = bw_contnonkernelvars[b][fromcontvars + l];
            for ( l = 0; l < tovars - fromvars; l++ )
               bucket->bucketvars[n++] = bw_nonkernelvars[b][fromvars + l];

            /* apply the upper for the integer variables only if necessary */
            if ( twolevel )
            {
               fromintvars = SCIPceil(scip, (bw_nintnonkernelvars[b] / (SCIP_Real)nbuckets) * (k - 1) );
               tointvars = SCIPceil(scip, (bw_nintnonkernelvars[b] / (SCIP_Real)nbuckets) * k );

               for ( l = 0; l < tointvars - fromintvars; l++ )
                  bucket->intbucketvars[m++] = bw_intnonkernelvars[b][fromintvars + l];
            }
         }
      }

      assert( j == contbucklength );
      assert( n == bucklength );
      if ( twolevel )
         assert( m == intbucklength );
   }

   if ( redcostlogsort )
   {
      SCIPfreeBufferArray(scip, &contbases);
      SCIPfreeBufferArray(scip, &bases);
      if ( twolevel )
         SCIPfreeBufferArray(scip, &intbases);

   }

   return SCIP_OKAY;
}

static
SCIP_RETCODE freeBucketArrays(
   SCIP*                scip,                      /**< main SCIP data structure */
   BUCKET*              bucket,                    /**< bucket to free the arrays from */
   SCIP_Bool            twolevel                   /**< usage of one or two level structure */
)
{
   if ( bucket->contbucketvars != NULL )
      SCIPfreeBufferArray(scip, &bucket->contbucketvars);
   if ( bucket->bucketvars != NULL )
      SCIPfreeBufferArray(scip, &bucket->bucketvars);
   if ( twolevel && bucket->intbucketvars != NULL )
      SCIPfreeBufferArray(scip, &bucket->intbucketvars);

   return SCIP_OKAY;
}

/** initialize a bucket */
static
SCIP_RETCODE initBucket(
   BUCKETLIST*          bucketlist                 /**< bucketlist structure where the bucket belongs to */
   )
{
   BUCKET* bucket;

   assert(bucketlist != NULL);
   assert(bucketlist->scip != NULL);

   bucket = &bucketlist->buckets[bucketlist->nbuckets];

   bucket->bucketlist = bucketlist;
   bucket->subscip = NULL;
   bucket->contbucketvars = NULL;
   bucket->bucketvars = NULL;
   bucket->intbucketvars = NULL;
   bucket->ncontbucketvars = 0;
   bucket->nbucketvars = 0;
   bucket->nintbucketvars = 0;
   bucket->number = bucketlist->nbuckets;
   bucket->twolevel = FALSE;
   bucket->scip2sub = NULL;
   bucket->sub2scip = NULL;

   ++bucketlist->nbuckets;

   return SCIP_OKAY;
}

/** free bucket structure */
static
SCIP_RETCODE freeBucket(
   SCIP*                   scip,                  /**< SCIP data structure */
   BUCKET*                 bucket                 /**< bucket structure to free */
   )
{
   SCIP_VAR**  subvars;
   int         nsubvars;

   assert(scip != NULL);
   assert(bucket != NULL);

   assert(bucket->subscip != NULL);

   SCIP_CALL( SCIPgetOrigVarsData(bucket->subscip, &subvars, &nsubvars, NULL, NULL, NULL, NULL) );

   /* free variable mappings subscip -> scip and scip -> subscip */
   if ( bucket->scip2sub != NULL )
      SCIPfreeBlockMemoryArrayNull(scip, &bucket->scip2sub, nsubvars);
   if ( bucket->sub2scip != NULL )
      SCIPfreeBlockMemoryArrayNull(scip, &bucket->sub2scip, SCIPgetNVars(scip));

   SCIP_CALL( SCIPfree(&bucket->subscip) );
   bucket->subscip = NULL;

   return SCIP_OKAY;
}

/** initialize the bucketlist */
static
SCIP_RETCODE initBucketlist(
   SCIP*                   scip,                
   BUCKETLIST**            bucketlist,             /**< pointer to bucketlist */
   int                     nbuckets                /**< number of buckets */
   )
{
   char name[SCIP_MAXSTRLEN];

   assert(scip != NULL);
   assert(bucketlist != NULL);

   SCIP_CALL( SCIPallocBlockMemory(scip, bucketlist) );
   assert(*bucketlist != NULL);

   (void)SCIPsnprintf(name, SCIP_MAXSTRLEN, "%s", SCIPgetProbName(scip));

   SCIPdebugMessage("initialized problem %s\n", name);

   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &(*bucketlist)->buckets, nbuckets) );

   (*bucketlist)->scip = scip;
   (*bucketlist)->nbuckets = 0;

   return SCIP_OKAY;
}

/** free bucketlist structure */
static
SCIP_RETCODE freeBucketlist(
   BUCKETLIST**               bucketlist,             /** < pointer to bucketlist to free */
   int                        nbuckets                /** < number of buckets to free */
   )
{
   SCIP* scip;

   assert(bucketlist != NULL);
   assert(*bucketlist != NULL);

   scip = (*bucketlist)->scip;
   assert(scip != NULL);

   /** free all buckets in the bucketlist */
   if ( (*bucketlist)->buckets != NULL )
      SCIPfreeBlockMemoryArray(scip, &(*bucketlist)->buckets, nbuckets);

   /* free BUCKETLIST structure and set the pointer to null*/
   if ( *bucketlist != NULL )
      SCIPfreeBlockMemory(scip, bucketlist);
   *bucketlist = NULL;

   return SCIP_OKAY;
}

/** creates the subscip for each bucket */
static 
SCIP_RETCODE bucketCreateSubscip(
   BUCKET*                 bucket,              /**< the bucket to create the subscip for */
   SCIP_Bool*              success              /**< pointer to store if the creation process was successfull */
   )
{
   BUCKETLIST* bucketlist;
   SCIP* scip;
   SCIP_VAR**  vars;
   SCIP_VAR**  subvars;
   SCIP_VAR*   var;
   SCIP_VAR*   subvar;
   SCIP_HASHMAP* varsmap;
   SCIP_HASHMAP* consmap;
   char probname[SCIP_MAXSTRLEN];
   int i;
   int nvars;
   int nsubvars;

   SCIP_CONS** conss;
   SCIP_CONS*  newcons;

   assert( bucket != NULL );
   assert( success != NULL );

   bucketlist = bucket->bucketlist;
   assert( bucketlist != NULL );

   scip = bucketlist->scip;
   assert( scip != NULL );

   /* start new creation process */
   SCIP_CALL( SCIPgetVarsData(scip, &vars, &nvars, NULL, NULL, NULL, NULL) );

   /* initializing the subproblem */
   SCIP_CALL( SCIPcreate(&bucket->subscip) );

   /* create variable hash mapping scip -> subscip */
   SCIP_CALL( SCIPhashmapCreate(&varsmap, SCIPblkmem(scip), nvars) );

   /* create subscip copy of scip */
   /* copy interesting plugins */
   (*success) = TRUE;

   /* from before: include default and copy limits */
#ifdef SCIP_DEBUG /* we print statistics later, so we need to copy statistics tables */
   SCIP_CALL( SCIPcopyPlugins(scip, bucket->subscip, TRUE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE,
         TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE, TRUE, TRUE, success) );
#else
   SCIP_CALL( SCIPcopyPlugins(scip, bucket->subscip, TRUE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE,
         TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, FALSE, TRUE, FALSE, FALSE, TRUE, success) );
#endif
   SCIP_CALL( SCIPcopyLimits(scip, bucket->subscip) );

   /* copy parameter settings */
   SCIP_CALL( SCIPcopyParamSettings(scip, bucket->subscip) );

   /* create problem in subscip */
   /* get name of the original problem and add "dksbucket" + [bucketnumber] */
   (void) SCIPsnprintf(probname, SCIP_MAXSTRLEN, "%s_dksbucket%d", SCIPgetProbName(scip), bucket->number);

   /* from before: avoid recursive calls */
   SCIP_CALL( SCIPsetSubscipsOff(bucket->subscip, TRUE) );

   /* copy all variables */
   SCIP_CALL( SCIPcopyProb(scip, bucket->subscip, varsmap, NULL, FALSE, probname) ); //changed consmap to NULL
   SCIP_CALL( SCIPcopyVars(scip, bucket->subscip, varsmap, NULL, NULL, NULL, 0, TRUE) );

   /* copy as many constraints as possible */
   SCIP_CALL( SCIPhashmapCreate(&consmap, SCIPblkmem(scip), SCIPgetNConss(scip)) );

   conss = SCIPgetConss(scip);
   
   for( i = 0; i < SCIPgetNConss(scip); ++i )
   {
      assert(!SCIPconsIsModifiable(conss[i]));
      /* copy the constraint */
      SCIP_CALL( SCIPgetConsCopy(scip, bucket->subscip, conss[i], &newcons, SCIPconsGetHdlr(conss[i]), varsmap, consmap, NULL,
                                SCIPconsIsInitial(conss[i]), SCIPconsIsSeparated(conss[i]), SCIPconsIsEnforced(conss[i]),
                                SCIPconsIsChecked(conss[i]), SCIPconsIsPropagated(conss[i]), FALSE, FALSE,
                                SCIPconsIsDynamic(conss[i]), SCIPconsIsRemovable(conss[i]), FALSE, FALSE, success) );

      /* abort if constraint was not successfully copied */
      if( !(*success) )
      {
         *success = FALSE;
         if ( newcons != NULL )
            SCIPreleaseCons(bucket->subscip, &newcons);
         SCIPhashmapFree(&varsmap);
         SCIPhashmapFree(&consmap);
         return SCIP_OKAY;
      }

      if ( newcons != NULL )
      {
         SCIP_CALL( SCIPaddCons(bucket->subscip, newcons) );
         SCIP_CALL( SCIPreleaseCons(bucket->subscip, &newcons) );
      }
   }
   
   SCIPhashmapFree(&consmap);
   if ( !(*success) )
   {
      SCIPverbMessage(scip, SCIP_VERBLEVEL_FULL, NULL, "In heur_dks: faild to copy some constraints to the subscip, continue anyway\n");
      SCIPdebugMsg(scip, "In heur_dks: faild to copy some constraints to subscip, continue anyway\n");
   } 
   
   /* create arrays translating scip transformed vars to subscip original vars, and vice versa
    * capture variables in scip and subscip
    * catch global bound change events
    */

   SCIP_CALL( SCIPgetVarsData(bucket->subscip, &subvars, &nsubvars, NULL, NULL, NULL, NULL) );
   
   SCIP_CALL( SCIPallocClearBlockMemoryArray(scip, &bucket->sub2scip, nsubvars) );
   SCIP_CALL( SCIPallocClearBlockMemoryArray(scip, &bucket->scip2sub, nvars) );

   /* iteration over varsmap to get the original and corresponding subscip variables*/
   for ( i = 0; i < SCIPhashmapGetNEntries(varsmap); i++ )
   {
      SCIP_HASHMAPENTRY* entry;
      entry = SCIPhashmapGetEntry(varsmap, i);
      if ( entry != NULL )
      {
         var = (SCIP_VAR*) SCIPhashmapEntryGetOrigin(entry);
         subvar = (SCIP_VAR*) SCIPhashmapEntryGetImage(entry);
         assert( subvar != NULL);
         assert( SCIPvarGetProbindex(subvar) >= 0 );
         assert( SCIPvarGetProbindex(subvar) <= nsubvars );

         if ( SCIPvarIsActive(var) )
         {
            assert( SCIPvarGetProbindex(var) <= nvars );
            assert(bucket->scip2sub[SCIPvarGetProbindex(var)] == NULL);
            bucket->scip2sub[SCIPvarGetProbindex(var)] = subvar;
         }
         assert(bucket->sub2scip[SCIPvarGetProbindex(subvar)] == NULL);
         bucket->sub2scip[SCIPvarGetProbindex(subvar)] = var;
      }
   }

#ifdef SCIP_DEBUG
   for ( i = 0; i < nsubvars; i++ )
   {
      subvar = SCIPgetVars(bucket->subscip)[i];
      assert(SCIPvarGetProbindex(subvar) == i);
      var = bucket->sub2scip[i];

      //SCIP_CALL( SCIPcaptureVar(bucket->subscip, subvar) );

      assert(SCIPisFeasEQ(scip, SCIPvarGetLbGlobal(var), SCIPvarGetLbGlobal(subvar)) );
      assert(SCIPisFeasEQ(scip, SCIPvarGetUbGlobal(var), SCIPvarGetUbGlobal(subvar)) );
   }
#endif

   SCIPhashmapFree(&varsmap);

   /* avoid recursive calls */
   SCIP_CALL( SCIPsetSubscipsOff(bucket->subscip, TRUE) );

   /* do not abort subproblem on CTRL-C */
   SCIP_CALL( SCIPsetBoolParam(bucket->subscip, "misc/catchctrlc", FALSE) );

#ifdef SCIP_DEBUG
   /* for debugging, enable full output */
   SCIP_CALL( SCIPsetIntParam(*subscip, "display/verblevel", 5) );
   SCIP_CALL( SCIPsetIntParam(*subscip, "display/freq", 100000000) );
#else
   /* disable statistic timing inside sub SCIP and output to console */
   SCIP_CALL( SCIPsetIntParam(bucket->subscip, "display/verblevel", 0) );
   SCIP_CALL( SCIPsetBoolParam(bucket->subscip, "timing/statistictiming", FALSE) );
#endif
   
   SCIPdebugMsg(scip, "created subscip of bucket %d\n", bucket->number);

   return SCIP_OKAY;
}

static 
SCIP_RETCODE createBucketlistAndBuckets(
   SCIP*                   scip,                   /**< SCIP data structure */
   SCIP_CONS**             bucketconss,            /**< constraints of one bucket as normal constraints */
   int                     nconss,                 /**< amount of constraints */
   int                     nbuckets,               /**< amount of buckets (without kernel only) */
   BUCKETLIST**            bucketlist,             /**< pointer to store bucketlist structure */
   SCIP_Bool               usetransprob,           /**< is the transformed problem used */
   SCIP_Bool*              success
   )
{
   BUCKET* bucket;
   int b;
   bucket = NULL;
   *success = TRUE;

   /* init bucketlist data structure with nbucket + 1 because the initial bucket with kernel vars is included */
   SCIP_CALL( initBucketlist(scip, bucketlist, nbuckets + 1) );
   assert( (*bucketlist)->buckets != NULL );

   /* loop over all buckets and the initial "kernel"bucket */
   for ( b = 0; b < nbuckets + 1; b++ )
   {
      SCIP_CALL( initBucket(*bucketlist) );
      assert((*bucketlist)->nbuckets == b + 1);

      bucket = &(*bucketlist)->buckets[b];
      
      /* build subscip for bucket */
      SCIP_CALL( bucketCreateSubscip(bucket, success) ); 
      if ( !(*success) )
         return SCIP_OKAY;
   }
   assert(nbuckets + 1 == (*bucketlist)->nbuckets);

   return SCIP_OKAY;
}

static 
SCIP_RETCODE searchKernelAndBucket(
   BUCKET*              bucket,                 /**< bucket to be solved next */
   SCIP_VAR**           contkernelvars,         /**< continuous variables in the latest kernel */
   int                  ncontkernelvars,        /**< amount of continuous variables in the latest kernel */
   SCIP_VAR**           kernelvars,             /**< variables in the kernel */
   int                  nkernelvars,            /**< amount of variables in the kernel */
   SCIP_VAR**           intkernelvars,          /**< variables in the integer kernel, if 2-level buckets are present */
   int                  nintkernelvars,         /**< amount of variables in the integer kernel */
   SCIP_VAR*            var,                    /**< variable to search for in the kernel/buckets */
   SCIP_Bool*           found                   /**< is the variable present in the current bucket or the kernel? */
   )
{
   int j;

   *found = FALSE;

   /* search in the current continuous kernel for the given variable */
   if ( SCIPvarGetType(var) == SCIP_VARTYPE_CONTINUOUS || SCIPvarGetType(var) == SCIP_VARTYPE_IMPLINT )
   {
      for ( j = 0; j < ncontkernelvars; j++ )
      {    
         if ( contkernelvars[j] != NULL && var == contkernelvars[j] )
         {
            *found = TRUE;
            return SCIP_OKAY;
         }
      }
      
      /* search for the current variable in the continuous bucket variables */
      for ( j = 0; j < bucket->ncontbucketvars; j++ )
      {
         if ( var == bucket->contbucketvars[j] )
         {
            *found = TRUE;
            return SCIP_OKAY;
         }
      }
   }


   /* search in the current (binary) kernel for the variable */
   for (j = 0; j < nkernelvars; j++ )
   {
      if ( kernelvars[j] != NULL && var == kernelvars[j] )
      {
         *found = TRUE;
         return SCIP_OKAY;
      }
   }

   /* if 2-level buckets are used, also search for the current variable in the integer kernel */
   for ( j = 0; j < nintkernelvars; j++ )
   {
      if ( intkernelvars[j] != NULL && var == intkernelvars[j] )
      {
         *found = TRUE;
         return SCIP_OKAY;
      }
   }

   /* search for the current variable in the (binary) bucket variables */
   for ( j = 0; j < bucket->nbucketvars; j++ )
   {
      if ( var == bucket->bucketvars[j] )
      {
         *found = TRUE;
         return SCIP_OKAY;
      }
   }

   /* if 2-level buckets are used, also search for the current variable in the integer bucket variables */
   for ( j = 0; j < bucket->nintbucketvars; j++ )
   {
      if ( var == bucket->intbucketvars[j] )
      {
         *found = TRUE;
         return SCIP_OKAY;
      }
   }

   return SCIP_OKAY;
}

static
SCIP_RETCODE adjustKernelVars(
   SCIP*                scip,                   /**< current scip */
   BUCKET*              bucket,                 /**< bucket that was solved last */
   SCIP_VAR***          contkernelvars,         /**< cont. kernelvars to adjust */
   int*                 ncontkernelvars,        /**< amount of cont. kernelvars */
   int                  maxcontkernelsize,      /**< maximal size of the continuous kernel */
   SCIP_VAR***          kernelvars,             /**< kernelvars to adjust */
   int*                 nkernelvars,            /**< amount of kernelvars */
   int                  maxkernelsize,          /**< maximal size of the kernel */
   SCIP_VAR***          intkernelvars,          /**< integer kernelvars to adjust */
   int*                 nintkernelvars,         /**< amount of integer kernelvars */
   int                  maxintkernelsize,       /**< maximal size of the integer kernel */
   SCIP_Bool            decreasekernel,         /**< boolean to decrease the kernel to non-lower-bound variables or not */
   SCIP_Bool            twolevel                /**< is a twolevel structure necessary */
   )
{
   /* initialization of the variables */       
   SCIP_VAR** contkvars;            /*< temporary storage for the continuous kernel variables */
   SCIP_VAR** kvars;                /*< temporary storage for the (binary/integer) kernel variables */
   SCIP_VAR** intkvars;             /*< temporary storage for the integer kernel variables */
   SCIP_VAR *var;                   /*< temporary variable */
   SCIP_Real val;                   /*< variable value in solution */
   SCIP_Real lb;                    /*< variable lower bound */
   SCIP_SOL* solution;              /*< solution of the current bucket */
   int nnewcontkernelvars;          /*< number of new continuous kernel variables */
   int nnewkernelvars;              /*< number of new (binary/integer) kernel variables */
   int nnewintkernelvars;           /*< number of new integer kernel variables */
   int n;                           /*< temporary variable counter */

   /* definition of old kernel arrays to update the actual ones live */
   contkvars = *contkernelvars;
   kvars = *kernelvars;
   intkvars = *intkernelvars;

   /* get the solution of the current solved subscip */
   solution = SCIPgetBestSol(bucket->subscip);

   /*** deletion of variables from the kernel ***/
   /* continuous kernelvariables with value equal to zero or their lb get deleted from the kernel */
   nnewcontkernelvars = 0;
   for ( n = 0; n < *ncontkernelvars; n++ )
   {
      /* skip null values */
      if ( contkvars[n] == NULL )
         continue;

      /* get the value of the current variable and its lower bound  */
      if ( SCIPvarIsActive(contkvars[n]) )
      {
         assert( SCIPvarGetProbindex(contkvars[n]) <= SCIPgetNVars(scip) );
         var = bucket->scip2sub[SCIPvarGetProbindex(contkvars[n])];
         if ( var != NULL )
            val = SCIPgetSolVal(bucket->subscip, solution, var);
         else
            continue;
      }
      else
         continue;

      lb = SCIPvarGetLbGlobal(contkvars[n]);

      /* if deviating from lb and zero, re-add into current kernel vars */
      if ( (!SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb)) || !decreasekernel )
         (*contkernelvars)[nnewcontkernelvars++] = contkvars[n];
      /* otherwise, delete it */
      else
         contkvars[n] = NULL;
   }
   
   /* dependent on one- or two-level structe, check the solution value of the binary/integer value to be unequal to 0 and/or its lb */
   nnewkernelvars = 0;
   for ( n = 0; n < *nkernelvars; n++ )
   {
      /* if there is a null value in the current kernelvars, skip it */
      if ( kvars[n] == NULL )
         continue;
      
      /* get the value of the current kernel variable in the solution and its lower bound */
      if ( SCIPvarIsActive(kvars[n]) )
      {
         assert( SCIPvarGetProbindex(kvars[n]) <= SCIPgetNVars(scip) );
         var = bucket->scip2sub[SCIPvarGetProbindex(kvars[n])];
         if ( var != NULL )
            val = SCIPgetSolVal(bucket->subscip, solution, var);
         else
            continue;
      }
      else
         continue;

      lb = SCIPvarGetLbGlobal(kvars[n]);

      /* if two-level structure is required, the binary case occurs and only deviation to 0 has to be checked */
      if ( (twolevel && !SCIPisEQ(scip, val, 0.0)) || !decreasekernel )
         (*kernelvars)[nnewkernelvars++] = kvars[n];
      /* if one-level case, the variable has to deviate from 0 and its lb */
      else if ( (!twolevel && !SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb)) || !decreasekernel )
         (*kernelvars)[nnewkernelvars++] = kvars[n];
      /* otherwise delete the variable from its current position in the kernel */
      else
         kvars[n] = NULL;
   }

   /* if necessary check the relevance of pure integer variables in the current kernel */
   if ( twolevel )
   {
      nnewintkernelvars = 0;

      for ( n = 0; n < *nintkernelvars; n++ )
      {
         /* skip null values */
         if ( intkvars[n] == NULL )
            continue;

         /* get the value of the current variable in the solution and its lower bound */
         if ( SCIPvarIsActive(intkvars[n]) )
         {
            assert( SCIPvarGetProbindex(intkvars[n]) <= SCIPgetNVars(scip) );
            var = bucket->scip2sub[SCIPvarGetProbindex(intkvars[n])];
            if ( var != NULL )
               val = SCIPgetSolVal(bucket->subscip, solution, var);
            else
               continue;
         }
         else
            continue;

         lb = SCIPvarGetLbGlobal(intkvars[n]);

         /* if variable value is unequal to 0 and its lower bound, it is re-added into the kernel */
         if ( (!SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb)) || !decreasekernel )
            (*intkernelvars)[nnewintkernelvars++] = intkvars[n];
         else
            intkvars[n] = NULL;
      }
   }

   /*** addition of new variables from the bucket to the kernel ***/

   /* add continuous bucket variables with suitable values to the kernel */
   for ( n = 0; n < bucket->ncontbucketvars; n++ )
   {
      if ( bucket->contbucketvars[n] == NULL )
         continue;

      if ( SCIPvarIsActive(bucket->contbucketvars[n]) )
      {
         assert( SCIPvarGetProbindex(bucket->contbucketvars[n]) <= SCIPgetNVars(scip) );
         var = bucket->scip2sub[SCIPvarGetProbindex(bucket->contbucketvars[n])];
         if ( var != NULL )
            val = SCIPgetSolVal(bucket->subscip, solution, var);
         else
            continue;
      }
      else
         continue;
      
      lb = SCIPvarGetLbGlobal(bucket->contbucketvars[n]);

      /* if the solution value of the bucket variable deviates in epsilon from zero and its lb, add it to the cont. kernel variables */
      if ( !SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb) )
      {
         if ( SCIPisGT(scip, nnewcontkernelvars, maxcontkernelsize) )
            break;
         else
            (*contkernelvars)[nnewcontkernelvars++] = bucket->contbucketvars[n];
      }
   }

   /* the size of the continuous kernel might be different -> change it */
   *ncontkernelvars = nnewcontkernelvars;

   /* add binary/integer bucketvariables with suitable values to the kernel */
   for ( n = 0; n < bucket->nbucketvars; n++ )
   {
      if ( bucket->bucketvars[n] == NULL )
         continue;

      if ( SCIPvarIsActive(bucket->bucketvars[n]) )
      {
         assert( SCIPvarGetProbindex(bucket->bucketvars[n]) <= SCIPgetNVars(scip) );
         var = bucket->scip2sub[SCIPvarGetProbindex(bucket->bucketvars[n])];
         if ( var != NULL )
            val = SCIPgetSolVal(bucket->subscip, solution, var);
         else
            continue;
      }
      else
         continue;

      lb = SCIPvarGetLbGlobal(bucket->bucketvars[n]);

      /* if bucket variable is not equal to zero and not equal to its lower bound (in epsilon), try adding it to the kernel variables */
      if ( twolevel && !SCIPisEQ(scip, val, 0.0) )
      {
         if ( SCIPisGT(scip, nnewkernelvars, maxkernelsize) )
            break;
         else
            (*kernelvars)[nnewkernelvars++] = bucket->bucketvars[n];
      }
      /* if one-level case, the variable has to deviate from 0 and its lb */
      else if ( !twolevel && !SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb) )
      {
         if ( SCIPisGT(scip, nnewkernelvars, maxkernelsize) )
            break; //to do: if kernel is "full", find a suitable variable to delete or extend kernel
         else
            (*kernelvars)[nnewkernelvars++] = bucket->bucketvars[n];
      }
   }

   /* the size of the kernel might be different, so change it */
   *nkernelvars = nnewkernelvars;

   /* if necessary, add integer bucket variables with suitable values to the integer kernel */
   if ( twolevel )
   {
      for ( n = 0; n < bucket->nintbucketvars; n++ )
      {
         if ( bucket->intbucketvars[n] == NULL )
            continue;

         if ( SCIPvarIsActive(bucket->intbucketvars[n]) )
         {
            assert( SCIPvarGetProbindex(bucket->intbucketvars[n]) <= SCIPgetNVars(scip) );
            var = bucket->scip2sub[SCIPvarGetProbindex(bucket->intbucketvars[n])];
            if ( var != NULL )
               val = SCIPgetSolVal(bucket->subscip, solution, var);
            else
               continue;
         }
         else
            continue;

         lb = SCIPvarGetLbGlobal(bucket->intbucketvars[n]);

         /* if the bucket variable's value is unequal to zero and its lb, try adding it to the integer kernel */
         if ( !SCIPisEQ(scip, val, 0.0) && !SCIPisEQ(scip, val, lb) )
         {
            if ( SCIPisGT(scip, nnewintkernelvars, maxintkernelsize) )
               break;
            else
               (*intkernelvars)[nnewintkernelvars++] = bucket->intbucketvars[n];
         }
      }
      /* if the size of the kernel is different, change it */
      *nintkernelvars = nnewintkernelvars; 
   }
   
   return SCIP_OKAY;
}
static
SCIP_RETCODE addUseConstraint(
   BUCKET*              bucket                  /**< current bucket to look at */
   )
{
   SCIP_CONS* constraint;
   SCIP_VAR** subvars;
   SCIP_VAR *var;
   char consname[SCIP_MAXSTRLEN];
   SCIP_Real* coeffs;
   SCIP_Real rhs;
   SCIP_Real lb;
   int n;
   int k;

   /* add an array to store the binary and integer variables of the constraint to add separatly */
   SCIP_CALL( SCIPallocBufferArray(bucket->subscip, &subvars, bucket->nbucketvars + bucket->nintbucketvars) );

   /* add an array for the coefficients of the binary and integer variables in the constraint */
   SCIP_CALL( SCIPallocBufferArray(bucket->subscip, &coeffs, bucket->nbucketvars + bucket->nintbucketvars) );

   /* for all (binary/integer) variables in the current bucket add the variables to the subvars and add coeff -1 */
   k = 0;
   rhs = -1.0;
   for ( n = 0; n < bucket->nbucketvars ; n++ )
   {
      if ( bucket->bucketvars[n] == NULL )
         continue;
      if ( SCIPvarIsActive(bucket->bucketvars[n]) )
         var = bucket->scip2sub[SCIPvarGetProbindex(bucket->bucketvars[n])];
      else 
         var = NULL;

      if ( var != NULL )
      {
         subvars[k] = var;
         coeffs[k++] = -1.0; /* constraint: (sum of x_i >= 1)   iff   (-1 * sum of x_1 <= -1) */

         /* if the variable has a positive lower bound, it is substracted from the rhs of the constraint */
         lb = SCIPvarGetLbGlobal( var );
         if ( SCIPvarGetType(var) == SCIP_VARTYPE_BINARY)
            rhs -= MAX(0.0, lb);
         else
            rhs -= lb;
      }
   }

   for ( n = 0; n < bucket->nintbucketvars; n++ )
   {
      if ( bucket->intbucketvars[n] == NULL )
         continue;
      if ( SCIPvarIsActive(bucket->intbucketvars[n]) )
         var = bucket->scip2sub[SCIPvarGetProbindex(bucket->intbucketvars[n])];
      else 
         var = NULL;

      if ( var != NULL )
      {
         subvars[k] = var;
         coeffs[k++] = -1.0;

         /* if the integer variable has a positive lower bound, it is added to the rhs of the new constraint */
         lb = SCIPvarGetLbGlobal( var );
         rhs -= lb;
      }
   }

   (void)SCIPsnprintf(consname, SCIP_MAXSTRLEN, "useconstraint_bucket_%d", bucket->number);

   /* add the constraint: (-1 * sum of bucket variables <= - sum of lbs - 1) ensuring that at least one of these variables is not zero */
   SCIP_CALL( SCIPcreateConsBasicLinear(bucket->subscip, &constraint, consname, 
                                       k, subvars, coeffs,
                                       -SCIPinfinity(bucket->subscip), rhs) );
   SCIP_CALL( SCIPaddCons(bucket->subscip, constraint) );
   SCIP_CALL( SCIPreleaseCons(bucket->subscip, &constraint) );

   /* free the arrays */
   if ( subvars != NULL)
      SCIPfreeBufferArray(bucket->subscip, &subvars);
   subvars = NULL;
   
   if ( coeffs != NULL)
      SCIPfreeBufferArray(bucket->subscip, &coeffs);
   coeffs = NULL;

   return SCIP_OKAY;
}

/*
 * Callback methods of primal heuristic
 */

/** copy method for primal heuristic plugins (called when SCIP copies plugins) */
static
SCIP_DECL_HEURCOPY(heurCopyDKS)
{  /*lint --e{715}*/
   assert(scip != NULL);
   assert(heur != NULL);
   assert(strcmp(SCIPheurGetName(heur), HEUR_NAME) == 0 );

   /* call inclusion method of primal heuristic */
   SCIP_CALL( SCIPincludeHeurDKS(scip) );

   return SCIP_OKAY;
}


/** destructor of primal heuristic to free user data (called when SCIP is exiting) */
static
SCIP_DECL_HEURFREE(heurFreeDKS)
{  /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;

   heurdata = SCIPheurGetData(heur);
   SCIPheurSetData(heur, NULL);

   assert(heurdata != NULL);

   SCIPfreeBlockMemory(scip, &heurdata);
   
   return SCIP_OKAY;
}

/** execution method of primal heuristic */
static
SCIP_DECL_HEUREXEC(heurExecDKS)
{  /*lint --e{715}*/
   SCIP_HEURDATA* heurdata;
   SCIP_DECOMP** alldecomps;
   SCIP_DECOMP* decomp;
   SCIP_HASHMAP* lbvarmap;       /*< variable map connection transformed variables to their original lower bound */
   SCIP_VAR*** bw_contkernelvars;
   SCIP_VAR*** bw_contnonkernelvars;
   SCIP_VAR*** bw_kernelvars;
   SCIP_VAR*** bw_nonkernelvars;
   SCIP_VAR*** bw_intkernelvars;
   SCIP_VAR*** bw_intnonkernelvars;
   SCIP_VAR** vars;
   SCIP_VAR** contkernelvars;
   SCIP_VAR** contnonkernelvars;
   SCIP_VAR** kernelvars;        /*< just the binary kernel variables if problem includes binary AND integer variables */
   SCIP_VAR** nonkernelvars;     /*< just the binary non kernel variables if problem includes binary AND integer variables */
   SCIP_VAR** intkernelvars;     /*< used if problem includes binary AND integer variables */
   SCIP_VAR** intnonkernelvars;  /*< used if problem includes binary AND integer variables */
   SCIP_VAR** binintvars;
   SCIP_CONS** conss;
   SCIP_CONS** bucketconss;
   SCIP_Real gapfactor;
   SCIP_Real maxcontkernelsize;
   SCIP_Real maxcontnonkernelsize;
   SCIP_Real maxkernelsize;
   SCIP_Real maxnonkernelsize;
   SCIP_Real maxintkernelsize;   /*< used if problem includes binary AND integer variables */
   SCIP_Real maxintnonkernelsize;
   SCIP_Real memory;
   SCIP_Real bestlocval;
   SCIP_Real subtimelim;
   SCIP_Real mipgap;
   SCIP_Real maxtime;
   SCIP_Real linkscore;
   SCIP_Real** bw_cont_redcost;
   SCIP_Real** bw_redcost;
   SCIP_Real** bw_int_redcost;
   SCIP_STATUS status;
   SCIP_Bool success;
   SCIP_Bool twolevel;           /*< clarifying if two level buckets are used. Depends on count of initial kernel vars */
   SCIP_Bool usebestsol;
   SCIP_SOL* bestcurrsol;
   BUCKETLIST* bucketlist;
   BUCKET* bucket;
   int* varlabels;
   int* conslabels;
   int* block2index;
   int* blocklabels;
   int* bw_ncontkernelvars;
   int* bw_ncontnonkernelvars;
   int* bw_nkernelvars;
   int* bw_nnonkernelvars;
   int* bw_nintkernelvars;
   int* bw_nintnonkernelvars;
   int* bw_contkernelcount;
   int* bw_contnonkernelcount;
   int* bw_kernelcount;
   int* bw_nonkernelcount;
   int* bw_intkernelcount;
   int* bw_intnonkernelcount;
   int gapcall;
   int blklbl_offset;
   int nblocks;
   int ndecomps;
   int nvars;
   int ncontkernelvars;
   int ncontnonkernelvars;
   int nkernelvars;
   int nnonkernelvars;
   int nintkernelvars;
   int nintnonkernelvars;
   int ncontvars;
   int nbinvars;
   int nintvars;
   int nbinintvars;
   int nbuckets;
   int nconss;
   int nbestbucket;
   int nusedratios;
   int nblocklabels;
   int iters;
   int b;
   int i;
   int j;
   int k;
   int l;
   int m;
   int n;

   assert(scip != NULL);
   assert(heur != NULL);
   assert(result != NULL);

   heurdata = SCIPheurGetData(heur);
   assert(heurdata != NULL);

   *result = SCIP_DIDNOTRUN;

   decomp = NULL;
   lbvarmap = NULL;

   bw_contkernelvars = NULL;
   bw_contnonkernelvars = NULL;
   bw_kernelvars = NULL;
   bw_nonkernelvars = NULL;
   bw_intkernelvars = NULL;
   bw_intnonkernelvars = NULL;

   vars = NULL;
   contkernelvars = NULL;
   contnonkernelvars = NULL;
   kernelvars = NULL;
   nonkernelvars = NULL;
   intkernelvars = NULL;
   intnonkernelvars = NULL;
   binintvars = NULL;

   conss = NULL;
   bucketconss = NULL;
   bestlocval = SCIPinfinity(scip);
   twolevel = FALSE;
   success = TRUE;
   bestcurrsol = NULL;
   bucketlist = NULL;

   varlabels = NULL;
   conslabels = NULL;

   blocklabels = NULL;
   block2index = NULL;

   bw_ncontkernelvars = NULL;
   bw_ncontnonkernelvars = NULL;
   bw_nkernelvars = NULL;
   bw_nnonkernelvars = NULL;
   bw_nintkernelvars = NULL;
   bw_nintnonkernelvars = NULL;
   bw_contkernelcount = NULL;
   bw_contnonkernelcount = NULL;
   bw_kernelcount = NULL;
   bw_nonkernelcount = NULL;
   bw_intkernelcount = NULL;
   bw_intnonkernelcount = NULL;

   bw_cont_redcost = NULL;
   bw_redcost = NULL;
   bw_int_redcost = NULL;

   gapfactor = 1.0;
   gapcall = 0;
   blklbl_offset = 0;
   nblocks = 0;
   ndecomps = 0;
   nvars = 0;
   ncontkernelvars = 0;
   ncontnonkernelvars = 0;
   nkernelvars = 0;
   nnonkernelvars = 0;
   nintkernelvars = 0;
   nintnonkernelvars = 0;
   ncontvars = 0;
   nbinvars = 0;
   nintvars = 0;
   nbinintvars = 0;
   nconss = 0;
   nbestbucket = -1;
   iters = 0;


#ifdef DKS_WRITE_PROBLEMS
   SCIP_CALL( SCIPwriteOrigProblem(scip, "orig_problem.lp", NULL, FALSE) );
   SCIP_CALL( SCIPwriteTransProblem(scip, "trans_problem.lp", NULL, FALSE) );
#endif

   /* do not call dks in components of decompositions but only in the whole problem */
   if ( SCIPgetSubscipDepth(scip) > 0)
      return SCIP_OKAY;

   /* increase the call counter for one */
   heurdata->ncalls++;

   /* extract variables, constraints and number of constraints */
   if ( heurdata->usetransprob )
   {
      SCIP_VAR* tempvar;           /*< the transformed variable to each original variable */
      tempvar = NULL;

      /* Extract the decompositions of the transformed problem */
      SCIPgetDecomps(scip, &alldecomps, &ndecomps, FALSE);

      /* get the variable data */
      SCIP_CALL( SCIPgetVarsData(scip, &vars, &nvars, &nbinvars, &nintvars, NULL, &ncontvars) );

      /* create and initialize the hashmap for the original lower bounds */
      SCIP_CALL( SCIPhashmapCreate(&lbvarmap, SCIPblkmem(scip), nvars) );
      for ( i = 0; i < nvars; i++ )
      {
         SCIP_Real scalar;
         SCIP_Real constant;
         tempvar = vars[i];

         SCIP_CALL( SCIPvarGetOrigvarSum(&tempvar, &scalar, &constant) );

         if ( tempvar != NULL ) 
            SCIPhashmapSetImageReal(lbvarmap, vars[i], SCIPvarGetLbOriginal(tempvar));
      }

      /* initialize the constraints of the transformed problem */
      nconss = SCIPgetNConss(scip);
      conss = SCIPgetConss(scip);
   }
   else
   {
      /* Extract the decompositions of the original problem */
      SCIPgetDecomps(scip, &alldecomps, &ndecomps, TRUE);

      /* get variable data like amount of integers, binaries, overall and the variables */
      SCIP_CALL( SCIPgetOrigVarsData(scip, &vars, &nvars, &nbinvars, &nintvars, NULL, &ncontvars) );

      /* it is necessary to take the original variables here! otherwise they cant be used later on */
      vars = SCIPgetOrigVars(scip);
      /* get the constraints and their amount */
      nconss = SCIPgetNOrigConss(scip);
      conss = SCIPgetOrigConss(scip);
   }

   if ( ndecomps == 0 || !heurdata->usedecomp) 
   {
      SCIPdebugMsg(scip, "No decompositions available or wanted, going ahead without decomp\n");
      ndecomps = 0;           /** < set to 0 for later unnecessary ifs */
      nblocks = 0;            /** < 0 means no decomp in use */
   }
   else
   {
      /* take the first decomposition */
      decomp = alldecomps[0];
      SCIPdebugMsg(scip, "First original decomposition is selected\n");
      assert( decomp != NULL );

      nblocks = SCIPdecompGetNBlocks(decomp);
   }

   nbinintvars = nbinvars + nintvars;


   /* if problem has no constraints or no variables, terminate */
   if (nvars == 0 || nconss == 0)
   {
      SCIPdebugMsg(scip, "problem has no constraints or variables\n");
      goto TERMINATE;
   }

   /* estimate required memory and terminate if not enough memory is available */
   SCIP_CALL( SCIPgetRealParam(scip, "limits/memory", &memory) );
   if ( (SCIPgetMemUsed(scip) + SCIPgetMemExternEstim(scip))/1048576.0 >= memory )
   {
      SCIPdebugMsg(scip, "The estimated memory usage is too large.\n");
      goto TERMINATE;
   }

   SCIP_CALL( SCIPallocBufferArray(scip, &varlabels, nvars) );
   SCIP_CALL( SCIPallocBufferArray(scip, &conslabels, nconss) );
   if ( ndecomps > 0 && heurdata->usedecomp)
   {
      /*  extract the varlabels to identify linking variables */
      SCIPdecompGetVarsLabels(decomp, vars, varlabels, nvars);
      SCIPdecompGetConsLabels(decomp, conss, conslabels, nconss);

      /* prepare the distinct finding of blocklabels */
      SCIP_CALL( SCIPallocBufferArray(scip, &blocklabels, nblocks) );

      /* check if linking score of the instance is sufficiently low to get called */
      SCIP_CALL( getLinkingScoreAndBlocklabels(scip, &blocklabels, varlabels, conslabels, &linkscore, &nblocklabels, nblocks, nvars, nconss) );
      if ( linkscore > heurdata->maxlinkscore )
      {
         SCIPdebugMsg(scip, "decomposition has not required linking score\n");
         goto TERMINATE;
      }

      /* sort the blocklabels ascending */
      SCIPsortInt(blocklabels, nblocklabels);

      /* if it exists the blocklabel 0, we have to add an offset of 1 to store the linking variables at 0 */
      if ( blocklabels[0] == 0 )
         blklbl_offset = 1;

      /* fill the mapping of blocklabels to blockindices */
      SCIP_CALL( SCIPallocClearBlockMemoryArray(scip, &block2index, blocklabels[nblocklabels - 1] + 1 + blklbl_offset) );

      block2index[0] = 0;     /* SCIP_DECOMP_LINKVAR = -1, but are saved at index 0 */
      for ( b = 0; b < nblocklabels; b++ )
         block2index[blocklabels[b] + blklbl_offset] = b + 1;
      
   }
   else
   {
      /* initialize dummy varlabels to avoid further distinctions in the following code*/
      int v;
      
      for ( v = 0; v < nvars; v++ )
         varlabels[v] = 0;

      /* fill the mapping of blocklabels 0 to blockindices 0; nblocks = 0 in this case */
      SCIP_CALL( SCIPallocBufferArray(scip, &block2index, 1) );
      block2index[0] = 0;
   }

   /* if  necessary store the current best solution for later use */
   usebestsol = heurdata->usebestsol;
   if (heurdata->usebestsol)
   {
      if (SCIPgetNSols(scip) > 1)
         bestcurrsol = SCIPgetBestSol(scip);
      else
         usebestsol = FALSE;
   }

   /* initialize a kernel variable counter and a non kernel variable counter for each block + linking block */
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_ncontkernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_ncontnonkernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_nkernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_nnonkernelvars, nblocks + 1) );
   BMSclearMemoryArray(bw_ncontkernelvars, nblocks + 1);
   BMSclearMemoryArray(bw_ncontnonkernelvars, nblocks + 1);
   BMSclearMemoryArray(bw_nkernelvars, nblocks + 1);
   BMSclearMemoryArray(bw_nnonkernelvars, nblocks + 1);

   if ( nbinvars > 0 && nintvars > 0 && heurdata->usetwolevel )
   {
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_nintkernelvars, nblocks + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_nintnonkernelvars, nblocks + 1) );
      BMSclearMemoryArray(bw_nintkernelvars, nblocks + 1);
      BMSclearMemoryArray(bw_nintnonkernelvars, nblocks + 1);
   }


   /* if there are either integer variables or binary variables only, just consider these */
   if ( nbinvars == 0 || nintvars == 0 || !heurdata->usetwolevel)
   {
      SCIP_CALL( countKernelVariables(scip, vars, bestcurrsol, lbvarmap,
                  twolevel, usebestsol, heurdata->usetransprob, heurdata->translbkernel,
                  &bw_ncontkernelvars, &bw_ncontnonkernelvars, &bw_nkernelvars, &bw_nnonkernelvars, NULL, NULL,
                  &ncontkernelvars, &ncontnonkernelvars, &nkernelvars, &nnonkernelvars, NULL, NULL,
                  block2index, varlabels, blklbl_offset, nvars));

      SCIPdebugMsg(scip, "%d initial kernel variables\n", nkernelvars);

      /* if every variable is zero or its lower bound in the lp solution, terminate */
      if ( nkernelvars == 0 )
      {
         SCIPdebugMsg(scip, "No suitable variables for dks found. Leaving heuristic. \n");
         goto TERMINATE;
      }
      else if ( nkernelvars > nnonkernelvars )
      {
         SCIPdebugMsg(scip, "There are more kernel variables than not in the kernel\n");
      }
      
   }
   else
   {
      /* assumption before kernel variable count: we use 2-level buckets */
      twolevel = TRUE;

      SCIP_CALL( countKernelVariables(scip, vars, bestcurrsol, lbvarmap,
                  twolevel, usebestsol, heurdata->usetransprob, heurdata->translbkernel,
                  &bw_ncontkernelvars, &bw_ncontnonkernelvars, &bw_nkernelvars, &bw_nnonkernelvars, &bw_nintkernelvars, &bw_nintnonkernelvars,
                  &ncontkernelvars, &ncontnonkernelvars, &nkernelvars, &nnonkernelvars, &nintkernelvars, &nintnonkernelvars,
                  block2index, varlabels, blklbl_offset, nvars));

      SCIPdebugMsg(scip, "%d initial binary kernel variables\n%d initial integer kernel variables\n", nkernelvars, nintkernelvars);
      
      if (nkernelvars == 0)
      {
         if (nintkernelvars == 0)
         {
            SCIPdebugMsg(scip, "No suitable variables for the construction of a kernel fnlinkbinvarsound. Leaving heuristic. \n");
            goto TERMINATE;
         }
         else
         {
            /* the binary variables are all zero in the lp solution -> 1-level buckets with integer first and binary variables in kernel afterwards */
            nkernelvars = nintkernelvars;
            nnonkernelvars += nintnonkernelvars;
            nintkernelvars = 0;
            nintnonkernelvars = 0;

            /* update the blockwise figures describing kernel sizes */
            for (b = 0; b < nblocks + 1; b++)
            {
               bw_nkernelvars[b] = bw_nintkernelvars[b];
               bw_nnonkernelvars[b] += bw_nintnonkernelvars[b];
               bw_nintnonkernelvars[b] = 0;
            }

            twolevel = FALSE;
         }
      }
      else if (nintkernelvars == 0)
      {
         /* integer variables are all zero in lp solution -> 1-level buckets with binary first and integer variables in the kernel afterwards */
         twolevel = FALSE;
      }
      else if (nkernelvars > nnonkernelvars || nintkernelvars > nintnonkernelvars)
      {
         SCIPdebugMsg(scip, "There are more kernel variables than not in the kernel\n");
      }
   } 

   /*** kernel initialization ***/
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_contkernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_contnonkernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_kernelvars, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_nonkernelvars, nblocks + 1) );

   if (twolevel)
   {
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_intkernelvars, nblocks + 1 ) );
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_intnonkernelvars, nblocks + 1) );
   }
   /* initialize kernel and non kernel variables for each block */
   for (b = 0; b < nblocks + 1; b++)
   {
      int contblocksize = bw_ncontkernelvars[b] + bw_ncontnonkernelvars[b];
      int blocksize = bw_nkernelvars[b] + bw_nnonkernelvars[b];

      SCIP_CALL( SCIPallocBufferArray(scip, &(bw_contkernelvars[b]), contblocksize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &(bw_contnonkernelvars[b]), contblocksize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &(bw_kernelvars[b]), blocksize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &(bw_nonkernelvars[b]), blocksize) );

      if (twolevel)
      {
         int intblocksize = bw_nintkernelvars[b] + bw_nintnonkernelvars[b];

         SCIP_CALL( SCIPallocBufferArray(scip, &(bw_intkernelvars[b]), intblocksize) );
         SCIP_CALL( SCIPallocBufferArray(scip, &(bw_intnonkernelvars[b]), intblocksize) );
      }
   }

   maxcontkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * ncontkernelvars), ncontkernelvars + ncontnonkernelvars);
   maxcontnonkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * ncontnonkernelvars), ncontkernelvars + ncontnonkernelvars);
   maxkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * nkernelvars), nkernelvars + nnonkernelvars);
   maxnonkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * nnonkernelvars), nkernelvars + nnonkernelvars);
   /* initialize the kernel and non kernel variable arrays (just binary (non/)kernel variables if 2-level buckets) */
   SCIP_CALL( SCIPallocBufferArray(scip, &contkernelvars, maxcontkernelsize) );
   SCIP_CALL( SCIPallocBufferArray(scip, &contnonkernelvars, maxcontnonkernelsize) );
   SCIP_CALL( SCIPallocBufferArray(scip, &kernelvars, maxkernelsize) );
   SCIP_CALL( SCIPallocBufferArray(scip, &nonkernelvars, maxnonkernelsize) );

   /* include all binary AND integer variables as a separate array */
   SCIP_CALL( SCIPallocBufferArray(scip, &binintvars, nbinintvars) );

   /* extract (potential) initial kernel variables (value > 0) and not kernel variables */
   j = 0;
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_contkernelcount, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_contnonkernelcount, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_kernelcount, nblocks + 1) );
   SCIP_CALL( SCIPallocBufferArray(scip, &bw_nonkernelcount, nblocks + 1) );
   BMSclearMemoryArray(bw_contkernelcount, nblocks + 1);
   BMSclearMemoryArray(bw_contnonkernelcount, nblocks + 1);
   BMSclearMemoryArray(bw_kernelcount, nblocks + 1);
   BMSclearMemoryArray(bw_nonkernelcount, nblocks + 1);

   /* 2-level buckets are necessary */
   if ( twolevel )
   {
      /* additionally determine maximal integer kernel size */
      maxintkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * nintkernelvars), nintkernelvars + nintnonkernelvars);
      maxintnonkernelsize = MIN(SCIPfloor(scip, heurdata->kernelsizefactor * nintnonkernelvars), nintkernelvars + nintnonkernelvars);

      /* additionally initialize the integer kernel and the non integer kernel variable arrays */
      SCIP_CALL( SCIPallocBufferArray(scip, &intkernelvars, maxintkernelsize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &intnonkernelvars, maxintnonkernelsize) );

      /* initialize blocks + 1 to respect the linking variables as one block */
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_intkernelcount, nblocks + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &bw_intnonkernelcount, nblocks + 1) );
      BMSclearMemoryArray(bw_intkernelcount, nblocks + 1);
      BMSclearMemoryArray(bw_intnonkernelcount, nblocks + 1);

      /* filling of the kernels with the variables */
      SCIP_CALL( fillKernels(scip, vars, &binintvars, 
                              &bw_contkernelvars, &bw_contnonkernelvars, &bw_kernelvars, &bw_nonkernelvars, &bw_intkernelvars, &bw_intnonkernelvars,
                              bestcurrsol, lbvarmap, twolevel, usebestsol, heurdata->usetransprob, heurdata->translbkernel,
                              &bw_contkernelcount, &bw_contnonkernelcount, &bw_kernelcount, &bw_nonkernelcount, &bw_intkernelcount, &bw_intnonkernelcount,
                              block2index, varlabels, nblocks, blklbl_offset, nvars) );
   }
   else
   {
      /* filling of the kernels with the variables */
      SCIP_CALL( fillKernels(scip, vars, &binintvars,
                              &bw_contkernelvars, &bw_contnonkernelvars, &bw_kernelvars, &bw_nonkernelvars, NULL, NULL,
                              bestcurrsol, lbvarmap, twolevel, usebestsol, heurdata->usetransprob, heurdata->translbkernel,
                              &bw_contkernelcount, &bw_contnonkernelcount, &bw_kernelcount, &bw_nonkernelcount, NULL, NULL,
                              block2index, varlabels, nblocks, blklbl_offset, nvars) );
   }

   /*** sorting of bucket variables according to the reduced costs in non-decreasing order ***/
   if ( heurdata->redcostsort )
   {
      SCIP_CALL( reducedCostSort(scip, &bw_contnonkernelvars, &bw_nonkernelvars, &bw_intnonkernelvars,
                                 &bw_cont_redcost, &bw_redcost, &bw_int_redcost, 
                                 bw_ncontnonkernelvars, bw_nnonkernelvars, bw_nintnonkernelvars,
                                 twolevel, nblocks) );
   }

   /*** initialization of the buckets ***/
   /* determine the amount of buckets needed */
   /* continuous variables are not included when calculating the amount of buckets to investigate, since they are easier to handle */
   nusedratios = 0;
   if ( twolevel )
   {
      SCIP_Real intratio;
      SCIP_Real binratio;
      
      nbuckets = 0;
      for (b = 0; b < nblocks + 1; b++)
      {
          /* calculate the upper gauss bracket of the ratio of the integer (binary) kernel and non kernel variables */
         if (bw_nintnonkernelvars[b] > 0 && bw_nintkernelvars[b] > 0)
            intratio = SCIPceil(scip, bw_nintnonkernelvars[b] / (SCIP_Real)bw_nintkernelvars[b]);
         else
            intratio = SCIPinfinity(scip);
         
         if (bw_nnonkernelvars[b] > 0 && bw_nkernelvars[b] > 0)
            binratio = SCIPceil(scip, bw_nnonkernelvars[b] / (SCIP_Real)bw_nkernelvars[b]);
         else
            binratio = SCIPinfinity(scip);

         if (!SCIPisInfinity(scip, intratio))
         {
            nbuckets += intratio;
            nusedratios++;
         }
         if (!SCIPisInfinity(scip, binratio))
         {
            nbuckets += binratio;
            nusedratios++;
         }
      }
   }
   else
   {
      /* take the rounded down average bucket ratio of all blocks*/
      nbuckets = 0;
      for (b = 0; b < nblocks + 1; b++)
      {
         if (bw_nnonkernelvars[b] > 0 && bw_nkernelvars[b] > 0)
         {
            nbuckets += SCIPceil(scip, bw_nnonkernelvars[b] / (SCIP_Real)bw_nkernelvars[b]);
            nusedratios++;
         }
      }
   }
   /* taking the average ratio as final one for all blocks */
   if (nusedratios > 0)
      nbuckets = SCIPceil(scip, nbuckets / (SCIP_Real)nusedratios);
   else
      nbuckets = 0;

   /* determine the amount of iterations over the buckets/ amount of investigated buckets */
   iters = MIN(nbuckets, heurdata->maxbucks) + 1;
   
   /* create an extra array for the bucket constraints for hashmap creation in createBucketlistAndBuckets() */
   SCIP_CALL( SCIPduplicateBufferArray(scip, &bucketconss, conss, nconss) );

   /* create the bucketlist and initialize as much buckets as investigated later on with a subscip for every bucket */
   SCIP_CALL( createBucketlistAndBuckets(scip, bucketconss, nconss, iters - 1, &bucketlist, heurdata->usetransprob, &success) );
   if ( !success )
   {
      goto TERMINATE;
   }
   
   /* fill every bucket with its variables, nothing to do for the first ('kernel') bucket -> k = 1 */
   SCIP_CALL( fillBuckets(scip, &bucketlist,
                           bw_contnonkernelvars, bw_nonkernelvars, bw_intnonkernelvars,
                           bw_ncontnonkernelvars, bw_nnonkernelvars, bw_nintnonkernelvars,
                           bw_cont_redcost, bw_redcost, bw_int_redcost,
                           twolevel, heurdata->redcostlogsort, iters, nbuckets, nblocks) );
   
   /* build the kernelvariables out of each blocks kernel variables */
   j = 0;
   n = 0;
   m = 0;
   for (b = 0; b < nblocks + 1; b++)
   {
      for (l = 0; l < bw_ncontkernelvars[b]; l++)
         contkernelvars[j++] = bw_contkernelvars[b][l];

      for (l = 0; l < bw_nkernelvars[b]; l++)
         kernelvars[n++] = bw_kernelvars[b][l];
      
      if (twolevel)
      {
         for (l = 0; l < bw_nintkernelvars[b]; l++)
            intkernelvars[m++] = bw_intkernelvars[b][l];
      }
   }
   assert( j == ncontkernelvars );
   assert( n == nkernelvars );
   if (twolevel)
      assert( m == nintkernelvars );

   /* loop over all buckets, solve the small MIP defined by the bucket, adjust kernel */
   mipgap = 0.0;                    
   for ( k = 0; k < iters; k++ )
   {  
      SCIP_Bool found;
      SCIP_Bool infeasible;
      SCIP_Bool fixed;
      SCIP_Real lowerbound;
      SCIP_VAR *var;

      /* take the next bucket */
      bucket = &bucketlist->buckets[k];

      /* fix all integer and binary variables to zero that are neither in the kernel nor in the current bucket */
      for ( i = 0; i < nvars ; i++ )
      {
         found = FALSE;
         infeasible = TRUE;
         fixed = FALSE;

         var = vars[i];
         
         if (var == NULL)
            SCIPdebugMsg(scip, "Variable is null!\n");

         if ( SCIPvarGetType(var) == SCIP_VARTYPE_CONTINUOUS || SCIPvarGetType(var) == SCIP_VARTYPE_IMPLINT )
            SCIPdebugMsg(scip, "Hit a cont variable");

         /* search for the current variable in the kernel and in the current bucket */
         SCIP_CALL( searchKernelAndBucket(bucket, contkernelvars, ncontkernelvars, kernelvars, nkernelvars, intkernelvars, nintkernelvars, var, &found) );

         if ( found == TRUE )
            continue;

         if (var == NULL)
            goto TERMINATE;

         /* variable not in kernel or bucket -> fix to zero or lb if lb > 0 */
         assert(SCIPvarIsActive(var));

         var = bucket->scip2sub[SCIPvarGetProbindex(var)];
         if ( var != NULL )
         {
            lowerbound = SCIPvarGetLbGlobal( var );
            if ( lowerbound == -SCIPinfinity(scip) || lowerbound <= 0.0 )
            {
               SCIP_CALL( SCIPfixVar( bucket->subscip, var, 0.0, &infeasible, &fixed) );
               assert( !infeasible && fixed );
            }
            else
            {
               SCIP_CALL( SCIPfixVar ( bucket->subscip, var, lowerbound, &infeasible, &fixed) );
               assert( !infeasible && fixed );
            }
            
         }
      }

      /* construct a constraint that ensures the use of the bucketvariables */
      if (heurdata->addUseConss && bucket->bucketvars != NULL)
      {
         SCIP_CALL( addUseConstraint(bucket));
      }
      /* add objective cutoff if desired */
      if (heurdata->objcutoff)
      {
         SCIP_Real cutoff;
         SCIP_Real upperbound = SCIPgetUpperbound(scip);
         if ( !SCIPisInfinity(scip, upperbound) )
         {
            if ( !SCIPisInfinity(scip, -1.0 * SCIPgetLowerbound(scip)) )
            {
               cutoff = SCIPgetUpperbound(scip) + SCIPgetLowerbound(scip);
            }
            else
            {
               cutoff = SCIPgetUpperbound(scip);
            }
            cutoff = MIN(upperbound, cutoff);
            SCIP_CALL( SCIPsetObjlimit(bucket->subscip, cutoff) );
         }
      }

#ifdef DKS_WRITE_PROBLEMS
   if (bucket->number < 0) {
      char name[SCIP_MAXSTRLEN];
      /* write the current bucket problem */
      (void) SCIPsnprintf(name, SCIP_MAXSTRLEN, "subscip_bucket_%d.lp", bucket->number);
      SCIP_CALL( SCIPwriteOrigProblem(bucket->subscip, name, NULL, FALSE) );
   }
#endif
      /* get the time limit for every subscip */
      SCIP_CALL( SCIPgetRealParam(scip, "limits/time", &subtimelim) );
      if ( !SCIPisInfinity(scip, subtimelim) ){
         maxtime = subtimelim * heurdata->maxtimeshare;
         subtimelim = MIN(MAX(0.0, subtimelim - SCIPgetSolvingTime(scip)), maxtime - SCIPheurGetTime(heur));
         if ( subtimelim <= 0.0 )
            goto TERMINATE;
         else
            subtimelim = MAX(1.0, subtimelim/(iters - k)); //subscip gets at least 1 sec.
      }

      /* set the time and mipgap parameter for the subscip */
      SCIP_CALL( SCIPsetRealParam(bucket->subscip, "limits/time", subtimelim) );
      SCIP_CALL( SCIPsetRealParam(bucket->subscip, "limits/gap", mipgap) );

      /* solve the current subscip */
      SCIP_CALL_ABORT( SCIPsolve(bucket->subscip));
      status = SCIPgetStatus(bucket->subscip);

      if ( bucket->number == 0 )
         *result = SCIP_DIDNOTFIND;

      /* if the time limit was reached, higher the mip gap */
      /* gapcall = 1 signals timelimit was reached before, -1 signals gaplimit, 0 means no status was reached */
      if ( status == SCIP_STATUS_TIMELIMIT )
      {
         if (gapcall != 0)
            gapfactor /= 2;

         mipgap += ( heurdata->buckmaxgap / iters) * gapfactor;
         gapcall = 1;
      }
      else if ( status == SCIP_STATUS_GAPLIMIT )
      {
         if (gapcall != 0)
            gapfactor /= 2;

         mipgap -= (heurdata->buckmaxgap / iters) * gapfactor;
         gapcall = -1;
      }

      /* check if the solution is better if one of the three cases occur:
         - solution is optimal 
         - solution reached gaplimit  
         - time limit is reached and there is one solution */

      if ( status == SCIP_STATUS_OPTIMAL || status == SCIP_STATUS_GAPLIMIT ||
            (status == SCIP_STATUS_TIMELIMIT && SCIPgetNSols(bucket->subscip) > 0) )
      {
         SCIP_Real val;
         SCIP_SOL* sol;

         /* extract the solution of the solved subproblem */
         sol = SCIPgetBestSol(bucket->subscip);
         /* get the value of the solution of the subproblem */
         val =  SCIPgetSolOrigObj(bucket->subscip, sol);
         
         /* if there is no solution yet or if the value of the current solution is better than the saved solution */
         if ( SCIPisInfinity(scip, bestlocval) || val <= bestlocval )
         {
            bestlocval = val;
            nbestbucket = bucket->number;

            if ( heurdata->primalonly )
               break;

            /* adjust the kernel(/-variables) */
            SCIP_CALL( adjustKernelVars(scip, bucket, 
                                       &contkernelvars, &ncontkernelvars, maxcontkernelsize, 
                                       &kernelvars, &nkernelvars, maxkernelsize, 
                                       &intkernelvars, &nintkernelvars, maxintkernelsize, heurdata->deckernel, twolevel) );
         }
         success = FALSE;
      }
      else if ( status == SCIP_STATUS_TIMELIMIT )
      {
         SCIPdebugMsg(scip, "Bucket reached time limit. No optimal solution available.\n");
      }
      else if (status == SCIP_STATUS_INFEASIBLE )
      {
         /* nothing to do but have to catch this case*/
         SCIPdebugMsg(scip, "Bucket infeasible, starting over with next one\n");
      }
      else
      {
         SCIPdebugMsg(scip, "Bucket solving status %d is not supported\n", status);
         goto TERMINATE;
      }
      
      SCIP_CALL( SCIPfreeTransform(bucket->subscip) );

#ifdef DKS_KERNEL_INFO
      fclose(variable_info);
#endif
   }

   /* if a solution of a bucket was found, save it to the scip */
   if ( nbestbucket > -1 )
   {
      SCIP_SOL* newsol;
      SCIP_SOL* bestsol;
      BUCKET* bestbucket;
      SCIP_Real* bestsolvals;
      SCIP_VAR** subvars;
      int nsubvars;

      /* bucket with the best solution */
      bestbucket = &bucketlist->buckets[nbestbucket];

      /* get the best solution */
      bestsol = SCIPgetBestSol(bestbucket->subscip);
      assert( bestsol != NULL );

      SCIPdebug( SCIP_CALL( SCIPprintSol(bestbucket->subscip, bestsol, NULL, FALSE) ) );

      /* extract changed amount of variables and the variables itself */
      if ( heurdata->usetransprob )
      {
         subvars = SCIPgetVars(bestbucket->subscip);
         nsubvars = SCIPgetNVars(bestbucket->subscip);
      }
      else
      {
         subvars = SCIPgetOrigVars(bestbucket->subscip);
         nsubvars = SCIPgetNOrigVars(bestbucket->subscip);
      }
      

      /* extract the values of all variables in the best solution of a bucket found */
      SCIP_CALL( SCIPallocBufferArray(scip, &bestsolvals, nsubvars) );

      /* extract the values of all variables in the best solution */
      SCIP_CALL( SCIPgetSolVals(bestbucket->subscip, bestsol, nsubvars, subvars, bestsolvals) );

      /* create a new solution for the main scip */
      SCIP_CALL( SCIPcreateSol(scip, &newsol, heur) );

      /* save the value of each variable in the found solution in the new created solution */
      for ( i = 0; i < nvars; i++ )
      {
         SCIP_VAR* origvar;
         SCIP_VAR* subvar;
         SCIP_Real solval;

         origvar = vars[i];
         assert(origvar != NULL);
         assert(SCIPvarIsActive(origvar));

         subvar = bestbucket->scip2sub[i];
         if ( subvar == NULL )
            continue;

         solval = SCIPgetSolVal(bestbucket->subscip, bestsol, subvar);
         
         if ( !(heurdata->usetransprob) && SCIPvarGetStatus(origvar) == SCIP_VARSTATUS_COLUMN ) {
            SCIPdebugMessage("Variable is fixed, cannot set solution value!\n");
            continue;
         }
         
         SCIP_CALL( SCIPsetSolVal(scip, newsol, origvar, solval) );
      }

      SCIPdebug( SCIP_CALL( SCIPprintSol(scip, newsol, NULL, FALSE) ) );
      SCIPdebugMsg(scip, "Objective value %.2f\n", SCIPgetSolOrigObj(scip, newsol));
      SCIPdebugMsg(scip, "Objective value of subscip %.2f\n", SCIPgetSolOrigObj(bestbucket->subscip, bestsol));
      
      /* check the feasibilty of the new created solution, save it if so and free it afterwards */
      SCIP_CALL( SCIPtrySol(scip, newsol, TRUE, FALSE, TRUE, TRUE, TRUE, &success) );
      SCIP_CALL( SCIPfreeSol(scip, &newsol) );
      if (!success)
      {
         SCIPdebugMsg(scip, "Solution copy failed\n");
         *result = SCIP_DIDNOTFIND;
      }
      else
      {
         SCIPdebugMsg(scip, "Solution copy successfull after %f sec\n", SCIPgetSolvingTime(scip));
         *result = SCIP_FOUNDSOL;
      }
      if ( bestsolvals != NULL )
         SCIPfreeBufferArray(scip, &bestsolvals);
   }
   else
   {
      SCIPdebugMsg(scip, "no solution found\n");
      *result = SCIP_DIDNOTFIND;
   }


TERMINATE:
   if ( bucketconss != NULL )
      SCIPfreeBufferArray(scip, &bucketconss);

   if ( bw_intnonkernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_intnonkernelcount);

   if ( bw_intkernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_intkernelcount);      

   if ( intnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &intnonkernelvars);

   if ( intkernelvars != NULL )
      SCIPfreeBufferArray(scip, &intkernelvars);

   if ( bw_nonkernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_nonkernelcount);

   if ( bw_kernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_kernelcount);

   if ( bw_contnonkernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_contnonkernelcount);

   if ( bw_contkernelcount != NULL )
      SCIPfreeBufferArray(scip, &bw_contkernelcount);

   if ( binintvars != NULL )
      SCIPfreeBufferArray(scip, &binintvars);
   
   if ( nonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &nonkernelvars);

   if ( kernelvars != NULL )
      SCIPfreeBufferArray(scip, &kernelvars);

   if ( contnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &contnonkernelvars);

   if ( contkernelvars != NULL )
      SCIPfreeBufferArray(scip, &contkernelvars);

   if ( bw_intkernelvars != NULL )
   {
      for (b = 0; b < nblocks + 1; b++)
      {
         if ( bw_intnonkernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_intnonkernelvars[b]));
         if ( bw_intkernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_intkernelvars[b]));
      }
   }

   if ( bw_kernelvars != NULL )
   {
      for (b = 0; b < nblocks + 1; b++)
      {
         if ( bw_nonkernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_nonkernelvars[b]));
         if ( bw_kernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_kernelvars[b]));
      }
   }

   if ( bw_contkernelvars != NULL )
   {
      for (b = 0; b < nblocks + 1; b++)
      {
         if ( bw_contnonkernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_contnonkernelvars[b]));
         if ( bw_contkernelvars[b] != NULL )
            SCIPfreeBufferArray(scip, &(bw_contkernelvars[b]));
      }
   }

   if ( bw_intnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_intnonkernelvars);

   if ( bw_intkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_intkernelvars);
 
   if ( bw_nonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_nonkernelvars);

   if ( bw_kernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_kernelvars);

   if ( bw_contnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_contnonkernelvars);

   if ( bw_contkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_contkernelvars);

   if ( bw_nintnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_nintnonkernelvars);

   if ( bw_nintkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_nintkernelvars);

   if ( bw_nnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_nnonkernelvars);

   if ( bw_nkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_nkernelvars);

   if (bw_ncontnonkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_ncontnonkernelvars);

   if (bw_ncontkernelvars != NULL )
      SCIPfreeBufferArray(scip, &bw_ncontkernelvars);
 
   if ( block2index != NULL )
   {
      if ( blocklabels != NULL )
         SCIPfreeBlockMemoryArrayNull(scip, &block2index, blocklabels[nblocklabels - 1] + 1 + blklbl_offset);
      else
         SCIPfreeBufferArray(scip, &block2index);
   }

   if ( blocklabels != NULL )
      SCIPfreeBufferArray(scip, &blocklabels);


   if ( conslabels != NULL )
      SCIPfreeBufferArray(scip, &conslabels);

   if ( varlabels != NULL )
      SCIPfreeBufferArray(scip, &varlabels);
  
   SCIP_CALL( freeRedcostArrays(scip, &bw_cont_redcost, &bw_redcost, &bw_int_redcost, nblocks) );


   if ( lbvarmap != NULL )
      SCIPhashmapFree(&lbvarmap);

   if ( bucketlist != NULL ) 
   {
      for ( k = bucketlist->nbuckets - 1; k >= 1; k-- )
      {
         SCIP_CALL( freeBucket(scip, &bucketlist->buckets[k]) );

         SCIP_CALL( freeBucketArrays(scip, &bucketlist->buckets[k], twolevel) );
      }
      SCIP_CALL( freeBucket(scip, &bucketlist->buckets[0]) );
   }

   if ( bucketlist != NULL )
   {
      SCIP_CALL( freeBucketlist(&bucketlist, iters) );
   }

   SCIPdebugMsg(scip, "Leave dks heuristic\n");

   return SCIP_OKAY;
}


/*
 * primal heuristic specific interface methods
 */

/** creates the DKS primal heuristic and includes it in SCIP */
SCIP_RETCODE SCIPincludeHeurDKS(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_HEURDATA* heurdata;
   SCIP_HEUR* heur;

   /* create dks primal heuristic data */
   SCIP_CALL( SCIPallocBlockMemory(scip, &heurdata) );
   //heurdata = NULL;

   heur = NULL;

   heurdata->ncalls = 0;

   /* include primal heuristic */

   /* use SCIPincludeHeurBasic() plus setter functions if you want to set callbacks one-by-one and your code should
    * compile independent of new callbacks being added in future SCIP versions
    */
   SCIP_CALL( SCIPincludeHeurBasic(scip, &heur,
         HEUR_NAME, HEUR_DESC, HEUR_DISPCHAR, HEUR_PRIORITY, HEUR_FREQ, HEUR_FREQOFS,
         HEUR_MAXDEPTH, HEUR_TIMING, HEUR_USESSUBSCIP, heurExecDKS, heurdata) );

   assert(heur != NULL);

   /* set non fundamental callbacks via setter functions */
   SCIP_CALL( SCIPsetHeurCopy(scip, heur, heurCopyDKS) );
   SCIP_CALL( SCIPsetHeurFree(scip, heur, heurFreeDKS) );

   /* add dks primal heuristic parameters */
   SCIP_CALL( SCIPaddIntParam(scip, "heuristics/" HEUR_NAME "/maxbucks",
      "maximal number of bucks to be investigated", &heurdata->maxbucks, TRUE, 20, 1, 100, NULL, NULL) );
   
   SCIP_CALL( SCIPaddRealParam(scip, "heuristics/" HEUR_NAME "/kernelsizefactor",
      "factor with which the initial kernel size can grow max", &heurdata->kernelsizefactor, 
      TRUE, 2.0, 1.0, 10.0, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/addUseConss",
      "should a constraint be added ensuring that bucket variables are used?",
      &heurdata->addUseConss, TRUE, DEFAULT_ADDUSECONSS, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/linkbucksize",
      "should the linking variables in the kernel influence the size of the buckets?",
      &heurdata->linkbucksize, TRUE, DEFAULT_LINKBUCKSIZE, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/translbkernel",
      "should a variable with different lower bound in trans and orig prob be in the kernel?",
      &heurdata->translbkernel, TRUE, DEFAULT_TRANSLBKERNEL, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/lesslockskernel",
      "should a variable with max one uplock and one downlock be in the kernel?",
      &heurdata->lesslockskernel, TRUE, DEFAULT_LESSLOCKSKERNEL, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/usetransprob", 
      "should dks use the transformed problem?", 
      &heurdata->usetransprob, TRUE, DEFAULT_USETRANSPROB, NULL, NULL) );

   SCIP_CALL( SCIPaddRealParam(scip, "heuristics/" HEUR_NAME "/buckmaxgap",
      "defines the maximum mipgap a bucket can be solved to", &heurdata->buckmaxgap,
      TRUE, 0.01, 0.0, 0.05, NULL, NULL) );

   SCIP_CALL( SCIPaddRealParam(scip, "heuristics/" HEUR_NAME "/maxlinkscore",
      "defines a bound to the linkscore of the decomp", &heurdata->maxlinkscore,
      TRUE, 1.0, 0.0, 1.0, NULL, NULL) );

   SCIP_CALL( SCIPaddRealParam(scip, "heuristics/" HEUR_NAME "/maxtimeshare", 
      "timelimit of heuristic as share of overall limit", &heurdata->maxtimeshare,
      FALSE, DEFAULT_MAX_TIME, 0.0, 1.0, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/usetwolevel",
      "should a two level bucket structure be used if possible", &heurdata->usetwolevel,
      FALSE, DEFAULT_USETWOLEVEL, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/usedecomp",
      "should a decomposition be used if given", &heurdata->usedecomp,
      FALSE, DEFAULT_USEDECOMP, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/usebestsol", 
      "should the best solution instead of the LP solution be used", &heurdata->usebestsol,
      FALSE, DEFAULT_USEBESTSOL, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/redcostsort", 
      "should the bucket variables be sorted by reduced costs in the LP solution", &heurdata->redcostsort,
      FALSE, DEFAULT_REDCOSTSORT, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/decreasekernel",
      "should the kernel be decreased when a better solution is found", &heurdata->deckernel,
      FALSE, DEFAULT_DECKERNEL, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/primalonly", 
      "should the heuristic terminate after the first primal solution found", &heurdata->primalonly, 
      FALSE, DEFAULT_PRIMALONLY, NULL, NULL) );

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/redcostlogsort",
      "should the bucket variables be sorted logarithmically by reduced costs in the LP solution", &heurdata->redcostlogsort,
      FALSE, DEFAULT_REDCOSTLOGSORT, NULL, NULL) ) ;

   SCIP_CALL( SCIPaddBoolParam(scip, "heuristics/" HEUR_NAME "/objcutoff",
      "should the next solution at least satisfy the old objective?",
      &heurdata->objcutoff, FALSE, DEFAULT_OBJCUTOFF, NULL, NULL) );

   return SCIP_OKAY;
}
