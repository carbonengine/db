// Copyright © 2006 CCP ehf.

/* 
	*************************************************************************

	StringStore.h

	Project:   EVE Server Database Access

	Description:   

		This guy provides functionality to reuse string and data objects from
		the database.  It is a singleton storage for byte data.
		When getting the rowset, all strings are entered into this, and it
		returns a pointer to a StringStore element, possibly shared.
		
	Dependencies:

		Python

	*************************************************************************
*/

#ifndef STRINGSTORE_H
#define STRINGSTORE_H

#include <unordered_set>
#include "DelayedException.h"
#include "BlockAllocator.h"


class StoreElementBase
{
public:
	virtual PyObject* GetPython() = 0;
};


template <typename DataType, bool IsText>
class InternalStoreElement : public StoreElementBase
{
public:
    explicit InternalStoreElement(DataType* data, const size_t elems)
    {
    	mPython = false;
    	mData = data;
    	mElements = elems;
    	mHash = hash();
    }

    InternalStoreElement(const InternalStoreElement& o)
    {
    	mPython = o.mPython;
    	if(mPython)
    	{
    		mObject = o.mObject;
    		Py_XINCREF( mObject );
    	}
    	else
    	{
    		mData = o.mData;
    		mElements = o.mElements;
    		mHash = o.mHash;
    	}
    }

	~InternalStoreElement()
    {
	    if (mPython)
	    {
		    Py_XDECREF( mObject );
	    }
    }

    InternalStoreElement& operator =(const InternalStoreElement& o)
    {
    	if(o.mPython)
    		Py_XINCREF( o.mObject );
    	if(mPython)
    		Py_XDECREF( mObject );
    	mPython = o.mPython;
    	if(mPython)
    	{
    		mObject = o.mObject;
    	}
    	else
    	{
    		mData = o.mData;
    		mElements = o.mElements;
    		mHash = o.mHash;
    	}
    	return *this;
    }

    bool operator <(const InternalStoreElement& rhs) const
    {
	    if(mPython)
	    {
	    	return false; // accessing mData is undefined behavior
	    }

	    int cmp = memcmp( mData, rhs.mData, min( mElements, rhs.mElements ) );
	    if(cmp != 0)
	    {
	    	return cmp < 0;
	    }

	    return mElements < rhs.mElements;
    }

	bool operator ==(const InternalStoreElement& rhs) const
    {
    	if(mPython || rhs.mPython)
    	{
    		// Already converted to Python, so the raw data is gone.
    		return this == &rhs;
    	}

    	if(mElements != rhs.mElements)
    	{
    		return false;
    	}

    	return memcmp( mData, rhs.mData, mElements * sizeof( DataType ) ) == 0;
    }

	explicit operator size_t() const { return mHash; }

    DelayedException* Claim(SimplePoolAllocator& allocator)
    {
	    _ASSERT( !mPython );
	    const size_t elen = sizeof( DataType );
	    auto tmp = static_cast<DataType*>(allocator.align( mElements * elen, static_cast<int>(elen) ));
	    if(!tmp)
	    {
	    	return DelayedException::NoMem();
	    }
	    memcpy( tmp, mData, mElements * elen );
	    mData = tmp;
	    return nullptr;
    }

    PyObject* GetPython() override
    {
    	if(!mPython && !ToPython())
    	{
    		return nullptr;
    	}
    	_ASSERT( mPython );
    	return mObject;
    }

	size_t hash() const
	{
		// hash range of elements
		size_t hash = 2166136261U;
		const size_t maxHash = 64;
		DataType* begin = mData;
		DataType* end = mData + min(mElements, maxHash);
		while (begin != end)
			hash = 16777619U * hash ^ (size_t)*begin++;

		return hash;
	}


private:
	bool ToPython();


	// raw data and python object are stored in a mutually exclusive manner for memory efficiency
	union
    {
		struct
		{
		    DataType* mData;
		    size_t mElements;
		    size_t mHash;
		};

