/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2013 Stanford University and the Authors.           *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#ifdef WIN32
  #define _USE_MATH_DEFINES // Needed to get M_PI
#endif
#include "CpuPme.h"
#include "../src/SimTKUtilities/SimTKOpenMMRealType.h"
#include <cmath>
#include <smmintrin.h>

using namespace OpenMM;
using namespace std;

static const int PME_ORDER = 5;

bool CpuPme::hasInitializedThreads = false;
int CpuPme::numThreads = 0;

static float extractFloat(__m128 v, unsigned int element) {
    float f[4];
    _mm_store_ps(f, v);
    return f[element];
}

#ifdef __APPLE__
   #include <sys/sysctl.h>
   #include <dlfcn.h>
#else
   #ifdef WIN32
      #include <windows.h>
   #else
      #include <dlfcn.h>
      #include <unistd.h>
   #endif
#endif

static int getNumProcessors() {
#ifdef __APPLE__
    int ncpu;
    size_t len = 4;
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0)
       return ncpu;
    else
       return 1;
#else
#ifdef WIN32
    SYSTEM_INFO siSysInfo;
    int ncpu;
    GetSystemInfo(&siSysInfo);
    ncpu = siSysInfo.dwNumberOfProcessors;
    if (ncpu < 1)
        ncpu = 1;
    return ncpu;
#else
    long nProcessorsOnline = sysconf(_SC_NPROCESSORS_ONLN);
    if (nProcessorsOnline == -1)
        return 1;
    else
        return (int) nProcessorsOnline;
#endif
#endif
}

