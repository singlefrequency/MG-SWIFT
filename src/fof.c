/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2018 James Willis (james.s.willis@durham.ac.uk)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

/* Config parameters. */
#include "../config.h"

/* This object's header. */
#include "fof.h"

/* Local headers. */
#include "threadpool.h"
#include "engine.h"

#define MPI_RANK_0_SEND_TAG 666
#define MPI_RANK_1_SEND_TAG 999

MPI_Datatype fof_mpi_type;
FILE *fof_file;

/* Initialises parameters for the FOF search. */
void fof_init(struct space *s, long long Ngas, long long Ngparts) {

  /* Calculate the particle linking length based upon the mean inter-particle
   * spacing of the DM particles. */
  const int total_nr_dmparts = Ngparts - Ngas;
  const double l_x = 0.2 * (s->dim[0] / cbrt(total_nr_dmparts));
  s->l_x2 = l_x * l_x;

#ifdef WITH_MPI
  if (MPI_Type_contiguous(sizeof(struct fof_mpi) / sizeof(unsigned char), MPI_BYTE,
        &fof_mpi_type) != MPI_SUCCESS ||
      MPI_Type_commit(&fof_mpi_type) != MPI_SUCCESS) {
    error("Failed to create MPI type for fof.");
  }
#endif

}

/* Finds the root ID of the group a particle exists in. */
__attribute__((always_inline)) INLINE static int fof_find(const int i,
                                                          int *group_id) {

  int root = i;
  /* TODO: May need this atomic here:
   * while (root != atomic_cas(&group_id[root], group_id[root], group_id[root])) 
   *   root = atomic_cas(&group_id[root], group_id[root], group_id[root]); */
  while (root != group_id[root]) root = group_id[root];

  /* Perform path compression. */
  // int index = i;
  // while(index != root) {
  //  int next = group_id[index];
  //  group_id[index] = root;
  //  index = next;
  //}

  return root;
}

/* Find the shortest distance between cells, remembering to account for boundary
 * conditions. */
__attribute__((always_inline)) INLINE static double cell_min_dist(
    const struct cell *ci, const struct cell *cj, const double *dim) {

  /* Get cell locations. */
  const double cix = ci->loc[0];
  const double ciy = ci->loc[1];
  const double ciz = ci->loc[2];
  const double cjx = cj->loc[0];
  const double cjy = cj->loc[1];
  const double cjz = cj->loc[2];

  /* Find the shortest distance between cells, remembering to account for
   * boundary conditions. */
  double dx[3], r2 = 0.0f;
  dx[0] = min3(fabs(nearest(cix - cjx, dim[0])),
               fabs(nearest(cix - (cjx + cj->width[0]), dim[0])),
               fabs(nearest((cix + ci->width[0]) - cjx, dim[0])));
  dx[0] = min(
      dx[0], fabs(nearest((cix + ci->width[0]) - (cjx + cj->width[0]), dim[0])));

  dx[1] = min3(fabs(nearest(ciy - cjy, dim[1])),
               fabs(nearest(ciy - (cjy + cj->width[1]), dim[1])),
               fabs(nearest((ciy + ci->width[1]) - cjy, dim[1])));
  dx[1] = min(
      dx[1], fabs(nearest((ciy + ci->width[1]) - (cjy + cj->width[1]), dim[1])));

  dx[2] = min3(fabs(nearest(ciz - cjz, dim[2])),
               fabs(nearest(ciz - (cjz + cj->width[2]), dim[2])),
               fabs(nearest((ciz + ci->width[2]) - cjz, dim[2])));
  dx[2] = min(
      dx[2], fabs(nearest((ciz + ci->width[2]) - (cjz + cj->width[2]), dim[2])));

  for (int k = 0; k < 3; k++) r2 += dx[k] * dx[k];

  return r2;
}

/* Recurse on a pair of cells and perform a FOF search between cells that are within
 * range. */
static void rec_fof_search_pair(struct cell *ci, struct cell *cj, struct space *s,
                           const double *dim,
                           const double search_r2) {

  /* Find the shortest distance between cells, remembering to account for
   * boundary conditions. */
  const double r2 = cell_min_dist(ci, cj, dim);

  /* Return if cells are out of range of each other. */
  if (r2 > search_r2) return;

  /* Recurse on both cells if they are both split. */
  if (ci->split && cj->split) {
    for (int k = 0; k < 8; k++) {
      if (ci->progeny[k] != NULL) {

        for (int l = 0; l < 8; l++)
          if (cj->progeny[l] != NULL)
            rec_fof_search_pair(ci->progeny[k], cj->progeny[l], s, dim, search_r2);
      }
    }
  }
  /* Perform FOF search between pairs of cells that are within the linking
   * length and not the same cell. */
  else if (ci != cj)
    fof_search_pair_cells(s, ci, cj);
  else if (ci == cj) error("Pair FOF called on same cell!!!");

}

/* Recurse on a pair of cells (one local, one foreign( and perform a FOF search between cells that are within
 * range. */
