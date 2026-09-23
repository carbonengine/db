// Copyright © 2023 CCP ehf.

/* 
	*************************************************************************

	SQLCommand.h

	Project:   EVE Server Database Access

	Description:   

		Functions relating to SQL commands


	Dependencies:

		Python

	*************************************************************************
*/
#pragma once
#ifndef _SQLCOMMAND_H_
#define _SQLCOMMAND_H_

#include "StdAfx.h"
#include "TmpRowset.h"
#include "Accessor.h"

class NSession;

class SQLCommand : public CCommand<ACCESSOR, CBulkRowset, CMultipleResults>
{
public:
	SQLCommand( NSession* s ) :
		mNSession( s )
	{
	}
	bool Prepare( size_t& paramLen, CSession* session, PyObject* schema, PyObject* args );
	PyObject* Raise( const char* msg, HRESULT hr = S_OK, const CDBErrorInfo* errorInfo = 0, ULONG nErrors = 0 );

private:
	bool SelectProcedure( CString& csSQL, const char* procname, PyObject* schema );

	//These two are reimplementations from the ATL, that bind parameters using cached
	//scheme data, rather than invoke a server roundtrip to inquire about the parameters.
	HRESULT GetParameterInfo( DB_UPARAMS* pcParams, DBPARAMINFO** prgParamInfo, OLECHAR** ppNamesBuffer, PyObject* procParamSchema );
	HRESULT BindParameters( HACCESSOR* pHAccessor, ICommand* pCommand, PyObject* procParamScema, void** ppParameterBuffer, bool fBindLength = false, bool fBindStatus = false, PyObject* params = 0 ) throw();
	SSIZE_T GetStringSize( DBPARAMINFO* info, PyObject* params );
	bool SetDefaultParams();
	bool SetParamsFromList( size_t& paramLen, PyObject* plist );
	bool SetParamsFromDict( size_t& paramLen, PyObject* pdict );
	template <class T>
	bool SetPyParamInt( size_t& len, DBORDINAL col, PyObject* value );

	NSession* const mNSession;

 protected:
	bool SetPyParam( size_t& len, DBORDINAL param, PyObject* val );

};

#endif