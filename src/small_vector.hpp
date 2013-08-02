/***************************************************************************
 *   Copyright (C) 2009-2011 by Francesco Biscani                          *
 *   bluescarni@gmail.com                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef PIRANHA_SMALL_VECTOR_HPP
#define PIRANHA_SMALL_VECTOR_HPP

#include <boost/integer_traits.hpp>
#include <cstddef>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>

#include "config.hpp"
#include "static_vector.hpp"
#include "type_traits.hpp"

namespace piranha {

namespace detail
{

// This is essentially a reduced std::vector replacement that should use less storage on most implementations
// (e.g., it has a size of 16 on Linux 64-bit and it _should_ have a size of 8 on many 32-bit archs).
// NOTE: earlier versions of this class used to have support for custom allocation. See, e.g., commit
// 6e1dd235c729ed05eb6e872dceece2f20a624f32
// It seems however that there are some defects currently in the standard C++ library that make it quite hard
// to implement reasonably strong exception safety and nothrow guarantees. See, e.g.,
// http://cplusplus.github.io/LWG/lwg-defects.html#2103
// It seems like in the current standard it is not possible to implemement noexcept move assignment not even
// for std::alloc. The crux of the matter is that if propagate_on_container_move_assignment is not defined,
// we will need in general a memory allocation (that can fail).
// Another, less important, problem is that for copy assignment the presence of
// propagate_on_container_copy_assignment seems to force one to nuke pre-emptively the content of the container
// before doing anything (thus preventing strong forms of exception safety).
// The approach taken here is thus to just hard-code std::allocator, which is stateless, and force nothrow
// behaviour when doing assignments. This should work with all stateless allocators (including our own allocator).
// Unfortunately it is not clear if the extension allocators from GCC are stateless, so better not to muck
// with them (apart from experimentation).
// See also:
// http://stackoverflow.com/questions/12332772/why-arent-container-move-assignment-operators-noexcept
template <typename T>
class dynamic_storage
{
		PIRANHA_TT_CHECK(is_container_element,T);
	public:
		using size_type = unsigned short;
		using value_type = T;
		using allocator_type = std::allocator<T>;
	private:
		using a_traits = std::allocator_traits<allocator_type>;
		using pointer = typename a_traits::pointer;
		using const_pointer = typename a_traits::const_pointer;
		// NOTE: common tuple implementations use EBCO, try to reduce size in case
		// of stateless allocators (such as std::allocator).
		// http://flamingdangerzone.com/cxx11/2012/07/06/optimal-tuple-i.html
		using pair_type = std::tuple<pointer,allocator_type>;
	public:
		// NOTE: this is guaranteed to be within the range of unsigned short.
		static const size_type max_size = 32768u;
		using iterator = pointer;
		using const_iterator = const_pointer;
		dynamic_storage():m_pair(nullptr,allocator_type()),m_size(0u),m_capacity(0u) {}
		// NOTE: move ctor of allocator is guaranteed not to throw (23.2.1/7).
		dynamic_storage(dynamic_storage &&other) noexcept : m_pair(std::move(other.m_pair)),m_size(other.m_size),m_capacity(other.m_capacity)
		{
			// Erase the other.
			other.ptr() = nullptr;
			other.m_size = 0u;
			other.m_capacity = 0u;
		}
		// NOTE: just keep the used of a_traits for copying the allocator, it won't hurt.
		dynamic_storage(const dynamic_storage &other) : m_pair(nullptr,a_traits::select_on_container_copy_construction(other.alloc())),
			m_size(0u),m_capacity(other.m_size) // NOTE: when copying, we set the capacity to the same value of the size.
		{
			// Obtain storage.
			// NOTE: here we are using the allocator obtained from copy, and we know the allocator can satisfy this (in terms of max_size()) as it
			// satisfied already a request equal or larger in other. Will just return nullptr if requested size is zero.
			ptr() = obtain_new_storage(other.m_size);
			// Attempt to copy-construct the elements from other.
			try {
				for (; m_size < other.m_size; ++m_size) {
					a_traits::construct(alloc(),ptr() + m_size,other[m_size]);
				}
			} catch (...) {
				// Roll back the construction and deallocate before re-throwing.
				destroy_and_deallocate();
				throw;
			}
		}
		~dynamic_storage() noexcept
		{
			destroy_and_deallocate();
		}
		dynamic_storage &operator=(dynamic_storage &&other) noexcept
		{
			if (likely(this != &other)) {
				// Destroy and deallocate this.
				destroy_and_deallocate();
				// Just pilfer the resources.
				m_pair = std::move(other.m_pair);
				m_size = other.m_size;
				m_capacity = other.m_capacity;
				// Nuke the other.
				other.ptr() = nullptr;
				other.m_size = 0u;
				other.m_capacity = 0u;
			}
			return *this;
		}
		dynamic_storage &operator=(const dynamic_storage &other)
		{
			if (likely(this != &other)) {
				dynamic_storage tmp(other);
				*this = std::move(tmp);
			}
			return *this;
		}
		bool empty() const
		{
			return m_size == 0u;
		}
		void push_back(const value_type &x)
		{
			push_back_impl(x);
		}
		void push_back(value_type &&x)
		{
			push_back_impl(std::move(x));
		}
		value_type &operator[](const size_type &n)
		{
			piranha_assert(n < m_size);
			return ptr()[n];
		}
		const value_type &operator[](const size_type &n) const
		{
			piranha_assert(n < m_size);
			return ptr()[n];
		}
		size_type size() const
		{
			return m_size;
		}
		iterator begin()
		{
			return ptr();
		}
		iterator end()
		{
			// NOTE: in case m_size is zero, this is guaranteed to return
			// the original pointer (5.7/7).
			return ptr() + m_size;
		}
		const_iterator begin() const
		{
			return ptr();
		}
		const_iterator end() const
		{
			return ptr() + m_size;
		}
		void reserve(const size_type &new_capacity)
		{
			piranha_assert(consistency_checks());
			// No need to do anything if we already have enough capacity.
			if (new_capacity <= m_capacity) {
				return;
			}
			// Check that we are not asking too much.
			// NOTE: a_traits::max_size() returns an unsigned integer type, so we are sure the comparison
			// will work as expected.
			// http://en.cppreference.com/w/cpp/concept/Allocator
			if (unlikely(new_capacity > max_size || new_capacity > a_traits::max_size(alloc()))) {
				piranha_throw(std::bad_alloc,);
			}
			// Start by allocating the new storage. New capacity is at least one at this point.
			pointer new_storage = obtain_new_storage(new_capacity);
			// Move in existing elements. Consistency checks ensure
			// that m_size is not greater than m_capacity and, by extension, new_capacity.
			for (size_type i = 0u; i < m_size; ++i) {
				a_traits::construct(alloc(),new_storage + i,std::move((*this)[i]));
			}
			// Destroy and deallocate original content.
			destroy_and_deallocate();
			// Move in the new pointer and capacity.
			ptr() = new_storage;
			m_capacity = new_capacity;
		}
		size_type capacity() const
		{
			return m_capacity;
		}
	private:
		// Common implementation of push_back().
		template <typename U>
		void push_back_impl(U &&x)
		{
			piranha_assert(consistency_checks());
			if (unlikely(m_capacity == m_size)) {
				increase_capacity();
			}
			a_traits::construct(alloc(),ptr() + m_size,std::forward<U>(x));
			++m_size;
		}
		void destroy_and_deallocate() noexcept
		{
			piranha_assert(consistency_checks());
			for (size_type i = 0u; i < m_size; ++i) {
				// NOTE: we do not try to use POD optimisations here as the allocator
				// might have side effects.
				// NOTE: exceptions here will call std::abort(), as we are in noexcept land.
				a_traits::destroy(alloc(),ptr() + i);
			}
			if (ptr() != nullptr) {
				// NOTE: deallocate can never throw.
				// http://en.cppreference.com/w/cpp/concept/Allocator
				a_traits::deallocate(alloc(),ptr(),m_capacity);
			}
		}
		// Obtain new storage, and throw an error in case something goes wrong.
		pointer obtain_new_storage(const size_type &size)
		{
			if (size == 0u) {
				return pointer(nullptr);
			}
			pointer storage = a_traits::allocate(alloc(),size);
			if (unlikely(storage == nullptr)) {
				piranha_throw(std::bad_alloc,);
			}
			return storage;
		}
		// Will try to double the capacity, or, in case this is not possible,
		// will set the capacity to max_size. If the initial capacity is already max,
		// then will throw an error.
		void increase_capacity()
		{
			if (unlikely(m_capacity == max_size)) {
				piranha_throw(std::bad_alloc,);
			}
			// NOTE: capacity should double, but without going past max_size, and in case it is zero it should go to 1.
			const size_type new_capacity = (m_capacity > max_size / 2u) ? max_size : ((m_capacity != 0u) ?
				static_cast<size_type>(m_capacity * 2u) : static_cast<size_type>(1u));
			reserve(new_capacity);
		}
		pointer &ptr()
		{
			return std::get<0u>(m_pair);
		}
		const pointer &ptr() const
		{
			return std::get<0u>(m_pair);
		}
		allocator_type &alloc()
		{
			return std::get<1u>(m_pair);
		}
		const allocator_type &alloc() const
		{
			return std::get<1u>(m_pair);
		}
		bool consistency_checks()
		{
			// Size cannot be greater than capacity.
			if (unlikely(m_size > m_capacity)) {
				return false;
			}
			// If we allocated something, capacity cannot be zero.
			if (unlikely(ptr() != nullptr && m_capacity == 0u)) {
				return false;
			}
			// If we have nothing allocated, capacity must be zero.
			if (unlikely(ptr() == nullptr && m_capacity != 0u)) {
				return false;
			}
			return true;
		}
	private:
		pair_type	m_pair;
		size_type	m_size;
		size_type	m_capacity;
};

// TMP to determine automatically the size of the static storage in small_vector.
template <typename T, std::size_t Size = 1u, typename = void>
struct auto_static_size
{
	static const std::size_t value = Size;
};

template <typename T, std::size_t Size>
struct auto_static_size<T,Size,typename std::enable_if<
	(sizeof(dynamic_storage<T>) > sizeof(static_vector<T,Size>))
	>::type>
{
	static_assert(Size < boost::integer_traits<std::size_t>::const_max,"Overflow error in auto_static_size.");
	static const std::size_t value = auto_static_size<T,Size + 1u>::value;
};

}

/// Small vector class.
/**
 * This class is a sequence container similar to the standard <tt>std::vector</tt> class. The class will avoid dynamic
 * memory allocation by using internal static storage up to a certain number of elements. If \p Size is zero, this
 * number is calculated automatically (so that the size of the static storage block is not greater than the totals size of the
 * members used to manage dynamically-allocated memory). Otherwise, the limit number is set to \p Size.
 *
 * \section type_requirements Type requirements
 *
 * \p T must satisfy piranha::is_container_element.
 *
 * \section exception_safety Exception safety guarantee
 *
 * Unless otherwise specified, this class provides the strong exception safety guarantee for all operations.
 *
 * \section move_semantics Move semantics
 *
 * After a move operation, the size of the container will not change, and its elements will be left in a moved-from state.
 *
 * @author Francesco Biscani (bluescarni@gmail.com)
 */