static void spreadCharge(float* posq, float* grid, int gridx, int gridy, int gridz, int numParticles, Vec3 periodicBoxSize) {
    float temp[4];
    __m128 boxSize = _mm_set_ps(0, (float) periodicBoxSize[2], (float) periodicBoxSize[1], (float) periodicBoxSize[0]);
    __m128 invBoxSize = _mm_set_ps(0, (float) (1/periodicBoxSize[2]), (float) (1/periodicBoxSize[1]), (float) (1/periodicBoxSize[0]));
    __m128 gridSize = _mm_set_ps(0, gridz, gridy, gridx);
    __m128 gridSizeInt = _mm_set_epi32(0, gridz, gridy, gridx);
    __m128 one  = _mm_set1_ps(1);
    __m128 scale = _mm_set1_ps(1.0f/(PME_ORDER-1));
    const float epsilonFactor = sqrt(ONE_4PI_EPS0);
    memset(grid, 0, sizeof(float)*gridx*gridy*gridz);
    for (int i = 0; i < numParticles; i++) {
        // Find the position relative to the nearest grid point.
        
        __m128 pos = _mm_load_ps(&posq[4*i]);
        __m128 posInBox = _mm_sub_ps(pos, _mm_mul_ps(boxSize, _mm_floor_ps(_mm_mul_ps(pos, invBoxSize))));
        __m128 t = _mm_mul_ps(_mm_mul_ps(posInBox, invBoxSize), gridSize);
        __m128 ti = _mm_cvttps_epi32(t);
        __m128 dr = _mm_sub_ps(t, _mm_cvtepi32_ps(ti));
        __m128 gridIndex = _mm_sub_epi32(ti, _mm_and_si128(gridSizeInt, _mm_cmpeq_epi32(ti, gridSizeInt)));
        
        // Compute the B-spline coefficients.
        
        __m128 data[PME_ORDER];
        data[PME_ORDER-1] = _mm_setzero_ps();
        data[1] = dr;
        data[0] = _mm_sub_ps(one, dr);
        for (int j = 3; j < PME_ORDER; j++) {
            __m128 div = _mm_set1_ps(1.0f/(j-1));
            data[j-1] = _mm_mul_ps(_mm_mul_ps(div, dr), data[j-2]);
            for (int k = 1; k < j-1; k++)
                data[j-k-1] = _mm_mul_ps(div, _mm_add_ps(_mm_mul_ps(_mm_add_ps(dr, _mm_set1_ps(k)), data[j-k-2]), _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(j-k), dr), data[j-k-1])));
            data[0] = _mm_mul_ps(_mm_mul_ps(div, _mm_sub_ps(one, dr)), data[0]);
        }
        data[PME_ORDER-1] = _mm_mul_ps(_mm_mul_ps(scale, dr), data[PME_ORDER-2]);
        for (int j = 1; j < (PME_ORDER-1); j++)
            data[PME_ORDER-j-1] = _mm_mul_ps(scale, _mm_add_ps(_mm_mul_ps(_mm_add_ps(dr, _mm_set1_ps(j)), data[PME_ORDER-j-2]), _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(PME_ORDER-j), dr), data[PME_ORDER-j-1])));
        data[0] = _mm_mul_ps(_mm_mul_ps(scale, _mm_sub_ps(one, dr)), data[0]);
        
        // Spread the charges.
        
        int gridIndexX = _mm_extract_epi32(gridIndex, 0);
        int gridIndexY = _mm_extract_epi32(gridIndex, 1);
        int gridIndexZ = _mm_extract_epi32(gridIndex, 2);
        float charge = epsilonFactor*posq[4*i+3];
        __m128 zdata0to3 = _mm_set_epi32(_mm_extract_ps(data[3], 2), _mm_extract_ps(data[2], 2), _mm_extract_ps(data[1], 2), _mm_extract_ps(data[0], 2));
        float zdata4 = extractFloat(data[4], 2);
        for (int ix = 0; ix < PME_ORDER; ix++) {
            int xbase = gridIndexX+ix;
            xbase -= (xbase >= gridx ? gridx : 0);
            xbase = xbase*gridy*gridz;
            float xdata = extractFloat(data[ix], 0);

            for (int iy = 0; iy < PME_ORDER; iy++) {
                int ybase = gridIndexY+iy;
                ybase -= (ybase >= gridy ? gridy : 0);
                ybase = xbase + ybase*gridz;
                float multiplier = charge*xdata*extractFloat(data[iy], 1);

                __m128 add0to3 = _mm_mul_ps(zdata0to3, _mm_set1_ps(multiplier));
                if (gridIndexZ+4 < gridz)
                    _mm_storeu_ps(&grid[ybase+gridIndexZ], _mm_add_ps(_mm_loadu_ps(&grid[ybase+gridIndexZ]), add0to3));
                else {
                    _mm_store_ps(temp, add0to3);
                    int zindex = gridIndexZ;
                    grid[ybase+zindex] += temp[0];
                    zindex++;
                    zindex -= (zindex >= gridz ? gridz : 0);
                    grid[ybase+zindex] += temp[1];
                    zindex++;
                    zindex -= (zindex >= gridz ? gridz : 0);
                    grid[ybase+zindex] += temp[2];
                    zindex++;
                    zindex -= (zindex >= gridz ? gridz : 0);
                    grid[ybase+zindex] += temp[3];
                }
                int zindex = gridIndexZ+4;
                zindex -= (zindex >= gridz ? gridz : 0);
                grid[ybase+zindex] += multiplier*zdata4;
            }
        }
    }
}

static float reciprocalEnergy(int start, int end, fftwf_complex* grid, int gridx, int gridy, int gridz, double alpha, vector<float>* bsplineModuli, Vec3 periodicBoxSize) {
    const unsigned int yzsize = gridy*gridz;
    const unsigned int zsizeHalf = gridz/2+1;
    const unsigned int yzsizeHalf = gridy*zsizeHalf;
    const float scaleFactor = (float) (M_PI*periodicBoxSize[0]*periodicBoxSize[1]*periodicBoxSize[2]);
    const float recipExpFactor = (float) (M_PI*M_PI/(alpha*alpha));
    const float invPeriodicBoxSizeX = (float) (1.0/periodicBoxSize[0]);
    const float invPeriodicBoxSizeY = (float) (1.0/periodicBoxSize[1]);
    const float invPeriodicBoxSizeZ = (float) (1.0/periodicBoxSize[2]);
    float energy = 0.0f;

    int firstz = (start == 0 ? 1 : 0);
    for (int kx = start; kx < end; kx++) {
        int mx = (kx < (gridx+1)/2) ? kx : kx-gridx;
        float mhx = mx*invPeriodicBoxSizeX;
        float bx = scaleFactor*bsplineModuli[0][kx];
        for (int ky = 0; ky < gridy; ky++) {
            int my = (ky < (gridy+1)/2) ? ky : ky-gridy;
            float mhy = my*invPeriodicBoxSizeY;
            float mhx2y2 = mhx*mhx + mhy*mhy;
            float bxby = bx*bsplineModuli[1][ky];
            for (int kz = firstz; kz < gridz; kz++) {
                int mz = (kz < (gridz+1)/2) ? kz : kz-gridz;
                float mhz = mz*invPeriodicBoxSizeZ;
                float bz = bsplineModuli[2][kz];
                float m2 = mhx2y2 + mhz*mhz;
                float denom = m2*bxby*bz;
                float eterm = exp(-recipExpFactor*m2)/denom;
                int kx1, ky1, kz1;
                if (kz >= gridz/2+1) {
                    kx1 = (kx == 0 ? kx : gridx-kx);
                    ky1 = (ky == 0 ? ky : gridy-ky);
                    kz1 = gridz-kz;
                }
                else {
                    kx1 = kx;
                    ky1 = ky;
                    kz1 = kz;
                }
                int index = kx1*yzsizeHalf + ky1*zsizeHalf + kz1;
                float gridReal = grid[index][0];
                float gridImag = grid[index][1];
                energy += eterm*(gridReal*gridReal+gridImag*gridImag);
            }
            firstz = 0;
        }
    }
    return energy;
}

