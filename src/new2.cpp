#include "../include/orizzonte.hpp"
#include <atomic>
#include <boost/variant.hpp>
#include <chrono>
#include <experimental/type_traits>
#include <iostream>
#include <thread>
#include <tuple>
#include <utility>

namespace ou = orizzonte::utility;

// TODO: define executor interface/concept
struct S
{
    template <typename F>
    void operator()(F&& f)
    {
        std::thread{std::move(f)}.detach();
    }
};

template <typename T>
struct in_t
{
};

template <typename T>
inline constexpr in_t<T> in{};



template <typename In, typename F>
struct leaf : F
{
    using in_type = In;
    using out_type = ou::result_of_ignoring_nothing_t<F&, in_type>;

    constexpr leaf(in_t<In>, F&& f) : F{std::move(f)}
    {
    }

    template <typename Scheduler, typename Input, typename Then, typename Step>
    void execute(Scheduler&, Input&& input, Then&& then = ou::noop_v,
        Step&& step = ou::noop_v)
    {
        // A `leaf` doesn't schedule a computation on a separate thread by
        // default. The parent of the `leaf` takes care of this if desired.
        FWD(then)(ou::call_ignoring_nothing(*this, FWD(input)));
        FWD(step)();
    }
};

template <typename A, typename B>
struct node_seq : A, B
{
    using in_type = typename A::in_type;
    using out_type = typename B::out_type;

    struct shared_state
    {
        std::atomic<int> _left;
    };

    orizzonte::utility::aligned_storage_for<shared_state> _state;

    constexpr node_seq(A&& a, B&& b) : A{std::move(a)}, B{std::move(b)}
    {
    }

    template <typename Scheduler, typename Input, typename Then, typename Step>
    void execute(Scheduler& scheduler, Input&& input, Then&& then = ou::noop_v,
        Step&& step = ou::noop_v)
    {
        _state.construct();
        _state->_left = 2;

        auto halfstep = [this, step] {
            if(_state->_left.fetch_sub(1) == 1)
            {
                step();
                _state.destroy();
            }
        };

        // A `node_seq` doesn't schedule a computation on a separate thread by
        // default. `A` could however be executed asynchronously - arguments to
        // this function need to be captured inside the closure passed to `A`.
        static_cast<A&>(*this).execute(scheduler, FWD(input),
            [this, &scheduler, then, halfstep](auto&& out) {
                static_cast<B&>(*this).execute(scheduler, out, then, halfstep);
            },
            halfstep);
    }
};

template <bool B, typename Scheduler, typename F>
void schedule_if(Scheduler& scheduler, F&& f)
{
    if constexpr(B)
    {
        FWD(f)();
    }
    else
    {
        // The computation has to be moved here as it will die at
        // the end of the `enumerate_args` lambda scope.
        scheduler([g = std::move(f)]() mutable { g(); });
    }
}

template <typename... Xs, typename Index, typename Scheduler, typename F>
void schedule_if_last(Index, Scheduler& scheduler, F&& f)
{
    constexpr bool is_last = Index{} == sizeof...(Xs) - 1;
    schedule_if<is_last>(scheduler, FWD(f));
}

template <typename... Fs>
struct node_all : Fs...
{
    using in_type = std::common_type_t<typename Fs::in_type...>;
    using out_type = std::tuple<typename Fs::out_type...>;

    struct shared_state
    {
        in_type _input;
        // TODO: padding
        std::atomic<int> _left{sizeof...(Fs)};

        template <typename Input>
        shared_state(Input&& input) : _input{FWD(input)}
        {
        }
    };

    orizzonte::utility::aligned_storage_for<shared_state> _state;
    // TODO: padding
    out_type _values;

    constexpr node_all(Fs&&... fs) : Fs{std::move(fs)}...
    {
    }

