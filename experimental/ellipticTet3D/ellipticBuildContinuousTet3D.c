/*

The MIT License (MIT)

Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "ellipticTet3D.h"


// compare on global indices
int parallelCompareRowColumn(const void *a, const void *b){

  nonZero_t *fa = (nonZero_t*) a;
  nonZero_t *fb = (nonZero_t*) b;

  if(fa->row < fb->row) return -1;
  if(fa->row > fb->row) return +1;

  if(fa->col < fb->col) return -1;
  if(fa->col > fb->col) return +1;

  return 0;
}

void ellipticBuildContinuousTet3D(solver_t *solver, dfloat lambda, nonZero_t **A, dlong *nnz, ogs_t **ogs, hlong *globalStarts, const char* options) {

  mesh3D *mesh = solver->mesh;

  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  /* Build a gather-scatter to assemble the global masked problem */
  dlong Ntotal = mesh->Np*mesh->Nelements;

  hlong *globalNumbering = (hlong *) calloc(Ntotal,sizeof(hlong));
  memcpy(globalNumbering,mesh->globalIds,Ntotal*sizeof(hlong)); 
  for (dlong n=0;n<solver->Nmasked;n++) 
    globalNumbering[solver->maskIds[n]] = -1;

  // squeeze node numbering
  meshParallelConsecutiveGlobalNumbering(mesh, Ntotal, globalNumbering, mesh->globalOwners, globalStarts);

  hlong *gatherMaskedBaseIds   = (hlong *) calloc(Ntotal,sizeof(hlong));
  for (dlong n=0;n<Ntotal;n++) {
    dlong id = mesh->gatherLocalIds[n];
    gatherMaskedBaseIds[n] = globalNumbering[id];
  }

  //build gather scatter with masked nodes
  int verbose = strstr(options,"VERBOSE") ? 1:0;
  *ogs = meshParallelGatherScatterSetup(mesh, Ntotal, 
                                        mesh->gatherLocalIds,  gatherMaskedBaseIds, 
                                        mesh->gatherBaseRanks, mesh->gatherHaloFlags, verbose);

  // Build non-zeros of stiffness matrix (unassembled)
  dlong nnzLocal = mesh->Np*mesh->Np*mesh->Nelements;

  nonZero_t *sendNonZeros = (nonZero_t*) calloc(nnzLocal, sizeof(nonZero_t));
  int *AsendCounts  = (int*) calloc(size, sizeof(int));
  int *ArecvCounts  = (int*) calloc(size, sizeof(int));
  int *AsendOffsets = (int*) calloc(size+1, sizeof(int));
  int *ArecvOffsets = (int*) calloc(size+1, sizeof(int));

  int *mask = (int *) calloc(mesh->Np*mesh->Nelements,sizeof(int));
  for (dlong n=0;n<solver->Nmasked;n++) mask[solver->maskIds[n]] = 1;

  //Build unassembed non-zeros
  if(rank==0) printf("Building full FEM matrix...");fflush(stdout);

  dlong cnt =0;
  #pragma omp parallel for
  for (dlong e=0;e<mesh->Nelements;e++) {

    dfloat Grr = mesh->ggeo[e*mesh->Nggeo + G00ID];
    dfloat Grs = mesh->ggeo[e*mesh->Nggeo + G01ID];
    dfloat Grt = mesh->ggeo[e*mesh->Nggeo + G02ID];
    dfloat Gss = mesh->ggeo[e*mesh->Nggeo + G11ID];
    dfloat Gst = mesh->ggeo[e*mesh->Nggeo + G12ID];
    dfloat Gtt = mesh->ggeo[e*mesh->Nggeo + G22ID];
    dfloat J   = mesh->ggeo[e*mesh->Nggeo + GWJID];

    for (int n=0;n<mesh->Np;n++) {
      if (mask[e*mesh->Np + n]) continue; //skip masked nodes
      for (int m=0;m<mesh->Np;m++) {
        if (mask[e*mesh->Np + m]) continue; //skip masked nodes
        dfloat val = 0.;

        val += Grr*mesh->Srr[m+n*mesh->Np];
        val += Grs*mesh->Srs[m+n*mesh->Np];
        val += Grt*mesh->Srt[m+n*mesh->Np];
        val += Grs*mesh->Ssr[m+n*mesh->Np];
        val += Gss*mesh->Sss[m+n*mesh->Np];
        val += Gst*mesh->Sst[m+n*mesh->Np];
        val += Grt*mesh->Str[m+n*mesh->Np];
        val += Gst*mesh->Sts[m+n*mesh->Np];
        val += Gtt*mesh->Stt[m+n*mesh->Np];
        val += J*lambda*mesh->MM[m+n*mesh->Np];

        dfloat nonZeroThreshold = 1e-7;
        if (fabs(val)>nonZeroThreshold) {
          #pragma omp critical
          {
            // pack non-zero
            sendNonZeros[cnt].val = val;
            sendNonZeros[cnt].row = globalNumbering[e*mesh->Np + n];
            sendNonZeros[cnt].col = globalNumbering[e*mesh->Np + m];
            sendNonZeros[cnt].ownerRank = mesh->globalOwners[e*mesh->Np + n];
            cnt++;
          }
        }
      }
    }
  }

  // Make the MPI_NONZERO_T data type
  MPI_Datatype MPI_NONZERO_T;
  MPI_Datatype dtype[4] = {MPI_HLONG, MPI_HLONG, MPI_INT, MPI_DFLOAT};
  int blength[4] = {1, 1, 1, 1};
  MPI_Aint addr[4], displ[4];
  MPI_Get_address ( &(sendNonZeros[0]          ), addr+0);
  MPI_Get_address ( &(sendNonZeros[0].col      ), addr+1);
  MPI_Get_address ( &(sendNonZeros[0].ownerRank), addr+2);
  MPI_Get_address ( &(sendNonZeros[0].val      ), addr+3);
  displ[0] = 0;
  displ[1] = addr[1] - addr[0];
  displ[2] = addr[2] - addr[0];
  displ[3] = addr[3] - addr[0];
  MPI_Type_create_struct (4, blength, displ, dtype, &MPI_NONZERO_T);
  MPI_Type_commit (&MPI_NONZERO_T);

  // count how many non-zeros to send to each process
  for(dlong n=0;n<cnt;++n)
    AsendCounts[sendNonZeros[n].ownerRank] += 1;

  // sort by row ordering
  qsort(sendNonZeros, cnt, sizeof(nonZero_t), parallelCompareRowColumn);

  // find how many nodes to expect (should use sparse version)
  MPI_Alltoall(AsendCounts, 1, MPI_INT, ArecvCounts, 1, MPI_INT, MPI_COMM_WORLD);

  // find send and recv offsets for gather
  *nnz = 0;
  for(int r=0;r<size;++r){
    AsendOffsets[r+1] = AsendOffsets[r] + AsendCounts[r];
    ArecvOffsets[r+1] = ArecvOffsets[r] + ArecvCounts[r];
    *nnz += ArecvCounts[r];
  }

  *A = (nonZero_t*) calloc(*nnz, sizeof(nonZero_t));

  // determine number to receive
  MPI_Alltoallv(sendNonZeros, AsendCounts, AsendOffsets, MPI_NONZERO_T,
                        (*A), ArecvCounts, ArecvOffsets, MPI_NONZERO_T,
                        MPI_COMM_WORLD);

  // sort received non-zero entries by row block (may need to switch compareRowColumn tests)
  qsort((*A), *nnz, sizeof(nonZero_t), parallelCompareRowColumn);

  // compress duplicates
  cnt = 0;
  for(dlong n=1;n<*nnz;++n){
    if((*A)[n].row == (*A)[cnt].row &&
       (*A)[n].col == (*A)[cnt].col){
      (*A)[cnt].val += (*A)[n].val;
    }
    else{
      ++cnt;
      (*A)[cnt] = (*A)[n];
    }
  }
  if (*nnz) cnt++;
  *nnz = cnt;

  if(rank==0) printf("done.\n");

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Type_free(&MPI_NONZERO_T);

  free(sendNonZeros);
  free(globalNumbering);

  free(AsendCounts);
  free(ArecvCounts);
  free(AsendOffsets);
  free(ArecvOffsets);

  free(mask);
}