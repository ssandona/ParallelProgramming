
#include <Timer.hpp>
#include <iostream>
#include <iomanip>

using LOFAR::NSTimer;
using std::cout;
using std::cerr;
using std::endl;
using std::fixed;
using std::setprecision;

const int HISTOGRAM_SIZE = 256;
const unsigned int B_WIDTH = 8;
const unsigned int B_HEIGHT = 4;

__global__ void histogram1DKernel(const int width, const int height, const unsigned char *inputImage, unsigned char *grayImage, unsigned int *histogram) {

    unsigned int i = blockIdx.y * blockDim.y + threadIdx.y;
    unsigned int j = blockIdx.x * blockDim.x + threadIdx.x;

    if(j >= width || i >= height) return;

    unsigned int globalIdx = threadIdx.x + (blockDim.x * threadIdx.y);

    //attention for images with sizes not multiple of 16

    //__shared__ unsigned char localImagePortion[B_WIDTH * B_HEIGHT * 3];
    __shared__ unsigned int localHistogram[HISTOGRAM_SIZE][B_WIDTH * B_HEIGHT];

    int k;

    for(k = 0; k < HISTOGRAM_SIZE; k++) {
        localHistogram[k][globalIdx]=histogram[k];

    }
    
    __syncthreads();

    float grayPix = 0.0f;
    //if(blockIdx.x >= 10) {
    float r = static_cast< float >(inputImage[(i * width) + j]);
    float g = static_cast< float >(inputImage[(width * height) + (i * width) + j]);
    float b = static_cast< float >(inputImage[(2 * width * height) + (i * width) + j]);

    grayPix = ((0.3f * r) + (0.59f * g) + (0.11f * b)) + 0.5f;
    //}
    grayImage[(i * width) + j] = static_cast< unsigned char >(grayPix);
    localHistogram[static_cast< unsigned int >(grayPix)][globalIdx]+=1;

    __syncthreads();

    int s1=0;
    int s2=0;
    int s3=0;
    int s4=0;
    int s5=0;
    int s6=0;
    int s7=0;
    int s8=0;

    for(k=0;k<HISTOGRAM_SIZE;k++){
        s1+=localHistogram[globalIdx][k];
        s2+=localHistogram[globalIdx+ B_WIDTH*B_HEIGHT][k];
        s3+=localHistogram[globalIdx+ 2*(B_WIDTH*B_HEIGHT)][k];
        s4+=localHistogram[globalIdx+ 3*(B_WIDTH*B_HEIGHT)][k];
        s5+=localHistogram[globalIdx+ 4*(B_WIDTH*B_HEIGHT)][k];
        s6+=localHistogram[globalIdx+ 5*(B_WIDTH*B_HEIGHT)][k];
        s7+=localHistogram[globalIdx+ 6*(B_WIDTH*B_HEIGHT)][k];
        s8+=localHistogram[globalIdx+ 7*(B_WIDTH*B_HEIGHT)][k];
    }
    atomicAdd((unsigned int *)&histogram[globalIdx], s1);
    atomicAdd((unsigned int *)&histogram[globalIdx+ B_WIDTH*B_HEIGHT], s2);
    atomicAdd((unsigned int *)&histogram[globalIdx+ B_WIDTH*B_HEIGHT], s3);
    atomicAdd((unsigned int *)&histogram[globalIdx+ 2*(B_WIDTH*B_HEIGHT)], s4);
    atomicAdd((unsigned int *)&histogram[globalIdx+ 3*(B_WIDTH*B_HEIGHT)], s5);
    atomicAdd((unsigned int *)&histogram[globalIdx+ 4*(B_WIDTH*B_HEIGHT)], s6);
    atomicAdd((unsigned int *)&histogram[globalIdx+ 5*(B_WIDTH*B_HEIGHT)], s7);
    atomicAdd((unsigned int *)&histogram[globalIdx+ 6*(B_WIDTH*B_HEIGHT)], s8);

}



