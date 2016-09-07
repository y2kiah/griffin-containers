/**
* @file handle_map.h
* @author Jeff Kiah
* @copyright The MIT License (MIT), Copyright (c) 2015 Jeff Kiah
*/
#pragma once
#ifndef GRIFFIN_HANDLE_MAP_H_
#define GRIFFIN_HANDLE_MAP_H_

#include <cstdint>
#include <vector>

namespace griffin {

	/**
	* @struct Id_T
	* @var	free		0 if active, 1 if slot is part of freelist, only applicable to inner ids
	* @var	typeId		relates to m_itemTypeId parameter of handle_map
	* @var	generation	incrementing generation of data at the index, for tracking accesses to old data
	* @var	index		When used as a handle (outer id, given to the client):
	*						free==0, index of id in the sparseIds array
	*					When used as an inner id (stored in sparseIds array):
	*						free==0, index of the item in the dense items array
	*						free==1, index of next free slot, forming an embedded linked list
	* @var	value		unioned with the above four vars, used for direct comparison of ids
	*/
	struct Id_T {
		union {
			/**
			* the order of this bitfield is important for sorting prioritized by free, then typeId,
			* then generation, then index
			*/
			struct {
				uint32_t index;
				uint16_t generation;
				uint16_t typeId : 15;
				uint16_t free : 1;
			};
			uint64_t value;
		};
	};
	typedef std::vector<Id_T> IdSet_T;
	#define NullId_T	Id_T{}


	/**
	* @class handle_map
	*	Stores objects using a dense inner array and sparse outer array scheme for good cache coherence
	*	of the inner items. The sparse array contains handles (outer ids) used to identify the item,
	*	and provides an extra indirection allowing the inner array to move items in memory to keep them
	*	tightly packed. The sparse array contains an embedded FIFO freelist, where removed ids push to
	*	the back while new ids pop from the front.
	*
	* @tparam	T	type of item to be stored
	*/
	template <typename T>
	class handle_map {
	public:
		/**
		* @struct Meta_T
		*/
		struct Meta_T {
			uint32_t	denseToSparse;	//!< index into m_sparseIds array stored in m_meta
		};

		typedef std::vector<T>      DenseSet_T;
		typedef std::vector<Meta_T> MetaSet_T;

		// Functions

		/**
		* Get a direct reference to a stored item by handle
		* @param[in]	handle		id of the item
		* @returns reference to the item
		*/
		T&			at(Id_T handle);
		const T&	at(Id_T handle) const;
		T&			operator[](Id_T handle)			{ return at(handle); }
		const T&	operator[](Id_T handle) const	{ return at(handle); }

		/**
		* create one item with default initialization
		* @tparam		Params	initialization arguments passed to constructor of item
		* @returns the id
		*/
		template <typename... Params>
		Id_T emplace(Params... args) { return insert(T{ args... }); }

		/**
		* create n items with initialization args specified by Params, return vector of ids
		* @param[in]	n		number of items to create
		* @tparam		Params	initialization arguments passed to constructor of each item created
		* @returns a collection of ids
		*/
		template <typename... Params>
		IdSet_T emplaceItems(int n, Params... args);

		/**
		* iterators over the dense set, they are invalidated by inserting and removing
		*/
		typename DenseSet_T::iterator		begin()			{ return m_items.begin(); }
		typename DenseSet_T::const_iterator	cbegin() const	{ return m_items.cbegin(); }
		typename DenseSet_T::iterator		end()			{ return m_items.end(); }
		typename DenseSet_T::const_iterator	cend() const	{ return m_items.cend(); }

		/**
		* remove the item identified by the provided handle
		* @param[in]	handle		id of the item
		* @returns count of items removed (0 or 1)
		*/
		size_t erase(Id_T handle);

		/**
		* remove the items identified in the set of handles
		* @param[in]	handles		set of ids
		* @returns count of items removed
		*/
		size_t eraseItems(const IdSet_T& handles);

		/**
		* add one item, forwarding the provided i into the store, return id
		* @param[in]	i	rvalue ref of of the object to move into inner storage
		* @returns the id
		*/
		Id_T insert(T&& i);

		/**
		* add one item, copying the provided i into the store, return id
		* @param[in]	i	const ref of of the object to copy into inner storage
		* @returns the id
		*/
		Id_T insert(const T& i);