static void rec_fof_search_pair_foreign(struct cell *ci, struct cell *cj, struct space *s,
                           const double *dim,
                           const double search_r2, size_t *send_count, struct fof_mpi *fof_send) {

  /* Find the shortest distance between cells, remembering to account for
   * boundary conditions. */
  const double r2 = cell_min_dist(ci, cj, dim);

  /* Return if cells are out of range of each other. */
  if (r2 > search_r2) return;

  /* Recurse on both cells if they are both split. */
  if (ci->split && cj->split) {
    for (int k = 0; k < 8; k++) {
      if (ci->progeny[k] != NULL) {

        for (int l = 0; l < 8; l++)
          if (cj->progeny[l] != NULL)
            rec_fof_search_pair_foreign(ci->progeny[k], cj->progeny[l], s, dim, search_r2, send_count, fof_send);
      }
    }
  }
  /* Perform FOF search between pairs of cells that are within the linking
   * length and not the same cell. */
  else if (ci != cj)
    fof_search_pair_cells_foreign(s, ci, cj, send_count, fof_send);
  else if (ci == cj) error("Pair FOF called on same cell!!!");

}

static void rec_fof_search_pair_foreign_count(struct cell *ci, struct cell *cj, struct space *s,
                           const double *dim,
                           const double search_r2, size_t *nr_links) {

  /* Find the shortest distance between cells, remembering to account for
   * boundary conditions. */
  const double r2 = cell_min_dist(ci, cj, dim);

  /* Return if cells are out of range of each other. */
  if (r2 > search_r2) return;

  /* Recurse on both cells if they are both split. */
  if (ci->split && cj->split) {
    for (int k = 0; k < 8; k++) {
      if (ci->progeny[k] != NULL) {

        for (int l = 0; l < 8; l++)
          if (cj->progeny[l] != NULL)
            rec_fof_search_pair_foreign_count(ci->progeny[k], cj->progeny[l], s, dim, search_r2, nr_links);
      }
    }
  }
  /* Perform FOF search between pairs of cells that are within the linking
   * length and not the same cell. */
  else if (ci != cj) {
    *nr_links += ci->gcount * cj->gcount;
    return;
  }
  else if (ci == cj) error("Pair FOF called on same cell!!!");

}

/* Recurse on a cell and perform a FOF search between cells that are within
 * range. */
static void rec_fof_search_self(struct cell *ci, struct space *s,
                           const double *dim,
                           const double search_r2) {

  /* Recurse? */
  if (ci->split) {
    
    /* Loop over all progeny. Perform pair and self recursion on progenies.*/
    for (int k = 0; k < 8; k++) {
      if (ci->progeny[k] != NULL) {

        rec_fof_search_self(ci->progeny[k], s, dim, search_r2);

        for (int l = k+1; l < 8; l++)
          if (ci->progeny[l] != NULL)
            rec_fof_search_pair(ci->progeny[k], ci->progeny[l], s, dim, search_r2);
      }
    }
  }
  /* Otherwise, compute self-interaction. */
  else 
    fof_search_cell(s, ci);
}

/* Perform naive N^2 FOF search on gravity particles using the Union-Find
 * algorithm.*/
void fof_search_serial(struct space *s) {

  const size_t nr_gparts = s->nr_gparts;
  struct gpart *gparts = s->gparts;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double l_x2 = s->l_x2;
  int *group_id = s->group_id;
  int *group_size;
  int num_groups = 0;

  message("Searching %zu gravity particles for links with l_x2: %lf", nr_gparts,
          l_x2);

  /* Allocate and initialise a group size array. */
  if (posix_memalign((void **)&group_size, 32, nr_gparts * sizeof(int)) != 0)
    error("Failed to allocate list of number in groups for FOF search.");

  bzero(group_size, nr_gparts * sizeof(int));

  /* Loop over particles and find which particles belong in the same group. */
  for (size_t i = 0; i < nr_gparts; i++) {

    struct gpart *pi = &gparts[i];
    const double pix = pi->x[0];
    const double piy = pi->x[1];
    const double piz = pi->x[2];

    /* Find the root of pi. */
    int root_i = fof_find(i, group_id);
    
    for (size_t j = i + 1; j < nr_gparts; j++) {

      /* Find the roots of pi and pj. */
      const int root_j = fof_find(j, group_id);

      /* Skip particles in the same group. */
      if (root_i == root_j) continue;

      struct gpart *pj = &gparts[j];
      const double pjx = pj->x[0];
      const double pjy = pj->x[1];
      const double pjz = pj->x[2];

      /* Compute pairwise distance, remembering to account for boundary
       * conditions. */
      float dx[3], r2 = 0.0f;
      dx[0] = pix - pjx;
      dx[1] = piy - pjy;
      dx[2] = piz - pjz;

      for (int k = 0; k < 3; k++) {
        dx[k] = nearest(dx[k], dim[k]);
        r2 += dx[k] * dx[k];
      }

      /* Hit or miss? */
      if (r2 < l_x2) {

        /* If the root ID of pj is lower than pi's root ID set pi's root to point to pj's. 
         * Otherwise set pj's to root to point to pi's.*/
        if (root_j < root_i) {
          group_id[root_i] = root_j;
          /* Update root_i on the fly. */
          root_i = root_j;
        }
        else
          group_id[root_j] = root_i;

      }
    }
  }

  /* Calculate the total number of particles in each group and total number of groups. */
  for (size_t i = 0; i < nr_gparts; i++) {
    group_size[fof_find(i, group_id)]++;
    if(group_id[i] == i) num_groups++;
  }

  fof_dump_group_data("fof_output_serial.dat", nr_gparts, group_id, group_size);

  int num_parts_in_groups = 0;
  int max_group_size = 0, max_group_id = 0;
  for (size_t i = 0; i < nr_gparts; i++) {

    if (group_size[i] > 1) num_parts_in_groups += group_size[i];
    if (group_size[i] > max_group_size) {
      max_group_size = group_size[i];
      max_group_id = i;
    }
  }

  message(
      "No. of groups: %d. No. of particles in groups: %d. No. of particles not "
      "in groups: %zu.",
      num_groups, num_parts_in_groups, nr_gparts - num_parts_in_groups);
  message("Biggest group size: %d with ID: %d", max_group_size, max_group_id);

  free(group_size);
}

