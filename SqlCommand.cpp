// Copyright © 2014 CCP ehf.
#include "SqlCommand.h"

#include "PythonBuff.h"

#include "Utils.h"
using Utilities::DbExc_RuntimeError;

#include "NSession.h"

//--------------------------------------------------------------------
// SQLCommand::Prepare:  Make the command ready
//--------------------------------------------------------------------
bool SQLCommand::Prepare(size_t &paramLen, CSession *session, PyObject *schema, PyObject* args)
{
	AUTOTASKLETC("DB::SQLCommand::Prepare", mNSession->mTimerDetail >= 2);
	HRESULT hr;
	char* procname;
	PyObject* parameters;
	PyObject* procParamSchema;

	SetBlobHandling(DBBLOBHANDLING_NOSTREAMS);
	if (!PyArg_ParseTuple(args, "sO:Execute", &procname, &parameters))
		return false;

	if (!PyList_Check(parameters) && !PyDict_Check(parameters))	{
		PyErr_SetString(PyExc_TypeError, "Second argument must be a list or dictionary");
		return false;
	}

	PyObject* procSchema = PyTuple_GET_ITEM(schema, 0);
	procParamSchema = PyDict_GetItemString(procSchema, procname);
	if (procParamSchema == NULL || strcmp(procname, "__schemas") == 0)
		return PyErr_Format(DbExc_RuntimeError, "Stored procedure '%s' not found", procname), false;
	
	CString csSQL;
	if (!SelectProcedure(csSQL, procname, procParamSchema))
		return false;
	hr = Create(*session, csSQL, DBGUID_SQL);
	if (FAILED(hr))
		return Utilities::SetErr32(hr, "Couldn't create command for %s", procname), false;

	//Bind parameters, using the custom bind thing.  Note, this is like this in
	//the atl samples.
	void* pDummy;
	hr = BindParameters( &(m_hParameterAccessor), m_spCommand, procParamSchema, &pDummy, true, true, parameters);
	if (FAILED(hr))
		return Utilities::SetErr32(hr, "Couldn't bind parameters for %s", csSQL), false;
	
	// Now let's put in the parameters
	if (!SetDefaultParams())
		return false;
	if (PyList_Check(parameters))
	{
		if (!SetParamsFromList(paramLen, parameters))
			return false;
	} else {
		if (!SetParamsFromDict(paramLen, parameters))
			return false;
	}

	return true;
}


bool SQLCommand::SelectProcedure(CString &csSQL, const char *procname, PyObject *procParamSchema)
{
	// It's a stored proc call
	Py_ssize_t numargs = PyList_GET_SIZE(procParamSchema);

	if (numargs == 1) {
		csSQL.Format("{ ? = CALL %s;1 }", procname);
	} else {
		CString csParams = "?";
		for (Py_ssize_t i = 2; i<numargs; i++)
			csParams += ",?";
		csSQL.Format("{ ? = CALL %s;1(%s) }", procname, csParams);
	}
	return true;
}


