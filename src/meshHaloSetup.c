#include <stdio.h>

#include "mesh.h"

typedef struct {
  
  dlong element, elementN;
  int face, faceN, rankN;

}facePair_t;

/* comparison function that orders halo element/face
   based on their indexes */
int compareHaloFaces(const void *a, 
                     const void *b){
  
  facePair_t *fa = (facePair_t*) a;
  facePair_t *fb = (facePair_t*) b;
  
  if(fa->rankN < fb->rankN) return -1;
  if(fa->rankN > fb->rankN) return +1;

  if(fa->elementN < fb->elementN) return -1;
  if(fa->elementN > fb->elementN) return +1;

  if(fa->faceN < fb->faceN) return -1;
  if(fa->faceN > fb->faceN) return +1;
  
  return 0;
}

// set up halo infomation for inter-processor MPI 
// exchange of trace nodes
void meshHaloSetup(mesh_t *mesh){

  // MPI info
  int rank, size;
  rank = mesh->rank;
  size = mesh->size;

  // non-blocking MPI isend/irecv requests (used in meshHaloExchange)
  mesh->haloSendRequests = calloc(size, sizeof(MPI_Request));
  mesh->haloRecvRequests = calloc(size, sizeof(MPI_Request));
  
  // count number of halo element nodes to swap
  mesh->totalHaloPairs = 0;
  mesh->NhaloPairs = (int*) calloc(size, sizeof(int));
  for(dlong e=0;e<mesh->Nelements;++e){
    for(int f=0;f<mesh->Nfaces;++f){
      int r = mesh->EToP[e*mesh->Nfaces+f]; // rank of neighbor
      if(r!=-1){
        mesh->totalHaloPairs += 1;
        mesh->NhaloPairs[r] += 1;
      }
    }
  }

  // count number of MPI messages in halo exchange
  mesh->NhaloMessages = 0;
  for(int r=0;r<size;++r)
    if(mesh->NhaloPairs[r])
      ++mesh->NhaloMessages;

  // create a list of element/faces with halo neighbor
  facePair_t *haloElements = 
    (facePair_t*) calloc(mesh->totalHaloPairs, sizeof(facePair_t));

  dlong cnt = 0;
  for(dlong e=0;e<mesh->Nelements;++e){
    for(int f=0;f<mesh->Nfaces;++f){
      dlong ef = e*mesh->Nfaces+f;
      if(mesh->EToP[ef]!=-1){
        haloElements[cnt].element  = e;
        haloElements[cnt].face     = f;
        haloElements[cnt].elementN = mesh->EToE[ef];
        haloElements[cnt].faceN    = mesh->EToF[ef];
        haloElements[cnt].rankN    = mesh->EToP[ef];

        ++cnt;
      }
    }
  }
  
  // sort the face pairs in order the destination requires
  qsort(haloElements, mesh->totalHaloPairs, sizeof(facePair_t), compareHaloFaces);

  // record the outgoing order for elements
  mesh->haloElementList = (dlong*) calloc(mesh->totalHaloPairs, sizeof(dlong));
  for(dlong i=0;i<mesh->totalHaloPairs;++i){
    dlong e = haloElements[i].element;
    mesh->haloElementList[i] = e;
  }

  // reconnect elements to ghost elements
  // (ghost elements appended to end of local element list)
  cnt = mesh->Nelements;
  for(int r=0;r<size;++r){
    for(dlong e=0;e<mesh->Nelements;++e){
      for(int f=0;f<mesh->Nfaces;++f){
        dlong ef = e*mesh->Nfaces+f;
        if(mesh->EToP[ef]==r)
          mesh->EToE[ef] = cnt++;
      }
    }
  }

  // create halo extension for x,y arrays
  dlong totalHaloNodes = mesh->totalHaloPairs*mesh->Np;
  dlong localNodes     = mesh->Nelements*mesh->Np;

  // temporary send buffer
  dfloat *sendBuffer = (dfloat*) calloc(totalHaloNodes, sizeof(dfloat));

  // extend x,y arrays to hold coordinates of node coordinates of elements in halo
  mesh->x = (dfloat*) realloc(mesh->x, (localNodes+totalHaloNodes)*sizeof(dfloat));
  mesh->y = (dfloat*) realloc(mesh->y, (localNodes+totalHaloNodes)*sizeof(dfloat));
  if(mesh->dim==3)
    mesh->z = (dfloat*) realloc(mesh->z, (localNodes+totalHaloNodes)*sizeof(dfloat));
  
  // send halo data and recv into extended part of arrays
  meshHaloExchange(mesh, mesh->Np*sizeof(dfloat), mesh->x, sendBuffer, mesh->x + localNodes);
  meshHaloExchange(mesh, mesh->Np*sizeof(dfloat), mesh->y, sendBuffer, mesh->y + localNodes);
  if(mesh->dim==3)
    meshHaloExchange(mesh, mesh->Np*sizeof(dfloat), mesh->z, sendBuffer, mesh->z + localNodes);   

  // grab EX,EY,EZ from halo
  mesh->EX = (dfloat*) realloc(mesh->EX, (mesh->Nelements+mesh->totalHaloPairs)*mesh->Nverts*sizeof(dfloat));
  mesh->EY = (dfloat*) realloc(mesh->EY, (mesh->Nelements+mesh->totalHaloPairs)*mesh->Nverts*sizeof(dfloat));
  if(mesh->dim==3)
    mesh->EZ = (dfloat*) realloc(mesh->EZ, (mesh->Nelements+mesh->totalHaloPairs)*mesh->Nverts*sizeof(dfloat));

  // send halo data and recv into extended part of arrays
  meshHaloExchange(mesh, mesh->Nverts*sizeof(dfloat), mesh->EX, sendBuffer, mesh->EX + mesh->Nverts*mesh->Nelements);
  meshHaloExchange(mesh, mesh->Nverts*sizeof(dfloat), mesh->EY, sendBuffer, mesh->EY + mesh->Nverts*mesh->Nelements);
  if(mesh->dim==3)
    meshHaloExchange(mesh, mesh->Nverts*sizeof(dfloat), mesh->EZ, sendBuffer, mesh->EZ + mesh->Nverts*mesh->Nelements);
  
  free(haloElements);
  free(sendBuffer);
}
