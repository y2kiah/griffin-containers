/**
* @file handle_map-inl.h
* @author Jeff Kiah
* @copyright The MIT License (MIT), Copyright (c) 2015 Jeff Kiah
*/
#pragma once
#ifndef GRIFFIN_HANDLE_MAP_INL_H_
#define GRIFFIN_HANDLE_MAP_INL_H_

#include "../handle_map.h"
#include <cassert>
#include <algorithm>
#include <type_traits>

namespace griffin {

	// struct Id_T comparison functions

	inline bool operator==(const Id_T& a, const Id_T& b) { return (a.value == b.value); }
	inline bool operator!=(const Id_T& a, const Id_T& b) { return (a.value != b.value); }
	inline bool operator< (const Id_T& a, const Id_T& b) { return (a.value < b.value); }
	inline bool operator> (const Id_T& a, const Id_T& b) { return (a.value > b.value); }

	// class handle_map 

	template <typename T>
	Id_T handle_map<T>::insert(T&& i)
	{
		Id_T handle = { 0 };
		m_fragmented = 1;

		if (freeListEmpty()) {
			Id_T innerId = {
				(uint32_t)m_items.size(),
				1,
				m_itemTypeId,
				0
			};

			handle = innerId;
			handle.index = (uint32_t)m_sparseIds.size();

			m_sparseIds.push_back(innerId);
		}
		else {
			uint32_t outerIndex = m_freeListFront;
			Id_T &innerId = m_sparseIds.at(outerIndex);

			m_freeListFront = innerId.index; // the index of a free slot refers to the next free slot
			if (freeListEmpty()) {
				m_freeListBack = m_freeListFront;
			}

			// convert the index from freelist to inner index
			innerId.free = 0;
			innerId.index = (uint32_t)m_items.size();

			handle = innerId;
			handle.index = outerIndex;
		}

		m_items.push_back(std::forward<T>(i));
		m_meta.push_back({ handle.index });

		return handle;
	}


	template <typename T>
	Id_T handle_map<T>::insert(const T& i)
	{
		return insert(T{ i });
	}


	template <typename T>
	template <typename... Params>
	IdSet_T handle_map<T>::emplaceItems(int n, Params... args)
	{
		IdSet_T handles(n);
		assert(n > 0 && "emplaceItems called with n = 0");
		m_fragmented = 1;

		m_items.reserve(m_items.size() + n); // reserve the space we need (if not already there)
		m_meta.reserve(m_meta.size() + n);

		std::generate_n(handles.begin(), n, [&, this](){ return emplace(args...); });

		return handles; // efficient to return vector by value, copy elided with NVRO (or with C++11 move semantics)
	}


	template <typename T>
	size_t handle_map<T>::erase(Id_T handle)
	{
		if (!isValid(handle)) {
			return 0;
		}
		m_fragmented = 1;

		Id_T innerId = m_sparseIds[handle.index];
		uint32_t innerIndex = innerId.index;

		// push this slot to the back of the freelist
		innerId.free = 1;
		++innerId.generation; // increment generation so remaining outer ids go stale
		innerId.index = 0xFFFFFFFF; // max numeric value represents the end of the freelist
		m_sparseIds[handle.index] = innerId; // write outer id changes back to the array

		if (freeListEmpty()) {
			// if the freelist was empty, it now starts (and ends) at this index
			m_freeListFront = handle.index;
			m_freeListBack = m_freeListFront;
		}
		else {
			m_sparseIds[m_freeListBack].index = handle.index; // previous back of the freelist points to new back
			m_freeListBack = handle.index; // new freelist back is stored
		}

		// remove the component by swapping with the last element, then pop_back
		if (m_items.size() > 1) {
			std::swap(m_items.at(innerIndex), m_items.back());
			std::swap(m_meta.at(innerIndex), m_meta.back());

			// fix the ComponentId index of the swapped component
			m_sparseIds[m_meta.at(innerIndex).denseToSparse].index = innerIndex;
		}

		m_items.pop_back();
		m_meta.pop_back();

		return 1;
	}

	
	template <typename T>
	size_t handle_map<T>::eraseItems(const IdSet_T& handles)
	{
		size_t count = 0;
		for (auto h : handles) {
			count += erase(h);
		}
		return count;
	}

	
	template <typename T>
	void handle_map<T>::clear() _NOEXCEPT
	{
		uint32_t size = static_cast<uint32_t>(m_sparseIds.size());

		if (size > 0) {
			m_items.clear();
			m_meta.clear();

			m_freeListFront = 0;
			m_freeListBack = size - 1;
			m_fragmented = 0;

			for (uint32_t i = 0; i < size; ++i) {
				auto& id = m_sparseIds[i];
				id.free = 1;
				++id.generation;
				id.index = i + 1;
			}
			m_sparseIds[size - 1].index = 0xFFFFFFFF;
		}
	}