template <typename T, std::size_t Size = 0u>
class small_vector
{
		PIRANHA_TT_CHECK(is_container_element,T);
		using s_storage = typename std::conditional<Size == 0u,static_vector<T,detail::auto_static_size<T>::value>,static_vector<T,Size>>::type;
		using d_storage = detail::dynamic_storage<T>;
		static constexpr std::size_t get_max(const std::size_t &a, const std::size_t &b)
		{
			return (a > b) ? a : b;
		}
		static const std::size_t align_union = get_max(alignof(s_storage),alignof(d_storage));
		static const std::size_t size_union = get_max(sizeof(s_storage),sizeof(d_storage));
		typedef typename std::aligned_storage<size_union,align_union>::type storage_type;
		s_storage *get_s()
		{
			return static_cast<s_storage *>(get_vs());
		}
		const s_storage *get_s() const
		{
			return static_cast<const s_storage *>(get_vs());
		}
		d_storage *get_d()
		{
			return static_cast<d_storage *>(get_vs());
		}
		const d_storage *get_d() const
		{
			return static_cast<const d_storage *>(get_vs());
		}
		void *get_vs()
		{
			return static_cast<void *>(&m_storage);
		}
		const void *get_vs() const
		{
			return static_cast<const void *>(&m_storage);
		}
		// The size type will be the one with most range among the two storages.
		using size_type_impl = typename std::conditional<(boost::integer_traits<typename s_storage::size_type>::const_max >
			boost::integer_traits<typename d_storage::size_type>::const_max),typename s_storage::size_type,
			typename d_storage::size_type>::type;
	public:
		/// An unsigned integer type representing the number of elements stored in the vector.
		using size_type = size_type_impl;
		small_vector():m_static(true)
		{
			::new (get_vs()) s_storage();
		}
		small_vector(const small_vector &other):m_static(other.m_static)
		{
			if (m_static) {
				::new (get_vs()) s_storage(*other.get_s());
			} else {
				::new (get_vs()) d_storage(*other.get_d());
			}
		}
		~small_vector() noexcept
		{
			if (m_static) {
				get_s()->~s_storage();
			} else {
				get_d()->~d_storage();
			}
		}
		size_type size() const
		{
			if (m_static) {
				return get_s()->size();
			}
			return get_d()->size();
		}
	private:
		storage_type	m_storage;
		bool		m_static;
};

}

#endif