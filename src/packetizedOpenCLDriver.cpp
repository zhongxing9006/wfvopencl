/**
 * @file   packetizedOpenCLDriver.cpp
 * @date   14.04.2010
 * @author Ralf Karrenberg
 *
 *
 * Copyright (C) 2010 Saarland University
 *
 * This file is part of packetizedOpenCLDriver.
 *
 * packetizedOpenCLDriver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * packetizedOpenCLDriver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with packetizedOpenCLDriver.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <assert.h>
#include <sstream>  // std::stringstream
#include <string.h> // memcpy
#include <sstream> // stringstream

#include <xmmintrin.h> // test output etc.

#ifdef __APPLE__
#include <OpenCL/cl_platform.h>
#include <OpenCL/cl.h>
#else
#include <CL/cl_platform.h> // e.g. for CL_API_ENTRY
#include <CL/cl.h>          // e.g. for cl_platform_id
#endif

#include "Packetizer/api.h" // packetizer
#include "llvmTools.hpp" // all LLVM functionality

#include "llvm/Analysis/LiveValues.h"

//----------------------------------------------------------------------------//
// Configuration
//----------------------------------------------------------------------------//
#define PACKETIZED_OPENCL_DRIVER_VERSION_STRING "0.1" // <major_number>.<minor_number>

#define PACKETIZED_OPENCL_DRIVER_EXTENSIONS "cl_khr_icd cl_amd_fp64 cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics cl_khr_local_int32_base_atomics cl_khr_local_int32_extended_atomics cl_khr_int64_base_atomics cl_khr_int64_extended_atomics cl_khr_byte_addressable_store cl_khr_gl_sharing cl_ext_device_fission cl_amd_device_attribute_query cl_amd_printf"
#define PACKETIZED_OPENCL_DRIVER_LLVM_DATA_LAYOUT_64 "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-f80:128:128"
#define PACKETIZED_OPENCL_DRIVER_FUNCTION_NAME_BARRIER "barrier"
#define PACKETIZED_OPENCL_DRIVER_BARRIER_SPECIAL_END_ID -1
#define PACKETIZED_OPENCL_DRIVER_BARRIER_SPECIAL_START_ID 0
#define PACKETIZED_OPENCL_DRIVER_MAX_WORK_GROUP_SIZE 8192




// these are assumed to be set by build script
//#define PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
//#define PACKETIZED_OPENCL_DRIVER_USE_OPENMP
//#define PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
//#define PACKETIZED_OPENCL_DRIVER_FORCE_ND_ITERATION_SCHEME
//#define NDEBUG
//#define PACKETIZED_OPENCL_DRIVER_USE_CLC_WRAPPER // outdated :p
//----------------------------------------------------------------------------//


#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
#include <omp.h>
#endif

#ifdef DEBUG
#define PACKETIZED_OPENCL_DRIVER_DEBUG(x) do { x } while (false)
#else
#define PACKETIZED_OPENCL_DRIVER_DEBUG(x) ((void)0)
#endif

#ifdef NDEBUG // force debug output disabled
#undef PACKETIZED_OPENCL_DRIVER_DEBUG
#define PACKETIZED_OPENCL_DRIVER_DEBUG(x) ((void)0)
#endif


//----------------------------------------------------------------------------//
// Tools
//----------------------------------------------------------------------------//

template<typename T, typename U> T ptr_cast(U* p) {
	return reinterpret_cast<T>(reinterpret_cast<size_t>(p));
}

template<typename T> void* void_cast(T* p) {
	return ptr_cast<void*>(p);
}

// helper: extract ith element of a __m128
inline float& get(const __m128& v, const unsigned idx) {
    return ((float*)&v)[idx];
}
inline unsigned& get(const __m128i& v, const unsigned idx) {
    return ((unsigned*)&v)[idx];
}
inline void printV(const __m128& v) {
	outs() << get(v, 0) << " " << get(v, 1) << " " << get(v, 2) << " " << get(v, 3);
}
inline void printV(const __m128i& v) {
	outs() << get(v, 0) << " " << get(v, 1) << " " << get(v, 2) << " " << get(v, 3);
}

//----------------------------------------------------------------------------//


#ifdef __cplusplus
extern "C" {
#endif


#define CL_CONSTANT 0x3 // does not exist in specification 1.0
#define CL_PRIVATE 0x4 // does not exist in specification 1.0

///////////////////////////////////////////////////////////////////////////
//                 OpenCL Runtime Implementation                         //
///////////////////////////////////////////////////////////////////////////

// a class 'OpenCLRuntime' would be nicer,
// but we cannot store address of member function to bitcode
namespace {

#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	static const cl_uint numCores = 4; // TODO: determine somehow, omp_get_num_threads() is dynamic (=1 here)
	size_t** currentGlobal; // 0 -> globalThreads[D] -1  (packetized: 0->globalThreads[D]/4 if D == simd dim)
	size_t** currentGroup;  // 0 -> (globalThreads[D] / localThreads[D]) -1
#else
	static const cl_uint numCores = 1;
	size_t* currentGlobal; // 0 -> globalThreads[D] -1  (packetized: 0->globalThreads[D]/4 if D == simd dim)
	size_t* currentGroup;  // 0 -> (globalThreads[D] / localThreads[D]) -1
#endif

	static const cl_uint simdWidth = 4;
	static const cl_uint maxNumThreads = numCores;

	static const cl_uint maxNumDimensions = 3;

	cl_uint dimensions;
	size_t* globalThreads; // total # work items per dimension, arbitrary size
	size_t* localThreads;  // size of each work group per dimension


	/* Num. of dimensions in use */
	inline cl_uint get_work_dim() {
		return dimensions;
	}

	/* Num. of global work-items */
	inline size_t get_global_size(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return 1;
		return globalThreads[D];
	}

	/* Global work-item ID value */
	inline size_t get_global_id(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return 0;
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		return currentGlobal[thread][D];
#else
		return currentGlobal[D];
#endif
	}

	/* Num. of local work-items */
	inline size_t get_local_size(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return 1;
		return localThreads[D];
	}

	/* Num. of work-groups */
	inline size_t get_num_groups(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return 1;
		const size_t num_groups = globalThreads[D] / localThreads[D];
		return num_groups > 0 ? num_groups : 1; // there is at least one group ;)
	}

	/* Returns the work-group ID */
	inline size_t get_group_id(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return CL_SUCCESS;
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		return currentGroup[thread][D];
#else
		return currentGroup[D];
#endif
	}

	inline void setCurrentGlobal(cl_uint D, size_t id) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  setCurrentGlobal(" << D << ", " << id << ")  ; (global size = " << get_global_size(D) << ")\n"; );
		assert (D < dimensions);
		assert (id < get_global_size(D));
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		currentGlobal[thread][D] = id;
#else
		currentGlobal[D] = id;
#endif
	}
	inline void setCurrentGroup(cl_uint D, size_t id) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  setCurrentGroup(" << D << ", " << id << ")  ; (# groups = " << get_num_groups(D) << ")\n"; );
		assert (D < dimensions);
		assert (id < get_num_groups(D));
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		currentGroup[thread][D] = id;
#else
		currentGroup[D] = id;
#endif
	}

	/*
	 * CLK_LOCAL_MEM_FENCE - The barrier function
	 * will either flush any variables stored in local memory
	 * or queue a memory fence to ensure correct ordering of
	 * memory operations to local memory.
	 * CLK_GLOBAL_MEM_FENCE – The barrier function
	 * will queue a memory fence to ensure correct ordering
	 * of memory operations to global memory. This can be
	 * useful when work-items, for example, write to buffer or
	 * image objects and then want to read the updated data.
	 */
	// TODO: no idea what the parameters do :P
	void barrier(unsigned a, unsigned b) {
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		#pragma omp barrier
		outs() << "#threads: " << omp_get_num_threads() << "\n";
#else
		// barrier does not do anything if no openmp is activated :P
#endif
	}


#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION

	// scalar implementation
	//

#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	size_t** currentLocal;  // 0 -> SIMD width -1
#else
	size_t* currentLocal;  // 0 -> SIMD width -1
#endif

	/* Local work-item ID */
	inline size_t get_local_id(cl_uint D) {
		assert (D < dimensions);
		if (D >= dimensions) return 0;
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		return currentLocal[thread][D];
#else
		return currentLocal[D];
#endif
	}

	inline void setCurrentLocal(cl_uint D, size_t id) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  setCurrentLocal(" << D << ", " << id << ")  ; (local size = " << get_local_size(D) << ")\n"; );
		assert (D < dimensions);
		assert (id < get_local_size(D));
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		currentLocal[thread][D] = id;
#else
		currentLocal[D] = id;
#endif
	}

	// called automatically by initializeOpenCL
	inline cl_uint initializeThreads(const size_t* gThreads, const size_t* lThreads) {
		for (cl_uint i=0; i<dimensions; ++i) {
			PACKETIZED_OPENCL_DRIVER_DEBUG(
				if (lThreads[i] > gThreads[i]) {
					errs() << "WARNING: local work size is larger than global work size for dimension " << i << "!\n";
				}
			);

			globalThreads[i] = gThreads[i];
			localThreads[i] = lThreads[i];
		}
		return CL_SUCCESS;
	}
	// simdDim is ignored here
	cl_uint initializeOpenCL(const cl_uint num_dims, const cl_uint simdDim, const size_t* gThreads, const size_t* lThreads) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nAutomatic Packetization disabled!\n"; );

		dimensions = num_dims;

		globalThreads = new size_t[num_dims]();
		localThreads = new size_t[num_dims]();

#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "OpenMP enabled!\n"; );
		currentGlobal = new size_t*[maxNumThreads]();
		currentLocal = new size_t*[maxNumThreads]();
		currentGroup = new size_t*[maxNumThreads]();

		for (cl_uint i=0; i<maxNumThreads; ++i) {
			currentGlobal[i] = new size_t[num_dims]();
			currentLocal[i] = new size_t[num_dims]();
			currentGroup[i] = new size_t[num_dims]();

			for (cl_uint j=0; j<num_dims; ++j) {
				currentGlobal[i][j] = 0;
				currentLocal[i][j] = 0;
				currentGroup[i][j] = 0;
			}
		}
#else
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "OpenMP disabled!\n"; );
		currentGlobal = new size_t[num_dims]();
		currentLocal = new size_t[num_dims]();
		currentGroup = new size_t[num_dims]();

		for (cl_uint i=0; i<num_dims; ++i) {
			currentGlobal[i] = 0;
			currentLocal[i] = 0;
			currentGroup[i] = 0;
		}
#endif

		return initializeThreads(gThreads, lThreads);
	}

#else

	// packetized implementation
	//

#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	__m128i** currentLocal;  // 0 -> SIMD width -1 (always 0 for non-simd dims)
#else
	__m128i* currentLocal;  // 0 -> SIMD width -1 (always 0 for non-simd dims)