//This dude returns parameter info from the already cached schema.
//Similar to the ICommandWithParameters::GetParameterInfo
//but without a server roundtrip. 
HRESULT SQLCommand::GetParameterInfo (
   DB_UPARAMS      *pcParams,
   DBPARAMINFO    **prgParamInfo,
   OLECHAR        **ppNamesBuffer, 
   PyObject		   *procParamSchema)
{

	// Create some binding sjite
	Py_ssize_t size = PyList_GET_SIZE(procParamSchema);
	DB_UPARAMS cols = Py_SAFE_DOWNCAST(size, Py_ssize_t, DB_UPARAMS);
	DBPARAMINFO* pinfo = (DBPARAMINFO*)CoTaskMemAlloc(cols * sizeof(DBPARAMINFO));
	if (!pinfo)
		return E_OUTOFMEMORY;
	size_t namesize = 0;

	DB_UPARAMS i;
	for (i = 0; i < cols; i++)
	{
		PyObject* colinfo = PyList_GET_ITEM(procParamSchema, i);
		CMiniProcParamsInfo* info =
			(CMiniProcParamsInfo*)PyCapsule_GetPointer(
				PyList_GET_ITEM(colinfo,6),
				"CMiniProcParamsInfo");

		// Calculate total size of name buffer
		namesize += strlen(info->m_szParameterName)+1;

		// Set the parameter characteristic flags
		switch(info->m_nType){
		case DBPARAMTYPE_INPUT:
			pinfo[i].dwFlags = DBPARAMFLAGS_ISINPUT; break;
		case DBPARAMTYPE_INPUTOUTPUT:
			pinfo[i].dwFlags = DBPARAMFLAGS_ISINPUT | DBPARAMFLAGS_ISOUTPUT; break;
		case DBPARAMTYPE_OUTPUT:
		case DBPARAMTYPE_RETURNVALUE:
			pinfo[i].dwFlags = DBPARAMFLAGS_ISOUTPUT; break;
		default:
			CCP_ASSERT(0);
		}
		if (info->m_bIsNullable)
			pinfo[i].dwFlags |= DBPARAMFLAGS_ISNULLABLE;
		if (info->m_nMaxLength > 8192)
			pinfo[i].dwFlags |= DBPARAMFLAGS_ISLONG;

		// Set the ordinal, starting with 1
		pinfo[i].iOrdinal = i+1;

		//temporarily store the pointer here.  translate and modify later
		pinfo[i].pwszName = (LPOLESTR)info->m_szParameterName;

		// There's no type info
		pinfo[i].pTypeInfo = NULL;

		pinfo[i].ulParamSize =
			info->m_nMaxLength ? info->m_nMaxLength : Utilities::GetSizeofDBType(info->m_nDataType);
		pinfo[i].wType = info->m_nDataType;
		pinfo[i].bPrecision = (BYTE)info->m_nPrecision;
		pinfo[i].bScale = (BYTE)info->m_nScale;
	}

	// Build the names
	LPOLESTR pnames = (LPOLESTR)CoTaskMemAlloc(namesize * sizeof(wchar_t));
	if (!pnames) {
		CoTaskMemFree(pinfo);
		return E_OUTOFMEMORY;
	}
	
	LPOLESTR ix = pnames;
	for (i = 0; i < cols; i++)
	{
		const char *pName = (const char*)pinfo[i].pwszName;
		CA2W pNameOLE(pName);
		wcscpy_s(ix, namesize, pNameOLE);
		pinfo[i].pwszName = ix;
		size_t l = wcslen(ix)+1;
		ix += l;
		namesize -= l;
	}
	
	*pcParams = cols;
	*prgParamInfo = pinfo;
	*ppNamesBuffer = pnames;
	return S_OK;
}