int histogram1D(const int width, const int height, const unsigned char *inputImage, unsigned char *grayImage, unsigned int *histogram) {
    cudaError_t devRetVal = cudaSuccess;
    unsigned char *devInputImage = 0;
    unsigned char *devGrayImage = 0;
    unsigned int *devHistogram = 0;

    int pixel_numbers;

    NSTimer globalTimer("GlobalTimer", false, false);
    NSTimer kernelTimer("KernelTimer", false, false);
    NSTimer memoryTimer("MemoryTimer", false, false);


    pixel_numbers = width * height;

    // Start of the computation
    globalTimer.start();
    // Convert the input image to grayscale and make it darker
    //*outputImage = new unsigned char[pixel_numbers];

    //cout << "FUNC2\n";
    // Allocate CUDA memory
    if ( (devRetVal = cudaMalloc(reinterpret_cast< void ** >(&devInputImage), pixel_numbers * 3 * sizeof(unsigned char))) != cudaSuccess ) {
        cerr << "Impossible to allocate device memory for inputImage." << endl;
        return 1;
    }
    if ( (devRetVal = cudaMalloc(reinterpret_cast< void ** >(&devGrayImage), pixel_numbers * sizeof(unsigned char))) != cudaSuccess ) {
        cerr << "Impossible to allocate device memory for darkGrayImage." << endl;
        return 1;
    }

    if ( (devRetVal = cudaMalloc(reinterpret_cast< void ** >(&devHistogram), HISTOGRAM_SIZE * sizeof(unsigned int))) != cudaSuccess ) {
        cerr << "Impossible to allocate device memory for histogram." << endl;
        return 1;
    }




    // Copy input to device
    memoryTimer.start();
    if ( (devRetVal = cudaMemcpy(devInputImage, (void *)(inputImage), pixel_numbers * 3 * sizeof(unsigned char), cudaMemcpyHostToDevice)) != cudaSuccess ) {
        cerr << "Impossible to copy inputImage to device." << endl;
        return 1;
    }

    if ( (devRetVal = cudaMemcpy(devHistogram, (void *)(histogram), HISTOGRAM_SIZE * sizeof(unsigned int), cudaMemcpyHostToDevice)) != cudaSuccess ) {
        cerr << "Impossible to copy inputImage to device." << endl;
        return 1;
    }

    /*if ( (devRetVal = cudaMemcpy(devDarkGrayImage, reinterpret_cast< void *>(*outputImage), pixel_numbers * sizeof(unsigned char), cudaMemcpyHostToDevice)) != cudaSuccess ) {
        cerr << "Impossible to copy outputImage to device." << endl;
        return 1;
    }*/
    memoryTimer.stop();

    //cout << "FUNC4\n";
    //int grid_width = width % B_WIDTH == 0 ? width / B_WIDTH : width / B_WIDTH + 1;
    //int grid_height = height % B_HEIGHT == 0 ? height / B_HEIGHT : height / B_HEIGHT + 1;

    //cout << "Image size (w,h): (" << width << ", " << height << ")\n";
    //cout << "Grid size (w,h): (" << grid_width << ", " << grid_height << ")\n";

    unsigned int grid_width = static_cast< unsigned int >(ceil(width / static_cast< float >(B_WIDTH)));
    unsigned int grid_height = static_cast< unsigned int >(ceil(height / static_cast< float >(B_HEIGHT)));
    // Execute the kernel
    dim3 gridSize(grid_width, grid_height);
    dim3 blockSize(B_WIDTH, B_HEIGHT);

    kernelTimer.start();
    //cout << "FUNC5\n";
    histogram1DKernel <<< gridSize, blockSize >>>(width, height, devInputImage, devGrayImage, devHistogram);
    cudaDeviceSynchronize();
    kernelTimer.stop();
    //cout << "FUNC6\n";
    // Check if the kernel returned an error
    if ( (devRetVal = cudaGetLastError()) != cudaSuccess ) {
        cerr << "Uh, the kernel had some kind of issue: " << cudaGetErrorString(devRetVal) << endl;
        return 1;
    }
    //cout << "FUNC7\n";
    // Copy the output back to host
    memoryTimer.start();
    if ( (devRetVal = cudaMemcpy(reinterpret_cast< void *>(grayImage), devGrayImage, pixel_numbers * sizeof(unsigned char), cudaMemcpyDeviceToHost)) != cudaSuccess ) {
        cerr << "Impossible to copy devC to host." << endl;
        return 1;
    }
    if ( (devRetVal = cudaMemcpy(reinterpret_cast< void *>(histogram), devHistogram, HISTOGRAM_SIZE * sizeof(unsigned int), cudaMemcpyDeviceToHost)) != cudaSuccess ) {
        cerr << "Impossible to copy devC to host." << endl;
        return 1;
    }
    memoryTimer.stop();

    globalTimer.stop();
    //cout << "FUNC8\n";
    //darkGrayImage._data = outputImage;
    // Time GFLOP/s GB/s
    cout << fixed << setprecision(6) << kernelTimer.getElapsed() << setprecision(3) << " " << (static_cast< long long unsigned int >(width) * height * 6) / 1000000000.0 / kernelTimer.getElapsed() << " " << (static_cast< long long unsigned int >(width) * height * ((4 * sizeof(unsigned char)) + (1 * sizeof(unsigned int)))) / 1000000000.0 / kernelTimer.getElapsed() << endl;


    // Print the timers
    cout << "Total (s): \t" << globalTimer.getElapsed() << endl;
    cout << "Kernel (s): \t" << kernelTimer.getElapsed() << endl;
    cout << "Memory (s): \t" << memoryTimer.getElapsed() << endl;
    cout << endl;

    cudaFree(devInputImage);
    cudaFree(devGrayImage);
    return 0;
}