    template <typename Scheduler, typename Input, typename Then, typename Step>
    void execute(Scheduler& scheduler, Input&& input, Then&& then, Step&& step)
    {
        // TODO: don't construct/destroy if lvalue?
        _state.construct(FWD(input));

        orizzonte::meta::enumerate_args(
            [&](auto i, auto t) {
                auto& f =
                    static_cast<orizzonte::meta::unwrap<decltype(t)>&>(*this);
                auto computation = [&, this, then,
                                       step /* TODO: fwd capture */] {
                    f.execute(scheduler, _state->_input,
                        [&](auto&& out) {
                            // TODO: padded tuple
                            std::get<i>(_values) = FWD(out);

                            if(_state->_left.fetch_sub(1) == 1)
                            {
                                // std::cout << "THEN ALL\n";
                                then(_values); // TODO: move?
                            }
                        },
                        [&, this, step] {
                            int expected = 0;
                            if(_state->_left.compare_exchange_strong(
                                   expected, -1))
                            {
                                // std::cout << "STEP ALL\n";
                                step();
                                // _state.destroy();
                            }
                        });
                };

                schedule_if_last<Fs...>(i, scheduler, std::move(computation));
            },
            orizzonte::meta::t<Fs>...);
    }

    /*template <typename Scheduler, typename Input>
    void execute(Scheduler& scheduler, Input&& input)
    {
        auto doit = [&](auto& f)
        {
            scheduler([&]
            {
                f.execute(scheduler, input, [&](auto&& out)
                {
                    // std::get<0>(_values) = out;
                });
            });
        };

        (doit(static_cast<Fs&>(*this)), ...);
        _left = 0;
    }*/
};

template <typename... Fs>
struct node_any : Fs...
{
    using in_type = std::common_type_t<typename Fs::in_type...>;

    // TODO: if they're all the same type, no need for variant
    using out_type = boost::variant<typename Fs::out_type...>;

    struct shared_state
    {
        in_type _input;
        // TODO: padding
        std::atomic<int> _left{sizeof...(Fs)};

        template <typename Input>
        shared_state(Input&& input) : _input{FWD(input)}
        {
        }
    };

    orizzonte::utility::aligned_storage_for<shared_state> _state;
    // TODO: padding
    out_type _values;

    constexpr node_any(Fs&&... fs) : Fs{std::move(fs)}...
    {
    }

    template <typename Scheduler, typename Input, typename Then, typename Step>
    void execute(Scheduler& scheduler, Input&& input, Then&& then, Step&& step)
    {
        // TODO: don't construct/destroy if lvalue?
        _state.construct(FWD(input));

        orizzonte::meta::enumerate_args(
            [&](auto i, auto t) {
                auto& f =
                    static_cast<orizzonte::meta::unwrap<decltype(t)>&>(*this);
                auto computation = [&, then, step /* TODO: fwd capture */] {
                    f.execute(scheduler, _state->_input,
                        [&](auto&& out) {
                            const auto old_left = _state->_left.fetch_sub(1);
                            if(old_left == sizeof...(Fs))
                            {
                                // TODO: huge issue: when one of the
                                // computations finishes, `then` is called with
                                // unblocks the latch and destroys the whole
                                // graph on the stack Need to find a way to keep
                                // the graph alive until all computations here
                                // have finished Idea: overload `then` on
                                // "final" and "non-final" states probably
                                // doesnt work Idea: walk over the graph and
                                // count nodes or something, have counter
                                // decrement on then

                                _values = out;
                                // std::cout << "THEN ANY\n";
                                then(_values); // TODO: move?
                            }
                        },
                        [&, this, step] {
                            int expected = 0;
                            if(_state->_left.compare_exchange_strong(
                                   expected, -1))
                            {
                                // std::cout << "STEP ANY\n";
                                // _state.destroy();
                                step();
                            }
                        });
                };

                schedule_if_last<Fs...>(i, scheduler, std::move(computation));
            },
            orizzonte::meta::t<Fs>...);
    }

    /*template <typename Scheduler, typename Input>
    void execute(Scheduler& scheduler, Input&& input)
    {
        auto doit = [&](auto& f)
        {
            scheduler([&]
            {
                f.execute(scheduler, input, [&](auto&& out)
                {
                    // std::get<l0>(_values) = out;
                });
            });
        };

        (doit(static_cast<Fs&>(*this)), ...);
        _left = 0;
    }*/
};