static void reciprocalConvolution(int start, int end, fftwf_complex* grid, int gridx, int gridy, int gridz, double alpha, vector<float>* bsplineModuli, Vec3 periodicBoxSize) {
    const unsigned int zsize = gridz/2+1;
    const unsigned int yzsize = gridy*zsize;
    const float scaleFactor = (float) (M_PI*periodicBoxSize[0]*periodicBoxSize[1]*periodicBoxSize[2]);
    const float recipExpFactor = (float) (M_PI*M_PI/(alpha*alpha));
    const float invPeriodicBoxSizeX = (float) (1.0/periodicBoxSize[0]);
    const float invPeriodicBoxSizeY = (float) (1.0/periodicBoxSize[1]);
    const float invPeriodicBoxSizeZ = (float) (1.0/periodicBoxSize[2]);

    int firstz = (start == 0 ? 1 : 0);
    for (int kx = start; kx < end; kx++) {
        int mx = (kx < (gridx+1)/2) ? kx : kx-gridx;
        float mhx = mx*invPeriodicBoxSizeX;
        float bx = scaleFactor*bsplineModuli[0][kx];
        for (int ky = 0; ky < gridy; ky++) {
            int my = (ky < (gridy+1)/2) ? ky : ky-gridy;
            float mhy = my*invPeriodicBoxSizeY;
            float mhx2y2 = mhx*mhx + mhy*mhy;
            float bxby = bx*bsplineModuli[1][ky];
            for (int kz = firstz; kz < zsize; kz++) {
                int index = kx*yzsize + ky*zsize + kz;
                int mz = (kz < (gridz+1)/2) ? kz : kz-gridz;
                float mhz = mz*invPeriodicBoxSizeZ;
                float bz = bsplineModuli[2][kz];
                float m2 = mhx2y2 + mhz*mhz;
                float denom = m2*bxby*bz;
                float eterm = exp(-recipExpFactor*m2)/denom;
                grid[index][0] *= eterm;
                grid[index][1] *= eterm;
            }
            firstz = 0;
        }
    }
}

