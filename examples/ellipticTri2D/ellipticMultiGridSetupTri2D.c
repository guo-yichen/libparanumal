#include "ellipticTri2D.h"

typedef struct{

  iint row;
  iint col;
  iint ownerRank;
  dfloat val;

}nonZero_t;

typedef struct {

  iint Nrows;
  iint Ncols;

  void **AxArgs;
  void (*Ax)(void **args, occa::memory o_x, occa::memory o_Ax);

  void **coarsenArgs;   
  void (*coarsen)(void **args, occa::memory o_x, occa::memory o_Rx);

  void **prolongateArgs;
  void (*prolongate)(void **args, occa::memory o_x, occa::memory o_Px);

  void **smootherArgs;  
  void (*smooth)(void **args, occa::memory o_r, occa::memory o_x, bool x_is_zero);

  dfloat *res;
  occa::memory *o_res;

} MGLevel;

// compare on global indices
int parallelCompareRowColumn(const void *a, const void *b);

extern "C"
{
  void dgetrf_ (int *, int *, double *, int *, int *, int *);
  void dgetri_ (int *, double *, int *, int *, double *, int *, int *);
}

void ellipticOperator2D(solver_t *solver, dfloat lambda, occa::memory &o_q, occa::memory &o_Aq, const char *options);
dfloat ellipticScaledAdd(solver_t *solver, dfloat alpha, occa::memory &o_a, dfloat beta, occa::memory &o_b);

void AxTri2D(void **args, occa::memory o_x, occa::memory o_Ax) {

  solver_t *solver = (solver_t *) args[0];
  dfloat *lambda = (dfloat *) args[1];
  char *options = (char *) args[2];

  ellipticOperator2D(solver,*lambda,o_x,o_Ax,options);
}

void coarsenTri2D(void **args, occa::memory o_x, occa::memory o_Rx) {

  solver_t *solver = (solver_t *) args[0];
  occa::memory *o_V = (occa::memory *) args[1];

  mesh_t *mesh = solver->mesh;
  precon_t *precon = solver->precon;

  precon->coarsenKernel(mesh->Nelements, *o_V, o_x, o_Rx);
}

void prolongateTri2D(void **args, occa::memory o_x, occa::memory o_Px) {

  solver_t *solver = (solver_t *) args[0];
  occa::memory *o_V = (occa::memory *) args[1];

  mesh_t *mesh = solver->mesh;
  precon_t *precon = solver->precon;

  precon->prolongateKernel(mesh->Nelements, *o_V, o_x, o_Px);
}

void smoothTri2D(void **args, occa::memory o_r, occa::memory o_x, bool xIsZero) {

  solver_t *solver = (solver_t *) args[0];
  MGLevel *level = (MGLevel *) args[1];
  
  occa::memory o_res = level->o_res;
  
  if (xIsZero) {
    //just call the smoother
    level->smooth(level->smootherArgs, o_r, o_x);
    return;
  }

  dfloat one = 1.; dfloat mone = -1.;

  //res = r-Ax
  level->Ax(level->AxArgs, o_x, o_res);
  solver->scaledAddKernel(level->Nrows, one, o_r, mone, o_res);

  //smooth the fine problem x = x + S(r-Ax)
  level->smooth(level->smootherArgs, o_res, o_res)
  solver->scaledAddKernel(level->Nrows, one, o_res, one, o_x);
}


void ellipticBuildIpdgTri2D(mesh2D *mesh, dfloat tau, dfloat lambda, iint *BCType, nonZero_t **A, iint *nnzA,
                              hgs_t **hgs, iint *globalStarts, const char *options);

void ellipticBuildContinuousTri2D(mesh2D *mesh, dfloat lambda, nonZero_t **A, iint *nnz,
                              hgs_t **hgs, iint *globalStarts, const char* options);