		PyObject* mObject;
    };

	// true if mObject is active part of storage union
    bool mPython;
};



using StringStoreElement = InternalStoreElement<char, true>;		// utf-8 storage element
using ByteStoreElement = InternalStoreElement<char, false>;			// byte storage element
using WStringStoreElement = InternalStoreElement<wchar_t, true>;	// unicode storage element

template<>
struct std::hash<StringStoreElement>
{
	std::size_t operator()(const StringStoreElement& s) const noexcept
	{
		return s.hash();
	}
};

template<>
struct std::hash<ByteStoreElement>
{
	std::size_t operator()(const ByteStoreElement& s) const noexcept
	{
		return s.hash();
	}
};

template<>
struct std::hash<WStringStoreElement>
{
	std::size_t operator()(const WStringStoreElement& s) const noexcept
	{
		return s.hash();
	}
};


template <typename ElementType, typename DataType>
class InternalStore
{
public:
	explicit InternalStore(SimplePoolAllocator& allocator) :
	m_stlAllocator(allocator),
	mSet( m_stlAllocator )
	{
		mMemSaved = 0;
	}

    DelayedException* Insert(ElementType*& res, DataType* data, size_t elems)
    {
    	ElementType tmp( data, elems );
    	std::pair<InternalStoreSet_iterator, bool> r = mSet.insert( tmp );

    	// This is slightly evil. The insert gives us back a const iterator
    	// to the StringStoreElem either found or just added. This used to be a
    	// regular iterator, but with VS2010 it is a const iterator.
    	// It would be cleaner to change this to a map - separate the
    	// key out from the value. We know that the ordering operator
    	// used on the StringStoreElem will not change so this does work - no
    	// operations we can do on the element will invalidate the set.
    	// Note that the std::set documentation clearly states that
    	// values should not change after they are added to the set.
    	InternalStoreSet_iterator& elIt = r.first;
    	auto& el = const_cast<ElementType&>(*elIt);

    	res = 0;
    	if(r.second)
    	{
    		DelayedException* e = el.Claim( m_stlAllocator.mA );
    		if(e)
    		{
    			return e;
    		}
    	}
    	else
    	{
    		mMemSaved += elems * sizeof( DataType );
    	}
    	res = &el;
    	return 0;
    }

    size_t GetMemSaved() const { return mMemSaved; }

private:
    using traits_t = std::hash<ElementType>;
    using keyEqual = std::equal_to<ElementType>;
    using allocator_t = StlPoolAllocator<ElementType>;

	using InternalStoreSet_t = std::unordered_set<ElementType, traits_t, keyEqual, allocator_t>;
    using InternalStoreSet_iterator = typename InternalStoreSet_t::iterator;

	allocator_t m_stlAllocator;
    InternalStoreSet_t mSet;
    size_t mMemSaved; // how much memory (without overhead) saved by the reuse
};

 struct Store
{
	explicit Store(SimplePoolAllocator& allocator) : stringStore(allocator), byteStore(allocator), wStringStore(allocator){}

	size_t GetMemSaved() const { return stringStore.GetMemSaved() + byteStore.GetMemSaved() + wStringStore.GetMemSaved(); }

	InternalStore<StringStoreElement, char> stringStore;
	InternalStore<ByteStoreElement, char> byteStore;
	InternalStore<WStringStoreElement, wchar_t> wStringStore;
};


template <>
inline bool InternalStoreElement<wchar_t, true>::operator <( const InternalStoreElement& rhs ) const
{
	if(mPython)
	{
		return false; // accessing mData is undefined behavior
	}

	int cmp = wmemcmp( mData, rhs.mData, min( mElements, rhs.mElements ) );
	if(cmp != 0)
	{
		return cmp < 0;
	}

	return mElements < rhs.mElements;
}


#endif //STRINGSTORE_H