/* Perform a FOF search on a single cell using the Union-Find algorithm.*/
void fof_search_cell(struct space *s, struct cell *c) {

  const size_t count = c->gcount;
  struct gpart *gparts = c->gparts;
  const double l_x2 = s->l_x2;
  int *group_id = s->group_id;

  /* Make a list of particle offsets into the global gparts array. */
  int *const offset = group_id + (ptrdiff_t)(gparts - s->gparts);

  /* Loop over particles and find which particles belong in the same group. */
  for (size_t i = 0; i < count; i++) {

    struct gpart *pi = &gparts[i];
    const double pix = pi->x[0];
    const double piy = pi->x[1];
    const double piz = pi->x[2];

    /* Find the root of pi. */
    int root_i = fof_find(offset[i], group_id);

    for (size_t j = i + 1; j < count; j++) {

      /* Find the root of pj. */
      const int root_j = fof_find(offset[j], group_id);

      /* Skip particles in the same group. */
      if (root_i == root_j) continue;

      struct gpart *pj = &gparts[j];
      const double pjx = pj->x[0];
      const double pjy = pj->x[1];
      const double pjz = pj->x[2];

      /* Compute the pairwise distance */
      float dx[3], r2 = 0.0f;
      dx[0] = pix - pjx;
      dx[1] = piy - pjy;
      dx[2] = piz - pjz;

      for (int k = 0; k < 3; k++) r2 += dx[k] * dx[k];

      /* Hit or miss? */
      if (r2 < l_x2) {

        /* If the root ID of pj is lower than pi's root ID set pi's root to point to pj's. 
         * Otherwise set pj's to root to point to pi's.*/
        if (root_j < root_i) {
          atomic_min(&group_id[root_i], root_j);
          /* Update root_i on the fly. */
          root_i = root_j;
        }
        else
          atomic_min(&group_id[root_j], root_i);

      }
    }
  }
}

/* Perform a FOF search on a pair of cells using the Union-Find algorithm.*/
void fof_search_pair_cells(struct space *s, struct cell *ci, struct cell *cj) {

  const size_t count_i = ci->gcount;
  const size_t count_j = cj->gcount;
  struct gpart *gparts_i = ci->gparts;
  struct gpart *gparts_j = cj->gparts;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double l_x2 = s->l_x2;
  int *group_id = s->group_id;
  
  /* Make a list of particle offsets into the global gparts array. */
  int *const offset_i = group_id + (ptrdiff_t)(gparts_i - s->gparts);
  int *const offset_j = group_id + (ptrdiff_t)(gparts_j - s->gparts);

  /* Account for boundary conditions.*/
  double shift[3] = {0.0, 0.0, 0.0};

  /* Get the relative distance between the pairs, wrapping. */
  const int periodic = s->periodic;
  double diff[3];
  for (int k = 0; k < 3; k++) {
    diff[k] = cj->loc[k] - ci->loc[k];
    if (periodic && diff[k] < -dim[k] / 2)
      shift[k] = dim[k];
    else if (periodic && diff[k] > dim[k] / 2)
      shift[k] = -dim[k];
    else
      shift[k] = 0.0;
    diff[k] += shift[k];
  }

  /* Loop over particles and find which particles belong in the same group. */
  for (size_t i = 0; i < count_i; i++) {

    struct gpart *pi = &gparts_i[i];
    const double pix = pi->x[0] - shift[0];
    const double piy = pi->x[1] - shift[1];
    const double piz = pi->x[2] - shift[2];

    /* Find the root of pi. */
    int root_i = fof_find(offset_i[i], group_id);
    
    for (size_t j = 0; j < count_j; j++) {

      /* Find the root of pj. */
      const int root_j = fof_find(offset_j[j], group_id);

      /* Skip particles in the same group. */
      if (root_i == root_j) continue;

      struct gpart *pj = &gparts_j[j];
      const double pjx = pj->x[0];
      const double pjy = pj->x[1];
      const double pjz = pj->x[2];

      /* Compute pairwise distance, remembering to account for boundary
       * conditions. */
      float dx[3], r2 = 0.0f;
      dx[0] = pix - pjx;
      dx[1] = piy - pjy;
      dx[2] = piz - pjz;

      for (int k = 0; k < 3; k++) r2 += dx[k] * dx[k];

      /* Hit or miss? */
      if (r2 < l_x2) {

        /* If the root ID of pj is lower than pi's root ID set pi's root to point to pj's. 
         * Otherwise set pj's to root to point to pi's.*/
        if (root_j < root_i) {
          atomic_min(&group_id[root_i], root_j);
          /* Update root_i on the fly. */
          root_i = root_j;
        }
        else
          atomic_min(&group_id[root_j], root_i);

      }
    }
  }
}