void ellipticBuildPatchesIpdgTri2D(mesh2D *mesh, iint basisNp, dfloat *basis,
                                   dfloat tau, dfloat lambda,
                                   iint *BCType, nonZero_t **A, iint *nnzA,
                                   hgs_t **hgs, iint *globalStarts,
                                   iint *Npataches, iint **patchesIndex, dfloat **patchesInvA, dfloat **localA,
                                   const char *options);

void ellipticCoarsePreconditionerSetupTri2D(mesh_t *mesh, precon_t *precon, dfloat tau, dfloat lambda,
                                   iint *BCType, dfloat **V1, nonZero_t **A, iint *nnzA,
                                   hgs_t **hgs, iint *globalStarts, const char *options);



void ellipticMultiGridSetupTri2D(solver_t *solver, precon_t* precon, 
                                dfloat tau, dfloat lambda, iint *BCType, 
                                const char *options, const char *parAlmondOptions) {

  void (*smoother)(void **args, occa::memory &o_r, occa::memory &o_x, bool xIsZero);

  iint rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  mesh2D *mesh = solver->mesh;

  //maually build multigrid levels
  precon->parAlmond = parAlmondInit(mesh, parAlmondOptions);

  void **AxArgs = (void **) calloc(3,sizeof(void*));
  dfloat *vlambda = (dfloat *) calloc(1,sizeof(dfloat));
  *vlambda = lambda;
  AxArgs[0] = (void *) solver;
  AxArgs[1] = (void *) vlambda;
  AxArgs[2] = (void *) options;


  //add matrix free top level
  parAlmondAddDeviceLevel(precon->parAlmond, 0,
                          mesh->Np*mesh->Nelements,
                          mesh->Np*(mesh->Nelements+mesh->totalHaloPairs),
                          AxArgs,AxTri2D,
                          NULL,NULL,
                          NULL,NULL,
                          smootherArgs,dampedJacobiTri2D);

  // coarse grid preconditioner
  nonZero_t *coarseA;
  iint nnzCoarseA;
  hgs_t *coarsehgs;
  dfloat *V1;

  iint *coarseGlobalStarts = (iint*) calloc(size+1, sizeof(iint));

  ellipticCoarsePreconditionerSetupTri2D(mesh, precon, tau, lambda, BCType,
                                         &V1, &coarseA, &nnzCoarseA,
                                         &coarsehgs, coarseGlobalStarts, options);

  iint Nnum = mesh->Nverts*(mesh->Nelements+mesh->totalHaloPairs);
  precon->o_V1  = mesh->device.malloc(mesh->Nverts*mesh->Np*sizeof(dfloat), V1);

  iint *Rows = (iint *) calloc(nnzCoarseA, sizeof(iint));
  iint *Cols = (iint *) calloc(nnzCoarseA, sizeof(iint));
  dfloat *Vals = (dfloat*) calloc(nnzCoarseA,sizeof(dfloat));

  for (iint n=0;n<nnzCoarseA;n++) {
    Rows[n] = coarseA[n].row;
    Cols[n] = coarseA[n].col;
    Vals[n] = coarseA[n].val;
  }

  void **coarsenArgs = (void **) calloc(2,sizeof(void*));
  coarsenArgs[0] = (void *) solver;
  coarsenArgs[1] = (void *) &(precon->o_V1);

  //add corasen and prolongation ops to level 1
  parAlmondAddDeviceLevel(precon->parAlmond, 1,
                          mesh->Np*mesh->Nelements,
                          mesh->Np*(mesh->Nelements+mesh->totalHaloPairs),
                          NULL,NULL,
                          coarsenArgs, coarsenTri2D,
                          coarsenArgs, prolongateTri2D,
                          NULL,NULL);

  // build amg starting at level 1
  parAlmondAgmgSetup(precon->parAlmond, 1,
                     coarseGlobalStarts,
                     nnzCoarseA,
                     Rows,
                     Cols,
                     Vals,
                     coarsehgs,
                     parAlmondOptions);


  free(coarseA); free(Rows); free(Cols); free(Vals);
}