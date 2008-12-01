/* 
 * cuda-hybrid-api.cu
 *
 * by Lukasz Wesolowski
 * 04.01.2008
 *
 * an interface for execution on the GPU
 *
 * description: 
 * -user enqueues one or more work requests to the work
 * request queue (wrQueue) to be executed on the GPU
 * - a converse function (gpuProgressFn) executes periodically to
 * offload work requests to the GPU one at a time
 *
 */

#include "wrqueue.h"
#include "cuda-hybrid-api.h"
#include "stdio.h"

extern void CUDACallbackManager(void *); 

/* initial size of host/device buffer arrays - dynamically expanded by
 *  the runtime system if needed
 */ 
#define NUM_BUFFERS 100
//#define GPU_DEBUG


/* work request queue */
workRequestQueue *wrQueue = NULL; 

/* The runtime system keeps track of all allocated buffers on the GPU.
 * The following arrays contain pointers to host (CPU) data and the
 * corresponding data on the device (GPU). 
 */ 

/* host buffers  */ 
void **hostBuffers = NULL; 

/* device buffers */
void **devBuffers = NULL; 

cudaStream_t kernel_stream; 
cudaStream_t data_in_stream;
cudaStream_t data_out_stream; 

extern void CUDACallbackManager(void * fn); 

/* setupData
   set up data on the gpu for this kernel's execution */
void setupData(workRequest *wr) {
  int returnVal; 
  dataInfo *bufferInfo = wr->bufferInfo; 

  if (bufferInfo != NULL) {
    for (int i=0; i<wr->nBuffers; i++) {
      int index = bufferInfo[i].bufferID; 
      int size = bufferInfo[i].size; 
      hostBuffers[index] = bufferInfo[i].hostBuffer; 
      
      /* allocate if the buffer for the corresponding index is NULL */
      if (devBuffers[index] == NULL) {
#ifdef GPU_DEBUG
	printf("buffer %d allocated\n", index); 
#endif
	returnVal = cudaMalloc((void **) &devBuffers[index], size); 
#ifdef GPU_DEBUG
	printf("cudaMalloc returned %d\n", returnVal); 
#endif
      }
      
      if (bufferInfo[i].transferToDevice) {
#ifdef GPU_DEBUG
	printf("transferToDevice bufId: %d\n", index); 
#endif

	cudaMemcpyAsync(devBuffers[index], hostBuffers[index], size, 
			cudaMemcpyHostToDevice, data_in_stream);
	/*
	cudaMemcpy(devBuffers[index], hostBuffers[index], size,
		   cudaMemcpyHostToDevice); 
	*/
      }
    }
  }
} 

/* copybackData
   transfer data from the GPU to the CPU after a work request is done */ 
void copybackData(workRequest *wr) {
  dataInfo *bufferInfo = wr->bufferInfo; 

  if (bufferInfo != NULL) {
    int nBuffers = wr->nBuffers; 
    
    for (int i=0; i<nBuffers; i++) {
      int index = bufferInfo[i].bufferID; 
      int size = bufferInfo[i].size; 
      
      if (bufferInfo[i].transferFromDevice) {
#ifdef GPU_DEBUG
	printf("transferFromDevice: %d\n", index); 
#endif

	cudaMemcpyAsync(hostBuffers[index], devBuffers[index], size,
			cudaMemcpyDeviceToHost, data_out_stream);

	/*	cudaMemcpy(hostBuffers[index], devBuffers[index],
	  size, cudaMemcpyDeviceToHost); 
	*/
      }
    }     
  }
}

void freeMemory(workRequest *wr) {
  dataInfo *bufferInfo = wr->bufferInfo;   
  int nBuffers = wr->nBuffers; 
  if (bufferInfo != NULL) {
    for (int i=0; i<nBuffers; i++) {    
      int index = bufferInfo[i].bufferID; 
      if (bufferInfo[i].freeBuffer) {
#ifdef GPU_DEBUG
	printf("buffer %d freed\n", index);
#endif 
	cudaFree(devBuffers[index]); 
	devBuffers[index] = NULL; 
      }
    }
    free(bufferInfo); 
  }
}

/* kernelSelect
 * a switch statement defined by the user to allow the library to execute
 * the correct kernel 
 */ 
void kernelSelect(workRequest *wr);

/* initHybridAPI
 *   initializes the work request queue and host/device buffer pointer arrays
 */
void initHybridAPI() {
  initWRqueue(&wrQueue);

  /* allocate host/device buffers array */
  hostBuffers = (void **) malloc(NUM_BUFFERS * sizeof(void *)); 
  devBuffers = (void **) malloc(NUM_BUFFERS * sizeof(void *)); 

  /* initialize device array to NULL */ 
  for (int i=0; i<NUM_BUFFERS; i++) {
    devBuffers[i] = NULL; 
  }
  
  cudaStreamCreate(&kernel_stream); 
  cudaStreamCreate(&data_in_stream); 
  cudaStreamCreate(&data_out_stream); 

}

/* gpuProgressFn
   called periodically to check if the current kernel has completed,
   and invoke subsequent kernel */
void gpuProgressFn() {
  int returnVal; 

  if (wrQueue == NULL) {
    return; 
  }

  while (!isEmpty(wrQueue)) {
    workRequest *wr = head(wrQueue); 
    
    if (wr->state == QUEUED) {
      setupData(wr); 
      wr->state = TRANSFERRING_IN; 
      return; 
    }  
    else if (wr->state == TRANSFERRING_IN) {

      if ((returnVal = cudaStreamQuery(data_in_stream)) == cudaSuccess) {
	kernelSelect(wr); 
	wr->state = EXECUTING; 
      }

#ifdef GPU_DEBUG
      printf("Querying memory stream returned: %d\n", returnVal);
#endif  
      
    }
    else if (wr->state == EXECUTING) {
      if ((returnVal = cudaStreamQuery(kernel_stream)) == cudaSuccess) {
        copybackData(wr);
	wr->state = TRANSFERRING_OUT;
      }
#ifdef GPU_DEBUG
      printf("Querying kernel completion returned: %d \n", returnVal);
#endif  

    }
    else if (wr->state == TRANSFERRING_OUT) {
      if (cudaStreamQuery(data_out_stream) == cudaSuccess) {
	freeMemory(wr); 
	dequeue(wrQueue);
	CUDACallbackManager(wr->callbackFn);
      }
    }
#ifdef GPU_DEBUG
    else {
      printf("Error: unrecognized state\n"); 
      return; 
    }
#endif
  }
}

/* exitHybridAPI
   cleans up and deletes memory allocated for the queue
*/
void exitHybridAPI() {
  deleteWRqueue(wrQueue); 

}
