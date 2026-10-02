// Copyright © 2005 CCP ehf.

/* 
	*************************************************************************

	TmpRowset.h

	Project:   EVE Server Database Access

	Description:   

		Classes that store a rowset temporarily during traversal. We cannot
		create python rowsets directly since this happens on a thread, and python
		is'nt multithreaded.  So, here we create something very similar to python
		rowsets.
		
	Dependencies:

		Python

	*************************************************************************
*/

#ifndef _TMPROWSET_H_
#define _TMPROWSET_H_

#include <string.h> //for _strdup()
#include <vector>
#include <string>
#include <atlstr.h>
#include <atldbcli.h>
#include "Utils.h"
#include "DelayedException.h"
#include "StringStore.h"
#include "BlockAllocator.h"
#include "RowDescriptor.h"
#include "Row.h"
#include "Accessor.h"

//A temporary thing, use this to locate the place where we are getting error results with no exception
template <class T>
void ForceException(T v, const char *msg){
	if (!v && !PyErr_Occurred( )) {
		std::string s("Forced Exception, Notify Kris! : ");
		s += msg;
		PyErr_SetString(PyExc_RuntimeError, s.c_str());
	}
}

class ToPythonCtxt
{
public:
	ToPythonCtxt(PyObject *blue, PyObject *beNice, int beNiceEvery, PyObject *getMem) :
		mBlue(blue), mBeNice(beNice), mBeNiceEvery(beNiceEvery), mGetMem(getMem)
	{
		mRowCounter = mTotalPyBytes = mLastPyBytes = 0;
	}
	size_t GetMem() const
	{
		if (!mGetMem)
			return 0;
		BluePy m(PyObject_CallObject(mGetMem, 0));
		if (m) {
			size_t now;
			if (sizeof(size_t) == 4)
				now = PyLong_AsUnsignedLongMask( m );
			else
				now = (size_t)PyLong_AsUnsignedLongLongMask( m );
			if (now != (size_t)-1)
				return now;
		}
		PyErr_Clear();
		return 0;
	}
	bool BeNice() const
	{
		if (!mBeNice)
			return true;
		BluePy r(PyObject_CallObject(mBeNice, 0));
		return r ? true : false;
	}

	PyObject *mBlue;
	PyObject *mBeNice;
	PyObject *mGetMem;
	size_t	mRowCounter;
	size_t	mTotalPyBytes;
	size_t	mLastPyBytes;
	int mBeNiceEvery;
};

class TmpRowset
{
public:
	TmpRowset(NSession *s, SimplePoolAllocator &allocator, Store& store) :
		mRD(s), mRC(DB_COUNTUNAVAILABLE), mAllocator(allocator), mStore(store), mRows(rows_a(allocator))
	{}
	typedef CAccessorRowset<ACCESSOR, CBulkRowset> rowset_t;
	DelayedException *Get(DBLENGTH &recvLen, rowset_t &a);
	~TmpRowset();

	//dissolve and change to python;
	PyObject *ToPython(ToPythonCtxt &ctxt);

	DBROWCOUNT mRC;

private:
	//typedef std::vector<Row*> rows_t;
	typedef StlPoolAllocator<Row*> rows_a;
	typedef std::list<Row*, rows_a> rows_t;
	typedef rows_t::iterator rows_i;

	RowDescriptor mRD;
	rows_t mRows;
	SimplePoolAllocator &mAllocator;
	Store& mStore;
};


class TmpRowsetList
{
public:
	TmpRowsetList() : mProcResult(0) , mAllocator(8*1024), mStore(mAllocator){}
	~TmpRowsetList();
	typedef CCommand<ACCESSOR, CBulkRowset, CMultipleResults> command_t;
	DelayedException *Get(DBLENGTH &recvLen, NSession *sess, command_t &cmd, DBROWCOUNT rc);

	//dissolve and change to python.
	PyObject *ToPython(ToPythonCtxt &ctxt);
	size_t GetMemSaved() const { return mStore.GetMemSaved(); }

private:
	typedef std::vector<TmpRowset*> rowsets_t;
	typedef rowsets_t::iterator rowsets_i;
	
	rowsets_t mRowsets;
	int mProcResult; //used when list is empty
	SimplePoolAllocator mAllocator;
	Store mStore;
};


#endif //_TMPROWSET_H_