/* Perform a FOF search between a local and foreign cell using the Union-Find algorithm. 
 * Communicate any links found between particles to the appropriate node.*/
void fof_search_pair_cells_foreign(struct space *s, struct cell *ci, struct cell *cj, size_t *send_count, struct fof_mpi *fof_send ) {

  const size_t count_i = ci->gcount;
  const size_t count_j = cj->gcount;
  struct gpart *gparts_i = ci->gparts;
  struct gpart *gparts_j = cj->gparts;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double l_x2 = s->l_x2;
  int *group_id = s->group_id;
  
  /* Make a list of particle offsets into the global gparts array. */
  int *const offset_i = group_id + (ptrdiff_t)(gparts_i - s->gparts);
  int *const offset_j = group_id + (ptrdiff_t)(gparts_j - s->gparts);

  /* Account for boundary conditions.*/
  double shift[3] = {0.0, 0.0, 0.0};

  /* Check whether cells are local to the node. */
  const int ci_local = (ci->nodeID == engine_rank);
  const int cj_local = (cj->nodeID == engine_rank);

  if((ci_local && cj_local) || (!ci_local && !cj_local)) error("FOF search of foreign cells called on two local cells or two foreign cells.");

  size_t local_send_count = 0;

  /* Get the relative distance between the pairs, wrapping. */
  const int periodic = s->periodic;
  double diff[3];
  
  if(ci_local) {

    for (int k = 0; k < 3; k++) {
      diff[k] = cj->loc[k] - ci->loc[k];
      if (periodic && diff[k] < -dim[k] / 2)
        shift[k] = dim[k];
      else if (periodic && diff[k] > dim[k] / 2)
        shift[k] = -dim[k];
      else
        shift[k] = 0.0;
      diff[k] += shift[k];
    }

    /* Loop over particles and find which particles belong in the same group. */
    for (size_t i = 0; i < count_i; i++) {

      struct gpart *pi = &gparts_i[i];
      const double pix = pi->x[0] - shift[0];
      const double piy = pi->x[1] - shift[1];
      const double piz = pi->x[2] - shift[2];

      /* Find the root of pi. */
      int root_i = fof_find(offset_i[i], group_id);

      for (size_t j = 0; j < count_j; j++) {

        struct gpart *pj = &gparts_j[j];
        const double pjx = pj->x[0];
        const double pjy = pj->x[1];
        const double pjz = pj->x[2];

        /* Compute pairwise distance, remembering to account for boundary
         * conditions. */
        float dx[3], r2 = 0.0f;
        dx[0] = pix - pjx;
        dx[1] = piy - pjy;
        dx[2] = piz - pjz;

        for (int k = 0; k < 3; k++) r2 += dx[k] * dx[k];

        /* Hit or miss? */
        if (r2 < l_x2) {

          /* Store the particle IDs and group ID for communication. */
          //fof_send[send_count].local_pid = pi->id_or_neg_offset;        
          fof_send[*send_count].foreign_pid = pj->id_or_neg_offset;        
          fof_send[*send_count].root_i = root_i;
          (*send_count)++;
          local_send_count++;

          fprintf(fof_file, "  %7zu %7zu %7d\n", pi->id_or_neg_offset, pj->id_or_neg_offset, root_i);
        }
      }
    }

  }

  if(cj_local) {

    for (int k = 0; k < 3; k++) {
      diff[k] = ci->loc[k] - cj->loc[k];
      if (periodic && diff[k] < -dim[k] / 2)
        shift[k] = dim[k];
      else if (periodic && diff[k] > dim[k] / 2)
        shift[k] = -dim[k];
      else
        shift[k] = 0.0;
      diff[k] += shift[k];
    }

    /* Loop over particles and find which particles belong in the same group. */
    for (size_t j = 0; j < count_j; j++) {

      struct gpart *pj = &gparts_j[j];
      const double pjx = pj->x[0] - shift[0];
      const double pjy = pj->x[1] - shift[1];
      const double pjz = pj->x[2] - shift[2];

      /* Find the root of pj. */
      int root_j = fof_find(offset_j[j], group_id);

      for (size_t i = 0; i < count_i; i++) {

        struct gpart *pi = &gparts_i[i];
        const double pix = pi->x[0];
        const double piy = pi->x[1];
        const double piz = pi->x[2];

        /* Compute pairwise distance, remembering to account for boundary
         * conditions. */
        float dx[3], r2 = 0.0f;
        dx[0] = pjx - pix;
        dx[1] = pjy - piy;
        dx[2] = pjz - piz;

        for (int k = 0; k < 3; k++) r2 += dx[k] * dx[k];

        /* Hit or miss? */
        if (r2 < l_x2) {

          /* Store the particle IDs and group ID for communication. */
          //fof_send[send_count].local_pid = pi->id_or_neg_offset;        
          fof_send[*send_count].foreign_pid = pi->id_or_neg_offset; 
          fof_send[*send_count].root_i = root_j;
          (*send_count)++;
          local_send_count++;
          fprintf(fof_file, "  %7zu %7zu %7d\n", pj->id_or_neg_offset, pi->id_or_neg_offset, root_j);
        }
      }
    }

  }

  //if(local_send_count > 0) {

  //  if(ci_local) 
  //    message("Rank: %d sending %zu links to rank %d for testing. ci: (%lf,%lf,%lf) -> cj: (%lf,%lf,%lf).", engine_rank, local_send_count, cj->nodeID, ci->loc[0], ci->loc[1], ci->loc[2], cj->loc[0], cj->loc[1], cj->loc[2]);
  //  
  //  if(cj_local) 
  //    message("Rank: %d sending %zu links to rank %d for testing. cj: (%lf,%lf,%lf) -> ci: (%lf,%lf,%lf).", engine_rank, local_send_count, ci->nodeID, cj->loc[0], cj->loc[1], cj->loc[2], ci->loc[0], ci->loc[1], ci->loc[2]);
  //}

}