static void interpolateForces(int start, int end, float* posq, float* force, float* grid, int gridx, int gridy, int gridz, int numParticles, Vec3 periodicBoxSize) {
    __m128 boxSize = _mm_set_ps(0, (float) periodicBoxSize[2], (float) periodicBoxSize[1], (float) periodicBoxSize[0]);
    __m128 invBoxSize = _mm_set_ps(0, (float) (1/periodicBoxSize[2]), (float) (1/periodicBoxSize[1]), (float) (1/periodicBoxSize[0]));
    __m128 gridSize = _mm_set_ps(0, gridz, gridy, gridx);
    __m128 gridSizeInt = _mm_set_epi32(0, gridz, gridy, gridx);
    __m128 one  = _mm_set1_ps(1);
    __m128 scale = _mm_set1_ps(1.0f/(PME_ORDER-1));
    const float epsilonFactor = sqrt(ONE_4PI_EPS0);
    for (int i = start; i < end; i++) {
        // Find the position relative to the nearest grid point.
        
        __m128 pos = _mm_load_ps(&posq[4*i]);
        __m128 posInBox = _mm_sub_ps(pos, _mm_mul_ps(boxSize, _mm_floor_ps(_mm_mul_ps(pos, invBoxSize))));
        __m128 t = _mm_mul_ps(_mm_mul_ps(posInBox, invBoxSize), gridSize);
        __m128 ti = _mm_cvttps_epi32(t);
        __m128 dr = _mm_sub_ps(t, _mm_cvtepi32_ps(ti));
        __m128 gridIndex = _mm_sub_epi32(ti, _mm_and_si128(gridSizeInt, _mm_cmpeq_epi32(ti, gridSizeInt)));
        
        // Compute the B-spline coefficients.
        
        __m128 data[PME_ORDER];
        __m128 ddata[PME_ORDER];
        data[PME_ORDER-1] = _mm_setzero_ps();
        data[1] = dr;
        data[0] = _mm_sub_ps(one, dr);
        for (int j = 3; j < PME_ORDER; j++) {
            __m128 div = _mm_set1_ps(1.0f/(j-1));
            data[j-1] = _mm_mul_ps(_mm_mul_ps(div, dr), data[j-2]);
            for (int k = 1; k < j-1; k++)
                data[j-k-1] = _mm_mul_ps(div, _mm_add_ps(_mm_mul_ps(_mm_add_ps(dr, _mm_set1_ps(k)), data[j-k-2]), _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(j-k), dr), data[j-k-1])));
            data[0] = _mm_mul_ps(_mm_mul_ps(div, _mm_sub_ps(one, dr)), data[0]);
        }
        ddata[0] = _mm_sub_ps(_mm_set1_ps(0), data[0]);
        for (int j = 1; j < PME_ORDER; j++)
            ddata[j] = _mm_sub_ps(data[j-1], data[j]);
        data[PME_ORDER-1] = _mm_mul_ps(_mm_mul_ps(scale, dr), data[PME_ORDER-2]);
        for (int j = 1; j < (PME_ORDER-1); j++)
            data[PME_ORDER-j-1] = _mm_mul_ps(scale, _mm_add_ps(_mm_mul_ps(_mm_add_ps(dr, _mm_set1_ps(j)), data[PME_ORDER-j-2]), _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(PME_ORDER-j), dr), data[PME_ORDER-j-1])));
        data[0] = _mm_mul_ps(_mm_mul_ps(scale, _mm_sub_ps(one, dr)), data[0]);
        
        // Compute the force on this atom.
        
        int gridIndexX = _mm_extract_epi32(gridIndex, 0);
        int gridIndexY = _mm_extract_epi32(gridIndex, 1);
        int gridIndexZ = _mm_extract_epi32(gridIndex, 2);
        __m128 f = _mm_set1_ps(0);
        for (int ix = 0; ix < PME_ORDER; ix++) {
            int xbase = gridIndexX+ix;
            xbase -= (xbase >= gridx ? gridx : 0);
            xbase = xbase*gridy*gridz;
            float dx = extractFloat(data[ix], 0);
            float ddx = extractFloat(ddata[ix], 0);
            __m128 xdata = _mm_set_ps(0, dx, dx, ddx);

            for (int iy = 0; iy < PME_ORDER; iy++) {
                int ybase = gridIndexY+iy;
                ybase -= (ybase >= gridy ? gridy : 0);
                ybase = xbase + ybase*gridz;
                float dy = extractFloat(data[iy], 1);
                float ddy = extractFloat(ddata[iy], 1);
                __m128 xydata = _mm_mul_ps(xdata, _mm_set_ps(0, dy, ddy, dy));

                for (int iz = 0; iz < PME_ORDER; iz++) {
                    int zindex = gridIndexZ+iz;
                    zindex -= (zindex >= gridz ? gridz : 0);
                    __m128 gridValue = _mm_set1_ps(grid[ybase+zindex]);
                    float dz = extractFloat(data[iz], 2);
                    float ddz = extractFloat(ddata[iz], 2);
                    __m128 zdata = _mm_set_ps(0, ddz, dz, dz);
                    f = _mm_add_ps(f, _mm_mul_ps(xydata, _mm_mul_ps(zdata, gridValue)));
                }
            }
        }
        f = _mm_mul_ps(invBoxSize, _mm_mul_ps(gridSize, _mm_mul_ps(f, _mm_set1_ps(-epsilonFactor*posq[4*i+3]))));
        _mm_store_ps(&force[4*i], _mm_add_ps(_mm_load_ps(&force[4*i]), f));        
    }
}

