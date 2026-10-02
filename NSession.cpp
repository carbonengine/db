// Copyright © 2014 CCP ehf.
#include "StdAfx.h"
#include "NSession.h"
#include <Scheduler.h>

#include "Utils.h"
using Utilities::DbExc_RuntimeError;

static CcpLogChannel_t s_chNSession = CCP_LOG_DEFINE_CHANNEL( "NSession" );

extern "C" const CLSID CLSID_DataConvert;

//tracing
#if 1
#define DBTRACE ATLTRACE
#else
#define DBTRACE __noop
#endif


ITaskletTimer *ttimer = 0;



/////////////////////////////////////////////
// The NSession::Request

//Raise a request exception
PyObject *NSession::Request::Raise()
{
	DBTRACE("Got exception in req %p\n", this);
	if (mException.get() == DelayedException::Dummy())
		return PyErr_NoMemory();
	if (FAILED(mException->mHR))
		mCommand.Raise(mException->mMsg.c_str(), mException->mHR, &mException->mErrorInfo, mException->mNErrors);
	else
		mException->Raise();
	return 0;
}

bool NSession::Request::GetSession(int timerDetail)
{
	AUTOTASKLETC("DB::Request::GetSession", timerDetail>=2);
	_ASSERT(!mSession);
	if (!mSessionKeeper.GetSession(mSession))
		return false;
	_ASSERT(mSession);
	return true;
}

void NSession::Request::ReleaseSession()
{
	if (mSession) {
		mSessionKeeper.ReturnSession();
		mSession = 0;
	}
}


void NSession::Request::DiscardSession()
{
	if (mSession) {
		mSessionKeeper.DiscardSession();
		mSession = 0;
	}
}


NSession::NSession()
{
	mWeakrefList = 0;
	if (!ttimer)
		ttimer = PyOS->GetTaskletTimer();
	DBTRACE("Create session %p\n", this);
	mLastWallclockTime = mLastKernelTime = mLastUserTime = 0.0;
	mLastPyBytes = 0;
	mTotalBytesSentParam = mTotalBytesReceived = 0;
	mBlobSizeLimit = 2048*1024; //2MB blob size limit - matches nvarchar(MAX)
	mAllowSync = 0;
	mTimerDetail = 0;
}


NSession::~NSession()
{
	if (mSessionPool)
		mSessionPool->Fini();
	if (mWeakrefList)
		PyObject_ClearWeakRefs(this);
	DBTRACE("Session %p dead\n", this);
}


PyObject *NSession::_New(PyTypeObject *subtype, PyObject *args, PyObject *kw)
{
	NSession *s = static_cast<NSession*>(Parent::_New(subtype, args, kw));
	if (! s->Init(args)) {
		Py_DECREF(s);
		return 0;
	}
	return s;
}


bool NSession::Init(PyObject *args)
{
	const char *initstr;
	if (!PyArg_ParseTuple(args, "s", &initstr))
		return false;

	HRESULT hr = mConv.CoCreateInstance(CLSID_DataConvert, NULL, CLSCTX_INPROC_SERVER);
	if (FAILED(hr))
		return Utilities::SetErr32(hr, "Creating data converter") ,false;

	hr = mDataSource.OpenFromInitializationString(CA2W(initstr));
	if (FAILED(hr))
		return Utilities::SetErr32(hr, "CDataSource::OpenFromInitializationString") ,false;
	mSessionPool = SessionPoolPtr(new SessionPool(mDataSource));
	if (!mSessionPool->Init())
		return false;
	mSessionPool->mTimerDetail = mTimerDetail;

	//prime the scehma
	GetSchemaB(false);

	//get blue.pyos.BeNice and blue.pyos.synchro.Yield
	mBlue = BluePy(PyImport_Import(BluePyStr("blue")));
	if (!mBlue) return false;
	BluePy i = BluePy(PyObject_GetAttrString(mBlue, "pyos"));
	if (!i)	return false;

	mBeNice = BluePy(PyObject_GetAttrString(i, "BeNice"));
	mBeNiceEvery = 1000;
	if (!mBeNice) {
		PyErr_Clear(); //oh, well
		mBeNiceEvery = 0;
	}

	i = BluePy(PyObject_GetAttrString(i, "synchro"));
	if (!i) return false;
	mYield = BluePy(PyObject_GetAttrString(i, "Yield"));
	if (!mYield) return false;

	mGetMem = BluePy(PyImport_Import(BluePyStr("sys")));
	if (!mGetMem)
		return false;
	mGetMem = BluePy(PyObject_GetAttrString(mGetMem, "getpymalloced"));
	if (!mGetMem)
		PyErr_Clear(); //oh well, not supported
	
	return true;
}