		/**
		* Removes all items, leaving the m_sparseIds set intact by adding each entry to the free-
		* list and incrementing its generation. This operation is slower than @c reset, but safer
		* for the detection of stale handle lookups later (in debug builds). Prefer to use @c reset
		* if safety is not a concern.
		* Complexity is linear.
		*/
		void clear() _NOEXCEPT;

		/**
		* Removes all items, destroying the m_sparseIds set. Leaves the container's capacity, but
		* otherwise equivalent to a default-constructed container. This is faster than @c clear,
		* but cannot safely detect lookups by stale handles obtained before the reset. Use @c clear
		* if safety is a concern, at least until it's proven not to be a problem.
		* Complexity is constant.
		*/
		void reset() _NOEXCEPT;

		/**
		* @returns true if handle handle refers to a valid item
		*/
		bool isValid(Id_T handle) const;

		/**
		* @returns size of the dense items array
		*/
		size_t size() const _NOEXCEPT { return m_items.size(); }

		/**
		* @returns capacity of the dense items array
		*/
		size_t capacity() const _NOEXCEPT { return m_items.capacity(); }

		/**
		* defragment uses the comparison function @c comp to establish an ideal order for the dense
		*	set in order to maximum cache locality for traversals. The dense set can become
		*	fragmented over time due to removal operations. This can be an expensive operation, so
		*	the sort operation is reentrant. Use the @c maxSwaps parameter to limit the number of
		*	swaps that will occur before the function returns.
		* @param[in]	comp	comparison function object, function pointer, or lambda
		* @param[in]	maxSwaps	maximum number of items to reorder in the insertion sort
		*	before the function returns. Pass 0 (default) to run until completion.
		* @tparam	Compare	comparison function object which returns ?true if the first argument is
		*	greater than (i.e. is ordered after) the second. The signature of the comparison
		*	function should be equivalent to the following:
		*	@code bool cmp(const T& a, const T& b); @endcode
		*	The signature does not need to have const &, but the function object must not modify
		*	the objects passed to it.
		* @returns the number of swaps that occurred, keeping in mind that this value could
		*	overflow on very large data sets
		*/
		template <typename Compare>
		size_t	defragment(Compare comp, size_t maxSwaps = 0);


		/**
		* these functions provide direct access to inner arrays, don't add or remove items, just
		* use them for lookups and iterating over the items
		*/
		DenseSet_T&			getItems()					{ return m_items; }
		const DenseSet_T&	getItems() const			{ return m_items; }
		MetaSet_T&			getMeta()					{ return m_meta; }
		const MetaSet_T&	getMeta() const				{ return m_meta; }
		IdSet_T&			getIds()					{ return m_sparseIds; }
		const IdSet_T&		getIds() const				{ return m_sparseIds; }

		uint32_t			getFreeListFront() const	{ return m_freeListFront; }
		uint32_t			getFreeListBack() const		{ return m_freeListBack; }

		uint16_t			getItemTypeId() const		{ return m_itemTypeId; }

		/**
		* @returns index into the inner DenseSet for a given outer id
		*/
		uint32_t			getInnerIndex(Id_T handle) const;
		
		/**
		* Constructor
		* @param	itemTypeId		typeId used by the Id_T::typeId variable for this container
		* @param	reserveCount	reserve space for inner storage
		*/
		explicit handle_map(uint16_t itemTypeId, size_t reserveCount)
			: m_itemTypeId(itemTypeId)
		{
			m_sparseIds.reserve(reserveCount);
			m_items.reserve(reserveCount);
			m_meta.reserve(reserveCount);
		}

	private:

		/**
		* freeList is empty when the front is set to 32 bit max value (the back will match)
		* @returns true if empty
		*/
		bool freeListEmpty() const { return (m_freeListFront == 0xFFFFFFFF); }

		// Variables

		uint32_t	m_freeListFront = 0xFFFFFFFF; //!< start index in the embedded ComponentId freelist
		uint32_t	m_freeListBack  = 0xFFFFFFFF; //!< last index in the freelist

		uint16_t	m_itemTypeId;	//!< the Id_T::typeId to use for ids produced by this handle_map<T>
		
		uint8_t		m_fragmented = 0; //<! set to 1 if modified by insert or erase since last complete defragment

		IdSet_T		m_sparseIds;	//!< stores a set of Id_Ts, these are "inner" ids indexing into m_items
		DenseSet_T	m_items;		//!< stores items of type T
		MetaSet_T	m_meta;			//!< stores Meta_T type for each item
	};

}

#include "impl/handle_map-inl.h"

#endif