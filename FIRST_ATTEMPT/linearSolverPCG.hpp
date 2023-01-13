
namespace libp {

namespace LinearSolver {

#define PCG_BLOCKSIZE 512

//Preconditioned Conjugate Gradient
template<typename T>
class pcg: public linearSolverBase_t<T> {
private:
  int flexible;
  
  kernel_t updatePCGKernel;

public:
  //  pcg(dlong _N, dlong _Nhalo,
  //       platform_t& _platform, settings_t& _settings, comm_t _comm);

  pcg(dlong _N, dlong _Nhalo,
      platform_t& _platform, settings_t& _settings, comm_t _comm):
    linearSolverBase_t(_N, _Nhalo, _platform, _settings, _comm) {
    
    platform.linAlg().InitKernels({"axpy", "innerProd", "norm2", "d2p", "p2d"});

    flexible = settings.compareSetting("LINEAR SOLVER", "FPCG");
    
    /* build kernels */
    properties_t kernelInfo = platform.props(); //copy base properties
    
    //add defines
    kernelInfo["defines/" "p_blockSize"] = (int)PCG_BLOCKSIZE;
    
    // combined PCG update and r.r kernel
    updatePCGKernel = platform.buildKernel(LINEARSOLVER_DIR "/okl/linearSolverUpdatePCG.okl",
					   "updatePCG", kernelInfo);
  }


  int Solve(operator_t& linearOperator, operator_t& precon,
               deviceMemory<dfloat>& o_x, deviceMemory<dfloat>& o_r,
               const dfloat tol, const int MAXIT, const int verbose) {

  int rank = comm.rank();
  linAlg_t &linAlg = platform.linAlg();

  // register scalars
  dfloat rdotz1 = 0.0;
  dfloat rdotz2 = 0.0;
  dfloat alpha = 0.0, beta = 0.0, pAp = 0.0;
  dfloat rdotr0 = 0.0;
  dfloat TOL = 0.0;

  /*Pre-reserve memory pool space to avoid some unnecessary re-sizing*/
  dlong Ntotal = N + Nhalo;
  // TW 3=>5
  platform.reserve<dfloat>(3*Ntotal + PCG_BLOCKSIZE
                           + 4 * platform.memPoolAlignment<dfloat>());

  platform.reserve<pfloat>(2*Ntotal +
                           + 4 * platform.memPoolAlignment<dfloat>());
  /*aux variables */
  deviceMemory<dfloat> o_p  = platform.reserve<dfloat>(Ntotal);
  deviceMemory<dfloat> o_z  = platform.reserve<dfloat>(Ntotal);  
  deviceMemory<dfloat> o_Ap = platform.reserve<dfloat>(Ntotal);

  deviceMemory<pfloat> o_pfloat_r  = platform.reserve<pfloat>(Ntotal);
  deviceMemory<pfloat> o_pfloat_z  = platform.reserve<pfloat>(Ntotal);
  
  // Comput norm of RHS (for stopping tolerance).
  if (settings.compareSetting("LINEAR SOLVER STOPPING CRITERION", "ABS/REL-RHS-2NORM")) {
    dfloat normb = linAlg.norm2(N, o_r, comm);
    TOL = std::max(tol*tol*normb*normb, tol*tol);
  }

  // compute A*x
  linearOperator.Operator(o_x, o_Ap);

  // subtract r = r - A*x
  linAlg.axpy(N, (dfloat)-1.f, o_Ap, (dfloat)1.f, o_r);

  rdotr0 = linAlg.norm2(N, o_r, comm);
  rdotr0 = rdotr0*rdotr0;

  if (settings.compareSetting("LINEAR SOLVER STOPPING CRITERION", "ABS/REL-INITRESID")) {
    TOL = std::max(tol*tol*rdotr0,tol*tol);
  }

  if (verbose&&(rank==0))
    printf("PCG: initial res norm %12.12f \n", sqrt(rdotr0));

  int iter;
  for(iter=0;iter<MAXIT;++iter){

    // Exit if tolerance is reached, taking at least one step.
    if (((iter == 0) && (rdotr0 == 0.0)) ||
        ((iter > 0) && (rdotr0 <= TOL))) {
      break;
    }

    // z = Precon^{-1} r
    if(sizeof(pfloat)==sizeof(dfloat)){
      precon.Operator(o_r, o_z);
    }
    else{
      linAlg.d2p(N, o_r, o_pfloat_r);
      precon.Operator(o_pfloat_r, o_pfloat_z);
      linAlg.p2d(N, o_pfloat_z, o_z);
    }

    // r.z
    rdotz2 = rdotz1;
    rdotz1 = linAlg.innerProd(N, o_r, o_z, comm);
    
    if(flexible){
      if (iter==0) {
        beta = 0.0;
      } else {
        dfloat zdotAp = linAlg.innerProd(N, o_z, o_Ap, comm);
        beta = -alpha*zdotAp/rdotz2;
      }
    } else {
      beta = (iter==0) ? 0.0 : rdotz1/rdotz2;
    }

    // p = z + beta*p
    linAlg.axpy(N, (dfloat)1.f, o_z, beta, o_p);

    // A*p
    linearOperator.Operator(o_p, o_Ap);

    // p.Ap
    pAp =  linAlg.innerProd(N, o_p, o_Ap, comm);

    alpha = rdotz1/pAp;

    //  x <= x + alpha*p
    //  r <= r - alpha*A*p
    //  dot(r,r)
    rdotr0 = UpdatePCG(alpha, o_p, o_Ap, o_x, o_r);

    if (verbose&&(rank==0)) {
      if(rdotr0<0)
        printf("WARNING CG: rdotr = %17.15lf\n", rdotr0);

      printf("CG: it %d, r norm %12.12le, alpha = %le \n", iter+1, sqrt(rdotr0), alpha);
    }
  }

  return iter;
}


  //  int Solve(operator_t& linearOperator, operator_t& precon,
  //            deviceMemory<dfloat>& o_x, deviceMemory<dfloat>& o_rhs,
  //            const dfloat tol, const int MAXIT, const int verbose);

  dfloat UpdatePCG(const dfloat alpha,
                      deviceMemory<dfloat>& o_p,
                      deviceMemory<dfloat>& o_Ap,
                      deviceMemory<dfloat>& o_x,
                      deviceMemory<dfloat>& o_r){

  // x <= x + alpha*p
  // r <= r - alpha*A*p
  // dot(r,r)
  int Nblocks = (N+PCG_BLOCKSIZE-1)/PCG_BLOCKSIZE;
  Nblocks = std::min(Nblocks, PCG_BLOCKSIZE); //limit to PCG_BLOCKSIZE entries

  //pinned tmp buffer for reductions
  pinnedMemory<dfloat> h_rdotr = platform.hostReserve<dfloat>(1);
  deviceMemory<dfloat> o_rdotr = platform.reserve<dfloat>(PCG_BLOCKSIZE);

  updatePCGKernel(N, Nblocks, o_Ap, alpha, o_r, o_rdotr);

  dfloat rdotr;
  if (Nblocks>0) {
    h_rdotr.copyFrom(o_rdotr, 1, 0, properties_t("async", true));
    platform.finish();
    rdotr = h_rdotr[0];
  } else {
    rdotr = 0.0;
  }

  // x <= x + alpha*p
  platform.linAlg().axpy(N, alpha, o_p, (dfloat)1.0, o_x);

  /*Compute allreduce while axpy is running*/
  comm.Allreduce(rdotr);
  return rdotr;
}

};
