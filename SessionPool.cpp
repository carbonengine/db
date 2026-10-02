// Copyright © 2009 CCP ehf.

/* 
	*************************************************************************

	SessionPool.h

	Project:   EVE Server Database Access

	Description:   

		A class that doles out DB Session objects from a pool.  Has the
		concept of maxSessions (limiting the number of sessions.)
		It will block a tasklet until a session is available if there
		is no room to generate a new one.
		It maintans a list of free sessions and has a min-max watermark
		of the level of free sessions.  It can be useful to keep a minimum
		of two idle sessions to make sure that one is always available.

	Dependencies:

		Python

	*************************************************************************
*/

#include "StdAfx.h"
#include "SessionPool.h"
#include <Scheduler.h>

#include "Utils.h"


static CcpLogChannel_t s_chPool = CCP_LOG_DEFINE_CHANNEL( "SessionPool" );


extern ITaskletTimer *ttimer;
#define AUTOTASKLET0(c) AutoTasklet _at(ttimer, (c))
#define AUTOTASKLETC(c, cond) AutoTasklet _at(ttimer, (c), (cond))
#define AUTOTASKLET1(c) AUTOTASKLETC(c, mTimerDetail>=1)
#define AUTOTASKLET2(c) AUTOTASKLETC(c, mTimerDetail>=2)
#define AUTOTASKLET3(c) AUTOTASKLETC(c, mTimerDetail>=3)


SessionPool::SessionPool(const ATL::CDataSource &d) :
	mDataSource(d)
{
	mSessionCount = 0;
	mSessionsInUse = 0;
	mMaxSessions = 32;
	mMinFreeSessions = 2;
	mMaxFreeSessions = 8;
	mTimerDetail = 0;
	mCleanEvery = 10.0; //Add or remove idle session every 10 seconds.
	mNextClean.QuadPart = 0;
	mAddingIdle = false;
}


bool SessionPool::Init()
{
	mChannel = BluePy( reinterpret_cast<PyObject*>( SchedulerAPI()->PyChannel_New( nullptr ) ) );
	if( !mChannel )
		return false;
	SchedulerAPI()->PyChannel_SetPreference( reinterpret_cast<PyChannelObject*>( mChannel.o ), 1 ); //sender preference = lazy wakeup
	return true;
}

void SessionPool::Fini()
{
	//cleanup on main thread
	_ASSERT( !mSessionsInUse );
	_ASSERT( SchedulerAPI()->PyChannel_GetBalance( reinterpret_cast<PyChannelObject*>( mChannel.o ) ) == 0 );
	FlushList();
	mChannel.Release();
}

ATL::CSession *SessionPool::PopList()
{
	std::lock_guard<std::mutex> lock(mMutex);
	ATL::CSession* session = nullptr;
	if (!mDeque.empty()) {
		session = mDeque.front();
		mDeque.pop_front();
	}
	return session;
}

void SessionPool::PushList(ATL::CSession *s)
{
	_ASSERT(s);
	std::lock_guard<std::mutex> lock(mMutex);
	if (s) {
		mDeque.push_back(s);
	}
}

void SessionPool::FlushList(){
	std::lock_guard<std::mutex> lock(mMutex);
	for (auto it : mDeque) {
		DiscardSession(it);
	}
	mDeque.clear();
}

//Create a new session for a DataSource.  This will block
//a thread.  mSessionCount has already been incremented
HRESULT SessionPool::NewSession(ATL::CSession* &s)
{
	s = new ATL::CSession;
	if (!s)
		return ERROR_OUTOFMEMORY;
	HRESULT hr = s->Open(mDataSource);
	if (!SUCCEEDED(hr)) {
		delete s;
		s = nullptr;
	}
	return hr;
}


void SessionPool::DeleteSession(ATL::CSession *s)
{
	_ASSERT(s);
	s->Close();
	delete s;
	InterlockedDecrement(&mSessionCount);
}

bool SessionPool::GetSession(ATL::CSession* &s)
{
	LARGE_INTEGER t1, t2;
	QueryPerformanceCounter( &t1 );
	bool result = GetSession_int( s );
	QueryPerformanceCounter( &t2 );
	t2.QuadPart -= t1.QuadPart;
	QueryPerformanceFrequency( &t1 );
	double duration = double( t2.QuadPart ) / double( t1.QuadPart );
	if( duration > 1.0 )
	{
		CCP_LOGWARN_CH( s_chPool, "NSession took %f to return a session", duration );
		int balance = SchedulerAPI()->PyChannel_GetBalance( reinterpret_cast<PyChannelObject*>( mChannel.o ) );
		CCP_LOGWARN_CH( s_chPool, "Status: nSessions=%d, inUse=%d, nQueue=%d", mSessionCount, mSessionsInUse, -balance );
	}
	return result;
}

