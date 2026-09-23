// Copyright © 2009 CCP ehf.
/* 
	*************************************************************************

	BlockAllocator.cpp

	Project:   EVE Server Database Access

	Description:   

		A simple class to dole out stuff, with a single cleanup at the end.
		
	Dependencies:

		Python

	*************************************************************************
*/

#include "StdAfx.h"
#include "BlockAllocator.h"

//Block allocator members
SimplePoolAllocator::SimplePoolAllocator(size_t blockSize) : mBlockSize(blockSize)
{
	mHead = mBigHead = 0;
	mCurrent = 0;
	mCurrentLeft = 0;
}

SimplePoolAllocator::~SimplePoolAllocator()
{
	free();
}

void *SimplePoolAllocator::malloc(size_t s)
{
#ifdef _WIN64
	return align(s, 16);
#else
	return align(s, 8);
#endif
}

void *SimplePoolAllocator::align(size_t s, int a)
{
	//use big blocks to reduce fragmentation
	if (s > mBlockSize>>1)
		return AddBigBlock(s, a);

	//round the cursor to the alignment
	char *p = RoundUp(mCurrent, a);
	ptrdiff_t d = p-mCurrent;
	if (s + d > mCurrentLeft) {
		if (!AddBlock(s, a))
			return 0;
		//block is aligned now
		d = 0;
	}
	void *result = mCurrent+d;
	_ASSERT(RoundUp((char*)result, a) == (char*)result);
	_ASSERT(mCurrentLeft >= s + d);

	mCurrent += s + d;
	mCurrentLeft -= s + d;
	return result;
}
		
char *SimplePoolAllocator::RoundUp(char *p, int align)
{
	return (char*)(((intptr_t)p+align-1) & (~(align-1)));
}

size_t SimplePoolAllocator::RoundUp(size_t s, int align)
{
	return (s+align-1) & (~(align-1));
}

bool SimplePoolAllocator::AddBlock(size_t s, int a)
{
		//Support only lower alignment than the defult currently
	//(we get default alignment from the underying alloc
#ifdef _WIN64
	_ASSERT(a <= 16);
#else
	_ASSERT(a <= 8);
#endif
	//how big should the block be?
	const size_t hsize = RoundUp(sizeof(void*), a);
	const size_t min_bsize = s + hsize;
	size_t bsize = min_bsize;
	if (bsize < mBlockSize)
		bsize = mBlockSize;

	//alloc it  (here we should catch exceptions if we ever enable them)
	char *block = (char*)CCP_MALLOC("SimplePoolAllocator", bsize);
	if (!block) {
		//ok, go for the minimum
		bsize = min_bsize;
		block = (char*)CCP_MALLOC("SimplePoolAllocator", bsize);
		if (!block)
			return false; //give up
	}

	//link it in
	*reinterpret_cast<void**>(block) = mHead;
	mHead = block;

	//set allocation space past the link pointer
	mCurrent = block + hsize;
	mCurrentLeft = bsize - hsize;
	_ASSERT(mCurrentLeft >= s);
	_ASSERT(RoundUp(mCurrent, a) == mCurrent);
	return true;
}

void *SimplePoolAllocator::AddBigBlock(size_t s, int a)
{
		//Support only lower alignment than the defult currently
	//(we get default alignment from the underying alloc
#ifdef _WIN64
	_ASSERT(a <= 16);
#else
	_ASSERT(a <= 8);
#endif
	//how big should the block be?
	const size_t hsize = RoundUp(sizeof(void*), a);
	const size_t bsize = s + hsize;
	
	//alloc it  (here we should catch exceptions if we ever enable them)
	char *block = (char*)CCP_MALLOC("SimplePoolAllocator", bsize);
	if (!block)
		return 0;
		
	void *result = block+hsize;

	//link it in
	*reinterpret_cast<void**>(block) = mBigHead;
	mBigHead = block;
	return result;
}


void  SimplePoolAllocator::free()
{
	while (mHead) {
		void *next = *reinterpret_cast<void**>(mHead);
		CCP_FREE(mHead);
		mHead = next;
	}
	while (mBigHead) {
		void *next = *reinterpret_cast<void**>(mBigHead);
		CCP_FREE(mBigHead);
		mBigHead = next;
	}
}
