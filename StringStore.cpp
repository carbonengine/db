// Copyright © 2014 CCP ehf.
#include "StdAfx.h"
#include "StringStore.h"

template <>
bool InternalStoreElement<char, true>::ToPython()
{
	_ASSERT( !mPython );

	PyObject* p = PyUnicode_FromStringAndSize( mData, mElements );

	if(!p)
	{
		return false;
	}

	mPython = true;
	mObject = p;

	return true;
}

template <>
bool InternalStoreElement<char, false>::ToPython()
{
	_ASSERT( !mPython );

	PyObject* p = PyBytes_FromStringAndSize( mData, mElements );

	if(!p)
	{
		return false;
	}

	mPython = true;
	mObject = p;

	return true;
}

template <>
bool InternalStoreElement<wchar_t, true>::ToPython()
{
	_ASSERT( !mPython );

	PyObject* p = PyUnicode_FromWideChar( mData, mElements );

	if(!p)
	{
		return false;
	}

	mPython = true;
	mObject = p;

	return true;
}