/* Perform a FOF search on gravity particles using the cells and applying the
 * Union-Find algorithm.*/
void fof_search_tree_serial(struct space *s) {

  const size_t nr_gparts = s->nr_gparts;
  const size_t nr_cells = s->nr_cells;
  struct gpart *gparts = s->gparts;
  int *group_id;
  int *group_size;
  float *group_mass;
  int num_groups = 0;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double search_r2 = s->l_x2;
  ticks tic = getticks();

  message("Searching %zu gravity particles for links with l_x2: %lf", nr_gparts,
          s->l_x2);

  /* Allocate and initialise array of particle group IDs. */
  if(s->group_id != NULL) free(s->group_id);

  if (posix_memalign((void **)&s->group_id, 32, nr_gparts * sizeof(int)) != 0)
    error("Failed to allocate list of particle group IDs for FOF search.");

  /* Initial group ID is particle offset into array. */
  for (size_t i = 0; i < nr_gparts; i++) s->group_id[i] = i;

  group_id = s->group_id;
  
  message("Rank: %d, Allocated group_id array of size %zu", engine_rank, s->nr_gparts);

  /* Allocate and initialise a group size array. */
  if (posix_memalign((void **)&group_size, 32, nr_gparts * sizeof(int)) != 0)
    error("Failed to allocate list of group size for FOF search.");
  
  /* Allocate and initialise a group mass array. */
  if (posix_memalign((void **)&group_mass, 32, nr_gparts * sizeof(float)) != 0)
    error("Failed to allocate list of group masses for FOF search.");

  bzero(group_size, nr_gparts * sizeof(int));
  bzero(group_mass, nr_gparts * sizeof(float));

  /* Loop over cells and find which cells are in range of each other to perform
   * the FOF search. */
  for (size_t cid = 0; cid < nr_cells; cid++) {

    struct cell *restrict ci = &s->cells_top[cid];
    
    /* Only perform FOF search on local cells. */
    if(ci->nodeID == engine_rank) {

      /* Skip empty cells. */
      if(ci->gcount == 0) continue;

      /* Perform FOF search on local particles within the cell. */
      rec_fof_search_self(ci, s, dim, search_r2);

      /* Loop over all top-level cells skipping over the cells already searched.
      */
      for (size_t cjd = cid + 1; cjd < nr_cells; cjd++) {

        struct cell *restrict cj = &s->cells_top[cjd];

        /* Only perform FOF search on local cells. */
        if(cj->nodeID == engine_rank) {
          /* Skip empty cells. */
          if(cj->gcount == 0) continue;

          rec_fof_search_pair(ci, cj, s, dim, search_r2);
        }
      }
    }   
  }

  /* Calculate the total number of particles in each group, group mass and the total number of groups. */
  for (size_t i = 0; i < nr_gparts; i++) {
    const int root = fof_find(i, group_id);
    group_size[root]++;
    group_mass[root] += gparts[i].mass;
    if(group_id[i] == i) num_groups++;
  }

  fof_dump_group_data("fof_output_tree_serial.dat", nr_gparts, group_id,
                      group_size);

  int num_parts_in_groups = 0;
  int max_group_size = 0, max_group_id = 0, max_group_mass_id = 0;
  float max_group_mass = 0;
  for (size_t i = 0; i < nr_gparts; i++) {

    /* Find the total number of particles in groups. */
    if (group_size[i] > 1) num_parts_in_groups += group_size[i];

    /* Find the largest group. */
    if (group_size[i] > max_group_size) {
      max_group_size = group_size[i];
      max_group_id = i;
    }
    
    /* Find the largest group by mass. */
    if (group_mass[i] > max_group_mass) {
      max_group_mass = group_mass[i];
      max_group_mass_id = i;
    }
  }

  message(
      "No. of groups: %d. No. of particles in groups: %d. No. of particles not "
      "in groups: %zu.",
      num_groups, num_parts_in_groups, nr_gparts - num_parts_in_groups);
  message("Biggest group size: %d with ID: %d", max_group_size, max_group_id);
  message("Biggest group by mass: %f with ID: %d", max_group_mass, max_group_mass_id);

  free(group_size);
  free(group_mass);
  
  message("Serial FOF search took: %.3f %s.",
      clocks_from_ticks(getticks() - tic), clocks_getunit());
}