#endif

	cl_uint simdDimension;

	inline __m128i get_global_id_SIMD(cl_uint D) {
		assert (D < dimensions);
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		const size_t simd_id = currentGlobal[thread][D];
#else
		const size_t simd_id = currentGlobal[D];
#endif
		const unsigned id0 = simd_id * 4;
		const unsigned id1 = id0 + 1;
		const unsigned id2 = id0 + 2;
		const unsigned id3 = id0 + 3;
		return _mm_set_epi32(id3, id2, id1, id0);
	}
	inline __m128i get_local_id_SIMD(cl_uint D) {
		assert (D < dimensions);
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		return currentLocal[thread][D];
#else
		return currentLocal[D];
#endif
	}

	inline void setCurrentLocal(cl_uint D, __m128i id) {
		assert (D < dimensions);
		assert (((unsigned*)&id)[0] < get_local_size(D));
		assert (((unsigned*)&id)[1] < get_local_size(D));
		assert (((unsigned*)&id)[2] < get_local_size(D));
		assert (((unsigned*)&id)[3] < get_local_size(D));
#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		const unsigned thread = omp_get_thread_num();
		assert (thread < maxNumThreads);
		currentLocal[thread][D] = id;
#else
		currentLocal[D] = id;
#endif
	}

	// called automatically by initializeOpenCL
	inline cl_uint initializeThreads(const size_t* gThreads, const size_t* lThreads) {
		
		// set up global/local thread numbers
		for (cl_uint i=0; i<dimensions; ++i) {
			PACKETIZED_OPENCL_DRIVER_DEBUG(
				if (lThreads[i] > gThreads[i]) {
					errs() << "WARNING: local work size is larger than global work size for dimension " << i << "!\n";
				}
			);

			const size_t globalThreadsDimI = gThreads[i];
			const size_t localThreadsDimI = lThreads[i] < simdWidth ? simdWidth : lThreads[i];

			globalThreads[i] = globalThreadsDimI;
			localThreads[i] = localThreadsDimI;
		}

		// safety checks
		PACKETIZED_OPENCL_DRIVER_DEBUG(
			size_t globalThreadNum = 0;
			size_t localThreadNum = 0;
			bool* alignedGlobalDims = new bool[dimensions]();
			bool* alignedLocalDims = new bool[dimensions]();

			bool error = false;
			for (cl_uint i=0; i<dimensions; ++i) {
				const size_t globalThreadsDimI = gThreads[i];
				const size_t localThreadsDimI = lThreads[i] < simdWidth ? simdWidth : lThreads[i];

				globalThreadNum += globalThreadsDimI;
				alignedGlobalDims[i] = (globalThreadsDimI % simdWidth == 0);

				localThreadNum += localThreadsDimI;
				alignedLocalDims[i] = (localThreadsDimI % simdWidth == 0);

				if (lThreads[i] > simdWidth) errs() << "WARNING: local work size (" << lThreads[i] << ") is larger than " << simdWidth << "!\n";
				if (lThreads[i] < simdWidth) {
					// TODO: fall back to scalar mode instead!
					errs() << "WARNING: local work size enlarged from " << lThreads[i] << " to " << simdWidth << "!\n";
				}


				if (i == simdDimension && !alignedGlobalDims[i]) {
					errs() << "ERROR: size of chosen SIMD dimension " << i
							<< " is globally not dividable by " << simdWidth
							<< " (global dimension)!\n";
					error = true;
				}
				if (i == simdDimension && !alignedLocalDims[i]) {
					errs() << "ERROR: size of chosen SIMD dimension " << i
							<< " is locally not dividable by " << simdWidth
							<< " (work-group dimension)!\n";
					error = true;
				}
				if (globalThreadsDimI % localThreadsDimI != 0) {
					errs() << "ERROR: size of global dimension " << i
							<< " not dividable by local dimension ("
							<< globalThreadsDimI << " / " << localThreadsDimI
							<< ")!\n";
					error = true;
				}
			}

			if (globalThreadNum % simdWidth != 0) {
				errs() << "ERROR: global number of threads is not dividable by "
						<< simdWidth << "!\n";
				error = true;
			}
			if (localThreadNum % simdWidth != 0) {
				errs() << "ERROR: number of threads in a group is not dividable by "
						<< simdWidth << "!\n";
				error = true;
			}

			delete [] alignedGlobalDims;
			delete [] alignedLocalDims;

			if (error) return CL_INVALID_GLOBAL_WORK_SIZE;
		);

		return CL_SUCCESS;
	}
	// simdDim ranges from 0 to num_dims-1 !
	cl_uint initializeOpenCL(const cl_uint num_dims, const cl_uint simdDim, const size_t* gThreads, const size_t* lThreads) {
		// print some information
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nAutomatic Packetization enabled!\n"; );

		dimensions = num_dims;
		simdDimension = simdDim;

		globalThreads = new size_t[num_dims]();
		localThreads = new size_t[num_dims]();

#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "OpenMP enabled!\n"; );
		currentGlobal = new size_t*[maxNumThreads]();
		currentLocal = new __m128i*[maxNumThreads]();
		currentGroup = new size_t*[maxNumThreads]();

		for (cl_uint i=0; i<maxNumThreads; ++i) {
			currentGlobal[i] = new size_t[num_dims]();
			currentLocal[i] = new __m128i[num_dims]();
			currentGroup[i] = new size_t[num_dims]();

			for (cl_uint j=0; j<num_dims; ++j) {
				if (j == simdDimension) {
					currentGlobal[i][j] = 0;
					currentLocal[i][j] = _mm_set_epi32(0, 1, 2, 3);
				} else {
					currentGlobal[i][j] = 0;
					currentLocal[i][j] = _mm_set_epi32(0, 0, 0, 0);
				}
				currentGroup[i][j] = 0;
			}
		}
#else
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "OpenMP disabled!\n"; );
		currentGlobal = new size_t[num_dims]();
		currentLocal = new __m128i[num_dims]();
		currentGroup = new size_t[num_dims]();

		for (cl_uint i=0; i<num_dims; ++i) {
			if (i == simdDimension) {
				currentGlobal[i] = 0;
				currentLocal[i] = _mm_set_epi32(3, 2, 1, 0);
			} else {
				currentGlobal[i] = 0;
				currentLocal[i] = _mm_set_epi32(0, 0, 0, 0);
			}
			currentGroup[i] = 0;
		}
#endif

		return initializeThreads(gThreads, lThreads);
	}


	bool __packetizeKernelFunction(
			const std::string& kernelName,
			const std::string& targetKernelName,
			llvm::Module* mod,
			const cl_uint packetizationSize,
			const bool use_sse41,
			const bool verbose)
	{
		if (!PacketizedOpenCLDriver::getFunction(kernelName, mod)) {
			errs() << "ERROR: source function '" << kernelName
					<< "' not found in module!\n";
			return false;
		}
		if (!PacketizedOpenCLDriver::getFunction(targetKernelName, mod)) {
			errs() << "ERROR: target function '" << targetKernelName
					<< "' not found in module!\n";
			return false;
		}

		Packetizer::Packetizer* packetizer = Packetizer::getPacketizer(use_sse41, verbose);
		Packetizer::addFunctionToPacketizer(
				packetizer,
				kernelName,
				targetKernelName,
				packetizationSize);

		Packetizer::addNativeFunctionToPacketizer(
				packetizer,
				"get_global_id",
				-1,
				PacketizedOpenCLDriver::getFunction("get_global_id", mod),
				true); // although call does not return packet, packetize everything that depends on it!
		Packetizer::addNativeFunctionToPacketizer(
				packetizer,
				"get_global_id_split",
				-1,
				PacketizedOpenCLDriver::getFunction("get_global_id_SIMD", mod),
				true); // packetization is mandatory
		Packetizer::addNativeFunctionToPacketizer(
				packetizer,
				"get_local_id",
				-1,
				PacketizedOpenCLDriver::getFunction("get_local_id_SIMD", mod),
				true); // packetization is mandatory

		Packetizer::runPacketizer(packetizer, mod);

		if (!PacketizedOpenCLDriver::getFunction(targetKernelName, mod)) {
			errs() << "ERROR: packetized target function not found in"
					"module!\n";
			return false;
		}

		return true;
	}

#endif



}

// common functionality, with or without packetization/openmp
namespace PacketizedOpenCLDriver {

	//------------------------------------------------------------------------//
	// LLVM tools
	//------------------------------------------------------------------------//
	void replaceCallbacksByArgAccess(Function* f, Value* arg, Function* source) {
		if (!f) return;
		assert (arg && source);

		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "replaceCallbacksByArgAccess(" << f->getNameStr() << ", " << *arg << ", " << source->getName() << ")\n"; );

		const bool isArrayArg = isa<ArrayType>(arg->getType());
		const bool isPointerArg = isa<PointerType>(arg->getType());
		
		for (Function::use_iterator U=f->use_begin(), UE=f->use_end(); U!=UE; ) {
			if (!isa<CallInst>(U)) continue;
			CallInst* call = cast<CallInst>(U++);

			if (call->getParent()->getParent() != source) continue; // ignore uses in other functions

			// if arg type is an array, check second operand of call (= supplied parameter)
			// and generate appropriate ExtractValueInst
			if (isArrayArg) {
				PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  array arg found!\n"; );
				const Value* dimVal = call->getOperand(1);
				assert (isa<ConstantInt>(dimVal));
				const ConstantInt* dimConst = cast<ConstantInt>(dimVal);
				const uint64_t* dimension = dimConst->getValue().getRawData();
				ExtractValueInst* ev = ExtractValueInst::Create(arg, *dimension, "", call);
				arg = ev;
				PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  new extract: " << *arg << "\n"; );
				
				// if the result is a 64bit integer value, truncate to 32bit -> more other problems :/
				//if (ev->getType() == f->getReturnType()) arg = ev;
				//else arg = TruncInst::CreateTruncOrBitCast(ev, f->getReturnType(), "", call);
				//outs() << "  new extract/cast: " << *arg << "\n";
			} else if (isPointerArg) {
				PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  pointer arg found!\n"; );
				Value* dimVal = call->getOperand(1);
				GetElementPtrInst* gep = GetElementPtrInst::Create(arg, dimVal, "", call);
				LoadInst* load = new LoadInst(gep, "", call);
				arg = load;
				PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  new gep: " << *gep << "\n"; );
				PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  new load: " << *load << "\n"; );
			}
		
			assert (f->getReturnType() == arg->getType());
			
			call->replaceAllUsesWith(arg);
			call->eraseFromParent();
		}
	}

	inline llvm::Function* generateKernelWrapper(
			const std::string& wrapper_name,
			llvm::Function* f_SIMD,
			llvm::Module* mod)
	{
		assert (f_SIMD && mod);
#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
		return PacketizedOpenCLDriver::generateFunctionWrapper(wrapper_name, f_SIMD, mod);
#else

		LLVMContext& context = mod->getContext();

		// analyze function for callbacks (get_global_id etc.)
		// TODO: Not necessary, as there currently is no efficient way to use
		//       this knowledge: We have to supply all arguments every time to
		//       retain the same function signature of the wrapper for all kernels.
		#if 0
		for (llvm::Function::const_iterator BB=f_SIMD->begin(), BBE=f_SIMD->end(); BB!=BBE; ++BB) {
			for (llvm::BasicBlock::const_iterator I=BB->begin(), IE=BB->end(); I!=IE; ++I) {
				if (!llvm::isa<llvm::CallInst>(I)) continue;

				llvm::CallInst* call = llvm::cast<llvm::CallInst>(I);
				
				llvm::Function* callee = call->getCalledFunction();
				if (callee->getNameStr() == "get_work_dim") {
					// ...
				} // else if ...

			}
		}
		#endif

		// collect return types of the callback functions of interest
		std::vector<const llvm::Type*> additionalParams;
		additionalParams.push_back(Type::getInt32Ty(context)); // get_work_dim = cl_uint

//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_global_size = size_t[]
//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_global_id = size_t[]
//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_local_size = size_t[]
//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_num_groups = size_t[]
//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_group_id = size_t[]
//#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
//		additionalParams.push_back(ArrayType::get(Type::getInt32Ty(context), maxNumDimensions)); // get_local_id = size_t[]
//#else
//		additionalParams.push_back(ArrayType::get(VectorType::get(Type::getInt32Ty(context), simdWidth), maxNumDimensions)); // get_global_id_SIMD = __m128i[]
//		additionalParams.push_back(ArrayType::get(VectorType::get(Type::getInt32Ty(context), simdWidth), maxNumDimensions)); // get_local_id_SIMD = __m128i[]
//#endif

		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_global_size = size_t[]
		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_global_id = size_t[]
		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_local_size = size_t[]
		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_num_groups = size_t[]
		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_group_id = size_t[]
#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
		additionalParams.push_back(Type::getInt32PtrTy(context, 0)); // get_local_id = size_t[]
#else
		additionalParams.push_back(PointerType::getUnqual(VectorType::get(Type::getInt32Ty(context), simdWidth))); // get_global_id_SIMD = __m128i[]
		additionalParams.push_back(PointerType::getUnqual(VectorType::get(Type::getInt32Ty(context), simdWidth))); // get_local_id_SIMD = __m128i[]
#endif

		// generate wrapper
		llvm::Function* wrapper = PacketizedOpenCLDriver::generateFunctionWrapperWithParams(wrapper_name, f_SIMD, mod, additionalParams);

		// set argument names and attributes
		Function::arg_iterator arg = wrapper->arg_begin();
		++arg; arg->setName("get_work_dim");
		++arg; arg->setName("get_global_size");
		++arg; arg->setName("get_global_id");
		++arg; arg->setName("get_local_size");
		++arg; arg->setName("get_num_groups");
		++arg; arg->setName("get_group_id");
#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
		++arg; arg->setName("get_local_id");
#else
		++arg; arg->setName("get_global_id_SIMD");
		++arg; arg->setName("get_local_id_SIMD");
#endif

		return wrapper;
#endif
	}

	inline void resolveRuntimeCalls(llvm::Module* mod) {
		std::vector< std::pair<llvm::Function*, void*> > funs;
		using std::make_pair;
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_work_dim",    mod), void_cast(get_work_dim)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_global_size", mod), void_cast(get_global_size)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_global_id",   mod), void_cast(get_global_id)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_local_size",  mod), void_cast(get_local_size)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_num_groups",  mod), void_cast(get_num_groups)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_group_id",    mod), void_cast(get_group_id)));

#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_local_id", mod), void_cast(get_local_id)));
#else
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_global_id_SIMD", mod), void_cast(get_global_id_SIMD)));
		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("get_local_id_SIMD",  mod), void_cast(get_local_id_SIMD)));