class CpuPme::ThreadData {
public:
    CpuPme& owner;
    int index;
    ThreadData(CpuPme& owner, int index) : owner(owner), index(index) {
    }
};

static void* threadBody(void* args) {
    CpuPme::ThreadData& data = *reinterpret_cast<CpuPme::ThreadData*>(args);
    data.owner.runThread(data.index);
    delete &data;
    return 0;
}

CpuPme::CpuPme(int gridx, int gridy, int gridz, int numParticles, double alpha) :
        gridx(gridx), gridy(gridy), gridz(gridz), numParticles(numParticles), alpha(alpha), hasCreatedPlan(false), isFinished(false), realGrid(NULL), complexGrid(NULL) {
    if (!hasInitializedThreads) {
        numThreads = getNumProcessors();
        fftwf_init_threads();
        hasInitializedThreads = true;
    }
    // Initialize threads.
    
    pthread_cond_init(&startCondition, NULL);
    pthread_cond_init(&endCondition, NULL);
    pthread_mutex_init(&lock, NULL);
    thread.resize(numThreads);
    for (int i = 0; i < numThreads; i++) {
        ThreadData* data = new ThreadData(*this, i);
        threadData.push_back(data);
        pthread_create(&thread[i], NULL, threadBody, data);
    }

    // Initialize FFTW.
    
    realGrid = (float*) fftwf_malloc(sizeof(float)*gridx*gridy*gridz);
    complexGrid = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex)*gridx*gridy*(gridz/2+1));
    fftwf_plan_with_nthreads(numThreads);
    forwardFFT = fftwf_plan_dft_r2c_3d(gridx, gridy, gridz, realGrid, complexGrid, FFTW_MEASURE);
    backwardFFT = fftwf_plan_dft_c2r_3d(gridx, gridy, gridz, complexGrid, realGrid, FFTW_MEASURE);
    hasCreatedPlan = true;
    
    // Initialize the b-spline moduli.

    int maxSize = max(max(gridx, gridy), gridz);
    vector<double> data(PME_ORDER);
    vector<double> ddata(PME_ORDER);
    vector<double> bsplinesData(maxSize);
    data[PME_ORDER-1] = 0.0;
    data[1] = 0.0;
    data[0] = 1.0;
    for (int i = 3; i < PME_ORDER; i++) {
        double div = 1.0/(i-1.0);
        data[i-1] = 0.0;
        for (int j = 1; j < (i-1); j++)
            data[i-j-1] = div*(j*data[i-j-2]+(i-j)*data[i-j-1]);
        data[0] = div*data[0];
    }

    // Differentiate.

    ddata[0] = -data[0];
    for (int i = 1; i < PME_ORDER; i++)
        ddata[i] = data[i-1]-data[i];
    double div = 1.0/(PME_ORDER-1);
    data[PME_ORDER-1] = 0.0;
    for (int i = 1; i < (PME_ORDER-1); i++)
        data[PME_ORDER-i-1] = div*(i*data[PME_ORDER-i-2]+(PME_ORDER-i)*data[PME_ORDER-i-1]);
    data[0] = div*data[0];
    for (int i = 0; i < maxSize; i++)
        bsplinesData[i] = 0.0;
    for (int i = 1; i <= PME_ORDER; i++)
        bsplinesData[i] = data[i-1];

    // Evaluate the actual bspline moduli for X/Y/Z.

    bsplineModuli[0].resize(gridx);
    bsplineModuli[1].resize(gridy);
    bsplineModuli[2].resize(gridz);
    for (int dim = 0; dim < 3; dim++) {
        int ndata = bsplineModuli[dim].size();
        vector<float>& moduli = bsplineModuli[dim];
        for (int i = 0; i < ndata; i++) {
            double sc = 0.0;
            double ss = 0.0;
            for (int j = 0; j < ndata; j++) {
                double arg = (2.0*M_PI*i*j)/ndata;
                sc += bsplinesData[j]*cos(arg);
                ss += bsplinesData[j]*sin(arg);
            }
            moduli[i] = (float) (sc*sc+ss*ss);
        }
        for (int i = 0; i < ndata; i++)
            if (moduli[i] < 1.0e-7f)
                moduli[i] = (moduli[i-1]+moduli[i+1])*0.5f;
    }
}