PyObject *NSession::GetSchema(PyObject *argv)
{
	PyObject *refresh = Py_False;
	if (!PyArg_ParseTuple(argv, "|O:GetSchema", &refresh))
		return 0;
	PyObject* ok = GetSchemaB( PyObject_IsTrue( refresh ) );
	if (!ok)
		return 0;
	CCP_ASSERT(mSchema);
	return mSchema.NewRef();
}


//Returns a borrowed reference to the schema
PyObject *NSession::GetSchemaB(bool refresh)
{
	if (mSchema && !refresh)
		return mSchema.o;
		
	// Get datasource db schema, need server and database name
	CComVariant ds;
	CComVariant cat;
	CHECKERR(
		mDataSource.GetProperty(DBPROPSET_DATASOURCEINFO, DBPROP_DATASOURCENAME, &ds),
		"GetSchema: Getting datasource name"
		);
	CHECKERR(
		mDataSource.GetProperty(DBPROPSET_DATASOURCE, DBPROP_CURRENTCATALOG, &cat),
		"GetSchema: Getting datasource name"
		);

	if (!mConnection) {
		// Create schema
		bool ok = BeClasses->CreateInstance( GetConnectionClsid(), BlueInterfaceIID<Connection>(), (void**)&mConnection );
		if (!ok)
			return Utilities::SetErrBlue("");
	}

	CSession *s = NULL;
	if (!mSessionPool->GetSession(s) || !s)
		return 0;
	BluePy tmp(mConnection->GetSchema(*s, refresh));
	if (!tmp) {
		mSessionPool->DiscardSession(s);
		return 0;
	}
	mSessionPool->ReturnSession(s);
	if (!mSessionPool->EndSession())
		return 0;
	mSchema = tmp;
	return mSchema.o;
}


//--------------------------------------------------------------------
// NSession::Execute   perform the query
//--------------------------------------------------------------------
PyObject *NSession::Execute(PyObject *args)
{
	AUTOTASKLET0("DB::NSession::Execute");
	// threaded or direct mode, based on blocking status 

	PyTaskletObject *current = (PyTaskletObject *)SchedulerAPI()->PyScheduler_GetCurrent();
	bool noblock = SchedulerAPI()->PyTasklet_IsMain(current) || SchedulerAPI()->PyTasklet_GetBlockTrap(current);
	Py_DECREF(current);

	if (noblock && !mAllowSync)
		//this is a tasklet that cannot block, so execution might as well be done on this thread
		return PyErr_SetString(PyExc_RuntimeError, "This tasklet cannot block, and synchronous calls are not allowed"), nullptr;

	BluePy result;
	if (noblock) {
		//this is a tasklet that cannot block, so execution might as well be done on this thread
		Request req(this);
		if (Prepare(req, args)) {
			result = BluePy(ExecuteDirectly(req));
			ForceException(result.o, "ExecuteDirectly");
		} else
			ForceException(false, "Prepare");
	} else {

		//indirect execution (in a worker thread).  must allocate controls stuff on heap, Stackless messes with stack.
		//otherwise, when the thread runs, this tasklet may be somewhere else, causing a crash
		auto req = std::make_shared<Request>( this );
		if (!req)
			return PyErr_NoMemory();

		if (Prepare(*req, args)) {
			result = BluePy(ExecuteBlock(*req));
			ForceException(result.o, "ExecuteBlock");
		} else
			ForceException(false, "Prepare");
	}
	
	return result.Detach();
}


bool NSession::Prepare(Request &req, PyObject *args)
{
	AUTOTASKLET1("DB::NSession::Prepare");
	if (mBlobSizeLimit>=0)
		req.mCommand.SetBlobSizeLimit(mBlobSizeLimit);
	size_t len;
	bool r = req.GetSession(mTimerDetail);
	if (!r)
		return r;
	r = req.mCommand.Prepare(len, req.mSession, GetSchemaB(false), args);
	if (r)
		mTotalBytesSentParam += len;
	return r;
}