	template <typename T>
	void handle_map<T>::reset() _NOEXCEPT
	{
		m_freeListFront = 0xFFFFFFFF;
		m_freeListBack = 0xFFFFFFFF;
		m_fragmented = 0;

		m_items.clear();
		m_meta.clear();
		m_sparseIds.clear();
	}


	template <typename T>
	inline T& handle_map<T>::at(Id_T handle)
	{
		assert(handle.index < m_sparseIds.size() && "outer index out of range");

		Id_T innerId = m_sparseIds[handle.index];

		assert(handle.typeId == m_itemTypeId && "typeId mismatch");
		assert(handle.generation == innerId.generation && "at called with old generation");
		assert(innerId.index < m_items.size() && "inner index out of range");
		
		return m_items[innerId.index];
	}


	template <typename T>
	inline const T& handle_map<T>::at(Id_T handle) const
	{
		assert(handle.index < m_sparseIds.size() && "outer index out of range");

		Id_T innerId = m_sparseIds[handle.index];

		assert(handle.typeId == m_itemTypeId && "typeId mismatch");
		assert(handle.generation == innerId.generation && "at called with old generation");
		assert(innerId.index < m_items.size() && "inner index out of range");

		return m_items[innerId.index];
	}


	template <typename T>
	inline bool handle_map<T>::isValid(Id_T handle) const
	{
		if (handle.index >= m_sparseIds.size()) {
			return false;
		}
		
		Id_T innerId = m_sparseIds[handle.index];
		
		return (innerId.index < m_items.size() &&
				handle.typeId == m_itemTypeId &&
				handle.generation == innerId.generation);
	}


	template <typename T>
	inline uint32_t handle_map<T>::getInnerIndex(Id_T handle) const
	{
		assert(handle.index < m_sparseIds.size() && "outer index out of range");

		Id_T innerId = m_sparseIds[handle.index];

		assert(handle.typeId == m_itemTypeId && "typeId mismatch");
		assert(handle.generation == innerId.generation && "at called with old generation");
		assert(innerId.index < m_items.size() && "inner index out of range");

		return innerId.index;
	}


	template <typename T>
	template <typename Compare>
	size_t handle_map<T>::defragment(Compare comp, size_t maxSwaps)
	{
		if (m_fragmented == 0) { return 0; }
		size_t swaps = 0;
		
		int i = 1;
		for (; i < m_items.size() && (maxSwaps == 0 || swaps < maxSwaps); ++i) {
			T tmp = m_items[i];
			Meta_T tmpMeta = m_meta[i];

			int j = i - 1;
			int j1 = j + 1;

			// trivially copyable implementation
			if (std::is_trivially_copyable<T>::value) {
				while (j >= 0 && comp(m_items[j], tmp)) {
					m_sparseIds[m_meta[j].denseToSparse].index = j1;
					--j;
					--j1;
				}
				if (j1 != i) {
					memmove(&m_items[j1+1], &m_items[j1], sizeof(T) * (i - j1));
					memmove(&m_meta[j1+1], &m_meta[j1], sizeof(Meta_T) * (i - j1));
					++swaps;

					m_items[j1] = tmp;
					m_meta[j1] = tmpMeta;
					m_sparseIds[m_meta[j1].denseToSparse].index = j1;
				}
			}
			// standard implementation
			else {
				while (j >= 0 && (maxSwaps == 0 || swaps < maxSwaps) &&
					   comp(m_items[j], tmp))
				{
					m_items[j1] = std::move(m_items[j]);
					m_meta[j1] = std::move(m_meta[j]);
					m_sparseIds[m_meta[j1].denseToSparse].index = j1;
					--j;
					--j1;
					++swaps;
				}

				if (j1 != i) {
					m_items[j1] = tmp;
					m_meta[j1] = tmpMeta;
					m_sparseIds[m_meta[j1].denseToSparse].index = j1;
				}
			}
		}
		if (i == m_items.size()) {
			m_fragmented = 0;
		}

		return swaps;
	}

}

#endif