// This is cut and pasted from atldbcli.h, line 4974.  Exactly the same as there, but
// uses the function above to get the paraminfo.
HRESULT SQLCommand::BindParameters(HACCESSOR* pHAccessor, ICommand* pCommand, PyObject *procParamScema,
			void** ppParameterBuffer, bool fBindLength, bool fBindStatus, PyObject *params) throw()
{
	// If we have already bound the parameters then just return
	// the pointer to the parameter buffer
	if (*pHAccessor != NULL)
	{
		*ppParameterBuffer = m_pParameterBuffer;
		return S_OK;
	}

	CComPtr<IAccessor> spAccessor;
	ATLASSERT(pCommand != NULL);
	HRESULT hr = pCommand->QueryInterface(&spAccessor);
	if (FAILED(hr))
		return hr;

	// This is our new hack.  CCPmodification.
	DB_UPARAMS ulParams     = 0;
	CComHeapPtr<DBPARAMINFO>    spParamInfo;
	LPOLESTR pNamesBuffer;
	// Get Parameter Information
	hr = GetParameterInfo(&ulParams, &spParamInfo, &pNamesBuffer, procParamScema);
	if (FAILED(hr))
		return hr;
	//CCPMOdification ends here.
	
	// Create the parameter information for binding
	hr = AllocateParameterInfo(ulParams);
	if (FAILED(hr))
	{
		CoTaskMemFree(pNamesBuffer);
		return hr;
	}

	DBBYTEOFFSET nOffset = 0;
	DBBYTEOFFSET nDataOffset = 0;
	DBBYTEOFFSET nLengthOffset = 0;
	DBBYTEOFFSET nStatusOffset = 0;

	DBBINDING* pCurrent = m_pParameterEntry;
	for (ULONG l=0; l<ulParams; l++)
	{
		m_pParameterEntry[l].eParamIO = 0;

		if (spParamInfo[l].dwFlags & DBPARAMFLAGS_ISINPUT)
			m_pParameterEntry[l].eParamIO |= DBPARAMIO_INPUT;

		if (spParamInfo[l].dwFlags & DBPARAMFLAGS_ISOUTPUT)
			m_pParameterEntry[l].eParamIO |= DBPARAMIO_OUTPUT;

		//CCP modification: Handle long data in input
		DBOBJECT* dbo = NULL;
		if (spParamInfo[l].dwFlags & DBPARAMFLAGS_ISLONG) {
			SSIZE_T size = GetStringSize(&spParamInfo[l], params);
			if (size < 0) {
				spParamInfo[l].ulParamSize = 0; //no data
			} else if (size>=0 && (size_t)size <= m_nBlobSize) {
				//we just pack it in there.
				spParamInfo[l].ulParamSize = Py_SAFE_DOWNCAST(size, SSIZE_T, DBLENGTH);
			} else {
				//we must pass this large parameter as a blob (CCP addition, blobs weren't supported)
				spParamInfo[l].wType = DBTYPE_IUNKNOWN;
				spParamInfo[l].ulParamSize = sizeof(void*);
				dbo = new DBOBJECT;
				if (!dbo) {
					if (l == 0)
						//otherwise, the pointer has been placed in there correctly
						CoTaskMemFree(pNamesBuffer);
					return E_OUTOFMEMORY;
				}
				m_pParameterEntry[l].pObject = dbo;
				dbo->dwFlags = STGM_READ;
				dbo->iid = IID_ISequentialStream;
			}
		}
	
		// if this is a BLOB, truncate column length to m_nBlobSize (like 8000 bytes)
		if( spParamInfo[l].ulParamSize > m_nBlobSize )
			spParamInfo[l].ulParamSize = m_nBlobSize;

		// if this is a string, recalculate column size in bytes
		DBLENGTH colLength = spParamInfo[l].ulParamSize;
		if (spParamInfo[l].wType == DBTYPE_STR)
			colLength += 1;
		if (spParamInfo[l].wType == DBTYPE_WSTR)
			colLength = colLength*2 + 2;

		// Calculate the column data offset
		nDataOffset = AlignAndIncrementOffset( nOffset, colLength, GetAlignment( spParamInfo[l].wType ) );

		if( fBindLength )
		{
			// Calculate the column length offset
			nLengthOffset = AlignAndIncrementOffset( nOffset, sizeof(DBLENGTH), __alignof(DBLENGTH) );
		}

		if( fBindStatus )
		{
			// Calculate the column status offset
			nStatusOffset = AlignAndIncrementOffset( nOffset, sizeof(DBSTATUS), __alignof(DBSTATUS) );
		}

		//CCP modification too
		CDynamicParameterAccessor::Bind(pCurrent, spParamInfo[l].iOrdinal, spParamInfo[l].wType,
			colLength, spParamInfo[l].bPrecision, spParamInfo[l].bScale,
			m_pParameterEntry[l].eParamIO, nDataOffset, nLengthOffset, nStatusOffset,
			dbo);

		pCurrent++;

		m_ppParamName[l] = pNamesBuffer;
		if (pNamesBuffer && *pNamesBuffer)
		{
			// Search for the NULL termination character
			while (*pNamesBuffer++)
				;
		}
	}

	// Allocate memory for the new buffer
	m_pParameterBuffer = NULL;
	ATLTRY(m_pParameterBuffer = new BYTE[nOffset]);
	if (m_pParameterBuffer == NULL)
	{
		// Note that pNamesBuffer will be freed in the destructor
		// by freeing *m_ppParamName
		return E_OUTOFMEMORY;
	}
	*ppParameterBuffer = m_pParameterBuffer;
	m_nParameterBufferSize = nOffset;
	m_nParams = ulParams;
	hr = BindEntries(m_pParameterEntry, ulParams, pHAccessor, nOffset, spAccessor);

	return hr;
}


