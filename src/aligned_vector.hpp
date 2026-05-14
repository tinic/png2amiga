#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

namespace png2amiga {

// std::vector-compatible allocator that returns pointers aligned to N
// bytes — for AVX2 SoA buffers we want N=32 so _mm256_load_ps and the
// implicit cacheline boundary at the start of the array both line up.
// N must be a power of two ≥ alignof(T).
//
// Stateless and trivially-copyable so vector copy/move are cheap; the
// equality operators return true unconditionally as required for any
// stateless allocator.
template<typename T, std::size_t N>
struct aligned_allocator {
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;
    static constexpr std::size_t alignment = N;

    // Required for MSVC's <vector>: the default std::allocator_traits
    // rebind mechanism uses class-template pattern matching that breaks
    // when the allocator has a non-type template parameter (N here),
    // so spell the legacy `rebind` member out explicitly.
    template<typename U>
    struct rebind {
        using other = aligned_allocator<U, N>;
    };

    aligned_allocator() noexcept = default;
    template<typename U>
    constexpr aligned_allocator(const aligned_allocator<U, N>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        std::size_t bytes = n * sizeof(T);
        // aligned_alloc requires size to be a multiple of alignment.
        bytes = (bytes + N - 1) & ~(N - 1);
#if defined(_MSC_VER)
        void* p = _aligned_malloc(bytes, N);
#else
        void* p = std::aligned_alloc(N, bytes);
#endif
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t /*n*/) noexcept {
#if defined(_MSC_VER)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
};

template<typename T, std::size_t N, typename U>
constexpr bool operator==(const aligned_allocator<T, N>&, const aligned_allocator<U, N>&) noexcept {
    return true;
}
template<typename T, std::size_t N, typename U>
constexpr bool operator!=(const aligned_allocator<T, N>&, const aligned_allocator<U, N>&) noexcept {
    return false;
}

// AVX2 cacheline-friendly float buffer: base aligned to 32 bytes.
using AlignedFloatVec = std::vector<float, aligned_allocator<float, 32>>;

}  // namespace png2amiga