#endif

		funs.push_back(make_pair(PacketizedOpenCLDriver::getFunction("barrier", mod), void_cast(barrier)));

		for (cl_uint i=0, e=funs.size(); i<e; ++i) {
			llvm::Function* funDecl = funs[i].first;
			void* funImpl = funs[i].second;

			if (funDecl) PacketizedOpenCLDriver::replaceAllUsesWith(funDecl, PacketizedOpenCLDriver::createFunctionPointer(funDecl, funImpl));
		}
	}
	inline void fixFunctionNames(Module* mod) {
		// fix __sqrt_f32
		if (PacketizedOpenCLDriver::getFunction("__sqrt_f32", mod)) {

			// create llvm.sqrt.f32 intrinsic
			const llvm::Type* floatType = PacketizedOpenCLDriver::getTypeFromString(mod, "f");
			std::vector<const llvm::Type*> params;
			params.push_back(floatType);
			PacketizedOpenCLDriver::createExternalFunction("llvm.sqrt.f32", floatType, params, mod);
			assert (PacketizedOpenCLDriver::getFunction("llvm.sqrt.f32", mod));

			PacketizedOpenCLDriver::replaceAllUsesWith(PacketizedOpenCLDriver::getFunction("__sqrt_f32", mod), PacketizedOpenCLDriver::getFunction("llvm.sqrt.f32", mod));
		}
	}

	inline cl_uint convertLLVMAddressSpace(cl_uint llvm_address_space) {
		switch (llvm_address_space) {
			case 0 : return CL_PRIVATE;
			case 1 : return CL_GLOBAL;
			case 3 : return CL_LOCAL;
			default : return llvm_address_space;
		}
	}
	inline std::string getAddressSpaceString(cl_uint cl_address_space) {
		switch (cl_address_space) {
			case CL_GLOBAL: return "CL_GLOBAL";
			case CL_PRIVATE: return "CL_PRIVATE";
			case CL_LOCAL: return "CL_LOCAL";
			case CL_CONSTANT: return "CL_CONSTANT";
			default: return "";
		}
	}

	// returns the new function that is called at the point of the barrier
	Function* eliminateBarrier(CallInst* barrier, const FunctionType* fTypeNew, const std::string& newFunName) {
		assert (barrier);
		BasicBlock* parentBlock = barrier->getParent();
		assert (parentBlock);
		Function* f = parentBlock->getParent();
		assert (f);
		Module* mod = f->getParent();
		assert (mod);

		LLVMContext& context = mod->getContext();

		//--------------------------------------------------------------------//
		// split block at the position of the barrier
		//--------------------------------------------------------------------//
		//BasicBlock* newBlock = parentBlock->splitBasicBlock(barrier, parentBlock->getNameStr()+".barrier");

		//--------------------------------------------------------------------//
		// do live value analysis
		//--------------------------------------------------------------------//
		//FunctionPassManager Passes(mod);
		//LiveValues* lvPass = createLiveValuesPass();

		//Passes.add(new TargetData(mod));
		//Passes.add(lvPass);

		//funPassManager->doInitialization();
		//funPassManager->run(f);
		//funPassManager->doFinalization();

		//lvPass->isKilledInBlock(val, block);
		//lvPass->isLiveThroughBlock(val, block);
		//lvPass->isUsedInBlock(val, block);

		//--------------------------------------------------------------------//
		// create struct with live-in values of newBlock
		//--------------------------------------------------------------------//

		//--------------------------------------------------------------------//
		// create new function that takes struct as argument and returns barrier id
		//--------------------------------------------------------------------//

		Function* continuation = Function::Create(fTypeNew, Function::ExternalLinkage, newFunName, mod); // TODO: check linkage type


		//--------------------------------------------------------------------//
		// copy all blocks 'below' parentBlock inside the new function (DFS)
		//--------------------------------------------------------------------//

		//--------------------------------------------------------------------//
		// delete the edge from parentBlock to newBlock (there is none as long
		// as we did not generate a branch ourselves)
		//--------------------------------------------------------------------//

		//--------------------------------------------------------------------//
		// (dead code elimination should remove newBlock and all blocks below
		// that are dead.)
		//--------------------------------------------------------------------//

		//--------------------------------------------------------------------//
		// create call to new function
		//--------------------------------------------------------------------//

		//--------------------------------------------------------------------//
		// create return that returns the result of the call
		//--------------------------------------------------------------------//


		// temporary: just delete it to be able to test ^^ and generate dummy return
		if (!barrier->use_empty()) barrier->replaceAllUsesWith(Constant::getNullValue(barrier->getType()));
		barrier->eraseFromParent();
		IRBuilder<> builder(context);
		BasicBlock* entryBB = BasicBlock::Create(context, "entry", continuation);
		builder.SetInsertPoint(entryBB);
		builder.CreateRet(ConstantInt::get(fTypeNew->getReturnType(), 1, true));

		return continuation;
	}

	Function* eliminateBarriers(Function* f) {
		assert (f);
		assert (f->getReturnType()->isVoidTy());
		Module* mod = f->getParent();
		assert (mod);
		LLVMContext& context = mod->getContext();

		//--------------------------------------------------------------------//
		// check how many barriers the function has
		//--------------------------------------------------------------------//
		unsigned numBarriers = 0;
		for (Function::iterator BB=f->begin(), BBE=f->end(); BB!=BBE; ++BB) {
			for (BasicBlock::iterator I=BB->begin(), IE=BB->end(); I!=IE; ++I) {
				if (!isa<CallInst>(I)) continue;
				CallInst* call = cast<CallInst>(I);

				const Function* callee = call->getCalledFunction();
				if (callee->getName().equals(PACKETIZED_OPENCL_DRIVER_FUNCTION_NAME_BARRIER)) ++numBarriers;
			}
		}

		if (numBarriers == 0) return f;

		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\neliminateBarriers(" << f->getNameStr() << ")\n"; );
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  number of barriers in function: " << numBarriers << "\n"; );

		//--------------------------------------------------------------------//
		// change return value of f to return unsigned (barrier id)
		// = create new function with new signature and clone all blocks
		// The former return statements now all return -1 (special end id)
		//--------------------------------------------------------------------//
		const FunctionType* fTypeOld = f->getFunctionType();
		std::vector<const Type*> params;
		for (FunctionType::param_iterator it=fTypeOld->param_begin(), E=fTypeOld->param_end(); it!=E; ++it) {
			params.push_back(*it);
		}
		const FunctionType* fTypeNew = FunctionType::get(Type::getInt32Ty(context), params, false);
		Function* newF = Function::Create(fTypeNew, Function::ExternalLinkage, f->getNameStr()+"_begin", mod); // TODO: check linkage type

		// specify mapping of parameters
		DenseMap<const Value*, Value*> valueMap;
		Function::arg_iterator A2 = newF->arg_begin();
		for (Function::arg_iterator A=f->arg_begin(), AE=f->arg_end(); A!=AE; ++A, ++A2) {
			valueMap.insert(std::make_pair(A, A2));
		}
		SmallVector<ReturnInst*, 2> returns;

		CloneAndPruneFunctionInto(newF, f, valueMap, returns);

		for (unsigned i=0; i<returns.size(); ++i) {
			BasicBlock* retBlock = returns[i]->getParent();
			returns[i]->eraseFromParent();
			ReturnInst::Create(context, ConstantInt::get(fTypeNew->getReturnType(), PACKETIZED_OPENCL_DRIVER_BARRIER_SPECIAL_END_ID, true), retBlock);
		}

		//--------------------------------------------------------------------//
		// call eliminateBarrier() for each barrier in newFunction
		//--------------------------------------------------------------------//
		const unsigned numContinuationFunctions = numBarriers+1;
		std::vector<Function*> continuations;
		//continuations.reserve(numContinuationFunctions);
		continuations.push_back(newF);
		unsigned barrierIndex = 0; // only used for naming scheme
		bool functionChanged = true;
		while (functionChanged) {
			functionChanged = false;
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "eliminating barriers...\n"; );
			for (Function::iterator BB=newF->begin(), BBE=newF->end(); BB!=BBE; ++BB) {
				for (BasicBlock::iterator I=BB->begin(), IE=BB->end(); I!=IE; ++I) {
					if (!isa<CallInst>(I)) continue;
					CallInst* call = cast<CallInst>(I);
					PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  found call: " << *call << "\n"; );

					const Function* callee = call->getCalledFunction();
					if (!callee->getName().equals(PACKETIZED_OPENCL_DRIVER_FUNCTION_NAME_BARRIER)) continue;

					PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    is barrier!\n"; );

					std::stringstream sstr;
					sstr << f->getNameStr() << "_cont_" << ++barrierIndex;  // "0123456789ABCDEF"[x] would be okay if we could guarantee a max size for continuations :p
					Function* continuationFun = eliminateBarrier(call, fTypeNew, sstr.str());
					assert (continuationFun);
					continuations.push_back(continuationFun);
					functionChanged = true;
					break;
				}
				if (functionChanged) break;
			}
		}

		assert (continuations.size() == numContinuationFunctions);

		//--------------------------------------------------------------------//
		// create wrapper function which contains a switch over the barrier id
		// inside a while loop.
		// the wrapper calls the function that corresponds to the barrier id.
		// If the id is the special 'begin' id, it calls the first function
		// (= the remainder of the original kernel).
		// The while loop iterates until the barrier id is set to a special
		// 'end' id.
		//--------------------------------------------------------------------//
		// Example:
		/*
		while (true) {
			switch (current_barrier_id) {
				case BARRIER_BEGIN: current_barrier_id = runOrigFunc(...); break;
				case BARRIER_END: return;
				case B0: current_barrier_id = runFunc0(...); break;
				case B1: current_barrier_id = runFunc1(...); break;
				...
				case BN: current_barrier_id = runFuncN(...); break;
				default: error; break;
			}
		}
		*/
		Function* wrapper = Function::Create(fTypeOld, Function::ExternalLinkage, f->getNameStr()+"_barrierswitch", mod); // TODO: check linkage type

		IRBuilder<> builder(context);

		// create entry block
		BasicBlock* entryBB = BasicBlock::Create(context, "entry", wrapper);

		// create blocks for while loop
		BasicBlock* headerBB = BasicBlock::Create(context, "while.header", wrapper);
		//BasicBlock* bodyBB = BasicBlock::Create(context, "while.body", wrapper);
		BasicBlock* latchBB = BasicBlock::Create(context, "while.latch", wrapper);

		// create call blocks (switch targets)
		BasicBlock** callBBs = new BasicBlock*[numContinuationFunctions]();
		for (unsigned i=0; i<numContinuationFunctions; ++i) {
			std::stringstream sstr;
			sstr << "switch." << i;  // "0123456789ABCDEF"[x] would be okay if we could guarantee a max size for continuations :p
			callBBs[i] = BasicBlock::Create(context, sstr.str(), wrapper);
		}

		// create exit block
		BasicBlock* exitBB = BasicBlock::Create(context, "exit", wrapper);



		// fill entry block (empty, directly branch to header)
		builder.SetInsertPoint(entryBB);
		builder.CreateBr(headerBB);

		// fill header
		builder.SetInsertPoint(headerBB);
		PHINode* current_barrier_id_phi = builder.CreatePHI(Type::getInt32Ty(context), "current_barrier_id");
		current_barrier_id_phi->addIncoming(ConstantInt::getNullValue(Type::getInt32Ty(context)), entryBB);

		SwitchInst* switchI = builder.CreateSwitch(current_barrier_id_phi, exitBB, numContinuationFunctions);
		for (unsigned i=0; i<numContinuationFunctions; ++i) {
			// add case for each continuation
			switchI->addCase(ConstantInt::get(context, APInt(32, i)), callBBs[i]);
		}


		// fill call blocks
		CallInst** calls = new CallInst*[numContinuationFunctions]();
		for (unsigned i=0; i<numContinuationFunctions; ++i) {
			BasicBlock* block = callBBs[i];
			builder.SetInsertPoint(block);
			// create the call to f
			SmallVector<Value*, 8> args;
			for (Function::arg_iterator A=wrapper->arg_begin(), AE=wrapper->arg_end(); A!=AE; ++A) {
				assert (isa<Value>(A));
				args.push_back(cast<Value>(A));
			}
			//CallInst* call = builder.CreateCall(continuations[i], wrapper->arg_begin(), wrapper->arg_end(), "continuation"+i); // doesn't work! ARGH!!!
			outs() << "creating call for continuation: " << continuations[i]->getNameStr() << "\n";
			std::stringstream sstr;
			sstr << "continuation." << i;  // "0123456789ABCDEF"[x] would be okay if we could guarantee a max size for continuations :p
			calls[i] = builder.CreateCall(continuations[i], args.begin(), args.end(), sstr.str());
			//calls[i]->addAttribute(1, Attribute::NoCapture);
			//calls[i]->addAttribute(1, Attribute::NoAlias);

			builder.CreateBr(latchBB);
		}

		// fill latch
		builder.SetInsertPoint(latchBB);

		// create phi for next barrier id coming from each call inside the switch
		PHINode* next_barrier_id_phi = builder.CreatePHI(Type::getInt32Ty(context), "next_barrier_id");
		for (unsigned i=0; i<numContinuationFunctions; ++i) {
			next_barrier_id_phi->addIncoming(calls[i], callBBs[i]);
		}

		// add the phi as incoming value to the phi in the loop header
		current_barrier_id_phi->addIncoming(next_barrier_id_phi, latchBB);

		// create check whether id is the special end id ( = is negative)
		// if yes, exit the loop, otherwise go on iterating
		Value* cond = builder.CreateICmpSLT(next_barrier_id_phi, ConstantInt::getNullValue(Type::getInt32Ty(context)), "exitcond");
		builder.CreateCondBr(cond, exitBB, headerBB);

		
		// fill exit
		builder.SetInsertPoint(exitBB);
		builder.CreateRetVoid();


		delete [] calls;
		delete [] callBBs;

		PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(mod); );

		//--------------------------------------------------------------------//
		// inline continuation functions & optimize wrapper
		//--------------------------------------------------------------------//
		PacketizedOpenCLDriver::inlineFunctionCalls(wrapper, new TargetData(mod));
		PacketizedOpenCLDriver::optimizeFunction(wrapper);

		PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(mod); );

		//outs() << *mod << "\n";
		//outs() << *wrapper << "\n";

		return wrapper;
	}
	
	//------------------------------------------------------------------------//
	// host information
	//------------------------------------------------------------------------//

	// TODO: get real info :p
	inline unsigned long long getDeviceMaxMemAllocSize() {
		return 0x3B9ACA00; // 1 GB
	}

}


///////////////////////////////////////////////////////////////////////////
//             Packetized OpenCL Internal Data Structures                //
///////////////////////////////////////////////////////////////////////////

struct _cl_platform_id {};
struct _cl_device_id {};

/*
An OpenCL context is created with one or more devices. Contexts
are used by the OpenCL runtime for managing objects such as command-queues,
memory, program and kernel objects and for executing kernels on one or more
devices specified in the context.
*/
struct _cl_context {};

/*
OpenCL objects such as memory, program and kernel objects are created using a
context.
Operations on these objects are performed using a command-queue. The
command-queue can be used to queue a set of operations (referred to as commands)
in order. Having multiple command-queues allows applications to queue multiple
independent commands without requiring synchronization. Note that this should
work as long as these objects are not being shared. Sharing of objects across
multiple command-queues will require the application to perform appropriate
synchronization. This is described in Appendix A.
*/
struct _cl_command_queue {
	_cl_context* context;
};

/*
Memory objects are categorized into two types: buffer objects, and image
objects. A buffer object stores a one-dimensional collection of elements whereas
an image object is used to store a two- or three- dimensional texture,
frame-buffer or image.
Elements of a buffer object can be a scalar data type (such as an int, float),
vector data type, or a user-defined structure. An image object is used to
represent a buffer that can be used as a texture or a frame-buffer. The elements
of an image object are selected from a list of predefined image formats. The
minimum number of elements in a memory object is one.
*/
struct _cl_mem {
private:
	_cl_context* context;
	size_t size; //entire size in bytes
	void* data;
	const bool canRead;
	const bool canWrite;
public:
	_cl_mem(_cl_context* ctx, size_t bytes, void* values, bool can_read, bool can_write)
			: context(ctx), size(bytes), data(values), canRead(can_read), canWrite(can_write) {}
	