//This function gets the size of a parameter blob
SSIZE_T SQLCommand::GetStringSize(DBPARAMINFO *info, PyObject *params)
{
	BluePy object;
	if (!params)
		return -1;
	if (PyList_Check(params))
		object = BluePy(PyList_GetItem(params, (info->iOrdinal-2)), true); //weird, ordinals start at 2
	else
		object = BluePy(PyDict_GetItemString(params, CW2A(info->pwszName)), true);
	if (!object) {
		PyErr_Clear();
		return -1;
	}
	if( PyUnicode_Check( object.o ) )
		return PyUnicode_GET_LENGTH( object.o ); //return number of wide chars
	if( object.o->ob_type->tp_as_buffer )
	{
		if( !PyObject_CheckBuffer( object.o ) )
		{
			PyErr_SetString( PyExc_TypeError, "Object must have a buffer interface." );
			return -1;
		}

		Py_buffer view;

		if( PyObject_GetBuffer( object.o, &view, 0 ) != 0 )
		{
			PyErr_SetString( PyExc_TypeError, "Object must have a buffer interface." );
			return -1;
		}

		if( !PyBuffer_IsContiguous( &view, 'A' ) )
		{
			PyBuffer_Release( &view );
			PyErr_SetString( PyExc_TypeError, "Object buffer must be contiguous." );
			return -1;
		}

		SSIZE_T bufferLength = view.len;
		PyBuffer_Release( &view );
		return bufferLength;
	}
	return -1;
}


// Set return value to OK
bool SQLCommand::SetDefaultParams()
{
	if (!SetParamStatus(1, DBSTATUS_S_OK))
		return PyErr_SetString(DbExc_RuntimeError,	"Couldn't set param status for return column"), false;

	// Set all other parameters to default
	DB_UPARAMS n = GetParamCount();
	for (DBORDINAL i = 1; i < n; i++)
		if (!SetParamStatus(i+1, DBSTATUS_S_DEFAULT)) {
			PyErr_Format(DbExc_RuntimeError, "Couldn't set param status for column %d:%s to DBSTATUS_S_DEFAULT",
				i, (const char*)CW2A(GetParamName(i+1)));
			return false;
		}
	return true;
}


bool SQLCommand::SetParamsFromList(size_t &paramLen, PyObject *plist)
{
	if (PyList_GET_SIZE(plist) > (Py_ssize_t)(GetParamCount()-1)) {
		PyErr_Format(DbExc_RuntimeError,
			"The stored procedure accepts at most %d argument(s), not %d",
			GetParamCount()-1, PyList_GET_SIZE(plist));
		return false;
	}
	paramLen = 0;
	for (int i = 0; i < PyList_GET_SIZE(plist); i++)
	{
		size_t len;
		if (!SetPyParam(len, i+2, PyList_GET_ITEM(plist, i)))
			return false;
		paramLen += len;
	}
	return true;
}


