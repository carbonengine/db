// Copyright © 2005 CCP ehf.

/* 
	*************************************************************************

	Session.h

	Project:   EVE Server Database Access

	Description:   

		A new database session object, which generates new style rowsets


	Dependencies:

		Python

	*************************************************************************
*/

#ifndef _NSESSION_H_
#define _NSESSION_H_

#include "Connection.h"
#include "PyTemplates.h"

#include "SessionPool.h"
#include "TmpRowset.h"

#include <atlstr.h>
#include <msdadc.h>	// for IDataConvert

#include "SqlCommand.h"


extern ITaskletTimer *ttimer;
#define AUTOTASKLET0(c) AutoTasklet _at(ttimer, (c))
#define AUTOTASKLETC(c, cond) AutoTasklet _at(ttimer, (c), (cond))
#define AUTOTASKLET1(c) AUTOTASKLETC(c, mTimerDetail>=1)
#define AUTOTASKLET2(c) AUTOTASKLETC(c, mTimerDetail>=2)
#define AUTOTASKLET3(c) AUTOTASKLETC(c, mTimerDetail>=3)


BLUE_DECLARE( Connection );

// not really a session in the OLEDB sense, more like a "connection"
class NSession : 
	public PyXObject2<NSession>
{
	typedef PyXObject2<NSession> Parent;
	typedef CCommand<CDynamicParameterAccessor, CArrayRowset, CMultipleResults> command_t;

public:
	PYTHON_CLASS((char*)"db.NSession");

	NSession();
	~NSession();

	//initialization
	static PyObject *_New(PyTypeObject*type, PyObject *args, PyObject *kw);
	bool Init(PyObject *args);

	PYTHON_METHODS_BEGIN()
		METHOD_VARARGS(Execute, "Executes a stored proc and returns rowset(s) or integer value.")
		METHOD_VARARGS(GetSchema, "get the database schema")
		METHOD_NOARGS(GetSessionStatus, "Get status of sessions")
		METHOD_NOARGS(GetSessionSettings, "Get settings for session pool")
		METHOD_O(SetSessionSettings, "Set settings for session pool")
	PYTHON_METHODS_END()
	PYTHON_GETSET_BEGIN()
		PYTHON_GETINT64( (char*)"totalBytesSentParam", mTotalBytesSentParam, (char*)"Total bytes sent in out params.")
		PYTHON_GETINT64( (char*)"totalBytesReceived", mTotalBytesReceived, (char*)"Total bytes received")
		PYTHON_GETINT64( (char*)"lastPyBytes", mLastPyBytes, (char*)"python bytes allocated in last request")
	PYTHON_GETSET_END()
	PYTHON_MEMBERS_BEGIN()
        PYTHON_MEMBER( (char*)"blobSizeLimit", T_INT, mBlobSizeLimit, 0)
		PYTHON_MEMBER( (char*)"lastWallclockTime", T_DOUBLE, mLastWallclockTime, READONLY)
		PYTHON_MEMBER( (char*)"lastKernelTime", T_DOUBLE, mLastKernelTime, READONLY)
		PYTHON_MEMBER( (char*)"lastUserTime", T_DOUBLE, mLastUserTime, READONLY)
		PYTHON_MEMBER( (char*)"beNiceEvery", T_INT, mBeNiceEvery, 0)
		PYTHON_MEMBER( (char*)"allowSync", T_INT, mAllowSync, 0)
		PYTHON_MEMBER( (char*)"timerDetail", T_INT, mTimerDetail, 0)
		PYTHON_MEMBER( (char*)"lastStringReuse", T_INT, mLastStringReuse, READONLY)
	PYTHON_MEMBERS_END()

	static bool InitType(PyTypeObject *type) 
	{
		type->tp_weaklistoffset = offsetof(NSession, mWeakrefList);
		return true;
	}

	CComPtr<IDataConvert> mConv; //for data conversion
	BluePy mBlue; //shortcut to blue module
	BluePy mYield; //shortcut to blye.pyos.Synchro.Yield
	BluePy mBeNice; //shortcut to blue.pyos.BeNice
	BluePy mGetMem; //shortcut to sys.getpymalloced
	int mBeNiceEvery; //line interval to call benice.  0 = off
	int mAllowSync; //if true, synchronous calls on noblock tasklets are allowd
	int mTimerDetail; //detail of tasklet timers. 0 is default, up this to increase.

private:
	PyObject *mWeakrefList; //weak references here
	PyObject *Execute(PyObject *args); //The main man
	PyObject *GetSchema(PyObject *args); //returns a new ref
	PyObject *GetSessionStatus();
	PyObject *GetSessionSettings();
	PyObject *SetSessionSettings(PyObject *settings);
	
	PyObject *GetSchemaB(bool refresh); //returns our cached schema.
	
	struct Request : public IOWorker
	{
		Request(NSession *ns) : mNSession(ns), mCommand(ns), mSessionKeeper(ns->mSessionPool) {
			mSession = 0;
		}
		~Request() {
			mCommand.Close();
			mCommand.ReleaseCommand();

			if (mException.get()) {
				DiscardSession();
			} else
				ReleaseSession();
		}

		void ThreadFunc() override; //the worker function
		void Execute(); //when we do a direct execute

		PyObject *Raise();
		bool GetSession(int timerDetail); //call on main thread
		void ReleaseSession();
		void DiscardSession(); //use this if an error occurred (from worker), since session state may be borked.
		
		double mWallclockTime;
		double mKernelTime;
		double mUserTime;
		class NSession * const mNSession;  //the nSession object
		SessionKeeper mSessionKeeper;
		ATL::CSession *mSession; //the db session to run on
		TmpRowsetList mResult;
		SQLCommand mCommand;
		DelayedException_ptr mException;
		size_t mBytesReceived; //length of data
	};
	
	bool Prepare(Request &rq, PyObject *args);
	PyObject *ExecuteBlock(Request &rq);
	PyObject *ExecuteDirectly(Request &rq);
	
private:
	
	CDataSource mDataSource; //each session has its own datasource object.  suboptimal.
	SessionPoolPtr mSessionPool;
	ConnectionPtr mConnection; //must keep reference to connection, to keep it alive.
	BluePy mSchema;		 //the database schema., from the connection

	int mBlobSizeLimit;
	
	
	//Stats:
	double mLastWallclockTime;
	double mLastKernelTime;
	double mLastUserTime;
	__int64 mLastPyBytes;
	__int64 mTotalBytesSentParam;
	__int64 mTotalBytesReceived;
	int		mLastStringReuse;
};





#endif //defined _NSESSION_H_