	inline _cl_context* get_context() const { return context; }
	inline void* get_data() const { return data; }
	inline size_t get_size() const { return size; }
	inline bool isReadOnly() const { return canRead && !canWrite; }
	inline bool isWriteOnly() const { return !canRead && canWrite; }

	inline void copy_data(
			const void* values,
			const size_t bytes,
			const size_t offset=0)
	{
		assert (bytes+offset <= size);
		if (offset == 0) memcpy(data, values, bytes);
		else {
			for (cl_uint i=offset; i<bytes; ++i) {
				((char*)data)[i] = ((const char*)values)[i];
			}
		}
	}
};

/*
A sampler object describes how to sample an image when the image is read in the
kernel. The built-in functions to read from an image in a kernel take a sampler
as an argument. The sampler arguments to the image read function can be sampler
objects created using OpenCL functions and passed as argument values to the
kernel or can be samplers declared inside a kernel. In this section we discuss
how sampler objects are created using OpenCL functions.
*/
struct _cl_sampler {
	_cl_context* context;
};

/*
An OpenCL program consists of a set of kernels that are identified as functions
declared with the __kernel qualifier in the program source. OpenCL programs may
also contain auxiliary functions and constant data that can be used by __kernel
functions. The program executable can be generated online or offline by the
OpenCL compiler for the appropriate target device(s).
A program object encapsulates the following information:
       An associated context.
       A program source or binary.
       The latest successfully built program executable, the list of devices for
       which the program executable is built, the build options used and a build
       log.
       The number of kernel objects currently attached.
*/
struct _cl_program {
	_cl_context* context;
	const char* fileName;
	llvm::Module* module;
	llvm::TargetData* targetData;
};


struct _cl_kernel_arg {
private:
	// known at kernel creation time
	const size_t element_size; // size of one item in bytes
	const cl_uint address_space;
	const bool uniform;
	void* mem_address; // values are inserted by kernel::set_arg_data()

	// only known after clSetKernelArg
	size_t size; // size of entire argument value

public:
	_cl_kernel_arg(
			const size_t _elem_size,
			const cl_uint _address_space,
			const bool _uniform,
			void* _mem_address,
			const size_t _size=0)
		: element_size(_elem_size),
		address_space(_address_space),
		uniform(_uniform),
		mem_address(_mem_address),
		size(_size)
	{}

	inline void set_size(size_t _size) { size = _size; }

	inline size_t get_size() const { return size; }
	inline size_t get_element_size() const { return element_size; }
	inline cl_uint get_address_space() const { return address_space; }
	inline void* get_mem_address() const { return mem_address; } // must not assert (data) -> can be 0 if non-pointer type (e.g. float)

	inline bool is_uniform() const { return uniform; }
};

/*
A kernel is a function declared in a program. A kernel is identified by the
__kernel qualifier applied to any function in a program. A kernel object
encapsulates the specific __kernel function declared in a program and the
argument values to be used when executing this __kernel function.
*/
struct _cl_kernel {
private:
	_cl_context* context;
	_cl_program* program;
	const void* compiled_function;

	std::vector<_cl_kernel_arg*> args;
	const cl_uint num_args;

	void* argument_struct;
	size_t argument_struct_size;

public:
	_cl_kernel(_cl_context* ctx, _cl_program* prog, llvm::Function* f,
			llvm::Function* f_wrapper, llvm::Function* f_SIMD=NULL)
		: context(ctx), program(prog), compiled_function(NULL), num_args(PacketizedOpenCLDriver::getNumArgs(f)),
		argument_struct(NULL), argument_struct_size(0),
		function(f), function_wrapper(f_wrapper), function_SIMD(f_SIMD)
	{
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  creating kernel object... \n"; );
		assert (ctx && prog && f && f_wrapper);

		// compile wrapper function (to be called in clEnqueueNDRangeKernel())
		// NOTE: be sure that f_SIMD or f are inlined and f_wrapper was optimized to the max :p
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    compiling function... "; );
		compiled_function = PacketizedOpenCLDriver::getPointerToFunction(prog->module, f_wrapper);
		if (!compiled_function) {
			errs() << "\nERROR: JIT compilation of kernel function failed!\n";
		}
		PACKETIZED_OPENCL_DRIVER_DEBUG( if (compiled_function) outs() << "done.\n"; );

		// get argument information
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    collecting argument information...\n"; );
		//num_args = PacketizedOpenCLDriver::getNumArgs(f);
		assert (num_args > 0); // TODO: don't we allow kernels without arguments? do they make sense?
		args.reserve(num_args);

		// determine size of each argument
		for (cl_uint arg_index=0; arg_index<num_args; ++arg_index) {
			// get type of argument and corresponding size
			const llvm::Type* argType = PacketizedOpenCLDriver::getArgumentType(f, arg_index);
			const size_t arg_size_bytes = PacketizedOpenCLDriver::getTypeSizeInBits(program->targetData, argType) / 8;

			argument_struct_size += arg_size_bytes;
		}

		// allocate memory for argument_struct
		// TODO: do we have to care about type padding?
		argument_struct = malloc(argument_struct_size);
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "      size of argument-struct: " << argument_struct_size << " bytes\n"; );
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "      address of argument-struct: " << argument_struct << "\n"; );
		PACKETIZED_OPENCL_DRIVER_DEBUG(
			const llvm::Type* argType = PacketizedOpenCLDriver::getArgumentType(f_wrapper, 0);
			outs() << "      LLVM type: " << *argType << "\n";
			const llvm::Type* sType = PacketizedOpenCLDriver::getContainedType(argType, 0); // get size of struct, not of pointer to struct
			outs() << "      LLVM type size: " << PacketizedOpenCLDriver::getTypeSizeInBits(prog->targetData, sType)/8 << "\n";
		);

		// create argument objects
		size_t current_size=0;
		for (cl_uint arg_index=0; arg_index<num_args; ++arg_index) {

			const llvm::Type* argType = PacketizedOpenCLDriver::getArgumentType(f, arg_index);
			const size_t arg_size_bytes = PacketizedOpenCLDriver::getTypeSizeInBits(program->targetData, argType) / 8;
			const cl_uint address_space = PacketizedOpenCLDriver::convertLLVMAddressSpace(PacketizedOpenCLDriver::getAddressSpace(argType));

			// check if argument is uniform
#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
			const bool arg_uniform = true; // no packetization = no need for uniform/varying
#else
			assert (f_SIMD);
			// save info if argument is uniform or varying
			// TODO: implement in packetizer directly
			// HACK: if types match, they are considered uniform, varying otherwise

			const llvm::Type* argType_SIMD = PacketizedOpenCLDriver::getArgumentType(f_SIMD, arg_index);
			const bool arg_uniform = argType == argType_SIMD;

			// check for sanity
			if (!arg_uniform && (address_space != CL_GLOBAL)) {
				// NOTE: This can not really exist, as the input data for such a value
				//       is always a scalar (the user's host program is not changed)!
				// NOTE: This case would mean there are values that change for each thread in a group
				//       but that is the same for the ith thread of any group.
				errs() << "WARNING: packet function must not use varying, non-pointer agument!\n";
				// TODO: should we do something about this and return some error code? :p
			}
#endif

			// save pointer to address of argument inside argument_struct
			void* arg_struct_addr = ((char*)argument_struct)+current_size;
			current_size += arg_size_bytes;

			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "      argument " << arg_index << "\n"; );
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "        size     : " << arg_size_bytes << " bytes\n"; );
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "        address  : " << (void*)arg_struct_addr << "\n"; );
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "        addrspace: " << PacketizedOpenCLDriver::getAddressSpaceString(address_space) << "\n"; );

			args[arg_index] = new _cl_kernel_arg(arg_size_bytes, address_space, arg_uniform, arg_struct_addr);
		}

		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  kernel object created successfully!\n\n"; );
	}

	~_cl_kernel() { args.clear(); }

	const llvm::Function* function;
	const llvm::Function* function_wrapper;
	const llvm::Function* function_SIMD;

	// Copy 'arg_size' bytes from 'data' into argument_struct at the position
	// of argument at index 'arg_index'.
	// There are some possible issues with the type of data being copied:
	// We simply distinguish different address spaces to know whether the data
	// is actually a _cl_mem object, raw data, or a local pointer - all three
	// cases have to be treated differently:
	// _cl_mem** - CL_GLOBAL  - access the mem object and copy its data
	// raw data  - CL_PRIVATE - copy the data directly
	// local ptr - CL_LOCAL   - copy the pointer
	//
	// OpenCL Specification 1.0 for clSetKernelArg:
	// The argument data pointed to by arg_value is copied and the arg_value
	// pointer can therefore be reused by the application after clSetKernelArg returns.
	//
	// arg_size specifies the size of the argument value. If the argument is a memory object, the size is
	// the size of the buffer or image object type. For arguments declared with the __local qualifier,
	// the size specified will be the size in bytes of the buffer that must be allocated for the __local
	// argument.
	inline cl_uint set_arg_data(const cl_uint arg_index, const void* data, const size_t arg_size) {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");

		// store argument size
		args[arg_index]->set_size(arg_size);

		char* arg_pos = (char*)arg_get_data(arg_index); //((char*)argument_struct)+current_size;

		// NOTE: for pointers, we supply &data because we really want to copy the pointer!
		switch (arg_get_address_space(arg_index)) {
			case CL_GLOBAL: {
				assert (arg_size == sizeof(_cl_mem*)); // = sizeof(cl_mem)
				// data is actually a _cl_mem* given by reference
				const _cl_mem* mem = *(const _cl_mem**)data; 
				const void* datax = mem->get_data();
				// copy the pointer, not what is pointed to
				memcpy(arg_pos, &datax, arg_size);
				break;
			}
			case CL_PRIVATE: {
				// copy the data itself
				memcpy(arg_pos, data, arg_size);
				break;
			}
			case CL_LOCAL: {
				// copy the pointer, not what is pointed to
				//memcpy(arg_pos, &data, arg_size);

				assert (!data);
				// allocate memory of size 'arg_size' and copy the pointer
				const void* datax = malloc(arg_size);
				memcpy(arg_pos, &datax, sizeof(void*));
				break;
			}
			case CL_CONSTANT: {
				errs() << "ERROR: support for constant memory not implemented yet!\n";
				assert (false && "support for constant memory not implemented yet!");
				return CL_INVALID_VALUE; // sth like that :p
			}
			default: {
				errs() << "ERROR: unknown address space found: " << arg_get_address_space(arg_index) << "\n";
				assert (false && "unknown address space found!");
				return CL_INVALID_VALUE; // sth like that :p
			}
		}

		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  data source: " << data << "\n"; );
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  target pointer: " << (void*)arg_pos << "\n"; );

		return CL_SUCCESS;
	}

	inline _cl_context* get_context() const { return context; }
	inline _cl_program* get_program() const { return program; }
	inline const void* get_compiled_function() const { return compiled_function; }
	inline cl_uint get_num_args() const { return num_args; }
	inline const void* get_argument_struct() const { return argument_struct; }
	inline size_t get_argument_struct_size() const { return argument_struct_size; }

	inline size_t arg_get_element_size(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_element_size();
	}
	inline cl_uint arg_get_address_space(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_address_space();
	}
	inline bool arg_is_global(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_address_space() == CL_GLOBAL;
	}
	inline bool arg_is_local(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_address_space() == CL_LOCAL;
	}
	inline bool arg_is_private(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_address_space() == CL_PRIVATE;
	}
	inline bool arg_is_constant(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_address_space() == CL_CONSTANT;
	}
	inline void* arg_get_data(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->get_mem_address();
	}

	inline bool arg_is_uniform(const cl_uint arg_index) const {
		assert (arg_index < num_args);
		assert (args[arg_index] && "kernel object not completely initialized?");
		return args[arg_index]->is_uniform();
	}
};

struct _cl_event {
	_cl_context* context;
};


///////////////////////////////////////////////////////////////////////////
//              Packetized OpenCL Driver Implementation                  //
///////////////////////////////////////////////////////////////////////////

/* Platform API */
CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformIDs(cl_uint          num_entries,
                 cl_platform_id * platforms,
                 cl_uint *        num_platforms) CL_API_SUFFIX__VERSION_1_0
{
	if (!platforms && !num_platforms) return CL_INVALID_VALUE;
	if (platforms && num_entries == 0) return CL_INVALID_VALUE;

	if (platforms) platforms[0] = new _cl_platform_id();
	if (num_platforms) *num_platforms = 1;

	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetPlatformInfo(cl_platform_id   platform,
                  cl_platform_info param_name,
                  size_t           param_value_size,
                  void *           param_value,
                  size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!platform) return CL_INVALID_PLATFORM; //effect implementation defined
	if (param_value && param_value_size < sizeof(char*)) return CL_INVALID_VALUE;

	switch (param_name) {
		case CL_PLATFORM_PROFILE:
			if (param_value) param_value = (void*)"FULL_PROFILE"; //or "EMBEDDED_PROFILE"
			break;
		case CL_PLATFORM_VERSION:
			if (param_value) param_value = (void*)"OpenCL 1.0 PACKETIZED OPENCL DRIVER";
			break;
		case CL_PLATFORM_NAME:
			if (param_value) param_value = (void*)"cpu";
			break;
		case CL_PLATFORM_VENDOR:
			if (param_value) param_value = (void*)"Saarland University";
			break;
		case CL_PLATFORM_EXTENSIONS:
			if (param_value) param_value = (void*)"";
			break;
		default:
			return CL_INVALID_VALUE;
	}

	return CL_SUCCESS;
}