bool SQLCommand::SetParamsFromDict(size_t &paramLen, PyObject *pdict)
{
	// Key/value arguments
	Py_ssize_t j = 0;
	PyObject* key;
	PyObject* value;

	paramLen = 0;
	while (PyDict_Next(pdict, &j, &key, &value))
	{
		BluePyStr pyname(BluePy(PyObject_Str(key)));
		// Look for the field
		DBORDINAL col;
		if (!_GetParameterNo(CA2T(pyname.Str()), col))
		{
			return PyErr_Format( DbExc_RuntimeError,"%s has no argument '%s'.",	"", pyname.Str()), false;
		}

		size_t len;
		if (!SetPyParam(len, col+1, value))
			return false;
		paramLen += len;
	}
	return true;
}

//Set an integer parameter
template<class T>
bool SQLCommand::SetPyParamInt(size_t &len, DBORDINAL col, PyObject *value)
{
	if( !PyLong_Check( value ) )
	{
		return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", col - 1, (const char*)CW2A( GetParamName( col ) ) ), false;
	}

	long long l = PyLong_AsLongLong( value );
	if (l == -1 && PyErr_Occurred())
		return false;
	T v = (T)l;
	if (v != l) {
		PyErr_Format(DbExc_RuntimeError, "Argument %d(%s) out of range:%lld", col, (const char*)CW2A(GetParamName(col)), l);
		return false;
	}
	SetParam(col, &v);
	len = sizeof(T);
	return true;
}