bool SessionPool::GetSession_int(ATL::CSession* &s)
{	
	AUTOTASKLET2("DB::NSession::SessionPool::GetSession");
	//depending on sessions in use, we must block on a tasklet
	//We use a non-strict queue so that we don't cause lock-convoying
	//(a woken up tasklet must wait until its turn in the runnable queue, meanwhile
	// the resource would be unavailable)
	while( mMaxSessions > 0 && mSessionsInUse >= mMaxSessions )
	{
		//must wait here
		BluePy r( SchedulerAPI()->PyChannel_Receive( reinterpret_cast<PyChannelObject*>( mChannel.o ) ) );
		if( !r )
			return false;
	}
	++mSessionsInUse;

	//first do a pop of the list.
	s = PopList();
	if (s) {
		FillIdle(mSessionCount);
		return true;
	}

	//We need to create a new one.  But see if idle list needs filling.
	FillIdle(mSessionCount + 1);
	
	//create a new one
	HRESULT hr = TaskletBlockingNewSession(s);
	if (FAILED(hr)) {
		--mSessionsInUse;
		Utilities::SetErr32(hr, "SessionPool::GetSession");
		return false;
	}
	return true;
}

void SessionPool::ReturnSession(ATL::CSession *s)
{
	_ASSERT(s);
	PushList(s);
}

void SessionPool::DiscardSession(ATL::CSession *s)
{
	_ASSERT(s);
	DeleteSession(s);
}

bool SessionPool::EndSession()
{
	_ASSERT(mSessionsInUse>0);
	--mSessionsInUse;
	return Pump();
}

//After work is done, see if any waiting tasklets can be released.
//Also, throw away idle sessions.
bool SessionPool::Pump()
{
	if( !mChannel )
		return true; //A late destructor call, after Fini has been called.
	int balance = SchedulerAPI()->PyChannel_GetBalance( reinterpret_cast<PyChannelObject*>( mChannel.o ) );
	if( balance )
	{
		_ASSERT( balance < 0 );
		int wakeup;
		if( mMaxSessions > 0 )
			wakeup = min( -balance, mMaxSessions - mSessionsInUse );
		else
			wakeup = -balance;
		for( int i = 0; i < wakeup; i++ )
		{
			//send does not block, since preference is 1 (sender)
			if( SchedulerAPI()->PyChannel_Send( reinterpret_cast<PyChannelObject*>( mChannel.o ), Py_None ) )
				return false;
		}
	}
	else
	{
		PruneIdle(); //prune only when no one was waiting.
	}
	return true;
}

//This function blocks on a tasklet.  This is useful.
HRESULT SessionPool::TaskletBlockingNewSession(ATL::CSession* &s)
{
	//Increment the session count immediately: Action has been
	//taken to increment the number of sessions.
	//Note we could change this to do synchronous wait for block-trapped tasklets,
	//but lets not worry.
	InterlockedIncrement(&mSessionCount);
	try {
		auto req = std::make_shared<Request>( this->shared_from_this() );
		req->ExecuteAndWait();
		return req->GetResult(s);
	} catch(std::exception) {
		InterlockedDecrement(&mSessionCount);
		return E_FAIL;
	}
}

//Create a new idle session in the background.  the mListSize is incremented immediately
void SessionPool::NewIdleSessions(int n)
{
	//Increment the session count immediately: Action has been
	//taken to increment the number of sessions.
	if (mAddingIdle)
		return; //only one allowed at a time.
	IdleRequest *req = new IdleRequest(shared_from_this(), n);
	if (!req)
		return;
	LONG value = n;
	InterlockedExchangeAdd(&mSessionCount, value);
	mAddingIdle = true;
	BOOL ok = QueueUserWorkItem(IdleRequest::ThreadProc, static_cast<void *>(req), WT_EXECUTELONGFUNCTION);
	if (!ok) {
		delete req;
		value = -n;
		InterlockedExchangeAdd(&mSessionCount, value);
		mAddingIdle = false;
		BeOS->SetError(BE32, NULL, "QueueUserWorkItem failed in NewIdleSession");
	}
}

//Create a new session and make it idle.  mNumSessions and mListSize
//has been incremented already.  This is typically called from a thread
void SessionPool::NewIdleSessions_thread(int n)
{
	for(int i = 0; i<n; ++i) {
		ATL::CSession *s=0;
		HRESULT hr = NewSession(s);
		if (SUCCEEDED(hr)) {
			PushList(s);
		} else {
			//silently fail, we have no thread safe error reporting gizmos
			InterlockedDecrement(&mSessionCount);
		}
	}
	mAddingIdle = false;
}

//Fill the idle list.  Will add a single idle session if required.
void SessionPool::FillIdle(int nSessions, bool fillAll)
{
	if (mAddingIdle)
		return;
	int nIdle = max(nSessions-mSessionsInUse, 0);
	int nMissing = mMinFreeSessions - nIdle;
	if (nMissing > 0) {
		//don't go over max sessions limit
		if (mMaxSessions > 0 && nSessions + nMissing > mMaxSessions)
			nMissing = mMaxSessions-nSessions;
		
		//don't go over max free sessions limit
		if (mMaxFreeSessions >= 0 && nIdle + nMissing > mMaxFreeSessions)
			nMissing = mMaxFreeSessions-nIdle;

		if (nMissing <= 0 )
			return;

		if (fillAll)
			//Create them all
			NewIdleSessions(nMissing);
		else
			//A single new session
			NewIdleSessions(1);
		//Sanity check
		_ASSERT(mSessionCount <= mMaxSessions);
	}
}