/* Device APIs */
CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceIDs(cl_platform_id   platform,
               cl_device_type   device_type,
               cl_uint          num_entries,
               cl_device_id *   devices,
               cl_uint *        num_devices) CL_API_SUFFIX__VERSION_1_0
{
	if (device_type != CL_DEVICE_TYPE_CPU) {
		errs() << "ERROR: packetized OpenCL driver can not handle devices other than CPU!\n";
		return CL_INVALID_DEVICE_TYPE;
	}
	if (devices && num_entries < 1) return CL_INVALID_VALUE;
	if (!devices && !num_devices) return CL_INVALID_VALUE;
	if (devices) devices = new cl_device_id();
	if (num_devices) num_devices = new cl_uint(1);
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetDeviceInfo(cl_device_id    device,
                cl_device_info  param_name,
                size_t          param_value_size,
                void *          param_value,
                size_t *        param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!device) return CL_INVALID_DEVICE;

	// TODO: move into _cl_device_id, this here is the wrong place !!
	switch (param_name) {
		case CL_DEVICE_TYPE: {
			if (param_value_size < sizeof(cl_device_type)) return CL_INVALID_VALUE;
			if (param_value) *(cl_device_type*)param_value = CL_DEVICE_TYPE_CPU;
			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_device_type);
			break;
		}
		case CL_DEVICE_VENDOR_ID: {
			if (param_value_size < sizeof(cl_uint)) return CL_INVALID_VALUE;
			if (param_value) *(cl_uint*)param_value = 0; // should be some "unique device vendor identifier"
			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_uint);
			break;
		}
		case CL_DEVICE_MAX_COMPUTE_UNITS: {
			if (param_value_size < sizeof(cl_uint)) return CL_INVALID_VALUE;

#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
	#ifndef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
			if (param_value) *(cl_uint*)param_value = 1;
	#else
			if (param_value) *(cl_uint*)param_value = numCores;
	#endif
#else
	#ifndef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
			if (param_value) *(cl_uint*)param_value = simdWidth;
	#else
			if (param_value) *(cl_uint*)param_value = numCores*simdWidth;
	#endif
#endif

			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_uint);
			break;
		}
		case CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS: {
			if (param_value_size < sizeof(cl_uint)) return CL_INVALID_VALUE;
			if (param_value) *(cl_uint*)param_value = maxNumDimensions;
			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_uint);
			break;
		}
		case CL_DEVICE_MAX_WORK_ITEM_SIZES: {
			if (param_value_size < sizeof(size_t)) return CL_INVALID_VALUE;
			if (param_value) {
				for (unsigned i=0; i<maxNumDimensions; ++i) {
					((size_t*)param_value)[i] = PacketizedOpenCLDriver::getDeviceMaxMemAllocSize(); // FIXME
				}
			}
			if (param_value_size_ret) *param_value_size_ret = sizeof(size_t)*maxNumDimensions;
			break;
		}
		case CL_DEVICE_MAX_WORK_GROUP_SIZE: {
			if (param_value_size < sizeof(size_t)) return CL_INVALID_VALUE;
			if (param_value) *(size_t*)param_value = PacketizedOpenCLDriver::getDeviceMaxMemAllocSize(); // FIXME
			if (param_value_size_ret) *param_value_size_ret = sizeof(size_t*);
			break;
		}
		case CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_CLOCK_FREQUENCY: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_ADDRESS_BITS: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_MEM_ALLOC_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE_SUPPORT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_READ_IMAGE_ARGS: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_WRITE_IMAGE_ARGS: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE2D_MAX_WIDTH: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE2D_MAX_HEIGHT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE3D_MAX_WIDTH: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE3D_MAX_HEIGHT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_IMAGE3D_MAX_DEPTH: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_SAMPLERS: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_PARAMETER_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MEM_BASE_ADDR_ALIGN: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MIN_DATA_TYPE_ALIGN_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_SINGLE_FP_CONFIG: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_GLOBAL_MEM_CACHE_TYPE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_GLOBAL_MEM_CACHE_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_GLOBAL_MEM_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_MAX_CONSTANT_ARGS: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_LOCAL_MEM_TYPE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_LOCAL_MEM_SIZE: {
			// local memory == global memory for cpu
			// TODO: make sure size*maxNumThreads(*simdWidth?) is really available on host ;)
			if (param_value_size < sizeof(unsigned long long)) return CL_INVALID_VALUE;
			if (param_value) *(unsigned long long*)param_value = PacketizedOpenCLDriver::getDeviceMaxMemAllocSize(); // FIXME: use own function
			if (param_value_size_ret) *param_value_size_ret = sizeof(unsigned long long);
			break;
		}
		case CL_DEVICE_ERROR_CORRECTION_SUPPORT: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PROFILING_TIMER_RESOLUTION: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_ENDIAN_LITTLE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_AVAILABLE: {
			if (param_value_size < sizeof(cl_bool)) return CL_INVALID_VALUE;
			if (param_value) *(cl_bool*)param_value = true; // TODO: check if cpu supports SSE
			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_bool);
			break;
		}
		case CL_DEVICE_COMPILER_AVAILABLE: {
			if (param_value_size < sizeof(cl_bool)) return CL_INVALID_VALUE;
			if (param_value) *(cl_bool*)param_value = true; // TODO: check if clang/llvm is available
			if (param_value_size_ret) *param_value_size_ret = sizeof(cl_bool);
			break;
		}
		case CL_DEVICE_EXECUTION_CAPABILITIES: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_QUEUE_PROPERTIES: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_PLATFORM: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_NAME: {
			if (param_value_size < sizeof(char*)) return CL_INVALID_VALUE;
			if (param_value) *(std::string*)param_value = "sse cpu"; // TODO: should just return "cpu"?
			if (param_value_size_ret) *param_value_size_ret = sizeof(char*);
			break;
		}
		case CL_DEVICE_VENDOR: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DRIVER_VERSION: {
			if (param_value_size < sizeof(char*)) return CL_INVALID_VALUE;
			if (param_value) *(std::string*)param_value = PACKETIZED_OPENCL_DRIVER_VERSION_STRING;
			if (param_value_size_ret) *param_value_size_ret = sizeof(char*);
			break;
		}
		case CL_DEVICE_PROFILE: {
			errs() << "ERROR: param_name '" << param_name << "' not implemented yet!\n";
			assert (false && "NOT IMPLEMENTED!");
			return CL_INVALID_VALUE;
		}
		case CL_DEVICE_VERSION: {
			if (param_value_size < sizeof(char*)) return CL_INVALID_VALUE;
			if (param_value) *(std::string*)param_value = "OpenCL 1.0 Packetized";
			if (param_value_size_ret) *param_value_size_ret = sizeof(char*);
			break;
		}
		case CL_DEVICE_EXTENSIONS: {
			if (param_value_size < sizeof(char*)) return CL_INVALID_VALUE;
			if (param_value) strcpy((char*)param_value, PACKETIZED_OPENCL_DRIVER_EXTENSIONS);
			if (param_value_size_ret) *param_value_size_ret = sizeof(char*);
			break;
		}

		default: {
			errs() << "ERROR: unknown param_name found: " << param_name << "!\n";
			return CL_INVALID_VALUE;
		}
	}

	return CL_SUCCESS;
}

/* Context APIs  */
CL_API_ENTRY cl_context CL_API_CALL
clCreateContext(const cl_context_properties * properties,
                cl_uint                       num_devices,
                const cl_device_id *          devices,
                void (*pfn_notify)(const char *, const void *, size_t, void *),
                void *                        user_data,
                cl_int *                      errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	*errcode_ret = CL_SUCCESS;
	return new _cl_context();
}

CL_API_ENTRY cl_context CL_API_CALL
clCreateContextFromType(const cl_context_properties * properties,
                        cl_device_type                device_type,
                        void (*pfn_notify)(const char *, const void *, size_t, void *),
                        void *                        user_data,
                        cl_int *                      errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!pfn_notify && user_data) { *errcode_ret = CL_INVALID_VALUE; return NULL; }

	if (device_type != CL_DEVICE_TYPE_CPU) { *errcode_ret = CL_DEVICE_NOT_AVAILABLE; return NULL; }

	*errcode_ret = CL_SUCCESS;
	return new _cl_context();
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainContext(cl_context context) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseContext(cl_context context) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseContext()\n"; );
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetContextInfo(cl_context         context,
                 cl_context_info    param_name,
                 size_t             param_value_size,
                 void *             param_value,
                 size_t *           param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clGetContextInfo()\n"; );
	if (param_value_size_ret) *param_value_size_ret = 4;
	return CL_SUCCESS;
}

/* Command Queue APIs */

/*
creates a command-queue on a specific device.
*/
// -> ??
CL_API_ENTRY cl_command_queue CL_API_CALL
clCreateCommandQueue(cl_context                     context,
                     cl_device_id                   device,
                     cl_command_queue_properties    properties,
                     cl_int *                       errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	errcode_ret = CL_SUCCESS;
	_cl_command_queue* cq = new _cl_command_queue();
	cq->context = context;
	return cq;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainCommandQueue(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseCommandQueue(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseCommandQueue()\n"; );
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetCommandQueueInfo(cl_command_queue      command_queue,
                      cl_command_queue_info param_name,
                      size_t                param_value_size,
                      void *                param_value,
                      size_t *              param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clSetCommandQueueProperty(cl_command_queue              command_queue,
                          cl_command_queue_properties   properties,
                          cl_bool                        enable,
                          cl_command_queue_properties * old_properties) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Memory Object APIs  */

/*
Memory objects are categorized into two types: buffer objects, and image objects. A buffer
object stores a one-dimensional collection of elements whereas an image object is used to store a
two- or three- dimensional texture, frame-buffer or image.
Elements of a buffer object can be a scalar data type (such as an int, float), vector data type, or a
user-defined structure. An image object is used to represent a buffer that can be used as a texture
or a frame-buffer. The elements of an image object are selected from a list of predefined image
formats. The minimum number of elements in a memory object is one.
*/
CL_API_ENTRY cl_mem CL_API_CALL
clCreateBuffer(cl_context   context,
               cl_mem_flags flags,
               size_t       size, //in bytes
               void *       host_ptr,
               cl_int *     errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!context) { if (errcode_ret) *errcode_ret = CL_INVALID_CONTEXT; return NULL; }
	if (size == 0 || size > PacketizedOpenCLDriver::getDeviceMaxMemAllocSize()) { if (errcode_ret) *errcode_ret = CL_INVALID_BUFFER_SIZE; return NULL; }
	const bool useHostPtr   = flags & CL_MEM_USE_HOST_PTR;
	const bool copyHostPtr  = flags & CL_MEM_COPY_HOST_PTR;
	const bool allocHostPtr = flags & CL_MEM_ALLOC_HOST_PTR;
	if (!host_ptr && (useHostPtr || copyHostPtr)) { if (errcode_ret) *errcode_ret = CL_INVALID_HOST_PTR; return NULL; }
	if (host_ptr && !useHostPtr && !copyHostPtr) { if (errcode_ret) *errcode_ret = CL_INVALID_HOST_PTR; return NULL; }
	if (useHostPtr && allocHostPtr) { if (errcode_ret) *errcode_ret = CL_INVALID_VALUE; return NULL; } // custom
	if (useHostPtr && copyHostPtr) { if (errcode_ret) *errcode_ret = CL_INVALID_VALUE; return NULL; } // custom

	const bool canRead     = (flags & CL_MEM_READ_ONLY) || (flags & CL_MEM_READ_WRITE);
	const bool canWrite    = (flags & CL_MEM_WRITE_ONLY) || (flags & CL_MEM_READ_WRITE);

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "clCreateBuffer(" << size << " bytes, " << host_ptr << ")\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  canRead     : " << (canRead ? "true" : "false") << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  canWrite    : " << (canWrite ? "true" : "false") << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  useHostPtr  : " << (useHostPtr ? "true" : "false") << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  copyHostPtr : " << (copyHostPtr ? "true" : "false") << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  allocHostPtr: " << (allocHostPtr ? "true" : "false") << "\n"; );

	void* device_ptr = NULL;

	if (useHostPtr) {
		assert (host_ptr);
		device_ptr = host_ptr;
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    using supplied host ptr: " << device_ptr << "\n"; );
	}

	if (allocHostPtr) {
		device_ptr = malloc(size);
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    new host ptr allocated: " << device_ptr << "\n"; );
		if (!device_ptr) { if (errcode_ret) *errcode_ret = CL_MEM_OBJECT_ALLOCATION_FAILURE; return NULL; }
	}

	if (copyHostPtr) {
		// CL_MEM_COPY_HOST_PTR can be used with
		// CL_MEM_ALLOC_HOST_PTR to initialize the contents of
		// the cl_mem object allocated using host-accessible (e.g.
		// PCIe) memory.
		assert (host_ptr);
		if (!allocHostPtr) {
			device_ptr = malloc(size);
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    new host ptr allocated for copying: " << device_ptr << "\n"; );
			if (!device_ptr) { if (errcode_ret) *errcode_ret = CL_MEM_OBJECT_ALLOCATION_FAILURE; return NULL; }
		}
		// copy data into new_host_ptr
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    copying data of supplied host ptr to new host ptr... "; );
		memcpy(device_ptr, host_ptr, size);
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "done.\n"; );
	}

	// if no flag was supplied, allocate memory (host_ptr must be NULL by specification)
	if (!device_ptr) {
		assert (!host_ptr);
		device_ptr = malloc(size);
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "    new host ptr allocated (no flag specified): " << device_ptr << "\n"; );
		if (!device_ptr) { if (errcode_ret) *errcode_ret = CL_MEM_OBJECT_ALLOCATION_FAILURE; return NULL; }
	}

	if (errcode_ret) *errcode_ret = CL_SUCCESS;
	return new _cl_mem(context, size, device_ptr, canRead, canWrite);
}

