#pragma once

#include <type_traits>
#include <functional>
#include <tuple>
#include <experimental/tuple>
#include <vrm/core/utility_macros.hpp>
#include <vrm/core/type_traits.hpp>

namespace orizzonte::impl
{
    using vrm::core::forward_like;
    using vrm::core::copy_if_rvalue;

    namespace detail
    {
        template <typename T>
        class fwd_capture_base
        {
            static_assert(!std::is_rvalue_reference<T>{});
            static_assert(!std::is_lvalue_reference<T>{});

        protected:
            T _x;

            constexpr fwd_capture_base(const T& x) noexcept(
                std::is_nothrow_copy_constructible<T>{})
                : _x{x}
            {
            }

            constexpr fwd_capture_base(T&& x) noexcept(
                std::is_nothrow_move_constructible<T>{})
                : _x{std::move(x)}
            {
            }

        public:
            constexpr fwd_capture_base(fwd_capture_base&& rhs) noexcept(
                std::is_nothrow_move_constructible<T>{})
                : _x{std::move(rhs).get()}
            {
            }

            constexpr fwd_capture_base&
            operator=(fwd_capture_base&& rhs) noexcept(
                std::is_nothrow_move_assignable<T>{})
            {
                _x = std::move(rhs).get();
                return *this;
            }

            constexpr fwd_capture_base(const fwd_capture_base& rhs)
                // noexcept(std::is_nothrow_copy_constructible<T>{})
                : _x{rhs.get()}
            {
            }

            constexpr fwd_capture_base& operator=(const fwd_capture_base& rhs)
            // noexcept(std::is_nothrow_copy_assignable<T>{})
            {
                _x = rhs.get();
                return *this;
            }

            constexpr auto& get() & noexcept
            {
                return _x;
            }

            constexpr const auto& get() const & noexcept
            {
                return _x;
            }

            constexpr auto get() &&
                noexcept(std::is_nothrow_move_constructible<T>{})
            {
                return std::move(_x);
            }
        };

        template <typename T>
        class fwd_capture_ref_base
        {
            static_assert(!std::is_rvalue_reference<T>{});

        private:
            std::reference_wrapper<T> _x;

        public:
            constexpr fwd_capture_ref_base(T& x) noexcept : _x{x}
            {
            }

            constexpr fwd_capture_ref_base(fwd_capture_ref_base&& rhs) noexcept
                : _x{rhs._x}
            {
            }

            constexpr fwd_capture_ref_base& operator=(
                fwd_capture_ref_base&& rhs) noexcept
            {
                _x = rhs._x;
                return *this;
            }

            // Prevent copies.
            fwd_capture_ref_base(const fwd_capture_ref_base&) = delete;
            fwd_capture_ref_base& operator=(
                const fwd_capture_ref_base&) = delete;

            constexpr auto& get() & noexcept
            {
                return _x.get();
            }

            constexpr const auto& get() const & noexcept
            {
                return _x.get();
            }
        };
    }

    template <typename T>
    class fwd_capture_wrapper : public detail::fwd_capture_base<T>
    {
    private:
        using base_type = detail::fwd_capture_base<T>;

    public:
        constexpr fwd_capture_wrapper(const T& x) noexcept(
            std::is_nothrow_copy_constructible<T>{})
            : base_type{x}
        {
        }

        constexpr fwd_capture_wrapper(T&& x) noexcept(
            std::is_nothrow_move_constructible<T>{})
            : base_type{std::move(x)}
        {
        }
    };

    template <typename T>
    class fwd_capture_wrapper<T&> : public detail::fwd_capture_ref_base<T>
    {
    private:
        using base_type = detail::fwd_capture_ref_base<T>;

    public:
        using base_type::base_type;
    };

    template <typename T>
    class fwd_copy_capture_wrapper : public detail::fwd_capture_base<T>
    {
    private:
        using base_type = detail::fwd_capture_base<T>;

    public:
        constexpr fwd_copy_capture_wrapper(const T& x) noexcept(
            std::is_nothrow_copy_constructible<T>{})
            : base_type{x}
        {
        }
    };

    template <typename T>
    class fwd_copy_capture_wrapper<T&> : public detail::fwd_capture_ref_base<T>
    {
    private:
        using base_type = detail::fwd_capture_ref_base<T>;

    public:
        using base_type::base_type;
    };

    template <typename T>
    auto fwd_capture(T&& x)
    {
        return fwd_capture_wrapper<T>(FWD(x));
    }

    template <typename T>
    auto fwd_copy_capture(T&& x)
    {
        return fwd_copy_capture_wrapper<T>(FWD(x));
    }
}

#define FWD_CAPTURE(...) orizzonte::impl::fwd_capture(FWD(__VA_ARGS__))

#define FWD_COPY_CAPTURE(...) \
    orizzonte::impl::fwd_copy_capture(FWD(__VA_ARGS__))

namespace orizzonte::impl
{
    template <typename... Ts>
    auto fwd_capture_as_tuple(Ts&&... xs)
    {
        return std::forward_as_tuple(FWD_CAPTURE(xs)...);
    }

    template <typename... Ts>
    auto fwd_copy_capture_as_tuple(Ts&&... xs)
    {
        return std::make_tuple(FWD_COPY_CAPTURE(xs)...);
    }

    template <typename TF, typename TFwdCapture>
    decltype(auto) apply_fwd_capture(TF&& f, TFwdCapture&& fc)
    {
        return std::experimental::apply(
            [&f](auto&&... xs) mutable -> decltype(
                auto) { return f(FWD(xs).get()...); },
            FWD(fc));
    }
}

#define FWD_CAPTURE_PACK_AS_TUPLE(...) \
    orizzonte::impl::fwd_capture_as_tuple(FWD(__VA_ARGS__)...)

#define FWD_COPY_CAPTURE_PACK_AS_TUPLE(...) \
    orizzonte::impl::fwd_copy_capture_as_tuple(FWD(__VA_ARGS__)...)