PyObject *NSession::ExecuteDirectly(Request &req)
{
	AUTOTASKLET1("DB::NSession::ExecuteDirectly");
	req.Execute();
	if (req.mException.get())
		return req.Raise();
	
	mTotalBytesReceived += req.mBytesReceived;
	mLastWallclockTime = req.mWallclockTime;
	mLastKernelTime = req.mKernelTime;
	mLastUserTime = req.mUserTime;
	ToPythonCtxt ctxt(mBlue, 0, 0, mGetMem);
	PyObject *res = req.mResult.ToPython(ctxt);
	mLastPyBytes = ctxt.mTotalPyBytes;
	mLastStringReuse = (int)req.mResult.GetMemSaved();
	return res;
}


PyObject *NSession::ExecuteBlock(Request &req)
{
	{
		AUTOTASKLET1("DB::NSession::WaitForData");
		try {
			if(!req.ExecuteAndWait())
			{
				return nullptr;
			}
		} catch(std::exception &e) {
			return Ccp::PyErrFromException(e);
		}
	}

	if (req.mException.get())
		return req.Raise();
	
	mTotalBytesReceived += req.mBytesReceived;
	mLastWallclockTime = req.mWallclockTime;
	mLastKernelTime = req.mKernelTime;
	mLastUserTime = req.mUserTime;
	mLastStringReuse = (int)req.mResult.GetMemSaved();

	PyObject *r;
	{
		AUTOTASKLET1("DB::NSession::ToPython");
		ToPythonCtxt ctxt(mBlue, mBeNice, mBeNiceEvery, mGetMem);
		r = req.mResult.ToPython(ctxt);
		mLastPyBytes = ctxt.mTotalPyBytes;
	}
	DBTRACE("Got results from req %p\n", &req);
	ForceException(r, "Result:ToPython");
	return r;
}

	
void NSession::Request::ThreadFunc()
{
	Execute();
}


void NSession::Request::Execute()
{
	__int64 startTime, endTime;
	ULARGE_INTEGER dummy, kernelTime1, userTime1, kernelTime2, userTime2;
	DBTRACE("thread starts on request %p\n", this);
	QueryPerformanceCounter((LARGE_INTEGER*)&startTime);
	GetThreadTimes(GetCurrentThread(), (LPFILETIME)&dummy, (LPFILETIME)&dummy,
				   (LPFILETIME)&kernelTime1, (LPFILETIME)&userTime1);
	DBROWCOUNT rc;
	HRESULT hr = mCommand.Open(0, &rc);
	if (FAILED(hr))
		mException = DelayedException_ptr(DelayedException::New(hr, "Open() failed in ThreadExecution"));
	else {
		DBLENGTH received;
		mException = DelayedException_ptr(mResult.Get(received, mNSession, mCommand, rc));
		mBytesReceived = (size_t) received;
	}
	//close the command before returning the session to the pool.
	mCommand.Close();
	mCommand.ReleaseCommand();
	if (mException.get() && mException->IsSessionFatal() ) {
		CCP_LOGERR_CH( s_chNSession, "Session 0x%x is invalid. Discarding it", mSession);
		DiscardSession();
	} else
		ReleaseSession();
	QueryPerformanceCounter((LARGE_INTEGER*)&endTime);
	GetThreadTimes(GetCurrentThread(), (LPFILETIME)&dummy, (LPFILETIME)&dummy,
				   (LPFILETIME)&kernelTime2, (LPFILETIME)&userTime2);
	
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	mWallclockTime = (double)(endTime-startTime) / (double)f.QuadPart;
	mKernelTime = (double)(kernelTime2.QuadPart-kernelTime1.QuadPart) * 1e-7;
	mUserTime = (double)(userTime2.QuadPart-userTime1.QuadPart) * 1e-7;
	DBTRACE("thread done with request %p\n", this);
}

//settings and status for the sessio npool
PyObject *NSession::GetSessionStatus()
{
	return mSessionPool->GetStatus();
}

PyObject *NSession::GetSessionSettings()
{
	return mSessionPool->GetSettings();
}

PyObject *NSession::SetSessionSettings(PyObject *settings)
{
	if (mSessionPool->ApplySettings(settings))
		Py_RETURN_NONE;
	return 0;
}







