/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the specific language
 * governing permissions and limitations under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */


 #pragma once

 #include <seastar/core/shared_ptr_debug_helper.hh>
 #include <seastar/util/is_smart_ptr.hh>
 #include <seastar/util/indirect.hh>
 #include <seastar/util/modules.hh>
  #include <seastar/util/backtrace.hh>
 #include <ostream>
 #ifndef SEASTAR_MODULE
 #include <boost/intrusive/parent_from_member.hpp>
 #include <fmt/core.h>
 #include <type_traits>
 #include <utility>
 #endif

 #if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ >= 12)
 // to silence the false alarm from GCC 12, see
 // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105204
 #define SEASTAR_IGNORE_USE_AFTER_FREE
 #endif

 namespace seastar::dbg {

 // This header defines two shared pointer facilities, lw_dbg_shared_ptr<> and
 // dbg_shared_ptr<>, both modeled after std::dbg_shared_ptr<>.
 //
 // Unlike std::dbg_shared_ptr<>, neither of these implementations are thread
 // safe, and two pointers sharing the same object must not be used in
 // different threads.
 //
 // lw_dbg_shared_ptr<> is the more lightweight variant, with a lw_dbg_shared_ptr<>
 // occupying just one machine word, and adding just one word to the shared
 // object.  However, it does not support polymorphism.
 //
 // dbg_shared_ptr<> is more expensive, with a pointer occupying two machine
 // words, and with two words of overhead in the shared object.  In return,
 // it does support polymorphism.
 //
 // Both variants support shared_from_this() via enable_shared_from_this<>
 // and lw_enable_shared_from_this<>().
 //

 SEASTAR_MODULE_EXPORT_BEGIN

//  #ifndef SEASTAR_DEBUG_SHARED_PTR
//  using dbg_shared_ptr_counter_type = long;
//  #else
//  using dbg_shared_ptr_counter_type = debug_shared_ptr_counter_type;
//  #endif

 using dbg_shared_ptr_counter_type = long;

 template <typename T>
 class dbg_shared_ptr;

 template <typename T>
 class enable_shared_from_this;


 template <typename T, typename... A>
 dbg_shared_ptr<T> make_shared(A&&... a);

 template <typename T>
 dbg_shared_ptr<T> make_shared(T&& a);

 template <typename T, typename U>
 dbg_shared_ptr<T> static_pointer_cast(const dbg_shared_ptr<U>& p);

 template <typename T, typename U>
 dbg_shared_ptr<T> dynamic_pointer_cast(const dbg_shared_ptr<U>& p);

 template <typename T, typename U>
 dbg_shared_ptr<T> const_pointer_cast(const dbg_shared_ptr<U>& p);

 struct lw_dbg_shared_ptr_counter_base {
     dbg_shared_ptr_counter_type _count = 0;
 };

 SEASTAR_MODULE_EXPORT_END

 SEASTAR_MODULE_EXPORT

 // Polymorphic shared pointer class

 struct dbg_shared_ptr_count_base {
     // destructor is responsible for fully-typed deletion
     virtual ~dbg_shared_ptr_count_base() {}
     dbg_shared_ptr_counter_type count = 0;
 };

 template <typename T>
 struct dbg_shared_ptr_count_for : dbg_shared_ptr_count_base {
     T data;
     template <typename... A>
     dbg_shared_ptr_count_for(A&&... a) : data(std::forward<A>(a)...) {}
 };

 SEASTAR_MODULE_EXPORT_BEGIN
 template <typename T>
 class enable_shared_from_this : private dbg_shared_ptr_count_base {
 public:
     dbg_shared_ptr<T> shared_from_this() noexcept;
     dbg_shared_ptr<const T> shared_from_this() const noexcept;
     long use_count() const noexcept { return count; }

     template <typename U>
     friend class dbg_shared_ptr;

     template <typename U, bool esft>
     friend struct dbg_shared_ptr_make_helper;
 };

 template <typename T>
 class dbg_shared_ptr {
     mutable dbg_shared_ptr_count_base* _b = nullptr;
     mutable T* _p = nullptr;

     void log_operation(const std::string_view operation, bool print_back_trace = false) const {
         std::cout << this << " " << operation
                   << " - Counter: " << (int)use_count()
                   << ", Counter Address: " << _b;
        // if (print_back_trace) {
        //     std::cout  << ", backtrace: " << current_tasktrace();
        // }
        std::cout << std::endl;
     }

 private:
     explicit dbg_shared_ptr(dbg_shared_ptr_count_for<T>* b) noexcept : _b(b), _p(&b->data) {
         ++_b->count;
         log_operation("Constructor(dbg_shared_ptr_count_for<T>)", true);
     }

     dbg_shared_ptr(dbg_shared_ptr_count_base* b, T* p) noexcept : _b(b), _p(p) {
         if (_b) {
             ++_b->count;
         }
         log_operation("Constructor(dbg_shared_ptr_count_base*, T*)", true);
     }

     explicit dbg_shared_ptr(enable_shared_from_this<std::remove_const_t<T>>* p) noexcept : _b(p), _p(static_cast<T*>(p)) {
         if (_b) {
             ++_b->count;
         }
         log_operation("Constructor(enable_shared_from_this<T>*)", true);
     }

 public:
     dbg_shared_ptr() noexcept {
         log_operation("Default Constructor");
     }

     dbg_shared_ptr(std::nullptr_t) noexcept : dbg_shared_ptr() {
         log_operation("Constructor(nullptr_t)");
     }

     dbg_shared_ptr(const dbg_shared_ptr& x) noexcept
             : _b(x._b)
             , _p(x._p) {
         if (_b) {
             ++_b->count;
         }
         log_operation("Copy Constructor", true);
     }

     dbg_shared_ptr(dbg_shared_ptr&& x) noexcept
             : _b(x._b)
             , _p(x._p) {
         x._b = nullptr;
         x._p = nullptr;
         log_operation("Move Constructor", true);
     }

     template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
     dbg_shared_ptr(const dbg_shared_ptr<U>& x) noexcept
             : _b(x._b)
             , _p(x._p) {
         if (_b) {
             ++_b->count;
         }
         log_operation("Copy Constructor from other type", true);
     }
     template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
     dbg_shared_ptr(dbg_shared_ptr<U>&& x) noexcept
             : _b(x._b)
             , _p(x._p) {
         x._b = nullptr;
         x._p = nullptr;
         log_operation("Move Constructor from other type", true);
     }


     ~dbg_shared_ptr() {
        log_operation("Destructor");
         if (_b && !--_b->count) {
             delete _b;
         }
     }
     dbg_shared_ptr& operator=(const dbg_shared_ptr& x) noexcept {
         if (this != &x) {
             this->~dbg_shared_ptr();
             new (this) dbg_shared_ptr(x);
         }
         log_operation("Copy Assignment Operator");
         return *this;
     }

     dbg_shared_ptr& operator=(dbg_shared_ptr&& x) noexcept {
         if (this != &x) {
             this->~dbg_shared_ptr();
             new (this) dbg_shared_ptr(std::move(x));
         }
         log_operation("Move Assignment Operator");
         return *this;
     }

     dbg_shared_ptr& operator=(std::nullptr_t) noexcept {
         return *this = dbg_shared_ptr();
     }

     template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
     dbg_shared_ptr& operator=(const dbg_shared_ptr<U>& x) noexcept {
         if (*this != x) {
             this->~dbg_shared_ptr();
             new (this) dbg_shared_ptr(x);
         }
         log_operation("Copy Assignment Operator (template)");
         return *this;
     }

     template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
     dbg_shared_ptr& operator=(dbg_shared_ptr<U>&& x) noexcept {
         if (*this != x) {
             this->~dbg_shared_ptr();
             new (this) dbg_shared_ptr(std::move(x));
         }
         log_operation("Move Assignment Operator (template)");
         return *this;
     }

     explicit operator bool() const noexcept {
         return _p;
     }
     T& operator*() const noexcept {
         return *_p;
     }
     T* operator->() const noexcept {
         return _p;
     }
     T* get() const noexcept {
         return _p;
     }
     long use_count() const noexcept {
         if (_b) {
             return _b->count;
         } else {
             return 0;
         }
     }

     template <bool esft>
     struct make_helper;

     template <typename U, typename... A>
     friend dbg_shared_ptr<U> make_shared(A&&... a);

     template <typename U>
     friend dbg_shared_ptr<U> make_shared(U&& a);

     template <typename V, typename U>
     friend dbg_shared_ptr<V> static_pointer_cast(const dbg_shared_ptr<U>& p);

     template <typename V, typename U>
     friend dbg_shared_ptr<V> dynamic_pointer_cast(const dbg_shared_ptr<U>& p);

     template <typename V, typename U>
     friend dbg_shared_ptr<V> const_pointer_cast(const dbg_shared_ptr<U>& p);

     template <bool esft, typename... A>
     static dbg_shared_ptr make(A&&... a);

     template <typename U>
     friend class enable_shared_from_this;

     template <typename U, bool esft>
     friend struct dbg_shared_ptr_make_helper;

     template <typename U>
     friend class dbg_shared_ptr;
 };
 SEASTAR_MODULE_EXPORT_END

 template <typename U, bool esft>
 struct dbg_shared_ptr_make_helper;

 template <typename T>
 struct dbg_shared_ptr_make_helper<T, false> {
     template <typename... A>
     static dbg_shared_ptr<T> make(A&&... a) {
         return dbg_shared_ptr<T>(new dbg_shared_ptr_count_for<T>(std::forward<A>(a)...));
     }
 };

 template <typename T>
 struct dbg_shared_ptr_make_helper<T, true> {
     template <typename... A>
     static dbg_shared_ptr<T> make(A&&... a) {
         auto p = new T(std::forward<A>(a)...);
         return dbg_shared_ptr<T>(p, p);
     }
 };

 SEASTAR_MODULE_EXPORT_BEGIN
 template <typename T, typename... A>
 inline
 dbg_shared_ptr<T>
 make_shared(A&&... a) {
     using helper = dbg_shared_ptr_make_helper<T, std::is_base_of_v<dbg_shared_ptr_count_base, T>>;
     return helper::make(std::forward<A>(a)...);
 }

 template <typename T>
 inline
 dbg_shared_ptr<T>
 make_shared(T&& a) {
     using helper = dbg_shared_ptr_make_helper<T, std::is_base_of_v<dbg_shared_ptr_count_base, T>>;
     return helper::make(std::forward<T>(a));
 }

 template <typename T, typename U>
 inline
 dbg_shared_ptr<T>
 static_pointer_cast(const dbg_shared_ptr<U>& p) {
     return dbg_shared_ptr<T>(p._b, static_cast<T*>(p._p));
 }

 template <typename T, typename U>
 inline
 dbg_shared_ptr<T>
 dynamic_pointer_cast(const dbg_shared_ptr<U>& p) {
     auto q = dynamic_cast<T*>(p._p);
     return dbg_shared_ptr<T>(q ? p._b : nullptr, q);
 }

 template <typename T, typename U>
 inline
 dbg_shared_ptr<T>
 const_pointer_cast(const dbg_shared_ptr<U>& p) {
     return dbg_shared_ptr<T>(p._b, const_cast<T*>(p._p));
 }
 SEASTAR_MODULE_EXPORT_END

 template <typename T>
 inline
 dbg_shared_ptr<T>
 enable_shared_from_this<T>::shared_from_this() noexcept {
     auto unconst = reinterpret_cast<enable_shared_from_this<std::remove_const_t<T>>*>(this);
     return dbg_shared_ptr<T>(unconst);
 }

 template <typename T>
 inline
 dbg_shared_ptr<const T>
 enable_shared_from_this<T>::shared_from_this() const noexcept {
     auto esft = const_cast<enable_shared_from_this*>(this);
     auto unconst = reinterpret_cast<enable_shared_from_this<std::remove_const_t<T>>*>(esft);
     return dbg_shared_ptr<const T>(unconst);
 }

 SEASTAR_MODULE_EXPORT_BEGIN
 template <typename T, typename U>
 inline
 bool
 operator==(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() == y.get();
 }

 template <typename T>
 inline
 bool
 operator==(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() == nullptr;
 }

 template <typename T>
 inline
 bool
 operator==(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr == y.get();
 }

 template <typename T, typename U>
 inline
 bool
 operator!=(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() != y.get();
 }

 template <typename T>
 inline
 bool
 operator!=(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() != nullptr;
 }

 template <typename T>
 inline
 bool
 operator!=(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr != y.get();
 }


 template <typename T, typename U>
 inline
 bool
 operator<(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() < y.get();
 }

 template <typename T>
 inline
 bool
 operator<(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() < nullptr;
 }

 template <typename T>
 inline
 bool
 operator<(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr < y.get();
 }

 template <typename T, typename U>
 inline
 bool
 operator<=(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() <= y.get();
 }

 template <typename T>
 inline
 bool
 operator<=(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() <= nullptr;
 }

 template <typename T>
 inline
 bool
 operator<=(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr <= y.get();
 }

 template <typename T, typename U>
 inline
 bool
 operator>(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() > y.get();
 }

 template <typename T>
 inline
 bool
 operator>(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() > nullptr;
 }

 template <typename T>
 inline
 bool
 operator>(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr > y.get();
 }

 template <typename T, typename U>
 inline
 bool
 operator>=(const dbg_shared_ptr<T>& x, const dbg_shared_ptr<U>& y) {
     return x.get() >= y.get();
 }

 template <typename T>
 inline
 bool
 operator>=(const dbg_shared_ptr<T>& x, std::nullptr_t) {
     return x.get() >= nullptr;
 }

 template <typename T>
 inline
 bool
 operator>=(std::nullptr_t, const dbg_shared_ptr<T>& y) {
     return nullptr >= y.get();
 }

 template <typename T>
 inline
 std::ostream& operator<<(std::ostream& out, const dbg_shared_ptr<T>& p) {
     if (!p) {
         return out << "null";
     }
     return out << *p;
 }

 template<typename T>
 using dbg_shared_ptr_equal_by_value = indirect_equal_to<dbg_shared_ptr<T>>;

 template<typename T>
 using dbg_shared_ptr_value_hash = indirect_hash<dbg_shared_ptr<T>>;

 SEASTAR_MODULE_EXPORT_END
}



SEASTAR_MODULE_EXPORT
namespace fmt {

template<typename T>
const void* ptr(const seastar::dbg::dbg_shared_ptr<T>& p) {
    return p.get();
}

template <typename T>
struct formatter<seastar::dbg::dbg_shared_ptr<T>> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::dbg::dbg_shared_ptr<T>& p, fmt::format_context& ctx) const {
        if (!p) {
            return fmt::format_to(ctx.out(), "null");
        }
        return fmt::format_to(ctx.out(), "{}", *p);
    }
};

}

namespace seastar {

SEASTAR_MODULE_EXPORT
template<typename T>
struct is_smart_ptr<dbg::dbg_shared_ptr<T>> : std::true_type {};

}

