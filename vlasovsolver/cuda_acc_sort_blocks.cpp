/*
 * This file is part of Vlasiator.
 * Copyright 2010-2016 Finnish Meteorological Institute
 *
 * For details of usage, see the COPYING file and read the "Rules of the Road"
 * at http://www.physics.helsinki.fi/vlasiator/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "../definitions.h"
#include "cuda_acc_sort_blocks.hpp"

// Ensure printing of CUDA runtime errors to console
#define CUB_STDERR
#include <cub/device/device_radix_sort.cuh>

using namespace std;
using namespace spatial_cell;

// Kernels for converting GIDs to dimension-sorted indices
__global__ void blocksID_mapped_dim0_kernel(
   const vmesh::VelocityMesh* vmesh,
   vmesh::GlobalID *blocksID_mapped,
   vmesh::LocalID *blocksLID_unsorted,
   const uint nBlocks
   ) {
   const int cudaBlocks = gridDim.x * gridDim.y * gridDim.z;
   const uint warpSize = blockDim.x * blockDim.y * blockDim.z;
   const int blocki = blockIdx.z*gridDim.x*gridDim.y + blockIdx.y*gridDim.x + blockIdx.x;
   const uint ti = threadIdx.z*blockDim.x*blockDim.y + threadIdx.y*blockDim.x + threadIdx.x;
   for (vmesh::LocalID index=blocki*warpSize; index<nBlocks; index += cudaBlocks*warpSize) {
      const vmesh::LocalID LID = (index+ti);
      if (LID < nBlocks) {
         blocksID_mapped[LID] = vmesh->getGlobalID(LID);
         blocksLID_unsorted[LID]=LID;
      }
   }
}

__global__ void blocksID_mapped_dim1_kernel(
   const vmesh::VelocityMesh* vmesh,
   vmesh::GlobalID *blocksID_mapped,
   vmesh::LocalID *blocksLID_unsorted,
   vmesh::LocalID D0,
   vmesh::LocalID D1,
   const uint nBlocks
   ) {
   const int cudaBlocks = gridDim.x * gridDim.y * gridDim.z;
   const uint warpSize = blockDim.x * blockDim.y * blockDim.z;
   const int blocki = blockIdx.z*gridDim.x*gridDim.y + blockIdx.y*gridDim.x + blockIdx.x;
   const uint ti = threadIdx.z*blockDim.x*blockDim.y + threadIdx.y*blockDim.x + threadIdx.x;
   for (vmesh::LocalID index=blocki*warpSize; index<nBlocks; index += cudaBlocks*warpSize) {
      const vmesh::LocalID LID = (index+ti);
      if (LID < nBlocks) {
         const vmesh::GlobalID GID = vmesh->getGlobalID(LID);
         const vmesh::LocalID x_index = GID % D0;
         const vmesh::LocalID y_index = (GID / D0) % D1;
         blocksID_mapped[LID] = GID - (x_index + y_index*D0) + y_index + x_index * D1;
         blocksLID_unsorted[LID]=LID;
      }
   }
}

__global__ void blocksID_mapped_dim2_kernel(
   const vmesh::VelocityMesh* vmesh,
   vmesh::GlobalID *blocksID_mapped,
   vmesh::LocalID *blocksLID_unsorted,
   vmesh::LocalID D0,
   vmesh::LocalID D1,
   vmesh::LocalID D2,
   const uint nBlocks
   ) {
   const int cudaBlocks = gridDim.x * gridDim.y * gridDim.z;
   const uint warpSize = blockDim.x * blockDim.y * blockDim.z;
   const int blocki = blockIdx.z*gridDim.x*gridDim.y + blockIdx.y*gridDim.x + blockIdx.x;
   const uint ti = threadIdx.z*blockDim.x*blockDim.y + threadIdx.y*blockDim.x + threadIdx.x;
   for (vmesh::LocalID index=blocki*warpSize; index<nBlocks; index += cudaBlocks*warpSize) {
      const vmesh::LocalID LID = (index+ti);
      if (LID < nBlocks) {
         const vmesh::GlobalID GID = vmesh->getGlobalID(LID);
         const vmesh::LocalID x_index = GID % D0;
         const vmesh::LocalID y_index = (GID / D0) % D1;
         const vmesh::LocalID z_index = (GID / (D0*D1));
         blocksID_mapped[LID] = z_index + y_index*D2 + x_index*D1*D2;
         blocksLID_unsorted[LID]=LID;
      }
   }
}

// LIDs are already in order.
// Now also order GIDS. (can be ridiculously parallel, minus memory access patterns)
__global__ void order_GIDs_kernel(
   const vmesh::VelocityMesh* vmesh,
   vmesh::GlobalID *blocksLID,
   vmesh::GlobalID *blocksGID,
   const uint nBlocks
   ) {
   const int cudaBlocks = gridDim.x * gridDim.y * gridDim.z;
   const uint warpSize = blockDim.x * blockDim.y * blockDim.z;
   const int blocki = blockIdx.z*gridDim.x*gridDim.y + blockIdx.y*gridDim.x + blockIdx.x;
   const uint ti = threadIdx.z*blockDim.x*blockDim.y + threadIdx.y*blockDim.x + threadIdx.x;
   for (vmesh::LocalID index=blocki*warpSize; index<nBlocks; index += cudaBlocks*warpSize) {
      const vmesh::LocalID i = (index+ti);
      if (i < nBlocks) {
         blocksGID[i]=vmesh->getGlobalID(blocksLID[i]);
      }
   }
}

__global__ void construct_columns_kernel(
   const vmesh::VelocityMesh* vmesh,
   vmesh::GlobalID *blocksID_mapped_sorted,
   ColumnOffsets* columnData,
   vmesh::LocalID DX,
   const uint nBlocks
   ) {
   const int cudaBlocks = gridDim.x * gridDim.y * gridDim.z;
   const uint warpSize = blockDim.x * blockDim.y * blockDim.z;
   // const int blocki = blockIdx.z*gridDim.x*gridDim.y + blockIdx.y*gridDim.x + blockIdx.x;
   // const uint ti = threadIdx.z*blockDim.x*blockDim.y + threadIdx.y*blockDim.x + threadIdx.x;
   if ((cudaBlocks!=1) || (warpSize!=1)) {
      printf("Error in construct_columns_kernel; unsafe warpSize or gridDim\n");
      return;
   }

   // Put in the sorted blocks, and also compute column offsets and lengths:
   columnData->columnBlockOffsets.device_push_back(0); //first offset
   columnData->setColumnOffsets.device_push_back(0); //first offset
   vmesh::LocalID prev_column_id, prev_dimension_id;

   for (vmesh::LocalID i=0; i<nBlocks; ++i) {
      // identifies a particular column
      vmesh::LocalID column_id = blocksID_mapped_sorted[i] / DX;
      // identifies a particular block in a column (along the dimension)
      vmesh::LocalID dimension_id = blocksID_mapped_sorted[i] % DX;

      if ( i > 0 &&  ( (column_id != prev_column_id) || (dimension_id != (prev_dimension_id + 1) ))) {
         //encountered new column! For i=0, we already entered the correct offset (0).
         //We also identify it as a new column if there is a break in the column (e.g., gap between two populations)
         /*add offset where the next column will begin*/
         columnData->columnBlockOffsets.device_push_back(i);
         /*add length of the current column that now ended*/
         columnData->columnNumBlocks.device_push_back(columnData->columnBlockOffsets[columnData->columnBlockOffsets.size()-1] - columnData->columnBlockOffsets[columnData->columnBlockOffsets.size()-2]);

         if (column_id != prev_column_id ){
            //encountered new set of columns, add offset to new set starting at present column
            columnData->setColumnOffsets.device_push_back(columnData->columnBlockOffsets.size() - 1);
            /*add length of the previous column set that ended*/
            columnData->setNumColumns.device_push_back(columnData->setColumnOffsets[columnData->setColumnOffsets.size()-1] - columnData->setColumnOffsets[columnData->setColumnOffsets.size()-2]);
         }
      }
      prev_column_id = column_id;
      prev_dimension_id = dimension_id;
   }

   columnData->columnNumBlocks.device_push_back(nBlocks - columnData->columnBlockOffsets[columnData->columnBlockOffsets.size()-1]);
   columnData->setNumColumns.device_push_back(columnData->columnNumBlocks.size() - columnData->setColumnOffsets[columnData->setColumnOffsets.size()-1]);
}