bool SQLCommand::SetPyParam(size_t &paramLen, DBORDINAL nparam, PyObject *value)
{
	//first, if we have a pynone, set the paramstatus
	if (value == Py_None) {
		SetParamStatus(nparam, DBSTATUS_S_ISNULL);
		paramLen = 0;
		return true;
	}
	DBTYPE type;
	GetParamType(nparam, &type);
	switch (type) {
	case DBTYPE_BOOL: {
		int res = PyObject_IsTrue( value );

		if(res == -1)
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must evaluate to a boolean", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		
		VARIANT_BOOL p = res ? VARIANT_TRUE : VARIANT_FALSE;
		SetParam(nparam, &p);
		paramLen = 1;
		return true; }
    case DBTYPE_I1:
		return SetPyParamInt<signed char>(paramLen, nparam, value);
	case DBTYPE_UI1:
		return SetPyParamInt<unsigned char>(paramLen, nparam, value);
	case DBTYPE_I2:
		return SetPyParamInt<signed short>(paramLen, nparam, value);
	case DBTYPE_UI2:
		return SetPyParamInt<unsigned short>(paramLen, nparam, value);
	case DBTYPE_I4:
		return SetPyParamInt<signed long>(paramLen, nparam, value);
	case DBTYPE_UI4: {
		//NOTE: This type is slightly broken. The value here should be an unsigned long
		//Issue has always been present and is also reflected in Blue PyRowSet

		if( !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		long l = PyLong_AsLong( value );

		if( l == -1 && PyErr_Occurred() )
			return PyErr_Format( DbExc_RuntimeError, "Failed to convert argument %d(%s) to long.", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;

		SetParam( nparam, &l );

		paramLen = sizeof( long );

		return true;
	}
		
	case DBTYPE_I8:
	case DBTYPE_FILETIME:{
		if( !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		signed __int64 ll = PyLong_AsLongLong(value);
		if( ll == -1 && PyErr_Occurred() )
			return PyErr_Format( DbExc_RuntimeError, "Failed to convert argument %d(%s) to long long.", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		SetParam(nparam, &ll);
		paramLen = sizeof(ll);
		return true;}
		
	case DBTYPE_UI8: {
		if( !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		unsigned __int64 ll = PyLong_AsUnsignedLongLong(value);
		if (ll == (unsigned __int64)-1 && PyErr_Occurred())
			return PyErr_Format( DbExc_RuntimeError, "Failed to convert argument %d(%s) to unsigned long long.", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		SetParam(nparam, &ll);
		paramLen = sizeof(ll);
		return true;}
		
	case DBTYPE_R4: {
		if( !PyFloat_Check( value ) && !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a number", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		float rr = (float)PyFloat_AsDouble(value);
		if (rr == -1.0f && PyErr_Occurred())
			return false;
		SetParam(nparam, &rr);
		paramLen = sizeof(rr);
		return true;}
		
	case DBTYPE_R8: {
		if( !PyFloat_Check( value ) && !PyLong_Check( value ))
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a number", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		double rr = PyFloat_AsDouble(value);
		if (rr == -1.0 && PyErr_Occurred())
			return false;
		SetParam(nparam, &rr);
		paramLen = sizeof(rr);
		return true;}

	case DBTYPE_CY: {
		if( !PyFloat_Check( value ) && !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a number", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		double dd = PyFloat_AsDouble(value);
		if (dd == -1.0 && PyErr_Occurred())
			return false;
		__int64 rr = (__int64)(floor(dd * 100.0 + 0.5) * 100.0);
		SetParam(nparam, &rr);
		paramLen = sizeof(rr);
		return true;}
		
	case DBTYPE_STR: {
		if( !PyUnicode_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be an ASCII string", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		//Convert using ASCII
		PyObject* valueAsAscii = PyUnicode_AsASCIIString( value );
		if( !valueAsAscii )
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be an ASCII string", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;

		const char* str = PyBytes_AsString( valueAsAscii );

		if (!SetParamString(nparam, str)) {
			Py_DecRef( valueAsAscii );
			DBLENGTH max;
			GetParamSize(nparam, &max);
			return PyErr_Format(DbExc_RuntimeError, "Argument %d(%s) too long, can be at most %d chars", nparam-1, (const char*)CW2A(GetParamName(nparam)), max), false;
		}
		/* only the actual string data is sent, not the max column size.  Verified using network packet sniffing */
		paramLen = strlen(str); //this string is preallocated
		Py_DecRef( valueAsAscii );
		break;}
	case DBTYPE_BSTR:
	case DBTYPE_WSTR: {
		if( !PyUnicode_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be UnicodeType", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		wchar_t* str = PyUnicode_AsWideCharString( value, nullptr );
		if( !str )
		{
			return PyErr_Format( DbExc_RuntimeError, "Failed to get wchar from unicode argument %d(%s)", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		if (!SetParamString(nparam, str)) {
			DBLENGTH max;
			GetParamSize(nparam, &max);
			PyMem_Free( str );
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) too long, can be at most %d chars", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ), max / 2 ), false;
		}
		paramLen = wcslen(str) * sizeof(wchar_t);
		PyMem_Free( str );
		break;}

	case DBTYPE_BYTES: {
		if( !PyObject_CheckBuffer( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must have buffer interface", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		Py_buffer view;

		if( PyObject_GetBuffer( value, &view, 0 ) != 0 )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must have buffer interface", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}

		if( !PyBuffer_IsContiguous( &view, 'A' ) )
		{
			PyBuffer_Release( &view );
			return PyErr_Format( PyExc_TypeError, "Buffer argument %d(%s) must be contiguous", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}


		DBLENGTH dblen;
		GetParamSize( nparam, &dblen );
		if( view.len > (Py_ssize_t)dblen )
		{
			PyBuffer_Release( &view );
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) BLOB too large.  is %d, can be at most %d", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ), view.len, dblen ), false;
		}

		char* dest = (char*)GetParam( nparam );
		memcpy( dest, view.buf, view.len );
		SetParamLength( nparam, Py_SAFE_DOWNCAST( view.len, Py_ssize_t, DBLENGTH ) );
		SetParamStatus( nparam, view.len ? DBSTATUS_S_OK : DBSTATUS_S_ISNULL );
		paramLen = view.len;
		PyBuffer_Release( &view );
		break;
	}
	case DBTYPE_IUNKNOWN: {
		PythonBuff *pbuff = new PythonBuff(value);
		if (!pbuff)
			return PyErr_NoMemory(), false;
		if (pbuff->GetLength() == -1) {
			delete pbuff;
			return false;
		}
		if (!pbuff->Valid()) {
			delete pbuff;
			return PyErr_Format(DbExc_RuntimeError, "Argument %d(%s) must have buffer interface", nparam-1, (const char*)CW2A(GetParamName(nparam))), false;
		}
		IUnknown *iu = pbuff;
		SetParam(nparam, &iu);
		size_t bufflen = pbuff->GetLength();
		SetParamLength(nparam, Py_SAFE_DOWNCAST(bufflen, size_t, DBLENGTH));
		paramLen = bufflen;
		break;}

	case DBTYPE_DBTIMESTAMP:
	case DBTYPE_DBDATE: {
		if( !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		__int64 filetime = PyLong_AsLongLong(value);
		if (filetime == -1 && PyErr_Occurred())
			return false;
		DBLENGTH dstlen;
		HRESULT hr = mNSession->mConv->DataConvert(
			DBTYPE_FILETIME, type,
			sizeof(filetime), &dstlen,
			&filetime, GetParam(nparam),
			sizeof (DBTIMESTAMP), 
			DBSTATUS_S_OK, GetParamStatus(nparam),
			0, 0,
			DBDATACONVERT_DEFAULT);
		if (FAILED(hr))
		{
			return Utilities::SetErr32( hr, "DataConvert failed" ), false;
		}
			
		paramLen = dstlen;
		return true; }

	case DBTYPE_DBTIME2: {
		if( !PyLong_Check( value ) )
		{
			return PyErr_Format( DbExc_RuntimeError, "Argument %d(%s) must be a long", nparam - 1, (const char*)CW2A( GetParamName( nparam ) ) ), false;
		}
		__int64 filetime = PyLong_AsLongLong(value);
		if (filetime == -1 && PyErr_Occurred())
			return false;
		DBTIME2 time;
		time.fraction = (ULONG)(filetime % 10000000) * 100;
		filetime /= 10000000;
		time.second = (USHORT)(filetime % 60);
		filetime /= 60;
		time.minute = (USHORT)(filetime % 60);
		filetime /= 60;
		time.hour = (USHORT)(filetime % 24);
		SetParam(nparam, &time);
		paramLen = sizeof(time);
		break; }

	default:
		return PyErr_Format(DbExc_RuntimeError, "Argument %d(%s) unexpected DB type: %d", nparam-1, (const char*)CW2A(GetParamName(nparam)), type), false;
	}
	return true;
}

//--------------------------------------------------------------------
// SetErr32
//--------------------------------------------------------------------

PyObject* SQLCommand::Raise(const char *msg, HRESULT hr, const CDBErrorInfo *errorInfo, ULONG nErrors )
{
	BluePy none(Py_None, true);
	
	//get column errors
	BluePy paramErrors, columnErrors;
	if (!Utilities::ParamErrorsToPy(hr, *this, &paramErrors, &columnErrors))
		return 0;
	
	//Get Error Records
	BluePy errorRecords(Utilities::ErrorRecToPy(errorInfo, nErrors));
	if (!errorRecords)
		return 0;
	
	//Get the command
	GUID sqlguid = DBGUID_SQL;
	CComQIPtr<ICommandText> cmdtext(m_spCommand);
	CComHeapPtr<OLECHAR> sql;
	if (cmdtext)
		cmdtext->GetCommandText(&sqlguid, &sql);

	// Put everything together
	BluePy errorArgs = BluePy(Py_BuildValue("isOuOO",
		hr, msg, errorRecords,
		sql?sql:(wchar_t*)L"", paramErrors, columnErrors));
	if (!errorArgs)
		return 0;
	return PyErr_SetObject(Utilities::ErrorClass(hr), errorArgs), nullptr;
}
