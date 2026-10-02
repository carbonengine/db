// Copyright © 2014 CCP ehf.
#include "StdAfx.h"
#include "DelayedException.h"
#include "Utils.h"


DelayedException::DelayedException(const char *msg, HRESULT hr, PyObject *cls) :
	mMsg(msg), mHR(hr), mCls(cls), mNErrors(0)
{
	if (FAILED(hr)) {
		HRESULT myhr = mErrorInfo.GetErrorRecords(&mNErrors);
		if (FAILED(myhr))
			mNErrors = 0;
	}
}

DelayedException *DelayedException::New(HRESULT hr, const char *msg)
{
	DelayedException *r = new(std::nothrow) DelayedException(msg, hr, 0);
	if (!r)
		r = Dummy();
	return r;
}

DelayedException *DelayedException::New(PyObject *cls, const char *msg)
{
	DelayedException *r = new(std::nothrow) DelayedException(msg, S_OK, cls);
	if (!r)
		r = Dummy();
	return r;
}

DelayedException *DelayedException::New(const char *msg)
{
	DelayedException *r = new(std::nothrow) DelayedException(msg, S_OK, Utilities::DbExc_RuntimeError);
	if (!r)
		r = Dummy();
	return r;
}

DelayedException *DelayedException::NoMem(const char *msg, ...)
{
	return Dummy();
}

void DelayedException::operator delete(void *p)
{
	if (static_cast<DelayedException*>(p) != Dummy())
		::delete(p);
}

bool DelayedException::Raise() {
	// raise all warnings/exceptions, the last one first
	if (mNext.get()) {
		if (!mNext->Raise())
			return false;
	}

	if (FAILED(mHR)) {
		if (mNErrors) {
			std::vector<std::wstring> records;
			Utilities::FormatErrorRec(records, mNErrors, mErrorInfo);
		}
		//Todo, this is not used, but we should pass the correct args to SQLError
		PyErr_Format(Utilities::ErrorClass(mHR),
					 "DelayedException HRESULT %d, msg %s, errorRecords:%d",
					 mHR, mMsg.c_str(), mNErrors);
	}
	else
	{
		if (PyType_IsSubtype((PyTypeObject*)mCls, (PyTypeObject*)PyExc_Warning)) {
			if (PyErr_WarnEx(mCls, mMsg.c_str(), 1) == 0)
				return true;  //just a warning then.
		} else
			PyErr_SetString(mCls, mMsg.c_str());
	}
	return false;
}

// We don't always want to drop our DB session.  If the error is sufficiently "soft", we hang on to it.
bool DelayedException::IsSessionFatal() const
{
	if (!FAILED(mHR))
		return false;

	return !Utilities::IsSoftError(mHR);
}