/*
   This function returns a sorted list of blocks in a cell.

   The sorted list is sorted according to the location, along the given dimension.
   This version uses triplets internally and also returns the LIDs of the sorted blocks.
*/
void sortBlocklistByDimension( //const spatial_cell::SpatialCell* spatial_cell,
                               vmesh::VelocityMesh* vmesh,
                               const uint dimension,
                               vmesh::GlobalID *blocksID_mapped,
                               vmesh::GlobalID *blocksID_mapped_sorted,
                               vmesh::GlobalID *blocksGID,
                               vmesh::LocalID *blocksLID_unsorted,
                               vmesh::LocalID *blocksLID,
                               ColumnOffsets* columnData,
   // split::SplitVector<uint> columnBlockOffsets; // indexes where columns start (in blocks, length totalColumns)
   // split::SplitVector<uint> columnNumBlocks; // length of column (in blocks, length totalColumns)
   // split::SplitVector<uint> setColumnOffsets; // index from columnBlockOffsets where new set of columns starts (length nColumnSets)
   // split::SplitVector<uint> setNumColumns; // how many columns in set of columns (length nColumnSets)
                               const uint cuda_async_queue_id,
                               cudaStream_t stream
   ) {

   columnData->columnBlockOffsets.clear();
   columnData->columnNumBlocks.clear();
   columnData->setColumnOffsets.clear();
   columnData->setNumColumns.clear();

   const vmesh::LocalID nBlocks = vmesh->size();
   const uint refL=0; //vAMR
   const vmesh::LocalID D0 = vmesh->getGridLength(refL)[0];
   const vmesh::LocalID D1 = vmesh->getGridLength(refL)[1];
   const vmesh::LocalID D2 = vmesh->getGridLength(refL)[2];
   vmesh::LocalID DX[3] = {D0,D1,D2};

   uint nCudaBlocks  = (nBlocks/CUDATHREADS) > CUDABLOCKS ? CUDABLOCKS : (nBlocks/CUDATHREADS);
   dim3 grid(nCudaBlocks,1,1);
   dim3 block(CUDATHREADS,1,1);

   phiprof::start("calc new dimension id");
   // Map blocks to new dimensionality
   switch( dimension ) {
      case 0: {
         blocksID_mapped_dim0_kernel<<<grid, block, 0, stream>>> (
            vmesh,
            blocksID_mapped,
            blocksLID_unsorted,
            nBlocks
            );
         break;
      }
      case 1: {
         blocksID_mapped_dim1_kernel<<<grid, block, 0, stream>>> (
            vmesh,
            blocksID_mapped,
            blocksLID_unsorted,
            D0,D1,
            nBlocks
            );
         break;
      }
      case 2: {
         blocksID_mapped_dim2_kernel<<<grid, block, 0, stream>>> (
            vmesh,
            blocksID_mapped,
            blocksLID_unsorted,
            D0,D1,D2,
            nBlocks
            );
         break;
      }
      default:
         printf("Incorrect dimension in cuda_acc_sort_blocks.cpp\n");
   }
   cudaStreamSynchronize(stream);
   phiprof::stop("calc new dimension id");

   phiprof::start("CUB sort");
   // Sort (with CUB)

   // Determine temporary device storage requirements
   void     *dev_temp_storage = NULL;
   size_t   temp_storage_bytes = 0;
   cub::DeviceRadixSort::SortPairs(dev_temp_storage, temp_storage_bytes,
                                   blocksID_mapped, blocksID_mapped_sorted,
                                   blocksLID_unsorted, blocksLID, nBlocks,
                                   0, sizeof(vmesh::GlobalID)*8, stream);
   phiprof::start("cudamallocasync");
   cudaMallocAsync((void**)&dev_temp_storage, temp_storage_bytes, stream);
   cudaStreamSynchronize(stream);
   phiprof::stop("cudamallocasync");
   printf("allocated %d bytes of temporary memory for CUB SortPairs\n",temp_storage_bytes);

   // Now sort
   cub::DeviceRadixSort::SortPairs(dev_temp_storage, temp_storage_bytes,
                                   blocksID_mapped, blocksID_mapped_sorted,
                                   blocksLID_unsorted, blocksLID, nBlocks,
                                   0, sizeof(vmesh::GlobalID)*8, stream);
   cudaStreamSynchronize(stream);
   phiprof::stop("CUB sort");

   // Gather GIDs in order
   phiprof::start("reorder GIDs");
   order_GIDs_kernel<<<grid, block, 0, stream>>> (
      vmesh,
      blocksLID,
      blocksGID,
      nBlocks
      );
   cudaStreamSynchronize(stream);
   phiprof::stop("reorder GIDs");

   phiprof::start("construct columns");
   // Construct columns. To ensure order,
   // these are done serially, but still form within a kernel.
   dim3 grid1(1,1,1);
   dim3 block1(1,1,1);
   printf("Start construct columns kernel\n");
   construct_columns_kernel<<<grid1, block1, 0, stream>>> (
      vmesh,
      blocksID_mapped_sorted,
      columnData,
      DX[dimension],
      nBlocks
      );

   cudaStreamSynchronize(stream);
   phiprof::stop("construct columns");

}
