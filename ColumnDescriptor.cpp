// Copyright © 2023 CCP ehf.
#include "ColumnDescriptor.h"

#include "Utils.h"

#include <atlstr.h>

DelayedException* ColumnDescriptor::TypeTranslate( DBTYPE& type, int& size )
{
	type &= 0xff;
	DBTYPE stype = type;
	switch( stype )
	{
	case DBTYPE_BOOL:
		size = 0;
		break;
	case DBTYPE_I1:
	case DBTYPE_UI1:
		size = 1;
		break;
	case DBTYPE_I2:
	case DBTYPE_UI2:
		size = 2;
		break;
	case DBTYPE_I4:
	case DBTYPE_UI4:
	case DBTYPE_R4:
		size = 3;
		break;
	case DBTYPE_I8:
	case DBTYPE_UI8:
	case DBTYPE_R8:
	case DBTYPE_CY:
	case DBTYPE_FILETIME:
		size = 4;
		break;
	case DBTYPE_DBTIMESTAMP:
	case DBTYPE_DBDATE:
	case DBTYPE_DBTIME2:
		type = DBTYPE_FILETIME; //will be converted when read
		size = 4;
		break;
	case DBTYPE_BSTR:
		type = DBTYPE_WSTR; //they are the same.
	case DBTYPE_STR:
	case DBTYPE_WSTR:
	case DBTYPE_BYTES:
		size = 5;
		break; //signals a pointer.
	default: {
		CString msg;
		msg.Format( "Unexpected type %d", type );
		PYDBERROR( msg );
	}
	}
	return 0;
}