CL_API_ENTRY cl_mem CL_API_CALL
clCreateImage2D(cl_context              context,
                cl_mem_flags            flags,
                const cl_image_format * image_format,
                size_t                  image_width,
                size_t                  image_height,
                size_t                  image_row_pitch,
                void *                  host_ptr,
                cl_int *                errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY cl_mem CL_API_CALL
clCreateImage3D(cl_context              context,
                cl_mem_flags            flags,
                const cl_image_format * image_format,
                size_t                  image_width,
                size_t                  image_height,
                size_t                  image_depth,
                size_t                  image_row_pitch,
                size_t                  image_slice_pitch,
                void *                  host_ptr,
                cl_int *                errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainMemObject(cl_mem memobj) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseMemObject(cl_mem memobj) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseMemObject()\n"; );
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetSupportedImageFormats(cl_context           context,
                           cl_mem_flags         flags,
                           cl_mem_object_type   image_type,
                           cl_uint              num_entries,
                           cl_image_format *    image_formats,
                           cl_uint *            num_image_formats) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetMemObjectInfo(cl_mem           memobj,
                   cl_mem_info      param_name,
                   size_t           param_value_size,
                   void *           param_value,
                   size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetImageInfo(cl_mem           image,
               cl_image_info    param_name,
               size_t           param_value_size,
               void *           param_value,
               size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Sampler APIs  */
CL_API_ENTRY cl_sampler CL_API_CALL
clCreateSampler(cl_context          context,
                cl_bool             normalized_coords,
                cl_addressing_mode  addressing_mode,
                cl_filter_mode      filter_mode,
                cl_int *            errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainSampler(cl_sampler sampler) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseSampler(cl_sampler sampler) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetSamplerInfo(cl_sampler         sampler,
                 cl_sampler_info    param_name,
                 size_t             param_value_size,
                 void *             param_value,
                 size_t *           param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Program Object APIs  */

/*
creates a program object for a context, and loads the source code specified by the text strings in
the strings array into the program object. The devices associated with the program object are the
devices associated with context.
*/
// -> read strings and store as .cl representation
CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithSource(cl_context        context,
                          cl_uint           count,
                          const char **     strings,
                          const size_t *    lengths,
                          cl_int *          errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	errcode_ret = CL_SUCCESS;
	_cl_program* p = new _cl_program();
	p->context = context;
	p->fileName = *strings;
	return p;
}

// -> read binary and store as .cl representation
CL_API_ENTRY cl_program CL_API_CALL
clCreateProgramWithBinary(cl_context                     context,
                          cl_uint                        num_devices,
                          const cl_device_id *           device_list,
                          const size_t *                 lengths,
                          const unsigned char **         binaries,
                          cl_int *                       binary_status,
                          cl_int *                       errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainProgram(cl_program program) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseProgram(cl_program program) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseProgram()\n"; );
	return CL_SUCCESS;
}

/*
builds (compiles & links) a program executable from the program source or binary for all the
devices or a specific device(s) in the OpenCL context associated with program. OpenCL allows
program executables to be built using the source or the binary. clBuildProgram must be called
for program created using either clCreateProgramWithSource or
clCreateProgramWithBinary to build the program executable for one or more devices
associated with program.
*/
// -> build LLVM module from .cl representation (from createProgramWithSource/Binary)
// -> invoke clc
// -> invoke llvm-as
// -> store module in _cl_program object
CL_API_ENTRY cl_int CL_API_CALL
clBuildProgram(cl_program           program,
               cl_uint              num_devices,
               const cl_device_id * device_list,
               const char *         options,
               void (*pfn_notify)(cl_program program, void * user_data),
               void *               user_data) CL_API_SUFFIX__VERSION_1_0
{
	if (!program) return CL_INVALID_PROGRAM;
	if (!device_list && num_devices > 0) return CL_INVALID_VALUE;
	if (device_list && num_devices == 0) return CL_INVALID_VALUE;
	if (user_data && !pfn_notify) return CL_INVALID_VALUE;

	// TODO: read .cl representation, invoke clc, invoke llvm-as
	// alternative: link libClang and use it directly from here :)

	//FIXME: hardcoded for testing ;)
	llvm::Module* mod = PacketizedOpenCLDriver::createModuleFromFile(program->fileName);
	if (!mod) return CL_BUILD_PROGRAM_FAILURE;

	// TODO: do this here or only after packetization?
	mod->setDataLayout(PACKETIZED_OPENCL_DRIVER_LLVM_DATA_LAYOUT_64);
	// we have to reset the target triple (LLVM does not know amd-opencl)
	mod->setTargetTriple("");
	program->targetData = new TargetData(mod);

	program->module = mod;
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clUnloadCompiler(void) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetProgramInfo(cl_program         program,
                 cl_program_info    param_name,
                 size_t             param_value_size,
                 void *             param_value,
                 size_t *           param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetProgramBuildInfo(cl_program            program,
                      cl_device_id          device,
                      cl_program_build_info param_name,
                      size_t                param_value_size,
                      void *                param_value,
                      size_t *              param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Kernel Object APIs */

// -> compile bitcode of function from .bc file to native code
// -> store void* in _cl_kernel object
CL_API_ENTRY cl_kernel CL_API_CALL
clCreateKernel(cl_program      program,
               const char *    kernel_name,
               cl_int *        errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!program) { *errcode_ret = CL_INVALID_PROGRAM; return NULL; }
	if (!program->module) { *errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE; return NULL; }
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nclCreateKernel(" << program->module->getModuleIdentifier() << ", " << kernel_name << ")\n"; );

	// does the returned error code mean we should compile before??
	llvm::Module* module = program->module;

	if (!kernel_name) { *errcode_ret = CL_INVALID_VALUE; return NULL; }

	std::stringstream strs;
	strs << "__OpenCL_" << kernel_name << "_kernel";
	const std::string new_kernel_name = strs.str();

	llvm::Function* f = PacketizedOpenCLDriver::getFunction(new_kernel_name, module);
	if (!f) { *errcode_ret = CL_INVALID_KERNEL_NAME; return NULL; }

	// optimize kernel // TODO: not necessary if we optimize wrapper afterwards
	PacketizedOpenCLDriver::inlineFunctionCalls(f, program->targetData);
	PacketizedOpenCLDriver::optimizeFunction(f);

	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::writeFunctionToFile(f, "scalar.ll"); );

	f = PacketizedOpenCLDriver::eliminateBarriers(f);

#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION

	#ifdef PACKETIZED_OPENCL_DRIVER_USE_CLC_WRAPPER
	// USE CLC-GENERATED WRAPPER
	//
	std::stringstream strs2;
	strs2 << "__OpenCL_" << kernel_name << "_stub";
	const std::string wrapper_name = strs2.str();
	#else
	// USE AUTO-GENERATED WRAPPER
	//
	std::stringstream strs2;
	strs2 << kernel_name << "_wrapper";
	const std::string wrapper_name = strs2.str();

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  generating kernel wrapper... "; );
	PacketizedOpenCLDriver::generateKernelWrapper(wrapper_name, f, module);
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "done.\n"; );
	#endif

#else

	// PACKETIZATION ENABLED
	// USE AUTO-GENERATED PACKET WRAPPER
	//
	// save SIMD function for argument checking (uniform vs. varying)
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  generating OpenCL-specific functions etc... "; );

	std::stringstream strs2;
	strs2 << kernel_name << "_SIMD";
	const std::string kernel_simd_name = strs2.str();
	llvm::Function* f_SIMD = PacketizedOpenCLDriver::generatePacketPrototypeFromOpenCLKernel(f, kernel_simd_name, module, simdWidth);

	PacketizedOpenCLDriver::generateOpenCLFunctions(module, simdWidth);

	llvm::Function* gid = PacketizedOpenCLDriver::getFunction("get_global_id", module);
	llvm::Function* gid_split = PacketizedOpenCLDriver::getFunction("get_global_id_split", module);
	if (!gid) {
		errs() << "\nERROR: could not find function 'get_global_id' in module!\n";
		*errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE; //sth like that :p
		return NULL;
	}
	if (!gid_split) {
		errs() << "\nERROR: could not find function 'get_global_id_split' in module!\n";
		*errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE; //sth like that :p
		return NULL;
	}
	PacketizedOpenCLDriver::replaceNonContiguousIndexUsage(f, gid, gid_split);
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "done.\n"; );

	// packetize scalar function into SIMD function
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::writeFunctionToFile(f, "prepared.ll"); );
	__packetizeKernelFunction(new_kernel_name, kernel_simd_name, module, simdWidth, true, false);
	f_SIMD = PacketizedOpenCLDriver::getFunction(kernel_simd_name, module); //pointer not valid anymore!
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::writeFunctionToFile(f_SIMD, "packetized.ll"); );

	strs2 << "_wrapper";
	const std::string wrapper_name = strs2.str();

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  generating kernel wrapper... "; );
	PacketizedOpenCLDriver::generateKernelWrapper(wrapper_name, f_SIMD, module);
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "done.\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );
	
	PacketizedOpenCLDriver::fixUniformPacketizedArrayAccesses(f_SIMD, PacketizedOpenCLDriver::getFunction("get_global_id_SIMD", module), simdWidth);
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );
#endif

#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
	// link runtime calls (e.g. get_global_id()) to Packetized OpenCL Runtime
	PacketizedOpenCLDriver::resolveRuntimeCalls(module);
	PacketizedOpenCLDriver::fixFunctionNames(module);
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );
#endif

	llvm::Function* f_wrapper = PacketizedOpenCLDriver::getFunction(wrapper_name, module);
	if (!f_wrapper) {
		errs() << "ERROR: could not find wrapper function in kernel module!\n";
		*errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE; //sth like that :p
		return NULL;
	}

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  optimizing wrapper... "; );
	// inline all calls inside wrapper_fn
	PacketizedOpenCLDriver::inlineFunctionCalls(f_wrapper);

#ifndef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
	// replace functions by parameter accesses (has to be done AFTER inlining!
	// start with second argument (first is void* of argument_struct)
	llvm::Function::arg_iterator arg = f_wrapper->arg_begin(); 
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_work_dim"),       cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_global_size"),    cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_global_id"),      cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_local_size"),     cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_num_groups"),     cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_group_id"),       cast<Value>(++arg), f_wrapper);
	#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_local_id"),       cast<Value>(++arg), f_wrapper);
	#else
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_global_id_SIMD"), cast<Value>(++arg), f_wrapper);
	PacketizedOpenCLDriver::replaceCallbacksByArgAccess(module->getFunction("get_local_id_SIMD"),  cast<Value>(++arg),   f_wrapper);
	#endif

	PacketizedOpenCLDriver::fixFunctionNames(module);
#endif

	// optimize wrapper with inlined kernel
	PacketizedOpenCLDriver::inlineFunctionCalls(f_wrapper, program->targetData);
	PacketizedOpenCLDriver::optimizeFunction(f_wrapper);
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "done.\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::verifyModule(module); );
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::writeFunctionToFile(f_wrapper, "wrapper.ll"); );
	PACKETIZED_OPENCL_DRIVER_DEBUG( PacketizedOpenCLDriver::writeModuleToFile(module, "mod.ll"); );


	// create kernel object
#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
	_cl_kernel* kernel = new _cl_kernel(program->context, program, f, f_wrapper);
#else
	_cl_kernel* kernel = new _cl_kernel(program->context, program, f, f_wrapper, f_SIMD);
#endif

	if (!kernel->get_compiled_function()) { *errcode_ret = CL_INVALID_PROGRAM_EXECUTABLE; return NULL; }

	*errcode_ret = CL_SUCCESS;
	return kernel;
}