/**
 * @brief Mapper function to perform FOF search.
 *
 * @param map_data An array of #cell%s.
 * @param num_elements Chunk size.
 * @param extra_data Pointer to a #space.
 */
void fof_search_tree_mapper(void *map_data, int num_elements,
                            void *extra_data) {

  /* Retrieve mapped data. */
  struct space *s = (struct space *)extra_data;
  struct cell *cells = (struct cell *)map_data;

  const size_t nr_cells = s->nr_cells;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double search_r2 = s->l_x2;
 
  /* Make a list of cell offsets into the top-level cell array. */
  int *const offset = s->cell_index + (ptrdiff_t)(cells - s->cells_top);

  /* Loop over cells and find which cells are in range of each other to perform
   * the FOF search. */
  for (size_t ind = 0; ind < num_elements; ind++) {
    
    /* Get the cell. */
    struct cell *restrict ci = &cells[ind];
    
    /* Only perform FOF search on local cells. */
    if(ci->nodeID == engine_rank) {

      /* Skip empty cells. */
      if(ci->gcount == 0) continue;

      /* Perform FOF search on local particles within the cell. */
      rec_fof_search_self(ci, s, dim, search_r2);

      /* Loop over all top-level cells skipping over the cells already searched.
      */
      for (size_t cjd = offset[ind] + 1; cjd < nr_cells; cjd++) {

        struct cell *restrict cj = &s->cells_top[cjd];

        /* Only perform FOF search on local cells. */
        if(cj->nodeID == engine_rank) {
          
          /* Skip empty cells. */
          if(cj->gcount == 0) continue;

          rec_fof_search_pair(ci, cj, s, dim, search_r2);
        }
      }
    }
  }
}

/**
 * @brief Search foreign cells for links and communicate any found to the appropriate node.
 *
 * @param s Pointer to a #space.
 */
