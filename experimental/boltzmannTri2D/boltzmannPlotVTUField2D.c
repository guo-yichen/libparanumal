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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mpi.h"

#include "mesh2D.h"

// interpolate data to plot nodes and save to file (one per process
void boltzmannPlotVTUField2D(mesh2D *mesh, char *fileName){

  //mesh2D *mesh = ins->mesh;
  
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  FILE *fp;
  
  fp = fopen(fileName, "w");

  fprintf(fp, "<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" byte_order=\"BigEndian\">\n");
  fprintf(fp, "  <UnstructuredGrid>\n");
  fprintf(fp, "    <Piece NumberOfPoints=\"%d\" NumberOfCells=\"%d\">\n", 
	  mesh->Nelements*mesh->plotNp, 
	  mesh->Nelements*mesh->plotNelements);
  
  // write out nodes
  fprintf(fp, "      <Points>\n");
  fprintf(fp, "        <DataArray type=\"Float32\" NumberOfComponents=\"3\" Format=\"ascii\">\n");
  
  // compute plot node coordinates on the fly
  for(int e=0;e<mesh->Nelements;++e){
    for(int n=0;n<mesh->plotNp;++n){
      dfloat plotxn = 0, plotyn = 0;

      for(int m=0;m<mesh->Np;++m){
        plotxn += mesh->plotInterp[n*mesh->Np+m]*mesh->x[m+e*mesh->Np];
        plotyn += mesh->plotInterp[n*mesh->Np+m]*mesh->y[m+e*mesh->Np];
      }

      fprintf(fp, "       ");
      fprintf(fp, "%g %g %g\n", plotxn,plotyn,0.);
    }
  }
  
  fprintf(fp, "        </DataArray>\n");
  fprintf(fp, "      </Points>\n");
  
  // write out pressure
  fprintf(fp, "      <PointData Scalars=\"scalars\">\n");





  fprintf(fp, "        <DataArray type=\"Float32\" Name=\"Q\" NumberOfComponents=\"6\" Format=\"ascii\">\n");
  for(int e=0;e<mesh->Nelements;++e){
    for(int n=0;n<mesh->plotNp;++n){
      dfloat plotq0 = 0, plotq1 = 0, plotq2 =0, plotq3=0, plotq4 =0, plotq5=0;
      for(int m=0;m<mesh->Np;++m){
        int base = mesh->Nfields*(m + e*mesh->Np);
        dfloat q0  = mesh->q[0 + base];
        dfloat q1  = mesh->q[1 + base];
        dfloat q2  = mesh->q[2 + base];
        dfloat q3  = mesh->q[3 + base];
        dfloat q4  = mesh->q[4 + base];
        dfloat q5  = mesh->q[5 + base];
        //
        plotq0 += mesh->plotInterp[n*mesh->Np+m]*q0;
        plotq1 += mesh->plotInterp[n*mesh->Np+m]*q1;
        plotq2 += mesh->plotInterp[n*mesh->Np+m]*q2;
        plotq3 += mesh->plotInterp[n*mesh->Np+m]*q3;
        plotq4 += mesh->plotInterp[n*mesh->Np+m]*q4;
        plotq5 += mesh->plotInterp[n*mesh->Np+m]*q5;
        
      }
    
      fprintf(fp, "       ");
      fprintf(fp, "%g %g %g %g %g %g\n", plotq0, plotq1,plotq2, plotq3,plotq4, plotq5);
    }
  }
  fprintf(fp, "       </DataArray>\n");




  fprintf(fp, "     </PointData>\n");
  
  fprintf(fp, "    <Cells>\n");
  fprintf(fp, "      <DataArray type=\"Int32\" Name=\"connectivity\" Format=\"ascii\">\n");
  
  for(int e=0;e<mesh->Nelements;++e){
    for(int n=0;n<mesh->plotNelements;++n){
      fprintf(fp, "       ");
      for(int m=0;m<mesh->plotNverts;++m){
	fprintf(fp, "%d ", e*mesh->plotNp + mesh->plotEToV[n*mesh->plotNverts+m]);
      }
      fprintf(fp, "\n");
    }
  }
  
  fprintf(fp, "        </DataArray>\n");
  
  fprintf(fp, "        <DataArray type=\"Int32\" Name=\"offsets\" Format=\"ascii\">\n");
  int cnt = 0;
  for(int e=0;e<mesh->Nelements;++e){
    for(int n=0;n<mesh->plotNelements;++n){
      cnt += mesh->plotNverts;
      fprintf(fp, "       ");
      fprintf(fp, "%d\n", cnt);
    }
  }
  fprintf(fp, "       </DataArray>\n");
  
  fprintf(fp, "       <DataArray type=\"Int32\" Name=\"types\" Format=\"ascii\">\n");
  for(int e=0;e<mesh->Nelements;++e){
    for(int n=0;n<mesh->plotNelements;++n){
      fprintf(fp, "5\n");
    }
  }
  fprintf(fp, "        </DataArray>\n");
  fprintf(fp, "      </Cells>\n");
  fprintf(fp, "    </Piece>\n");
  fprintf(fp, "  </UnstructuredGrid>\n");
  fprintf(fp, "</VTKFile>\n");
  fclose(fp);

}