CL_API_ENTRY cl_int CL_API_CALL
clCreateKernelsInProgram(cl_program     program,
                         cl_uint        num_kernels,
                         cl_kernel *    kernels,
                         cl_uint *      num_kernels_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainKernel(cl_kernel    kernel) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseKernel(cl_kernel   kernel) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseKernel()\n"; );
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clSetKernelArg(cl_kernel    kernel,
               cl_uint      arg_index,
               size_t       arg_size,
               const void * arg_value) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nclSetKernelArg(" << kernel->function_wrapper->getNameStr() << ", " << arg_index << ", " << arg_size << ")\n"; );
	if (!kernel) return CL_INVALID_KERNEL;
	if (arg_index > kernel->get_num_args()) return CL_INVALID_ARG_INDEX;

	kernel->set_arg_data(arg_index, arg_value, arg_size);
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetKernelInfo(cl_kernel       kernel,
                cl_kernel_info  param_name,
                size_t          param_value_size,
                void *          param_value,
                size_t *        param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetKernelWorkGroupInfo(cl_kernel                  kernel,
                         cl_device_id               device,
                         cl_kernel_work_group_info  param_name,
                         size_t                     param_value_size,
                         void *                     param_value,
                         size_t *                   param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (!kernel) return CL_INVALID_KERNEL;
	//if (!device) return CL_INVALID_DEVICE;
	switch (param_name) {
		case CL_KERNEL_WORK_GROUP_SIZE:{
			*(size_t*)param_value = PACKETIZED_OPENCL_DRIVER_MAX_WORK_GROUP_SIZE; //simdWidth * maxNumThreads;
			break; // type conversion slightly hacked (should use param_value_size) ;)
		}
		case CL_KERNEL_COMPILE_WORK_GROUP_SIZE: {
			assert (false && "NOT IMPLEMENTED");
			break;
		}
		case CL_KERNEL_LOCAL_MEM_SIZE: {
			*(cl_ulong*)param_value = 0; // FIXME ?
			break;
		}
		default: return CL_INVALID_VALUE;
	}
	return CL_SUCCESS;
}

/* Event Object APIs  */
CL_API_ENTRY cl_int CL_API_CALL
clWaitForEvents(cl_uint             num_events,
                const cl_event *    event_list) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clWaitForEvents()\n"; );
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clGetEventInfo(cl_event         event,
               cl_event_info    param_name,
               size_t           param_value_size,
               void *           param_value,
               size_t *         param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clRetainEvent(cl_event event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clReleaseEvent(cl_event event) CL_API_SUFFIX__VERSION_1_0
{
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "TODO: implement clReleaseEvent()\n"; );
	return CL_SUCCESS;
}

/* Profiling APIs  */
CL_API_ENTRY cl_int CL_API_CALL
clGetEventProfilingInfo(cl_event            event,
                        cl_profiling_info   param_name,
                        size_t              param_value_size,
                        void *              param_value,
                        size_t *            param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Flush and Finish APIs */
CL_API_ENTRY cl_int CL_API_CALL
clFlush(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clFinish(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
	if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
	// do nothing :P
	return CL_SUCCESS;
}

/* Enqueued Commands APIs */
CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBuffer(cl_command_queue    command_queue,
                    cl_mem              buffer,
                    cl_bool             blocking_read,
                    size_t              offset,
                    size_t              cb,
                    void *              ptr,
                    cl_uint             num_events_in_wait_list,
                    const cl_event *    event_wait_list,
                    cl_event *          event) CL_API_SUFFIX__VERSION_1_0
{
	if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
	if (!buffer) return CL_INVALID_MEM_OBJECT;
	if (!ptr || buffer->get_size() < cb+offset) return CL_INVALID_VALUE;
	if (!event_wait_list && num_events_in_wait_list > 0) return CL_INVALID_EVENT_WAIT_LIST;
	if (event_wait_list && num_events_in_wait_list == 0) return CL_INVALID_EVENT_WAIT_LIST;
	if (command_queue->context != buffer->get_context()) return CL_INVALID_CONTEXT;
    //err = clEnqueueReadBuffer( commands, output, CL_TRUE, 0, sizeof(float) * count, results, 0, NULL, NULL );

	// Write data back into host memory (ptr) from device memory (buffer)
	// In our case, we actually should not have to copy data
	// because we are still on the CPU. However, const void* prevents this.
	// Thus, just copy over each byte.
	// TODO: specification seems to require something different?
	//       storing access patterns to command_queue or sth like that?
	
	void* data = buffer->get_data();
	memcpy(ptr, data, cb);

	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBuffer(cl_command_queue   command_queue,
                     cl_mem             buffer,
                     cl_bool            blocking_write,
                     size_t             offset,
                     size_t             cb,
                     const void *       ptr,
                     cl_uint            num_events_in_wait_list,
                     const cl_event *   event_wait_list,
                     cl_event *         event) CL_API_SUFFIX__VERSION_1_0
{
	if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
	if (!buffer) return CL_INVALID_MEM_OBJECT;
	if (!ptr || buffer->get_size() < cb+offset) return CL_INVALID_VALUE;
	if (!event_wait_list && num_events_in_wait_list > 0) return CL_INVALID_EVENT_WAIT_LIST;
	if (event_wait_list && num_events_in_wait_list == 0) return CL_INVALID_EVENT_WAIT_LIST;
	if (command_queue->context != buffer->get_context()) return CL_INVALID_CONTEXT;

	// Write data into 'device memory' (buffer)
	// In our case, we actually should not have to copy data
	// because we are still on the CPU. However, const void* prevents this.
	// Thus, just copy over each byte.
	// TODO: specification seems to require something different?
	//       storing access patterns to command_queue or sth like that?
	buffer->copy_data(ptr, cb, offset); //cb is size in bytes

	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBuffer(cl_command_queue    command_queue,
                    cl_mem              src_buffer,
                    cl_mem              dst_buffer,
                    size_t              src_offset,
                    size_t              dst_offset,
                    size_t              cb,
                    cl_uint             num_events_in_wait_list,
                    const cl_event *    event_wait_list,
                    cl_event *          event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadImage(cl_command_queue     command_queue,
                   cl_mem               image,
                   cl_bool              blocking_read,
                   const size_t         origin[3],
                   const size_t         region[3],
                   size_t               row_pitch,
                   size_t               slice_pitch,
                   void *               ptr,
                   cl_uint              num_events_in_wait_list,
                   const cl_event *     event_wait_list,
                   cl_event *           event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteImage(cl_command_queue    command_queue,
                    cl_mem              image,
                    cl_bool             blocking_write,
                    const size_t        origin[3],
                    const size_t        region[3],
                    size_t              input_row_pitch,
                    size_t              input_slice_pitch,
                    const void *        ptr,
                    cl_uint             num_events_in_wait_list,
                    const cl_event *    event_wait_list,
                    cl_event *          event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImage(cl_command_queue     command_queue,
                   cl_mem               src_image,
                   cl_mem               dst_image,
                   const size_t         src_origin[3],
                   const size_t         dst_origin[3],
                   const size_t         region[3],
                   cl_uint              num_events_in_wait_list,
                   const cl_event *     event_wait_list,
                   cl_event *           event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImageToBuffer(cl_command_queue command_queue,
                           cl_mem           src_image,
                           cl_mem           dst_buffer,
                           const size_t     src_origin[3],
                           const size_t     region[3],
                           size_t           dst_offset,
                           cl_uint          num_events_in_wait_list,
                           const cl_event * event_wait_list,
                           cl_event *       event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBufferToImage(cl_command_queue command_queue,
                           cl_mem           src_buffer,
                           cl_mem           dst_image,
                           size_t           src_offset,
                           const size_t     dst_origin[3],
                           const size_t     region[3],
                           cl_uint          num_events_in_wait_list,
                           const cl_event * event_wait_list,
                           cl_event *       event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY void * CL_API_CALL
clEnqueueMapBuffer(cl_command_queue command_queue,
                   cl_mem           buffer,
                   cl_bool          blocking_map,
                   cl_map_flags     map_flags,
                   size_t           offset,
                   size_t           cb,
                   cl_uint          num_events_in_wait_list,
                   const cl_event * event_wait_list,
                   cl_event *       event,
                   cl_int *         errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY void * CL_API_CALL
clEnqueueMapImage(cl_command_queue  command_queue,
                  cl_mem            image,
                  cl_bool           blocking_map,
                  cl_map_flags      map_flags,
                  const size_t      origin[3],
                  const size_t      region[3],
                  size_t *          image_row_pitch,
                  size_t *          image_slice_pitch,
                  cl_uint           num_events_in_wait_list,
                  const cl_event *  event_wait_list,
                  cl_event *        event,
                  cl_int *          errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueUnmapMemObject(cl_command_queue command_queue,
                        cl_mem           memobj,
                        void *           mapped_ptr,
                        cl_uint          num_events_in_wait_list,
                        const cl_event *  event_wait_list,
                        cl_event *        event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}


#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
inline cl_int executeRangeKernel1D(cl_kernel kernel, const size_t global_work_size, const size_t local_work_size) {
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  global_work_size: " << global_work_size << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  local_work_size: " << local_work_size << "\n"; );
	if (global_work_size % local_work_size != 0) return CL_INVALID_WORK_GROUP_SIZE;
	//if (global_work_size[0] > pow(2, sizeof(size_t)) /* oder so :P */) return CL_OUT_OF_RESOURCES;

#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
	typedef void (*kernelFnPtr)(const void*);
#else
	const size_t groupnr = global_work_size / local_work_size;
	const cl_uint argument_get_global_size = global_work_size;
	const cl_uint argument_get_local_size  = local_work_size;
	const cl_uint argument_get_num_groups  = groupnr == 0 ? 1 : groupnr;
	typedef void (*kernelFnPtr)(
			const void*,
			const cl_uint,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*);
#endif
	kernelFnPtr typedPtr = ptr_cast<kernelFnPtr>(kernel->get_compiled_function());

	const void* argument_struct = kernel->get_argument_struct();

	//
	// execute the kernel
	//
	const size_t num_iterations = global_work_size; // = total # threads
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "executing kernel (#iterations: " << num_iterations << ")...\n"; );

	unsigned i;
	#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	omp_set_num_threads(maxNumThreads);
	#pragma omp parallel for default(none) private(i) shared(argument_struct, typedPtr)
	#endif
	for (i=0; i<num_iterations; ++i) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\niteration " << i << "\n"; );

#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
		setCurrentGlobal(0, i);
		setCurrentGroup(0, i / local_work_size);
		setCurrentLocal(0, i % local_work_size);

		PACKETIZED_OPENCL_DRIVER_DEBUG(
			outs() << "  global id: " << get_global_id(0) << "\n";
			outs() << "  local id: " << get_local_id(0) << "\n";
			outs() << "  group id: " << get_group_id(0) << "\n";
			PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
		);

		typedPtr(argument_struct);
#else
		// TODO: optimize for constant case where group or local ids do not change?
		const cl_uint argument_get_global_id   = i;
		const cl_uint argument_get_group_id    = i / local_work_size;
		const cl_uint argument_get_local_id    = i % local_work_size;

		PACKETIZED_OPENCL_DRIVER_DEBUG(
			outs() << "  global id: " << argument_get_global_id << "\n";
			outs() << "  local id: " << argument_get_local_id << "\n";
			outs() << "  group id: " << argument_get_group_id << "\n";
			PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
		);

		typedPtr(
			argument_struct,
			1U, // get_work_dim
			&argument_get_global_size,
			&argument_get_global_id,
			&argument_get_local_size,
			&argument_get_num_groups,
			&argument_get_group_id,
			&argument_get_local_id
		);
#endif

		PACKETIZED_OPENCL_DRIVER_DEBUG(
			outs() << "iteration " << i << " finished!\n";
			PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
		);
	}

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "execution of kernel finished!\n"; );

	return CL_SUCCESS;
}
inline cl_int executeRangeKernelND(cl_kernel kernel, cl_uint num_dimensions, const size_t* global_work_size, const size_t* local_work_size) {

	#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	PACKETIZED_OPENCL_DRIVER_DEBUG( errs() << "WARNING: clEnqueueNDRangeKernels with work_dim > 1 currently does not support multithreading - falling back to single-thread mode!\n"; );
	#endif

	#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
	PACKETIZED_OPENCL_DRIVER_DEBUG( errs() << "WARNING: clEnqueueNDRangeKernels with work_dim > 1 currently does not allow using callbacks instead of arguments!\n"; );
	#endif

	typedef void (*kernelFnPtr)(
			const void*,
			const cl_uint,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*);
	kernelFnPtr typedPtr = ptr_cast<kernelFnPtr>(kernel->get_compiled_function());

	const void* argument_struct = kernel->get_argument_struct();

	// TODO: move allocation somewhere else
	size_t* num_groups = new size_t[num_dimensions](); // #groups per dimension
	size_t* cur_global = new size_t[num_dimensions](); // ids per dimension
	size_t* cur_local = new size_t[num_dimensions](); // ids per dimension
	size_t* cur_group = new size_t[num_dimensions](); // ids per dimension

	for (unsigned cur_work_dim=0; cur_work_dim < num_dimensions; ++cur_work_dim) {
		const size_t groupnr = global_work_size[cur_work_dim] / local_work_size[cur_work_dim];
		// if local size is larger than global size, groupnr is 0 but we have one group ;)
		num_groups[cur_work_dim] = groupnr == 0 ? 1 : groupnr;

		cur_global[cur_work_dim] = 0;
		cur_local[cur_work_dim] = 0;
		cur_group[cur_work_dim] = 0;
	}


	bool kernel_finished = false;
	do {

		bool group_finished = false;
		do {
			PACKETIZED_OPENCL_DRIVER_DEBUG(
				outs() << "\nexecuting kernel...\n  global:";
				for (unsigned i=0; i<num_dimensions; ++i) {
					outs() << " " << get_global_id(i);
				}
				outs() << "\n  local:";
				for (unsigned i=0; i<num_dimensions; ++i) {
					outs() << " " << get_local_id(i);
				}
				outs() << "\n  group:";
				for (unsigned i=0; i<num_dimensions; ++i) {
					outs() << " " << get_group_id(i);
				}
				outs() << "\n";
			);

			// execute kernel
			const cl_uint argument_get_work_dim = num_dimensions;

			const cl_uint argument_get_global_size[3] = { global_work_size[0] };
			const cl_uint argument_get_global_id[3] = { cur_global[0] };
			const cl_uint argument_get_local_size[3] = { local_work_size[0] };
			const cl_uint argument_get_num_groups[3] = { num_groups[0] };
			const cl_uint argument_get_group_id[3] = { cur_group[0] };
			const cl_uint argument_get_local_id[3] = { cur_local[0] };

			typedPtr(
					argument_struct,
					argument_get_work_dim,
					argument_get_global_size,
					argument_get_global_id,
					argument_get_local_size,
					argument_get_num_groups,
					argument_get_group_id,
					argument_get_local_id
			);

			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "kernel execution finished!\n"; );

			for (int cur_work_dim=num_dimensions-1; cur_work_dim >= 0; --cur_work_dim) {
				++cur_local[cur_work_dim];
				++cur_global[cur_work_dim];

				// if local work size is allowed to be larger than global work
				// size, we additionally need to check global id.
				if (cur_local[cur_work_dim] >= local_work_size[cur_work_dim] ||
						cur_global[cur_work_dim] >= global_work_size[cur_work_dim]) {
					if (cur_work_dim == 0) {
						group_finished = true;
						// globals are updated after group update
						break;
					}
					cur_local[cur_work_dim] = 0;

					//cur_global[cur_work_dim] -= local_work_size[cur_work_dim]
					cur_global[cur_work_dim] = cur_group[cur_work_dim] * local_work_size[cur_work_dim];
				} else {
					break;
				}
			}

		} while (!group_finished);



		// update group ids of all dimensions (leave untouched, increment, or reset)
		// This means we perform exactly one increment and at most one reset
		for (int cur_work_dim=num_dimensions-1; cur_work_dim >= 0; --cur_work_dim) {
			// Increment group id of dimension for next iteration
			++cur_group[cur_work_dim];

			if (cur_group[cur_work_dim] >= num_groups[cur_work_dim]) {
				// Dimension is finished: Reset corresponding group index to 0
				// If this is the outermost loop (cur_work_dim = 0), stop iterating.
				// (This means all dimensions have reached their max index.)
				if (cur_work_dim == 0) {
					kernel_finished = true;
					break;
				}
				cur_group[cur_work_dim] = 0;
			} else {
				// Otherwise, this dimension has not reached its maximum,
				// so we stop updating (= leave all outer indices untouched).
				break;
			}
		}

		if (kernel_finished) break;

		// update global ids using info of new group
		for (int cur_work_dim=num_dimensions-1; cur_work_dim >= 0; --cur_work_dim) {
			cur_global[cur_work_dim] = cur_group[cur_work_dim] * local_work_size[cur_work_dim];
		}

	} while (!kernel_finished);

	delete [] num_groups;
	delete [] cur_global;
	delete [] cur_local;
	delete [] cur_group;

	return CL_SUCCESS;
}
#else
inline cl_int executeRangeKernel1DPacket(cl_kernel kernel, const size_t global_work_size, const size_t local_work_size) {
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  global_work_size: " << global_work_size << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  local_work_size: " << local_work_size << "\n"; );
	if (global_work_size % local_work_size != 0) return CL_INVALID_WORK_GROUP_SIZE;
	//if (global_work_size[0] > pow(2, sizeof(size_t)) /* oder so :P */) return CL_OUT_OF_RESOURCES;

#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
	typedef void (*kernelFnPtr)(const void*);
#else
	const size_t groupnr = global_work_size / local_work_size;
	const cl_uint argument_get_global_size = global_work_size;
	const cl_uint argument_get_local_size  = local_work_size;
	const cl_uint argument_get_num_groups  = groupnr == 0 ? 1 : groupnr;
	const __m128i argument_get_local_id_SIMD  = _mm_set_epi32(3, 2, 1, 0);
	typedef void (*kernelFnPtr)(
			const void*,
			const cl_uint,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const cl_uint*,
			const __m128i*,
			const __m128i*);
#endif
	kernelFnPtr typedPtr = ptr_cast<kernelFnPtr>(kernel->get_compiled_function());

	const void* argument_struct = kernel->get_argument_struct();

	//
	// execute the kernel
	//
	const size_t num_iterations = global_work_size / simdWidth; //local_work_size; // = #groups
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nexecuting kernel (#iterations: " << num_iterations << ")...\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "global_size(" << 0 << "): " << get_global_size(0) << "\n"; );

	unsigned i;
	#ifdef PACKETIZED_OPENCL_DRIVER_USE_OPENMP
	omp_set_num_threads(maxNumThreads);
	#pragma omp parallel for default(none) private(i) shared(argument_struct, typedPtr)
	#endif
	for (i=0; i<num_iterations; ++i) {
		PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\niteration " << i << "\n"; );

#ifdef PACKETIZED_OPENCL_DRIVER_USE_CALLBACKS
		setCurrentGlobal(0, i);
		setCurrentGroup(0, i);

		PACKETIZED_OPENCL_DRIVER_DEBUG(
			outs() << "  current global: " << i << "\n";
			outs() << "  get_global_id: " << get_global_id(0) << "\n";
			outs() << "  get_global_id_SIMD: "; printV(get_global_id_SIMD(0)); outs() << "\n";
			PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
		);

		typedPtr(argument_struct);
#else
		const cl_uint argument_get_global_id      = i;
		const cl_uint argument_get_group_id       = i;
		const unsigned id0 = i * 4;
		const __m128i argument_get_global_id_SIMD = _mm_set_epi32(id0 + 3, id0 + 2, id0 + 1, id0);

		typedPtr(
			argument_struct,
			1U,
			&argument_get_global_size,
			&argument_get_global_id,
			&argument_get_local_size,
			&argument_get_num_groups,
			&argument_get_group_id,
			&argument_get_global_id_SIMD,
			&argument_get_local_id_SIMD
		);
#endif

		PACKETIZED_OPENCL_DRIVER_DEBUG(
			outs() << "  iteration " << i << " finished!\n";
			PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
		);
	}

	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "execution of kernel finished!\n"; );

	return CL_SUCCESS;
}
inline cl_int executeRangeKernelNDPacket(cl_kernel kernel, cl_uint num_dimensions, const size_t* global_work_size, const size_t* local_work_size) {
	errs() << "ERROR: clEnqueueNDRangeKernels with work_dim > 1 currently does not support packetization!\n";
	assert (false && "NOT IMPLEMENTED!");
	return CL_INVALID_PROGRAM_EXECUTABLE; // just return something != CL_SUCCESS :P
}
#endif

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueNDRangeKernel(cl_command_queue command_queue,
                       cl_kernel        kernel,
                       cl_uint          work_dim,
                       const size_t *   global_work_offset,
                       const size_t *   global_work_size,
                       const size_t *   local_work_size,
                       cl_uint          num_events_in_wait_list,
                       const cl_event * event_wait_list,
                       cl_event *       event) CL_API_SUFFIX__VERSION_1_0
{
	const unsigned num_dimensions = work_dim; // rename for better understandability ;)
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\nclEnqueueNDRangeKernel(" << kernel->function_wrapper->getNameStr() << ")\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  work_dims: " << num_dimensions << "\n"; );
	PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "  num_events_in_wait_list: " << num_events_in_wait_list << "\n"; );
	if (!command_queue) return CL_INVALID_COMMAND_QUEUE;
	if (!kernel) return CL_INVALID_KERNEL;
	if (command_queue->context != kernel->get_context()) return CL_INVALID_CONTEXT;
	//if (command_queue->context != event_wait_list->context) return CL_INVALID_CONTEXT;
	if (num_dimensions < 1 || num_dimensions > maxNumDimensions) return CL_INVALID_WORK_DIMENSION;
	if (!kernel->get_compiled_function()) return CL_INVALID_PROGRAM_EXECUTABLE; // ?
	if (!global_work_size) return CL_INVALID_GLOBAL_WORK_SIZE;
	if (!local_work_size) return CL_INVALID_WORK_GROUP_SIZE;
	if (global_work_offset) return CL_INVALID_GLOBAL_OFFSET; // see specification p.109
	if (!event_wait_list && num_events_in_wait_list > 0) return CL_INVALID_EVENT_WAIT_LIST;
	if (event_wait_list && num_events_in_wait_list == 0) return CL_INVALID_EVENT_WAIT_LIST;

	//
	// set up runtime
	// TODO: try to get rid of this!
	//
	const unsigned simd_dim = 0; // ignored if packetization is disabled
	assert (simd_dim < num_dimensions);
	initializeOpenCL(num_dimensions, simd_dim, global_work_size, local_work_size);

	// DON'T USE local_work_size BELOW UNTIL ISSUE WITH size < 4 IS SOLVED !

#ifdef PACKETIZED_OPENCL_DRIVER_FORCE_ND_ITERATION_SCHEME
	#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION
	return executeRangeKernelND(kernel, num_dimensions, globalThreads, localThreads);
	#else
	return executeRangeKernelNDPacket(kernel, num_dimensions, globalThreads, localThreads);
	#endif
#endif

#ifdef PACKETIZED_OPENCL_DRIVER_NO_PACKETIZATION

	if (num_dimensions == 1) {
		return executeRangeKernel1D(kernel, globalThreads[0], localThreads[0]);
	} else {
		return executeRangeKernelND(kernel, num_dimensions, globalThreads, localThreads);
	}

#else

	if (num_dimensions == 1) {
		return executeRangeKernel1DPacket(kernel, globalThreads[0], localThreads[0]);
	} else {
		return executeRangeKernelNDPacket(kernel, num_dimensions, globalThreads, localThreads);
	}
	
#endif
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueTask(cl_command_queue  command_queue,
              cl_kernel         kernel,
              cl_uint           num_events_in_wait_list,
              const cl_event *  event_wait_list,
              cl_event *        event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueNativeKernel(cl_command_queue  command_queue,
					  void (*user_func)(void *),
                      void *            args,
                      size_t            cb_args,
                      cl_uint           num_mem_objects,
                      const cl_mem *    mem_list,
                      const void **     args_mem_loc,
                      cl_uint           num_events_in_wait_list,
                      const cl_event *  event_wait_list,
                      cl_event *        event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueMarker(cl_command_queue    command_queue,
                cl_event *          event) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWaitForEvents(cl_command_queue command_queue,
                       cl_uint          num_events,
                       const cl_event * event_list) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueBarrier(cl_command_queue command_queue) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return CL_SUCCESS;
}

/* Extension function access
 *
 * Returns the extension function address for the given function name,
 * or NULL if a valid function can not be found.  The client must
 * check to make sure the address is not NULL, before using or
 * calling the returned function address.
 */
CL_API_ENTRY void * CL_API_CALL clGetExtensionFunctionAddress(const char * func_name) CL_API_SUFFIX__VERSION_1_0
{
	assert (false && "NOT IMPLEMENTED!");
	return NULL;
}

#ifdef __cplusplus
}
#endif



/*
for (i=0; i<num_iterations; ++i) {
			PACKETIZED_OPENCL_DRIVER_DEBUG( outs() << "\niteration " << i << "\n"; );

			// update runtime environment
			setCurrentGlobal(cur_work_dim, i);
			setCurrentGroup(cur_work_dim, i / lws);
			setCurrentLocal(cur_work_dim, i % lws);

			PACKETIZED_OPENCL_DRIVER_DEBUG(
				outs() << "  global id: " << get_global_id(cur_work_dim) << "\n";
				outs() << "  local id: " << get_local_id(cur_work_dim) << "\n";
				outs() << "  group id: " << get_group_id(cur_work_dim) << "\n";

				//hardcoded debug output for simpleTest
				//typedef struct { float* input; float* output; unsigned count; } tt;
				//outs() << "  input-addr : " << ((tt*)kernel->get_argument_struct())->input << "\n";
				//outs() << "  output-addr: " << ((tt*)kernel->get_argument_struct())->output << "\n";
				//outs() << "  input : " << ((tt*)kernel->get_argument_struct())->input[i] << "\n";
				//outs() << "  output: " << ((tt*)kernel->get_argument_struct())->output[i] << "\n";
				//outs() << "  count : " << ((tt*)kernel->get_argument_struct())->count << "\n";

				//hardcoded debug output for DwtHaar1D
				//PacketizedOpenCLDriver::writeFunctionToFile(kernel->function_wrapper, "executed.ll");
				//typedef struct { float* inSignal; float *coefsSignal; float *AverageSignal; float *sharedArray;
				//unsigned tLevels; unsigned signalLength; unsigned levelsDone; unsigned mLevels; } tt;
				//outs() << "  inSignal : " << ((tt*)kernel->get_argument_struct())->inSignal << "\n";
				//outs() << "  coefsSignal : " << ((tt*)kernel->get_argument_struct())->coefsSignal << "\n";
				//outs() << "  AverageSignal : " << ((tt*)kernel->get_argument_struct())->AverageSignal << "\n";
				//outs() << "  sharedArray : " << ((tt*)kernel->get_argument_struct())->sharedArray << "\n";
				////outs() << "  tLevels : " << ((tt*)kernel->get_argument_struct())->tLevels << "\n";
				//outs() << "  signalLength : " << ((tt*)kernel->get_argument_struct())->signalLength << "\n";
				//outs() << "  levelsDone : " << ((tt*)kernel->get_argument_struct())->levelsDone << "\n";
				//outs() << "  mLevels : " << ((tt*)kernel->get_argument_struct())->mLevels << "\n";
				//const float* arr = (float*)((tt*)kernel->get_argument_struct())->inSignal;
				//float before[64];
				//for (unsigned j=0; j<64; ++j) {
				//before[j] = arr[j];
				//}

				//hardcoded debug output for Histogram
				//typedef struct { unsigned* data; unsigned* sharedArray; unsigned* binResult; } tt;
				//outs() << "  data: " << ((tt*)kernel->get_argument_struct())->data << "\n";
				//outs() << "  sharedArray: " << ((tt*)kernel->get_argument_struct())->sharedArray << "\n";
				//outs() << "  binResult: " << ((tt*)kernel->get_argument_struct())->binResult << "\n";
				//const unsigned* arr = (unsigned*)((tt*)kernel->get_argument_struct())->data;
				//outs() << "  data:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr[j] << "\n";
				//}
				//outs() << "-------\n";
				//const unsigned* arr2 = (unsigned*)((tt*)kernel->get_argument_struct())->sharedArray;
				//outs() << "  sharedArray:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr2[j] << "\n";
				//}
				//outs() << "-------\n";
				//const unsigned* arr3 = (unsigned*)((tt*)kernel->get_argument_struct())->binResult;
				//outs() << "  binResult:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr3[j] << "\n";
				//}
				//outs() << "-------\n";

				PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
				//outs() << "  verification before execution successful!\n";
			);

			// call kernel
			typedPtr(argument_struct);

			PACKETIZED_OPENCL_DRIVER_DEBUG(
				//hardcoded debug output for SimpleTest
				//typedef struct { float* input; float* output; unsigned count; } tt;
				//outs() << "  result: " << ((tt*)kernel->get_argument_struct())->output[i] << "\n";

				//hardcoded debug output for Histogram
				//typedef struct { unsigned* data; unsigned* sharedArray; unsigned* binResult; } tt;
				//const unsigned* arr = (unsigned*)((tt*)kernel->get_argument_struct())->data;
				//outs() << "  data:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr[j] << "\n";
				//}
				//outs() << "-------\n";
				//const unsigned* arr2 = (unsigned*)((tt*)kernel->get_argument_struct())->sharedArray;
				//outs() << "  sharedArray:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr2[j] << "\n";
				//}
				//outs() << "-------\n";
				//const unsigned* arr3 = (unsigned*)((tt*)kernel->get_argument_struct())->binResult;
				//outs() << "  binResult:\n-------\n";
				//for (unsigned j=0; j<8; ++j) {
				//outs() << "    " << arr3[j] << "\n";
				//}
				//outs() << "-------\n";

				outs() << "  iteration " << i << " finished!\n";
				PacketizedOpenCLDriver::verifyModule(kernel->get_program()->module);
				//outs() << "  verification after execution successful!\n";
			);
		}
*/