void fof_search_foreign_cells(struct space *s) {

  struct cell *cells = s->cells_top;
  const size_t nr_cells = s->nr_cells;
  const double dim[3] = {s->dim[0], s->dim[1], s->dim[2]};
  const double search_r2 = s->l_x2;

  message("Searching foreign cells for links.");

  size_t nr_links = 0;
  int count = 0;

  for (size_t cid = 0; cid < nr_cells; cid++) {

    struct cell *restrict ci = &cells[cid];

    /* Skip empty cells. */
    if(ci->gcount == 0) continue;

    /* Loop over all top-level cells skipping over the cells already searched.
    */
    for (size_t cjd = cid + 1; cjd < nr_cells; cjd++) {

      struct cell *restrict cj = &cells[cjd];

      /* Only perform pair FOF search between a local and foreign cell. */
      if((ci->nodeID == engine_rank && cj->nodeID != engine_rank) || (ci->nodeID != engine_rank && cj->nodeID == engine_rank)) {
        /* Skip empty cells. */
        if(cj->gcount == 0) continue;

        const double r2 = cell_min_dist(ci, cj, dim);
        /* Assume there are only links between cells that touch. */
        if(r2 < search_r2) {
          rec_fof_search_pair_foreign_count(ci, cj, s, dim, search_r2, &nr_links);
          count++;
        }
      }
    }
  }

  message("Rank: %d, Total no. of possible links: %zu, cells touching: %d", engine_rank, nr_links, count);

  struct fof_mpi *fof_send;
  struct fof_mpi *fof_recv;
  struct cell **interface_cells;
  int interface_cell_count = 0;
  int *cell_added;
  
  if (posix_memalign((void**)&cell_added, SWIFT_STRUCT_ALIGNMENT,
                       nr_cells * sizeof(int)) != 0)
    error("Error while allocating memory for cell_added array");
  
  bzero(cell_added, nr_cells * sizeof(int));

  if (posix_memalign((void**)&fof_send, SWIFT_STRUCT_ALIGNMENT,
                       nr_links * sizeof(struct fof_mpi)) != 0)
    error("Error while allocating memory for FOF MPI communication");

  if (posix_memalign((void**)&interface_cells, SWIFT_STRUCT_ALIGNMENT,
                       count * sizeof(struct cell *)) != 0)
    error("Error while allocating memory for interface cells");
  
  size_t send_count = 0;

  char fof_filename[200] = "part_links";
  sprintf(fof_filename + strlen(fof_filename), "_%d.dat",
      engine_rank);
  
  fof_file = fopen(fof_filename, "w");
  fprintf(fof_file, "# %7s %7s %7s\n", "Local PID", "Foreign PID", "Root ID");
  fprintf(fof_file, "#-------------------------------\n");

  /* Loop over cells and find which cells are in range of each other to perform
   * the FOF search. */
  for (size_t cid = 0; cid < nr_cells; cid++) {

    struct cell *restrict ci = &cells[cid];

    /* Skip empty cells. */
    if(ci->gcount == 0) continue;

    /* Loop over all top-level cells skipping over the cells already searched.
    */
    for (size_t cjd = cid + 1; cjd < nr_cells; cjd++) {

      struct cell *restrict cj = &cells[cjd];

      /* Only perform pair FOF search between a local and foreign cell. */
      if((ci->nodeID == engine_rank && cj->nodeID != engine_rank) || (ci->nodeID != engine_rank && cj->nodeID == engine_rank)) {
        /* Skip empty cells. */
        if(cj->gcount == 0) continue;

        rec_fof_search_pair_foreign(ci, cj, s, dim, search_r2, &send_count, fof_send);

        const double r2 = cell_min_dist(ci, cj, dim);
        if(r2 < search_r2) {

          if(ci->nodeID == engine_rank && cell_added[cid] == 0) {
            interface_cells[interface_cell_count++] = ci;
            cell_added[cid] = 1;
          }
          if(cj->nodeID == engine_rank && cell_added[cjd] == 0) {
            interface_cells[interface_cell_count++] = cj;
            cell_added[cjd] = 1;
          }
        }
      }
    }
  }

  message("No. of interface cells: %d", interface_cell_count);

  if (posix_memalign((void**)&fof_recv, SWIFT_STRUCT_ALIGNMENT,
                       send_count * sizeof(struct fof_mpi)) != 0)
    error("Error while allocating memory for FOF MPI communication");

  MPI_Request send_request, recv_request;

  message("Rank: %d sending %zu links to rank %d for testing.", engine_rank, send_count, 1);

  /* Send communication of linked particles to other rank.*/
  if(engine_rank == 0) {
    MPI_Isend(fof_send, send_count, fof_mpi_type, 1, MPI_RANK_0_SEND_TAG, MPI_COMM_WORLD, &send_request);
    MPI_Irecv(fof_recv, send_count, fof_mpi_type, 1, MPI_RANK_1_SEND_TAG, MPI_COMM_WORLD, &recv_request);
  }
  else if (engine_rank == 1) {
    MPI_Isend(fof_send, send_count, fof_mpi_type, 0, MPI_RANK_1_SEND_TAG, MPI_COMM_WORLD, &send_request);
    MPI_Irecv(fof_recv, send_count, fof_mpi_type, 0, MPI_RANK_0_SEND_TAG, MPI_COMM_WORLD, &recv_request);
  }

  int res = 0, err = 0;
  MPI_Status stat;

  message("Rank: %d Testing asynchronous sends and receives", engine_rank);

  while(!res) {
    if ((err = MPI_Test(&send_request, &res, &stat)) != MPI_SUCCESS) {
      char buff[MPI_MAX_ERROR_STRING];
      int len;
      MPI_Error_string(err, buff, &len);
      error("Failed to test request on send on rank: %d, %s.",
          engine_rank, buff);
    }
  }
  
  res = 0; 
  
  while(!res) {
    if ((err = MPI_Test(&recv_request, &res, &stat)) != MPI_SUCCESS) {
      char buff[MPI_MAX_ERROR_STRING];
      int len;
      MPI_Error_string(err, buff, &len);
      error("Failed to test request on recv on rank: %d, %s.",
          engine_rank, buff);
    }
  }

  message("Rank: %d Finished testing asynchronous sends and receives", engine_rank);

  message("Rank: %d Searching received links....", engine_rank);

  for(int i=0; i<send_count; i++ ) {

    for(int j = 0; j<interface_cell_count; j++) {

      struct cell *c = interface_cells[j];

      struct gpart *gparts = c->gparts;

      /* Make a list of particle offsets into the global gparts array. */
      int *const offset = s->group_id + (ptrdiff_t)(gparts - s->gparts);

      for(int k=0; k<c->gcount; k++) {

        struct gpart *gp = &gparts[k];

        if(gp->id_or_neg_offset == fof_recv[i].foreign_pid) {

          int local_root = fof_find(offset[k], s->group_id);

          if(fof_recv[i].root_i < local_root) {

            s->group_id[local_root] = fof_recv[i].root_i;

            message("Rank: %d Particle %lld found new group with root: %d", engine_rank, gp->id_or_neg_offset, fof_recv[i].root_i);
          }
        }
      }
    }
  }
  
  message("Rank: %d Finished searching received links....", engine_rank);

  fclose(fof_file);

}