template <int N, typename... Ts>
using NthTypeOf = typename std::tuple_element<N, std::tuple<Ts...>>::type;

template <int N, typename... Ts>
auto& get(boost::variant<Ts...>& v)
{
    using target = NthTypeOf<N, Ts...>;
    return boost::get<target>(v);
}

template <int N, typename... Ts>
auto& get(const boost::variant<Ts...>& v)
{
    using target = NthTypeOf<N, Ts...>;
    return boost::get<target>(v);
}

template <typename Scheduler, typename Computation, typename Input>
void execute_and_block(Scheduler& s, Computation&& c, Input&& input)
{
    orizzonte::utility::scoped_bool_latch l;
    FWD(c).execute(s, FWD(input), [](auto&&...) {}, [&] { l.count_down(); });
    // std::cout << "DONE\n";
}

void t0()
{
    auto scheduler = S{};

    auto l0 = [] {};
    auto l1 = [] {};
    auto l2 = [] {};

    auto s0 = node_seq{leaf{in<ou::nothing>, std::move(l0)},
        leaf{in<ou::nothing>, std::move(l1)}};

    auto s1 = node_seq{std::move(s0), leaf{in<ou::nothing>, std::move(l2)}};
    execute_and_block(scheduler, s1, ou::nothing{});
}

void t1()
{
    auto scheduler = S{};

    auto l0 = [] {};
    auto l1 = [] { return 42; };
    auto l2 = [](int x) { assert(x == 42); };

    auto s0 = node_seq{leaf{in<ou::nothing>, std::move(l0)},
        leaf{in<ou::nothing>, std::move(l1)}};

    auto s1 = node_seq{std::move(s0), leaf{in<int>, std::move(l2)}};
    execute_and_block(scheduler, s1, ou::nothing{});
}

#define ENSURE(...)       \
    if(!(__VA_ARGS__))    \
    {                     \
        std::terminate(); \
    }



void t2()
{
    auto scheduler = S{};

    std::atomic<int> ctr = 0;

    auto l0 = [&] {
        ++ctr;
        return 0;
    };
    auto l1 = [&] {
        ++ctr;
        return 1;
    };
    auto l2 = [&] {
        ++ctr;
        return 2;
    };

    auto total = node_all{//
        leaf{in<ou::nothing>, std::move(l0)},
        leaf{in<ou::nothing>, std::move(l1)},
        leaf{in<ou::nothing>, std::move(l2)}};

    execute_and_block(scheduler, total, ou::nothing{});

    ENSURE(ctr == 3);
}

void t3()
{
    auto scheduler = S{};

    std::atomic<int> ctr = 0;

    auto l0 = [] { return 0; };
    auto l1 = [] { return 1; };
    auto l2 = [] { return 2; };

    auto cp0 = [&](int x) { ctr += x; };
    auto cp1 = [&](int x) { ctr += x; };
    auto cp2 = [&](int x) { ctr += x; };

    auto lf0 = leaf{in<ou::nothing>, std::move(l0)};
    auto lf1 = leaf{in<ou::nothing>, std::move(l1)};
    auto lf2 = leaf{in<ou::nothing>, std::move(l2)};

    auto total = node_any{//
        node_seq{std::move(lf0), leaf{in<int>, std::move(cp0)}},
        node_seq{std::move(lf1), leaf{in<int>, std::move(cp1)}},
        node_seq{std::move(lf2), leaf{in<int>, std::move(cp2)}}};

    execute_and_block(scheduler, total, ou::nothing{});

    ENSURE(ctr == 3);
}