//Remove extra free sessions
void SessionPool::PruneIdle(bool all)
{
	const int nIdle = mSessionCount - mSessionsInUse;
	int a, b, c; //how many to delete

	if (nIdle<1)
		return;
	if (!CanClean())
		return;

	//idle sessions can be limited by the maxFreeSessions, or by the mMaxSessions
	if (mMaxFreeSessions >= 0)
		a = max(nIdle-mMaxFreeSessions, 0);
	else
		a = 0;

	//Also, by the total number of sessions:
	if (mMaxSessions > 0)
		b = min(max(mSessionCount-mMaxSessions, 0), nIdle);
	else
		b = 0;

	//take the larger of the two.
	c = max(a, b);
	if (c) {
		
		_ASSERT(c <= nIdle);
		if (!all)
			c = 1; //just kill one at a time.
		MarkClean();
		for(int i = 0; i<c ;++i) {
			ATL::CSession *morbid = PopList();
			if (morbid)
				DeleteSession(morbid);
		}
	}
}


bool SessionPool::CanClean(bool mark)
{
	ULARGE_INTEGER t;
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	memcpy(&t, &ft, sizeof(ft));
	if (t.QuadPart >= mNextClean.QuadPart) {
		if (mark)
			mNextClean.QuadPart = t.QuadPart + (ULONGLONG)(mCleanEvery*1e7f);
		return true;
	}
	return false;
}

void SessionPool::MarkClean()
{
	ULARGE_INTEGER t;
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	memcpy(&t, &ft, sizeof(ft));
	mNextClean.QuadPart = t.QuadPart + (ULONGLONG)(mCleanEvery*1e7f);
}

//Informative Python interface functions:
PyObject *SessionPool::GetStatus()
{
	return Py_BuildValue("{sisisi}",
		"sessionsInUse", mSessionsInUse,
		"sessionCount", mSessionCount,
		"freeSessions", mDeque.size());
}

PyObject *SessionPool::GetSettings()
{
	return Py_BuildValue("{sisisisf}",
		"maxSessions", mMaxSessions,
		"minFreeSessions", mMinFreeSessions,
		"maxFreeSessions", mMaxFreeSessions,
		"cleanEvery", mCleanEvery);
}

bool SessionPool::ApplySettings(PyObject *settings)
{
	PyObject *v;
	int i;
	double d;
	v = PyDict_GetItemString(settings, "maxSessions");
	if (v) {
		i = (int)PyLong_AsLong( v );
		if (i==-1 && PyErr_Occurred()) return false;
		mMaxSessions = max(0, i);
	}

	v = PyDict_GetItemString(settings, "minFreeSessions");
	if (v) {
		i = (int)PyLong_AsLong( v );
		if (i==-1 && PyErr_Occurred()) return false;
		mMinFreeSessions = max(0, i);
	}

	v = PyDict_GetItemString(settings, "maxFreeSessions");
	if (v) {
		i = (int)PyLong_AsLong( v );
		if (i==-1 && PyErr_Occurred()) return false;
		mMaxFreeSessions = max(-1, i);
	}

	v = PyDict_GetItemString(settings, "cleanEvery");
	if (v) {
		d = PyFloat_AsDouble(v);
		if (d==-1.0 && PyErr_Occurred()) return false;
		mCleanEvery = max(0, (float)d);
		mNextClean.QuadPart = 0;
	}

	//Add idle sessions if needed...
	FillIdle(mMaxSessions, true);
	//or remove extra ones if needed.
	return Pump();
}


//////////////////////////////
// The SessionPool::Request

SessionPool::Request::Request(SessionPoolPtr pool) :
	mPool(pool),
	mSession(0),
	mHr(S_OK)
{}

SessionPool::Request::~Request()
{
	if (mSession)
		mPool->DeleteSession(mSession);
}

void SessionPool::Request::ThreadFunc()
{
	mHr = mPool->NewSession(mSession);
}

HRESULT SessionPool::Request::GetResult(ATL::CSession* &le)
{
	le = mSession;
	mSession = nullptr;
	switch( mState )
	{
	case IOWorker::DONE:
		return S_OK;
	case IOWorker::FAILED:
		return E_FAIL;
	case IOWorker::PENDING:
		return E_PENDING;
	default:
		return E_UNEXPECTED;
	}
}

//////////////////////////////
// The SessionPool::IdleRequest
DWORD WINAPI SessionPool::IdleRequest::ThreadProc(LPVOID arg)
{
	SessionPool::IdleRequest *self = static_cast<SessionPool::IdleRequest*>(arg);
	self->mPool->NewIdleSessions_thread(self->mN);
	delete self;
	return 0;
}