/* Perform a FOF search on gravity particles using the cells and applying the
 * Union-Find algorithm.*/
void fof_search_tree(struct space *s) {

  const size_t nr_gparts = s->nr_gparts;
  const size_t nr_cells = s->nr_cells;
  struct gpart *gparts = s->gparts;
  int *group_id;
  int *group_size;
  float *group_mass;
  int num_groups = 0;
  ticks tic = getticks();

  message("Searching %zu gravity particles for links with l_x2: %lf", nr_gparts,
          s->l_x2);

  /* Allocate and initialise array of particle group IDs. */
  if(s->group_id != NULL) free(s->group_id);

  if (posix_memalign((void **)&s->group_id, 32, nr_gparts * sizeof(int)) != 0)
    error("Failed to allocate list of particle group IDs for FOF search.");

  /* Initial group ID is particle offset into array. */
  for (size_t i = 0; i < nr_gparts; i++) s->group_id[i] = i;

  group_id = s->group_id;
  
  message("Rank: %d, Allocated group_id array of size %zu", engine_rank, s->nr_gparts);

  /* Allocate and initialise a group size array. */
  if (posix_memalign((void **)&group_size, 32, nr_gparts * sizeof(int)) != 0)
    error("Failed to allocate list of group size for FOF search.");

  /* Allocate and initialise a group mass array. */
  if (posix_memalign((void **)&group_mass, 32, nr_gparts * sizeof(float)) != 0)
    error("Failed to allocate list of group masses for FOF search.");

  bzero(group_size, nr_gparts * sizeof(int));
  bzero(group_mass, nr_gparts * sizeof(float));

  /* Activate all the regular tasks */
  threadpool_map(&s->e->threadpool, fof_search_tree_mapper, s->cells_top,
                 nr_cells, sizeof(struct cell), 1, s);

  /* Find any particle links with other nodes. */
  fof_search_foreign_cells(s);

  /* Calculate the total number of particles in each group, group mass and the total number of groups. */
  for (size_t i = 0; i < nr_gparts; i++) {
    const int root = fof_find(i, group_id);
    group_size[root]++;
    group_mass[root] += gparts[i].mass;
    if(group_id[i] == i) num_groups++;
  }

  int total_num_groups = 0;
  MPI_Reduce(&num_groups, &total_num_groups, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

  fof_dump_group_data("fof_output_tree_serial.dat", nr_gparts, group_id,
                      group_size);

  int num_parts_in_groups = 0;
  int max_group_size = 0, max_group_id = 0, max_group_mass_id = 0;
  float max_group_mass = 0;
  for (size_t i = 0; i < nr_gparts; i++) {

    /* Find the total number of particles in groups. */
    if (group_size[i] > 1) num_parts_in_groups += group_size[i];

    /* Find the largest group. */
    if (group_size[i] > max_group_size) {
      max_group_size = group_size[i];
      max_group_id = i;
    }
    
    /* Find the largest group by mass. */
    if (group_mass[i] > max_group_mass) {
      max_group_mass = group_mass[i];
      max_group_mass_id = i;
    }
  }

  message(
      "No. of groups: %d. No. of particles in groups: %d. No. of particles not "
      "in groups: %zu.",
      num_groups, num_parts_in_groups, nr_gparts - num_parts_in_groups);
  if(engine_rank == 0) message("Total number of grousp: %d", total_num_groups);
  message("Biggest group size: %d with ID: %d", max_group_size, max_group_id);
  message("Biggest group by mass: %f with ID: %d", max_group_mass, max_group_mass_id);

  free(group_size);
  free(group_mass);

  message("FOF search took: %.3f %s.",
      clocks_from_ticks(getticks() - tic), clocks_getunit());
}

/* Dump FOF group data. */
void fof_dump_group_data(char *out_file, const size_t nr_gparts, int *group_id,
                         int *group_size) {

  FILE *file = fopen(out_file, "w");
  fprintf(file, "# %7s %7s %7s\n", "ID", "Root ID", "Group Size");
  fprintf(file, "#-------------------------------\n");

  for (size_t i = 0; i < nr_gparts; i++) {
    fprintf(file, "  %7zu %7d %7d\n", i, group_id[i], group_size[i]);
  }

  fclose(file);
}
