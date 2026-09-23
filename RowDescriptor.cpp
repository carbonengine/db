// Copyright © 2023 CCP ehf.
#include "RowDescriptor.h"

#include "Utils.h"

#include <atlstr.h>

//Set up the object from the DynamicAccessor
// layout is: [binary columns, null flags, Object columns ]
DelayedException* RowDescriptor::Init( ACCESSOR& a )
{
	//first pass.  Build column list, count sizes;
	int sizes[6] = { 0 };
	mNObjects = 0;
	mColumnList.clear();
	DBORDINAL i;
	DBORDINAL nc = a.GetColumnCount();
	for( i = 0; i < nc; i++ )
	{
		CW2A name( a.GetColumnName( i + 1 ) );
		mColumnList.push_back( ColumnDescriptor( name ) );
		ColumnDescriptor& cd = mColumnList.back();
		DBTYPE type;
		if( !a.GetColumnType( i + 1, &type ) )
		{
			CString msg;
			msg.Format( "Couldn't get column type of column %d", i );
			PYDBERROR( msg );
		}
		int size;
		DelayedException* de = ColumnDescriptor::TypeTranslate( type, size );
		if( de )
			return de;
		sizes[size]++;
		cd.mType = type;
		cd.mSize = size;
	}

	//now, compute offsets, starting with the largest (real) size
	int offsets[5];
	int offset = 0; //totaloffset to extra data
	for( i = 4; i > 0; i-- )
	{ //regular ints
		offsets[i] = offset;
		offset += sizes[i] * ( 1 << ( i - 1 ) );
	}
	//bools
	offset *= 8; //turn into bits;
	offsets[0] = offset; //start of boolean flags, in bits
	offset += sizes[0]; //space required for bits

	//null flags
	mSNull = offset; //start of null flags in bits
	offset += (int)mColumnList.size(); //space required for bits,

	//round up to bytes again
	offset = ( offset + 7 ) / 8;

	mDataLen = offset; //this is the length of the data and null flags (in bytes).

	//pointers
	mNObjects = sizes[5];
	if( mNObjects )
	{
		offset = ( offset + ( sizeof( void* ) - 1 ) ) & ~( sizeof( void* ) - 1 ); //round up in size.
		mSObjects = offset / sizeof( void* ); //get offset for the object counter.
		mTotalLen = offset + mNObjects * sizeof( void* );
	}
	else
	{
		mSObjects = 0;
		mTotalLen = mDataLen;
	}

	//second pass, compute offsets and modify sizes
	for( i = 0; i < (int)mColumnList.size(); i++ )
	{
		ColumnDescriptor& cd = mColumnList[i];
		if( cd.mSize > 0 && cd.mSize < 5 )
		{
			//an intergral object or flag
			cd.mOffset = offsets[cd.mSize];
			int size = 1 << ( cd.mSize - 1 );
			offsets[cd.mSize] += size;
			cd.mSize = size;
		}
		else if( cd.mSize == 0 )
		{
			//bools
			cd.mOffset = offsets[0]++;
			cd.mSize = -1;
		}
		else
		{
			//pointers
			cd.mOffset = offset;
			cd.mSize = sizeof( void* );
			offset += cd.mSize;
		}
	}
	return 0;
}


PyObject* RowDescriptor::ToPython( PyObject* blueModule )
{
	TTIMER2( "DB::NSession::ToPython::RowDescriptor" );
	BluePy raw( PyCapsule_New( &mColumnList, "DBRowDescriptor", 0 ) );
	if( !raw )
		return 0;
	return PyObject_CallMethod( blueModule, (char*)"DBRowDescriptor", (char*)"O", raw.o );
}