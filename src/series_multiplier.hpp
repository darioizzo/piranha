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

#ifndef PIRANHA_SERIES_MULTIPLIER_HPP
#define PIRANHA_SERIES_MULTIPLIER_HPP

#include <algorithm>
#include <atomic>
#include <boost/any.hpp>
#include <cmath>
#include <cstddef>
#include <future>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <new> // For bad_alloc.
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "cache_aligning_allocator.hpp"
#include "config.hpp"
#include "detail/series_fwd.hpp"
#include "detail/series_multiplier_fwd.hpp"
#include "exceptions.hpp"
#include "key_is_multipliable.hpp"
#include "memory.hpp"
#include "mp_integer.hpp"
#include "runtime_info.hpp"
#include "safe_cast.hpp"
#include "settings.hpp"
#include "thread_pool.hpp"
#include "tracing.hpp"
#include "tuning.hpp"
#include "type_traits.hpp"

namespace piranha
{

/// Default series multiplier.
/**
 * This class is used by the multiplication operators involving two series operands. The class works as follows:
 * 
 * - an instance of series multiplier is created using two series as construction arguments;
 * - when operator()() is called, an instance of \p Series is returned, representing
 *   the result of the multiplication of the two series used for construction.
 * 
 * Any specialisation of this class must respect the protocol described above (i.e., construction from series
 * instances and operator()() to compute the result). Note that this class is guaranteed to be used after the symbolic arguments of the series used for construction
 * have been merged (in other words, the two series have identical symbolic arguments sets).
 * 
 * ## Type requirements ##
 * 
 * \p Series must satisfy the piranha::is_series type trait.
 * 
 * ## Exception safety guarantee ##
 * 
 * This class provides the strong exception safety guarantee.
 * 
 * @author Francesco Biscani (bluescarni@gmail.com)
 */
/*
 * \todo we need a user-configurable parameter to determine when to use multiple threads.
 * \todo optimization in case one series has 1 term with unitary key and both series same type: multiply directly coefficients.
 * \todo think about the possibility of caching optimizations. For instance: merge the arguments of series coefficients, avoiding n ** 2 merge
 * operations during multiplication.
 * \todo possibly we could adopt some of the optimizations adopted, e.g., in the Kronecker multiplier. For instance, have a fast mode for the multiplier
 * to kick in when doing the full computation in order to avoid some branching in the insertion routines. The code though is already quite complex,
 * so better be very sure it is worth before embarking in this.
*/
template <typename Series, typename Enable = void>
class series_multiplier
{
		PIRANHA_TT_CHECK(is_series,Series);
		// Enabler for the call operator.
		template <typename T>
		using call_enabler = typename std::enable_if<key_is_multipliable<typename T::term_type::cf_type, typename T::term_type::key_type>::value,int>::type;
	public:
		/// Constructor.
		/**
		 * @param[in] s1 first series.
		 * @param[in] s2 second series.
		 * 
		 * @throws std::invalid_argument if the symbol sets of \p s1 and \p s2 differ.
		 * @throws unspecified any exception thrown by memory allocation errors in standard containers.
		 */
		explicit series_multiplier(const Series &s1, const Series &s2) : m_s1(&s1),m_s2(&s2)
		{
			using term_type = typename Series::term_type;
			if (s1.size() < s2.size()) {
				std::swap(m_s1,m_s2);
			}
			if (unlikely(m_s1->m_symbol_set != m_s2->m_symbol_set)) {
				piranha_throw(std::invalid_argument,"incompatible arguments sets");
			}
			if (unlikely(m_s1->empty() || m_s2->empty())) {
				return;
			}
			m_v1.reserve(m_s1->size());
			m_v2.reserve(m_s2->size());
			// Fill in the vectors of pointers.
			std::back_insert_iterator<decltype(m_v1)> bii1(m_v1);
			std::transform(m_s1->m_container.begin(),m_s1->m_container.end(),bii1,[](const term_type &t) {return &t;});
			std::back_insert_iterator<decltype(m_v2)> bii2(m_v2);
			std::transform(m_s2->m_container.begin(),m_s2->m_container.end(),bii2,[](const term_type &t) {return &t;});
		}
		/// Deleted copy constructor.
		series_multiplier(const series_multiplier &) = delete;
		/// Deleted move constructor.
		series_multiplier(series_multiplier &&) = delete;
		/// Deleted copy assignment operator.
		series_multiplier &operator=(const series_multiplier &) = delete;
		/// Deleted move assignment operator.
		series_multiplier &operator=(series_multiplier &&) = delete;
		/// Compute result of series multiplication.
		/**
		 * \note
		 * This operator is enabled only if the key type of \p Series satisfies piranha::key_is_multipliable.
		 *
		 * This method will call execute() using default_functor as multiplication functor.
		 * 
		 * @return the result of multiplying the two series.
		 * 
		 * @throws unspecified any exception thrown by execute().
		 */
		template <typename T = Series, call_enabler<T> = 0>
		Series operator()() const
		{
			return execute2<default_functor>();
		}
	protected:
		/// Low-level implementation of series multiplication.
		/**
		 * The multiplication algorithm proceeds as follows:
		 * 
		 * - if one of the two series is empty, a default-constructed instance of \p Series is returned;
		 * - a heuristic determines whether to enable multi-threaded mode or not;
		 * - in single-threaded mode:
		 *   - an instance of \p Functor is created and used to compute the return value via term-by-term multiplications and
		 *     insertions in the return series;
		 * - in multi-threaded mode:
		 *   - the first series is subdivided into segments and the same process described for single-threaded mode is run in parallel,
		 *     storing the multiple resulting series in a list;
		 *   - the series in the result list are merged into a single series via piranha::series::insert().
		 * 
		 * \p Functor must be a type exposing the same public interface as default_functor.
		 * It will be used to compute term-by-term multiplications and insert the terms into the return series.
		 * 
		 * @return the result of multiplying the first series by the second series.
		 * 
		 * @throws std::overflow_error in case of overflowing arithmetic operations on integral types.
		 * @throws unspecified any exception thrown by:
		 * - memory allocation errors in standard containers,
		 * - piranha::safe_cast(),
		 * - the public methods of \p Functor,
		 * - errors in threading primitives,
		 * - piranha::series::insert().
		 */
		template <typename Functor>
		Series execute() const
		{
			// Do not do anything if one of the two series is empty.
			if (unlikely(m_s1->empty() || m_s2->empty())) {
				// NOTE: requirement is ok, a series must be def-ctible.
				return Series{};
			}
			// This is the size type that will be used throughout the calculations.
			using size_type = decltype(m_v1.size());
			const size_type size1 = m_v1.size(), size2 = m_v2.size();
			piranha_assert(size1 && size2);
			// Establish the number of threads to use.
			size_type n_threads = safe_cast<size_type>(thread_pool::use_threads(
				integer(size1) * size2,integer(settings::get_min_work_per_thread())
			));
			piranha_assert(n_threads);
			// An additional check on n_threads is that its size is not greater than the size of the first series,
			// as we are using the first operand to split up the work.
			if (n_threads > size1) {
				n_threads = size1;
			}
			piranha_assert(n_threads >= 1u);
			if (likely(n_threads == 1u)) {
				Series retval;
				retval.m_symbol_set = m_s1->m_symbol_set;
				Functor f(&m_v1[0u],size1,&m_v2[0u],size2,retval);
				const auto tmp = rehasher(f);
				blocked_multiplication(f);
				if (tmp.first) {
					trace_estimates(retval.size(),tmp.second);
				}
				return retval;
			} else {
				// Build the return values and the multiplication functors.
				std::list<Series,cache_aligning_allocator<Series>> retval_list;
				std::list<Functor,cache_aligning_allocator<Functor>> functor_list;
				const auto block_size = size1 / n_threads;
				for (size_type i = 0u; i < n_threads; ++i) {
					// Last thread needs a different size from block_size.
					const size_type s1 = (i == n_threads - 1u) ? (size1 - i * block_size) : block_size;
					// NOTE: ok, series must be def-ctible.
					retval_list.emplace_back();
					retval_list.back().m_symbol_set = m_s1->m_symbol_set;
					functor_list.push_back(Functor(&m_v1[0u] + i * block_size,s1,&m_v2[0u],size2,retval_list.back()));
				}
				auto f_it = functor_list.begin();
				auto r_it = retval_list.begin();
				future_list<std::future<void>> f_list;
				try {
					for (size_type i = 0u; i < n_threads; ++i, ++f_it, ++r_it) {
						// Functor for use in the thread.
						auto f = [f_it,r_it,this]() {
							const auto tmp = this->rehasher(*f_it);
							this->blocked_multiplication(*f_it);
							if (tmp.first) {
								this->trace_estimates(r_it->m_container.size(),tmp.second);
							}
						};
						f_list.push_back(thread_pool::enqueue(static_cast<unsigned>(i),f));
					}
					f_list.wait_all();
					f_list.get_all();
				} catch (...) {
					f_list.wait_all();
					throw;
				}
				Series retval;
				retval.m_symbol_set = m_s1->m_symbol_set;
				auto final_estimate = estimate_final_series_size(Functor(&m_v1[0u],size1,&m_v2[0u],size2,retval));
				// We want to make sure that final_estimate contains at least 1 element, so that we can use faster low-level
				// methods in hash_set.
				if (unlikely(!final_estimate)) {
					final_estimate = 1u;
				}
				// Try to see if a series already has enough buckets.
				auto it = std::find_if(retval_list.begin(),retval_list.end(),[&final_estimate](const Series &r) {
					return static_cast<double>(r.m_container.bucket_count()) * r.m_container.max_load_factor() >= final_estimate;
				});
				if (it != retval_list.end()) {
					retval = std::move(*it);
					retval_list.erase(it);
				} else {
					// Otherwise, just rehash to the desired value, corrected for max load factor.
					retval.m_container.rehash(
						safe_cast<typename Series::size_type>(std::ceil(static_cast<double>(final_estimate) / retval.m_container.max_load_factor()))
					);
				}
				// Cleanup functor that will erase all elements in retval_list.
				auto cleanup = [&retval_list]() {
					std::for_each(retval_list.begin(),retval_list.end(),[](Series &r) {r.m_container.clear();});
				};
				try {
					final_merge(retval,retval_list,n_threads);
					// Here the estimate will be correct if the initial estimate (corrected for load factor)
					// is at least equal to the final real bucket count.
					trace_estimates(retval.m_container.size(),final_estimate);
				} catch (...) {
					// Do the cleanup before re-throwing, as we use mutable iterators in final_merge.
					cleanup();
					// Clear also retval: it could have bogus number of elements if errors arise in the final merge.
					retval.m_container.clear();
					throw;
				}
				// Clean up the temporary series.
				cleanup();
				return retval;
			}
		}
		struct atomic_flag_array
		{
			using value_type = std::atomic_flag;
			explicit atomic_flag_array(const std::size_t &size):m_ptr(nullptr),m_size(size)
			{
				if (unlikely(size > std::numeric_limits<std::size_t>::max() / sizeof(value_type))) {
					piranha_throw(std::bad_alloc,);
				}
				m_ptr = static_cast<value_type *>(aligned_palloc(0u,size * sizeof(value_type)));
				for (std::size_t i = 0u; i < m_size; ++i) {
					::new (static_cast<void *>(m_ptr + i)) value_type(ATOMIC_FLAG_INIT);
				}
			}
			~atomic_flag_array()
			{
				for (std::size_t i = 0u; i < m_size; ++i) {
					(m_ptr + i)->~value_type();
				}
				aligned_pfree(0u,static_cast<void *>(m_ptr));
			}
			value_type &operator[](const std::size_t &i)
			{
				return *(m_ptr + i);
			}
			const value_type &operator[](const std::size_t &i) const
			{
				return *(m_ptr + i);
			}
			value_type		*m_ptr;
			const std::size_t	m_size;
		};
		struct atomic_lock_guard
		{
			explicit atomic_lock_guard(std::atomic_flag &af):m_af(af)
			{
				while (m_af.test_and_set(std::memory_order_acquire)) {}
			}
			~atomic_lock_guard()
			{
				m_af.clear(std::memory_order_release);
			}
			std::atomic_flag &m_af;
		};
		template <typename Functor>
		Series execute2() const
		{
			// Do not do anything if one of the two series is empty.
			if (unlikely(m_s1->empty() || m_s2->empty())) {
				// NOTE: requirement is ok, a series must be def-ctible.
				return Series{};
			}
			// This is the size type that will be used throughout the calculations.
			using size_type = decltype(m_v1.size());
			const size_type size1 = m_v1.size(), size2 = m_v2.size();
			piranha_assert(size1 && size2);
			// Establish the number of threads to use.
			size_type n_threads = safe_cast<size_type>(thread_pool::use_threads(
				integer(size1) * size2,integer(settings::get_min_work_per_thread())
			));
			piranha_assert(n_threads);
			// An additional check on n_threads is that its size is not greater than the size of the first series,
			// as we are using the first operand to split up the work.
			if (n_threads > size1) {
				n_threads = size1;
			}
			if (likely(n_threads == 1u)) {
				Series retval;
				retval.m_symbol_set = m_s1->m_symbol_set;
				Functor f(&m_v1[0u],size1,&m_v2[0u],size2,retval);
				const auto tmp = rehasher(f);
				blocked_multiplication(f);
				if (tmp.first) {
					trace_estimates(retval.size(),tmp.second);
				}
				return retval;
			} else {
				// Init the return series.
				Series retval;
				retval.m_symbol_set = m_s1->m_symbol_set;
				// Estimate the final size and rehash accordingly.
				{
				Functor f(&m_v1[0u],size1,&m_v2[0u],size2,retval);
				// NOTE: this method will either clear up retval on exit, or throw. In any case we don't need
				// to care about retval containing elements.
				auto size = estimate_final_series_size(f);
				piranha_assert(retval.size() == 0u);
				retval.m_container.rehash(safe_cast<decltype(size)>(std::ceil(static_cast<double>(size) / retval.m_container.max_load_factor())));
				}
				// Init the vector of spinlocks.
				atomic_flag_array sl_array(safe_cast<std::size_t>(retval.m_container.bucket_count()));
				// Init the future list.
				future_list<std::future<void>> f_list;
				// Block size.
				const auto block_size = size1 / n_threads;
				// TODO doc.
				using bucket_size_type = decltype(retval.m_container.bucket_count());
				std::mutex ins_mutex;
				integer tot_ins(0);
				try {
					for (size_type i = 0u; i < n_threads; ++i) {
						// The thread functor.
						auto f = [i,this,block_size,n_threads,&sl_array,&retval,&ins_mutex,&tot_ins]() {
							// TODO overflow protection.
							bucket_size_type insertion_count = 0u;
							// Key type.
							using key_type = typename Series::term_type::key_type;
							// The type used to store the result of each term-by-term multiplication.
							constexpr std::size_t arity = key_type::multiply_arity;
							using mult_res_type = std::array<typename Series::term_type,arity>;
							mult_res_type tmp;
							// End of retval container (thread-safe).
							const auto c_end = retval.m_container.end();
							const auto e1 = (i == n_threads - 1u) ? (&(this->m_v1[0u]) + this->m_v1.size()) :
								(&(this->m_v1[0u]) + (i + 1u) * block_size);
							const auto e2 = &(this->m_v2[0u]) + this->m_v2.size();
							for (auto s1 = &(this->m_v1[0u]) + i * block_size; s1 != e1; ++s1) {
								for (auto s2 = &(this->m_v2[0u]); s2 != e2; ++s2) {
									// Perform the multiplication.
									key_type::multiply(tmp,**s1,**s2,retval.m_symbol_set);
									// Insertion.
									for (std::size_t j = 0u; j < arity; ++j) {
										auto &t = tmp[j];
										// Try to locate the term into retval.
										auto bucket_idx = retval.m_container._bucket(t);
										// Lock the bucket.
										atomic_lock_guard alg(sl_array[static_cast<std::size_t>(bucket_idx)]);
										const auto it = retval.m_container._find(t,bucket_idx);
										if (it == c_end) {
											retval.m_container._unique_insert(std::move(t),bucket_idx);
											++insertion_count;
										} else {
											it->m_cf += std::move(t.m_cf);
										}
									}
								}
							}
							// Final update of insertion count.
							std::lock_guard<std::mutex> lock(ins_mutex);
							tot_ins += insertion_count;
						};
						f_list.push_back(thread_pool::enqueue(static_cast<unsigned>(i),f));
					}
					f_list.wait_all();
					f_list.get_all();
				} catch (...) {
					f_list.wait_all();
					throw;
				}
				retval.m_container._update_size(static_cast<bucket_size_type>(tot_ins));
				return retval;
			}
		}
		/// Default multiplication functor.
		/**
		 * This multiplication functor uses the <tt>multiply()</tt> method of the key to compute the result of
		 * term-by-term multiplications, and employs the piranha::series::insert() method to accumulate the terms
		 * in the return value.
		 */
		class default_functor
		{
				// NOTE: this should not create problems if the key_type has no multiply_arity, as series_multiplier
				// is a template and thus its members are instantiated only as needed. In the series_has_multiplier checks,
				// we never need to access this typedef.
				using mult_res_type = std::array<typename Series::term_type,Series::term_type::key_type::multiply_arity>;
			public:
				/// Alias for the term type of \p Series.
				using term_type = typename Series::term_type;
				/// Alias for the size type.
				/**
				 * This unsigned type will be used to represent sizes throughout the calculations.
				 */
				using size_type = typename std::vector<term_type const *>::size_type;
				/// Constructor.
				/**
				 * The functor is constructed from arrays of pointers to the input series terms on which the functor
				 * will operate and the return value into which the results of term-by-term
				 * multiplications will be accumulated. The input parameters (or references to them) are stored as public
				 * class members for later use.
				 * 
				 * @param[in] ptr1 start of the first array of pointers.
				 * @param[in] s1 size of the first array of pointers.
				 * @param[in] ptr2 start of the second array of pointers.
				 * @param[in] s2 size of the second array of pointers.
				 * @param[in] retval series into which the result of the multiplication will be accumulated.
				 */
				explicit default_functor(term_type const **ptr1, const size_type &s1,
					term_type const **ptr2, const size_type &s2, Series &retval):
					m_ptr1(ptr1),m_s1(s1),m_ptr2(ptr2),m_s2(s2),m_retval(retval)
				{}
				/// Term multiplication.
				/**
				 * This function call operator will multiply the i-th term in the first array of pointers by the j-th term
				 * in the second array of pointers, and store the result in the \p m_tmp member variable.
				 * 
				 * @param[in] i index of the first term operand.
				 * @param[in] j index of the second term operand.
				 * 
				 * @throws unspecified any exception thrown by the <tt>multiply()</tt> method of the key type of the series.
				 */
				void operator()(const size_type &i, const size_type &j) const
				{
					using key_type = typename Series::term_type::key_type;
					piranha_assert(i < m_s1 && j < m_s2);
					key_type::multiply(m_tmp,*(m_ptr1[i]),*(m_ptr2[j]),m_retval.m_symbol_set);
				}
				/// Term insertion.
				/**
				 * This method will insert into the return value \p m_retval the term(s) stored in \p m_tmp.
				 * 
				 * @throws unspecified any exception thrown by series::insert().
				 */
				void insert() const
				{
					for (std::size_t i = 0u; i < Series::term_type::key_type::multiply_arity; ++i) {
						m_retval.insert(m_tmp[i]);
					}
				}
				/// Pointer to the first array of pointers.
				term_type const **	m_ptr1;
				/// Size of the first array of pointers.
				const size_type		m_s1;
				/// Pointer to the second array of pointers.
				term_type const **	m_ptr2;
				/// Size of the second array of pointers.
				const size_type		m_s2;
				/// Reference to the return series object used during construction.
				Series			&m_retval;
				/// Object holding the result of term-by-term multiplications.
				/**
				 * This object is an \p std::array of \p term_type.
				 */
				mutable mult_res_type	m_tmp;
		};
		/// Block-by-block multiplication.
		/**
		 * This method expects a \p Functor type exposing the same inteface as default_functor. Functionally, the method
		 * is equivalent to repeated calls of the methods of \p Functor that will multiply term-by-term the terms
		 * of the input series and accumulate the result in the output series.
		 *
		 * The method will perform the multiplications after logically subdividing the input series in blocks, in order to
		 * optimize cache memory access patterns.
		 * 
		 * @param[in] f multiplication functor.
		 * 
		 * @throws unspecified any exception thrown by the public interface of \p Functor or by piranha::safe_cast().
		 */
		template <typename Functor>
		static void blocked_multiplication(const Functor &f)
		{
			typedef typename std::decay<decltype(f.m_s1)>::type size_type;
			const size_type size1 = f.m_s1, size2 = f.m_s2, bsize1 = safe_cast<size_type>(tuning::get_multiplication_block_size()),
				nblocks1 = size1 / bsize1, bsize2 = bsize1, nblocks2 = size2 / bsize2;
			// Start and end of last (possibly irregular) blocks.
			const size_type i_ir_start = nblocks1 * bsize1, i_ir_end = size1;
			const size_type j_ir_start = nblocks2 * bsize2, j_ir_end = size2;
			for (size_type n1 = 0u; n1 < nblocks1; ++n1) {
				const size_type i_start = n1 * bsize1, i_end = i_start + bsize1;
				// regulars1 * regulars2
				for (size_type n2 = 0u; n2 < nblocks2; ++n2) {
					const size_type j_start = n2 * bsize2, j_end = j_start + bsize2;
					for (size_type i = i_start; i < i_end; ++i) {
						for (size_type j = j_start; j < j_end; ++j) {
							f(i,j);
							f.insert();
						}
					}
				}
				// regulars1 * rem2
				for (size_type i = i_start; i < i_end; ++i) {
					for (size_type j = j_ir_start; j < j_ir_end; ++j) {
						f(i,j);
						f.insert();
					}
				}
			}
			// rem1 * regulars2
			for (size_type n2 = 0u; n2 < nblocks2; ++n2) {
				const size_type j_start = n2 * bsize2, j_end = j_start + bsize2;
				for (size_type i = i_ir_start; i < i_ir_end; ++i) {
					for (size_type j = j_start; j < j_end; ++j) {
						f(i,j);
						f.insert();
					}
				}
			}
			// rem1 * rem2.
			for (size_type i = i_ir_start; i < i_ir_end; ++i) {
				for (size_type j = j_ir_start; j < j_ir_end; ++j) {
					f(i,j);
					f.insert();
				}
			}
		}
		/// Estimate size of series multiplication.
		/**
		 * This method expects a \p Functor type exposing the same inteface as default_functor. The method
		 * will employ a statistical approach to estimate the size of the output of the series multiplication represented
		 * by \p f (without actually going through the whole calculation).
		 * 
		 * If either input series has a null size, zero will be returned.
		 * 
		 * @param[in] f multiplication functor.
		 * 
		 * @return the estimated size of the series multiplication represented by \p f.
		 * 
		 * @throws unspecified any exception thrown by:
		 * - memory allocation errors in standard containers,
		 * - overflow errors while converting between integer types,
		 * - the public interface of \p Functor.
		 */
		template <typename Functor>
		static typename Series::size_type estimate_final_series_size(const Functor &f)
		{
			typedef typename Series::size_type bucket_size_type;
			typedef typename std::decay<decltype(f.m_s1)>::type size_type;
			const size_type size1 = f.m_s1, size2 = f.m_s2;
			// If one of the two series is empty, just return 0.
			if (unlikely(!size1 || !size2)) {
				return 0u;
			}
			// Cache reference to return series.
			auto &retval = f.m_retval;
			// NOTE: Hard-coded number of trials = 10.
			// NOTE: here consider that in case of extremely sparse series with few terms (e.g., next to the lower limit
			// for which this function is called) this will incur in noticeable overhead, since we will need many term-by-term
			// before encountering the first duplicate.
			const auto ntrials = 10u;
			// NOTE: Hard-coded value for the estimation multiplier.
			// NOTE: This value should be tuned for performance/memory usage tradeoffs.
			const auto multiplier = 2;
			// Size of the multiplication result
			// Vectors of indices.
			std::vector<size_type> v_idx1, v_idx2;
			for (size_type i = 0u; i < size1; ++i) {
				v_idx1.push_back(i);
			}
			for (size_type i = 0u; i < size2; ++i) {
				v_idx2.push_back(i);
			}
			// Maxium number of random multiplications before which a duplicate term must be generated.
			const size_type max_M = static_cast<size_type>(((integer(size1) * size2) / multiplier).sqrt());
			// Random number engine.
			std::mt19937 engine;
			// Init counter.
			integer total(0);
			// Go with the trials.
			// NOTE: This could be easily parallelised, but not sure if it is worth.
			for (auto n = 0u; n < ntrials; ++n) {
				// Randomise.
				std::shuffle(v_idx1.begin(),v_idx1.end(),engine);
				std::shuffle(v_idx2.begin(),v_idx2.end(),engine);
				size_type count = 0u;
				auto it1 = v_idx1.begin(), it2 = v_idx2.begin();
				while (count < max_M) {
					if (it1 == v_idx1.end()) {
						// Each time we wrap around the first series,
						// wrap around also the second one and rotate it.
						it1 = v_idx1.begin();
						auto middle = v_idx2.end();
						--middle;
						std::rotate(v_idx2.begin(),middle,v_idx2.end());
						it2 = v_idx2.begin();
					}
					if (it2 == v_idx2.end()) {
						it2 = v_idx2.begin();
					}
					// Perform multiplication and check if it produces a new term.
					f(*it1,*it2);
					f.insert();
					constexpr std::size_t result_size = Series::term_type::key_type::multiply_arity;
					// Check for unlikely overflow when increasing count.
					if (unlikely(result_size > std::numeric_limits<size_type>::max() ||
						count > std::numeric_limits<size_type>::max() - result_size))
					{
						piranha_throw(std::overflow_error,"overflow error");
					}
					if (retval.size() != count + result_size) {
						break;
					}
					// Increase cycle variables.
					count += result_size;
					++it1;
					++it2;
				}
				total += count;
				// Reset retval.
				retval.m_container.clear();
			}
			const auto mean = total / ntrials;
			// Avoid division by zero.
			if (total.sign()) {
				const integer M = (mean * mean * multiplier * total) / total;
				return static_cast<bucket_size_type>(M);
			} else {
				return static_cast<bucket_size_type>(mean * mean * multiplier);
			}
		}
		/// Trace series size estimates.
		/**
		 * Record in the piranha::tracing framework the outcome of result size estimation via estimate_final_series_size().
		 * 
		 * The string descriptors and associated data are:
		 * 
		 * - <tt>number_of_estimates</tt>, <tt>unsigned long long</tt>, corresponding to the total number of times
		 *   this function has been called;
		 * - <tt>number_of_correct_estimates</tt>, <tt>unsigned long long</tt>, corresponding to the total number of times
		 *   this function has been called with <tt>estimate >= real_size</tt>;
		 * - <tt>accumulated_estimate_ratio</tt>, <tt>double</tt>, corresponding to the accumulated value of <tt>estimate / real_size</tt>.
		 * 
		 * In order to avoid potential divisions by zero, if \p real_size is zero no tracing will be performed.
		 * 
		 * @param[in] real_size the real size of the output series.
		 * @param[in] estimate the size of the output series as estimated by estimate_final_series_size().
		 * 
		 * @throws unspecified any exception thrown by tracing::trace().
		 */
		static void trace_estimates(const typename Series::size_type &real_size, const typename Series::size_type &estimate)
		{
			if (unlikely(!real_size)) {
				return;
			}
			tracing::trace("number_of_estimates",[](boost::any &x) {
				if (unlikely(x.empty())) {
					x = 0ull;
				}
				auto ptr = boost::any_cast<unsigned long long>(&x);
				if (likely((bool)ptr)) {
					++*ptr;
				}
			});
			tracing::trace("number_of_correct_estimates",[real_size,estimate](boost::any &x) {
				if (unlikely(x.empty())) {
					x = 0ull;
				}
				auto ptr = boost::any_cast<unsigned long long>(&x);
				if (likely((bool)ptr)) {
					*ptr += static_cast<unsigned long long>(estimate >= real_size);
				}
			});
			tracing::trace("accumulated_estimate_ratio",[real_size,estimate](boost::any &x) {
				if (unlikely(x.empty())) {
					x = 0.;
				}
				auto ptr = boost::any_cast<double>(&x);
				if (likely((bool)ptr && estimate)) {
					*ptr += static_cast<double>(estimate) / static_cast<double>(real_size);
				}
			});
		}
	private:
		template <typename RetvalList, typename Size>
		static void final_merge(Series &retval, RetvalList &retval_list, const Size &n_threads)
		{
			piranha_assert(n_threads > 1u);
			piranha_assert(retval.m_container.bucket_count());
			typedef typename std::vector<std::pair<typename Series::size_type,decltype(retval.m_container._m_begin())>> container_type;
			typedef typename container_type::size_type size_type;
			// First, let's fill a vector assigning each term of each element in retval_list to a bucket in retval.
			const size_type size = static_cast<size_type>(
				std::accumulate(retval_list.begin(),retval_list.end(),integer(0),[](const integer &n, const Series &r) {return n + r.size();}));
			container_type idx(size);
			future_list<std::future<void>> f_list1;
			size_type i = 0u;
			piranha_assert(retval_list.size() <= n_threads);
			try {
				unsigned thread_idx = 0u;
				for (auto r_it = retval_list.begin(); r_it != retval_list.end(); ++r_it, ++thread_idx) {
					auto f = [&idx,&retval,i,r_it]() {
						const auto it_f = r_it->m_container._m_end();
						// NOTE: size_type can represent the sum of the sizes of all retvals,
						// so it will not overflow here.
						size_type tmp_i = i;
						for (auto it = r_it->m_container._m_begin(); it != it_f; ++it, ++tmp_i) {
							piranha_assert(tmp_i < idx.size());
							idx[tmp_i] = std::make_pair(retval.m_container._bucket(*it),it);
						}
					};
					f_list1.push_back(thread_pool::enqueue(thread_idx,f));
					i += r_it->size();
				}
				f_list1.wait_all();
				f_list1.get_all();
			} catch (...) {
				f_list1.wait_all();
				throw;
			}
			future_list<std::future<void>> f_list2;
			std::mutex m;
			std::vector<integer> new_sizes;
			new_sizes.reserve(static_cast<std::vector<integer>::size_type>(n_threads));
			if (unlikely(new_sizes.capacity() != n_threads)) {
				piranha_throw(std::bad_alloc,);
			}
			const typename Series::size_type bucket_count = retval.m_container.bucket_count(), block_size = bucket_count / n_threads;
			try {
				for (Size n = 0u; n < n_threads; ++n) {
					// These are the bucket indices allowed to the current thread.
					const typename Series::size_type start = n * block_size, end = (n == n_threads - 1u) ? bucket_count : (n + 1u) * block_size;
					auto f = [start,end,&size,&retval,&idx,&m,&new_sizes]() {
						size_type count_plus = 0u, count_minus = 0u;
						for (size_type i = 0u; i < size; ++i) {
							const auto &bucket_idx = idx[i].first;
							auto &term_it = idx[i].second;
							if (bucket_idx >= start && bucket_idx < end) {
								// Reconstruct part of series insertion, without the update in the number of elements.
								// NOTE: compatibility and ignorability do not matter, as terms being inserted come from series
								// anyway. Same for check on bucket_count.
								const auto it = retval.m_container._find(*term_it,bucket_idx);
								if (it == retval.m_container.end()) {
									// Term is new.
									retval.m_container._unique_insert(std::move(*term_it),bucket_idx);
									// Update term count.
									piranha_assert(count_plus < std::numeric_limits<size_type>::max());
									++count_plus;
								} else {
									// Assert the existing term is not ignorable and it is compatible.
									piranha_assert(!it->is_ignorable(retval.m_symbol_set) && it->is_compatible(retval.m_symbol_set));
									// Cleanup function.
									auto cleanup = [&]() {
										if (unlikely(it->is_ignorable(retval.m_symbol_set))) {
											retval.m_container._erase(it);
											// After term is erased, update count.
											piranha_assert(count_minus < std::numeric_limits<size_type>::max());
											++count_minus;
										}
									};
									try {
										// The term exists already, update it.
										retval.template insertion_cf_arithmetics<true>(it,std::move(*term_it));
										// Check if the term has become ignorable after the modification.
										cleanup();
									} catch (...) {
										cleanup();
										throw;
									}
								}
							}
						}
						// Store the new size.
						std::lock_guard<std::mutex> lock(m);
						new_sizes.push_back(integer(count_plus) - integer(count_minus));
					};
					// NOTE: here the static cast is safe as nthreads is coming from a call to
					// the size of the thread pool.
					f_list2.push_back(thread_pool::enqueue(static_cast<unsigned>(n),f));
				}
				f_list2.wait_all();
				f_list2.get_all();
			} catch (...) {
				f_list2.wait_all();
				throw;
			}
			// Final update of retval's size.
			const auto new_size = std::accumulate(new_sizes.begin(),new_sizes.end(),integer(0));
			piranha_assert(new_size + retval.m_container.size() >= 0);
			retval.m_container._update_size(static_cast<decltype(retval.m_container.size())>(new_size + retval.m_container.size()));
			// Take care of rehashing, if needed.
			if (unlikely(retval.m_container.load_factor() > retval.m_container.max_load_factor())) {
				retval.m_container.rehash(
					safe_cast<typename Series::size_type>(std::ceil(static_cast<double>(retval.size()) / retval.m_container.max_load_factor()))
				);
			}
		}
		// Functor tasked to prepare return value(s) with estimated bucket sizes (if
		// it is worth to perform such analysis).
		template <typename Functor>
		static std::pair<bool,typename Series::size_type> rehasher(const Functor &f)
		{
			const auto s1 = f.m_s1, s2 = f.m_s2;
			auto &r = f.m_retval;
			// NOTE: hard-coded value of 100000 for minimm number of terms multiplications.
			if (s2 && s1 >= 100000u / s2) {
				// NOTE: here we could have (very unlikely) some overflow or memory error in the computation
				// of the estimate or in rehashing. In such a case, just ignore the rehashing, clean
				// up retval just to be sure, and proceed.
				try {
					auto size = estimate_final_series_size(f);
					r.m_container.rehash(safe_cast<decltype(size)>(std::ceil(static_cast<double>(size) / r.m_container.max_load_factor())));
					return std::make_pair(true,size);
				} catch (...) {
					r.m_container.clear();
					return std::make_pair(false,typename Series::size_type(0u));
				}
			}
			return std::make_pair(false,typename Series::size_type(0u));
		}
	protected:
		/// Const pointer to the first series operand.
		Series const						*m_s1;
		/// Const pointer to the second series operand.
		Series const						*m_s2;
		/// Vector of const pointers to the terms in the first series.
		mutable std::vector<typename Series::term_type const *>	m_v1;
		/// Vector of const pointers to the terms in the second series.
		mutable std::vector<typename Series::term_type const *>	m_v2;
};

}

#endif