CpuPme::~CpuPme() {
    if (complexGrid != NULL)
        fftwf_free(complexGrid);
    if (hasCreatedPlan) {
        fftwf_destroy_plan(forwardFFT);
        fftwf_destroy_plan(backwardFFT);
    }
    isFinished = true;
    pthread_cond_broadcast(&startCondition);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < thread.size(); i++)
        pthread_join(thread[i], NULL);
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&startCondition);
    pthread_cond_destroy(&endCondition);
}

#include <sys/time.h>
double diff(struct timeval t1, struct timeval t2) {
    return t2.tv_usec-t1.tv_usec+1e6*(t2.tv_sec-t1.tv_sec);
}

void CpuPme::runThread(int index) {
    int particleStart = (index*numParticles)/numThreads;
    int particleEnd = ((index+1)*numParticles)/numThreads;
    int gridStart = (index*gridx)/numThreads;
    int gridEnd = ((index+1)*gridx)/numThreads;
    while (!isFinished) {
        threadWait();
        if (isFinished)
            break;
        if (includeEnergy) {
            double threadEnergy = reciprocalEnergy(gridStart, gridEnd, &complexGrid[0], gridx, gridy, gridz, alpha, bsplineModuli, periodicBoxSize);
            pthread_mutex_lock(&lock);
            energy += threadEnergy;
            pthread_mutex_unlock(&lock);
            threadWait();
        }
        reciprocalConvolution(gridStart, gridEnd, complexGrid, gridx, gridy, gridz, alpha, bsplineModuli, periodicBoxSize);
        threadWait();
        interpolateForces(particleStart, particleEnd, posq, force, realGrid, gridx, gridy, gridz, numParticles, periodicBoxSize);
    }
}

void CpuPme::threadWait() {
    pthread_mutex_lock(&lock);
    waitCount++;
    pthread_cond_signal(&endCondition);
    pthread_cond_wait(&startCondition, &lock);
    pthread_mutex_unlock(&lock);
}

void CpuPme::advanceThreads() {
    pthread_mutex_lock(&lock);
    waitCount = 0;
    pthread_cond_broadcast(&startCondition);
    while (waitCount < numThreads) {
        pthread_cond_wait(&endCondition, &lock);
    }
    pthread_mutex_unlock(&lock);
}

double CpuPme::computeForceAndEnergy(float* posq, float* force, Vec3 periodicBoxSize, bool includeEnergy) {
    this->posq = posq;
    this->force = force;
    this->periodicBoxSize = periodicBoxSize;
    this->includeEnergy = includeEnergy;
    energy = 0.0;
    struct timeval t1, t2, t3, t4, t5, t6, t7;
    gettimeofday(&t1, NULL);
    spreadCharge(posq, realGrid, gridx, gridy, gridz, numParticles, periodicBoxSize);
    gettimeofday(&t2, NULL);
    fftwf_execute_dft_r2c(forwardFFT, realGrid, complexGrid);
    gettimeofday(&t3, NULL);
    double energy = 0.0;
    if (includeEnergy)
        advanceThreads(); // Signal threads to compute energy.
    gettimeofday(&t4, NULL);
    advanceThreads(); // Signal threads to perform reciprocal convolution.
    gettimeofday(&t5, NULL);
    fftwf_execute_dft_c2r(backwardFFT, complexGrid, realGrid);
    gettimeofday(&t6, NULL);
    advanceThreads(); // Signal threads to interpolate forces.
    gettimeofday(&t7, NULL);
    printf("time %g %g %g %g %g %g\n", diff(t1, t2), diff(t2, t3), diff(t3, t4), diff(t4, t5), diff(t5, t6), diff(t6, t7));
    return energy;
}