void t4()
{
    auto scheduler = S{};

    std::atomic<int> ctr = 0;

    auto l0 = [] { return 0; };
    auto l1 = [] { return 1; };
    auto l2 = [] { return 2; };

    auto cp0 = [&](int x) { ctr += x; };
    auto cp1 = [&](int x) { ctr += x; };
    auto cp2 = [&](int x) { ctr += x; };

    auto lf0 = leaf{in<ou::nothing>, std::move(l0)};
    auto lf1 = leaf{in<ou::nothing>, std::move(l1)};
    auto lf2 = leaf{in<ou::nothing>, std::move(l2)};

    auto top0 = node_any{//
        node_seq{std::move(lf0), leaf{in<int>, std::move(cp0)}},
        node_seq{std::move(lf1), leaf{in<int>, std::move(cp1)}},
        node_seq{std::move(lf2), leaf{in<int>, std::move(cp2)}}};

    auto total = node_seq{std::move(top0),
        leaf{in<boost::variant<int, int, int>>, [](const auto&) {}}};

    execute_and_block(scheduler, total, ou::nothing{});

    // std::cout <<"ctr: " << ctr << '\n';
    // ENSURE(ctr == 3);
}

void t5()
{
    auto scheduler = S{};

    std::atomic<int> ctr = 0;

    auto top0 = node_any{//
        leaf{in<int>,
            [&](int x) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ctr += x;
            }},
        leaf{in<int>, [&](int x) { ctr += x; }}};

    auto total =
        node_seq{leaf{in<ou::nothing>, [] { return 0; }}, std::move(top0)};

    execute_and_block(scheduler, total, ou::nothing{});

    // ENSURE(ctr == 2);
}

void t9()
{
    using namespace std;
    auto scheduler = S{};

    // clang-format off
    auto l0 = []()      { cout <<               "l0\n"; return 0; };
    auto l1 = [](int x) { cout << "(" << x << ") l1\n"; return 1; };

    auto l2 = []()      { cout <<               "l2\n"; return 2; };
    auto l3 = [](int x) { cout << "(" << x << ") l3\n"; return 3; };

    auto l4 = []()      { cout <<               "l4\n"; return 4; };
    auto l5 = [](int x) { cout << "(" << x << ") l5\n"; return 5; };

    auto l6 = []()      { cout <<               "l6\n"; return 6; };
    auto l7 = [](int x) { cout << "(" << x << ") l7\n"; return 7; };

    auto l8 = []()      { cout <<               "l8\n"; return 8; };
    auto l9 = [](int x) { cout << "(" << x << ") l9\n"; return 9; };
    // clang-format on

    using namespace orizzonte::utility;

    auto s0 = node_seq{leaf{in<nothing>, move(l0)}, leaf{in<int>, move(l1)}};
    auto s1 = node_seq{leaf{in<nothing>, move(l2)}, leaf{in<int>, move(l3)}};
    auto s2 = node_seq{leaf{in<nothing>, move(l4)}, leaf{in<int>, move(l5)}};

    auto s3 = node_seq{leaf{in<nothing>, move(l6)}, leaf{in<int>, move(l7)}};
    auto s4 = node_seq{leaf{in<nothing>, move(l8)}, leaf{in<int>, move(l9)}};

    auto a0 = node_any{move(s0), move(s1), move(s2)};
    auto a1 = node_all{move(s3), move(s4)};

    auto top0 = node_seq{move(a0), leaf{in<boost::variant<int, int, int>>,
                                       [](const auto&) { return -1; }}};

    auto top1 = node_seq{move(a1),
        leaf{in<std::tuple<int, int>>, [](const auto& t) {
                 cout << std::get<0>(t) << ", " << std::get<1>(t) << '\n';
                 return -1;
             }}};

    auto total = node_all{std::move(top0), std::move(top1)};
    execute_and_block(scheduler, total, ou::nothing{});
    // this_thread::sleep_for(chrono::milliseconds(1));
}

int main()
{
#define DO_T(n)                  \
                                 \
    std::cout << "t" #n "\n";    \
    for(int i = 0; i < 1000; ++i) \
        t##n();                  \
    std::cout << "t" #n "end\n";

    DO_T(1);
    DO_T(2);
    DO_T(3);
    DO_T(4);
    DO_T(5);
    //DO_T(9);
}
